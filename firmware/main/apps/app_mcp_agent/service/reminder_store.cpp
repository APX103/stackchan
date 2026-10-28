/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "reminder_store.h"

#include <algorithm>

namespace mcp_lan {

int ReminderStore::create(uint32_t now_ms, uint32_t duration_ms, const std::string& message, bool repeat)
{
    const int id = next_id_++;
    entries_.push_back({id, now_ms + duration_ms, duration_ms, message, repeat});
    return id;
}

void ReminderStore::stop(int id)
{
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [id](const Entry& entry) {
        return entry.id == id;
    }), entries_.end());
}

std::vector<ReminderInfo> ReminderStore::active() const
{
    std::vector<ReminderInfo> result;
    result.reserve(entries_.size());
    for (const auto& entry : entries_) {
        result.push_back({entry.id, entry.duration_ms, entry.message, entry.repeat});
    }
    return result;
}

void ReminderStore::clear()
{
    entries_.clear();
}

void ReminderStore::tick(uint32_t now_ms, const std::function<void(const ReminderInfo&)>& on_trigger)
{
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (now_ms < it->trigger_at) {
            ++it;
            continue;
        }
        if (on_trigger) {
            on_trigger({it->id, it->duration_ms, it->message, it->repeat});
        }
        if (it->repeat) {
            it->trigger_at = now_ms + it->duration_ms;
            ++it;
        } else {
            it = entries_.erase(it);
        }
    }
}

}  // namespace mcp_lan
