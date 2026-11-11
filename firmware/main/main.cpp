/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/apps.h>
#include <hal/hal.h>
#include <hal/hal_toggles.h>
#include <hal/sep/sep_automation.h>  // SEP-0002

using namespace mooncake;
using namespace smooth_ui_toolkit;

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // HAL init
    GetHAL().init();

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    const bool skip_mooncake = GetHAL().getXiaozhiConfig().startAiAgentOnBoot && GetHAL().getWarmRebootTarget() < 0;

    if (!skip_mooncake) {
        // Install apps
        // SEP-0003: slim build — keep only AI.AGENT + SETUP (launcher stays as the desktop).
        // Commented-out apps below are part of the phone-app ecosystem (archived, unused).
        GetMooncake().installApp(std::make_unique<AppLauncher>());
        hal_toggles::register_app_id(hal_toggles::App::AiAgent,
                                     GetMooncake().installApp(std::make_unique<AppAiAgent>()));
        // hal_toggles::register_app_id(hal_toggles::App::Avatar, GetMooncake().installApp(std::make_unique<AppAvatar>()));
        // hal_toggles::register_app_id(hal_toggles::App::EspnowRemote,
        //                              GetMooncake().installApp(std::make_unique<AppEspnowControl>()));
        // hal_toggles::register_app_id(hal_toggles::App::AppCenter,
        //                              GetMooncake().installApp(std::make_unique<AppAppCenter>()));
        // hal_toggles::register_app_id(hal_toggles::App::Ezdata, GetMooncake().installApp(std::make_unique<AppEzdata>()));
        // hal_toggles::register_app_id(hal_toggles::App::Dance, GetMooncake().installApp(std::make_unique<AppDance>()));
        GetMooncake().installApp(std::make_unique<AppSetup>());
        // SEP-0005: LAN MCP server app
        GetMooncake().installApp(std::make_unique<AppMcpAgent>());
        // SEP-0008: camera preview app
        GetMooncake().installApp(std::make_unique<AppCameraTest>());

        // SEP-0002: automation console + http api (launch app by name)
        sep::init();

        // Main loop
        while (1) {
            GetHAL().feedTheDog();
            GetHAL().updateHeapStatusLog();

            sep::poll();  // SEP-0002: execute queued app launches

            GetMooncake().update();

            if (GetHAL().isXiaozhiStartRequested()) {
                break;
            }
        }

        // Uninstall all apps and destroy mooncake
        GetMooncake().uninstallAllApps();
        DestroyMooncake();
    }

    // Start xiaozhi, never returns
    GetHAL().startXiaozhi();
}
