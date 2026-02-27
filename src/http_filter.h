#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <iostream>

#include "wasm_module_manager.h"  // need full definition for callbacks

// Global module manager instance (defined in main.cpp)
extern std::unique_ptr<WasmModuleManager> g_module_manager;

// HTTP Request/Response data structure
struct HttpData {
  std::string method;
  std::string path;
  std::string version;
  std::map<std::string, std::string> request_headers;
  std::map<std::string, std::string> response_headers;
  std::string request_body;
  std::string response_body;

  // Populated by WASM filter via sendLocalResponse.
  bool has_local_response = false;
  uint32_t local_response_code = 0;
  std::string local_response_body;
};

/**
 * HttpFilterContext - Represents an HTTP filter processing context
 * This is the host-side equivalent that manages filter execution
 */
class HttpFilterContext {
public:
  HttpFilterContext(uint32_t context_id, HttpData *http_data)
      : context_id_(context_id), http_data_(http_data) {}

  // note: global module manager is declared externally (see below)

  ~HttpFilterContext() = default;

  // Lifecycle callbacks
  void onCreate() {
    std::cout << "[Filter] Context created (ID: " << context_id_ << ")" << std::endl;
  }

  void onDelete() {
    std::cout << "[Filter] Context deleted (ID: " << context_id_ << ")" << std::endl;
  }

  // HTTP stream lifecycle hooks (filter callback points)
  
  void onRequestHeaders() {
    std::cout << "[Filter] onRequestHeaders called (context_id: " << context_id_ << ")" << std::endl;
    if (g_module_manager) {
      for (auto &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onRequestHeaders");
        // Check if the WASM module sent a local response.
        checkLocalResponse(m);
      }
    }
  }

  void onRequestBody() {
    std::cout << "[Filter] onRequestBody called (context_id: " << context_id_ << ")" << std::endl;
    if (g_module_manager) {
      for (auto &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onRequestBody");
      }
    }
  }

  void onRequestTrailers() {
    std::cout << "[Filter] onRequestTrailers called (context_id: " << context_id_ << ")" << std::endl;
    if (g_module_manager) {
      for (auto &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onRequestTrailers");
      }
    }
  }

  void onResponseHeaders() {
    std::cout << "[Filter] onResponseHeaders called (context_id: " << context_id_ << ")" << std::endl;
    if (g_module_manager) {
      for (auto &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onResponseHeaders");
      }
    }
  }

  void onResponseBody() {
    std::cout << "[Filter] onResponseBody called (context_id: " << context_id_ << ")" << std::endl;
    if (g_module_manager) {
      for (auto &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onResponseBody");
      }
    }
  }

  void onResponseTrailers() {
    std::cout << "[Filter] onResponseTrailers called (context_id: " << context_id_ << ")" << std::endl;
    if (g_module_manager) {
      for (auto &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onResponseTrailers");
      }
    }
  }

  void onDone() {
    std::cout << "[Filter] Stream processing complete (context_id: " << context_id_ << ")" << std::endl;
    if (g_module_manager) {
      for (auto &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onDone");
      }
    }
  }

  // Metadata handling
  void onRequestMetadata() {
    std::cout << "[Filter] onRequestMetadata called (context_id: " << context_id_ << ")" << std::endl;
    if (g_module_manager) {
      for (auto &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onRequestMetadata");
      }
    }
  }

  void onResponseMetadata() {
    std::cout << "[Filter] onResponseMetadata called (context_id: " << context_id_ << ")" << std::endl;
    if (g_module_manager) {
      for (auto &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onResponseMetadata");
      }
    }
  }

  // Connection events
  void onNewConnection() {
    std::cout << "[Filter] New connection (context_id: " << context_id_ << ")" << std::endl;
    if (g_module_manager) {
      for (auto &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onNewConnection");
      }
    }
  }

  void onDownstreamConnectionClose() {
    std::cout << "[Filter] Downstream connection closed (context_id: " << context_id_ << ")" << std::endl;
    if (g_module_manager) {
      for (auto &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onDownstreamConnectionClose");
      }
    }
  }

  void onUpstreamConnectionClose() {
    std::cout << "[Filter] Upstream connection closed (context_id: " << context_id_ << ")" << std::endl;
    if (g_module_manager) {
      for (auto &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onUpstreamConnectionClose");
      }
    }
  }

  // Data events
  void onDownstreamData() {
    std::cout << "[Filter] Downstream data (context_id: " << context_id_ << ")" << std::endl;
    if (g_module_manager) {
      for (auto &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onDownstreamData");
      }
    }
  }

  void onUpstreamData() {
    std::cout << "[Filter] Upstream data (context_id: " << context_id_ << ")" << std::endl;
    if (g_module_manager) {
      for (auto &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onUpstreamData");
      }
    }
  }

  // Accessors
  uint32_t getContextId() const { return context_id_; }
  HttpData *getHttpData() { return http_data_; }
  const HttpData *getHttpData() const { return http_data_; }

private:
  void checkLocalResponse(const std::string &module_name) {
    if (g_module_manager && g_module_manager->hasLocalResponse(module_name)) {
      http_data_->has_local_response = true;
      http_data_->local_response_code = g_module_manager->getLocalResponseCode(module_name);
      http_data_->local_response_body = g_module_manager->getLocalResponseBody(module_name);
      std::cout << "[Filter] WASM module '" << module_name
                << "' sent local response (code=" << http_data_->local_response_code
                << ", body_size=" << http_data_->local_response_body.size() << ")" << std::endl;
    }
  }

  uint32_t context_id_;
  HttpData *http_data_;
};

/**
 * RootHttpFilterContext - Root context for HTTP filtering
 * Handles plugin-level initialization and configuration
 */
class RootHttpFilterContext {
public:
  explicit RootHttpFilterContext(const std::string &plugin_name)
      : plugin_name_(plugin_name) {}

  ~RootHttpFilterContext() = default;

  // Plugin lifecycle
  void onConfigure(size_t configuration_size) {
    std::cout << "[Filter] Plugin configured (config size: " << configuration_size << ")" << std::endl;
  }

  void onStart(size_t vm_configuration_size) {
    std::cout << "[Filter] Plugin started (VM config size: " << vm_configuration_size << ")" << std::endl;
  }

  void validateConfiguration(size_t configuration_size) {
    std::cout << "[Filter] Validating configuration (size: " << configuration_size << ")" << std::endl;
  }

  void onTick() {
    std::cout << "[Filter] Tick event" << std::endl;
  }

  void onQueueReady(uint32_t token) {
    std::cout << "[Filter] Queue ready (token: " << token << ")" << std::endl;
  }

  void onDone() {
    std::cout << "[Filter] Plugin done" << std::endl;
  }

  // Accessors
  const std::string &getPluginName() const { return plugin_name_; }

private:
  std::string plugin_name_;
};
