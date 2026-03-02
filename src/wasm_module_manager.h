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
    for (const auto &h : additional_headers) {
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
    auto it = header_maps_.find(type);
    if (it != header_maps_.end()) {
      for (const auto &p : it->second) {
        result->emplace_back(std::string_view(p.first), std::string_view(p.second));
      }
    }
    return proxy_wasm::WasmResult::Ok;
  }

  proxy_wasm::WasmResult setHeaderMapPairs(proxy_wasm::WasmHeaderMapType type,
                                           const proxy_wasm::Pairs &pairs) override {
    auto &owned = header_maps_[type];
    owned.clear();
    for (const auto &p : pairs) {
      owned.emplace_back(std::string(p.first), std::string(p.second));
    }
    return proxy_wasm::WasmResult::Ok;
  }

  proxy_wasm::WasmResult getHeaderMapValue(proxy_wasm::WasmHeaderMapType type,
                                           std::string_view key,
                                           std::string_view *result) override {
    auto it = header_maps_.find(type);
    if (it != header_maps_.end()) {
      for (const auto &pair : it->second) {
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
    auto &pairs = header_maps_[type];
    // Remove all existing occurrences of this key (handles multi-value headers).
    pairs.erase(std::remove_if(pairs.begin(), pairs.end(),
                               [&key](const auto &p) { return header_name_eq(p.first, key); }),
                pairs.end());
    // Add a single replacement value.
    pairs.emplace_back(std::string(key), std::string(value));
    return proxy_wasm::WasmResult::Ok;
  }

  proxy_wasm::WasmResult removeHeaderMapValue(proxy_wasm::WasmHeaderMapType type,
                                              std::string_view key) override {
    auto it = header_maps_.find(type);
    if (it != header_maps_.end()) {
      auto &pairs = it->second;
      pairs.erase(std::remove_if(pairs.begin(), pairs.end(),
                                 [&key](const auto &p) { return header_name_eq(p.first, key); }),
                  pairs.end());
    }
    return proxy_wasm::WasmResult::Ok;
  }

  proxy_wasm::WasmResult getHeaderMapSize(proxy_wasm::WasmHeaderMapType type,
                                          uint32_t *result) override {
    auto it = header_maps_.find(type);
    if (it != header_maps_.end()) {
      // Size in bytes: sum of key + value lengths + 4 bytes per pair for NULs.
      uint32_t size = 0;
      for (const auto &pair : it->second) {
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
    auto it = header_maps_.find(type);
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
