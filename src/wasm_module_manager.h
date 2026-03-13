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

#pragma once

#include <string>
#include <memory>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <chrono>

#include "log.h"
#include "http_utils.h"

#include "proxy-wasm/wasm_vm.h"
#include "proxy-wasm/wasm.h"
#include "proxy-wasm/context.h"

#ifdef WASM_RUNTIME_WASMTIME
#include "proxy-wasm/wasmtime.h"
#elif defined(WASM_RUNTIME_V8)
#include "proxy-wasm/v8.h"
#elif defined(WASM_RUNTIME_WASMEDGE)
#include "proxy-wasm/wasmedge.h"
#elif defined(WASM_RUNTIME_WAMR)
#include "proxy-wasm/wamr.h"
#endif

// ResponseSink is used by the streaming response methods in LsWasmContext.
#include "response_sink.h"

// Streaming response state machine for LsWasmContext.
enum class StreamingResponseState {
  Idle,          // No streaming response in progress (default).
  HeadersSent,   // Response status line + headers have been written.
  Finished       // Final chunk sent; response is complete.
};

// header_name_eq() is now defined in http_utils.h (included above).

namespace lswasm {

/**
 * LsWasmIntegration - WasmVmIntegration for lswasm host.
 * Provides logging and error handling hooks required by the proxy-wasm VM.
 */
class LsWasmIntegration : public proxy_wasm::WasmVmIntegration {
public:
  ~LsWasmIntegration() override = default;
  proxy_wasm::WasmVmIntegration *clone() override { return new LsWasmIntegration{}; }

  proxy_wasm::LogLevel getLogLevel() override { return proxy_wasm::LogLevel::info; }

  void error(std::string_view message) override {
    LOG_ERROR("[WASM VM Error] " << message);
  }

  void trace(std::string_view message) override {
    LOG_INFO("[WASM VM Trace] " << message);
  }

  bool getNullVmFunction(std::string_view /*function_name*/, bool /*returns_word*/,
                         int /*number_of_arguments*/, proxy_wasm::NullPlugin * /*plugin*/,
                         void * /*ptr_to_function_return*/) override {
    return false;
  }
};

// Forward declaration — full definition follows after MetricStore.
class LsWasm;

/**
 * LsWasmContext - Custom ContextBase for lswasm.
 * Captures proxy_log output and sendLocalResponse data so the host can
 * relay them back to the HTTP client.
 */
class LsWasmContext : public proxy_wasm::ContextBase {
public:
  using proxy_wasm::ContextBase::ContextBase;

  // Fix up the parent context for stream (non-root) contexts.
  // The factory method LsWasm::createContext() uses the root context
  // constructor (WasmBase*, PluginBase) which sets parent_context_ = this
  // (self-referencing).  Before calling onCreate(), the caller must call
  // this method so that proxy_on_context_create receives the *root*
  // context id rather than the stream context's own id.
  void setParentContext(proxy_wasm::ContextBase *parent) {
    parent_context_ = parent;
    parent_context_id_ = parent ? parent->id() : 0;
  }

  // Override error() to log via LOG_ERROR instead of the base-class behaviour
  // (std::cerr + abort), which makes unimplemented-but-non-critical API calls
  // survivable rather than immediately fatal.
  void error(std::string_view message) override {
    LOG_ERROR("[WASM Context Error] " << message);
  }

  // Override unimplemented() so that missing API methods log and return
  // WasmResult::Unimplemented instead of aborting.
  proxy_wasm::WasmResult unimplemented() override {
    LOG_ERROR("[WASM] unimplemented proxy-wasm API called");
    return proxy_wasm::WasmResult::Unimplemented;
  }

  // ---- ABI 0.1.0 compatibility: getConfiguration / getStatus ----
  // In ABI 0.1.0, modules call proxy_get_configuration during
  // proxy_on_vm_start (to get vm_configuration) and proxy_on_configure
  // (to get plugin_configuration).  The base class defaults to
  // unimplemented(), which logs an error and returns "".
  //
  // The ContextBase stores the current plugin in temp_plugin_ during
  // onConfigure, and the vm_configuration lives on the WasmBase.
  // We return whichever is appropriate.
  std::string_view getConfiguration() override {
    // During onConfigure, plugin_ or temp_plugin_ holds the plugin
    // whose configuration the module is asking for.
    if (plugin_) {
      return plugin_->plugin_configuration_;
    }
    // During onStart (proxy_on_vm_start), plugin_ may not be set yet;
    // return the VM-level configuration instead.
    if (wasm()) {
      return wasm()->vm_configuration();
    }
    return "";
  }

  std::pair<uint32_t, std::string_view> getStatus() override {
    // Return OK status; lswasm does not have a concept of status codes
    // outside of HTTP responses.
    return std::make_pair(0, "");
  }

