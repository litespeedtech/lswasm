/*****************************************************************************
*    Open LiteSpeed is an open source HTTP server.                           *
*    Copyright (C) 2026  LiteSpeed Technologies, Inc.                        *
*                                                                            *
*    This program is free software: you can redistribute it and/or modify    *
*    it under the terms of the GNU General Public License as published by    *
*    the Free Software Foundation, either version 3 of the License, or       *
*    (at your option) any later version.                                     *
*                                                                            *
*    This program is distributed in the hope that it will be useful,         *
*    but WITHOUT ANY WARRANTY; without even the implied warranty of          *
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the            *
*    GNU General Public License for more details.                            *
*                                                                            *
*    You should have received a copy of the GNU General Public License       *
*    along with this program. If not, see http://www.gnu.org/licenses/.      *
*****************************************************************************/

#include <iostream>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <atomic>
#include <stdexcept>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <csignal>
#include <cerrno>

#if defined(WASM_RUNTIME_V8)
#include "v8-initialization.h"
#endif

#include "connection_io.h"
#include "http_filter.h"
#include "thread_pool.h"
#include "wasm_module_manager.h"
#include "proxy-wasm/exports.h"   // RegisterForeignFunction, current_context_

// Version (injected by CMake via -DLSWASM_VERSION="x.y.z")
#ifndef LSWASM_VERSION
#define LSWASM_VERSION "unknown"
#endif

// HTTP Server Configuration
const int DEFAULT_PORT = 8080;
const char *DEFAULT_UDS_PATH = "/tmp/lswasm.sock";
const int BACKLOG = 128;
const int BUFFER_SIZE = 65536;          // 64 KB per recv() syscall
const int MAX_EPOLL_EVENTS = 64;
const size_t MAX_HEADER_SIZE = 65536;   // 64 KB limit for request headers
const size_t BODY_CHUNK_SIZE = 524288;  // 512 KB streaming chunk size

// Global state
static std::atomic<bool> g_shutdown{false};
static int g_server_socket = -1;  // For signal handler to unblock accept()
static std::string g_uds_path;    // For cleanup on shutdown
static std::atomic<uint32_t> g_next_context_id{1};
static bool g_body_pacifier = false;  // When true, include diagnostic body in responses.
std::unique_ptr<WasmModuleManager> g_module_manager;

// ── Streaming response foreign functions ──────────────────────────────
// These are registered once at static-init time and dispatched by
// proxy_call_foreign_function inside the WASM module.  Each handler
// casts current_context_ to LsWasmContext* and delegates to the
// streaming methods added in wasm_module_manager.h.

namespace {

lswasm::LsWasmContext *streaming_context() {
  auto *ctx = dynamic_cast<lswasm::LsWasmContext *>(proxy_wasm::current_context_);
  if (!ctx) {
    LOG_ERROR("[Streaming] current_context_ is not LsWasmContext");
  }
  return ctx;
}

} // anonymous namespace

// ── lswasm_send_response_headers ──
// Argument format:
//   4 bytes  uint32_t  status_code
//   remainder          proxy-wasm pairs (marshalled headers)
static proxy_wasm::RegisterForeignFunction register_send_response_headers(
    "lswasm_send_response_headers",
    [](proxy_wasm::WasmBase & /*wasm*/, std::string_view argument,
       std::function<void *(size_t)> /*alloc_result*/) -> proxy_wasm::WasmResult {
      auto *ctx = streaming_context();
      if (!ctx) return proxy_wasm::WasmResult::InternalFailure;

      if (argument.size() == 0) {
        // Zero-byte argument is the isSupported() probe — expected behavior,
        // not an error.  Return BadArgument so the filter knows the host
        // recognises the function, but don't log to stderr.
        LOG_INFO("[Streaming] send_response_headers: isSupported() probe");
        return proxy_wasm::WasmResult::BadArgument;
      }
      if (argument.size() < 4) {
        LOG_ERROR("[Streaming] send_response_headers: argument too short ("
                  << argument.size() << " bytes), expected >=4");
        return proxy_wasm::WasmResult::BadArgument;
      }
      uint32_t status_code;
      std::memcpy(&status_code, argument.data(), 4);

      HeaderPairs headers;
      http_utils::deserialize_header_pairs(argument.substr(4), headers);

      return ctx->streamingSendHeaders(status_code, headers);
    });

// ── lswasm_write_response_chunk ──
// Argument is the raw body bytes to write.
static proxy_wasm::RegisterForeignFunction register_write_response_chunk(
    "lswasm_write_response_chunk",
    [](proxy_wasm::WasmBase & /*wasm*/, std::string_view argument,
       std::function<void *(size_t)> /*alloc_result*/) -> proxy_wasm::WasmResult {
      auto *ctx = streaming_context();
      if (!ctx) return proxy_wasm::WasmResult::InternalFailure;
      return ctx->streamingWriteChunk(argument);
    });

// ── lswasm_finish_response ──
// No argument expected.
static proxy_wasm::RegisterForeignFunction register_finish_response(
    "lswasm_finish_response",
    [](proxy_wasm::WasmBase & /*wasm*/, std::string_view /*argument*/,
       std::function<void *(size_t)> /*alloc_result*/) -> proxy_wasm::WasmResult {
      auto *ctx = streaming_context();
      if (!ctx) return proxy_wasm::WasmResult::InternalFailure;
      return ctx->streamingFinish();
    });

