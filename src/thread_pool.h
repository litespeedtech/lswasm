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

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

/**
 * ThreadPool — A simple fixed-size thread pool.
 *
 * Workers pull tasks from a shared queue and execute them.  submit() enqueues
 * a task and returns immediately.  shutdown() stops accepting new tasks,
 * drains all pending work, and joins every worker thread.
 */
class ThreadPool {
public:
    /**
     * Create a pool with \p num_threads worker threads.
     * If \p num_threads is 0, defaults to std::thread::hardware_concurrency()
     * (or 4 if that returns 0).
     */
    explicit ThreadPool(size_t num_threads = 0) {
        if (num_threads == 0) {
            num_threads = std::thread::hardware_concurrency();
            if (num_threads == 0) num_threads = 4;
        }
        workers_.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    // Non-copyable, non-movable.
    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    ~ThreadPool() { shutdown(); }

    /**
     * Enqueue a task for execution.  Returns immediately.
     * If shutdown() has been called, the task is silently dropped.
     */
    void submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    /**
     * Stop accepting new tasks, drain all pending work, and join every
     * worker thread.  Safe to call multiple times (idempotent).
     */
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            stop_ = true;
        }
        cv_.notify_all();
        for (std::thread &t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    /** Number of worker threads in the pool. */
    size_t size() const { return workers_.size(); }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};
