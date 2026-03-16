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

/**
 * lswasm_streaming.h — Filter-side convenience wrappers for the lswasm
 *                      streaming response API.
 *
 * Include this header in your proxy-wasm C++ filter to access:
 *
 *   lswasm::streaming::sendResponseHeaders(status, headers)
 *   lswasm::streaming::writeResponseChunk(data, size)
 *   lswasm::streaming::finishResponse()
 *   lswasm::streaming::isSupported()
 *
 * These wrap proxy_call_foreign_function with the three lswasm-specific
 * foreign function names:
 *
 *   lswasm_send_response_headers
 *   lswasm_write_response_chunk
 *   lswasm_finish_response
 *
 * The argument encoding matches the proxy-wasm pairs format used by
 * marshalPairs() in the C++ SDK, prefixed with a uint32_t status code
 * for the headers call.
 *
 * Backward compatibility:
 *   On hosts that do not implement the streaming API,
 *   proxy_call_foreign_function returns WasmResult::NotFound (1).
 *   Use isSupported() to detect this.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// proxy-wasm SDK types
#include "proxy_wasm_externs.h"

namespace lswasm {
namespace streaming {

using HeaderList = std::vector<std::pair<std::string, std::string>>;

// ── Internal helpers ────────────────────────────────────────────────

namespace detail {

/// Calculate the marshalled byte size for a list of header pairs.
inline size_t pairsSize(const HeaderList &pairs) {
  size_t size = 4;  // uint32_t num_pairs
  for (const auto &p : pairs) {
    size += 8;                    // uint32_t key_size + uint32_t value_size
    size += p.first.size() + 1;   // key + NUL
    size += p.second.size() + 1;  // value + NUL
  }
  return size;
}

/// Marshal header pairs into the proxy-wasm pairs wire format.
inline void marshalPairs(const HeaderList &pairs, char *buffer) {
  char *b = buffer;
  uint32_t n = static_cast<uint32_t>(pairs.size());
  std::memcpy(b, &n, 4);
  b += 4;
  for (const auto &p : pairs) {
    uint32_t ks = static_cast<uint32_t>(p.first.size());
    uint32_t vs = static_cast<uint32_t>(p.second.size());
    std::memcpy(b, &ks, 4); b += 4;
    std::memcpy(b, &vs, 4); b += 4;
  }
  for (const auto &p : pairs) {
    std::memcpy(b, p.first.data(), p.first.size());
    b += p.first.size();
    *b++ = '\0';
    std::memcpy(b, p.second.data(), p.second.size());
    b += p.second.size();
    *b++ = '\0';
  }
}

/// Call a foreign function by name; returns the WasmResult code.
inline WasmResult callForeign(const char *name, size_t name_len,
                               const char *arg, size_t arg_len) {
  char *result = nullptr;
  size_t result_size = 0;
  return proxy_call_foreign_function(name, name_len, arg, arg_len,
                                     &result, &result_size);
}

} // namespace detail

// ── Public API ──────────────────────────────────────────────────────

/**
 * Send HTTP response headers to the client.
 *
 * @param status_code  HTTP status code (e.g. 200).
 * @param headers      Response headers.
 * @return WasmResult::Ok on success; WasmResult::NotFound if the host
 *         does not support streaming.
 */
inline WasmResult sendResponseHeaders(uint32_t status_code,
                                       const HeaderList &headers) {
  // Build argument: 4-byte status_code + marshalled pairs.
  size_t pairs_size = detail::pairsSize(headers);
  std::string arg(4 + pairs_size, '\0');
  std::memcpy(&arg[0], &status_code, 4);
  detail::marshalPairs(headers, &arg[4]);

  static const char name[] = "lswasm_send_response_headers";
  return detail::callForeign(name, sizeof(name) - 1, arg.data(), arg.size());
}

/**
 * Write a chunk of response body data.
 *
 * @param data  Pointer to body bytes.
 * @param size  Number of bytes.
 * @return WasmResult::Ok on success.
 */
inline WasmResult writeResponseChunk(const char *data, size_t size) {
  static const char name[] = "lswasm_write_response_chunk";
  return detail::callForeign(name, sizeof(name) - 1, data, size);
}

/**
 * Signal that the response is complete.
 * No more chunks may be written after this call.
 *
 * @return WasmResult::Ok on success.
 */
inline WasmResult finishResponse() {
  static const char name[] = "lswasm_finish_response";
  return detail::callForeign(name, sizeof(name) - 1, nullptr, 0);
}

/**
 * Detect whether the host supports the streaming response API.
 *
 * This probes lswasm_send_response_headers with an intentionally
 * invalid (empty) argument. If the host recognises the function name
 * it should return BadArgument (or Ok on permissive implementations);
 * if it doesn't, it returns NotFound.
 *
 * IMPORTANT: Call this *before* sendResponseHeaders(), since it does
 * NOT actually start a streaming response.
 */
inline bool isSupported() {
  static const char name[] = "lswasm_send_response_headers";
  char *result = nullptr;
  size_t result_size = 0;
  WasmResult rc = proxy_call_foreign_function(
      name, sizeof(name) - 1, nullptr, 0, &result, &result_size);
  return rc == WasmResult::Ok || rc == WasmResult::BadArgument;
}

} // namespace streaming
} // namespace lswasm