  // ---- continueStream / closeStream / clearRouteCache ----
  // These are called when the filter wants to resume processing after
  // pausing the stream (e.g. after an async call), close the stream, or
  // clear the routing cache.  For lswasm's simple single-request model
  // they are all no-ops.
  proxy_wasm::WasmResult continueStream(proxy_wasm::WasmStreamType /* stream_type */) override {
    return proxy_wasm::WasmResult::Ok;
  }

  proxy_wasm::WasmResult closeStream(proxy_wasm::WasmStreamType /* stream_type */) override {
    return proxy_wasm::WasmResult::Ok;
  }

  void clearRouteCache() override {
    // No-op: lswasm does not maintain a route cache.
  }

  // ---- Buffer access (proxy_get_buffer_bytes / proxy_set_buffer_bytes) ----
  // Modules call proxy_get_buffer_bytes to read the plugin configuration
  // (ABI 0.2.x) or HTTP body (all ABIs).  Return a BufferBase pointing
  // at the relevant data when we have it, nullptr otherwise.
  proxy_wasm::BufferInterface *getBuffer(proxy_wasm::WasmBufferType type) override {
    switch (type) {
      case proxy_wasm::WasmBufferType::PluginConfiguration:
        if (plugin_) {
          buffer_.set(plugin_->plugin_configuration_);
          return &buffer_;
        }
        if (temp_plugin_) {
          buffer_.set(temp_plugin_->plugin_configuration_);
          return &buffer_;
        }
        return nullptr;
      case proxy_wasm::WasmBufferType::VmConfiguration:
        if (wasm()) {
          buffer_.set(wasm()->vm_configuration());
          return &buffer_;
        }
        return nullptr;
      case proxy_wasm::WasmBufferType::HttpRequestBody:
        if (!request_body_.empty()) {
          buffer_.set(request_body_);
          return &buffer_;
        }
        return nullptr;
      case proxy_wasm::WasmBufferType::HttpResponseBody:
        if (!response_body_.empty()) {
          buffer_.set(response_body_);
          return &buffer_;
        }
        return nullptr;
      default:
        LOG_INFO("[WASM] getBuffer: unsupported buffer type "
                 << static_cast<int>(type));
        return nullptr;
    }
  }

  bool endOfStream(proxy_wasm::WasmStreamType /* type */) override {
    return end_of_stream_;
  }

  // Set the request body data for the current chunk (called by the host
  // before invoking onRequestBody).
  void setRequestBody(std::string_view body) { request_body_ = body; }

  // Set the response body data for the current chunk (called by the host
  // before invoking onResponseBody).
  void setResponseBody(std::string_view body) { response_body_ = body; }

  // Set the end-of-stream flag (called by the host before invoking
  // onRequestBody / onResponseBody).
  void setEndOfStream(bool eos) { end_of_stream_ = eos; }

  // Property accessor — the proxy-wasm-cpp-SDK calls proxy_get_property
  // during proxy_on_context_create to obtain "plugin_root_id", which is
  // used to look up the registered RootContext factory.  Without this
  // override the base class aborts with "unimplemented proxy-wasm API".
  proxy_wasm::WasmResult getProperty(std::string_view path,
                                     std::string *result) override {
    if (!result) {
      return proxy_wasm::WasmResult::BadArgument;
    }
    if (path == "plugin_root_id") {
      *result = std::string(root_id());
      return proxy_wasm::WasmResult::Ok;
    }
    // For any other property, return NotFound instead of aborting.
    LOG_INFO("[WASM] getProperty: unknown path '" << std::string(path) << "'");
    return proxy_wasm::WasmResult::NotFound;
  }

  proxy_wasm::WasmResult setProperty(std::string_view key,
                                     std::string_view /* serialized_value */) override {
    LOG_INFO("[WASM] setProperty: '" << std::string(key) << "' (ignored)");
    return proxy_wasm::WasmResult::Ok;
  }

  // ---- Metrics ----
  // Delegates to the LsWasm metric store so values are shared across all
  // contexts within a module.  Implemented out-of-line after LsWasm is
  // defined (see below).
  proxy_wasm::WasmResult defineMetric(uint32_t type, std::string_view name,
                                      uint32_t *metric_id_ptr) override;
  proxy_wasm::WasmResult incrementMetric(uint32_t metric_id,
                                         int64_t offset) override;
  proxy_wasm::WasmResult recordMetric(uint32_t metric_id,
                                      uint64_t value) override;
  proxy_wasm::WasmResult getMetric(uint32_t metric_id,
                                   uint64_t *value_ptr) override;

  // ---- HTTP call stub ----
  // Some filters attempt outbound HTTP calls.  Return Unimplemented
  // gracefully (no abort).
  proxy_wasm::WasmResult httpCall(std::string_view target,
                                  const proxy_wasm::Pairs & /* request_headers */,
                                  std::string_view /* request_body */,
                                  const proxy_wasm::Pairs & /* request_trailers */,
                                  int /* timeout_milliseconds */,
                                  uint32_t * /* token_ptr */) override {
    LOG_INFO("[WASM] httpCall to '" << std::string(target) << "' (not supported)");
    return proxy_wasm::WasmResult::Unimplemented;
  }

