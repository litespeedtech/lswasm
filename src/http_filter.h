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
 *
 * Thread safety: each HttpFilterContext instance is used by a single
 * thread (the worker handling the request).  The RequestScope creates
 * per-thread WASM VM clones via getOrCreateThreadLocalPlugin().
 *
 * Lifetime: WASM contexts are created once during onRequestHeaders()
 * and persist across all subsequent phases (onRequestBody, onRequestTrailers,
 * onResponseHeaders, etc.) until the HttpFilterContext is destroyed.
 * This allows stateful WASM filters to accumulate data across phases.
 */
class HttpFilterContext {
public:
  HttpFilterContext(uint32_t context_id, HttpData *http_data)
      : context_id_(context_id), http_data_(http_data) {}

  /// Inject the ConnectionIO for this request so that streaming response
  /// foreign-function handlers can write directly to the client socket.
  void setConnectionIO(ConnectionIO *conn) { conn_ = conn; }

  /// True if any WASM context in the filter chain started a streaming
  /// response (i.e. called lswasm_send_response_headers).
  bool hasStreamingResponse() const {
    for (const auto &[name, scope] : scopes_) {
      if (scope.context() && scope.context()->hasStreamingResponse())
        return true;
    }
    return false;
  }

  /// True if the streaming response has been finished (lswasm_finish_response
  /// was called).
  bool isStreamingFinished() const {
    for (const auto &[name, scope] : scopes_) {
      if (scope.context() && scope.context()->isStreamingFinished())
        return true;
    }
    return false;
  }

  // note: global module manager is declared externally (see below)

  ~HttpFilterContext() {
    // Scopes are destroyed here, which calls onDone()/onDelete() on each
    // WASM stream context via the RequestScope destructor.
    scopes_.clear();
    module_order_.clear();
  }

  // Non-copyable (owns RequestScopes).
  HttpFilterContext(const HttpFilterContext &) = delete;
  HttpFilterContext &operator=(const HttpFilterContext &) = delete;

  // Lifecycle callbacks
  void onCreate() {
    LOG_INFO("[Filter] Context created (ID: " << context_id_ << ")");
  }

  void onDelete() {
    LOG_INFO("[Filter] Context deleted (ID: " << context_id_ << ")");
  }

  // HTTP stream lifecycle hooks (filter callback points)

  // onRequestHeaders creates a RequestScope per loaded module and keeps it
  // alive for the duration of the request.  Subsequent phases reuse the
  // same WASM stream context so filter-level member variables persist.
  void onRequestHeaders(bool end_of_stream = true) {
    LOG_INFO("[Filter] onRequestHeaders called (context_id: " << context_id_
             << ", end_of_stream=" << end_of_stream << ")");

    // Proxy-wasm filters expect HTTP/2-style pseudo-headers in the request
    // header map.  Synthesize them from the parsed HTTP/1.1 request line
    // before passing headers to WASM modules.
    synthesizePseudoHeaders();

    if (g_module_manager) {
      for (const std::string &m : g_module_manager->getLoadedModules()) {
        WasmModuleManager::RequestScope scope;
        if (!g_module_manager->createRequestScope(m, context_id_, scope)) {
          LOG_ERROR("[Filter] Failed to create RequestScope for module '" << m << "'");
          continue;
        }
        // Inject ConnectionIO for streaming response support.
        // Thread safety: conn_ is set once here on the worker thread and
        // only used by this same worker thread during WASM callbacks.
        // ConnectionIO::writeData() has its own internal mutex protection
        // for the worker↔epoll boundary.
        scope.context()->setConnectionIO(conn_);
        // Push request headers into the WASM context before execution.
        scope.context()->setHeaderMap(
            proxy_wasm::WasmHeaderMapType::RequestHeaders, http_data_->request_headers);
        scope.context()->onRequestHeaders(0, end_of_stream);
        // Pull back any modifications the WASM module made to request headers.
        http_data_->request_headers = scope.context()->getHeaderMapOwned(
            proxy_wasm::WasmHeaderMapType::RequestHeaders);
        // Check if the WASM module sent a local response.
        checkLocalResponse(scope, m);

        // Store the scope for reuse in later phases.
        module_order_.push_back(m);
        scopes_.emplace(m, std::move(scope));

        if (http_data_->has_local_response) break;  // Stop filter chain
      }
    }
  }

  void onRequestBody(bool end_of_stream = true) {
    LOG_INFO("[Filter] onRequestBody called (context_id: " << context_id_
             << ", body_size=" << http_data_->request_body.size()
             << ", eos=" << end_of_stream << ")");
    for (const std::string &m : module_order_) {
      if (http_data_->has_local_response) break;
      auto it = scopes_.find(m);
      if (it == scopes_.end() || !it->second.valid()) continue;
      auto *ctx = it->second.context();
      // Set body buffer and end-of-stream flag on the WASM context
      // so that proxy_get_buffer_bytes(HttpRequestBody) returns the
      // current chunk data.
      ctx->setRequestBody(http_data_->request_body);
      ctx->setEndOfStream(end_of_stream);
      ctx->onRequestBody(http_data_->request_body.size(), end_of_stream);
      checkLocalResponse(it->second, m);
    }
  }

