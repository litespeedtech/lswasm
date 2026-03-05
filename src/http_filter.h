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
#include <vector>
#include <map>
#include <memory>
#include <iostream>
#include <utility>

#include "wasm_module_manager.h"  // need full definition for callbacks
#include "log.h"

// Global module manager instance (defined in main.cpp)
extern std::unique_ptr<WasmModuleManager> g_module_manager;

// HeaderPairs is defined in wasm_module_manager.h

// HTTP Request/Response data structure
struct HttpData {
  std::string method;
  std::string path;
  std::string version;
  HeaderPairs request_headers;
  HeaderPairs response_headers;
  std::string request_body;
  std::string response_body;

  // Populated by WASM filter via sendLocalResponse.
  bool has_local_response = false;
  uint32_t local_response_code = 0;
  std::string local_response_body;
  HeaderPairs local_response_additional_headers;
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
    LOG_INFO("[Filter] Context created (ID: " << context_id_ << ")");
  }

  void onDelete() {
    LOG_INFO("[Filter] Context deleted (ID: " << context_id_ << ")");
  }

  // HTTP stream lifecycle hooks (filter callback points)
  
  // NOTE on has_local_response checks:
  //   onRequestHeaders is the only phase that can *set* has_local_response
  //   (via checkLocalResponse after executeFilter).  All other phases check
  //   the flag *before* calling executeFilter so they short-circuit the
  //   entire chain immediately if a prior phase already produced a local
  //   response.
  void onRequestHeaders() {
    LOG_INFO("[Filter] onRequestHeaders called (context_id: " << context_id_ << ")");

    // Proxy-wasm filters expect HTTP/2-style pseudo-headers in the request
    // header map.  Synthesize them from the parsed HTTP/1.1 request line
    // before passing headers to WASM modules.
    synthesizePseudoHeaders();

    if (g_module_manager) {
      for (const std::string &m : g_module_manager->getLoadedModules()) {
        // Ensure the stream context exists *before* setting headers.
        // executeFilter() used to lazily create the context, which meant
        // setContextHeaders() found no stream_context and silently dropped
        // the headers.
        g_module_manager->ensureStreamContext(m, context_id_);
        // Push request headers into the WASM context before execution.
        g_module_manager->setContextHeaders(
            m, proxy_wasm::WasmHeaderMapType::RequestHeaders, http_data_->request_headers);
        g_module_manager->executeFilter(m, context_id_, "onRequestHeaders");
        // Pull back any modifications the WASM module made to request headers.
        http_data_->request_headers = g_module_manager->getContextHeaders(
            m, proxy_wasm::WasmHeaderMapType::RequestHeaders);
        // Check if the WASM module sent a local response.
        checkLocalResponse(m);
        if (http_data_->has_local_response) break;  // Stop filter chain
      }
    }
  }

  void onRequestBody() {
    LOG_INFO("[Filter] onRequestBody called (context_id: " << context_id_ << ")");
    if (g_module_manager) {
      for (const std::string &m : g_module_manager->getLoadedModules()) {
        if (http_data_->has_local_response) break;
        g_module_manager->executeFilter(m, context_id_, "onRequestBody");
      }
    }
  }

  void onRequestTrailers() {
    LOG_INFO("[Filter] onRequestTrailers called (context_id: " << context_id_ << ")");
    if (g_module_manager) {
      for (const std::string &m : g_module_manager->getLoadedModules()) {
        if (http_data_->has_local_response) break;
        g_module_manager->executeFilter(m, context_id_, "onRequestTrailers");
      }
    }
  }

  void onResponseHeaders() {
    LOG_INFO("[Filter] onResponseHeaders called (context_id: " << context_id_ << ")");
    if (g_module_manager) {
      for (const std::string &m : g_module_manager->getLoadedModules()) {
        if (http_data_->has_local_response) break;
        // Push response headers into the WASM context before execution.
        g_module_manager->setContextHeaders(
            m, proxy_wasm::WasmHeaderMapType::ResponseHeaders, http_data_->response_headers);
        g_module_manager->executeFilter(m, context_id_, "onResponseHeaders");
        // Pull back any modifications the WASM module made to response headers.
        http_data_->response_headers = g_module_manager->getContextHeaders(
            m, proxy_wasm::WasmHeaderMapType::ResponseHeaders);
      }
    }
  }

  void onResponseBody() {
    LOG_INFO("[Filter] onResponseBody called (context_id: " << context_id_ << ")");
    if (g_module_manager) {
      for (const std::string &m : g_module_manager->getLoadedModules()) {
        if (http_data_->has_local_response) break;
        g_module_manager->executeFilter(m, context_id_, "onResponseBody");
      }
    }
  }

  void onResponseTrailers() {
    LOG_INFO("[Filter] onResponseTrailers called (context_id: " << context_id_ << ")");
    if (g_module_manager) {
      for (const std::string &m : g_module_manager->getLoadedModules()) {
        if (http_data_->has_local_response) break;
        g_module_manager->executeFilter(m, context_id_, "onResponseTrailers");
      }
    }
  }

  void onDone() {
    LOG_INFO("[Filter] Stream processing complete (context_id: " << context_id_ << ")");
    if (g_module_manager) {
      for (const std::string &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onDone");
      }
    }
  }

  // Metadata handling
  void onRequestMetadata() {
    LOG_INFO("[Filter] onRequestMetadata called (context_id: " << context_id_ << ")");
    if (g_module_manager) {
      for (const std::string &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onRequestMetadata");
      }
    }
  }

  void onResponseMetadata() {
    LOG_INFO("[Filter] onResponseMetadata called (context_id: " << context_id_ << ")");
    if (g_module_manager) {
      for (const std::string &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onResponseMetadata");
      }
    }
  }

  // Connection events
  void onNewConnection() {
    LOG_INFO("[Filter] New connection (context_id: " << context_id_ << ")");
    if (g_module_manager) {
      for (const std::string &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onNewConnection");
      }
    }
  }

  void onDownstreamConnectionClose() {
    LOG_INFO("[Filter] Downstream connection closed (context_id: " << context_id_ << ")");
    if (g_module_manager) {
      for (const std::string &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onDownstreamConnectionClose");
      }
    }
  }

  void onUpstreamConnectionClose() {
    LOG_INFO("[Filter] Upstream connection closed (context_id: " << context_id_ << ")");
    if (g_module_manager) {
      for (const std::string &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onUpstreamConnectionClose");
      }
    }
  }

  // Data events
  void onDownstreamData() {
    LOG_INFO("[Filter] Downstream data (context_id: " << context_id_ << ")");
    if (g_module_manager) {
      for (const std::string &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onDownstreamData");
      }
    }
  }

  void onUpstreamData() {
    LOG_INFO("[Filter] Upstream data (context_id: " << context_id_ << ")");
    if (g_module_manager) {
      for (const std::string &m : g_module_manager->getLoadedModules()) {
        g_module_manager->executeFilter(m, context_id_, "onUpstreamData");
      }
    }
  }

  // Accessors
  uint32_t getContextId() const { return context_id_; }
  HttpData *getHttpData() { return http_data_; }
  const HttpData *getHttpData() const { return http_data_; }

