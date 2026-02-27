#pragma once

#include <string>
#include <memory>
#include <map>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <chrono>

#include "proxy-wasm/wasm_vm.h"
#include "proxy-wasm/wasm.h"
#include "proxy-wasm/context.h"

#ifdef ENABLE_WASMTIME
#include "proxy-wasm/wasmtime.h"
#endif

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
    std::cerr << "[WASM VM Error] " << message << std::endl;
  }

  void trace(std::string_view message) override {
    std::cout << "[WASM VM Trace] " << message << std::endl;
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
    std::cout << "[WASM log L" << level << "] " << message << std::endl;
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
    has_local_response_ = true;
    std::cout << "[WASM] sendLocalResponse: code=" << response_code
              << " body_size=" << body.size() << std::endl;
    return proxy_wasm::WasmResult::Ok;
  }

  // Accessors for captured local response.
  bool hasLocalResponse() const { return has_local_response_; }
  uint32_t localResponseCode() const { return local_response_code_; }
  const std::string &localResponseBody() const { return local_response_body_; }
  const std::string &logOutput() const { return log_; }

  void resetLocalResponse() {
    has_local_response_ = false;
    local_response_code_ = 0;
    local_response_body_.clear();
    local_response_details_.clear();
  }

private:
  std::string log_;
  bool has_local_response_ = false;
  uint32_t local_response_code_ = 0;
  std::string local_response_body_;
  std::string local_response_details_;
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
