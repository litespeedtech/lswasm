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

// send_recv_all.cpp — Example WASM filter using the proxy-wasm-cpp-sdk.
//
// Demonstrates:
//   - RootContext / Context class hierarchy
//   - Request header inspection via getRequestHeaderPairs()
//   - WASI environment variable enumeration via __wasi_environ_*
//   - Streaming request body processing via onRequestBody() + getBufferBytes()
//   - Sending a local response once the full body has been received
//
// Behaviour:
//   1. onRequestHeaders  — builds the env-var and header sections; stores
//      them in the context. Returns Continue so the host keeps delivering
//      body data to the filter.
//   2. onRequestBody     — each time a chunk arrives the chunk bytes are
//      appended to body_accum_.  Processing is done incrementally so that
//      neither the input nor the output buffer fills up.  When end_of_stream
//      is true the complete local response is sent.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <wasi/api.h>

#include "proxy_wasm_intrinsics.h"

// ---------------------------------------------------------------------------
// Root context — handles VM-level lifecycle events.
// ---------------------------------------------------------------------------
class SendRecvAllRootContext : public RootContext {
public:
  explicit SendRecvAllRootContext(uint32_t id, std::string_view root_id)
      : RootContext(id, root_id) {}

  bool onStart(size_t) override;
  bool onConfigure(size_t) override;
};

// ---------------------------------------------------------------------------
// Stream context — handles per-request events.
// ---------------------------------------------------------------------------
class SendRecvAllContext : public Context {
public:
  explicit SendRecvAllContext(uint32_t id, RootContext *root)
      : Context(id, root) {}

  void onCreate() override;
  FilterHeadersStatus onRequestHeaders(uint32_t headers, bool end_of_stream) override;
  FilterDataStatus onRequestBody(size_t body_buffer_length, bool end_of_stream) override;
  FilterHeadersStatus onResponseHeaders(uint32_t headers, bool end_of_stream) override;
  void onDone() override;
  void onLog() override;
  void onDelete() override;

private:
  // Sections built during onRequestHeaders and sent together in onRequestBody.
  std::string header_section_;   // env vars + request headers
  std::string body_accum_;       // accumulated request body bytes
};

// Register the context factories with the SDK.
static RegisterContextFactory register_SendRecvAllContext(
    CONTEXT_FACTORY(SendRecvAllContext),
    ROOT_FACTORY(SendRecvAllRootContext));

// ---------------------------------------------------------------------------
// Root context implementation
// ---------------------------------------------------------------------------
bool SendRecvAllRootContext::onStart(size_t) {
  LOG_INFO("send_recv_all: onStart");
  return true;
}

bool SendRecvAllRootContext::onConfigure(size_t) {
  LOG_INFO("send_recv_all: onConfigure");
  return true;
}

// ---------------------------------------------------------------------------
// Stream context implementation
// ---------------------------------------------------------------------------
void SendRecvAllContext::onCreate() {
  LOG_INFO("send_recv_all: onCreate");
}