  // Capture log messages from the WASM module.
  proxy_wasm::WasmResult log(uint32_t level, std::string_view message) override {
    LOG_INFO("[WASM log L" << level << "] " << message);
    log_ += std::string(message) + "\n";
    return proxy_wasm::WasmResult::Ok;
  }

  uint32_t getLogLevel() override { return static_cast<uint32_t>(proxy_wasm::LogLevel::trace); }

  uint64_t getCurrentTimeNanoseconds() override {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  uint64_t getMonotonicTimeNanoseconds() override {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  // Capture sendLocalResponse from the WASM module (called by proxy_send_local_response).
  proxy_wasm::WasmResult sendLocalResponse(uint32_t response_code, std::string_view body,
                                           proxy_wasm::Pairs additional_headers,
                                           proxy_wasm::GrpcStatusCode grpc_status,
                                           std::string_view details) override {
    local_response_code_ = response_code;
    local_response_body_ = std::string(body);
    local_response_details_ = std::string(details);
    // Convert string_view pairs to owned strings.
    local_response_headers_.clear();
    for (const std::pair<std::string_view, std::string_view> &h : additional_headers) {
      local_response_headers_.emplace_back(std::string(h.first), std::string(h.second));
    }
    has_local_response_ = true;
    LOG_INFO("[WASM] sendLocalResponse: code=" << response_code
             << " body_size=" << body.size()
             << " additional_headers=" << local_response_headers_.size());
    return proxy_wasm::WasmResult::Ok;
  }

  // ---- Header/Trailer/Metadata Map overrides ----
  // Internal storage uses owned strings (HeaderPairs).  The proxy-wasm API
  // uses string_view Pairs, so we convert at the boundary.

  proxy_wasm::WasmResult getHeaderMapPairs(proxy_wasm::WasmHeaderMapType type,
                                           proxy_wasm::Pairs *result) override {
    result->clear();
    std::map<proxy_wasm::WasmHeaderMapType, HeaderPairs>::const_iterator it = header_maps_.find(type);
    if (it != header_maps_.end()) {
      for (const std::pair<std::string, std::string> &p : it->second) {
        result->emplace_back(std::string_view(p.first), std::string_view(p.second));
      }
    }
    return proxy_wasm::WasmResult::Ok;
  }

  proxy_wasm::WasmResult setHeaderMapPairs(proxy_wasm::WasmHeaderMapType type,
                                           const proxy_wasm::Pairs &pairs) override {
    HeaderPairs &owned = header_maps_[type];
    owned.clear();
    for (const std::pair<std::string_view, std::string_view> &p : pairs) {
      owned.emplace_back(std::string(p.first), std::string(p.second));
    }
    return proxy_wasm::WasmResult::Ok;
  }

  proxy_wasm::WasmResult getHeaderMapValue(proxy_wasm::WasmHeaderMapType type,
                                           std::string_view key,
                                           std::string_view *result) override {
    std::map<proxy_wasm::WasmHeaderMapType, HeaderPairs>::const_iterator it = header_maps_.find(type);
    if (it != header_maps_.end()) {
      for (const std::pair<std::string, std::string> &pair : it->second) {
        if (header_name_eq(pair.first, key)) {
          // Return view into owned string — valid until map is modified.
          *result = std::string_view(pair.second);
          return proxy_wasm::WasmResult::Ok;
        }
      }
    }
    *result = "";
    return proxy_wasm::WasmResult::Ok;
  }

  proxy_wasm::WasmResult addHeaderMapValue(proxy_wasm::WasmHeaderMapType type,
                                           std::string_view key,
                                           std::string_view value) override {
    header_maps_[type].emplace_back(std::string(key), std::string(value));
    return proxy_wasm::WasmResult::Ok;
  }

  proxy_wasm::WasmResult replaceHeaderMapValue(proxy_wasm::WasmHeaderMapType type,
                                               std::string_view key,
                                               std::string_view value) override {
    HeaderPairs &pairs = header_maps_[type];
    // Remove all existing occurrences of this key (handles multi-value headers).
    pairs.erase(std::remove_if(pairs.begin(), pairs.end(),
                                [&key](const std::pair<std::string, std::string> &p) { return header_name_eq(p.first, key); }),
                pairs.end());
    // Add a single replacement value.
    pairs.emplace_back(std::string(key), std::string(value));
    return proxy_wasm::WasmResult::Ok;
  }

  proxy_wasm::WasmResult removeHeaderMapValue(proxy_wasm::WasmHeaderMapType type,
                                              std::string_view key) override {
    std::map<proxy_wasm::WasmHeaderMapType, HeaderPairs>::iterator it = header_maps_.find(type);
    if (it != header_maps_.end()) {
      HeaderPairs &pairs = it->second;
      pairs.erase(std::remove_if(pairs.begin(), pairs.end(),
                                  [&key](const std::pair<std::string, std::string> &p) { return header_name_eq(p.first, key); }),
                  pairs.end());
    }
    return proxy_wasm::WasmResult::Ok;
  }

  proxy_wasm::WasmResult getHeaderMapSize(proxy_wasm::WasmHeaderMapType type,
                                          uint32_t *result) override {
    std::map<proxy_wasm::WasmHeaderMapType, HeaderPairs>::const_iterator it = header_maps_.find(type);
    if (it != header_maps_.end()) {
      // Size in bytes: sum of key + value lengths + 4 bytes per pair for NULs.
      uint32_t size = 0;
      for (const std::pair<std::string, std::string> &pair : it->second) {
        size += pair.first.size() + pair.second.size() + 4;
      }
      *result = size;
    } else {
      *result = 0;
    }
    return proxy_wasm::WasmResult::Ok;
  }

  // ---- Header map accessors for the host (owned strings) ----

  /** Set the header map for a given type (used by the host to populate before filter execution). */
  void setHeaderMap(proxy_wasm::WasmHeaderMapType type, const HeaderPairs &pairs) {
    header_maps_[type] = pairs;
  }

  /** Get the header map for a given type (used by the host to read after filter execution). */
  HeaderPairs getHeaderMapOwned(proxy_wasm::WasmHeaderMapType type) const {
    std::map<proxy_wasm::WasmHeaderMapType, HeaderPairs>::const_iterator it = header_maps_.find(type);
    return it != header_maps_.end() ? it->second : HeaderPairs{};
  }

  // Accessors for captured local response.
  bool hasLocalResponse() const { return has_local_response_; }
  uint32_t localResponseCode() const { return local_response_code_; }
  const std::string &localResponseBody() const { return local_response_body_; }
  const HeaderPairs &localResponseHeaders() const { return local_response_headers_; }
  const std::string &logOutput() const { return log_; }

  void resetLocalResponse() {
    has_local_response_ = false;
    local_response_code_ = 0;
    local_response_body_.clear();
    local_response_details_.clear();
    local_response_headers_.clear();
  }

  void resetHeaderMaps() {
    header_maps_.clear();
  }

  // ---- Streaming response API ----
  // These methods are called by the foreign-function handlers registered
  // in main.cpp.  They delegate I/O to the ResponseSink injected via
  // setResponseSink().

  /// Inject the ResponseSink for this request (called by HttpFilterContext).
  void setResponseSink(ResponseSink *sink) { sink_ = sink; }

  /// Current streaming state.
  StreamingResponseState streamingState() const { return streaming_state_; }

  /// True if a streaming response has been started (headers sent).
  bool hasStreamingResponse() const {
    return streaming_state_ != StreamingResponseState::Idle;
  }

  /// True if the streaming response has been finished.
  bool isStreamingFinished() const {
    return streaming_state_ == StreamingResponseState::Finished;
  }

  /// Send the response status line and headers via the ResponseSink.
  /// Must be called exactly once before any streamingWriteChunk() calls.
  /// Returns Ok on success, or an error WasmResult.
  proxy_wasm::WasmResult streamingSendHeaders(uint32_t status_code,
                                              const HeaderPairs &headers) {
    if (!sink_) {
      LOG_ERROR("[Streaming] no ResponseSink — cannot send headers");
      return proxy_wasm::WasmResult::InternalFailure;
    }
    if (streaming_state_ != StreamingResponseState::Idle) {
      LOG_ERROR("[Streaming] headers already sent");
      return proxy_wasm::WasmResult::BadArgument;
    }

    if (!sink_->sendHeaders(status_code, headers, /*streaming=*/true)) {
      LOG_ERROR("[Streaming] sendHeaders failed");
      return proxy_wasm::WasmResult::InternalFailure;
    }
    streaming_state_ = StreamingResponseState::HeadersSent;
    LOG_INFO("[Streaming] sent headers: status=" << status_code
             << " header_count=" << headers.size());
    return proxy_wasm::WasmResult::Ok;
  }

  /// Write a chunk of body data.  May be called zero or more times after
  /// streamingSendHeaders() and before streamingFinish().
  proxy_wasm::WasmResult streamingWriteChunk(std::string_view data) {
    if (!sink_) {
      LOG_ERROR("[Streaming] no ResponseSink — cannot write chunk");
      return proxy_wasm::WasmResult::InternalFailure;
    }
    if (streaming_state_ != StreamingResponseState::HeadersSent) {
      LOG_ERROR("[Streaming] writeChunk called in wrong state ("
                << static_cast<int>(streaming_state_) << ")");
      return proxy_wasm::WasmResult::BadArgument;
    }
    if (!data.empty()) {
      if (!sink_->writeBody(data)) {
        LOG_ERROR("[Streaming] writeBody failed");
        return proxy_wasm::WasmResult::InternalFailure;
      }
    }
    return proxy_wasm::WasmResult::Ok;
  }

  /// Signal that the response is complete.  No more chunks may be written.
  proxy_wasm::WasmResult streamingFinish() {
    if (!sink_) {
      LOG_ERROR("[Streaming] no ResponseSink — cannot finish");
      return proxy_wasm::WasmResult::InternalFailure;
    }
    if (streaming_state_ != StreamingResponseState::HeadersSent) {
      LOG_ERROR("[Streaming] finish called in wrong state ("
                << static_cast<int>(streaming_state_) << ")");
      return proxy_wasm::WasmResult::BadArgument;
    }
    if (!sink_->finishBody()) {
      LOG_ERROR("[Streaming] finishBody failed");
      return proxy_wasm::WasmResult::InternalFailure;
    }
    streaming_state_ = StreamingResponseState::Finished;
    LOG_INFO("[Streaming] response finished");
    return proxy_wasm::WasmResult::Ok;
  }

  /// Reset streaming state between requests (if context is reused).
  void resetStreamingState() {
    sink_ = nullptr;
    streaming_state_ = StreamingResponseState::Idle;
  }

private:
  // Helper: downcast wasm() to LsWasm* (defined out-of-line after LsWasm).
  inline LsWasm *lswasm();

  std::string log_;
  bool has_local_response_ = false;
  uint32_t local_response_code_ = 0;
  std::string local_response_body_;
  std::string local_response_details_;
  HeaderPairs local_response_headers_;

  // Per-request header maps keyed by WasmHeaderMapType.  Stored as owned strings.
  std::map<proxy_wasm::WasmHeaderMapType, HeaderPairs> header_maps_;

  // Per-request body data set by the host before filter callbacks.
  std::string_view request_body_;
  std::string_view response_body_;
  bool end_of_stream_ = true;

  // Scratch buffer for getBuffer() — points into owned data elsewhere
  // (plugin_configuration_, vm_configuration, body data, etc.).
  proxy_wasm::BufferBase buffer_;

  // ---- Streaming response state ----
  ResponseSink *sink_ = nullptr;
  StreamingResponseState streaming_state_ = StreamingResponseState::Idle;
};

/**
 * MetricStore - In-memory metric storage for a WASM module.
 *
 * Thread safety:
 *   - define() is protected by a std::mutex (cold path, only called during
 *     module initialization or occasional metric definition).
 *   - increment(), record(), get() use std::atomic<int64_t> for lock-free
 *     access on the hot path.
 *
 * Metrics are identified by a uint32_t ID whose low 2 bits encode the
 * MetricType (Counter=0, Gauge=1, Histogram=2), matching the convention
 * used by WasmBase in the proxy-wasm-cpp-host.
 *
 * Counter:   monotonically increasing via incrementMetric (offset added).
 * Gauge:     point-in-time value via recordMetric (value replaced).
 * Histogram: simplified — each recordMetric accumulates into a sum.
 */
class MetricStore {
public:
  // MetricType values (from proxy_wasm_enums.h):
  //   Counter   = 0
  //   Gauge     = 1
  //   Histogram = 2
  static constexpr uint32_t kTypeMask = 0x3;
  static constexpr uint32_t kIdIncrement = 0x4;

  struct MetricEntry {
    std::string name;
    uint32_t type;                    // proxy_wasm::MetricType cast to uint32_t
    std::atomic<int64_t> value{0};    // signed to support negative gauge deltas

    MetricEntry() : type(0) {}
    MetricEntry(std::string n, uint32_t t) : name(std::move(n)), type(t) {}

    // Non-copyable due to atomic — provide move semantics.
    MetricEntry(MetricEntry &&other) noexcept
        : name(std::move(other.name)), type(other.type),
          value(other.value.load(std::memory_order_relaxed)) {}
    MetricEntry &operator=(MetricEntry &&other) noexcept {
      name = std::move(other.name);
      type = other.type;
      value.store(other.value.load(std::memory_order_relaxed), std::memory_order_relaxed);
      return *this;
    }

    MetricEntry(const MetricEntry &) = delete;
    MetricEntry &operator=(const MetricEntry &) = delete;
  };

  /**
   * Define (or look up) a metric.
   * If a metric with the same name and type already exists, the existing
   * ID is returned (idempotent).
   * Thread-safe: protected by define_mutex_.
   */
  proxy_wasm::WasmResult define(uint32_t type, std::string_view name, uint32_t *id_out) {
    std::unique_lock<std::shared_mutex> lock(define_mutex_);

    // Check for an existing metric with the same name.
    for (const auto &[id, entry] : metrics_) {
      if (entry.name == name) {
        if (entry.type != type) {
          LOG_ERROR("[Metrics] redefining '" << name << "' with different type");
          return proxy_wasm::WasmResult::BadArgument;
        }
        if (id_out) *id_out = id;
        return proxy_wasm::WasmResult::Ok;
      }
    }

    // Allocate a new ID — low bits encode the type.
    uint32_t &next = nextIdForType(type);
    uint32_t id = next;
    next += kIdIncrement;

    metrics_.emplace(id, MetricEntry(std::string(name), type));

    LOG_INFO("[Metrics] defined " << metricTypeName(type)
             << " '" << name << "' → id " << id);

    if (id_out) *id_out = id;
    return proxy_wasm::WasmResult::Ok;
  }

  /**
   * Increment a Counter (or Gauge) by the given offset.
   * Thread-safe: shared lock on define_mutex_ protects the map lookup;
   * atomic fetch_add protects the value.
   */
  proxy_wasm::WasmResult increment(uint32_t id, int64_t offset) {
    std::shared_lock<std::shared_mutex> lock(define_mutex_);
    auto it = metrics_.find(id);
    if (it == metrics_.end()) {
      return proxy_wasm::WasmResult::NotFound;
    }
    it->second.value.fetch_add(offset, std::memory_order_relaxed);
    return proxy_wasm::WasmResult::Ok;
  }

  /**
   * Record a metric value.
   * Counter:   value is added (same as increment with a positive offset).
   * Gauge:     value replaces the current reading.
   * Histogram: value is added to a running sum (simplified).
   * Thread-safe: shared lock on define_mutex_ protects the map lookup;
   * atomic operations protect the value.
   */
  proxy_wasm::WasmResult record(uint32_t id, uint64_t value) {
    std::shared_lock<std::shared_mutex> lock(define_mutex_);
    auto it = metrics_.find(id);
    if (it == metrics_.end()) {
      return proxy_wasm::WasmResult::NotFound;
    }
    uint32_t type = it->second.type;
    if (type == 1) {  // Gauge
      it->second.value.store(static_cast<int64_t>(value), std::memory_order_relaxed);
    } else {
      // Counter or Histogram: accumulate.
      it->second.value.fetch_add(static_cast<int64_t>(value), std::memory_order_relaxed);
    }
    return proxy_wasm::WasmResult::Ok;
  }

  /**
   * Read the current metric value.
   * Thread-safe: shared lock on define_mutex_ protects the map lookup;
   * atomic load protects the value.
   */
  proxy_wasm::WasmResult get(uint32_t id, uint64_t *value_out) const {
    std::shared_lock<std::shared_mutex> lock(define_mutex_);
    auto it = metrics_.find(id);
    if (it == metrics_.end()) {
      return proxy_wasm::WasmResult::NotFound;
    }
    if (value_out) {
      *value_out = static_cast<uint64_t>(it->second.value.load(std::memory_order_relaxed));
    }
    return proxy_wasm::WasmResult::Ok;
  }

  /** Retrieve all metrics (for diagnostics / HTTP endpoint). */
  const std::map<uint32_t, MetricEntry> &all() const { return metrics_; }

private:
  uint32_t &nextIdForType(uint32_t type) {
    switch (type) {
      case 0:  return next_counter_id_;
      case 1:  return next_gauge_id_;
      case 2:  return next_histogram_id_;
      default: return next_counter_id_;  // fallback
    }
  }

  static const char *metricTypeName(uint32_t type) {
    switch (type) {
      case 0:  return "Counter";
      case 1:  return "Gauge";
      case 2:  return "Histogram";
      default: return "Unknown";
    }
  }

  std::map<uint32_t, MetricEntry> metrics_;
  uint32_t next_counter_id_   = 0;  // Counter   type = 0
  uint32_t next_gauge_id_     = 1;  // Gauge     type = 1
  uint32_t next_histogram_id_ = 2;  // Histogram type = 2
  mutable std::shared_mutex define_mutex_;  // Protects metrics_ map: unique lock in define(), shared lock in read methods
};

/**
 * LsWasm - Custom WasmBase for lswasm.
 * Overrides context creation to use LsWasmContext.
 */
class LsWasm : public proxy_wasm::WasmBase {
public:
  LsWasm(std::unique_ptr<proxy_wasm::WasmVm> wasm_vm,
         std::unordered_map<std::string, std::string> envs,
         std::string_view vm_id = "", std::string_view vm_configuration = "",
         std::string_view vm_key = "")
      : proxy_wasm::WasmBase(std::move(wasm_vm), vm_id, vm_configuration, vm_key,
                              std::move(envs), {}),
        metrics_(std::make_shared<MetricStore>()) {}

  // Clone constructor: creates a thread-local VM clone that shares the
  // MetricStore with the base VM.  Uses the WasmBase(base_handle, factory)
  // constructor which internally calls WasmVm::clone().
  LsWasm(const std::shared_ptr<proxy_wasm::WasmHandleBase> &base_handle,
         const proxy_wasm::WasmVmFactory &factory)
      : proxy_wasm::WasmBase(base_handle, factory),
        metrics_(std::dynamic_pointer_cast<LsWasm>(base_handle->wasm())->sharedMetrics()) {}

  proxy_wasm::ContextBase *createVmContext() override { return new LsWasmContext(this); }

  proxy_wasm::ContextBase *createRootContext(
      const std::shared_ptr<proxy_wasm::PluginBase> &plugin) override {
    return new LsWasmContext(this, plugin);
  }

  proxy_wasm::ContextBase *createContext(
      const std::shared_ptr<proxy_wasm::PluginBase> &plugin) override {
    return new LsWasmContext(this, plugin);
  }

  /** Per-module metric store shared by all contexts (including thread-local clones). */
  MetricStore &metrics() { return *metrics_; }
  const MetricStore &metrics() const { return *metrics_; }

  /** Shared pointer to the metric store — used when creating clones. */
  std::shared_ptr<MetricStore> sharedMetrics() { return metrics_; }

private:
  std::shared_ptr<MetricStore> metrics_;
};

// ---- Out-of-line LsWasmContext metric methods (need LsWasm definition) ----

inline LsWasm *LsWasmContext::lswasm() {
  return dynamic_cast<LsWasm *>(wasm());
}

inline proxy_wasm::WasmResult LsWasmContext::defineMetric(uint32_t type, std::string_view name,
                                                          uint32_t *metric_id_ptr) {
  LsWasm *lw = lswasm();
  if (!lw) return proxy_wasm::WasmResult::InternalFailure;
  return lw->metrics().define(type, name, metric_id_ptr);
}

inline proxy_wasm::WasmResult LsWasmContext::incrementMetric(uint32_t metric_id, int64_t offset) {
  LsWasm *lw = lswasm();
  if (!lw) return proxy_wasm::WasmResult::InternalFailure;
  return lw->metrics().increment(metric_id, offset);
}

inline proxy_wasm::WasmResult LsWasmContext::recordMetric(uint32_t metric_id, uint64_t value) {
  LsWasm *lw = lswasm();
  if (!lw) return proxy_wasm::WasmResult::InternalFailure;
  return lw->metrics().record(metric_id, value);
}

inline proxy_wasm::WasmResult LsWasmContext::getMetric(uint32_t metric_id, uint64_t *value_ptr) {
  LsWasm *lw = lswasm();
  if (!lw) return proxy_wasm::WasmResult::InternalFailure;
  return lw->metrics().get(metric_id, value_ptr);
}

// ---- WasmHandleBase / PluginHandleBase subclasses for thread-local cloning ----

/**
 * LsWasmHandle - WasmHandleBase subclass for lswasm.
 * Required by the proxy-wasm createWasm() / getOrCreateThreadLocalPlugin() API.
 */
class LsWasmHandle : public proxy_wasm::WasmHandleBase {
public:
  explicit LsWasmHandle(std::shared_ptr<proxy_wasm::WasmBase> wasm_base)
      : proxy_wasm::WasmHandleBase(std::move(wasm_base)) {}
};

/**
 * LsPluginHandle - PluginHandleBase subclass for lswasm.
 * Required by the proxy-wasm getOrCreateThreadLocalPlugin() API.
 */
class LsPluginHandle : public proxy_wasm::PluginHandleBase {
public:
  LsPluginHandle(std::shared_ptr<proxy_wasm::WasmHandleBase> wasm_handle,
                 std::shared_ptr<proxy_wasm::PluginBase> plugin)
      : proxy_wasm::PluginHandleBase(std::move(wasm_handle), std::move(plugin)) {}
};

} // namespace lswasm

/**
 * WasmModuleManager - Manages loading and execution of WASM modules
 * using the proxy-wasm-cpp-host library.
 *
 * Thread safety:
 *   - modules_ map is protected by modules_mutex_ (shared_mutex).
 *   - Request processing uses thread-local VM clones via
 *     getOrCreateThreadLocalPlugin() — no per-request locking needed.
 *   - Module load/unload takes a write lock; request processing takes a read lock.
 */
class WasmModuleManager {
public:
  WasmModuleManager() = default;
  ~WasmModuleManager() = default;

