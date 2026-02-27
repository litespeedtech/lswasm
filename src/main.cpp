#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <stdexcept>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <csignal>

#include "http_filter.h"
#include "wasm_module_manager.h"

// HTTP Server Configuration
const int DEFAULT_PORT = 8080;
const int BACKLOG = 5;
const int BUFFER_SIZE = 4096;
const int MAX_THREADS = 10;

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
        while (g_shutdown == 0) {
            sockaddr_storage client_addr{};
            socklen_t client_addrlen = sizeof(client_addr);

            int client_socket = accept(server_socket_,
                                       reinterpret_cast<sockaddr *>(&client_addr),
                                       &client_addrlen);
            if (client_socket < 0) {
                if (g_shutdown != 0) break;
                std::cerr << "Accept error" << std::endl;
                continue;
            }

            // Handle client in a thread
            std::thread(&HttpServer::handle_client, this, client_socket).detach();
        }
    }

private:
    enum class Mode { TCP, UDS };

    HttpServer() : mode_(Mode::TCP), port_(DEFAULT_PORT), server_socket_(-1) {}

    // ── TCP listener ────────────────────────────────────────────────────

    bool start_tcp() {
        server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_socket_ < 0) {
            std::cerr << "Failed to create TCP socket" << std::endl;
            return false;
        }

        int opt = 1;
        if (setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            std::cerr << "Failed to set socket options" << std::endl;
            close(server_socket_);
            return false;
        }

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        server_addr.sin_port = htons(port_);

        if (bind(server_socket_, reinterpret_cast<sockaddr *>(&server_addr),
                 sizeof(server_addr)) < 0) {
            std::cerr << "Failed to bind TCP socket to port " << port_ << std::endl;
            close(server_socket_);
            return false;
        }

        if (listen(server_socket_, BACKLOG) < 0) {
            std::cerr << "Failed to listen on TCP socket" << std::endl;
            close(server_socket_);
            return false;
        }

        g_server_socket = server_socket_;
        std::cout << "HTTP Server listening on TCP port " << port_ << std::endl;
        return true;
    }

    // ── Unix Domain Socket listener ─────────────────────────────────────

    bool start_uds() {
        server_socket_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_socket_ < 0) {
            std::cerr << "Failed to create Unix domain socket" << std::endl;
            return false;
        }

        // Remove any stale socket file.
        ::unlink(uds_path_.c_str());

        sockaddr_un server_addr{};
        server_addr.sun_family = AF_UNIX;

        if (uds_path_.size() >= sizeof(server_addr.sun_path)) {
            std::cerr << "Unix socket path too long (max "
                      << sizeof(server_addr.sun_path) - 1 << " chars): "
                      << uds_path_ << std::endl;
            close(server_socket_);
            return false;
        }
        std::strncpy(server_addr.sun_path, uds_path_.c_str(),
                      sizeof(server_addr.sun_path) - 1);

        if (bind(server_socket_, reinterpret_cast<sockaddr *>(&server_addr),
                 sizeof(server_addr)) < 0) {
            std::cerr << "Failed to bind Unix domain socket at " << uds_path_
                      << ": " << strerror(errno) << std::endl;
            close(server_socket_);
            return false;
        }

        // Restrict socket access to the owner only (rw-------).
        if (chmod(uds_path_.c_str(), 0600) != 0) {
            std::cerr << "Failed to set permissions on Unix domain socket: "
                      << strerror(errno) << std::endl;
            close(server_socket_);
            cleanup_uds();
            return false;
        }

        if (listen(server_socket_, BACKLOG) < 0) {
            std::cerr << "Failed to listen on Unix domain socket" << std::endl;
            close(server_socket_);
            cleanup_uds();
            return false;
        }

        g_server_socket = server_socket_;
        g_uds_path = uds_path_;
        std::cout << "HTTP Server listening on Unix socket " << uds_path_ << std::endl;
        return true;
    }

    void cleanup_uds() {
        if (!uds_path_.empty()) {
            ::unlink(uds_path_.c_str());
        }
    }

    // ── Request handling (shared by both modes) ─────────────────────────

    void handle_client(int client_socket) {
        char buffer[BUFFER_SIZE];
        std::memset(buffer, 0, sizeof(buffer));

        // Read HTTP request
        ssize_t bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes_read <= 0) {
            close(client_socket);
            return;
        }

        buffer[bytes_read] = '\0';

        // Parse HTTP request
        std::string request(buffer);
        HttpData http_data;
        
        if (!parse_request(request, http_data)) {
            close(client_socket);
            return;
        }

        // create a filter context for this request
        uint32_t ctx_id = g_next_context_id.fetch_add(1);
        HttpFilterContext filter_ctx(ctx_id, &http_data);
        filter_ctx.onCreate();

        // Execute phases via context methods
        std::cout << "\n[HTTP] Processing request in filter chain..." << std::endl;
        filter_ctx.onRequestHeaders();

        // If the WASM filter sent a local response, use it directly.
        if (http_data.has_local_response) {
            std::cout << "[HTTP] WASM filter sent local response, using it." << std::endl;
            auto response = build_local_response(http_data);
            send(client_socket, response.c_str(), response.length(), 0);
            close(client_socket);
            return;
        }

        filter_ctx.onRequestBody();
        filter_ctx.onRequestTrailers();
        filter_ctx.onDone();

        // Generate HTTP response
        auto response = process_request(http_data);

        // Execute response phases via context
        std::cout << "[HTTP] Processing response in filter chain..." << std::endl;
        filter_ctx.onResponseHeaders();
        filter_ctx.onResponseBody();
        filter_ctx.onResponseTrailers();
        filter_ctx.onDone();

        // Send HTTP response
        send(client_socket, response.c_str(), response.length(), 0);
        close(client_socket);
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
                http_data.request_headers[header_name] = header_value;
            }
        }

        return true;
    }

    // Build an HTTP response from the WASM filter's local response.
    std::string build_local_response(const HttpData& http_data) {
        std::ostringstream response;
        response << "HTTP/1.1 " << http_data.local_response_code << " OK\r\n";
        response << "Content-Type: text/plain\r\n";
        response << "X-Powered-By: lswasm/proxy-wasm\r\n";
        response << "Connection: close\r\n";
        response << "Content-Length: " << http_data.local_response_body.length() << "\r\n";
        response << "\r\n";
        response << http_data.local_response_body;
        return response.str();
    }

    std::string process_request(const HttpData& http_data) {
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n";
        response << "Content-Type: text/plain\r\n";
        response << "Connection: close\r\n";

        std::string body = "=== WASM HTTP Proxy Server ===\n\n";
        body += "Request Information:\n";
        body += "  Method: " + http_data.method + "\n";
        body += "  Path: " + http_data.path + "\n";
        body += "  Version: " + http_data.version + "\n\n";

        body += "Runtime Information:\n";
#ifdef ENABLE_WASMTIME
        body += "  ✓ Wasmtime runtime available\n";
#endif
#ifdef ENABLE_WASMER
        body += "  ✓ Wasmer runtime available\n";
#endif
#if !defined(ENABLE_WASMTIME) && !defined(ENABLE_WASMER)
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
        body += "  • Connection events\n";
        body += "  • Metadata and data processing\n";
        body += "  • Status/error codes\n\n";

        body += "Submodules:\n";
        body += "  • proxy-wasm-cpp-host\n";
        body += "  • proxy-wasm-cpp-sdk\n";
        body += "  • proxy-wasm-spec\n";

        response << "Content-Length: " << body.length() << "\r\n";
        response << "\r\n";
        response << body;

        return response.str();
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
                std::cerr << "Invalid --env format, expected KEY=VALUE: " << env_str << std::endl;
                return 1;
            }
        } else if (arg == "--help") {
            std::cout << "WASM HTTP Proxy Server with Proxy-WASM Support\n";
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  --port PORT      : Listen on TCP port (default: " << DEFAULT_PORT << ")\n";
            std::cout << "  --uds PATH       : Listen on Unix domain socket at PATH\n";
            std::cout << "  --module PATH    : Load WASM filter module\n";
            std::cout << "  --env KEY=VALUE  : Set environment variable for WASM module (repeatable)\n";
            std::cout << "  --help           : Show this help message\n";
            std::cout << "\nWhen both --port and --uds are given, only --uds is used.\n";
            return 0;
        }
    }

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize WASM module manager
    g_module_manager = std::make_unique<WasmModuleManager>();

    // Print runtime information
    std::cout << "\n=== WASM HTTP Proxy Server ===" << std::endl;
