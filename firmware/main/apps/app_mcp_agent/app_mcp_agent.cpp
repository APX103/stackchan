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
// connection, no autonomous behavior. The face stays clean and cute; the
// only on-screen hint is a small dim "ip:port" line at the bottom edge.
// Full diagnostics live on GET / and the serial log.
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

        // The only hint: a small dim "ip:port" at the bottom edge so a LAN
        // client knows where to connect. Nothing else covers the face.
        _label_ip_hint = std::make_unique<Label>(lv_screen_active());
        _label_ip_hint->setTextFont(&lv_font_montserrat_14);
        _label_ip_hint->setTextColor(lv_color_hex(0x8899AA));
        _label_ip_hint->align(LV_ALIGN_BOTTOM_MID, 0, -30);
        _label_ip_hint->setText("");
    }

    // Bring Wi-Fi up if needed (same flow as the App Center), then serve.
    if (!mcp_dev::wifi_connected()) {
        std::unique_ptr<view::LoadingPage> loading_page;
        {
            LvglLockGuard lock;
            loading_page = std::make_unique<view::LoadingPage>(_theme_color, _theme_color_dark);
        }
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
        _label_ip_hint->setText("start failed");
        return;
    }
    _last_refresh_ms = 0;
    refresh_ip_hint();
}

void AppMcpAgent::onRunning()
{
    {
        LvglLockGuard lock;

        GetStackChan().update();

        view::update_home_indicator();
        view::update_status_bar();

        refresh_ip_hint();
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

        _label_ip_hint.reset();

        view::destroy_home_indicator();
        view::destroy_status_bar();
    }
}

void AppMcpAgent::refresh_ip_hint()
{
    const uint32_t now = GetHAL().millis();
    if (now - _last_refresh_ms < 1000) {
        return;
    }
    _last_refresh_ms = now;

    auto st = mcp_lan::service_status();
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
