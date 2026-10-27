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
#include <mutex>

// On-device audio test with two independent tests (never both at once, so no
// acoustic feedback / howling):
//   SPECTRUM — live microphone FFT: 32 log-spaced bands, animated bar chart
//              (UI timer at 50 FPS, data rate ~33 Hz), peak frequency readout.
//   TONES    — speaker scale test: do re mi fa sol (and back down) at 20 %
//              volume, then auto-return to the menu.
class AppAudioTest : public mooncake::AppAbility {
public:
    AppAudioTest();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    static constexpr int kBandCount = 32;

    enum class View { Menu, Spectrum, Tones };
    enum class TaskMode { Idle, Spectrum, Tones };

    // ---- UI ----
    void build_menu_view();
    void build_spectrum_view();
    void build_tones_view();
    void destroy_view();
    void on_ui_tick();          // 50 FPS animation timer (LVGL context)
    static void ui_timer_cb(lv_timer_t* t);

    // ---- audio task ----
    static void audio_task(void* arg);
    void run_spectrum_mode();
    void run_tones_mode();

    // ---- shared with the audio task ----
    std::mutex _spec_mtx;
    float _bands[kBandCount] = {0};   // 0..1 per band
    float _peak_hz           = 0;
    float _peak_db           = -100;
    std::atomic<int> _task_mode{(int)TaskMode::Idle};
    std::atomic<bool> _task_running{false};
    std::atomic<bool> _task_exited{true};
    std::atomic<int> _tone_note{-1};         // index into the scale, -1 = none
    std::atomic<bool> _tones_done{false};
    std::atomic<int> _mic_level{0};          // 0..100, small meter on spectrum view

    // ---- UI state ----
    std::unique_ptr<uitk::lvgl_cpp::Container> _root;
    lv_obj_t* _view           = nullptr;     // current view container (child of root)
    lv_obj_t* _bars[kBandCount] = {nullptr};
    lv_obj_t* _spec_info      = nullptr;
    lv_obj_t* _tone_label     = nullptr;
    float _bar_height[kBandCount] = {0};
    lv_timer_t* _ui_timer     = nullptr;
    View _view_kind           = View::Menu;
    uint32_t _last_info_ms    = 0;
    bool _volume_saved        = false;
    uint8_t _saved_volume     = 0;
};
