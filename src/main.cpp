#include <iostream>
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
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <csignal>
#include <cerrno>

#include "http_filter.h"
#include "wasm_module_manager.h"

// HTTP Server Configuration
const int DEFAULT_PORT = 8080;
const int BACKLOG = 5;
const int BUFFER_SIZE = 4096;
const int MAX_EPOLL_EVENTS = 64;
const size_t MAX_REQUEST_SIZE = 65536;  // 64 KB limit per request

// Global state
static volatile sig_atomic_t g_shutdown = 0;
static int g_server_socket = -1;  // For signal handler to unblock accept()
static std::string g_uds_path;    // For cleanup on shutdown
static std::atomic<uint32_t> g_next_context_id{1};
std::unique_ptr<WasmModuleManager> g_module_manager;

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
    static HttpServer uds(const std::string &path) {
        HttpServer s;
        s.mode_ = Mode::UDS;
        s.uds_path_ = path;
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

    void accept_connections() {
        int epoll_fd = epoll_create1(0);
        if (epoll_fd < 0) {
            LOG_ERROR("Failed to create epoll fd: " << strerror(errno));
            return;
        }

        // Make the server socket non-blocking so accept() won't block.
        set_nonblocking(server_socket_);

        // Register the server (listening) socket with epoll.
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = server_socket_;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket_, &ev) < 0) {
            LOG_ERROR("Failed to add server socket to epoll: " << strerror(errno));
            close(epoll_fd);
            return;
        }

        // Per-connection read buffers keyed by client fd.
        std::unordered_map<int, std::string> client_buffers;

        struct epoll_event events[MAX_EPOLL_EVENTS];

        while (g_shutdown == 0) {
            int nfds = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, 200 /*ms*/);
            if (nfds < 0) {
                if (errno == EINTR) continue;  // Interrupted by signal
                if (g_shutdown != 0) break;
                LOG_ERROR("epoll_wait error: " << strerror(errno));
                break;
            }

            for (int i = 0; i < nfds; ++i) {
                int fd = events[i].data.fd;

                if (fd == server_socket_) {
                    // Accept all pending connections.
                    while (true) {
                        sockaddr_storage client_addr{};
                        socklen_t client_addrlen = sizeof(client_addr);
                        int client_socket = accept(server_socket_,
                                                   reinterpret_cast<sockaddr *>(&client_addr),
                                                   &client_addrlen);
                        if (client_socket < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                break;  // No more pending connections.
                            }
                            if (g_shutdown != 0) break;
                            LOG_ERROR("Accept error: " << strerror(errno));
                            break;
                        }

                        set_nonblocking(client_socket);

                        struct epoll_event client_ev{};
                        client_ev.events = EPOLLIN;
                        client_ev.data.fd = client_socket;
                        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &client_ev) < 0) {
                            LOG_ERROR("Failed to add client socket to epoll: "
                                      << strerror(errno));
                            close(client_socket);
                            continue;
                        }

                        client_buffers[client_socket] = std::string();
                    }
                } else {
                    // Client socket is readable — accumulate data.
                    char buf[BUFFER_SIZE];
                    ssize_t n = recv(fd, buf, sizeof(buf), 0);

                    if (n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            continue;  // No data right now, wait for next epoll event.
                        }
                        // Real error — clean up.
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                        close(fd);
                        client_buffers.erase(fd);
                        continue;
                    }
                    if (n == 0) {
                        // Peer closed connection.
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                        close(fd);
                        client_buffers.erase(fd);
                        continue;
                    }

                    client_buffers[fd].append(buf, static_cast<size_t>(n));

                    // Guard against unbounded buffer growth (e.g. slow / malicious clients).
                    if (client_buffers[fd].size() > MAX_REQUEST_SIZE) {
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                        close(fd);
                        client_buffers.erase(fd);
                        continue;
                    }

                    // Check whether we have received the full HTTP headers.
                    if (client_buffers[fd].find("\r\n\r\n") != std::string::npos) {
                        // Full request headers received — process synchronously.
                        handle_client_data(fd, client_buffers[fd]);

                        // Done with this connection.
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                        close(fd);
                        client_buffers.erase(fd);
                    }
                }
            }
        }

        // Clean up remaining client connections.
        for (auto &[fd, _buf] : client_buffers) {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
            close(fd);
        }
        client_buffers.clear();
        close(epoll_fd);
    }

