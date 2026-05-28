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
#include <chrono>
#include <thread>
#include <stdexcept>
#include <sstream>
#include <cstring>
#include <cstdlib>
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
#include <pthread.h>

#if defined(WASM_RUNTIME_V8)
#include "v8-initialization.h"
#endif

#include "connection_io.h"
#include "http_filter.h"
#include "http_response_sink.h"
#include "thread_pool.h"
#include "wasm_module_manager.h"
#include "proxy-wasm/exports.h"   // RegisterForeignFunction, current_context_

// LSAPI support (C library)
extern "C" {
#include "lsapilib.h"
}

// Version (injected by CMake via -DLSWASM_VERSION="x.y.z")
#ifndef LSWASM_VERSION
#define LSWASM_VERSION "unknown"
#endif

// HTTP Server Configuration
const int DEFAULT_PORT = 8080;
const char *DEFAULT_UDS_PATH = "/tmp/lswasm.sock";
// Default TCP bind address: loopback only.  Non-loopback access is opt-in via
// --bind so that lswasm is not exposed to the network by accident.
const char *DEFAULT_BIND_ADDR = "127.0.0.1";
// Default Unix domain socket permissions: owner-only (0600).  Cross-user
// access (e.g. when a web server runs as a different user than lswasm)
// requires an explicit --sock-perm override and/or group setup.
const mode_t DEFAULT_SOCK_PERM = 0600;
// Maximum HTTP request body size accepted by the LSPROXY listener (1 GiB).
// This caps Content-Length to bound a single connection's resource use; it is
// not a per-process limit.
const size_t MAX_REQUEST_BODY = static_cast<size_t>(1) << 30;
const int BACKLOG = 128;
const int BUFFER_SIZE = 65536;          // 64 KB per recv() syscall
const int MAX_EPOLL_EVENTS = 64;
const size_t MAX_HEADER_SIZE = 65536;   // 64 KB limit for request headers
const size_t BODY_CHUNK_SIZE = 524288;  // 512 KB streaming chunk size
// Maximum concurrent client connections accepted by the LSPROXY listener.
// Above this we close incoming connections immediately to bound memory and
// fd usage under slow-loris or fork-bomb-style load.  Compile-time constant;
// no CLI knob today because the realistic deployment uses LSAPI mode.
const size_t MAX_LSPROXY_CONNECTIONS = 1024;
// Per-connection idle timeout in seconds: a connection that neither sends
// request bytes nor receives response bytes within this window is closed.
const int LSPROXY_IDLE_TIMEOUT_SECS = 60;

// Global state
static std::atomic<bool> g_shutdown{false};
static int g_server_socket = -1;  // For shutdown coordination to unblock accept()/read().
constexpr size_t MAX_TRACKED_LSAPI_FDS = 4096;
static int g_tracked_lsapi_fds[MAX_TRACKED_LSAPI_FDS] = {};
// Worker thread handles for shutdown: pthread_kill(SIGUSR2) interrupts accept().
constexpr size_t MAX_WORKER_THREADS = 256;
static pthread_t g_worker_threads[MAX_WORKER_THREADS] = {};
static std::atomic<size_t> g_num_worker_threads{0};
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

namespace {
int reserve_tracked_lsapi_fd_slot();
void publish_tracked_lsapi_fd(int slot, int fd);
void clear_tracked_lsapi_fd(int slot);
void shutdown_tracked_lsapi_fds();
bool ensure_lsapi_listener_socket_permissions(int listen_fd, mode_t sock_perm = DEFAULT_SOCK_PERM);
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

// Parsed result of HTTP framing headers (Content-Length / Transfer-Encoding).
struct FramingHeaders {
    enum class Status {
        Ok,
        ConflictingContentLength,    // 400: duplicate or contradictory CL values
        InvalidContentLength,        // 400: non-digit / overflow
        ContentLengthTooLarge,       // 413: exceeds MAX_REQUEST_BODY
        TransferEncodingPresent,     // 400: any TE other than identity
        CLAndTEMixed,                // 400: both CL and TE — smuggling vector
    };
    Status status = Status::Ok;
    size_t content_length = 0;
};

// RFC 7230 §3.2.6 — token characters are the only valid header-name bytes.
static bool is_tchar(unsigned char c) {
    if (c >= 'a' && c <= 'z') return true;
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '-': case '.': case '^': case '_':
    case '`': case '|': case '~':
        return true;
    default:
        return false;
    }
}

// Header-name character predicate for full header validation.
static bool is_valid_header_name(std::string_view name) {
    if (name.empty()) return false;
    for (unsigned char c : name) {
        if (!is_tchar(c)) return false;
    }
    return true;
}

// Header-value character predicate.  RFC 7230 §3.2.6 allows VCHAR, SP, HTAB
// and obs-text; we reject CR/LF/NUL outright (the rest of CTL is permitted by
// obs-text but harmless to forward).
static bool is_valid_header_value(std::string_view value) {
    for (unsigned char c : value) {
        if (c == '\0' || c == '\r' || c == '\n') return false;
    }
    return true;
}