#ifdef ENABLE_WASMTIME
    std::cout << "✓ Wasmtime runtime enabled" << std::endl;
#endif
#ifdef ENABLE_WASMER
    std::cout << "✓ Wasmer runtime enabled" << std::endl;
#endif
#if !defined(ENABLE_WASMTIME) && !defined(ENABLE_WASMER)
    std::cout << "ℹ No WASM runtime enabled (using Null VM)" << std::endl;
#endif
    std::cout << "Submodules:" << std::endl;
    std::cout << "  • proxy-wasm-cpp-host" << std::endl;
    std::cout << "  • proxy-wasm-cpp-sdk" << std::endl;
    std::cout << "  • proxy-wasm-spec" << std::endl;
    std::cout << "==============================\n" << std::endl;

    // Set environment variables for WASM modules
    if (!wasm_envs.empty()) {
        std::cout << "WASM environment variables:" << std::endl;
        for (const auto &[key, value] : wasm_envs) {
            std::cout << "  " << key << "=" << value << std::endl;
        }
        g_module_manager->setEnvironmentVariables(wasm_envs);
    }

    // Load WASM module if provided
    if (!wasm_module_path.empty()) {
        std::string module_name = "custom_filter";
        std::cout << "Loading WASM filter module: " << wasm_module_path << std::endl;
        if (g_module_manager->loadModule(wasm_module_path, module_name)) {
            std::cout << "✓ Filter module loaded successfully" << std::endl;
        } else {
            std::cerr << "✗ Failed to load filter module" << std::endl;
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
            std::cerr << "Failed to start HTTP server" << std::endl;
            return 1;
        }

        std::cout << "Server ready. Press Ctrl+C to stop.\n" << std::endl;

        // Accept incoming connections
        server->accept_connections();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    // Clean up UDS file if used.
    if (!g_uds_path.empty()) {
        ::unlink(g_uds_path.c_str());
    }

    std::cout << "Server stopped" << std::endl;
    return 0;
}
