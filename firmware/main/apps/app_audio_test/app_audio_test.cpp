/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_audio_test.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include <esp_opus_enc.h>
#include <esp_opus_dec.h>
#include <esp_ae_rate_cvt.h>

#include <hal/hal.h>
#include <mooncake_log.h>
#include <assets/assets.h>
#include <apps/common/common.h>

#include "hal/mcp_lan/device_access.h"

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;

static const char* TAG = "AudioTest";

static constexpr uint32_t _theme_color      = 0x3366CC;
static constexpr uint32_t _theme_color_dark = 0x101F3C;
// Hard stop so a forgotten test session cannot run forever.
static constexpr uint32_t kMaxTestSeconds = 60;

AppAudioTest::AppAudioTest()
{
    setAppInfo().name = "AUDIO.TEST";
    static auto icon  = assets::get_image("icon_audio_test.bin");
    setAppInfo().icon = (void*)&icon;
    setAppInfo().userData = (void*)&_theme_color;
}

void AppAudioTest::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppAudioTest::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    LvglLockGuard lock;

    // Full-screen root that doubles as the start/stop button.
    _root = std::make_unique<Container>(lv_screen_active());
    _root->setSize(320, 240);
    _root->setAlign(LV_ALIGN_CENTER);
    _root->setBorderWidth(0);
    _root->setPaddingAll(0);
    lv_obj_set_style_bg_color(_root->get(), lv_color_hex(_theme_color_dark), 0);
    lv_obj_set_style_bg_opa(_root->get(), LV_OPA_COVER, 0);
    lv_obj_add_flag(_root->get(), LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_root->get(), click_cb, LV_EVENT_CLICKED, this);

    _title = lv_label_create(_root->get());
    lv_obj_set_style_text_font(_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(_title, LV_ALIGN_TOP_MID, 0, 44);
    lv_label_set_text(_title, "AUDIO TEST");

    _status = lv_label_create(_root->get());
    lv_obj_set_style_text_font(_status, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_status, lv_color_hex(0xC8D6F0), 0);
    lv_obj_set_style_text_align(_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_status, LV_ALIGN_CENTER, 0, -6);
    lv_label_set_text(_status, "Tap to start\nmic -> speaker loopback");

    _bar = lv_bar_create(_root->get());
    lv_bar_set_range(_bar, 0, 100);
    lv_obj_set_size(_bar, 260, 14);
    lv_obj_align(_bar, LV_ALIGN_CENTER, 0, 48);
    lv_obj_set_style_bg_color(_bar, lv_color_hex(0x24406B), 0);
    lv_obj_set_style_bg_color(_bar, lv_color_hex(0x7FA8F0), LV_PART_INDICATOR);
    lv_bar_set_value(_bar, 0, LV_ANIM_OFF);

    view::create_home_indicator([&]() { close(); }, _theme_color, _theme_color_dark);
}

void AppAudioTest::click_cb(lv_event_t* e)
{
    static_cast<AppAudioTest*>(lv_event_get_user_data(e))->toggle_test();
}

void AppAudioTest::toggle_test()
{
    if (_task_running.load()) {
        stop_test();
    } else {
        start_test();
    }
}

void AppAudioTest::start_test()
{
    if (_task_running.load()) {
        return;
    }
    auto* codec = mcp_dev::audio_codec();
    if (codec == nullptr) {
        lv_label_set_text(_status, "no audio codec");
        return;
    }

    // A zero volume would look like a failure — boost for the test, restore
    // on stop.
    if (GetHAL().getSpeakerVolume() == 0) {
        GetHAL().setSpeakerVolume(80);
        _volume_boosted = true;
    }

    _mic_level.store(0);
    _frames.store(0);
    _shown_level   = 0;
    _was_running   = true;
    _task_running.store(true);
    _task_exited.store(false);
    if (xTaskCreatePinnedToCore(loopback_task, "audtest", 2048 * 12, this, 4, nullptr, 1) != pdPASS) {
        ESP_LOGE(TAG, "failed to create audtest task");
        _task_running.store(false);
        _task_exited.store(true);
        return;
    }
    lv_label_set_text(_status, "Listening...\nspeak now (tap to stop)");
    mclog::tagInfo(getAppInfo().name, "loopback test started");
}

