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

// LSAPI C library
extern "C" {
#include "lsapilib.h"
}

/**
 * LsapiResponseSink — ResponseSink backed by an LSAPI_Request.
 *
 * Translates the transport-abstract ResponseSink API into LSAPI protocol
 * calls.  Unlike the HTTP sink, LSAPI handles its own protocol framing —
 * there is no chunked transfer encoding.  Headers and body bytes are
 * passed through via LSAPI_SetRespStatus_r(), LSAPI_AppendRespHeader2_r(),
 * LSAPI_FinalizeRespHeaders_r(), and LSAPI_Write_r().
 */
class LsapiResponseSink : public ResponseSink {
public:
    /// Construct with a pointer to the LSAPI request object.
    /// The caller must ensure the LSAPI_Request outlives this sink.
    explicit LsapiResponseSink(LSAPI_Request *req) : req_(req) {}

    bool sendHeaders(uint32_t status_code, const HeaderPairs &headers,
                     bool /*streaming*/) override {
        if (!req_) {
            LOG_ERROR("[LsapiResponseSink] no LSAPI_Request");
            error_ = true;
            return false;
        }

        // Set the HTTP status code.
        LSAPI_SetRespStatus_r(req_, static_cast<int>(status_code));

        // Append each header.  Skip Transfer-Encoding: chunked because
        // LSAPI manages its own framing.
        for (const auto &hdr : headers) {
            if (header_name_eq(hdr.first, "Transfer-Encoding")) {
                continue;  // LSAPI handles framing internally.
            }
            if (LSAPI_AppendRespHeader2_r(req_,
                    hdr.first.c_str(), hdr.second.c_str()) < 0) {
                LOG_ERROR("[LsapiResponseSink] LSAPI_AppendRespHeader2_r failed "
                          "for header '" << hdr.first << "'");
                error_ = true;
                return false;
            }
        }

        // Finalize response headers — tells LSAPI the header block is
        // complete and body data will follow.
        if (LSAPI_FinalizeRespHeaders_r(req_) < 0) {
            LOG_ERROR("[LsapiResponseSink] LSAPI_FinalizeRespHeaders_r failed");
            error_ = true;
            return false;
        }

        return true;
    }

    bool writeBody(std::string_view data) override {
        if (!req_ || error_) return false;
        if (data.empty()) return true;

        ssize_t written = LSAPI_Write_r(req_, data.data(),
                                        static_cast<size_t>(data.size()));
        if (written < 0) {
            LOG_ERROR("[LsapiResponseSink] LSAPI_Write_r failed");
            error_ = true;
            return false;
        }
        return true;
    }

    bool finishBody() override {
        if (!req_ || error_) return false;
        // Flush any buffered data.
        if (LSAPI_Flush_r(req_) < 0) {
            LOG_ERROR("[LsapiResponseSink] LSAPI_Flush_r failed");
            error_ = true;
            return false;
        }
        return true;
    }

    bool hasError() const override { return error_; }

private:
    LSAPI_Request *req_ = nullptr;
    bool error_ = false;
};
