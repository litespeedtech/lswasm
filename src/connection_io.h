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
#include <string_view>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <cstring>

#include "log.h"

/**
 * ConnectionIO — bridge between a worker thread and the epoll event loop.
 *
 * The epoll loop owns ALL socket I/O.  Workers interact exclusively with
 * in-memory buffers via this class.
 *
 * Thread safety:
 *   - Worker calls: headers(), bodyPrefix(), contentLength(),
 *     readBodyChunk(), writeData(), finish()
 *   - Epoll-loop calls: setHeaderData(), feedBody(), pendingWriteData(),
 *     advanceWrite(), isWritePending(), isFinished(), needsWriteEvent()
 *   - Shared state is protected by read_mutex_ / write_mutex_.
 */
class ConnectionIO {
public:
    explicit ConnectionIO(int fd, int event_fd)
        : fd_(fd), event_fd_(event_fd) {}

    // Non-copyable, non-movable
    ConnectionIO(const ConnectionIO &) = delete;
    ConnectionIO &operator=(const ConnectionIO &) = delete;

    int fd() const { return fd_; }

    // ════════════════════════════════════════════════════════════════════
    //  Epoll-loop-side setup (called before dispatching to worker)
    // ════════════════════════════════════════════════════════════════════

    /// Set the raw header data and any body prefix bytes that arrived
    /// with the headers.  Also set the parsed Content-Length.
    void setHeaderData(std::string header_data, std::string body_prefix,
                       size_t content_length) {
        header_data_ = std::move(header_data);
        body_prefix_ = std::move(body_prefix);
        content_length_ = content_length;
        body_bytes_fed_ = body_prefix_.size();
    }

    // ════════════════════════════════════════════════════════════════════
    //  Worker-side API (blocking)
    // ════════════════════════════════════════════════════════════════════

    /// Return the raw header data (everything up to and including CRLFCRLF).
    const std::string &headers() const { return header_data_; }

    /// Return any body bytes that arrived with the header data.
    const std::string &bodyPrefix() const { return body_prefix_; }

    /// Return the Content-Length value (0 if none).
    size_t contentLength() const { return content_length_; }

    /// Read body data from the event loop.  Blocks until data is available
    /// or end-of-stream.  Returns empty string on EOF or error.
    std::string readBodyChunk(size_t max_chunk) {
        std::unique_lock<std::mutex> lock(read_mutex_);
        read_cv_.wait(lock, [this] {
            return read_data_ready_ || read_eof_ || read_error_;
        });
        if (read_error_) return {};
        if (read_chunk_.empty() && read_eof_) return {};

        std::string result;
        if (read_chunk_.size() <= max_chunk) {
            result = std::move(read_chunk_);
            read_chunk_.clear();
        } else {
            result = read_chunk_.substr(0, max_chunk);
            read_chunk_.erase(0, max_chunk);
        }
        if (read_chunk_.empty()) {
            read_data_ready_ = false;
        }
        return result;
    }

    /// Enqueue response data for the event loop to write.
    /// Blocks until the event loop has consumed all previously queued data.
    void writeData(const std::string &data) {
        if (data.empty()) return;
        std::unique_lock<std::mutex> lock(write_mutex_);
        // Wait until any previous write buffer has been fully consumed.
        write_cv_.wait(lock, [this] {
            return write_buffer_.empty() || write_error_;
        });
        if (write_error_) return;
        write_buffer_ = data;
        write_cursor_ = 0;
        write_pending_ = true;
        // Signal the event loop via eventfd.
        signal_eventfd();
    }

    /// Enqueue response data (move version).
    void writeData(std::string &&data) {
        if (data.empty()) return;
        std::unique_lock<std::mutex> lock(write_mutex_);
        write_cv_.wait(lock, [this] {
            return write_buffer_.empty() || write_error_;
        });
        if (write_error_) return;
        write_buffer_ = std::move(data);
        write_cursor_ = 0;
        write_pending_ = true;
        signal_eventfd();
    }

    /// Signal that the worker is done producing data.
    void finish() {
        std::lock_guard<std::mutex> lock(write_mutex_);
        finished_ = true;
        signal_eventfd();
    }