// Parse Content-Length and Transfer-Encoding from the raw header block.
// Walks every "CR? LF"-terminated line, identifying duplicate / mismatched
// Content-Length values and any Transfer-Encoding presence so the caller can
// reject smuggling-prone requests.
static FramingHeaders parse_framing_headers(const std::string &headers) {
    FramingHeaders out;
    bool saw_cl = false;
    bool saw_te = false;

    size_t pos = 0;
    // Skip the request line.
    {
        size_t line_end = headers.find('\n', pos);
        if (line_end == std::string::npos) return out;
        pos = line_end + 1;
    }

    while (pos < headers.size()) {
        size_t line_end = headers.find('\n', pos);
        if (line_end == std::string::npos) line_end = headers.size();
        std::string_view line(headers.data() + pos, line_end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        pos = line_end + 1;
        if (line.empty()) break;  // end of header block

        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string_view name = line.substr(0, colon);

        // Match Content-Length and Transfer-Encoding case-insensitively.
        if (header_name_eq(name, "Content-Length")) {
            std::string_view value = line.substr(colon + 1);
            // Trim OWS.
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                value.remove_prefix(1);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                value.remove_suffix(1);
            if (value.empty()) {
                out.status = FramingHeaders::Status::InvalidContentLength;
                return out;
            }
            // RFC 7230 §3.3.3 rule 4: a list of identical numeric values is
            // permissible; conflicting values must be rejected.  We support a
            // single numeric value here and reject comma lists outright as a
            // smuggling hazard.
            size_t j = 0;
            while (j < value.size() && value[j] >= '0' && value[j] <= '9') ++j;
            if (j == 0 || j != value.size()) {
                out.status = FramingHeaders::Status::InvalidContentLength;
                return out;
            }
            size_t parsed = 0;
            try {
                parsed = std::stoull(std::string(value));
            } catch (...) {
                out.status = FramingHeaders::Status::InvalidContentLength;
                return out;
            }
            if (saw_cl && parsed != out.content_length) {
                out.status = FramingHeaders::Status::ConflictingContentLength;
                return out;
            }
            if (parsed > MAX_REQUEST_BODY) {
                out.content_length = parsed;
                out.status = FramingHeaders::Status::ContentLengthTooLarge;
                return out;
            }
            saw_cl = true;
            out.content_length = parsed;
        } else if (header_name_eq(name, "Transfer-Encoding")) {
            std::string_view value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                value.remove_prefix(1);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                value.remove_suffix(1);
            // "identity" alone is harmless; anything else (including chunked,
            // which we do not decode) is a smuggling hazard.
            if (!value.empty() && !header_name_eq(value, "identity")) {
                saw_te = true;
            }
        }
    }

    if (saw_te) {
        out.status = saw_cl ? FramingHeaders::Status::CLAndTEMixed
                            : FramingHeaders::Status::TransferEncodingPresent;
    }
    return out;
}

// HTTP server supporting both TCP and Unix Domain Socket listeners.
class HttpServer {
public:
    // Construct a TCP listener bound to the given address and port.
    // bind_addr should be a numeric IPv4 address (e.g. "127.0.0.1" for
    // loopback only, "0.0.0.0" for all interfaces).
    static HttpServer tcp(int port, const std::string &bind_addr) {
        HttpServer s;
        s.mode_ = Mode::TCP;
        s.port_ = port;
        s.bind_addr_ = bind_addr;
        return s;
    }

    // Construct a Unix Domain Socket listener at the given path.
    // sock_perm defaults to owner-only (DEFAULT_SOCK_PERM = 0600); use the
    // --sock-perm flag to broaden access for cross-user deployments.
    static HttpServer uds(const std::string &path,
                          mode_t sock_perm = DEFAULT_SOCK_PERM) {
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

        using SteadyClock = std::chrono::steady_clock;

        struct ConnCtx {
            ConnState state = ConnState::ReadingHeaders;
            std::string header_buf;                    // accumulates header bytes
            std::shared_ptr<ConnectionIO> conn_io;     // bridge to worker thread
            bool body_complete = false;                // all body bytes received
            uint32_t epoll_events = EPOLLIN;           // currently registered events
            SteadyClock::time_point last_active{};      // for idle-timeout eviction
        };

        std::unordered_map<int, ConnCtx> connections;
        const auto idle_timeout = std::chrono::seconds(LSPROXY_IDLE_TIMEOUT_SECS);
        auto last_idle_sweep = SteadyClock::now();

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

            // Periodic idle sweep: close connections whose last_active is
            // older than the idle-timeout window.  Run at most once per
            // second to keep the hot path cheap.
            {
                auto now = SteadyClock::now();
                if (now - last_idle_sweep >= std::chrono::seconds(1)) {
                    last_idle_sweep = now;
                    std::vector<int> stale;
                    for (auto &[cfd, cctx] : connections) {
                        if (cctx.last_active.time_since_epoch().count() == 0) continue;
                        if (now - cctx.last_active > idle_timeout) {
                            stale.push_back(cfd);
                        }
                    }
                    for (int cfd : stale) {
                        auto cit = connections.find(cfd);
                        if (cit == connections.end()) continue;
                        LOG_INFO("[LSPROXY] Closing idle connection fd " << cfd
                                 << " (no activity for "
                                 << LSPROXY_IDLE_TIMEOUT_SECS << "s)");
                        close_conn(cfd, cit->second);
                        connections.erase(cit);
                    }
                }
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

                        // Enforce the concurrent-connection cap.  Returning
                        // a 503 with Connection: close gives the client a
                        // clear failure signal before tearing down.
                        if (connections.size() >= MAX_LSPROXY_CONNECTIONS) {
                            LOG_ERROR("Connection cap reached (" << MAX_LSPROXY_CONNECTIONS
                                      << "); rejecting fd " << client_fd);
                            const char *resp =
                                "HTTP/1.1 503 Service Unavailable\r\n"
                                "Connection: close\r\nContent-Length: 0\r\n\r\n";
                            ::send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
                            close(client_fd);
                            continue;
                        }

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

                        ConnCtx ctx_new;
                        ctx_new.last_active = SteadyClock::now();
                        connections[client_fd] = std::move(ctx_new);
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
                // Any epoll event on this fd counts as activity for the
                // idle-timeout sweep above.
                ctx.last_active = SteadyClock::now();

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

                                FramingHeaders framing = parse_framing_headers(header_data);
                                if (framing.status != FramingHeaders::Status::Ok) {
                                    const char *resp = nullptr;
                                    const char *reason = "unknown";
                                    switch (framing.status) {
                                    case FramingHeaders::Status::ContentLengthTooLarge:
                                        resp = "HTTP/1.1 413 Payload Too Large\r\n"
                                               "Connection: close\r\nContent-Length: 0\r\n\r\n";
                                        reason = "Content-Length exceeds limit";
                                        break;
                                    case FramingHeaders::Status::TransferEncodingPresent:
                                    case FramingHeaders::Status::CLAndTEMixed:
                                        resp = "HTTP/1.1 400 Bad Request\r\n"
                                               "Connection: close\r\nContent-Length: 0\r\n\r\n";
                                        reason = "Transfer-Encoding not supported";
                                        break;
                                    case FramingHeaders::Status::ConflictingContentLength:
                                    case FramingHeaders::Status::InvalidContentLength:
                                    default:
                                        resp = "HTTP/1.1 400 Bad Request\r\n"
                                               "Connection: close\r\nContent-Length: 0\r\n\r\n";
                                        reason = "invalid Content-Length";
                                        break;
                                    }
                                    LOG_ERROR("Rejecting request on fd " << fd
                                              << ": " << reason);
                                    ::send(fd, resp, strlen(resp), MSG_NOSIGNAL);
                                    close_conn(fd, ctx);
                                    connections.erase(it);
                                    continue;
                                }

                                size_t content_length = framing.content_length;

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

    HttpServer()
        : mode_(Mode::TCP),
          port_(DEFAULT_PORT),
          bind_addr_(DEFAULT_BIND_ADDR),
          sock_perm_(DEFAULT_SOCK_PERM),
          server_socket_(-1) {}

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
        server_addr.sin_port = htons(static_cast<uint16_t>(port_));
        if (inet_pton(AF_INET, bind_addr_.c_str(), &server_addr.sin_addr) != 1) {
            LOG_ERROR("Invalid --bind address (expected numeric IPv4): "
                      << bind_addr_);
            close(server_socket_);
            return false;
        }

        if (bind(server_socket_, reinterpret_cast<sockaddr *>(&server_addr),
                 sizeof(server_addr)) < 0) {
            LOG_ERROR("Failed to bind TCP socket to " << bind_addr_ << ":"
                      << port_ << ": " << strerror(errno));
            close(server_socket_);
            return false;
        }

        if (listen(server_socket_, BACKLOG) < 0) {
            LOG_ERROR("Failed to listen on TCP socket");
            close(server_socket_);
            return false;
        }

        g_server_socket = server_socket_;
        LOG_INFO("HTTP Server listening on TCP " << bind_addr_ << ":" << port_);
        if (bind_addr_ == "0.0.0.0") {
            LOG_INFO("WARNING: --bind 0.0.0.0 exposes lswasm to all network "
                     "interfaces with no authentication. Restrict access via "
                     "firewall/ACL.");
        }
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
        // Directory permissions follow the process umask; we deliberately do
        // not lock the directory to 0700 so that operators can place the
        // socket in shared directories when they need cross-user access.
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

        // Remove any pre-existing socket path before bind().  This recovers
        // from a stale UDS file left behind by an earlier crash or forced
        // stop.  Refuse to unlink symlinks: an attacker who can plant a
        // symlink in a world-writable directory could otherwise redirect the
        // unlink/chmod operations onto an unrelated file.
        {
            struct stat st{};
            if (::lstat(uds_path_.c_str(), &st) == 0) {
                if (S_ISLNK(st.st_mode)) {
                    LOG_ERROR("Refusing to operate on UDS path that is a symlink: "
                              << uds_path_
                              << ". Remove it manually after verifying its target.");
                    close(server_socket_);
                    return false;
                }
                if (!S_ISSOCK(st.st_mode)) {
                    LOG_ERROR("Refusing to unlink non-socket file at UDS path: "
                              << uds_path_);
                    close(server_socket_);
                    return false;
                }
                if (::unlink(uds_path_.c_str()) != 0) {
                    LOG_ERROR("Failed to remove existing Unix domain socket path "
                              << uds_path_ << ": " << strerror(errno));
                    close(server_socket_);
                    return false;
                }
                LOG_INFO("Removed existing Unix domain socket path " << uds_path_);
            } else if (errno != ENOENT) {
                LOG_ERROR("Failed to inspect Unix domain socket path "
                          << uds_path_ << ": " << strerror(errno));
                close(server_socket_);
                return false;
            }
        }

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

        // Constrain the umask so bind() creates the socket file with the
        // requested mode and no broader.  umask is per-process so other
        // threads briefly observe the tightened mask; bind() is the only
        // file-creation call in this critical section.
        const mode_t mask = static_cast<mode_t>(0777) & ~sock_perm_;
        mode_t prev_umask = ::umask(mask);

        int bind_rc = bind(server_socket_, reinterpret_cast<sockaddr *>(&server_addr),
                           sizeof(server_addr));
        int bind_errno = errno;
        ::umask(prev_umask);

        if (bind_rc < 0) {
            LOG_ERROR("Failed to bind Unix domain socket at " << uds_path_
                      << ": " << strerror(bind_errno));
            close(server_socket_);
            return false;
        }

        // Verify what was actually created.  If it is not a regular socket
        // owned by us, do not proceed (refuses to chmod something that an
        // attacker replaced via a path race).
        {
            struct stat st{};
            if (::lstat(uds_path_.c_str(), &st) != 0) {
                LOG_ERROR("Failed to stat newly bound UDS path " << uds_path_
                          << ": " << strerror(errno));
                close(server_socket_);
                cleanup_uds();
                return false;
            }
            if (!S_ISSOCK(st.st_mode) || st.st_uid != ::geteuid()) {
                LOG_ERROR("UDS path " << uds_path_
                          << " is not a socket owned by this process; refusing to chmod.");
                close(server_socket_);
                cleanup_uds();
                return false;
            }
            // chmod is only needed if umask alone could not produce the
            // requested permissions (e.g. setgid bits, or to widen permissions
            // beyond what umask would allow).  Skip it when the bind-time
            // mode already matches.
            const mode_t actual = st.st_mode & 0777;
            if (actual != sock_perm_) {
                if (::chmod(uds_path_.c_str(), sock_perm_) != 0) {
                    LOG_ERROR("Failed to set permissions on Unix domain socket: "
                              << strerror(errno));
                    close(server_socket_);
                    cleanup_uds();
                    return false;
                }
            }
        }

        if (listen(server_socket_, BACKLOG) < 0) {
            LOG_ERROR("Failed to listen on Unix domain socket");
            close(server_socket_);
            cleanup_uds();
            return false;
        }

        g_server_socket = server_socket_;
        g_uds_path = uds_path_;
        LOG_INFO("HTTP Server listening on Unix socket " << uds_path_
                 << " (mode 0" << std::oct << static_cast<unsigned>(sock_perm_)
                 << std::dec << ")");
        if ((sock_perm_ & 0006) != 0) {
            LOG_INFO("WARNING: UDS is world-accessible (mode 0"
                     << std::oct << static_cast<unsigned>(sock_perm_) << std::dec
                     << "). Any local user can connect; restrict via "
                     "--sock-perm or directory ACLs.");
        }
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

        // Create a response sink for HTTP transport.
        HttpResponseSink sink(conn.get());

        // Create a filter context for this request.
        uint32_t ctx_id = g_next_context_id.fetch_add(1);
        HttpFilterContext filter_ctx(ctx_id, &http_data);
        filter_ctx.setResponseSink(&sink);
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
        LOG_INFO("[HTTP] Request-phase streaming check: owner='"
                 << (filter_ctx.streamingOwner().empty() ? std::string("<none>")
                                                        : filter_ctx.streamingOwner())
                 << "' has_streaming=" << filter_ctx.hasStreamingResponse()
                 << " finished=" << filter_ctx.isStreamingFinished()
                 << " error=" << filter_ctx.hasStreamingError());
        if (filter_ctx.hasStreamingError()) {
            LOG_ERROR("[HTTP] Conflicting streaming owners detected");
            conn->setError();
            return;
        }
        if (filter_ctx.hasStreamingResponse()) {
            if (!filter_ctx.isStreamingFinished()) {
                LOG_ERROR("[HTTP] Streaming response started by module '"
                          << filter_ctx.streamingOwner()
                          << "' but was not finished");
                conn->setError();
                return;
            }
            LOG_INFO("[HTTP] Streaming response handled by WASM filter module '"
                     << filter_ctx.streamingOwner() << "'.");
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
                LOG_ERROR("[HTTP] Local response requested after streaming response started"
                          << " by module '" << filter_ctx.streamingOwner() << "'");
                conn->setError();
                return;
            }
            filter_ctx.onDone();
            std::string response = build_local_response(http_data);
            write_chunked(conn, response);
            conn->finish();
            return;
        }

        LOG_INFO("[HTTP] Response-phase streaming check: owner='"
                 << (filter_ctx.streamingOwner().empty() ? std::string("<none>")
                                                        : filter_ctx.streamingOwner())
                 << "' has_streaming=" << filter_ctx.hasStreamingResponse()
                 << " finished=" << filter_ctx.isStreamingFinished()
                 << " error=" << filter_ctx.hasStreamingError());
        if (filter_ctx.hasStreamingError()) {
            LOG_ERROR("[HTTP] Conflicting streaming owners detected");
            conn->setError();
            return;
        }
        if (filter_ctx.hasStreamingResponse()) {
            if (!filter_ctx.isStreamingFinished()) {
                LOG_ERROR("[HTTP] Streaming response started by module '"
                          << filter_ctx.streamingOwner()
                          << "' but was not finished");
                conn->setError();
                return;
            }
            LOG_INFO("[HTTP] Streaming response handled by WASM filter module '"
                     << filter_ctx.streamingOwner() << "'.");
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

    // Parse a raw HTTP/1.x request header block.  Returns false on any
    // malformed input: embedded NUL, obs-fold continuation lines, whitespace
    // between field name and colon, non-token header-name characters, or
    // CRLF inside a header value.  These each provide smuggling or
    // request-splitting pivots and must be rejected outright.
    bool parse_request(const std::string &request, HttpData &http_data) {
        if (request.find('\0') != std::string::npos) {
            LOG_ERROR("Request contains embedded NUL");
            return false;
        }

        size_t pos = 0;

        // ── Request line ─────────────────────────────────────────────────
        size_t line_end = request.find('\n', pos);
        if (line_end == std::string::npos) return false;
        std::string_view request_line(request.data() + pos,
                                       line_end - pos);
        if (!request_line.empty() && request_line.back() == '\r')
            request_line.remove_suffix(1);
        pos = line_end + 1;

        // method SP path SP version
        size_t sp1 = request_line.find(' ');
        if (sp1 == std::string_view::npos) return false;
        size_t sp2 = request_line.find(' ', sp1 + 1);
        if (sp2 == std::string_view::npos) return false;
        std::string_view method = request_line.substr(0, sp1);
        std::string_view path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
        std::string_view version = request_line.substr(sp2 + 1);

        if (method.empty() || path.empty() || version.empty()) return false;
        // method must be tokens; path and version forbid CTL chars.
        for (unsigned char c : method) {
            if (!is_tchar(c)) return false;
        }
        for (unsigned char c : path) {
            if (c < 0x20 || c == 0x7f) return false;
        }
        for (unsigned char c : version) {
            if (c < 0x20 || c == 0x7f) return false;
        }
        http_data.method.assign(method);
        http_data.path.assign(path);
        http_data.version.assign(version);

        // ── Header lines ─────────────────────────────────────────────────
        while (pos < request.size()) {
            line_end = request.find('\n', pos);
            if (line_end == std::string::npos) line_end = request.size();
            std::string_view line(request.data() + pos, line_end - pos);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            pos = line_end + 1;
            if (line.empty()) break;  // end of header block

            // Reject obs-fold (RFC 7230 §3.2.4): no line may start with WS.
            if (line.front() == ' ' || line.front() == '\t') {
                LOG_ERROR("Rejecting obs-fold header continuation");
                return false;
            }

            size_t colon = line.find(':');
            if (colon == std::string_view::npos) {
                LOG_ERROR("Header line missing colon");
                return false;
            }
            std::string_view name = line.substr(0, colon);
            // RFC 7230 §3.2.4: no whitespace between field name and colon.
            if (!is_valid_header_name(name)) {
                LOG_ERROR("Invalid header name");
                return false;
            }

            std::string_view value = line.substr(colon + 1);
            // Trim leading OWS.
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                value.remove_prefix(1);
            // Trim trailing OWS.
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                value.remove_suffix(1);
            if (!is_valid_header_value(value)) {
                LOG_ERROR("Invalid character in header value");
                return false;
            }

            http_data.request_headers.emplace_back(std::string(name),
                                                    std::string(value));
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
    std::string bind_addr_;
    std::string uds_path_;
    mode_t sock_perm_;
    int server_socket_;
};

// No-op handler for SIGUSR2: the sole purpose is to interrupt blocking
// syscalls (accept, read) with EINTR in worker threads during shutdown.
void sigusr2_handler(int) {}

// Signal handler: keep shutdown work minimal and async-signal-safe while
// forcing blocking LSAPI/HTTP syscalls to return promptly.
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        const int server_socket = g_server_socket;
        g_server_socket = -1;
        g_shutdown.store(true, std::memory_order_relaxed);
        LSAPI_Stop();
        shutdown_tracked_lsapi_fds();
        if (server_socket >= 0) {
            shutdown(server_socket, SHUT_RDWR);
            close(server_socket);
        }
        // Interrupt all worker threads blocked in accept()/read() so they
        // can observe g_shutdown and exit their loops.
        const size_t n = g_num_worker_threads.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) {
            pthread_kill(g_worker_threads[i], SIGUSR2);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  LSAPI transport mode (default)
//
//  Unless --lsproxy is specified on the command line, lswasm operates as an
//  LSAPI application process. LiteSpeed/OpenLiteSpeed connects to lswasm
//  via the LSAPI protocol instead of the standalone LSPROXY listener.
//
//  A single accept thread receives complete LSAPI requests and dispatches
//  them onto the internal worker thread pool.  Each worker owns one
//  LSAPI_Request for the lifetime of that request and responds through
//  LsapiResponseSink using the same WASM filter chain as the HTTP path.
// ═══════════════════════════════════════════════════════════════════════

#include "lsapi_response_sink.h"

namespace {

int reserve_tracked_lsapi_fd_slot() {
    for (size_t i = 0; i < MAX_TRACKED_LSAPI_FDS; ++i) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&g_tracked_lsapi_fds[i], &expected, -1,
                                        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return static_cast<int>(i);
        }
    }
    LOG_ERROR("[LSAPI] Exhausted tracked request-fd slots; shutdown may block");
    return -1;
}

void publish_tracked_lsapi_fd(int slot, int fd) {
    if (slot >= 0) {
        __atomic_store_n(&g_tracked_lsapi_fds[slot], fd, __ATOMIC_RELEASE);
    }
}

void clear_tracked_lsapi_fd(int slot) {
    if (slot >= 0) {
        __atomic_store_n(&g_tracked_lsapi_fds[slot], 0, __ATOMIC_RELEASE);
    }
}

void shutdown_tracked_lsapi_fds() {
    for (size_t i = 0; i < MAX_TRACKED_LSAPI_FDS; ++i) {
        const int fd = __atomic_load_n(&g_tracked_lsapi_fds[i], __ATOMIC_ACQUIRE);
        if (fd > 0) {
            shutdown(fd, SHUT_RDWR);
        }
    }
}

bool ensure_lsapi_listener_socket_permissions(int listen_fd, mode_t sock_perm) {
    if (listen_fd < 0) {
        return true;
    }

    sockaddr_un addr{};
    socklen_t addr_len = sizeof(addr);
    if (getsockname(listen_fd, reinterpret_cast<sockaddr *>(&addr), &addr_len) != 0) {
        LOG_ERROR("[LSAPI] getsockname() failed for listener socket: " << strerror(errno));
        return false;
    }

    if (addr.sun_family != AF_UNIX) {
        return true;
    }

    if (addr.sun_path[0] == '\0') {
        LOG_INFO("[LSAPI] Listener is using an abstract Unix socket; skipping chmod");
        return true;
    }

    // Verify the path is a socket owned by us before chmod-ing it.  This
    // guards against a symlink swap between LSAPI_CreateListenSock() and
    // here in a world-writable directory.
    struct stat st{};
    if (::lstat(addr.sun_path, &st) != 0) {
        LOG_ERROR("[LSAPI] lstat() failed on listener socket path "
                  << addr.sun_path << ": " << strerror(errno));
        return false;
    }
    if (!S_ISSOCK(st.st_mode) || st.st_uid != ::geteuid()) {
        LOG_ERROR("[LSAPI] Listener socket path " << addr.sun_path
                  << " is not a socket owned by this process; refusing to chmod.");
        return false;
    }

    if (chmod(addr.sun_path, sock_perm) != 0) {
        LOG_ERROR("[LSAPI] Failed to set permissions on LSAPI Unix socket "
                  << addr.sun_path << ": " << strerror(errno));
        return false;
    }

    LOG_INFO("[LSAPI] Set permissions on LSAPI Unix socket " << addr.sun_path
             << " to " << std::oct << static_cast<unsigned>(sock_perm) << std::dec);
    if ((sock_perm & 0006) != 0) {
        LOG_INFO("[LSAPI] WARNING: listener socket is world-accessible (mode 0"
                 << std::oct << static_cast<unsigned>(sock_perm) << std::dec
                 << "). Any local user can speak LSAPI to lswasm; restrict via "
                 "--sock-perm or directory ACLs.");
    }
    return true;
}

/// A reusable LSAPI request object.  Buffers (iovec, response buffer,
/// response header buffer, request buffer) are allocated once on construction
/// and persist across requests, avoiding per-request malloc/free overhead.
/// Between requests only lightweight pointer resets are performed via
/// LSAPI_Reset_r().
class LsapiReusableRequest {
public:
    explicit LsapiReusableRequest(int listen_fd) {
        tracked_fd_slot_ = reserve_tracked_lsapi_fd_slot();
        if (LSAPI_InitRequest(&req_, listen_fd) != 0) {
            clear_tracked_lsapi_fd(tracked_fd_slot_);
            throw std::runtime_error("[LSAPI] LSAPI_InitRequest() failed");
        }
        initialized_ = true;
    }

    ~LsapiReusableRequest() { destroy(); }

    LsapiReusableRequest(const LsapiReusableRequest &) = delete;
    LsapiReusableRequest &operator=(const LsapiReusableRequest &) = delete;

    LSAPI_Request *request() { return &req_; }

    void markAccepted() {
        request_active_ = true;
        publish_tracked_lsapi_fd(tracked_fd_slot_, req_.m_fd);
    }

    /// End the current response and close the connection, but keep all
    /// allocated buffers for reuse.  After this call the object is ready
    /// for another LSAPI_Accept_r() cycle.
    void finishRequest() {
        if (!initialized_ || !request_active_) {
            return;
        }
        clear_tracked_lsapi_fd(tracked_fd_slot_);
        LSAPI_End_Response_r(&req_);
        request_active_ = false;
        // Reset lightweight state pointers; buffers are preserved.
        LSAPI_Reset_r(&req_);
    }

    /// Finish the current response but keep the connection alive for
    /// potential reuse (HTTP keep-alive).  After this call the fd remains
    /// open; the next LSAPI_Accept_r() will attempt to read a new request
    /// on the same connection before falling back to accept().
    void keepAliveFinish() {
        if (!initialized_ || !request_active_) {
            return;
        }
        clear_tracked_lsapi_fd(tracked_fd_slot_);
        LSAPI_Finish_r(&req_);
        request_active_ = false;
    }

private:
    /// Full teardown — frees all buffers.  Only called from destructor.
    void destroy() {
        clear_tracked_lsapi_fd(tracked_fd_slot_);
        if (!initialized_) {
            return;
        }
        if (request_active_) {
            LSAPI_End_Response_r(&req_);
            request_active_ = false;
        }
        if (req_.m_fd != -1) {
            close(req_.m_fd);
            req_.m_fd = -1;
        }
        LSAPI_Release_r(&req_);
        if (req_.m_pRespBuf) {
            std::free(req_.m_pRespBuf);
            req_.m_pRespBuf = nullptr;
        }
        if (req_.m_pIovec) {
            std::free(req_.m_pIovec);
            req_.m_pIovec = nullptr;
        }
        initialized_ = false;
    }

    LSAPI_Request req_{};
    int tracked_fd_slot_ = -1;
    bool initialized_ = false;
    bool request_active_ = false;
};

/// A fixed-size pool of reusable LsapiReusableRequest objects.  The accept
/// thread takes a request from the pool before calling LSAPI_Accept_r();
/// the worker thread returns it after finishing the response.  This avoids
/// per-request malloc/free of LSAPI buffers and eliminates redundant signal
/// setup.
class LsapiRequestPool {
public:
    /// Create \p pool_size reusable request objects, each bound to \p listen_fd.
    LsapiRequestPool(int listen_fd, size_t pool_size) {
        pool_.reserve(pool_size);
        for (size_t i = 0; i < pool_size; ++i) {
            pool_.push_back(std::make_unique<LsapiReusableRequest>(listen_fd));
            available_.push(pool_.back().get());
        }
    }

    /// Take a request from the pool, blocking until one is available.
    /// Returns nullptr if the pool has been shut down.
    LsapiReusableRequest *acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !available_.empty() || stopped_; });
        if (stopped_ && available_.empty()) return nullptr;
        auto *req = available_.front();
        available_.pop();
        return req;
    }

    /// Return a request to the pool for reuse.
    void release(LsapiReusableRequest *req) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            available_.push(req);
        }
        cv_.notify_one();
    }

    /// Wake up any threads blocked in acquire().
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