// Extract Content-Length value from raw HTTP header data (case-insensitive).
// Returns 0 if the header is absent or on parse error.
static size_t extract_content_length(const std::string &headers) {
    // Scan for lines beginning with "Content-Length:" (case-insensitive).
    size_t pos = 0;
    while (pos < headers.size()) {
        // Find start of the next line.
        size_t line_end = headers.find('\n', pos);
        if (line_end == std::string::npos) line_end = headers.size();
        std::string_view line(headers.data() + pos, line_end - pos);
        // Strip trailing \r
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        if (line.size() > 15 && line[14] == ':') {
            if (header_name_eq(line.substr(0, 14), "Content-Length")) {
                size_t j = 15;
                while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) ++j;
                size_t start = j;
                while (j < line.size() && line[j] >= '0' && line[j] <= '9') ++j;
                if (j > start) {
                    try {
                        return std::stoull(std::string(line.substr(start, j - start)));
                    } catch (...) {
                        return 0;
                    }
                }
            }
        }
        pos = line_end + 1;
    }
    return 0;
}

// HTTP server supporting both TCP and Unix Domain Socket listeners.
class HttpServer {
public:
    // Construct a TCP listener on the given port.
    static HttpServer tcp(int port) {
        HttpServer s;
        s.mode_ = Mode::TCP;
        s.port_ = port;
        return s;
    }

    // Construct a Unix Domain Socket listener at the given path.
    static HttpServer uds(const std::string &path, mode_t sock_perm = 0666) {
        HttpServer s;
        s.mode_ = Mode::UDS;
        s.uds_path_ = path;
        s.sock_perm_ = sock_perm;
        return s;
    }

    ~HttpServer() {
        if (server_socket_ >= 0) {
            close(server_socket_);
        }
        cleanup_uds();
    }

    bool start() {
        switch (mode_) {
        case Mode::TCP:
            return start_tcp();
        case Mode::UDS:
            return start_uds();
        }
        return false;
    }

    // ════════════════════════════════════════════════════════════════════
    //  Streaming I/O event loop
    //
    //  The epoll loop owns ALL socket I/O.  Worker threads interact only
    //  with in-memory buffers via ConnectionIO.  Neither request nor
    //  response is fully buffered — data flows in BODY_CHUNK_SIZE chunks.
    //
    //  Per-connection state machine:
    //    ReadingHeaders → Active → (closed)
    //
    //  In the Active state, the fd can have:
    //    EPOLLIN  — body bytes still arriving from the client
    //    EPOLLOUT — response bytes ready to send to the client
    //    (both)   — simultaneous body reading and response writing
    //    (none)   — worker processing, no I/O pending
    //
    //  A global eventfd is used for worker→epoll notification.  When a
    //  worker enqueues response data or finishes, it writes to the
    //  eventfd.  The epoll loop consumes the counter and scans active
    //  connections for pending writes or finished workers.
    // ════════════════════════════════════════════════════════════════════