  /**
   * Per-module state.  After loadModule(), all fields are read-only
   * except that thread-local VM clones are created on demand.
   */
  struct ModuleState {
    std::shared_ptr<proxy_wasm::WasmHandleBase> base_handle;  // base VM (read-only after load)
    std::shared_ptr<proxy_wasm::PluginBase> plugin;            // plugin config (read-only)
    proxy_wasm::WasmHandleCloneFactory clone_factory;          // creates thread-local VM clones
    proxy_wasm::PluginHandleFactory plugin_factory;            // creates thread-local plugin handles
  };

  /**
   * RequestScope - Owns a stream context for one request's lifetime.
   *
   * Calls getOrCreateThreadLocalPlugin() to obtain a per-thread VM clone,
   * then creates a stream context on that clone.  Each worker thread gets
   * its own VM instance — no locking needed on VM state.
   *
   * RAII: the destructor calls onDone() and onDelete() to properly tear
   * down the WASM stream context.
   */
  class RequestScope {
  public:
    /**
     * Create a request scope for the given module.
     * @param state  Module state (from the loaded modules map).
     * @param context_id  Unique context ID for this request.
     * @return true if the scope was successfully created, false on failure.
     */
    bool init(const ModuleState &state, uint32_t context_id) {
      // Obtain (or create) a thread-local VM clone + plugin context.
      plugin_handle_ = proxy_wasm::getOrCreateThreadLocalPlugin(
          state.base_handle, state.plugin,
          state.clone_factory, state.plugin_factory);
      if (!plugin_handle_) {
        LOG_ERROR("[RequestScope] Failed to get thread-local plugin for context "
                  << context_id);
        return false;
      }

      proxy_wasm::WasmBase *wasm = plugin_handle_->wasm().get();
      if (!wasm || wasm->isFailed()) {
        LOG_ERROR("[RequestScope] Thread-local VM not available for context "
                  << context_id);
        return false;
      }

      // Get the root context for this plugin on the thread-local VM.
      proxy_wasm::ContextBase *root_ctx = wasm->getRootContext(state.plugin, false);
      if (!root_ctx) {
        LOG_ERROR("[RequestScope] No root context for context " << context_id);
        return false;
      }

      // Create a stream context via createContext.
      proxy_wasm::ContextBase *ctx = wasm->createContext(state.plugin);
      if (!ctx) {
        LOG_ERROR("[RequestScope] Failed to create stream context for context "
                  << context_id);
        return false;
      }

      ctx_ = dynamic_cast<lswasm::LsWasmContext *>(ctx);
      if (!ctx_) {
        LOG_ERROR("[RequestScope] Dynamic cast to LsWasmContext failed");
        return false;
      }

      // Fix up the parent context (same as the old ensureStreamContext).
      ctx_->setParentContext(root_ctx);
      ctx_->onCreate();
      ctx_->resetLocalResponse();
      ctx_->resetHeaderMaps();
      context_id_ = context_id;
      return true;
    }

