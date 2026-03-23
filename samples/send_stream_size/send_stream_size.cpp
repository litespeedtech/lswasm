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

// send_stream_size.cpp — Streaming size-based response generator WASM filter.
//
// This sample generates a response of a caller-specified size using the
// lswasm streaming response API.  The response size is extracted from the
// query string of the :path: pseudo-header.
//
// Behaviour:
//   1. onRequestHeaders  — reads the :path: header, extracts the numeric
//      query string (everything after '?') as the desired response size in
//      bytes.  If no valid size is found, returns 400 Bad Request.
//   2. Generates a block of pseudo-random data (up to 32768 bytes) once and
//      reuses the same buffer for every full-sized chunk, only regenerating
//      for the final shorter chunk.  Streams via writeResponseChunk() until
//      the full requested size has been sent.
//   3. Falls back to sendLocalResponse() when the streaming API is
//      unavailable.
//
// Example request:
//   GET /html/test.wasm?1048576 HTTP/1.1
//
//   → extracts size = 1048576 (1 MB), streams 1 MB of random data back.

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "proxy_wasm_intrinsics.h"
#include "lswasm_streaming.h"

// Maximum bytes per chunk written to the streaming API.
static constexpr size_t MAX_CHUNK_SIZE = 32768;

// Printable ASCII characters used for data generation (0x20–0x7E).
// This produces browser-safe text output.
static constexpr char PRINTABLE_MIN = 0x20;  // space
static constexpr char PRINTABLE_RANGE = 95;  // 0x7E - 0x20 + 1