    void accept_connections(ThreadPool &pool) {
        int epoll_fd = epoll_create1(0);
        if (epoll_fd < 0) {
            LOG_ERROR("Failed to create epoll fd: " << strerror(errno));
            return;
        }

        // Create eventfd for worker→epoll notification.
        int event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (event_fd < 0) {
            LOG_ERROR("Failed to create eventfd: " << strerror(errno));
            close(epoll_fd);
            return;
        }

        // Make the server socket non-blocking so accept() won't block.
        set_nonblocking(server_socket_);

        // Register the server (listening) socket with epoll.
        {
            struct epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = server_socket_;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket_, &ev) < 0) {
                LOG_ERROR("Failed to add server socket to epoll: " << strerror(errno));
                close(event_fd);
                close(epoll_fd);
                return;
            }
        }

        // Register eventfd with epoll.
        {
            struct epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = event_fd;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_fd, &ev) < 0) {
                LOG_ERROR("Failed to add eventfd to epoll: " << strerror(errno));
                close(event_fd);
                close(epoll_fd);
                return;
            }
        }

        // ── Per-connection state ────────────────────────────────────────

        enum class ConnState { ReadingHeaders, Active };

        struct ConnCtx {
            ConnState state = ConnState::ReadingHeaders;
            std::string header_buf;                    // accumulates header bytes
            std::shared_ptr<ConnectionIO> conn_io;     // bridge to worker thread
            bool body_complete = false;                // all body bytes received
            uint32_t epoll_events = EPOLLIN;           // currently registered events
        };

        std::unordered_map<int, ConnCtx> connections;

        // Helper: update epoll registration for a client fd.
        auto update_epoll = [&](int fd, ConnCtx &ctx, uint32_t new_events) {
            if (new_events == ctx.epoll_events) return;
            if (new_events == 0) {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
            } else if (ctx.epoll_events == 0) {
                struct epoll_event ev2{};
                ev2.events = new_events;
                ev2.data.fd = fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev2);
            } else {
                struct epoll_event ev2{};
                ev2.events = new_events;
                ev2.data.fd = fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev2);
            }
            ctx.epoll_events = new_events;
        };

        // Helper: tear down a connection (signal errors to worker, close fd).
        auto close_conn = [&](int fd, ConnCtx &ctx) {
            if (ctx.epoll_events) {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                ctx.epoll_events = 0;
            }
            if (ctx.conn_io) {
                ctx.conn_io->feedError();   // wake worker blocked in readBodyChunk()
                ctx.conn_io->writeError();  // wake worker blocked in writeData()
            }
            close(fd);
        };

        struct epoll_event events[MAX_EPOLL_EVENTS];

        while (!g_shutdown.load(std::memory_order_relaxed)) {
            int nfds = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, 200 /*ms*/);
            if (nfds < 0) {
                if (errno == EINTR) continue;
                if (g_shutdown.load(std::memory_order_relaxed)) break;
                LOG_ERROR("epoll_wait error: " << strerror(errno));
                break;
            }

            for (int i = 0; i < nfds; ++i) {
                int fd = events[i].data.fd;
                uint32_t ev = events[i].events;

                // ── Server socket: accept new connections ──────────────
                if (fd == server_socket_) {
                    while (true) {
                        sockaddr_storage client_addr{};
                        socklen_t client_addrlen = sizeof(client_addr);
                        int client_fd = accept(server_socket_,
                                               reinterpret_cast<sockaddr *>(&client_addr),
                                               &client_addrlen);
                        if (client_fd < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            if (g_shutdown.load(std::memory_order_relaxed)) break;
                            LOG_ERROR("Accept error: " << strerror(errno));
                            break;
                        }
                        LOG_INFO("Accepted new connection: fd " << client_fd);

                        set_nonblocking(client_fd);

                        struct epoll_event client_ev{};
                        client_ev.events = EPOLLIN;
                        client_ev.data.fd = client_fd;
                        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev) < 0) {
                            LOG_ERROR("Failed to add client socket to epoll: "
                                      << strerror(errno));
                            close(client_fd);
                            continue;
                        }

                        connections[client_fd] = ConnCtx{};
                    }
                    continue;
                }

                // ── Eventfd: worker signalled ──────────────────────────
                if (fd == event_fd) {
                    // Consume the counter.
                    uint64_t val;
                    ssize_t rr = ::read(event_fd, &val, sizeof(val));
                    (void)rr;

                    // Scan active connections for pending writes or finished workers.
                    std::vector<int> to_close;
                    for (auto &[cfd, cctx] : connections) {
                        if (cctx.state != ConnState::Active) continue;
                        if (!cctx.conn_io) continue;
                        if (cctx.conn_io->hasError()) {
                            to_close.push_back(cfd);
                            continue;
                        }
                        if (cctx.conn_io->isFinished() && !cctx.conn_io->isWritePending()) {
                            to_close.push_back(cfd);
                            continue;
                        }
                        if (cctx.conn_io->isWritePending()) {
                            uint32_t wanted = EPOLLOUT;
                            if (!cctx.body_complete) wanted |= EPOLLIN;
                            update_epoll(cfd, cctx, wanted);
                        }
                    }
                    for (int cfd : to_close) {
                        auto cit = connections.find(cfd);
                        if (cit != connections.end()) {
                            close_conn(cfd, cit->second);
                            connections.erase(cit);
                        }
                    }
                    continue;
                }

                // ── Client fd ─────────────────────────────────────────
                auto it = connections.find(fd);
                if (it == connections.end()) continue;
                ConnCtx &ctx = it->second;

                if (ev & (EPOLLERR | EPOLLHUP)) {
                    close_conn(fd, ctx);
                    connections.erase(it);
                    continue;
                }

                // ── EPOLLIN ───────────────────────────────────────────
                if (ev & EPOLLIN) {
                    char buf[BUFFER_SIZE];
                    ssize_t n = recv(fd, buf, sizeof(buf), 0);

                    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        close_conn(fd, ctx);
                        connections.erase(it);
                        continue;
                    }

                    if (n == 0) {
                        // Peer closed their write direction.
                        if (ctx.state == ConnState::ReadingHeaders) {
                            close_conn(fd, ctx);
                            connections.erase(it);
                            continue;
                        }
                        // Active state: signal EOF to body reader.
                        if (!ctx.body_complete && ctx.conn_io) {
                            ctx.conn_io->feedBody(nullptr, 0, true);
                        }
                        ctx.body_complete = true;
                        uint32_t wanted = ctx.epoll_events & ~(uint32_t)EPOLLIN;
                        update_epoll(fd, ctx, wanted);
                    }

                    if (n > 0) {
                        if (ctx.state == ConnState::ReadingHeaders) {
                            ctx.header_buf.append(buf, static_cast<size_t>(n));

                            // Guard against oversized headers.
                            if (ctx.header_buf.size() > MAX_HEADER_SIZE) {
                                const char *resp =
                                    "HTTP/1.1 431 Request Header Fields Too Large\r\n"
                                    "Connection: close\r\nContent-Length: 0\r\n\r\n";
                                ::send(fd, resp, strlen(resp), MSG_NOSIGNAL);
                                close_conn(fd, ctx);
                                connections.erase(it);
                                continue;
                            }

                            // Check whether full headers have arrived.
                            size_t hdr_end = ctx.header_buf.find("\r\n\r\n");
                            if (hdr_end != std::string::npos) {
                                hdr_end += 4;  // include the \r\n\r\n
                                std::string header_data = ctx.header_buf.substr(0, hdr_end);
                                std::string body_prefix = ctx.header_buf.substr(hdr_end);
                                ctx.header_buf.clear();
                                ctx.header_buf.shrink_to_fit();

                                size_t content_length = extract_content_length(header_data);

                                LOG_INFO("Received request: fd " << fd << ", content-length " << content_length);

                                // Create the ConnectionIO bridge.
                                auto conn_io = std::make_shared<ConnectionIO>(fd, event_fd);
                                conn_io->setHeaderData(std::move(header_data),
                                                       std::move(body_prefix),
                                                       content_length);
                                ctx.conn_io = conn_io;
                                ctx.state = ConnState::Active;

                                // Determine if the body is already complete.
                                if (content_length == 0 ||
                                    conn_io->bodyBytesReceived() >= content_length) {
                                    ctx.body_complete = true;
                                    update_epoll(fd, ctx, 0);  // idle until worker produces data
                                } else {
                                    ctx.body_complete = false;
                                    // Keep EPOLLIN for body reading.
                                }

                                // Dispatch to worker thread pool.
                                pool.submit([this, conn = std::move(conn_io)]() {
                                    try {
                                        handle_request(conn);
                                    } catch (const std::exception &e) {
                                        LOG_ERROR("Worker exception: " << e.what());
                                        conn->setError();
                                    } catch (...) {
                                        LOG_ERROR("Worker unknown exception");
                                        conn->setError();
                                    }
                                });
                            }

                        } else if (ctx.state == ConnState::Active && !ctx.body_complete) {
                            // Feed body bytes to ConnectionIO.
                            size_t received = ctx.conn_io->bodyBytesReceived();
                            size_t cl = ctx.conn_io->contentLength();
                            size_t remaining = (cl > received) ? (cl - received) : 0;
                            size_t to_feed = std::min(static_cast<size_t>(n), remaining);
                            bool eof = (to_feed >= remaining);
                            ctx.conn_io->feedBody(buf, to_feed, eof);

                            if (eof) {
                                ctx.body_complete = true;
                                uint32_t wanted = ctx.epoll_events & ~(uint32_t)EPOLLIN;
                                update_epoll(fd, ctx, wanted);
                            }
                        }
                    }
                }

                // ── EPOLLOUT ──────────────────────────────────────────
                if (ev & EPOLLOUT) {
                    auto it2 = connections.find(fd);
                    if (it2 == connections.end()) continue;
                    ConnCtx &ctx2 = it2->second;
                    if (!ctx2.conn_io) continue;

                    std::string_view pending = ctx2.conn_io->pendingWriteData();
                    if (pending.empty()) {
                        if (ctx2.conn_io->isFinished()) {
                            close_conn(fd, ctx2);
                            connections.erase(it2);
                        } else {
                            uint32_t wanted = ctx2.body_complete ? 0 : EPOLLIN;
                            update_epoll(fd, ctx2, wanted);
                        }
                        continue;
                    }

                    ssize_t sent = ::send(fd, pending.data(), pending.size(), MSG_NOSIGNAL);
                    if (sent < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            ctx2.conn_io->writeError();
                            close_conn(fd, ctx2);
                            connections.erase(it2);
                        }
                        continue;
                    }

                    ctx2.conn_io->advanceWrite(static_cast<size_t>(sent));

                    if (!ctx2.conn_io->isWritePending()) {
                        if (ctx2.conn_io->isFinished()) {
                            close_conn(fd, ctx2);
                            connections.erase(it2);
                        } else {
                            uint32_t wanted = ctx2.body_complete ? 0 : EPOLLIN;
                            update_epoll(fd, ctx2, wanted);
                        }
                    }
                }
            }
        }

        // Clean up remaining client connections.
        for (auto &[fd, ctx] : connections) {
            close_conn(fd, ctx);
        }
        connections.clear();
        close(event_fd);
        close(epoll_fd);
    }