private:
    std::vector<std::unique_ptr<LsapiReusableRequest>> pool_;
    std::queue<LsapiReusableRequest *> available_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopped_ = false;
};

// LSAPI_ForeachHeader_r callback: accumulate headers into HeaderPairs.
//
// Header names and values arrive from the LSAPI library which does only
// limited validation.  Drop any pair whose name is not a valid RFC 7230
// token or whose value contains CR/LF/NUL — forwarding such pairs into the
// WASM filter (and possibly back out via setHeaderMapPairs) would create a
// response-splitting pivot.
int lsapi_header_cb(const char *key, int keyLen,
                    const char *value, int valLen, void *arg) {
    if (keyLen <= 0 || valLen < 0) {
        return 1;  // skip but continue
    }
    std::string_view raw_value(value, static_cast<size_t>(valLen));
    if (!http_utils::is_valid_header_value_chars(raw_value)) {
        LOG_ERROR("[LSAPI] dropping header with CR/LF/NUL in value");
        return 1;
    }

    auto *hdrs = static_cast<HeaderPairs *>(arg);
    // LSAPI delivers CGI-style header names (HTTP_ACCEPT, HTTP_HOST, …).
    // Convert them to standard HTTP header names:
    //   - strip the "HTTP_" prefix
    //   - replace underscores with hyphens
    //   - title-case each word
    std::string name(key, static_cast<size_t>(keyLen));
    if (name.size() > 5 && name.substr(0, 5) == "HTTP_") {
        name = name.substr(5);
    }
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '_') name[i] = '-';
    }
    // Title-case: first char and chars after '-' are upper, rest lower.
    bool next_upper = true;
    for (char &c : name) {
        if (c == '-') {
            next_upper = true;
        } else if (next_upper) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            next_upper = false;
        } else {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (!http_utils::is_valid_header_name_chars(name)) {
        LOG_ERROR("[LSAPI] dropping header with invalid name characters");
        return 1;
    }
    LOG_INFO("lsapi_header: " << name << " = " << raw_value);
    hdrs->emplace_back(std::move(name), std::string(raw_value));
    return 1;  // continue iteration
}

