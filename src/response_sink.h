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
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

#include "http_utils.h"
#include "log.h"

/**
 * ResponseSink — transport-abstract interface for writing HTTP responses.
 *
 * The host creates a concrete sink for each request and injects it into
 * the filter chain.  Streaming response foreign functions and the
 * non-streaming response path both write through this interface.
 *
 * Two concrete implementations exist:
 *   - HttpResponseSink  (wraps ConnectionIO for the epoll/HTTP path)
 *   - LsapiResponseSink (wraps LSAPI_Request* for the LSAPI path)
 *
 * Lifecycle:
 *   1. sendHeaders()  — exactly once
 *   2. writeBody()    — zero or more times
 *   3. finishBody()   — exactly once (sends chunked terminator for HTTP
 *                       streaming, no-op otherwise)
 *
 * Connection lifecycle (close, error signalling, fd cleanup) is NOT the
 * responsibility of the sink.  The caller manages that separately.
 */
class ResponseSink {
public:
    virtual ~ResponseSink() = default;

    /**
     * Send HTTP response status line and headers.
     *
     * @param status_code  HTTP status code (e.g. 200, 404).
     * @param headers      Response header pairs.
     * @param streaming    If true, the sink may adjust headers for streaming
     *                     (e.g. add Transfer-Encoding: chunked for HTTP).
     * @return true on success, false on error.
     */
    virtual bool sendHeaders(uint32_t status_code, const HeaderPairs &headers,
                             bool streaming) = 0;

    /**
     * Write response body bytes.  May be called zero or more times after
     * sendHeaders() and before finishBody().
     *
     * @param data  Body bytes to write.  May be empty (no-op).
     * @return true on success, false on error.
     */
    virtual bool writeBody(std::string_view data) = 0;

    /**
     * Signal end of body.  For HTTP streaming this sends the chunked
     * transfer-encoding terminator.  For other transports it may flush
     * or be a no-op.
     *
     * @return true on success, false on error.
     */
    virtual bool finishBody() = 0;

    /**
     * Check if the sink has encountered a transport-level error.
     */
    virtual bool hasError() const = 0;
};