void AppAudioTest::stop_test()
{
    _task_running.store(false);
}

void AppAudioTest::loopback_task(void* arg)
{
    auto* self      = static_cast<AppAudioTest*>(arg);
    auto* codec     = mcp_dev::audio_codec();
    const bool ok   = codec != nullptr;

    if (ok) {
        codec->EnableInput(true);
        // The duplex I2S bus is clocked from the output side — the input
        // never sees a BCLK without it (nothing is played besides the test).
        codec->EnableOutput(true);
        codec->Start();
    }

    esp_ae_rate_cvt_cfg_t rs_cfg = {};
    rs_cfg.src_rate              = codec->input_sample_rate();
    rs_cfg.dest_rate             = 16000;
    rs_cfg.channel               = 1;
    rs_cfg.bits_per_sample       = ESP_AUDIO_BIT16;
    rs_cfg.complexity            = 2;
    rs_cfg.perf_type             = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
    esp_ae_rate_cvt_handle_t rs  = nullptr;
    esp_opus_enc_config_t enc_cfg = {};
    enc_cfg.sample_rate           = ESP_AUDIO_SAMPLE_RATE_16K;
    enc_cfg.channel               = ESP_AUDIO_MONO;
    enc_cfg.bits_per_sample       = ESP_AUDIO_BIT16;
    enc_cfg.bitrate               = ESP_OPUS_BITRATE_AUTO;
    enc_cfg.frame_duration        = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    enc_cfg.application_mode      = ESP_OPUS_ENC_APPLICATION_AUDIO;
    enc_cfg.complexity            = 0;
    enc_cfg.enable_fec            = false;
    // DTX off: a loopback must stay continuous, silence included.
    enc_cfg.enable_dtx            = false;
    enc_cfg.enable_vbr            = true;
    void* enc                     = nullptr;
    esp_opus_dec_cfg_t dec_cfg    = {};
    dec_cfg.sample_rate           = codec->output_sample_rate();
    dec_cfg.channel               = ESP_AUDIO_MONO;
    dec_cfg.frame_duration        = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    dec_cfg.self_delimited        = false;
    void* dec                     = nullptr;
    int frame_bytes = 0, enc_out_bytes = 0;

    bool setup_ok = ok
        && esp_ae_rate_cvt_open(&rs_cfg, &rs) == ESP_AUDIO_ERR_OK
        && esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &enc) == ESP_AUDIO_ERR_OK
        && esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &dec) == ESP_AUDIO_ERR_OK;
    if (setup_ok) {
        esp_opus_enc_get_frame_size(enc, &frame_bytes, &enc_out_bytes);
    } else {
        ESP_LOGE(TAG, "codec pipeline setup failed");
    }

    const size_t in_chunk   = codec->input_sample_rate() / 1000 * 60;  // 1440 interleaved
    const size_t frame_samples = frame_bytes / 2;                      // 960 @ 60 ms
    const size_t out_samples = codec->output_sample_rate() / 1000 * 60; // 1440 @ 24 k
    std::vector<int16_t> in_buf(in_chunk);
    std::vector<int16_t> mic_buf(in_chunk / 2);
    std::vector<int16_t> rs_buf(in_chunk);
    std::vector<int16_t> pcm;
    pcm.reserve(frame_samples * 2);
    std::vector<uint8_t> packet(enc_out_bytes > 0 ? enc_out_bytes : 512);
    std::vector<int16_t> play_buf(out_samples);

    const uint32_t started_ms = GetHAL().millis();
    while (setup_ok && self->_task_running.load()
           && GetHAL().millis() - started_ms < kMaxTestSeconds * 1000) {
        if (!codec->InputData(in_buf)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // De-interleave slot 0 (mic), apply the same x16 digital gain as the
        // /mic stream, and track the peak for the UI bar.
        const size_t mic_samples = in_buf.size() / 2;
        int peak = 0;
        for (size_t i = 0; i < mic_samples; i++) {
            int32_t v  = (int32_t)in_buf[i * 2] * 16;
            v          = std::min<int32_t>(32767, std::max<int32_t>(-32768, v));
            mic_buf[i] = (int16_t)v;
            const int a = v > 0 ? v : -v;
            if (a > peak) {
                peak = a;
            }
        }
        const int level = std::min(100, peak / 328);  // 0-100
        int level_now   = self->_mic_level.load();
        while (level > level_now && !self->_mic_level.compare_exchange_weak(level_now, level)) {
        }

        uint32_t produced = rs_buf.size();
        if (esp_ae_rate_cvt_process(rs, (esp_ae_sample_t*)mic_buf.data(), mic_samples,
                                    (esp_ae_sample_t*)rs_buf.data(), &produced) != ESP_AE_ERR_OK) {
            continue;
        }
        pcm.insert(pcm.end(), rs_buf.begin(), rs_buf.begin() + produced);

        while (pcm.size() >= frame_samples) {
            esp_audio_enc_in_frame_t in = {
                .buffer = (uint8_t*)pcm.data(),
                .len    = (uint32_t)(frame_samples * sizeof(int16_t)),
            };
            esp_audio_enc_out_frame_t out = {
                .buffer        = packet.data(),
                .len           = (uint32_t)packet.size(),
                .encoded_bytes = 0,
            };
            if (esp_opus_enc_process(enc, &in, &out) != ESP_AUDIO_ERR_OK || out.encoded_bytes == 0) {
                break;
            }
            esp_audio_dec_in_raw_t raw = {};
            raw.buffer                 = packet.data();
            raw.len                    = out.encoded_bytes;
            raw.frame_recover          = ESP_AUDIO_DEC_RECOVERY_NONE;
            esp_audio_dec_out_frame_t dec_out = {};
            dec_out.buffer             = (uint8_t*)play_buf.data();
            dec_out.len                = (uint32_t)(play_buf.size() * sizeof(int16_t));
            esp_audio_dec_info_t info  = {};
            if (esp_opus_dec_decode(dec, &raw, &dec_out, &info) == ESP_AUDIO_ERR_OK
                && dec_out.decoded_size > 0) {
                const size_t samples = dec_out.decoded_size / sizeof(int16_t);
                std::vector<int16_t> chunk(play_buf.begin(), play_buf.begin() + samples);
                codec->OutputData(chunk);
                self->_frames.fetch_add(1);
            }
            pcm.erase(pcm.begin(), pcm.begin() + frame_samples);
        }
    }

    if (enc != nullptr) esp_opus_enc_close(enc);
    if (dec != nullptr) esp_opus_dec_close(dec);
    if (rs != nullptr) esp_ae_rate_cvt_close(rs);
    if (ok) {
        codec->EnableInput(false);
        codec->EnableOutput(false);
    }
    self->_task_running.store(false);
    self->_task_exited.store(true);
    ESP_LOGI(TAG, "loopback task done (%u frames)", (unsigned)self->_frames.load());
    vTaskDelete(nullptr);
}