/// Process a single LSAPI request through the WASM filter chain.
/// This is the LSAPI analogue of HttpServer::handle_request().
void lsapi_handle_request(LSAPI_Request *req) {
    LOG_INFO("Processing LSAPI request.");
    HttpData http_data;

    // ── Populate request method, path, version ──
    char *method = LSAPI_GetRequestMethod_r(req);
    http_data.method = method ? method : "GET";

    // Build the request path from SCRIPT_NAME + QUERY_STRING.
    char *script = LSAPI_GetScriptName_r(req);
    char *query  = LSAPI_GetQueryString_r(req);
    if (script) {
        http_data.path = script;
    } else {
        // Fall back to REQUEST_URI if available.
        char *uri = LSAPI_GetEnv_r(req, "REQUEST_URI");
        http_data.path = uri ? uri : "/";
    }
    if (query && query[0] != '\0') {
        http_data.path += '?';
        http_data.path += query;
    }

    // LSAPI does not expose HTTP version directly; assume HTTP/1.1.
    http_data.version = "HTTP/1.1";

    // ── Populate request headers ──
    LSAPI_ForeachHeader_r(req, lsapi_header_cb, &http_data.request_headers);

    // If Content-Type is present from the CGI env vars but missing from
    // the header list, add it.  Same for Content-Length.
    auto has_hdr = [&](const char *name) {
        for (const auto &p : http_data.request_headers) {
            if (header_name_eq(p.first, name)) return true;
        }
        return false;
    };
    if (!has_hdr("Content-Type")) {
        char *ct = LSAPI_GetHeader_r(req, H_CONTENT_TYPE);
        if (ct && ct[0] != '\0') {
            http_data.request_headers.emplace_back("Content-Type", ct);
        }
    }
    if (!has_hdr("Content-Length")) {
        char *cl = LSAPI_GetHeader_r(req, H_CONTENT_LENGTH);
        if (cl && cl[0] != '\0') {
            http_data.request_headers.emplace_back("Content-Length", cl);
        }
    }

    // ── Create ResponseSink and filter context ──
    LsapiResponseSink sink(req);
    uint32_t ctx_id = g_next_context_id.fetch_add(1);
    HttpFilterContext filter_ctx(ctx_id, &http_data);
    filter_ctx.setResponseSink(&sink);
    filter_ctx.onCreate();

    // ── Request headers phase ──
    off_t body_len = LSAPI_GetReqBodyLen_r(req);
    bool has_body = (body_len > 0);
    LOG_INFO("\n[LSAPI] Processing request in filter chain...");
    filter_ctx.onRequestHeaders(/*end_of_stream=*/!has_body);

    if (http_data.has_local_response) {
        LOG_INFO("[LSAPI] WASM filter sent local response.");
        sink.sendHeaders(http_data.local_response_code,
                         http_data.local_response_additional_headers,
                         /*streaming=*/false);
        sink.writeBody(http_data.local_response_body);
        sink.finishBody();
        return;
    }

    // ── Stream request body ──
    if (has_body) {
        char body_buf[BODY_CHUNK_SIZE];
        off_t body_consumed = 0;
        while (body_consumed < body_len && !http_data.has_local_response) {
            size_t want = std::min(static_cast<size_t>(body_len - body_consumed),
                                   BODY_CHUNK_SIZE);
            ssize_t n = LSAPI_ReadReqBody_r(req, body_buf, want);
            if (n <= 0) {
                LOG_ERROR("[LSAPI] Request body read error after "
                          << body_consumed << " / " << body_len << " bytes");
                return;
            }
            body_consumed += n;
            http_data.request_body.assign(body_buf, static_cast<size_t>(n));
            filter_ctx.onRequestBody(body_consumed >= body_len);
        }
    }

    if (!http_data.has_local_response) {
        filter_ctx.onRequestTrailers();
    }

    if (http_data.has_local_response) {
        sink.sendHeaders(http_data.local_response_code,
                         http_data.local_response_additional_headers,
                         /*streaming=*/false);
        sink.writeBody(http_data.local_response_body);
        sink.finishBody();
        return;
    }

    // Request-phase streaming responses.
    LOG_INFO("[LSAPI] Request-phase streaming check: owner='"
             << (filter_ctx.streamingOwner().empty() ? std::string("<none>")
                                                    : filter_ctx.streamingOwner())
             << "' has_streaming=" << filter_ctx.hasStreamingResponse()
             << " finished=" << filter_ctx.isStreamingFinished()
             << " error=" << filter_ctx.hasStreamingError());
    if (filter_ctx.hasStreamingError()) {
        LOG_ERROR("[LSAPI] Conflicting streaming owners detected");
        return;
    }
    if (filter_ctx.hasStreamingResponse()) {
        if (!filter_ctx.isStreamingFinished()) {
            LOG_ERROR("[LSAPI] Streaming response started by module '"
                      << filter_ctx.streamingOwner()
                      << "' but was not finished");
            return;
        }
        LOG_INFO("[LSAPI] Streaming response handled by WASM filter module '"
                 << filter_ctx.streamingOwner() << "'.");
        filter_ctx.onDone();
        return;
    }

    // ── Generate default response ──
    http_data.response_body.clear();
    if (g_body_pacifier) {
        // Re-use the diagnostic body builder from HttpServer (static-like).
        http_data.response_body = "=== WASM LSAPI Proxy ===\n";
        http_data.response_body += "Method: " + http_data.method + "\n";
        http_data.response_body += "Path: " + http_data.path + "\n";
    }

    http_data.response_headers.clear();
    http_data.response_headers.emplace_back("Content-Type", "text/plain");

    // ── Response phases ──
    LOG_INFO("[LSAPI] Processing response in filter chain...");
    filter_ctx.onResponseHeaders();
    filter_ctx.onResponseBody();
    filter_ctx.onResponseTrailers();

    if (http_data.has_local_response) {
        if (filter_ctx.hasStreamingResponse()) {
            LOG_ERROR("[LSAPI] Local response after streaming started by module '"
                      << filter_ctx.streamingOwner() << "'");
            return;
        }
        filter_ctx.onDone();
        sink.sendHeaders(http_data.local_response_code,
                         http_data.local_response_additional_headers,
                         /*streaming=*/false);
        sink.writeBody(http_data.local_response_body);
        sink.finishBody();
        return;
    }

    LOG_INFO("[LSAPI] Response-phase streaming check: owner='"
             << (filter_ctx.streamingOwner().empty() ? std::string("<none>")
                                                    : filter_ctx.streamingOwner())
             << "' has_streaming=" << filter_ctx.hasStreamingResponse()
             << " finished=" << filter_ctx.isStreamingFinished()
             << " error=" << filter_ctx.hasStreamingError());
    if (filter_ctx.hasStreamingError()) {
        LOG_ERROR("[LSAPI] Conflicting streaming owners detected");
        return;
    }
    if (filter_ctx.hasStreamingResponse()) {
        if (!filter_ctx.isStreamingFinished()) {
            LOG_ERROR("[LSAPI] Streaming response started by module '"
                      << filter_ctx.streamingOwner()
                      << "' but was not finished");
            return;
        }
        LOG_INFO("[LSAPI] Streaming response handled by WASM filter module '"
                 << filter_ctx.streamingOwner() << "'.");
        filter_ctx.onDone();
        return;
    }

    filter_ctx.onDone();

    // Add Content-Length.
    HeaderPairs &hdrs = http_data.response_headers;
    hdrs.erase(std::remove_if(hdrs.begin(), hdrs.end(),
        [](const std::pair<std::string, std::string> &p) {
            return header_name_eq(p.first, "Content-Length");
        }), hdrs.end());
    hdrs.emplace_back("Content-Length",
                       std::to_string(http_data.response_body.length()));

    sink.sendHeaders(200, hdrs, /*streaming=*/false);
    sink.writeBody(http_data.response_body);
    sink.finishBody();
}

