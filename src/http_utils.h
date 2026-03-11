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

#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Owned header pairs type (std::string, not string_view).
using HeaderPairs = std::vector<std::pair<std::string, std::string>>;

namespace http_utils {

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
inline std::string serialize_headers(uint32_t status_code,
                                     const HeaderPairs &headers) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status_code << " "
             << reason_phrase(status_code) << "\r\n";
    for (const auto &[key, value] : headers) {
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
inline void deserialize_header_pairs(std::string_view data, HeaderPairs &out) {
    out.clear();
    if (data.size() < 4) return;

    uint32_t n;
    std::memcpy(&n, data.data(), 4);
    const char *p = data.data() + 4;
    const char *end = data.data() + data.size();

    // Validate we have enough bytes for the size table.
    if (p + static_cast<size_t>(n) * 8 > end) return;

    // Read sizes.
    struct SizePair { uint32_t key_size; uint32_t value_size; };
    std::vector<SizePair> sizes(n);
    for (uint32_t i = 0; i < n; ++i) {
        std::memcpy(&sizes[i].key_size, p, 4);   p += 4;
        std::memcpy(&sizes[i].value_size, p, 4);  p += 4;
    }

    // Read key/value data.
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (p + sizes[i].key_size + 1 > end) return;
        std::string key(p, sizes[i].key_size);
        p += sizes[i].key_size + 1;  // skip NUL

        if (p + sizes[i].value_size + 1 > end) return;
        std::string value(p, sizes[i].value_size);
        p += sizes[i].value_size + 1;  // skip NUL

        out.emplace_back(std::move(key), std::move(value));
    }
}

} // namespace http_utils
