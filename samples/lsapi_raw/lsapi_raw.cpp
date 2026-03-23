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

// lsapi_raw.cpp — Raw LSAPI prefork streaming application for benchmarking.
//
// This is a native C++ application (NOT a WASM filter) that speaks the
// LSAPI protocol directly, using the LSAPI prefork model (fork-based
// worker processes).
//
// Two modes of operation:
//
//   A. Size-based generation (query string present):
//      If the request URL contains a query string that is a positive integer,
//      the application generates that many bytes of pseudo-random printable
//      ASCII data and streams it back.  This mirrors the behaviour of the
//      send_stream_size WASM filter, providing a direct performance baseline.
//
//   B. Echo mode (no query string):
//      Replicates the same behaviour as the send_recv_stream WASM filter:
//        1. Dumps environment variables.
//        2. Dumps request headers.
//        3. Streams back the request body chunk-by-chunk.
//
// The purpose is to provide a performance baseline that isolates LSAPI
// transport overhead from the WASM runtime overhead, making it possible
// to measure the cost of the WASM layer independently.
//
// The prefork model matches LiteSpeed's typical LSAPI deployment where
// a parent process listens and forks child workers to handle requests
// concurrently — the same model used by PHP-LSAPI.
//
// Usage:
//   # Inherited socket from LiteSpeed (typical LSAPI deployment):
//   # Set LSAPI_CHILDREN=20 in environment (or via LiteSpeed external app config)
//   LSAPI_CHILDREN=20 ./lsapi_raw
//
//   # Explicit bind address with fork workers:
//   ./lsapi_raw --bind 127.0.0.1:8000 --children 20
//   ./lsapi_raw --bind /tmp/lsapi_raw.sock --children 20
//
//   # Size-based benchmark (generates 1 MB of data):
//   #   GET /path/to/lsapi_raw?1048576 HTTP/1.1

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "lsapilib.h"
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr int BACKLOG = 100;
static constexpr size_t READ_BUF_SIZE = 16384;

// Maximum bytes per chunk written for size-based generation.
// LSAPI internally frames data into 16 KB packets (LSAPI_MAX_DATA_PACKET_LEN)
// and batches up to 4 packets per writev() syscall.  A 64 KB chunk perfectly
// fills one batch, minimising system call overhead.
static constexpr size_t MAX_CHUNK_SIZE = 65536;

// Printable ASCII characters used for data generation (0x20–0x7E).
static constexpr char PRINTABLE_MIN = 0x20;  // space
static constexpr char PRINTABLE_RANGE = 95;  // 0x7E - 0x20 + 1

// ---------------------------------------------------------------------------
// Simple xorshift64* PRNG — same as send_stream_size for comparability.
// ---------------------------------------------------------------------------
class Xorshift64 {
public:
  explicit Xorshift64(uint64_t seed) : state_(seed ? seed : 0x12345678ABCDEF01ULL) {}

  uint64_t next() {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 0x2545F4914F6CDD1DULL;
  }

  // Fill a buffer with printable ASCII characters.
  void fillPrintable(char *buf, size_t len) {
    size_t col = 0;
    for (size_t i = 0; i < len; ++i) {
      if (col >= 76) {
        buf[i] = '\n';
        col = 0;
      } else {
        if ((i & 7) == 0) {
          pending_ = next();
        }
        uint8_t byte = static_cast<uint8_t>(pending_ & 0xFF);
        pending_ >>= 8;
        buf[i] = static_cast<char>(PRINTABLE_MIN + (byte % PRINTABLE_RANGE));
        ++col;
      }
    }
  }

private:
  uint64_t state_;
  uint64_t pending_ = 0;
};

// ---------------------------------------------------------------------------
// Parse a null-terminated string as a positive size_t.
// Returns 0 on failure (empty, non-numeric, overflow).
// ---------------------------------------------------------------------------
static size_t parse_size(const char *str) {
  if (!str || !*str) return 0;
  size_t result = 0;
  for (const char *p = str; *p; ++p) {
    // Stop at '&' or '#' (additional query parameters).
    if (*p == '&' || *p == '#') break;
    if (*p < '0' || *p > '9') return 0;
    size_t prev = result;
    result = result * 10 + static_cast<size_t>(*p - '0');
    if (result < prev) return 0;  // overflow
  }
  return result;
}

