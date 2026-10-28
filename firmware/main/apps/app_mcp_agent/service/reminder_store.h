/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mcp_lan {

struct ReminderInfo {
    int id = 0;
    uint32_t duration_ms = 0;
    std::string message;
    bool repeat = false;
};

// App-local reminder store. All methods are expected to run in the same UI
// context (Mooncake's onRunning or one of its queued UI actions), so no other
// application's reminder manager is touched and no cross-thread vector access
// is required.
class ReminderStore {
public:
    int create(uint32_t now_ms, uint32_t duration_ms, const std::string& message, bool repeat);
    void stop(int id);
    std::vector<ReminderInfo> active() const;
    void clear();

    void tick(uint32_t now_ms, const std::function<void(const ReminderInfo&)>& on_trigger);

private:
    struct Entry {
        int id = 0;
        uint32_t trigger_at = 0;
        uint32_t duration_ms = 0;
        std::string message;
        bool repeat = false;
    };

    std::vector<Entry> entries_;
    int next_id_ = 1;
};

}  // namespace mcp_lan