// ---------------------------------------------------------------------------
// Simple xorshift64* PRNG — fast, self-contained, no external dependencies.
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
  // Each byte is mapped to the range 0x20 (' ') through 0x7E ('~'),
  // with a newline inserted every 76 characters for readability.
  void fillPrintable(char *buf, size_t len) {
    size_t col = 0;
    for (size_t i = 0; i < len; ++i) {
      if (col >= 76) {
        buf[i] = '\n';
        col = 0;
      } else {
        // Use the top bits of the PRNG output for better distribution.
        // Advance the PRNG every 8 characters (one uint64_t gives 8 bytes).
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
// Root context — handles VM-level lifecycle events.
// ---------------------------------------------------------------------------
class SendStreamSizeRootContext : public RootContext {
public:
  explicit SendStreamSizeRootContext(uint32_t id, std::string_view root_id)
      : RootContext(id, root_id) {}

  bool onStart(size_t) override;
  bool onConfigure(size_t) override;
};

// ---------------------------------------------------------------------------
// Stream context — handles per-request events.
// ---------------------------------------------------------------------------
class SendStreamSizeContext : public Context {
public:
  explicit SendStreamSizeContext(uint32_t id, RootContext *root)
      : Context(id, root) {}

  FilterHeadersStatus onRequestHeaders(uint32_t headers, bool end_of_stream) override;
  FilterHeadersStatus onResponseHeaders(uint32_t headers, bool end_of_stream) override;
  void onDone() override;
  void onLog() override;
  void onDelete() override;

private:
  // Extract the query-string size from the :path: header.
  // Returns 0 if no valid size is found.
  size_t extractSizeFromPath();

  // Stream the full response of the requested size.
  // Returns true on success.
  bool streamResponse(size_t total_size);

  // Record a terminal streaming failure after headers were committed.
  void markStreamingFailed(const std::string &operation, WasmResult rc);

  bool headers_sent_ = false;
  bool stream_failed_ = false;
  size_t total_sent_ = 0;
};

// Register the context factories with the SDK.
static RegisterContextFactory register_SendStreamSizeContext(
    CONTEXT_FACTORY(SendStreamSizeContext),
    ROOT_FACTORY(SendStreamSizeRootContext));

// ---------------------------------------------------------------------------
// Root context implementation
// ---------------------------------------------------------------------------
bool SendStreamSizeRootContext::onStart(size_t) {
  LOG_INFO("send_stream_size: onStart");
  return true;
}

bool SendStreamSizeRootContext::onConfigure(size_t) {
  LOG_INFO("send_stream_size: onConfigure");
  return true;
}

// ---------------------------------------------------------------------------
// Stream context helpers
// ---------------------------------------------------------------------------
void SendStreamSizeContext::markStreamingFailed(const std::string &operation,
                                                WasmResult rc) {
  LOG_ERROR("send_stream_size: " + operation + " failed (rc=" +
            std::to_string(static_cast<int>(rc)) + ")");
  stream_failed_ = true;
}

size_t SendStreamSizeContext::extractSizeFromPath() {
  // Get the :path: pseudo-header.
  auto path_pair = getRequestHeader(":path");
  if (!path_pair || path_pair->view().empty()) {
    LOG_ERROR("send_stream_size: missing :path: header");
    return 0;
  }

  std::string_view path = path_pair->view();
  LOG_INFO("send_stream_size: :path: = " + std::string(path));

  // Find the query string delimiter.
  auto qpos = path.find('?');
  if (qpos == std::string_view::npos || qpos + 1 >= path.size()) {
    LOG_ERROR("send_stream_size: no query string in :path:");
    return 0;
  }

  std::string_view size_str = path.substr(qpos + 1);

  // Parse the numeric size.  Stop at '&' or '#' if present.
  auto end_pos = size_str.find_first_of("&#");
  if (end_pos != std::string_view::npos) {
    size_str = size_str.substr(0, end_pos);
  }

  if (size_str.empty()) {
    LOG_ERROR("send_stream_size: empty size in query string");
    return 0;
  }

  // Manual integer parse (no std::stoul in some WASI environments).
  size_t result = 0;
  for (char c : size_str) {
    if (c < '0' || c > '9') {
      LOG_ERROR("send_stream_size: non-numeric character in size: '" +
                std::string(size_str) + "'");
      return 0;
    }
    size_t prev = result;
    result = result * 10 + static_cast<size_t>(c - '0');
    if (result < prev) {
      // Overflow.
      LOG_ERROR("send_stream_size: size overflow");
      return 0;
    }
  }

  return result;
}

bool SendStreamSizeContext::streamResponse(size_t total_size) {
  // Send HTTP response headers.
  lswasm::streaming::HeaderList response_headers;
  response_headers.emplace_back("Content-Type", "text/plain");
  response_headers.emplace_back("Content-Length", std::to_string(total_size));
  response_headers.emplace_back("X-Wasm-Filter", "send_stream_size/active");
  response_headers.emplace_back("X-Powered-By", "lswasm/proxy-wasm");

  WasmResult rc = lswasm::streaming::sendResponseHeaders(200, response_headers);
  if (rc != WasmResult::Ok) {
    LOG_ERROR("send_stream_size: sendResponseHeaders failed (rc=" +
              std::to_string(static_cast<int>(rc)) + ")");
    return false;
  }
  headers_sent_ = true;

  // Seed the PRNG with the context id for reproducibility per request.
  Xorshift64 rng(static_cast<uint64_t>(id()) ^ 0xDEADBEEFCAFE0001ULL);

  // Allocate one reusable chunk buffer, filled once at full size.
  // Re-fill only when the final chunk is smaller than MAX_CHUNK_SIZE.
  char chunk_buf[MAX_CHUNK_SIZE];
  size_t buf_filled = 0;  // size of valid data currently in chunk_buf
  size_t remaining = total_size;

  while (remaining > 0) {
    size_t chunk_size = remaining < MAX_CHUNK_SIZE ? remaining : MAX_CHUNK_SIZE;

    // Only regenerate the buffer when the required size differs from what
    // is already prepared (i.e. the first iteration and the final short chunk).
    if (chunk_size != buf_filled) {
      rng.fillPrintable(chunk_buf, chunk_size);
      buf_filled = chunk_size;
    }

    rc = lswasm::streaming::writeResponseChunk(chunk_buf, chunk_size);
    if (rc != WasmResult::Ok) {
      markStreamingFailed("writeResponseChunk", rc);
      return false;
    }

    remaining -= chunk_size;
    total_sent_ += chunk_size;
  }

  // Finish the response.
  rc = lswasm::streaming::finishResponse();
  if (rc != WasmResult::Ok) {
    markStreamingFailed("finishResponse", rc);
    return false;
  }

  LOG_INFO("send_stream_size: streamed " + std::to_string(total_sent_) +
           " bytes successfully");
  return true;
}

// ---------------------------------------------------------------------------
// Request handling — header phase
// ---------------------------------------------------------------------------
FilterHeadersStatus SendStreamSizeContext::onRequestHeaders(
    uint32_t, bool end_of_stream) {
  LOG_INFO("send_stream_size: onRequestHeaders (eos=" +
           std::to_string(end_of_stream) + ")");

  // Extract the requested response size from the query string.
  size_t requested_size = extractSizeFromPath();
  if (requested_size == 0) {
    std::string error_body =
        "400 Bad Request\n\n"
        "Usage: GET /any/path?SIZE\n"
        "  SIZE = number of bytes to generate (must be > 0)\n\n"
        "Example: GET /html/test.wasm?1048576\n"
        "  → streams 1 MB of random data\n";
    sendLocalResponse(400, "", error_body, {});
    return FilterHeadersStatus::StopIteration;
  }

  LOG_INFO("send_stream_size: requested_size=" + std::to_string(requested_size));

  // Check whether the host supports the streaming API.
  bool streaming_supported = lswasm::streaming::isSupported();
  LOG_INFO("send_stream_size: streaming API " +
           std::string(streaming_supported ? "supported" : "NOT supported"));

  if (streaming_supported) {
    if (streamResponse(requested_size)) {
      return FilterHeadersStatus::StopIteration;
    }
    // If streaming failed before headers were committed, fall through.
    if (headers_sent_ || stream_failed_) {
      return FilterHeadersStatus::StopIteration;
    }
  }

  // Fallback: generate the entire response in memory via sendLocalResponse().
  // This only works for small sizes; large responses will hit buffer limits.
  LOG_WARN("send_stream_size: using sendLocalResponse fallback for " +
           std::to_string(requested_size) + " bytes — may fail for large sizes");

  Xorshift64 rng(static_cast<uint64_t>(id()) ^ 0xDEADBEEFCAFE0001ULL);
  std::string body;
  body.reserve(requested_size);

  char chunk_buf[MAX_CHUNK_SIZE];
  size_t buf_filled = 0;  // size of valid data currently in chunk_buf
  size_t remaining = requested_size;
  while (remaining > 0) {
    size_t chunk_size = remaining < MAX_CHUNK_SIZE ? remaining : MAX_CHUNK_SIZE;
    if (chunk_size != buf_filled) {
      rng.fillPrintable(chunk_buf, chunk_size);
      buf_filled = chunk_size;
    }
    body.append(chunk_buf, chunk_size);
    remaining -= chunk_size;
  }

  sendLocalResponse(200, "", body,
                    {{"Content-Type", "text/plain"},
                     {"X-Wasm-Filter", "send_stream_size/fallback"},
                     {"X-Powered-By", "lswasm/proxy-wasm"}});
  return FilterHeadersStatus::StopIteration;
}

// ---------------------------------------------------------------------------
// Response handling
// ---------------------------------------------------------------------------
FilterHeadersStatus SendStreamSizeContext::onResponseHeaders(uint32_t, bool) {
  LOG_INFO("send_stream_size: onResponseHeaders");
  if (!headers_sent_) {
    addResponseHeader("X-Wasm-Filter", "send_stream_size/active");
    addResponseHeader("X-Powered-By", "lswasm/proxy-wasm");
  }
  return FilterHeadersStatus::Continue;
}

void SendStreamSizeContext::onDone() {
  LOG_INFO("send_stream_size: onDone");
}

void SendStreamSizeContext::onLog() {
  LOG_INFO("send_stream_size: onLog (total_sent=" +
           std::to_string(total_sent_) + ")");
}

void SendStreamSizeContext::onDelete() {
  LOG_INFO("send_stream_size: onDelete");
}