// ---------------------------------------------------------------------------
// LSAPI callback: emit one environment variable line.
// ---------------------------------------------------------------------------
static int env_handler(const char *pKey, int keyLen,
                       const char *pValue, int valLen, void * /* arg */) {
    // Format: "  KEY=VALUE\r\n"
    LSAPI_Write("  ", 2);
    LSAPI_Write(pKey, keyLen);
    LSAPI_Write("=", 1);
    LSAPI_Write(pValue, valLen);
    LSAPI_Write("\r\n", 2);
    return 1;  // >0 = continue
}

// ---------------------------------------------------------------------------
// LSAPI callback: emit one header line.
// ---------------------------------------------------------------------------
static int header_handler(const char *pKey, int keyLen,
                          const char *pValue, int valLen, void * /* arg */) {
    // Format: "  Key: Value\r\n"
    LSAPI_Write("  ", 2);
    LSAPI_Write(pKey, keyLen);
    LSAPI_Write(": ", 2);
    LSAPI_Write(pValue, valLen);
    LSAPI_Write("\r\n", 2);
    return 1;
}

// ---------------------------------------------------------------------------
// Handle size-based generation: stream `total_size` bytes of printable data.
// ---------------------------------------------------------------------------
static void handle_size_request(size_t total_size) {
    // ── Response headers ────────────────────────────────────────────
    LSAPI_SetRespStatus(200);
    LSAPI_AppendRespHeader(
        const_cast<char *>("Content-Type: text/plain"),
        static_cast<int>(strlen("Content-Type: text/plain")));

    // Content-Length header.
    char cl_hdr[64];
    snprintf(cl_hdr, sizeof(cl_hdr), "Content-Length: %zu", total_size);
    LSAPI_AppendRespHeader(const_cast<char *>(cl_hdr),
                           static_cast<int>(strlen(cl_hdr)));

    LSAPI_AppendRespHeader(
        const_cast<char *>("X-Powered-By: lsapi_raw/size"),
        static_cast<int>(strlen("X-Powered-By: lsapi_raw/size")));
    LSAPI_FinalizeRespHeaders();

    // Static buffer persists across requests in the same child process.
    // Filled once at full size on first use; subsequent requests reuse it.
    static char chunk_buf[MAX_CHUNK_SIZE];
    static size_t buf_filled = 0;

    if (buf_filled != MAX_CHUNK_SIZE) {
        Xorshift64 rng(static_cast<uint64_t>(getpid()) ^ 0xDEADBEEFCAFE0001ULL);
        rng.fillPrintable(chunk_buf, MAX_CHUNK_SIZE);
        buf_filled = MAX_CHUNK_SIZE;
    }

    size_t remaining = total_size;

    while (remaining > 0) {
        size_t chunk_size = remaining < MAX_CHUNK_SIZE ? remaining : MAX_CHUNK_SIZE;
        LSAPI_Write(chunk_buf, static_cast<ssize_t>(chunk_size));
        remaining -= chunk_size;
    }

    LSAPI_Flush();
}

// ---------------------------------------------------------------------------
// Handle echo request: dump env, headers, and echo body.
// ---------------------------------------------------------------------------
static void handle_echo_request() {
    // ── Response headers ────────────────────────────────────────────
    LSAPI_SetRespStatus(200);
    LSAPI_AppendRespHeader(
        const_cast<char *>("Content-Type: text/plain"),
        static_cast<int>(strlen("Content-Type: text/plain")));
    LSAPI_AppendRespHeader(
        const_cast<char *>("X-Powered-By: lsapi_raw/echo"),
        static_cast<int>(strlen("X-Powered-By: lsapi_raw/echo")));
    LSAPI_FinalizeRespHeaders();

    // ── Environment variables ───────────────────────────────────────
    LSAPI_Write("=== Environment Variables ===\r\n",
                static_cast<int>(strlen("=== Environment Variables ===\r\n")));
    int count = LSAPI_ForeachEnv(env_handler, nullptr);
    if (count == 0) {
        LSAPI_Write("  (none)\r\n", static_cast<int>(strlen("  (none)\r\n")));
    }
    // Also dump special env vars (SERVER_SOFTWARE, etc.)
    LSAPI_ForeachSpecialEnv(env_handler, nullptr);

    // ── Request headers ─────────────────────────────────────────────
    LSAPI_Write("\r\n=== Request Headers ===\r\n",
                static_cast<int>(strlen("\r\n=== Request Headers ===\r\n")));
    LSAPI_ForeachOrgHeader(header_handler, nullptr);

    // ── Request body (streamed) ─────────────────────────────────────
    LSAPI_Write("\r\n=== Request Body ===\r\n",
                static_cast<int>(strlen("\r\n=== Request Body ===\r\n")));

    off_t body_len = LSAPI_GetReqBodyLen();
    if (body_len <= 0) {
        LSAPI_Write("(no body)\r\n",
                     static_cast<int>(strlen("(no body)\r\n")));
    } else {
        char buf[READ_BUF_SIZE];
        size_t total_echoed = 0;
        for (;;) {
            ssize_t n = LSAPI_ReadReqBody(buf, sizeof(buf));
            if (n <= 0) break;
            LSAPI_Write(buf, n);
            total_echoed += static_cast<size_t>(n);
        }
        // Append a trailing newline if the body didn't end with one.
        if (total_echoed > 0 && buf[(total_echoed < sizeof(buf) ? total_echoed : sizeof(buf)) - 1] != '\n') {
            LSAPI_Write("\r\n", 2);
        }
    }

    LSAPI_Flush();
}

