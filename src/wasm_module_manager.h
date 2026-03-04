#pragma once

#include <string>
#include <memory>
#include <algorithm>
#include <cctype>
#include <map>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <chrono>

#include "log.h"

#include "proxy-wasm/wasm_vm.h"
#include "proxy-wasm/wasm.h"
#include "proxy-wasm/context.h"

#ifdef WASM_RUNTIME_WASMTIME
#include "proxy-wasm/wasmtime.h"
#elif defined(WASM_RUNTIME_V8)
#include "proxy-wasm/v8.h"
#endif

// Owned header pairs type (std::string, not string_view).
using HeaderPairs = std::vector<std::pair<std::string, std::string>>;

// Case-insensitive comparison for HTTP header field names (RFC 7230 §3.2).
inline bool header_name_eq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

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

  // ---- continueStream / clearRouteCache ----
  // These are called when the filter wants to resume processing after
  // pausing the stream (e.g. after an async call) or to clear the
  // routing cache.  For lswasm's simple single-request model they are
  // no-ops.
  proxy_wasm::WasmResult continueStream(proxy_wasm::WasmStreamType /* stream_type */) override {
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
      default:
        LOG_INFO("[WASM] getBuffer: unsupported buffer type "
                 << static_cast<int>(type));
        return nullptr;
    }
  }

  bool endOfStream(proxy_wasm::WasmStreamType /* type */) override {
    // For the simple request model, always report end of stream.
    return true;
  }

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

private:
  std::string log_;
  bool has_local_response_ = false;
  uint32_t local_response_code_ = 0;
  std::string local_response_body_;
  std::string local_response_details_;
  HeaderPairs local_response_headers_;

  // Per-request header maps keyed by WasmHeaderMapType.  Stored as owned strings.
  std::map<proxy_wasm::WasmHeaderMapType, HeaderPairs> header_maps_;

  // Scratch buffer for getBuffer() — points into owned data elsewhere
  // (plugin_configuration_, vm_configuration, etc.).
  proxy_wasm::BufferBase buffer_;
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
                              std::move(envs), {}) {}

  proxy_wasm::ContextBase *createVmContext() override { return new LsWasmContext(this); }

  proxy_wasm::ContextBase *createRootContext(
      const std::shared_ptr<proxy_wasm::PluginBase> &plugin) override {
    return new LsWasmContext(this, plugin);
  }

  proxy_wasm::ContextBase *createContext(
      const std::shared_ptr<proxy_wasm::PluginBase> &plugin) override {
    return new LsWasmContext(this, plugin);
  }
};

} // namespace lswasm

/**
 * WasmModuleManager - Manages loading and execution of WASM modules
 * using the proxy-wasm-cpp-host library with Wasmtime runtime.
 */
class WasmModuleManager {
public:
  WasmModuleManager() = default;
  ~WasmModuleManager() = default;

  /**
   * Set environment variables to pass to WASM modules via WASI environ_get.
   * Must be called before loadModule().
   */
  void setEnvironmentVariables(const std::unordered_map<std::string, std::string> &envs) {
    envs_ = envs;
  }

  /**
   * Load a WASM module from file
   * @param module_path Path to the .wasm file
   * @param module_name Unique identifier for the module
   * @return true if successfully loaded
   */
  bool loadModule(const std::string &module_path, const std::string &module_name);

  /**
   * Load a WASM module from memory
   * @param code WASM bytecode
   * @param code_size Size of bytecode
   * @param module_name Unique identifier for the module
   * @return true if successfully loaded
   */
  bool loadModuleFromMemory(const uint8_t *code, size_t code_size,
                            const std::string &module_name);

  /**
   * Execute module filter on HTTP request/response.
   * Calls the appropriate proxy_on_* function in the WASM module.
   * @param module_name Name of the module to execute
   * @param context_id Context identifier
   * @param phase Filter phase (onRequestHeaders, onResponseHeaders, etc.)
   * @return true if execution successful
   */
  /**
   * Ensure a stream context exists for the given module and context_id.
   * Creates one (via proxy_on_context_create) if it doesn't already exist.
   * Must be called before setContextHeaders() so headers can be populated
   * before the filter phase executes.
   */
  bool ensureStreamContext(const std::string &module_name, uint32_t context_id);

  bool executeFilter(const std::string &module_name, uint32_t context_id,
                     const std::string &phase);

  /**
   * Get the local response body captured from the last sendLocalResponse call.
   * @param module_name Name of the module
   * @return the response body, or empty string if none
   */
  std::string getLocalResponseBody(const std::string &module_name) const;

  /**
   * Get the local response code captured from the last sendLocalResponse call.
   * @param module_name Name of the module
   * @return the response code, or 0 if none
   */
  uint32_t getLocalResponseCode(const std::string &module_name) const;

  /**
   * Check if a module has a pending local response.
   * @param module_name Name of the module
   * @return true if there is a pending local response
   */
  bool hasLocalResponse(const std::string &module_name) const;

  /**
   * Get the additional headers from the last sendLocalResponse call.
   * @param module_name Name of the module
   * @return the additional headers, or empty vector if none
   */
  HeaderPairs getLocalResponseHeaders(const std::string &module_name) const;

  /**
   * Set a header map on the module's stream context before filter execution.
   * @param module_name Name of the module
   * @param type Header map type (RequestHeaders, ResponseHeaders, etc.)
   * @param pairs Header key-value pairs (owned strings)
   */
  void setContextHeaders(const std::string &module_name, proxy_wasm::WasmHeaderMapType type,
                         const HeaderPairs &pairs);

  /**
   * Get a header map from the module's stream context after filter execution.
   * @param module_name Name of the module
   * @param type Header map type (RequestHeaders, ResponseHeaders, etc.)
   * @return Header key-value pairs (owned strings)
   */
  HeaderPairs getContextHeaders(const std::string &module_name,
                                proxy_wasm::WasmHeaderMapType type) const;

  /**
   * Unload a module
   * @param module_name Name of the module to unload
   * @return true if successfully unloaded
   */
  bool unloadModule(const std::string &module_name);

  /**
   * Get list of loaded modules
   * @return List of module names
   */
  std::vector<std::string> getLoadedModules() const;

  /**
   * Check if module is loaded
   * @param module_name Name of the module
   * @return true if module is loaded
   */
  bool hasModule(const std::string &module_name) const;

private:
  struct ModuleState {
    std::shared_ptr<lswasm::LsWasm> wasm;
    std::shared_ptr<proxy_wasm::PluginBase> plugin;
    // The stream context used for the current request
    lswasm::LsWasmContext *stream_context = nullptr;
    uint32_t stream_context_id = 0;
  };

  std::map<std::string, ModuleState> modules_;
  std::unordered_map<std::string, std::string> envs_;
};