void AppAudioTest::onRunning()
{
    LvglLockGuard lock;

    view::update_home_indicator();

    const uint32_t now = GetHAL().millis();
    if (now - _last_ui_ms < 100) {
        return;
    }
    _last_ui_ms = now;

    // Peak-hold with decay for a responsive but calm bar.
    const int level = _mic_level.exchange(0);
    _shown_level    = std::max(level, _shown_level * 85 / 100);
    lv_bar_set_value(_bar, _shown_level, LV_ANIM_OFF);

    if (_task_running.load()) {
        _was_running = true;
    } else if (_was_running) {
        // Covers both user tap and the 60 s auto-stop.
        _was_running = false;
        char result[96];
        snprintf(result, sizeof(result), "Done: %u frames\nTap to test again",
                 (unsigned)_frames.load());
        lv_label_set_text(_status, result);
        if (_volume_boosted) {
            GetHAL().setSpeakerVolume(0);
            _volume_boosted = false;
        }
    }
}

void AppAudioTest::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    _task_running.store(false);
    for (int i = 0; i < 15 && !_task_exited.load(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!_task_exited.load()) {
        ESP_LOGW(TAG, "loopback task did not exit in time");
    }
    if (_volume_boosted) {
        GetHAL().setSpeakerVolume(0);
        _volume_boosted = false;
    }

    LvglLockGuard lock;
    _root.reset();  // deletes children (title/status/bar) and the click cb
    view::destroy_home_indicator();
}