  void onRequestTrailers() {
    LOG_INFO("[Filter] onRequestTrailers called (context_id: " << context_id_ << ")");
    for (const std::string &m : module_order_) {
      if (http_data_->has_local_response) break;
      auto it = scopes_.find(m);
      if (it == scopes_.end() || !it->second.valid()) continue;
      it->second.context()->onRequestTrailers(0);
      checkLocalResponse(it->second, m);
    }
  }

  void onResponseHeaders() {
    LOG_INFO("[Filter] onResponseHeaders called (context_id: " << context_id_ << ")");
    for (const std::string &m : module_order_) {
      if (http_data_->has_local_response) break;
      auto it = scopes_.find(m);
      if (it == scopes_.end() || !it->second.valid()) continue;
      auto *ctx = it->second.context();
      // Push response headers into the WASM context before execution.
      ctx->setHeaderMap(
          proxy_wasm::WasmHeaderMapType::ResponseHeaders, http_data_->response_headers);
      ctx->onResponseHeaders(0, true);
      // Pull back any modifications the WASM module made to response headers.
      http_data_->response_headers = ctx->getHeaderMapOwned(
          proxy_wasm::WasmHeaderMapType::ResponseHeaders);
    }
  }

  void onResponseBody() {
    LOG_INFO("[Filter] onResponseBody called (context_id: " << context_id_ << ")");
    for (const std::string &m : module_order_) {
      if (http_data_->has_local_response) break;
      auto it = scopes_.find(m);
      if (it == scopes_.end() || !it->second.valid()) continue;
      it->second.context()->onResponseBody(0, true);
    }
  }

  void onResponseTrailers() {
    LOG_INFO("[Filter] onResponseTrailers called (context_id: " << context_id_ << ")");
    for (const std::string &m : module_order_) {
      if (http_data_->has_local_response) break;
      auto it = scopes_.find(m);
      if (it == scopes_.end() || !it->second.valid()) continue;
      it->second.context()->onResponseTrailers(0);
    }
  }

  void onDone() {
    LOG_INFO("[Filter] Stream processing complete (context_id: " << context_id_ << ")");
    // Scopes are cleaned up in the destructor, which calls onDone()/onDelete()
    // on each WASM stream context.
  }

  // Metadata handling
  void onRequestMetadata() {
    LOG_INFO("[Filter] onRequestMetadata called (context_id: " << context_id_ << ")");
    // No proxy-wasm ABI callback for metadata; reserved for future use.
  }

  void onResponseMetadata() {
    LOG_INFO("[Filter] onResponseMetadata called (context_id: " << context_id_ << ")");
    // No proxy-wasm ABI callback for metadata; reserved for future use.
  }

  // Connection events
  void onNewConnection() {
    LOG_INFO("[Filter] New connection (context_id: " << context_id_ << ")");
    // Connection-level events are not used in the per-request model.
  }

  void onDownstreamConnectionClose() {
    LOG_INFO("[Filter] Downstream connection closed (context_id: " << context_id_ << ")");
  }

  void onUpstreamConnectionClose() {
    LOG_INFO("[Filter] Upstream connection closed (context_id: " << context_id_ << ")");
  }

  // Data events
  void onDownstreamData() {
    LOG_INFO("[Filter] Downstream data (context_id: " << context_id_ << ")");
  }

  void onUpstreamData() {
    LOG_INFO("[Filter] Upstream data (context_id: " << context_id_ << ")");
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

  void checkLocalResponse(WasmModuleManager::RequestScope &scope,
                          const std::string &module_name) {
    if (scope.context() && scope.context()->hasLocalResponse()) {
      http_data_->has_local_response = true;
      http_data_->local_response_code = scope.context()->localResponseCode();
      http_data_->local_response_body = scope.context()->localResponseBody();
      http_data_->local_response_additional_headers = scope.context()->localResponseHeaders();
      LOG_INFO("[Filter] WASM module '" << module_name
                << "' sent local response (code=" << http_data_->local_response_code
                << ", body_size=" << http_data_->local_response_body.size()
                << ", additional_headers=" << http_data_->local_response_additional_headers.size()
                << ")");
    }
  }

  uint32_t context_id_;
  HttpData *http_data_;
  ConnectionIO *conn_ = nullptr;  // Injected for streaming response support.

  // Persistent WASM contexts — created once in onRequestHeaders(), reused
  // across all subsequent phases, destroyed in ~HttpFilterContext().
  std::vector<std::string> module_order_;
  std::map<std::string, WasmModuleManager::RequestScope> scopes_;
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
