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
#include <lvgl.h>

#include <atomic>
#include <cstdint>
#include <memory>

// On-device audio test: mic -> x16 gain -> 16 kHz resample -> Opus encode ->
// Opus decode -> speaker, i.e. the exact codec path the MCP Agent's /mic and
// /speaker WebSocket streams use. Tap the screen to start/stop; a live mic
// level bar shows the input, and the result line shows frames + peak level.
class AppAudioTest : public mooncake::AppAbility {
public:
    AppAudioTest();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    static void click_cb(lv_event_t* e);
    void toggle_test();
    void start_test();
    void stop_test();

    static void loopback_task(void* arg);

    std::unique_ptr<uitk::lvgl_cpp::Container> _root;
    lv_obj_t* _title  = nullptr;
    lv_obj_t* _status = nullptr;
    lv_obj_t* _bar    = nullptr;

    std::atomic<bool> _task_running{false};
    std::atomic<bool> _task_exited{true};
    std::atomic<int> _mic_level{0};        // 0-100, for the UI bar
    std::atomic<uint32_t> _frames{0};      // encoded (= decoded) opus frames
    bool _was_running    = false;
    bool _volume_boosted = false;
    uint32_t _last_ui_ms = 0;
    int _shown_level     = 0;
};