private:
  // Synthesize HTTP/2-style pseudo-headers that proxy-wasm filters expect.
  // These are derived from the HTTP/1.1 request line (method, path, version)
  // and the Host header.  They are prepended to the request header list so
  // the filter sees them via proxy_get_http_request_header(":method") etc.
  void synthesizePseudoHeaders() {
    HeaderPairs &hdrs = http_data_->request_headers;

    // Only add pseudo-headers if they are not already present.
    auto has = [&hdrs](const char *name) -> bool {
      for (const auto &p : hdrs) {
        if (header_name_eq(p.first, name)) return true;
      }
      return false;
    };

    // Build the list of pseudo-headers to prepend (in the canonical order).
    HeaderPairs pseudo;

    if (!has(":method") && !http_data_->method.empty()) {
      pseudo.emplace_back(":method", http_data_->method);
    }
    if (!has(":path") && !http_data_->path.empty()) {
      pseudo.emplace_back(":path", http_data_->path);
    }
    if (!has(":scheme")) {
      // lswasm always serves plain HTTP (no TLS termination).
      pseudo.emplace_back(":scheme", "http");
    }
    if (!has(":authority")) {
      // Derive :authority from the Host header, falling back to "localhost".
      std::string authority = "localhost";
      for (const auto &p : hdrs) {
        if (header_name_eq(p.first, "Host")) {
          authority = p.second;
          break;
        }
      }
      pseudo.emplace_back(":authority", authority);
    }

    if (!pseudo.empty()) {
      // Prepend pseudo-headers before the real headers.
      pseudo.insert(pseudo.end(), hdrs.begin(), hdrs.end());
      hdrs = std::move(pseudo);
    }
  }

  void checkLocalResponse(const std::string &module_name) {
    if (g_module_manager && g_module_manager->hasLocalResponse(module_name)) {
      http_data_->has_local_response = true;
      http_data_->local_response_code = g_module_manager->getLocalResponseCode(module_name);
      http_data_->local_response_body = g_module_manager->getLocalResponseBody(module_name);
      http_data_->local_response_additional_headers =
          g_module_manager->getLocalResponseHeaders(module_name);
      LOG_INFO("[Filter] WASM module '" << module_name
                << "' sent local response (code=" << http_data_->local_response_code
                << ", body_size=" << http_data_->local_response_body.size()
                << ", additional_headers=" << http_data_->local_response_additional_headers.size()
                << ")");
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
    LOG_INFO("[Filter] Plugin configured (config size: " << configuration_size << ")");
  }

  void onStart(size_t vm_configuration_size) {
    LOG_INFO("[Filter] Plugin started (VM config size: " << vm_configuration_size << ")");
  }

  void validateConfiguration(size_t configuration_size) {
    LOG_INFO("[Filter] Validating configuration (size: " << configuration_size << ")");
  }

  void onTick() {
    LOG_INFO("[Filter] Tick event");
  }

  void onQueueReady(uint32_t token) {
    LOG_INFO("[Filter] Queue ready (token: " << token << ")");
  }

  void onDone() {
    LOG_INFO("[Filter] Plugin done");
  }

  // Accessors
  const std::string &getPluginName() const { return plugin_name_; }

private:
  std::string plugin_name_;
};