private:
    enum class Mode { TCP, UDS };

    HttpServer() : mode_(Mode::TCP), port_(DEFAULT_PORT), sock_perm_(0666), server_socket_(-1) {}

    // ── Helper: set a socket to non-blocking mode ───────────────────────

    static void set_nonblocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) flags = 0;
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    // ── TCP listener ────────────────────────────────────────────────────

    bool start_tcp() {
        server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_socket_ < 0) {
            LOG_ERROR("Failed to create TCP socket");
            return false;
        }

        int opt = 1;
        if (setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            LOG_ERROR("Failed to set socket options");
            close(server_socket_);
            return false;
        }

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        server_addr.sin_port = htons(port_);

        if (bind(server_socket_, reinterpret_cast<sockaddr *>(&server_addr),
                 sizeof(server_addr)) < 0) {
            LOG_ERROR("Failed to bind TCP socket to port " << port_);
            close(server_socket_);
            return false;
        }

        if (listen(server_socket_, BACKLOG) < 0) {
            LOG_ERROR("Failed to listen on TCP socket");
            close(server_socket_);
            return false;
        }

        g_server_socket = server_socket_;
        LOG_INFO("HTTP Server listening on TCP port " << port_);
        return true;
    }

    // ── Unix Domain Socket listener ─────────────────────────────────────

    bool start_uds() {
        server_socket_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_socket_ < 0) {
            LOG_ERROR("Failed to create Unix domain socket");
            return false;
        }

        // Ensure the parent directory exists (e.g. /tmp/lshttpd/).
        std::filesystem::path parent = std::filesystem::path(uds_path_).parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                LOG_ERROR("Failed to create parent directory for UDS socket "
                          << parent << ": " << ec.message());
                close(server_socket_);
                return false;
            }
        }

        // Remove any stale socket file.
        ::unlink(uds_path_.c_str());

        sockaddr_un server_addr{};
        server_addr.sun_family = AF_UNIX;

        if (uds_path_.size() >= sizeof(server_addr.sun_path)) {
            LOG_ERROR("Unix socket path too long (max "
                      << sizeof(server_addr.sun_path) - 1 << " chars): "
                      << uds_path_);
            close(server_socket_);
            return false;
        }
        std::strncpy(server_addr.sun_path, uds_path_.c_str(),
                      sizeof(server_addr.sun_path) - 1);

        if (bind(server_socket_, reinterpret_cast<sockaddr *>(&server_addr),
                 sizeof(server_addr)) < 0) {
            LOG_ERROR("Failed to bind Unix domain socket at " << uds_path_
                      << ": " << strerror(errno));
            close(server_socket_);
            return false;
        }

        // Set socket file permissions (configurable via --sock-perm, default 0666).
        if (chmod(uds_path_.c_str(), sock_perm_) != 0) {
            LOG_ERROR("Failed to set permissions on Unix domain socket: "
                      << strerror(errno));
            close(server_socket_);
            cleanup_uds();
            return false;
        }

        if (listen(server_socket_, BACKLOG) < 0) {
            LOG_ERROR("Failed to listen on Unix domain socket");
            close(server_socket_);
            cleanup_uds();
            return false;
        }

        g_server_socket = server_socket_;
        g_uds_path = uds_path_;
        LOG_INFO("HTTP Server listening on Unix socket " << uds_path_);
        return true;
    }

    void cleanup_uds() {
        if (!uds_path_.empty()) {
            ::unlink(uds_path_.c_str());
        }
    }

    // ── Helpers ─────────────────────────────────────────────────────────
    // NOTE: serialize_headers() and reason_phrase() live in http_utils.h
    //       (included via wasm_module_manager.h).

    // ── Request handling (dispatched to thread pool workers) ─────────────

    // Process an HTTP request via the ConnectionIO bridge.
    // Called from a thread pool worker.  The worker does NOT touch the
    // socket directly — all I/O goes through conn->readBodyChunk() and
    // conn->writeData().  The epoll loop handles actual socket I/O.
    void handle_request(std::shared_ptr<ConnectionIO> conn) {
        HttpData http_data;

        if (!parse_request(conn->headers(), http_data)) {
            conn->setError();
            return;
        }

        // Create a filter context for this request.
        uint32_t ctx_id = g_next_context_id.fetch_add(1);
        HttpFilterContext filter_ctx(ctx_id, &http_data);
        filter_ctx.setConnectionIO(conn.get());
        filter_ctx.onCreate();

        // Execute request header phase via filter chain.
        // end_of_stream is false when the request has a body, so that
        // WASM filters know to expect onRequestBody() calls.
        bool has_body = (conn->contentLength() > 0);
        LOG_INFO("\n[HTTP] Processing request in filter chain...");
        filter_ctx.onRequestHeaders(/*end_of_stream=*/!has_body);

        // If the WASM filter sent a local response, write it and return.
        if (http_data.has_local_response) {
            LOG_INFO("[HTTP] WASM filter sent local response, using it.");
            std::string response = build_local_response(http_data);
            write_chunked(conn, response);
            conn->finish();
            return;
        }

        // ── Stream request body in chunks via ConnectionIO ────────
        size_t content_length = conn->contentLength();
        LOG_INFO("Request has Content-Length: " << content_length);
        if (content_length > 0) {
            const std::string &prefix = conn->bodyPrefix();
            size_t body_consumed = prefix.size();
            LOG_INFO("Prefix size: " << body_consumed);
            if (!prefix.empty()) {
                http_data.request_body = prefix;
                filter_ctx.onRequestBody(body_consumed >= content_length);
            }

            LOG_INFO("Read: " << body_consumed << " / " << content_length);
            while (body_consumed < content_length && !http_data.has_local_response) {
                size_t want = std::min(content_length - body_consumed, BODY_CHUNK_SIZE);
                ConnectionIO::BodyReadResult read_result = conn->readBodyChunk(want);
                if (read_result.status == ConnectionIO::BodyReadStatus::Error) {
                    LOG_ERROR("[HTTP] Request body read error after " << body_consumed
                              << " / " << content_length << " bytes");
                    conn->setError();
                    return;
                }
                if (read_result.status == ConnectionIO::BodyReadStatus::Truncated) {
                    LOG_ERROR("[HTTP] Request body truncated after " << body_consumed
                              << " / " << content_length << " bytes");
                    conn->setError();
                    return;
                }
                if (read_result.data.empty()) {
                    LOG_ERROR("[HTTP] Request body read returned no data before completion");
                    conn->setError();
                    return;
                }
                body_consumed += read_result.data.size();
                http_data.request_body = std::move(read_result.data);
                LOG_INFO("Read: " << body_consumed << " / " << content_length);
                filter_ctx.onRequestBody(body_consumed >= content_length);
            }
        }

        if (!http_data.has_local_response) {
            filter_ctx.onRequestTrailers();
        }

        // Check again after body processing.
        if (http_data.has_local_response) {
            std::string response = build_local_response(http_data);
            write_chunked(conn, response);
            conn->finish();
            return;
        }

        // Request-phase streaming responses must be fully finished before the
        // host treats them as a terminal success path.
        if (filter_ctx.hasStreamingResponse()) {
            if (!filter_ctx.isStreamingFinished()) {
                LOG_ERROR("[HTTP] Streaming response started but was not finished");
                conn->setError();
                return;
            }
            LOG_INFO("[HTTP] Streaming response handled by WASM filter.");
            filter_ctx.onDone();
            conn->finish();
            return;
        }

        // Generate the response body.
        http_data.response_body.clear();
        if (g_body_pacifier) {
            http_data.response_body = build_response_body(http_data);
        }

        // Populate default response headers.
        http_data.response_headers.clear();
        http_data.response_headers.emplace_back("Content-Type", "text/plain");
        http_data.response_headers.emplace_back("Connection", "close");

        // Execute response phases — WASM modules can modify response headers,
        // response body bytes, or replace the response entirely.
        LOG_INFO("[HTTP] Processing response in filter chain...");
        filter_ctx.onResponseHeaders();
        filter_ctx.onResponseBody();
        filter_ctx.onResponseTrailers();

        if (http_data.has_local_response) {
            if (filter_ctx.hasStreamingResponse()) {
                LOG_ERROR("[HTTP] Local response requested after streaming response started");
                conn->setError();
                return;
            }
            filter_ctx.onDone();
            std::string response = build_local_response(http_data);
            write_chunked(conn, response);
            conn->finish();
            return;
        }

        if (filter_ctx.hasStreamingResponse()) {
            if (!filter_ctx.isStreamingFinished()) {
                LOG_ERROR("[HTTP] Streaming response started but was not finished");
                conn->setError();
                return;
            }
            LOG_INFO("[HTTP] Streaming response handled by WASM filter.");
            filter_ctx.onDone();
            conn->finish();
            return;
        }

        filter_ctx.onDone();

        // Ensure Content-Length is correct after filter chain.
        HeaderPairs &hdrs = http_data.response_headers;
        hdrs.erase(std::remove_if(hdrs.begin(), hdrs.end(),
            [](const std::pair<std::string, std::string> &p) {
                return header_name_eq(p.first, "Content-Length");
            }), hdrs.end());
        hdrs.emplace_back("Content-Length", std::to_string(http_data.response_body.length()));

        // Write response headers.
        std::string hdr_str = http_utils::serialize_headers(200, hdrs);
        conn->writeData(hdr_str);

        // Write response body in chunks.
        write_chunked(conn, http_data.response_body);

        conn->finish();
    }

    bool parse_request(const std::string &request, HttpData &http_data) {
        std::istringstream iss(request);
        iss >> http_data.method >> http_data.path >> http_data.version;

        if (http_data.method.empty() || http_data.path.empty()) {
            return false;
        }

        // Parse headers (simplified).
        std::string line;
        // Consume the remainder of the request line.
        std::getline(iss, line);
        while (std::getline(iss, line) && !line.empty() && line != "\r") {
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string header_name = line.substr(0, colon);
                std::string header_value = line.substr(colon + 1);
                // Trim leading whitespace (OWS per RFC 7230)
                size_t start = header_value.find_first_not_of(" \t");
                if (start != std::string::npos) {
                    header_value = header_value.substr(start);
                } else {
                    header_value.clear();
                }
                // Remove trailing \r if present
                if (!header_value.empty() && header_value.back() == '\r') {
                    header_value.pop_back();
                }
                http_data.request_headers.emplace_back(std::move(header_name),
                                                       std::move(header_value));
            }
        }

        return true;
    }

    // Build an HTTP response from the WASM filter's local response.
    std::string build_local_response(const HttpData &http_data) {
        HeaderPairs headers;
        headers.emplace_back("Content-Type", "text/plain");
        headers.emplace_back("X-Powered-By", "lswasm/proxy-wasm");
        headers.emplace_back("Connection", "close");
        // Merge additional headers from sendLocalResponse.
        for (const std::pair<std::string, std::string> &h :
             http_data.local_response_additional_headers) {
            if (header_name_eq(h.first, "Content-Length") ||
                header_name_eq(h.first, "Content-Type") ||
                header_name_eq(h.first, "Connection")) {
                continue;
            }
            headers.emplace_back(h.first, h.second);
        }
        headers.emplace_back("Content-Length",
                             std::to_string(http_data.local_response_body.length()));

        std::string hdr_str = http_utils::serialize_headers(http_data.local_response_code, headers);
        return hdr_str + http_data.local_response_body;
    }

    // Build the diagnostic response body.
    std::string build_response_body(const HttpData &http_data) {
        std::string body = "=== WASM HTTP Proxy Server ===\n\n";
        body += "Request Information:\n";
        body += "  Method: " + http_data.method + "\n";
        body += "  Path: " + http_data.path + "\n";
        body += "  Version: " + http_data.version + "\n\n";

        body += "Runtime Information:\n";
#if defined(WASM_RUNTIME_WASMTIME)
        body += "  ✓ Wasmtime runtime available\n";
#elif defined(WASM_RUNTIME_V8)
        body += "  ✓ V8 runtime available\n";
#elif defined(WASM_RUNTIME_WASMEDGE)
        body += "  ✓ WasmEdge runtime available\n";
#elif defined(WASM_RUNTIME_WAMR)
        body += "  ✓ WAMR runtime available\n";
#else
        body += "  ℹ No WASM runtime enabled\n";
#endif

        body += "\nFilter Status:\n";
        if (g_module_manager) {
            std::vector<std::string> modules = g_module_manager->getLoadedModules();
            if (modules.empty()) {
                body += "  • No filters loaded\n";
            } else {
                body += "  Loaded filters:\n";
                for (const std::string &module : modules) {
                    body += "    - " + module + "\n";
                }
            }
        }

        body += "\nProxy-WASM Support:\n";
        body += "  • RootContext lifecycle callbacks\n";
        body += "  • HTTP filter callbacks (onRequest*, onResponse*)\n";
        body += "  • Response header manipulation from WASM modules\n";
        body += "  • Connection events\n";
        body += "  • Metadata and data processing\n";
        body += "  • Status/error codes\n\n";

        body += "Submodules:\n";
        body += "  • proxy-wasm-cpp-host\n";
        body += "  • proxy-wasm-cpp-sdk\n";
        body += "  • proxy-wasm-spec\n";

        return body;
    }

    // Write a string through ConnectionIO in BODY_CHUNK_SIZE chunks.
    static void write_chunked(std::shared_ptr<ConnectionIO> &conn,
                              const std::string &data) {
        size_t offset = 0;
        while (offset < data.size()) {
            size_t chunk = std::min(data.size() - offset, BODY_CHUNK_SIZE);
            conn->writeData(data.substr(offset, chunk));
            offset += chunk;
        }
    }

    Mode mode_;
    int port_;
    std::string uds_path_;
    mode_t sock_perm_;
    int server_socket_;
};

