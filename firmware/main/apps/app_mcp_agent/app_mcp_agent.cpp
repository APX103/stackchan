/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_mcp_agent.h"

#include <sdkconfig.h>
#include <esp_system.h>
#include <misc/lv_timer.h>

#include <hal/hal.h>
#include <mooncake_log.h>
#include <assets/assets.h>
#include <stackchan/stackchan.h>
#include <stackchan/avatar/skins/default/default.h>
#include <apps/common/common.h>

#include "platform/device_access.h"
#include "service/lan_service.h"

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace stackchan;

// Blue theme of the MCP Agent app (launcher icon / status bar).
static constexpr uint32_t _theme_color      = 0x3366CC;
static constexpr uint32_t _theme_color_dark = 0x101F3C;

AppMcpAgent::~AppMcpAgent()
{
    if (_service != nullptr) {
        // uninstallAllApps() destroys abilities directly, without onClose().
        // The final desktop-to-AI transition must not leave callbacks or
        // tasks using this App. Home never takes this synchronous path.
        _closing = true;
        _service->request_stop();
        while (!_service->stopped()) {
            GetHAL().feedTheDog();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        _service->reclaim();
        onClose();
    }
    if (_opened_once && GetHAL().isXiaozhiStartRequested()) {
        mcp_dev::wifi_stop_for_ai_handoff();
    }
}

AppMcpAgent::AppMcpAgent()
{
    // Configure App name
    setAppInfo().name = "MCP.AGENT";
    // Server-stack glyph (generated, RGB565A8, same format as the other icons)
    static auto icon  = assets::get_image("icon_mcp_agent.bin");
    setAppInfo().icon = (void*)&icon;
    setAppInfo().userData = (void*)&_theme_color;
}

void AppMcpAgent::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

// Standalone MCP server app: shows the same StackChan face the AI Agent app
// uses, but never starts the xiaozhi application — no voice chat, no cloud
// connection, no autonomous behavior. The face stays clean and cute; the
// only on-screen hint is a small dim "ip:port" line at the bottom edge.
// Full diagnostics live on GET / and the serial log.
void AppMcpAgent::onOpen()
{
    if (_closing) {
        if (_service && !_service->stopped()) {
            return;  // previous close is still stopping; do not reinitialize
        }
        onClose();
    }

    mclog::tagInfo(getAppInfo().name, "on open");
    _opened_once = true;

    {
        LvglLockGuard lock;

        // Same face as the AI Agent app (StackChan avatar system), with
        // only the passive expression animation: breathing and blinking.
        // Deliberately NO idle-motion / IMU modifiers — the servos must
        // not move unless an MCP tool says so.
        auto avatar = std::make_unique<avatar::DefaultAvatar>();
        avatar->init(lv_screen_active());
        GetStackChan().attachAvatar(std::move(avatar));
        _breath_modifier_id = GetStackChan().addModifier(std::make_unique<BreathModifier>());
        _blink_modifier_id = GetStackChan().addModifier(std::make_unique<BlinkModifier>());
        // Head-pet interaction (same as the AI Agent app): stroking the
        // head makes the face blush with hearts and a happy wiggle, then
        // returns to normal after a few seconds.
        _headpet_modifier_id = GetStackChan().addModifier(std::make_unique<HeadPetModifier>());
        // Gesture observability: every head-pet gesture shows up in the
        // service event line (GET / last_event) — separates "touch not
        // detected" from "modifier not reacting".
        _pet_conn = GetHAL().onHeadPetGesture.connect([this](HeadPetGesture g) {
            if (_service != nullptr) {
                _service->push_event("headpet gesture %d", (int)g);
            }
        });
        _reminder_conn = tools::on_reminder_triggered().connect([this](int, std::string_view message) {
            LvglLockGuard lock;
            if (GetStackChan().hasAvatar()) {
                GetStackChan().avatar().addDecorator(
                    std::make_unique<view::ReminderView>(lv_screen_active(), message));
            }
        });

        // Common widgets (status bar + home indicator at the bottom to exit)
        view::create_home_indicator([this]() {
            _closing = true;
            if (_service != nullptr) {
                _service->request_stop();
            }
        }, _theme_color, _theme_color_dark);
        view::create_status_bar(_theme_color, _theme_color_dark);

        // The only hint: a small dim "ip:port" at the bottom edge so a LAN
        // client knows where to connect. Nothing else covers the face.
        _label_ip_hint = std::make_unique<Label>(lv_screen_active());
        _label_ip_hint->setTextFont(&lv_font_montserrat_14);
        _label_ip_hint->setTextColor(lv_color_hex(0x8899AA));
        _label_ip_hint->align(LV_ALIGN_BOTTOM_MID, 0, -30);
        _label_ip_hint->setText("");
    }

    _service = std::make_unique<mcp_lan::LanService>();
    _service->set_ui_hooks({
        .execute = [this](const mcp_lan::LanService::UiAction& action, uint32_t timeout_ms) {
            return ui_execute(action, timeout_ms);
        },
        .show_message = [this](const std::string& text, uint32_t duration_ms) {
            show_message(text, duration_ms);
        },
        .clear_message = [this] { clear_message(); },
        .create_reminder = [this](uint32_t duration_ms, const std::string& message, bool repeat) {
            return create_reminder(duration_ms, message, repeat);
        },
        .get_reminders = [this] { return get_reminders(); },
        .stop_reminder = [this](int id) { stop_reminder(id); },
    });
    if (!mcp_dev::wifi_connected()) {
        {
            LvglLockGuard lock;
            // Offline startup remains interactive: the launcher/Setup app can
            // provision Wi-Fi later; HTTPD starts only after LwIP is ready.
            _service_start_pending = true;
            _label_ip_hint->setText("offline - configure WiFi in SETUP");
        }
        mcp_dev::wifi_start();
        return;
    }

    std::string err;
    if (!_service->start(&err)) {
        mclog::tagError(getAppInfo().name, "service start failed: {}", err);
        LvglLockGuard lock;
        _label_ip_hint->setText("start failed");
        return;
    }
    _last_refresh_ms = 0;
    {
        LvglLockGuard lock;
        refresh_ip_hint();
    }
}

void AppMcpAgent::onRunning()
{
    if (_closing) {
        if (!_service || _service->stopped()) {
            if (_service) {
                _service->reclaim();
            }
            close();
        }
        return;
    }

    if (_service_start_pending && mcp_dev::wifi_connected()) {
        _service_start_pending = false;
        std::string err;
        if (!_service->start(&err)) {
            mclog::tagError(getAppInfo().name, "deferred service start failed: {}", err);
            LvglLockGuard lock;
            _label_ip_hint->setText("start failed");
            return;
        }
        _last_refresh_ms = 0;
    }

    tools::update_reminders();
    {
        LvglLockGuard lock;

        process_ui_jobs();
        GetStackChan().update();

        view::update_home_indicator();
        view::update_status_bar();

        refresh_ip_hint();
    }
}

void AppMcpAgent::onSleeping()
{
    // A direct closeApp() advances Mooncake to Sleeping after onClose().
    // Finish deferred cleanup here instead of leaving the old UI connected
    // until the next open.
    if (_closing && _service != nullptr && _service->stopped()) {
        _service->reclaim();
        onClose();
    }
}

// UI and service cleanup runs only after the asynchronous stop has completed.
// No HTTPD or worker waits are allowed here.
void AppMcpAgent::onClose()
{
    _closing = true;
    if (_service) {
        _service->request_stop();
        if (!_service->stopped()) {
            return;  // Mooncake close must not destroy live UI/service state
        }
        _service->reclaim();
    }

    mclog::tagInfo(getAppInfo().name, "on close");

    {
        LvglLockGuard lock;

        if (_breath_modifier_id >= 0) {
            GetStackChan().removeModifier(_breath_modifier_id);
            _breath_modifier_id = -1;
        }
        if (_blink_modifier_id >= 0) {
            GetStackChan().removeModifier(_blink_modifier_id);
            _blink_modifier_id = -1;
        }
        if (_headpet_modifier_id >= 0) {
            GetStackChan().removeModifier(_headpet_modifier_id);
            _headpet_modifier_id = -1;
        }
        clear_message();
        cancel_ui_jobs();
        GetStackChan().resetAvatar();

        if (_pet_conn != 0) {
            GetHAL().onHeadPetGesture.disconnect(_pet_conn);
            _pet_conn = 0;
        }
        if (_reminder_conn != 0) {
            tools::on_reminder_triggered().disconnect(_reminder_conn);
            _reminder_conn = 0;
        }
        _label_ip_hint.reset();

        view::destroy_home_indicator();
        view::destroy_status_bar();
    }
    _service.reset();
    mcp_dev::wifi_restore_power_save();
    _closing = false;
}

void AppMcpAgent::refresh_ip_hint()
{
    const uint32_t now = GetHAL().millis();
    if (now - _last_refresh_ms < 1000) {
        return;
    }
    _last_refresh_ms = now;

    auto st = _service->status();
    if (_service_start_pending) {
        _label_ip_hint->setText("offline - configure WiFi in SETUP");
        return;
    }
    // Be honest on screen: an IP without a listener is worse than no hint.
    if (!st.running) {
        _label_ip_hint->setText("service failed");
        return;
    }
    // The LAN server must stay reachable: without this the default Wi-Fi
    // modem sleep makes the device miss ARP within a minute of idling.
    // Re-assert every 5 s while connected — a Wi-Fi drop/reconnect silently
    // restores the power-save default, and a set-once flag misses that.
    if (!st.ip.empty() && st.ip != "0.0.0.0") {
        if (now - _last_ps_ms > 5000) {
            _last_ps_ms = now;
            mcp_dev::wifi_set_no_power_save();
        }
    }
    // The hint has served its purpose once a client found the server.
    if (st.requests > 0) {
        _label_ip_hint->setText("");
        return;
    }
    const std::string& ip = st.ip;
    if (ip.empty() || ip == "0.0.0.0") {
        _label_ip_hint->setText("");
        return;
    }
    char hint[28];
    snprintf(hint, sizeof(hint), "%s:%d", ip.c_str(), st.port);
    _label_ip_hint->setText(hint);
}

bool AppMcpAgent::ui_execute(const std::function<void()>& action, uint32_t timeout_ms)
{
    if (!action || _closing) {
        return false;
    }
    auto pending = std::make_shared<UiPending>();
    {
        std::lock_guard<std::mutex> lock(_ui_jobs_mtx);
        if (_closing) {
            return false;
        }
        _ui_jobs.emplace_back(action, pending);
    }

    return pending->wait(timeout_ms);
}

void AppMcpAgent::process_ui_jobs()
{
    for (int count = 0; count < 4; ++count) {
        UiJob job;
        {
            std::lock_guard<std::mutex> lock(_ui_jobs_mtx);
            if (_ui_jobs.empty()) {
                return;
            }
            job = std::move(_ui_jobs.front());
            _ui_jobs.pop_front();
        }

        const auto result = job.second->run(job.first);
        if (result == mcp_lan::UiJobResult::Failed) {
            mclog::tagError(getAppInfo().name, "UI action raised an exception");
        }
    }
}

void AppMcpAgent::cancel_ui_jobs()
{
    std::deque<UiJob> jobs;
    {
        std::lock_guard<std::mutex> lock(_ui_jobs_mtx);
        jobs.swap(_ui_jobs);
    }
    for (auto& job : jobs) {
        job.second->cancel();
    }
}

void AppMcpAgent::show_message(const std::string& text, uint32_t duration_ms)
{
    if (!GetStackChan().hasAvatar()) {
        return;
    }
    GetStackChan().avatar().setSpeech(text);
    if (_speech_timer != nullptr) {
        lv_timer_delete(_speech_timer);
        _speech_timer = nullptr;
    }
    if (duration_ms > 0) {
        _speech_timer = lv_timer_create(speech_timer_cb, duration_ms, this);
        if (_speech_timer != nullptr) {
            lv_timer_set_repeat_count(_speech_timer, 1);
        }
    }
}

void AppMcpAgent::clear_message()
{
    if (GetStackChan().hasAvatar()) {
        GetStackChan().avatar().setSpeech("");
    }
    if (_speech_timer != nullptr) {
        lv_timer_delete(_speech_timer);
        _speech_timer = nullptr;
    }
}

void AppMcpAgent::speech_timer_cb(lv_timer_t* timer)
{
    auto* app = static_cast<AppMcpAgent*>(lv_timer_get_user_data(timer));
    if (app == nullptr) {
        return;
    }
    if (GetStackChan().hasAvatar()) {
        GetStackChan().avatar().setSpeech("");
    }
    app->_speech_timer = nullptr;
    lv_timer_delete(timer);
}

int AppMcpAgent::create_reminder(uint32_t duration_ms, const std::string& message, bool repeat)
{
    return tools::create_reminder(duration_ms, message, repeat);
}

std::vector<tools::ReminderInfo_t> AppMcpAgent::get_reminders()
{
    return tools::get_active_reminders();
}

void AppMcpAgent::stop_reminder(int id)
{
    tools::stop_reminder(id);
}
