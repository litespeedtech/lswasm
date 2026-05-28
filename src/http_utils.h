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

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

namespace http_utils {

// Hard caps for proxy-wasm pairs deserialization.  These bound resource use
// when a (potentially compromised or buggy) WASM filter sends marshalled
// header pairs through the streaming foreign functions.
inline constexpr uint32_t kMaxHeaderPairs = 1024;
inline constexpr size_t kMaxHeaderPairsBytes = static_cast<size_t>(1) << 20;  // 1 MiB

// True iff `name` is a non-empty RFC 7230 token (i.e., a valid header name).
inline bool is_valid_header_name_chars(std::string_view name) {
    if (name.empty()) return false;
    for (unsigned char c : name) {
        // Reject CTLs, whitespace, separators, and obviously-bad bytes.
        // Token = 1*tchar (RFC 7230 §3.2.6).
        if (c <= 0x20 || c == 0x7f) return false;
        switch (c) {
        case '"': case '(': case ')': case ',': case '/':
        case ':': case ';': case '<': case '=': case '>':
        case '?': case '@': case '[': case '\\': case ']':
        case '{': case '}':
            return false;
        default:
            break;
        }
    }
    return true;
}

// True iff `value` contains no bytes that would terminate a header line
// (NUL/CR/LF), enabling response splitting.  Other CTLs are technically
// disallowed by RFC 7230 obs-text rules but are not a splitting hazard.
inline bool is_valid_header_value_chars(std::string_view value) {
    for (unsigned char c : value) {
        if (c == '\0' || c == '\r' || c == '\n') return false;
    }
    return true;
}

// Clamp a status code to a sensible HTTP range, used wherever a WASM filter
// or other untrusted source supplies a status_code.
inline uint32_t sanitize_status_code(uint32_t code) {
    if (code < 100 || code > 599) return 500;
    return code;
}

// Map common HTTP status codes to reason phrases.
inline const char *reason_phrase(uint32_t status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default:  return "";
    }
}

// Serialize HTTP/1.1 response status line + headers into a string.
// Does NOT include the response body.
//
// Header keys and values are filtered to remove any bytes that would
// terminate the header block (NUL, CR, LF).  These bytes can only appear
// when a WASM filter forwards untrusted input into a response header; left
// unchecked they enable HTTP response splitting / smuggling.  Invalid pairs
// are dropped from the serialized output rather than aborting the response,
// matching the behavior of other proxy-wasm hosts.
inline std::string serialize_headers(uint32_t status_code,
                                     const HeaderPairs &headers) {
    uint32_t code = sanitize_status_code(status_code);
    std::ostringstream response;
    response << "HTTP/1.1 " << code << " " << reason_phrase(code) << "\r\n";
    for (const auto &[key, value] : headers) {
        if (!is_valid_header_name_chars(key) ||
            !is_valid_header_value_chars(value)) {
            // Refuse to emit a header that would split the response.
            // Caller-side validation should have caught this; this is a
            // belt-and-braces filter at the serialization boundary.
            continue;
        }
        response << key << ": " << value << "\r\n";
    }
    response << "\r\n";
    return response.str();
}

// Deserialize proxy-wasm pairs format into HeaderPairs.
//
// Wire format (same as proxy_wasm_api.h marshalPairs):
//   4 bytes: uint32_t num_pairs
//   For each pair: uint32_t key_size, uint32_t value_size
//   For each pair: key bytes + NUL, value bytes + NUL
//
// All inputs come from a WASM module and must be treated as untrusted.  In
// addition to the standard truncation checks, this rejects:
//   - num_pairs above kMaxHeaderPairs (DoS via giant allocation),
//   - sum of key/value sizes above kMaxHeaderPairsBytes,
//   - sizes large enough to overflow when added to the cursor.
// Any validation failure leaves `out` empty so the caller does not act on
// a partial parse.
inline void deserialize_header_pairs(std::string_view data, HeaderPairs &out) {
    out.clear();
    if (data.size() < 4) return;

    uint32_t n;
    std::memcpy(&n, data.data(), 4);
    if (n > kMaxHeaderPairs) return;
    const char *p = data.data() + 4;
    const char *end = data.data() + data.size();

    // Validate we have enough bytes for the size table; check via subtraction
    // to avoid pointer arithmetic overflow.
    const size_t available_after_header = static_cast<size_t>(end - p);
    if (static_cast<uint64_t>(n) * 8u > available_after_header) return;

    // Read sizes.
    struct SizePair { uint32_t key_size; uint32_t value_size; };
    std::vector<SizePair> sizes(n);
    uint64_t declared_total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        std::memcpy(&sizes[i].key_size, p, 4);   p += 4;
        std::memcpy(&sizes[i].value_size, p, 4);  p += 4;
        declared_total += static_cast<uint64_t>(sizes[i].key_size) +
                          static_cast<uint64_t>(sizes[i].value_size) + 2u;
        if (declared_total > kMaxHeaderPairsBytes) {
            return;
        }
    }

    // Read key/value data.  Bounds checks below use subtraction so they are
    // safe regardless of attacker-chosen sizes.
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        size_t remaining = static_cast<size_t>(end - p);
        size_t need_key = static_cast<size_t>(sizes[i].key_size) + 1u;
        if (need_key > remaining) { out.clear(); return; }
        std::string key(p, sizes[i].key_size);
        p += need_key;  // skip key + NUL

        remaining = static_cast<size_t>(end - p);
        size_t need_val = static_cast<size_t>(sizes[i].value_size) + 1u;
        if (need_val > remaining) { out.clear(); return; }
        std::string value(p, sizes[i].value_size);
        p += need_val;  // skip value + NUL

        out.emplace_back(std::move(key), std::move(value));
    }
}

} // namespace http_utils