    /// Called by the worker to indicate an error (e.g. parse failure).
    /// The event loop will close the fd.
    void setError() {
        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            write_error_ = true;
            finished_ = true;
        }
        write_cv_.notify_all();
        signal_eventfd();
    }

    // ════════════════════════════════════════════════════════════════════
    //  Epoll-loop-side API (non-blocking)
    // ════════════════════════════════════════════════════════════════════

    /// Called when EPOLLIN fires and the connection is in the body-reading
    /// state.  Appends body bytes and wakes the worker if blocked.
    void feedBody(const char *data, size_t len, bool eof) {
        std::lock_guard<std::mutex> lock(read_mutex_);
        if (len > 0) {
            read_chunk_.append(data, len);
            body_bytes_fed_ += len;
            read_data_ready_ = true;
        }
        if (eof) {
            read_eof_ = true;
        }
        read_cv_.notify_one();
    }

    /// Called when an error occurs on the socket during body reading.
    void feedError() {
        std::lock_guard<std::mutex> lock(read_mutex_);
        read_error_ = true;
        read_cv_.notify_one();
    }

    /// Called when a write error occurs on the socket.
    void writeError() {
        std::lock_guard<std::mutex> lock(write_mutex_);
        write_error_ = true;
        write_buffer_.clear();
        write_cv_.notify_one();
    }

    /// Returns a view into the pending write data.  Empty if nothing pending.
    std::string_view pendingWriteData() {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (write_cursor_ >= write_buffer_.size()) return {};
        return std::string_view(write_buffer_).substr(write_cursor_);
    }

    /// Advance the write cursor by n bytes.  If the buffer is fully
    /// consumed, clears it and wakes the worker so it can produce more.
    void advanceWrite(size_t n) {
        std::lock_guard<std::mutex> lock(write_mutex_);
        write_cursor_ += n;
        if (write_cursor_ >= write_buffer_.size()) {
            write_buffer_.clear();
            write_cursor_ = 0;
            write_pending_ = false;
            write_cv_.notify_one();
        }
    }

    /// Check if the worker has new data to write (for eventfd handler).
    bool isWritePending() {
        std::lock_guard<std::mutex> lock(write_mutex_);
        return write_pending_;
    }

    /// Check if the worker has finished AND all write data has been consumed.
    bool isFinished() {
        std::lock_guard<std::mutex> lock(write_mutex_);
        return finished_ && write_buffer_.empty();
    }

    /// Check if the worker signalled an error.
    bool hasError() {
        std::lock_guard<std::mutex> lock(write_mutex_);
        return write_error_;
    }

    /// Check if the connection still needs body data from the socket.
    bool needsMoreBody() {
        std::lock_guard<std::mutex> lock(read_mutex_);
        return body_bytes_fed_ < content_length_ && !read_eof_ && !read_error_;
    }

    /// Total body bytes received so far (prefix + fed).
    size_t bodyBytesReceived() {
        std::lock_guard<std::mutex> lock(read_mutex_);
        return body_bytes_fed_;
    }

private:
    void signal_eventfd() {
        uint64_t val = 1;
        // Best-effort write — if it fails (e.g. would-block), the event
        // loop will pick up the pending state on the next iteration anyway.
        ssize_t r = ::write(event_fd_, &val, sizeof(val));
        (void)r;
    }

    int fd_;
    int event_fd_;

    // ── Header data (immutable after setHeaderData) ──
    std::string header_data_;
    std::string body_prefix_;
    size_t content_length_ = 0;

    // ── Read side (epoll feeds, worker consumes) ──
    std::string read_chunk_;
    size_t body_bytes_fed_ = 0;
    bool read_data_ready_ = false;
    bool read_eof_ = false;
    bool read_error_ = false;
    std::mutex read_mutex_;
    std::condition_variable read_cv_;

    // ── Write side (worker produces, epoll drains) ──
    std::string write_buffer_;
    size_t write_cursor_ = 0;
    bool write_pending_ = false;
    bool finished_ = false;
    bool write_error_ = false;
    std::mutex write_mutex_;
    std::condition_variable write_cv_;
};
