/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>

namespace mcp_lan {

enum class UiJobResult { Completed, Cancelled, Failed };

// Shared by the worker and the UI thread. A timeout cancels execution instead
// of leaving a callback referencing a timed-out request.
class UiPending {
public:
    bool wait(uint32_t timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mtx_);
        const bool finished = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            return done_ || cancelled_;
        });
        if (!finished) {
            cancelled_ = true;
            return false;
        }
        return done_ && succeeded_ && !cancelled_;
    }

    UiJobResult run(const std::function<void()>& action)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        UiJobResult result = UiJobResult::Completed;
        if (cancelled_) {
            result = UiJobResult::Cancelled;
        } else {
            try {
                action();
            } catch (...) {
                result = UiJobResult::Failed;
            }
        }
        succeeded_ = result == UiJobResult::Completed;
        done_ = true;
        cv_.notify_all();
        return result;
    }

    void cancel()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        cancelled_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    bool done_ = false;
    bool succeeded_ = false;
    bool cancelled_ = false;
};

}  // namespace mcp_lan
