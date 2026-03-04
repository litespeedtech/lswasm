// sample_filter.cpp — Example WASM filter using the proxy-wasm-cpp-sdk.
//
// Demonstrates:
//   - RootContext / Context class hierarchy
//   - Request header inspection via getRequestHeaderPairs()
//   - Sending a local response via sendLocalResponse()
//   - Response header manipulation via addResponseHeader()
//   - WASI environment variable access

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <wasi/api.h>

#include "proxy_wasm_intrinsics.h"

// ---------------------------------------------------------------------------
// Root context — handles VM-level lifecycle events.
// ---------------------------------------------------------------------------
class SampleRootContext : public RootContext {
public:
  explicit SampleRootContext(uint32_t id, std::string_view root_id)
      : RootContext(id, root_id) {}

  bool onStart(size_t) override;
  bool onConfigure(size_t) override;
};

// ---------------------------------------------------------------------------
// Stream context — handles per-request events.
// ---------------------------------------------------------------------------
class SampleContext : public Context {
public:
  explicit SampleContext(uint32_t id, RootContext *root) : Context(id, root) {}

  void onCreate() override;
  FilterHeadersStatus onRequestHeaders(uint32_t headers, bool end_of_stream) override;
  FilterHeadersStatus onResponseHeaders(uint32_t headers, bool end_of_stream) override;
  FilterDataStatus onRequestBody(size_t body_buffer_length, bool end_of_stream) override;
  void onDone() override;
  void onLog() override;
  void onDelete() override;
};

// Register the context factories with the SDK.
static RegisterContextFactory register_SampleContext(CONTEXT_FACTORY(SampleContext),
                                                     ROOT_FACTORY(SampleRootContext));

// ---------------------------------------------------------------------------
// Root context implementation
// ---------------------------------------------------------------------------
bool SampleRootContext::onStart(size_t) {
  LOG_INFO("sample_filter: onStart");
  return true;
}

bool SampleRootContext::onConfigure(size_t) {
  LOG_INFO("sample_filter: onConfigure");
  return true;
}

// ---------------------------------------------------------------------------
// Stream context implementation
// ---------------------------------------------------------------------------
void SampleContext::onCreate() {
  LOG_INFO("sample_filter: onCreate");
}

// ---------------------------------------------------------------------------
// Environment variable helpers (WASI-specific, no SDK API available)
// ---------------------------------------------------------------------------
namespace {

std::string build_environ_body() {
  std::string body;
  body.reserve(4096);

  body += "=== Environment Variables ===\n\n";

  __wasi_size_t env_count = 0;
  __wasi_size_t env_buf_size = 0;

  if (__wasi_environ_sizes_get(&env_count, &env_buf_size) != 0) {
    body += "Error: failed to retrieve environment variable sizes\n";
    return body;
  }

  body += "Environment variable count: " + std::to_string(env_count) + "\n\n";

  if (env_count == 0) {
    body += "(no environment variables set)\n";
    return body;
  }

  // Heap-allocate buffers using the actual sizes from the WASI call.
  std::unique_ptr<uint8_t *[]> env_ptrs = std::make_unique<uint8_t *[]>(env_count);
  std::unique_ptr<uint8_t[]> env_buf = std::make_unique<uint8_t[]>(env_buf_size);

  if (__wasi_environ_get(env_ptrs.get(), env_buf.get()) != 0) {
    body += "Error: failed to retrieve environment variables\n";
    return body;
  }

  for (__wasi_size_t i = 0; i < env_count; ++i) {
    const char *entry = reinterpret_cast<const char *>(env_ptrs[i]);
    if (entry) {
      body += "  ";
      body += entry;
      body += "\n";
    }
  }

  return body;
}

std::string build_header_body() {
  std::string body;
  body.reserve(4096);

  body += "\n\n=== Request Headers ===\n\n";

  WasmDataPtr result = getRequestHeaderPairs();
  std::vector<std::pair<std::string_view, std::string_view>> pairs = result->pairs();

  body += "Header count: " + std::to_string(pairs.size()) + "\n\n";

  for (const std::pair<std::string_view, std::string_view> &p : pairs) {
    body += "  ";
    body += std::string(p.first);
    body += ": ";
    body += std::string(p.second);
    body += "\n";
  }

  return body;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Request handling
// ---------------------------------------------------------------------------
FilterHeadersStatus SampleContext::onRequestHeaders(uint32_t, bool) {
  LOG_INFO("sample_filter: onRequestHeaders");

  // Build response body: environment variables + request headers.
  std::string body = build_environ_body();
  body += build_header_body();

  // Send a local response (bypasses upstream).
  sendLocalResponse(200, "", body, {});

  return FilterHeadersStatus::StopIteration;
}

FilterHeadersStatus SampleContext::onResponseHeaders(uint32_t, bool) {
  LOG_INFO("sample_filter: onResponseHeaders — adding custom headers");

  // Demonstrate response header manipulation.
  addResponseHeader("X-Wasm-Filter", "sample_filter/active");
  addResponseHeader("X-Powered-By", "lswasm/proxy-wasm");

  return FilterHeadersStatus::Continue;
}

FilterDataStatus SampleContext::onRequestBody(size_t, bool) {
  LOG_INFO("sample_filter: onRequestBody");
  return FilterDataStatus::Continue;
}

void SampleContext::onDone() {
  LOG_INFO("sample_filter: onDone");
}

void SampleContext::onLog() {}

void SampleContext::onDelete() {}
