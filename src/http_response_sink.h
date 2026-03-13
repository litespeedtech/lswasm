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

#include "response_sink.h"
#include "connection_io.h"

/**
 * HttpResponseSink — ResponseSink backed by a ConnectionIO bridge.
 *
 * This is the concrete sink used by the epoll/HTTP transport path.
 * It serialises HTTP/1.1 status lines, headers, and (when streaming)
 * chunked transfer-encoding framing before writing through ConnectionIO.
 *
 * For the non-streaming path, headers are written with Content-Length and
 * the body is written as a flat byte stream.  For the streaming path,
 * headers are written with Transfer-Encoding: chunked and each writeBody()
 * call wraps the data in a chunked-encoding frame.
 */
class HttpResponseSink : public ResponseSink {
public:
    /// Construct with a non-owning pointer to the ConnectionIO bridge.
    /// The caller must ensure the ConnectionIO outlives this sink.
    explicit HttpResponseSink(ConnectionIO *conn) : conn_(conn) {}

    bool sendHeaders(uint32_t status_code, const HeaderPairs &headers,
                     bool streaming) override {
        if (!conn_) {
            LOG_ERROR("[HttpResponseSink] no ConnectionIO");
            error_ = true;
            return false;
        }
        streaming_ = streaming;

        if (streaming) {
            // For streaming responses: strip Content-Length, ensure
            // Transfer-Encoding: chunked is present.
            HeaderPairs normalized;
            normalized.reserve(headers.size() + 1);
            bool saw_chunked = false;
            for (const auto &hdr : headers) {
                if (header_name_eq(hdr.first, "Content-Length")) {
                    continue;  // Remove Content-Length for chunked streaming.
                }
                if (header_name_eq(hdr.first, "Transfer-Encoding")) {
                    std::string val_lower(hdr.second);
                    std::transform(val_lower.begin(), val_lower.end(),
                                   val_lower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    if (val_lower == "chunked") {
                        saw_chunked = true;
                        normalized.emplace_back(hdr.first, hdr.second);
                    } else {
                        LOG_ERROR("[HttpResponseSink] refusing conflicting "
                                  "Transfer-Encoding on chunked streaming response");
                        error_ = true;
                        return false;
                    }
                    continue;
                }
                normalized.emplace_back(hdr.first, hdr.second);
            }
            if (!saw_chunked) {
                normalized.emplace_back("Transfer-Encoding", "chunked");
            }
            std::string hdr_str = http_utils::serialize_headers(status_code,
                                                                 normalized);
            conn_->writeData(std::move(hdr_str));
        } else {
            // Non-streaming: headers are written as-is (caller manages
            // Content-Length).
            std::string hdr_str = http_utils::serialize_headers(status_code,
                                                                 headers);
            conn_->writeData(std::move(hdr_str));
        }
        return true;
    }

    bool writeBody(std::string_view data) override {
        if (!conn_ || error_) return false;
        if (data.empty()) return true;

        if (streaming_) {
            // Wrap data in HTTP/1.1 chunked transfer encoding:
            //   <hex-size>\r\n<data>\r\n
            char size_buf[24];
            int n = std::snprintf(size_buf, sizeof(size_buf), "%zx\r\n",
                                  data.size());
            std::string chunk;
            chunk.reserve(static_cast<size_t>(n) + data.size() + 2);
            chunk.append(size_buf, static_cast<size_t>(n));
            chunk.append(data.data(), data.size());
            chunk.append("\r\n");
            conn_->writeData(std::move(chunk));
        } else {
            // Non-streaming: write raw body bytes.
            conn_->writeData(std::string(data));
        }
        return true;
    }

    bool finishBody() override {
        if (!conn_ || error_) return false;
        if (streaming_) {
            // Send the chunked transfer encoding terminator: zero-length chunk.
            conn_->writeData(std::string("0\r\n\r\n"));
        }
        // Non-streaming: nothing to do — body is already complete.
        return true;
    }

    bool hasError() const override { return error_; }

private:
    ConnectionIO *conn_ = nullptr;
    bool streaming_ = false;
    bool error_ = false;
};