private:
    enum class Mode { TCP, UDS };

    HttpServer() : mode_(Mode::TCP), port_(DEFAULT_PORT), server_socket_(-1) {}

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

        // Restrict socket access to the owner only (rw-------).
        if (chmod(uds_path_.c_str(), 0600) != 0) {
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

    // Write all bytes to a non-blocking socket, retrying on partial writes.
    static void send_all(int fd, const std::string &data) {
        // Switch to blocking mode for the synchronous send so we don't
        // have to manage EAGAIN / partial-write retries ourselves.
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

        size_t total_sent = 0;
        while (total_sent < data.size()) {
            ssize_t n = ::send(fd, data.c_str() + total_sent,
                               data.size() - total_sent, 0);
            if (n < 0) {
                LOG_ERROR("send error: " << strerror(errno));
                break;
            }
            total_sent += static_cast<size_t>(n);
        }

        // Restore non-blocking mode (fd will be closed shortly, but be tidy).
        fcntl(fd, F_SETFL, flags);
    }

    // ── Request handling (shared by both modes) ─────────────────────────

    // Process a fully-accumulated HTTP request for the given client fd.
    // The caller (epoll loop) is responsible for closing the fd afterwards.
    void handle_client_data(int client_socket, const std::string &request) {
        HttpData http_data;

        if (!parse_request(request, http_data)) {
            return;
        }

        // create a filter context for this request
        uint32_t ctx_id = g_next_context_id.fetch_add(1);
        HttpFilterContext filter_ctx(ctx_id, &http_data);
        filter_ctx.onCreate();

        // Execute phases via context methods
        LOG_INFO("\n[HTTP] Processing request in filter chain...");
        filter_ctx.onRequestHeaders();

        // If the WASM filter sent a local response, use it directly.
        if (http_data.has_local_response) {
            LOG_INFO("[HTTP] WASM filter sent local response, using it.");
            auto response = build_local_response(http_data);
            send_all(client_socket, response);
            return;
        }

        filter_ctx.onRequestBody();
        filter_ctx.onRequestTrailers();

        // Generate the response body first.
        std::string body = build_response_body(http_data);

        // Populate default response headers (without Content-Length — added after filter chain).
        http_data.response_headers.clear();
        http_data.response_headers.emplace_back("Content-Type", "text/plain");
        http_data.response_headers.emplace_back("Connection", "close");

        // Execute response phases via context — WASM modules can now modify response_headers.
        LOG_INFO("[HTTP] Processing response in filter chain...");
        filter_ctx.onResponseHeaders();
        filter_ctx.onResponseBody();
        filter_ctx.onResponseTrailers();
        filter_ctx.onDone();

        // Ensure Content-Length is correct after filter chain (remove any stale value, re-add).
        auto &hdrs = http_data.response_headers;
        hdrs.erase(std::remove_if(hdrs.begin(), hdrs.end(),
            [](const auto &p) { return header_name_eq(p.first, "Content-Length"); }), hdrs.end());
        hdrs.emplace_back("Content-Length", std::to_string(body.length()));

        // Serialize the final response using (potentially modified) headers.
        auto response = serialize_response(200, http_data.response_headers, body);
        send_all(client_socket, response);
    }

    bool parse_request(const std::string& request, HttpData &http_data) {
        std::istringstream iss(request);
        iss >> http_data.method >> http_data.path >> http_data.version;
        
        if (http_data.method.empty() || http_data.path.empty()) {
            return false;
        }

        // Parse headers (simplified)
        std::string line;
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
                http_data.request_headers.emplace_back(std::move(header_name), std::move(header_value));
            }
        }

        return true;
    }

    // Map common HTTP status codes to reason phrases.
    static const char *reason_phrase(uint32_t status_code) {
        switch (status_code) {
            case 200: return "OK";
            case 201: return "Created";
            case 204: return "No Content";
            case 301: return "Moved Permanently";
            case 302: return "Found";
            case 304: return "Not Modified";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 500: return "Internal Server Error";
            case 502: return "Bad Gateway";
            case 503: return "Service Unavailable";
            default:  return "";
        }
    }

    // Serialize an HTTP response from status code, headers, and body.
    std::string serialize_response(uint32_t status_code, const HeaderPairs &headers,
                                   const std::string &body) {
        std::ostringstream response;
        response << "HTTP/1.1 " << status_code << " " << reason_phrase(status_code) << "\r\n";
        for (const auto &[key, value] : headers) {
            response << key << ": " << value << "\r\n";
        }
        response << "\r\n";
        response << body;
        return response.str();
    }

    // Build an HTTP response from the WASM filter's local response.
    std::string build_local_response(const HttpData& http_data) {
        HeaderPairs headers;
        headers.emplace_back("Content-Type", "text/plain");
        headers.emplace_back("X-Powered-By", "lswasm/proxy-wasm");
        headers.emplace_back("Connection", "close");
        // Merge additional headers from sendLocalResponse, but skip reserved names
        // that are managed by the host to prevent duplicate or conflicting values.
        for (const auto &h : http_data.local_response_additional_headers) {
            if (header_name_eq(h.first, "Content-Length") ||
                header_name_eq(h.first, "Content-Type") ||
                header_name_eq(h.first, "Connection")) {
                continue;
            }
            headers.emplace_back(h.first, h.second);
        }
        headers.emplace_back("Content-Length", std::to_string(http_data.local_response_body.length()));
        return serialize_response(http_data.local_response_code, headers, http_data.local_response_body);
    }

    // Build the response body (without headers).
    std::string build_response_body(const HttpData& http_data) {
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
#else
        body += "  ℹ No WASM runtime enabled\n";
#endif

        body += "\nFilter Status:\n";
        if (g_module_manager) {
            auto modules = g_module_manager->getLoadedModules();
            if (modules.empty()) {
                body += "  • No filters loaded\n";
            } else {
                body += "  Loaded filters:\n";
                for (const auto &module : modules) {
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

    Mode mode_;
    int port_;
    std::string uds_path_;
    int server_socket_;
};

// Signal handler (only async-signal-safe operations)
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_shutdown = 1;
        // Shutdown the listening socket to unblock accept()
        if (g_server_socket >= 0) {
            shutdown(g_server_socket, SHUT_RDWR);
        }
    }
}

int main(int argc, char* argv[]) {
    int port = DEFAULT_PORT;
    std::string wasm_module_path;
    std::string uds_path;
    std::unordered_map<std::string, std::string> wasm_envs;
    bool debug = false;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--uds" && i + 1 < argc) {
            uds_path = argv[++i];
        } else if (arg == "--module" && i + 1 < argc) {
            wasm_module_path = argv[++i];
        } else if (arg == "--env" && i + 1 < argc) {
            // Parse KEY=VALUE environment variable
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
        } else if (arg == "--debug") {
            debug = true;
        } else if (arg == "--help") {
            std::cout << "WASM HTTP Proxy Server with Proxy-WASM Support\n";
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  --port PORT      : Listen on TCP port (default: " << DEFAULT_PORT << ")\n";
            std::cout << "  --uds PATH       : Listen on Unix domain socket at PATH\n";
            std::cout << "  --module PATH    : Load WASM filter module\n";
            std::cout << "  --env KEY=VALUE  : Set environment variable for WASM module (repeatable)\n";
            std::cout << "  --debug          : Enable debug logging to " << lswasm_log::LOG_PATH << "\n";
            std::cout << "  --help           : Show this help message\n";
            std::cout << "\nWhen both --port and --uds are given, only --uds is used.\n";
            return 0;
        }
    }

    // Initialize logging: active if /tmp/lshttpd/lswasm.dolog exists or --debug is given.
    lswasm_log::log_init(debug);

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize WASM module manager
    g_module_manager = std::make_unique<WasmModuleManager>();

    // Print runtime information
    LOG_INFO("\n=== WASM HTTP Proxy Server ===");
