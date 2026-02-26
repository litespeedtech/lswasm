#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <strings.h>

static void handle_client(int client_fd, const std::string &runtime, const std::string &module_path) {
  std::string req;
  char buf[4096];
  ssize_t n;
  // Read request headers (very small/simple HTTP parse)
  while ((n = recv(client_fd, buf, sizeof(buf), 0)) > 0) {
    req.append(buf, n);
    if (req.find("\r\n\r\n") != std::string::npos) break;
  }

  if (req.empty()) { close(client_fd); return; }

  std::istringstream ss(req);
  std::string line;
  size_t content_len = 0;
  std::string request_line;
  if (std::getline(ss, request_line)) {
    if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();
  }
  while (std::getline(ss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;
    auto pos = line.find(':');
    if (pos != std::string::npos) {
      std::string key = line.substr(0, pos);
      std::string val = line.substr(pos+1);
      // trim
      while (!val.empty() && (val.front()==' '||val.front()=='\t')) val.erase(val.begin());
      if (strcasecmp(key.c_str(), "Content-Length") == 0) {
        content_len = std::stoul(val);
      }
    }
  }

  std::string body;
  if (content_len) {
    size_t header_end = req.find("\r\n\r\n");
    if (header_end != std::string::npos) {
      size_t have = req.size() - (header_end + 4);
      if (have >= content_len) body = req.substr(header_end + 4, content_len);
      else {
        body = req.substr(header_end + 4);
        size_t toread = content_len - body.size();
        while (toread > 0 && (n = recv(client_fd, buf, sizeof(buf), 0)) > 0) {
          body.append(buf, n);
          toread = content_len - body.size();
        }
      }
    }
  }

  int inpipe[2];
  int outpipe[2];
  if (pipe(inpipe) != 0 || pipe(outpipe) != 0) {
    close(client_fd); return;
  }

  pid_t pid = fork();
  if (pid == 0) {
    // child
    dup2(inpipe[0], STDIN_FILENO);
    dup2(outpipe[1], STDOUT_FILENO);
    // close unused
    close(inpipe[1]); close(outpipe[0]);
    // execute runtime: try `wasmtime run <module>` or `wasmer run <module>`
    execlp(runtime.c_str(), runtime.c_str(), "run", module_path.c_str(), (char*)NULL);
    // fallback: runtime <module>
    execlp(runtime.c_str(), runtime.c_str(), module_path.c_str(), (char*)NULL);
    perror("execlp");
    _exit(127);
  }

  // parent
  close(inpipe[0]); close(outpipe[1]);
  if (!body.empty()) {
    ssize_t w = write(inpipe[1], body.data(), body.size());
    (void)w;
  }
  close(inpipe[1]);

  std::string wasm_out;
  while ((n = read(outpipe[0], buf, sizeof(buf))) > 0) wasm_out.append(buf, n);
  close(outpipe[0]);

  waitpid(pid, nullptr, 0);

  // If wasm output looks like HTTP response (starts with HTTP/), send as-is
  if (wasm_out.rfind("HTTP/", 0) == 0) {
    send(client_fd, wasm_out.data(), wasm_out.size(), 0);
  } else {
    std::ostringstream resp;
    resp << "HTTP/1.1 200 OK\r\n";
    resp << "Content-Type: text/plain; charset=utf-8\r\n";
    resp << "Content-Length: " << wasm_out.size() << "\r\n";
    resp << "Connection: close\r\n\r\n";
    resp << wasm_out;
    std::string outstr = resp.str();
    send(client_fd, outstr.data(), outstr.size(), 0);
  }

  close(client_fd);
}

int main(int argc, char **argv) {
  int port = 8080;
  std::string module = "../samples/handler.wasm";
  std::string runtime = "wasmtime";

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--port" && i+1 < argc) port = std::stoi(argv[++i]);
    else if (a == "--module" && i+1 < argc) module = argv[++i];
    else if (a == "--runtime" && i+1 < argc) runtime = argv[++i];
    else if (a == "-h" || a == "--help") {
      std::cout << "Usage: " << argv[0] << " [--port N] [--module path] [--runtime wasmtime|wasmer]\n";
      return 0;
    }
  }

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) { perror("socket"); return 1; }

  int opt = 1; setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
  if (listen(server_fd, 16) < 0) { perror("listen"); return 1; }

  std::cout << "wasm-proxy listening on port " << port << " using runtime='" << runtime << "' module='" << module << "'\n";

  while (true) {
    int client = accept(server_fd, nullptr, nullptr);
    if (client < 0) { if (errno == EINTR) continue; perror("accept"); break; }
    pid_t pid = fork();
    if (pid == 0) {
      close(server_fd);
      handle_client(client, runtime, module);
      _exit(0);
    } else if (pid > 0) {
      close(client);
      // reap zombies
      while (waitpid(-1, nullptr, WNOHANG) > 0) {}
    }
  }

  close(server_fd);
  return 0;
}
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