// Signal handler (only async-signal-safe operations)
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_shutdown.store(true, std::memory_order_relaxed);
        // Shutdown the listening socket to unblock accept()
        if (g_server_socket >= 0) {
            shutdown(g_server_socket, SHUT_RDWR);
        }
    }
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    std::string wasm_module_path;
    std::string uds_path = DEFAULT_UDS_PATH;
    mode_t sock_perm = 0666;
    std::unordered_map<std::string, std::string> wasm_envs;
    bool debug = false;
    bool port_specified = false;
    size_t num_workers = 0;  // 0 = auto (hardware_concurrency)

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
            port_specified = true;
        } else if (arg == "--uds" && i + 1 < argc) {
            uds_path = argv[++i];
        } else if (arg == "--sock-perm" && i + 1 < argc) {
            const char *val = argv[++i];
            char *endptr = nullptr;
            unsigned long parsed = std::strtoul(val, &endptr, 8);
            if (endptr == val || *endptr != '\0' || parsed > 0777) {
                LOG_ERROR("Invalid --sock-perm value (expected octal 0-0777): " << val);
                return 1;
            }
            sock_perm = static_cast<mode_t>(parsed);
        } else if (arg == "--module" && i + 1 < argc) {
            wasm_module_path = argv[++i];
        } else if (arg == "--env" && i + 1 < argc) {
            std::string env_str = argv[++i];
            size_t eq_pos = env_str.find('=');
            if (eq_pos != std::string::npos) {
                std::string key = env_str.substr(0, eq_pos);
                std::string value = env_str.substr(eq_pos + 1);
                wasm_envs[key] = value;
            } else {
                LOG_ERROR("Invalid --env format, expected KEY=VALUE: " << env_str);
                return 1;
            }
        } else if (arg == "--workers" && i + 1 < argc) {
            num_workers = static_cast<size_t>(std::stoi(argv[++i]));
        } else if (arg == "--body-pacifier") {
            g_body_pacifier = true;
        } else if (arg == "--debug") {
            debug = true;
        } else if (arg == "--version") {
            std::cout << "lswasm " << LSWASM_VERSION << "\n";
            return 0;
        } else if (arg == "--help") {
            std::cout << "lswasm " << LSWASM_VERSION
                      << " — WASM HTTP Proxy Server with Proxy-WASM Support\n";
            std::cout << "Usage: " << argv[0] << " --module <path> [options]\n";
            std::cout << "Options:\n";
            std::cout << "  --port PORT      : Listen on TCP port (instead of UDS)\n";
            std::cout << "  --uds PATH       : Listen on Unix domain socket (default: "
                      << DEFAULT_UDS_PATH << ")\n";
            std::cout << "  --sock-perm MODE : Set UDS file permissions in octal (default: 0666)\n";
            std::cout << "  --module PATH    : Load WASM filter module (required)\n";
            std::cout << "  --env KEY=VALUE  : Set environment variable for WASM module (repeatable)\n";
            std::cout << "  --workers N      : Number of worker threads (default: hardware_concurrency)\n";
            std::cout << "  --body-pacifier  : Include diagnostic body in HTTP responses\n";
            std::cout << "  --debug          : Enable debug logging to "
                      << lswasm_log::LOG_PATH << "\n";
            std::cout << "  --version        : Show version number\n";
            std::cout << "  --help           : Show this help message\n";
            std::cout << "\nBy default, listens on UDS at " << DEFAULT_UDS_PATH << ".\n";
            std::cout << "Use --port to listen on TCP instead. "
                      << "When both --port and --uds are given, only --uds is used.\n";
            return 0;
        }
    }

    // Initialize logging: active if /tmp/lswasm.dolog exists or --debug is given.
    lswasm_log::log_init(debug);

    // Initialize WASM module manager.
    g_module_manager = std::make_unique<WasmModuleManager>();

    // Print runtime information
    LOG_INFO("\n=== lswasm " << LSWASM_VERSION << " ===");
