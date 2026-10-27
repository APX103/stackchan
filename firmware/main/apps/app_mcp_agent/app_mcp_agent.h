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
#include <memory>

class AppMcpAgent : public mooncake::AppAbility {
public:
    AppMcpAgent();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    // Keeps the small bottom IP hint fresh (rate-limited inside). The avatar
    // itself stays clean — all diagnostics live on GET / and the serial log.
    void refresh_ip_hint();

    std::unique_ptr<uitk::lvgl_cpp::Label> _label_ip_hint;
    unsigned _pet_conn = 0;  // head-pet gesture observability connection
    uint32_t _last_refresh_ms = 0;
    uint32_t _last_ps_ms      = 0;
};
