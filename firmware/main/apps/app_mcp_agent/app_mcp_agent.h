/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <mooncake.h>
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <smooth_lvgl.hpp>

#include <cstdint>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>

#include "service/reminder_store.h"
#include "service/ui_pending.h"

namespace mcp_lan {
class LanService;
}

class AppMcpAgent : public mooncake::AppAbility {
public:
    AppMcpAgent();
    ~AppMcpAgent() override;

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onSleeping() override;
    void onClose() override;

private:
    // Keeps the small bottom IP hint fresh (rate-limited inside). The avatar
    // itself stays clean — all diagnostics live on GET / and the serial log.
    void refresh_ip_hint();

    using UiPending = mcp_lan::UiPending;
    using UiJob = std::pair<std::function<void()>, std::shared_ptr<UiPending>>;

    bool ui_execute(const std::function<void()>& action, uint32_t timeout_ms);
    void process_ui_jobs();
    void cancel_ui_jobs();
    void show_message(const std::string& text, uint32_t duration_ms);
    void clear_message();
    int create_reminder(uint32_t duration_ms, const std::string& message, bool repeat);
    std::vector<mcp_lan::ReminderInfo> get_reminders();
    void stop_reminder(int id);
    static void speech_timer_cb(lv_timer_t* timer);

    std::mutex _ui_jobs_mtx;
    std::deque<UiJob> _ui_jobs;
    lv_timer_t* _speech_timer = nullptr;
    mcp_lan::ReminderStore _reminders;

    std::unique_ptr<uitk::lvgl_cpp::Label> _label_ip_hint;
    std::unique_ptr<mcp_lan::LanService> _service;
    unsigned _pet_conn = 0;  // head-pet gesture observability connection
    int _breath_modifier_id = -1;
    int _blink_modifier_id = -1;
    int _headpet_modifier_id = -1;
    std::atomic<bool> _closing{false};
    bool _service_start_pending = false;
    bool _opened_once = false;
    uint32_t _last_refresh_ms = 0;
    uint32_t _last_ps_ms      = 0;
};