#if defined(WASM_RUNTIME_WASMTIME)
    LOG_INFO("✓ Wasmtime runtime enabled");
#elif defined(WASM_RUNTIME_V8)
    LOG_INFO("✓ V8 runtime enabled");
#elif defined(WASM_RUNTIME_WASMEDGE)
    LOG_INFO("✓ WasmEdge runtime enabled");
#elif defined(WASM_RUNTIME_WAMR)
    LOG_INFO("✓ WAMR runtime enabled");
#else
    LOG_INFO("ℹ No WASM runtime enabled (using Null VM)");
#endif
    LOG_INFO("Submodules:");
    LOG_INFO("  • proxy-wasm-cpp-host");
    LOG_INFO("  • proxy-wasm-cpp-sdk");
    LOG_INFO("  • proxy-wasm-spec");
    LOG_INFO("==============================\n");

    // Set environment variables for WASM modules
    if (!wasm_envs.empty()) {
        LOG_INFO("WASM environment variables:");
        for (const auto &[key, value] : wasm_envs) {
            LOG_INFO("  " << key << "=" << value);
        }
        g_module_manager->setEnvironmentVariables(wasm_envs);
    }

    // Load WASM module (required).
    if (wasm_module_path.empty()) {
        LOG_ERROR("No WASM module specified. Use --module <path> to load a filter.");
        std::cerr << "Error: --module is required. Run with --help for usage.\n";
        return 1;
    }
    {
        std::string module_name = "custom_filter";
        LOG_INFO("Loading WASM filter module: " << wasm_module_path);
        if (g_module_manager->loadModule(wasm_module_path, module_name)) {
            LOG_INFO("✓ Filter module loaded successfully");
        } else {
            LOG_ERROR("✗ Failed to load filter module");
            return 1;
        }
    }

    // Register signal handlers AFTER V8/runtime initialisation.
    {
        struct sigaction sa{};
        sa.sa_handler = signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;  // No SA_RESTART – we want epoll_wait/accept to return EINTR.
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
    }

    // Create thread pool for worker threads.
    ThreadPool pool(num_workers);
    LOG_INFO("Thread pool started with " << pool.size() << " workers");

    try {
        // Create server: default to UDS; use TCP only if --port was explicitly
        // given without a custom --uds override.
        std::unique_ptr<HttpServer> server;
        bool explicit_uds = (uds_path != DEFAULT_UDS_PATH);
        if (explicit_uds || !port_specified) {
            server = std::make_unique<HttpServer>(HttpServer::uds(uds_path, sock_perm));
        } else {
            server = std::make_unique<HttpServer>(HttpServer::tcp(port));
        }

        if (!server->start()) {
            LOG_ERROR("Failed to start HTTP server");
            return 1;
        }

        LOG_INFO("Server ready. Press Ctrl+C to stop.\n");

        // Accept incoming connections (blocks until g_shutdown).
        server->accept_connections(pool);

        // ── Shutdown sequence ────────────────────────────────────────────
        // 1. Epoll loop has exited (g_shutdown is true).
        // 2. Drain the thread pool — all in-flight requests finish.
        LOG_INFO("Draining thread pool...");
        pool.shutdown();

        // 3. Destroy the HttpServer (closes the listening socket).
        server.reset();

    } catch (const std::exception &e) {
        LOG_ERROR("Error: " << e.what());
        pool.shutdown();
        return 1;
    }

    // Clean up UDS file if used.
    if (!g_uds_path.empty()) {
        ::unlink(g_uds_path.c_str());
    }

    LOG_INFO("Server stopped");

    // 4. Release the module manager — tears down base WASM VMs.
    g_module_manager.reset();
    return 0;
}