#if defined(WASM_RUNTIME_WASMTIME)
    LOG_INFO("✓ Wasmtime runtime enabled");
#elif defined(WASM_RUNTIME_V8)
    LOG_INFO("✓ V8 runtime enabled");
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

    // Load WASM module if provided
    if (!wasm_module_path.empty()) {
        std::string module_name = "custom_filter";
        LOG_INFO("Loading WASM filter module: " << wasm_module_path);
        if (g_module_manager->loadModule(wasm_module_path, module_name)) {
            LOG_INFO("✓ Filter module loaded successfully");
        } else {
            LOG_ERROR("✗ Failed to load filter module");
        }
    }

    try {
        // Create server: prefer UDS if specified, otherwise TCP.
        std::unique_ptr<HttpServer> server;
        if (!uds_path.empty()) {
            server = std::make_unique<HttpServer>(HttpServer::uds(uds_path));
        } else {
            server = std::make_unique<HttpServer>(HttpServer::tcp(port));
        }

        if (!server->start()) {
            LOG_ERROR("Failed to start HTTP server");
            return 1;
        }

        LOG_INFO("Server ready. Press Ctrl+C to stop.\n");

        // Accept incoming connections
        server->accept_connections();

    } catch (const std::exception& e) {
        LOG_ERROR("Error: " << e.what());
        return 1;
    }

    // Clean up UDS file if used.
    if (!g_uds_path.empty()) {
        ::unlink(g_uds_path.c_str());
    }

    LOG_INFO("Server stopped");
    return 0;
}