// ---------------------------------------------------------------------------
// Handle a single LSAPI request — dispatch based on query string.
// ---------------------------------------------------------------------------
static void handle_request() {
    const char *qs = LSAPI_GetQueryString();
    size_t requested_size = parse_size(qs);

    if (requested_size > 0) {
        handle_size_request(requested_size);
    } else {
        handle_echo_request();
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    std::string bind_addr;
    int children = 0;  // 0 = use LSAPI_CHILDREN env var (or single-process)

    // Minimal argument parsing.
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--bind" || arg == "-b") && i + 1 < argc) {
            bind_addr = argv[++i];
        } else if ((arg == "--children" || arg == "-c") && i + 1 < argc) {
            children = atoi(argv[++i]);
            if (children < 0) children = 0;
        } else if (arg == "--help" || arg == "-h") {
            fprintf(stdout,
                "lsapi_raw — Raw LSAPI prefork streaming echo (benchmark baseline)\n"
                "Usage: %s [options]\n"
                "Options:\n"
                "  --bind ADDR      Bind LSAPI to address (e.g. 127.0.0.1:8000 or /tmp/lsapi.sock)\n"
                "  --children N     Number of prefork worker processes (default: LSAPI_CHILDREN env)\n"
                "  --help           Show this help message\n"
                "\nBy default the listening socket is inherited from the parent process (LiteSpeed).\n"
                "Set LSAPI_CHILDREN in the environment or use --children to enable prefork mode.\n",
                argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    // If --children was specified on the command line, set the environment
    // variable so LSAPI_Init_Env_Parameters() picks it up automatically.
    if (children > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", children);
        setenv("LSAPI_CHILDREN", buf, 1);
    }

    // If a bind address was provided, create a listener socket and move it
    // to fd 0 so that LSAPI_Init() picks it up.
    if (!bind_addr.empty()) {
        int fd = LSAPI_CreateListenSock(bind_addr.c_str(), BACKLOG);
        if (fd == -1) {
            fprintf(stderr, "LSAPI_CreateListenSock(\"%s\") failed: %s\n",
                    bind_addr.c_str(), strerror(errno));
            return 1;
        }
        // For Unix domain sockets, ensure the socket file is accessible
        // by the web server process regardless of the current umask.
        if (bind_addr[0] == '/') {
            chmod(bind_addr.c_str(), 0666);
        }
        if (fd != 0) {
            if (dup2(fd, 0) < 0) {
                int err = errno;
                close(fd);
                fprintf(stderr, "dup2(%d, 0) failed: %s\n", fd, strerror(err));
                return 1;
            }
            close(fd);
        }
    }

    if (LSAPI_Init() < 0) {
        fprintf(stderr, "LSAPI_Init() failed\n");
        return 1;
    }

    // LSAPI_Init_Env_Parameters reads LSAPI_CHILDREN from the environment.
    // When LSAPI_CHILDREN > 1 and a listener socket is available, it calls
    // LSAPI_Init_Prefork_Server() internally, which sets up the fork-based
    // worker pool.  The parent becomes a process manager and child workers
    // each run the accept loop below.
    LSAPI_Init_Env_Parameters(nullptr);

    // Prefork accept loop.  LSAPI_Prefork_Accept_r() handles:
    //   - In the parent: forking child workers on demand.
    //   - In each child: accepting a connection and returning 0 (success).
    //   - Returns -1 when the child should exit (max requests reached,
    //     idle timeout, shutdown signal, etc.)
    while (LSAPI_Prefork_Accept_r(&g_req) >= 0) {
        handle_request();
        LSAPI_Finish();
    }

    return 0;
}