/// Run the LSAPI accept loop.  Blocks until the web server closes the
/// connection or the process is terminated.
///
/// In LSAPI mode, the WASM runtime is initialised once up front in this
/// process.  When LSAPI provides a listener socket, lswasm accepts requests
/// on one thread and dispatches them to the worker pool.  When LSAPI instead
/// provides a single connected channel, requests are handled serially on that
/// channel because there is no independent listener to accept from.
int run_lsapi_loop(const std::string &wasm_module_path,
                   const std::unordered_map<std::string, std::string> &wasm_envs,
                   size_t num_workers,
                   const std::string &bind_addr,
                   mode_t sock_perm) {
    // If a bind address was provided, create a listener socket and dup2 it
    // onto fd 0 so that LSAPI_Init() picks it up as the listening socket.
    if (!bind_addr.empty()) {
        LOG_INFO("[LSAPI] Creating listener socket on " << bind_addr);
        int fd = LSAPI_CreateListenSock(bind_addr.c_str(), BACKLOG);
        if (fd == -1) {
            LOG_ERROR("[LSAPI] LSAPI_CreateListenSock(\"" << bind_addr << "\") failed");
            return 1;
        }
        if (fd != 0) {
            if (dup2(fd, 0) < 0) {
                const int err = errno;
                close(fd);
                LOG_ERROR("[LSAPI] dup2(" << fd << ", 0) failed: " << strerror(err));
                return 1;
            }
            close(fd);
        }
    }

    LOG_INFO("[LSAPI] Initializing LSAPI...");
    if (LSAPI_Init() < 0) {
        LOG_ERROR("[LSAPI] LSAPI_Init() failed");
        return 1;
    }
    LSAPI_Init_Env_Parameters(nullptr);

    const int listen_fd = g_req.m_fdListen;
    const int connected_fd = g_req.m_fd;
    if (listen_fd < 0 && connected_fd < 0) {
        LOG_ERROR("[LSAPI] No listener or connected fd available after LSAPI_Init()");
        return 1;
    }
    if (listen_fd >= 0 && !ensure_lsapi_listener_socket_permissions(listen_fd, sock_perm)) {
        return 1;
    }
    g_server_socket = (listen_fd >= 0) ? listen_fd : connected_fd;

    g_module_manager = std::make_unique<WasmModuleManager>();
    if (!wasm_envs.empty()) {
        // Log only the keys; values may contain secrets.
        LOG_INFO("[LSAPI] WASM environment variables (" << wasm_envs.size() << "):");
        for (const auto &[key, value] : wasm_envs) {
            (void)value;
            LOG_INFO("[LSAPI]   " << key << "=<redacted>");
        }
        g_module_manager->setEnvironmentVariables(wasm_envs);
    }

    {
        std::string module_name = "custom_filter";
        LOG_INFO("[LSAPI] Loading WASM filter module: " << wasm_module_path);
        if (!g_module_manager->loadModule(wasm_module_path, module_name)) {
            LOG_ERROR("[LSAPI] Failed to load WASM module");
            g_module_manager.reset();
            return 1;
        }
        LOG_INFO("[LSAPI] ✓ Filter module loaded successfully");
    }

    try {
        LOG_INFO("[LSAPI] Entering accept loop...");
        if (listen_fd >= 0) {
            // ── Worker-owned accept loops with keep-alive ──
            //
            // Each worker thread owns one LSAPI_Request and runs its own
            // accept loop.  After processing a request, the worker calls
            // LSAPI_Finish_r() (keep-alive finish) instead of
            // LSAPI_End_Response_r() (close).  When the web server sends
            // another request on the same connection, the next
            // LSAPI_Accept_r() reads it directly without a new accept()
            // syscall — eliminating per-request accept/close overhead.
            //
            // WASM VM warmup: each worker thread calls warmupThreadLocal()
            // before entering the accept loop, forcing the expensive
            // thread-local VM clone + signal setup to happen once at
            // startup rather than on the first real request.

            // Default to hardware_concurrency if num_workers is 0.
            size_t actual_workers = num_workers;
            if (actual_workers == 0) {
                actual_workers = std::thread::hardware_concurrency();
                if (actual_workers == 0) actual_workers = 4;
            }

            std::vector<std::unique_ptr<LsapiReusableRequest>> workers;
            std::vector<std::thread> threads;
            workers.reserve(actual_workers);
            threads.reserve(actual_workers);

            for (size_t i = 0; i < actual_workers; ++i) {
                workers.push_back(std::make_unique<LsapiReusableRequest>(listen_fd));
            }

            LOG_INFO("[LSAPI] Listener mode: launching " << actual_workers
                     << " worker threads with keep-alive accept loops");

            for (size_t i = 0; i < actual_workers; ++i) {
                LsapiReusableRequest *req = workers[i].get();
                threads.emplace_back([req, i]() {
                    // Register this thread so the signal handler can
                    // pthread_kill(SIGUSR2) to unblock accept()/read().
                    if (i < MAX_WORKER_THREADS) {
                        g_worker_threads[i] = pthread_self();
                        g_num_worker_threads.store(
                            std::max(g_num_worker_threads.load(std::memory_order_relaxed), i + 1),
                            std::memory_order_release);
                    }

                    // Warm up the WASM VM on this thread before accepting
                    // any requests, so the expensive VM clone + signal
                    // setup happens now rather than on the first request.
                    if (g_module_manager) {
                        LOG_INFO("[LSAPI] Worker " << i << " warming up WASM VM...");
                        g_module_manager->warmupThreadLocal();
                        LOG_INFO("[LSAPI] Worker " << i << " WASM VM warm-up complete");
                    }

                    // Per-worker accept loop with keep-alive.
                    while (!g_shutdown.load(std::memory_order_relaxed)) {
                        if (LSAPI_Accept_r(req->request()) < 0) {
                            if (!g_shutdown.load(std::memory_order_relaxed)) {
                                LOG_ERROR("[LSAPI] Worker " << i
                                          << " LSAPI_Accept_r() failed: "
                                          << strerror(errno));
                            }
                            break;
                        }
                        if (g_shutdown.load(std::memory_order_relaxed) ||
                            !LSAPI_IsRunning() || req->request()->m_fd == -1) {
                            break;
                        }

                        req->markAccepted();
                        try {
                            lsapi_handle_request(req->request());
                        } catch (const std::exception &e) {
                            LOG_ERROR("[LSAPI] Worker " << i
                                      << " exception: " << e.what());
                        } catch (...) {
                            LOG_ERROR("[LSAPI] Worker " << i
                                      << " unknown exception");
                        }
                        // Keep-alive finish: sends RESP_END but keeps the
                        // connection fd open for the next request.
                        req->keepAliveFinish();
                    }
                    LOG_INFO("[LSAPI] Worker " << i << " exiting accept loop");
                });
            }

            // Wait for all workers to exit.
            for (auto &t : threads) {
                if (t.joinable()) t.join();
            }
            LOG_INFO("[LSAPI] All worker threads joined");
        } else {
            // ── Connected-channel mode (single fd from web server) ──
            // Use keep-alive finish here too so the web server can send
            // multiple requests on the same connection.
            LOG_INFO("[LSAPI] Connected-channel mode detected; processing requests serially");

            // Warm up the WASM VM on the main thread.
            if (g_module_manager) {
                g_module_manager->warmupThreadLocal();
            }

            while (!g_shutdown.load(std::memory_order_relaxed)) {
                if (LSAPI_Accept_r(&g_req) < 0) {
                    if (!g_shutdown.load(std::memory_order_relaxed)) {
                        LOG_ERROR("[LSAPI] LSAPI_Accept_r() failed");
                    }
                    break;
                }
                if (g_shutdown.load(std::memory_order_relaxed) || !LSAPI_IsRunning() ||
                    g_req.m_fd == -1) {
                    break;
                }

                try {
                    lsapi_handle_request(&g_req);
                } catch (const std::exception &e) {
                    LOG_ERROR("[LSAPI] Request processing threw exception: " << e.what());
                } catch (...) {
                    LOG_ERROR("[LSAPI] Request processing threw unknown exception");
                }
                // LSAPI_Finish_r keeps the connection alive for the next
                // request.  LSAPI_Accept_r will call it again at the top
                // of the loop (idempotent if already finished).
                LSAPI_Finish_r(&g_req);
            }
        }
    } catch (const std::exception &e) {
        LOG_ERROR("[LSAPI] Error: " << e.what());
        g_module_manager.reset();
        return 1;
    }

    LOG_INFO("[LSAPI] Accept loop exited.");
    g_module_manager.reset();
    return 0;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════
//  main()
// ═══════════════════════════════════════════════════════════════════════

int main(int argc, char *argv[]) {
    if (geteuid() == 0) {
        std::cerr << "ERROR: lswasm must not be run as root." << std::endl;
        return 1;
    }

    int port = DEFAULT_PORT;
    std::string wasm_module_path;
    std::string uds_path = DEFAULT_UDS_PATH;
    std::string bind_addr = DEFAULT_BIND_ADDR;
    mode_t sock_perm = DEFAULT_SOCK_PERM;
    std::unordered_map<std::string, std::string> wasm_envs;
    bool debug = false;
    bool port_specified = false;
    bool uds_specified = false;
    bool bind_specified = false;
    bool sock_perm_specified = false;
    bool lsapi_mode = true;
    std::string lsapi_bind_addr;  // Optional LSAPI listening socket address (e.g. "127.0.0.1:8000")
    size_t num_workers = 0;  // 0 = auto (hardware_concurrency)
    constexpr size_t WORKER_HARD_CAP = MAX_WORKER_THREADS;

    // Helper: parse a non-negative integer CLI argument with bounds.
    auto parse_uint_arg = [](const char *flag, const char *raw,
                              unsigned long lo, unsigned long hi,
                              unsigned long &out) -> bool {
        if (!raw || *raw == '\0') {
            std::cerr << "Error: " << flag << " requires a value.\n";
            return false;
        }
        char *endptr = nullptr;
        errno = 0;
        unsigned long parsed = std::strtoul(raw, &endptr, 10);
        if (errno != 0 || endptr == raw || *endptr != '\0') {
            std::cerr << "Error: " << flag << " requires a non-negative integer (got '"
                      << raw << "').\n";
            return false;
        }
        if (parsed < lo || parsed > hi) {
            std::cerr << "Error: " << flag << " out of range [" << lo << ", "
                      << hi << "] (got " << parsed << ").\n";
            return false;
        }
        out = parsed;
        return true;
    };

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            unsigned long parsed = 0;
            if (!parse_uint_arg("--port", argv[++i], 1, 65535, parsed)) {
                return 1;
            }
            port = static_cast<int>(parsed);
            port_specified = true;
        } else if (arg == "--bind" && i + 1 < argc) {
            bind_addr = argv[++i];
            bind_specified = true;
        } else if (arg == "--uds" && i + 1 < argc) {
            uds_path = argv[++i];
            uds_specified = true;
        } else if (arg == "--sock-perm" && i + 1 < argc) {
            const char *val = argv[++i];
            char *endptr = nullptr;
            errno = 0;
            unsigned long parsed = std::strtoul(val, &endptr, 8);
            if (errno != 0 || endptr == val || *endptr != '\0' || parsed > 0777) {
                LOG_ERROR("Invalid --sock-perm value (expected octal 0-0777): " << val);
                return 1;
            }
            sock_perm = static_cast<mode_t>(parsed);
            sock_perm_specified = true;
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
            unsigned long parsed = 0;
            if (!parse_uint_arg("--workers", argv[++i], 1, WORKER_HARD_CAP, parsed)) {
                return 1;
            }
            num_workers = static_cast<size_t>(parsed);
        } else if (arg == "--lsapi-addr" && i + 1 < argc) {
            lsapi_bind_addr = argv[++i];
        } else if (arg == "--lsproxy") {
            lsapi_mode = false;
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
            std::cout << "  --port PORT      : Listen on TCP port in LSPROXY mode (instead of UDS)\n";
            std::cout << "  --bind ADDR      : TCP bind address for LSPROXY mode (default: "
                      << DEFAULT_BIND_ADDR << "; use 0.0.0.0 for all interfaces)\n";
            std::cout << "  --uds PATH       : Unix domain socket path for LSPROXY mode (default: "
                      << DEFAULT_UDS_PATH << ")\n";
            std::cout << "  --sock-perm MODE : Set listener UDS file permissions in octal (default: 0"
                      << std::oct << static_cast<unsigned>(DEFAULT_SOCK_PERM) << std::dec
                      << ", owner-only).\n"
                      << "                     Broaden (e.g. 0660 with group setup, or 0666) when the\n"
                      << "                     web server or other clients run as a different user.\n";
            std::cout << "  --module PATH    : Load WASM filter module (required)\n";
            std::cout << "  --env KEY=VALUE  : Set environment variable for WASM module (repeatable)\n";
            std::cout << "  --workers N      : Number of worker threads (default: hardware_concurrency, max "
                      << WORKER_HARD_CAP << ")\n";
            std::cout << "  --lsapi-addr ADDR: Bind LSAPI to address (e.g. 127.0.0.1:8000 or /tmp/lswasm.sock)\n";
            std::cout << "  --lsproxy        : Switch from default LSAPI mode to standalone LSPROXY mode\n";
            std::cout << "  --body-pacifier  : Include diagnostic body in generated responses\n";
            std::cout << "  --debug          : Enable debug logging to "
                      << lswasm_log::LOG_PATH << "\n";
            std::cout << "  --version        : Show version number\n";
            std::cout << "  --help           : Show this help message\n";
            std::cout << "\nBy default, lswasm runs in LSAPI mode.\n";
            std::cout << "Use --lsproxy for standalone UDS/TCP LSPROXY mode. "
                      << "When both --port and --uds are given, only --uds is used.\n";
            std::cout << "\nSecurity defaults: TCP listener binds to "
                      << DEFAULT_BIND_ADDR << " and UDS sockets are created mode 0"
                      << std::oct << static_cast<unsigned>(DEFAULT_SOCK_PERM) << std::dec
                      << " (owner-only).\n"
                      << "Override --bind / --sock-perm when other users or hosts need access; "
                      << "lswasm has no built-in authentication.\n";
            return 0;
        }
    }

    // Validate LSAPI-vs-LSPROXY option usage.
    // --sock-perm is accepted in either mode: LSAPI listeners created via
    // --lsapi-addr also need permission control.
    if (lsapi_mode && (port_specified || bind_specified || uds_specified)) {
        std::cerr << "Error: --port, --bind, and --uds require --lsproxy.\n";
        return 1;
    }
    if (!lsapi_mode && !lsapi_bind_addr.empty()) {
        std::cerr << "Error: --lsapi-addr cannot be used with --lsproxy.\n";
        return 1;
    }

    // Initialize logging: active if /tmp/lswasm.dolog exists or --debug is given.
    lswasm_log::log_init(debug);

    // Validate --module (required for all modes).
    if (wasm_module_path.empty()) {
        LOG_ERROR("No WASM module specified. Use --module <path> to load a filter.");
        std::cerr << "Error: --module is required. Run with --help for usage.\n";
        return 1;
    }

    // Print runtime information
    LOG_INFO("\n=== lswasm " << LSWASM_VERSION << " ===");
    LOG_INFO("Transport: " << (lsapi_mode ? "LSAPI" : "LSPROXY"));
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

    // SIGUSR2: no-op handler used to interrupt worker threads during shutdown.
    {
        struct sigaction sa2 {};
        sa2.sa_handler = sigusr2_handler;
        sigemptyset(&sa2.sa_mask);
        sa2.sa_flags = 0;  // No SA_RESTART – we need EINTR.
        sigaction(SIGUSR2, &sa2, nullptr);
    }

    struct sigaction sa {};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // No SA_RESTART – we want blocking syscalls to return EINTR.
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // ── Branch on transport mode ──────────────────────────────────────────
    if (lsapi_mode) {
        // LSAPI transport (default) — single-process runtime initialization.
        // Listener-based LSAPI setups use threaded dispatch; direct-channel
        // LSAPI setups fall back to serial request processing.
        LOG_INFO("Starting LSAPI transport mode...");
        return run_lsapi_loop(wasm_module_path, wasm_envs, num_workers,
                              lsapi_bind_addr, sock_perm);
    }

    // ── LSPROXY transport mode (--lsproxy) ────────────────────────────────
    // Initialize WASM module manager and load the module eagerly (no fork).
    g_module_manager = std::make_unique<WasmModuleManager>();

    if (!wasm_envs.empty()) {
        // Log only the keys.  Values are operator-supplied configuration that
        // commonly contains secrets (API keys, tokens); never write them to
        // the log file or stderr.
        LOG_INFO("WASM environment variables (" << wasm_envs.size()
                 << "):");
        for (const auto &[key, value] : wasm_envs) {
            (void)value;
            LOG_INFO("  " << key << "=<redacted>");
        }
        g_module_manager->setEnvironmentVariables(wasm_envs);
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

    // ── LSPROXY transport mode (--lsproxy) ────────────────────────────────
    // Create thread pool for worker threads.
    ThreadPool pool(num_workers);
    LOG_INFO("Thread pool started with " << pool.size() << " workers");

    try {
        // Create standalone LSPROXY listener: default to UDS; use TCP only if
        // --port was explicitly given without a custom --uds override.
        std::unique_ptr<HttpServer> server;
        bool explicit_uds = (uds_path != DEFAULT_UDS_PATH);
        if (explicit_uds || !port_specified) {
            server = std::make_unique<HttpServer>(HttpServer::uds(uds_path, sock_perm));
        } else {
            server = std::make_unique<HttpServer>(HttpServer::tcp(port, bind_addr));
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