    ~RequestScope() {
      if (ctx_) {
        ctx_->onDone();
        ctx_->onDelete();
      }
    }

    // Non-copyable, but movable (to allow storage in containers).
    RequestScope() = default;
    RequestScope(const RequestScope &) = delete;
    RequestScope &operator=(const RequestScope &) = delete;
    RequestScope(RequestScope &&other) noexcept
        : ctx_(other.ctx_), plugin_handle_(std::move(other.plugin_handle_)),
          context_id_(other.context_id_) {
      other.ctx_ = nullptr;
      other.context_id_ = 0;
    }
    RequestScope &operator=(RequestScope &&other) noexcept {
      if (this != &other) {
        // Clean up existing context if any.
        if (ctx_) {
          ctx_->onDone();
          ctx_->onDelete();
        }
        ctx_ = other.ctx_;
        plugin_handle_ = std::move(other.plugin_handle_);
        context_id_ = other.context_id_;
        other.ctx_ = nullptr;
        other.context_id_ = 0;
      }
      return *this;
    }

    /** The stream context for this request (nullptr if init() failed). */
    lswasm::LsWasmContext *context() { return ctx_; }
    const lswasm::LsWasmContext *context() const { return ctx_; }

    /** Check if the scope was successfully initialized. */
    bool valid() const { return ctx_ != nullptr; }

