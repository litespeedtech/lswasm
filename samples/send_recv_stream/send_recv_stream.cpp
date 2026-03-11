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

// send_recv_stream.cpp — Streaming echo WASM filter using the lswasm
//                        streaming response API.
//
// This sample demonstrates the lswasm_send_response_headers /
// lswasm_write_response_chunk / lswasm_finish_response foreign-function
// API for streaming large responses without buffering the entire body
// in a single sendLocalResponse() call.
//
// Behaviour:
//   1. onRequestHeaders  — probes for streaming API support, builds the
//      env-var and header diagnostic sections, and if there is no body
//      (end_of_stream == true) sends the entire response immediately via
//      streaming (or falls back to sendLocalResponse).
//   2. onRequestBody     — each body chunk is echoed back to the client
//      incrementally via writeResponseChunk().  On the first chunk the
//      headers + diagnostics preamble is written; on end_of_stream the
//      response is finished.
//
// Comparison with send_recv_all:
//   send_recv_all accumulates the entire body into memory and uses a
//   single sendLocalResponse().  This works for small payloads but
//   hits buffer limits with large bodies.
//
//   send_recv_stream writes each chunk to the wire as it arrives, keeping
//   constant memory usage regardless of body size.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <wasi/api.h>

#include "proxy_wasm_intrinsics.h"
#include "lswasm_streaming.h"

// ---------------------------------------------------------------------------
// Root context — handles VM-level lifecycle events.
// ---------------------------------------------------------------------------
class SendRecvStreamRootContext : public RootContext {
public:
  explicit SendRecvStreamRootContext(uint32_t id, std::string_view root_id)
      : RootContext(id, root_id) {}

  bool onStart(size_t) override;
  bool onConfigure(size_t) override;
};

// ---------------------------------------------------------------------------
// Stream context — handles per-request events.
// ---------------------------------------------------------------------------
class SendRecvStreamContext : public Context {
public:
  explicit SendRecvStreamContext(uint32_t id, RootContext *root)
      : Context(id, root) {}

  void onCreate() override;
  FilterHeadersStatus onRequestHeaders(uint32_t headers, bool end_of_stream) override;
  FilterDataStatus onRequestBody(size_t body_buffer_length, bool end_of_stream) override;
  FilterHeadersStatus onResponseHeaders(uint32_t headers, bool end_of_stream) override;
  void onDone() override;
  void onLog() override;
  void onDelete() override;

private:
  // Build the diagnostic preamble (env vars + request headers).
  void buildPreamble();

  // Send response headers + preamble via streaming API.
  // Returns true if successful, false if streaming is not available
  // (caller should fall back to sendLocalResponse).
  bool streamPreamble();

  std::string preamble_;             // diagnostic text (env + headers)
  bool streaming_supported_ = false; // host has the streaming API?
  bool headers_sent_ = false;        // streaming headers already sent?
  size_t total_body_echoed_ = 0;     // bytes of request body echoed
};

// Register the context factories with the SDK.
static RegisterContextFactory register_SendRecvStreamContext(
    CONTEXT_FACTORY(SendRecvStreamContext),
    ROOT_FACTORY(SendRecvStreamRootContext));

// ---------------------------------------------------------------------------
// Root context implementation
// ---------------------------------------------------------------------------
bool SendRecvStreamRootContext::onStart(size_t) {
  LOG_INFO("send_recv_stream: onStart");
  return true;
}

bool SendRecvStreamRootContext::onConfigure(size_t) {
  LOG_INFO("send_recv_stream: onConfigure");
  return true;
}

// ---------------------------------------------------------------------------
// Stream context implementation
// ---------------------------------------------------------------------------
void SendRecvStreamContext::onCreate() {
  LOG_INFO("send_recv_stream: onCreate (context_id=" +
           std::to_string(id()) + ")");
}

// ---------------------------------------------------------------------------
// Build diagnostic preamble — same as send_recv_all.
// ---------------------------------------------------------------------------
void SendRecvStreamContext::buildPreamble() {
  preamble_.clear();

  // ── Environment variables ────────────────────────────────────────
  preamble_ += "=== Environment Variables ===\n";
  __wasi_size_t env_count = 0;
  __wasi_size_t env_buf_size = 0;
  if (__wasi_environ_sizes_get(&env_count, &env_buf_size) == __WASI_ERRNO_SUCCESS &&
      env_count > 0 && env_buf_size > 0) {
    std::unique_ptr<uint8_t *[]> env_ptrs(new uint8_t *[env_count]);
    std::unique_ptr<uint8_t[]> env_buf(new uint8_t[env_buf_size]);
    if (__wasi_environ_get(env_ptrs.get(), env_buf.get()) == __WASI_ERRNO_SUCCESS) {
      for (__wasi_size_t i = 0; i < env_count; ++i) {
        const char *entry = reinterpret_cast<const char *>(env_ptrs[i]);
        if (entry) {
          preamble_ += "  ";
          preamble_ += entry;
          preamble_ += '\n';
        }
      }
    }
  } else {
    preamble_ += "  (none)\n";
  }

  // ── Request headers ──────────────────────────────────────────────
  preamble_ += "\n=== Request Headers ===\n";
  auto hdrs = getRequestHeaderPairs();
  for (const auto &p : hdrs->pairs()) {
    preamble_ += "  " + std::string(p.first) + ": " +
                 std::string(p.second) + "\n";
  }

  preamble_ += "\n=== Request Body ===\n";
}