static void handle_client(int client_fd, const std::string &runtime, const std::string &module_path) {
  std::string req;
  char buf[4096];
  ssize_t n;
  // Read request (very small/simple HTTP parse)
  while ((n = recv(client_fd, buf, sizeof(buf), 0)) > 0) {
    req.append(buf, n);
    if (req.find("\r\n\r\n") != std::string::npos) break;
  }

  if (req.empty()) { close(client_fd); return; }

  // parse Content-Length if present
  std::istringstream ss(req);
  std::string line;
  size_t content_len = 0;
  std::vector<std::string> headers;
  std::string request_line;
  if (std::getline(ss, request_line)) request_line.pop_back();
  while (std::getline(ss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;
    headers.push_back(line);
    std::string key = line.substr(0, line.find(':'));
    if (strcasecmp(key.c_str(), "Content-Length") == 0) {
      size_t pos = line.find(':');
      if (pos != std::string::npos) content_len = std::stoul(line.substr(pos+1));
    }
  }

  std::string body;
  if (content_len) {
    // attempt to get remaining body already read
    size_t header_end = req.find("\r\n\r\n");
    if (header_end != std::string::npos) {
      size_t have = req.size() - (header_end + 4);
      if (have >= content_len) body = req.substr(header_end + 4, content_len);
      else {
        body = req.substr(header_end + 4);
        size_t toread = content_len - body.size();
        while (toread > 0 && (n = recv(client_fd, buf, sizeof(buf), 0)) > 0) {
          body.append(buf, n);
          toread = content_len - body.size();
        }
      }
    }
  }

  // Run wasm module as subprocess using chosen runtime (wasmtime/wasmer)
  int inpipe[2];
  int outpipe[2];
  if (pipe(inpipe) != 0 || pipe(outpipe) != 0) {
    close(client_fd); return;
  }

  pid_t pid = fork();
  if (pid == 0) {
    // child
    dup2(inpipe[0], STDIN_FILENO);
    dup2(outpipe[1], STDOUT_FILENO);
    close(inpipe[1]); close(outpipe[0]);
    // exec: runtime run <module>
    execlp(runtime.c_str(), runtime.c_str(), "run", module_path.c_str(), (char*)NULL);
    // fallback: runtime <module>
    execlp(runtime.c_str(), runtime.c_str(), module_path.c_str(), (char*)NULL);
    // if exec fails
    perror("execlp");
    _exit(127);
  }

  // parent
  close(inpipe[0]); close(outpipe[1]);
  // write body to child's stdin
  if (!body.empty()) write(inpipe[1], body.data(), body.size());
  close(inpipe[1]);

  // read child's stdout fully
  std::string wasm_out;
  while ((n = read(outpipe[0], buf, sizeof(buf))) > 0) wasm_out.append(buf, n);
  close(outpipe[0]);

  int status = 0; waitpid(pid, &status, 0);

  // If wasm output looks like HTTP response (starts with HTTP/), send as-is
  if (wasm_out.rfind("HTTP/", 0) == 0) {
    send(client_fd, wasm_out.data(), wasm_out.size(), 0);
  } else {
    std::ostringstream resp;
    resp << "HTTP/1.1 200 OK\r\n";
    resp << "Content-Type: text/plain; charset=utf-8\r\n";
    resp << "Content-Length: " << wasm_out.size() << "\r\n";
    resp << "Connection: close\r\n\r\n";
    resp << wasm_out;
    std::string outstr = resp.str();
    send(client_fd, outstr.data(), outstr.size(), 0);
  }

  close(client_fd);
}

int main(int argc, char **argv) {
  int port = 8080;
  std::string module = "handler.wasm";
  std::string runtime = "wasmtime";

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--port" && i+1 < argc) port = std::stoi(argv[++i]);
    else if (a == "--module" && i+1 < argc) module = argv[++i];
    else if (a == "--runtime" && i+1 < argc) runtime = argv[++i];
    else if (a == "-h" || a == "--help") {
      std::cout << "Usage: " << argv[0] << " [--port N] [--module path] [--runtime wasmtime|wasmer]\n";
      return 0;
    }
  }

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) { perror("socket"); return 1; }

  int opt = 1; setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
  if (listen(server_fd, 16) < 0) { perror("listen"); return 1; }

  std::cout << "wasm-proxy listening on port " << port << " using runtime='" << runtime << "' module='" << module << "'\n";

  while (true) {
    int client = accept(server_fd, nullptr, nullptr);
    if (client < 0) { if (errno == EINTR) continue; perror("accept"); break; }
    pid_t pid = fork();
    if (pid == 0) {
      close(server_fd);
      handle_client(client, runtime, module);
      _exit(0);
    } else if (pid > 0) {
      close(client);
      // reap zombies
      while (waitpid(-1, nullptr, WNOHANG) > 0) {}
    }
  }

  close(server_fd);
  return 0;
}
