/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_mcp_agent.h"

#include <sdkconfig.h>
#include <esp_system.h>

#include <hal/hal.h>
#include <mooncake_log.h>
#include <assets/assets.h>
#include <stackchan/stackchan.h>
#include <stackchan/avatar/skins/default/default.h>
#include <apps/common/common.h>

#include "hal/mcp_lan/device_access.h"
#include "hal/mcp_lan/lan_service.h"

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace stackchan;

// Blue theme of the MCP Agent app (launcher icon / status bar).
static constexpr uint32_t _theme_color      = 0x3366CC;
static constexpr uint32_t _theme_color_dark = 0x101F3C;

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
// connection, no autonomous behavior. Nothing moves on its own; the robot
// only acts on tools/call requests from http://<device-ip>:<port>/mcp.
void AppMcpAgent::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    {
        LvglLockGuard lock;

        // Same face as the AI Agent app (StackChan avatar system), with
        // only the passive expression animation: breathing and blinking.
        // Deliberately NO idle-motion / IMU modifiers — the servos must
        // not move unless an MCP tool says so.
        auto avatar = std::make_unique<avatar::DefaultAvatar>();
        avatar->init(lv_screen_active());
        GetStackChan().attachAvatar(std::move(avatar));
        GetStackChan().addModifier(std::make_unique<BreathModifier>());
        GetStackChan().addModifier(std::make_unique<BlinkModifier>());
        // Head-pet interaction (same as the AI Agent app): stroking the
        // head makes the face blush with hearts and a happy wiggle, then
        // returns to normal after a few seconds.
        GetStackChan().addModifier(std::make_unique<HeadPetModifier>());

        // Common widgets (status bar + home indicator at the bottom to exit)
        view::create_home_indicator([&]() { close(); }, _theme_color, _theme_color_dark);
        view::create_status_bar(_theme_color, _theme_color_dark);

        // Status page
        _label_url = std::make_unique<Label>(lv_screen_active());
        _label_url->setTextFont(&lv_font_montserrat_16);
        _label_url->setTextColor(lv_color_hex(0xFFFFFF));
        _label_url->align(LV_ALIGN_CENTER, 0, -34);
        _label_url->setText("starting...");

        _label_status = std::make_unique<Label>(lv_screen_active());
        _label_status->setTextFont(&lv_font_montserrat_14);
        _label_status->setTextColor(lv_color_hex(0xFFFFFF));
        _label_status->align(LV_ALIGN_CENTER, 0, -6);
        _label_status->setText("");

        _label_event = std::make_unique<Label>(lv_screen_active());
        _label_event->setTextFont(&lv_font_montserrat_14);
        _label_event->setTextColor(lv_color_hex(0x9FB4D8));
        _label_event->align(LV_ALIGN_CENTER, 0, 22);
        _label_event->setText("");
    }

    // Bring Wi-Fi up if needed (same flow as the App Center), then serve.
    if (!mcp_dev::wifi_connected()) {
        std::unique_ptr<view::LoadingPage> loading_page;
        {
            LvglLockGuard lock;
            loading_page = std::make_unique<view::LoadingPage>(_theme_color, _theme_color_dark);        }
        GetHAL().startNetwork([&](std::string_view msg) {
            LvglLockGuard lock;
            loading_page->setMessage(msg);
        });
        {
            LvglLockGuard lock;
            loading_page.reset();
        }
    }

    std::string err;
    if (!mcp_lan::service_start(&err)) {
        mclog::tagError(getAppInfo().name, "service start failed: {}", err);
        LvglLockGuard lock;
        _label_url->setText("start failed");
        _label_status->setText(err.c_str());
        return;
    }
    _last_refresh_ms = 0;
    refresh_labels();
}

void AppMcpAgent::onRunning()
{
    {
        LvglLockGuard lock;

        GetStackChan().update();

        view::update_home_indicator();
        view::update_status_bar();

        refresh_labels();
    }
}

// Exiting this app is what stops the server: all endpoints, tasks and audio
// claims are released, then control returns to the launcher.
void AppMcpAgent::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    mcp_lan::service_stop();

    {
        LvglLockGuard lock;

        GetStackChan().resetAvatar();

        _label_url.reset();
        _label_status.reset();
        _label_event.reset();

        view::destroy_home_indicator();
        view::destroy_status_bar();
    }
}

void AppMcpAgent::refresh_labels()
{
    const uint32_t now = GetHAL().millis();
    if (now - _last_refresh_ms < 500) {
        return;
    }
    _last_refresh_ms = now;

    auto st = mcp_lan::service_status();
    if (st.ip.empty() || st.ip == "0.0.0.0") {
        _label_url->setText("offline");
    } else {
        char url[48];
        snprintf(url, sizeof(url), "http://%s:%d/mcp", st.ip.c_str(), st.port);
        _label_url->setText(url);
    }

    char status[96];
    snprintf(status, sizeof(status), "tools:%u calls:%u err:%u  mic:%c spk:%c  heap:%uk",
             (unsigned)st.tools, (unsigned)st.tool_calls, (unsigned)st.tool_errors, st.mic_active ? '+' : '-',
             st.spk_active ? '+' : '-', (unsigned)(esp_get_free_heap_size() / 1024));
    _label_status->setText(status);

    _label_event->setText(st.last_event.c_str());
}