// ---------------------------------------------------------------------------
// Stream the preamble.
// ---------------------------------------------------------------------------
bool SendRecvStreamContext::streamPreamble() {
  if (headers_sent_) return true;

  // Send HTTP response headers.
  lswasm::streaming::HeaderList response_headers;
  response_headers.emplace_back("Content-Type", "text/plain");
  response_headers.emplace_back("Connection", "close");
  response_headers.emplace_back("X-Wasm-Filter", "send_recv_stream/active");
  response_headers.emplace_back("X-Powered-By", "lswasm/proxy-wasm");

  WasmResult rc = lswasm::streaming::sendResponseHeaders(200, response_headers);
  if (rc != WasmResult::Ok) {
    LOG_ERROR("send_recv_stream: sendResponseHeaders failed (rc=" +
              std::to_string(static_cast<int>(rc)) + ")");
    return false;
  }
  headers_sent_ = true;

  // Write the preamble as the first body chunk.
  if (!preamble_.empty()) {
    rc = lswasm::streaming::writeResponseChunk(preamble_.data(),
                                                preamble_.size());
    if (rc != WasmResult::Ok) {
      LOG_ERROR("send_recv_stream: writeResponseChunk (preamble) failed");
    }
    preamble_.clear();  // free memory
    preamble_.shrink_to_fit();
  }
  return true;
}

// ---------------------------------------------------------------------------
// Request handling — header phase
// ---------------------------------------------------------------------------
FilterHeadersStatus SendRecvStreamContext::onRequestHeaders(
    uint32_t, bool end_of_stream) {
  LOG_INFO("send_recv_stream: onRequestHeaders (eos=" +
           std::to_string(end_of_stream) + ")");

  // Check whether the host supports the streaming API.
  streaming_supported_ = lswasm::streaming::isSupported();
  LOG_INFO("send_recv_stream: streaming API " +
           std::string(streaming_supported_ ? "supported" : "NOT supported"));

  // Build the diagnostic preamble.
  buildPreamble();

  if (end_of_stream) {
    // No body expected — send the entire response now.
    if (streaming_supported_) {
      if (streamPreamble()) {
        lswasm::streaming::writeResponseChunk("(no body)\n", 10);
        lswasm::streaming::finishResponse();
        return FilterHeadersStatus::StopIteration;
      }
      // Fall through to sendLocalResponse on failure.
    }
    // Fallback: traditional single-shot response.
    std::string body = std::move(preamble_);
    body += "(no body)\n";
    sendLocalResponse(200, "", body, {});
    return FilterHeadersStatus::StopIteration;
  }

  // Tell the host to keep delivering body data to this filter.
  return FilterHeadersStatus::Continue;
}

// ---------------------------------------------------------------------------
// Request handling — body phase (streaming)
// ---------------------------------------------------------------------------
FilterDataStatus SendRecvStreamContext::onRequestBody(
    size_t body_buffer_length, bool end_of_stream) {
  LOG_INFO("send_recv_stream: onRequestBody (len=" +
           std::to_string(body_buffer_length) +
           ", eos=" + std::to_string(end_of_stream) + ")");

  if (streaming_supported_) {
    // Ensure headers + preamble have been sent.
    if (!streamPreamble()) {
      // Streaming failed — fall back to single-shot.
      // This is a rare edge case; log and continue.
      LOG_ERROR("send_recv_stream: streaming fallback — "
                "preamble send failed during body phase");
      streaming_supported_ = false;
      // Fall through to accumulation path below.
    }
  }

  // Fetch the current chunk from the host's HttpRequestBody buffer.
  if (body_buffer_length > 0) {
    WasmDataPtr chunk =
        getBufferBytes(WasmBufferType::HttpRequestBody, 0, body_buffer_length);
    if (chunk && chunk->size() > 0) {
      if (streaming_supported_) {
        // Stream the chunk directly to the client.
        WasmResult rc = lswasm::streaming::writeResponseChunk(
            chunk->data(), chunk->size());
        if (rc != WasmResult::Ok) {
          LOG_ERROR("send_recv_stream: writeResponseChunk failed (rc=" +
                    std::to_string(static_cast<int>(rc)) + ")");
        }
        total_body_echoed_ += chunk->size();
      } else {
        // Non-streaming fallback: accumulate.
        preamble_.append(chunk->data(), chunk->size());
      }
    }
  }

  if (!end_of_stream) {
    return FilterDataStatus::StopIterationAndBuffer;
  }

  // End of stream — finish the response.
  if (streaming_supported_) {
    LOG_INFO("send_recv_stream: finishing streaming response, "
             "total_body_echoed=" + std::to_string(total_body_echoed_));
    lswasm::streaming::finishResponse();
  } else {
    // Fallback: send everything via sendLocalResponse.
    if (preamble_.empty()) {
      preamble_ = "(empty body)\n";
    } else if (preamble_.back() != '\n') {
      preamble_ += '\n';
    }
    sendLocalResponse(200, "", preamble_, {});
  }

  return FilterDataStatus::StopIterationNoBuffer;
}

// ---------------------------------------------------------------------------
// Response handling
// ---------------------------------------------------------------------------
FilterHeadersStatus SendRecvStreamContext::onResponseHeaders(uint32_t, bool) {
  LOG_INFO("send_recv_stream: onResponseHeaders");
  // Note: When streaming, the response headers have already been sent
  // via sendResponseHeaders().  The host should skip this callback in
  // that case, but we add custom headers just in case a non-streaming
  // path is taken.
  if (!headers_sent_) {
    addResponseHeader("X-Wasm-Filter", "send_recv_stream/active");
    addResponseHeader("X-Powered-By", "lswasm/proxy-wasm");
  }
  return FilterHeadersStatus::Continue;
}

void SendRecvStreamContext::onDone() {
  LOG_INFO("send_recv_stream: onDone");
}

void SendRecvStreamContext::onLog() {
  LOG_INFO("send_recv_stream: onLog");
}

void SendRecvStreamContext::onDelete() {
  LOG_INFO("send_recv_stream: onDelete");
}