  private:
    lswasm::LsWasmContext *ctx_ = nullptr;
    std::shared_ptr<proxy_wasm::PluginHandleBase> plugin_handle_;
    uint32_t context_id_ = 0;
  };

  /**
   * Set environment variables to pass to WASM modules via WASI environ_get.
   * Must be called before loadModule().
   */
  void setEnvironmentVariables(const std::unordered_map<std::string, std::string> &envs) {
    envs_ = envs;
  }

  /**
   * Load a WASM module from file.
   * Thread-safe: takes a write lock on modules_mutex_.
   */
  bool loadModule(const std::string &module_path, const std::string &module_name);

  /**
   * Load a WASM module from memory.
   * Thread-safe: takes a write lock on modules_mutex_.
   */
  bool loadModuleFromMemory(const uint8_t *code, size_t code_size,
                            const std::string &module_name);

  /**
   * Create a RequestScope for the named module.
   * Thread-safe: takes a read lock on modules_mutex_.
   * @param module_name  Name of the loaded module.
   * @param context_id   Unique context ID for this request.
   * @param scope        Output: the initialized RequestScope.
   * @return true if the scope was successfully created.
   */
  bool createRequestScope(const std::string &module_name, uint32_t context_id,
                          RequestScope &scope) const;

  /**
   * Unload a module.
   * Thread-safe: takes a write lock on modules_mutex_.
   */
  bool unloadModule(const std::string &module_name);

  /**
   * Get list of loaded module names.
   * Thread-safe: takes a read lock on modules_mutex_.
   */
  std::vector<std::string> getLoadedModules() const;

  /**
   * Check if module is loaded.
   * Thread-safe: takes a read lock on modules_mutex_.
   */
  bool hasModule(const std::string &module_name) const;

private:
  mutable std::shared_mutex modules_mutex_;
  std::map<std::string, ModuleState> modules_;
  std::unordered_map<std::string, std::string> envs_;
};