// ---------------------------------------------------------------------------
// Environment variable helpers (WASI-specific, no SDK API available)
// ---------------------------------------------------------------------------
namespace {

// Build the "=== Environment Variables ===" section of the response body.
std::string build_environ_section() {
  std::string out;
  out.reserve(4096);

  out += "=== Environment Variables ===\n\n";

  __wasi_size_t env_count   = 0;
  __wasi_size_t env_buf_size = 0;

  if (__wasi_environ_sizes_get(&env_count, &env_buf_size) != 0) {
    out += "Error: failed to retrieve environment variable sizes\n";
    return out;
  }

  out += "Environment variable count: ";
  out += std::to_string(env_count);
  out += "\n\n";

  if (env_count == 0) {
    out += "(no environment variables set)\n";
    return out;
  }

  // Heap-allocate buffers using the actual sizes from the WASI call.
  std::unique_ptr<uint8_t *[]> env_ptrs =
      std::make_unique<uint8_t *[]>(env_count);
  std::unique_ptr<uint8_t[]>   env_buf  =
      std::make_unique<uint8_t[]>(env_buf_size);

  if (__wasi_environ_get(env_ptrs.get(), env_buf.get()) != 0) {
    out += "Error: failed to retrieve environment variables\n";
    return out;
  }

  for (__wasi_size_t i = 0; i < env_count; ++i) {
    const char *entry = reinterpret_cast<const char *>(env_ptrs[i]);
    if (entry) {
      out += "  ";
      out += entry;
      out += "\n";
    }
  }

  return out;
}

// Build the "=== Request Headers ===" section of the response body.
std::string build_headers_section() {
  std::string out;
  out.reserve(4096);

  out += "\n\n=== Request Headers ===\n\n";

  WasmDataPtr result = getRequestHeaderPairs();
  std::vector<std::pair<std::string_view, std::string_view>> pairs =
      result->pairs();

  out += "Header count: ";
  out += std::to_string(pairs.size());
  out += "\n\n";

  for (const std::pair<std::string_view, std::string_view> &p : pairs) {
    out += "  ";
    out += std::string(p.first);
    out += ": ";
    out += std::string(p.second);
    out += "\n";
  }

  return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Request handling — headers phase
// ---------------------------------------------------------------------------
FilterHeadersStatus SendRecvAllContext::onRequestHeaders(uint32_t, bool end_of_stream) {
  LOG_INFO("send_recv_all: onRequestHeaders (eos=" + std::to_string(end_of_stream) + ")");

  // Build and cache the header section so we don't have to call the SDK
  // functions again in onRequestBody (the header map may not be available
  // then on some hosts).
  header_section_  = build_environ_section();
  header_section_ += build_headers_section();

  // Append the "body follows" separator.  The body itself will be appended
  // incrementally in onRequestBody.
  header_section_ += "\n\n=== Request Body ===\n\n";

  if (end_of_stream) {
    // No body expected — send the response immediately.
    std::string body = std::move(header_section_);
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
// Each call receives up to one chunk of body data.  The chunk is fetched from
// the HttpRequestBody buffer via getBufferBytes() and appended to body_accum_.
// When end_of_stream is true the accumulated body is complete and we send the
// local response.
//
// Processing the body in chunks means:
//   - The host is never asked to buffer more than one chunk at a time.
//   - The WASM module itself only holds one chunk in memory at a time (plus
//     the accumulated output); neither the host input buffer nor the host
//     output buffer is overwhelmed.
FilterDataStatus SendRecvAllContext::onRequestBody(size_t body_buffer_length,
                                                   bool end_of_stream) {
  LOG_INFO("send_recv_all: onRequestBody (len=" + std::to_string(body_buffer_length) +
           ", eos=" + std::to_string(end_of_stream) + ")");

  // Fetch the current chunk from the host's HttpRequestBody buffer.
  if (body_buffer_length > 0) {
    WasmDataPtr chunk =
        getBufferBytes(WasmBufferType::HttpRequestBody, 0, body_buffer_length);
    if (chunk && chunk->size() > 0) {
      body_accum_.append(chunk->data(), chunk->size());
    }
  }

  if (!end_of_stream) {
    // More data is coming — keep buffering.
    return FilterDataStatus::StopIterationAndBuffer;
  }

  // End of stream — compose and send the complete local response.
  std::string response_body;
  response_body.reserve(header_section_.size() + body_accum_.size() + 2);
  response_body  = std::move(header_section_);
  if (body_accum_.empty()) {
    response_body += "(empty body)\n";
  } else {
    response_body += std::move(body_accum_);
    // Ensure the body section ends with a newline for readability.
    if (response_body.back() != '\n') {
      response_body += '\n';
    }
  }

  sendLocalResponse(200, "", response_body, {});
  return FilterDataStatus::StopIterationNoBuffer;
}

// ---------------------------------------------------------------------------
// Response handling
// ---------------------------------------------------------------------------
FilterHeadersStatus SendRecvAllContext::onResponseHeaders(uint32_t, bool) {
  LOG_INFO("send_recv_all: onResponseHeaders — adding custom headers");

  // Demonstrate response header manipulation.
  addResponseHeader("X-Wasm-Filter", "send_recv_all/active");
  addResponseHeader("X-Powered-By", "lswasm/proxy-wasm");

  return FilterHeadersStatus::Continue;
}

void SendRecvAllContext::onDone() {
  LOG_INFO("send_recv_all: onDone");
}

void SendRecvAllContext::onLog() {}

void SendRecvAllContext::onDelete() {}
