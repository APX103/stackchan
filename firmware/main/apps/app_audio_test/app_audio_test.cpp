/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_audio_test.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <math.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include <hal/hal.h>
#include <mooncake_log.h>
#include <assets/assets.h>
#include <apps/common/common.h>

#include "audio_test_audio.h"

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;

static const char* TAG = "AudioTest";

// The single installed instance (used by the LVGL timer callback).
static AppAudioTest* s_active_instance = nullptr;

static constexpr uint32_t _theme_color      = 0x3366CC;
static constexpr uint32_t _theme_color_dark = 0x101F3C;
static constexpr uint32_t _accent           = 0x7FA8F0;
static constexpr uint32_t _card_bg          = 0x24406B;
static constexpr uint32_t _text_dim         = 0xC8D6F0;

// ---------------------------------------------------------------- helpers

static lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t color,
                            lv_align_t align, int32_t x, int32_t y, const char* text)
{
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_align(label, align, x, y);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t* make_card(lv_obj_t* parent, int32_t x, int32_t w, lv_event_cb_t cb, void* user_data)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, w, 118);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, x, 66);
    lv_obj_set_style_bg_color(card, lv_color_hex(_card_bg), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(_accent), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, user_data);
    return card;
}

// 512-point iterative radix-2 FFT with a lazily owned twiddle table.
constexpr int kFftN = 512;

struct AudioTestFft {
    float wr[kFftN / 2];
    float wi[kFftN / 2];

    AudioTestFft()
    {
        for (int k = 0; k < kFftN / 2; k++) {
            const float ang = -2.0f * (float)M_PI * k / kFftN;
            wr[k]           = cosf(ang);
            wi[k]           = sinf(ang);
        }
    }

    void run(float* re, float* im) const
    {
        // bit-reversal permutation
        for (int i = 1, j = 0; i < kFftN; i++) {
            int bit = kFftN >> 1;
            for (; j & bit; bit >>= 1) {
                j ^= bit;
            }
            j ^= bit;
            if (i < j) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (int len = 2; len <= kFftN; len <<= 1) {
            const int step = kFftN / len;
            for (int i = 0; i < kFftN; i += len) {
                for (int k = 0; k < len / 2; k++) {
                    const float cwr = wr[k * step];
                    const float cwi = wi[k * step];
                    const int a     = i + k;
                    const int b     = a + len / 2;
                    const float vr  = re[b] * cwr - im[b] * cwi;
                    const float vi  = re[b] * cwi + im[b] * cwr;
                    re[b]           = re[a] - vr;
                    im[b]           = im[a] - vi;
                    re[a] += vr;
                    im[a] += vi;
                }
            }
        }
    }
};

// Log-spaced band edges (~80 Hz .. ~12 kHz across 32 bands).
namespace {
float band_edge(int i)
{
    const float lo    = 80.0f;
    const float ratio = 12000.0f / lo;
    return lo * powf(ratio, (float)i / 32);
}

}  // namespace

// ---------------------------------------------------------------- app

AppAudioTest::~AppAudioTest()
{
    if (_root == nullptr && _audio_task_handle == nullptr) {
        return;
    }
    _closing = true;
    _task_mode.store((int)TaskMode::Idle);
    _task_running.store(false);
    while (_audio_task_handle != nullptr && !_task_exited.load()) {
        GetHAL().feedTheDog();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    onClose();
}

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
    if (_closing) {
        if (_audio_task_handle != nullptr && !_task_exited.load()) {
            return;
        }
        onClose();
    }
    mclog::tagInfo(getAppInfo().name, "on open");
    s_active_instance = this;

    {
        LvglLockGuard lock;

        _root = std::make_unique<Container>(lv_screen_active());
        _root->setSize(320, 240);
        _root->setAlign(LV_ALIGN_CENTER);
        _root->setBorderWidth(0);
        _root->setPaddingAll(0);
        lv_obj_set_style_bg_color(_root->get(), lv_color_hex(_theme_color_dark), 0);
        lv_obj_set_style_bg_opa(_root->get(), LV_OPA_COVER, 0);

        build_menu_view();

        view::create_home_indicator([this]() {
            _closing = true;
            _task_mode.store((int)TaskMode::Idle);
            _task_running.store(false);
        }, _theme_color, _theme_color_dark);
    }

    _task_running.store(true);
    _task_exited.store(false);
    // 24 KB internal stack (BYTES with WithCaps; must not be PSRAM — see
    // mic_ws_stream.cpp for the flash-write/cache constraint).
    if (xTaskCreatePinnedToCoreWithCaps(audio_task, "audtest", 2048 * 12, this, 4, &_audio_task_handle, 1,
                                        MALLOC_CAP_INTERNAL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create audtest task");
        _task_running.store(false);
        _task_exited.store(true);
    }
}

// ---------------------------------------------------------------- views

void AppAudioTest::destroy_view()
{
    if (_view != nullptr) {
        lv_obj_delete(_view);
        _view = nullptr;
    }
    for (auto& b : _bars) {
        b = nullptr;
    }
    _spec_info  = nullptr;
    _tone_label = nullptr;
}

void AppAudioTest::build_menu_view()
{
    destroy_view();
    _view_kind = View::Menu;

    _view = lv_obj_create(_root->get());
    lv_obj_set_size(_view, 320, 240);
    lv_obj_set_style_bg_opa(_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_view, 0, 0);
    lv_obj_set_style_pad_all(_view, 0, 0);
    lv_obj_clear_flag(_view, LV_OBJ_FLAG_SCROLLABLE);

    make_label(_view, &lv_font_montserrat_24, 0xFFFFFF, LV_ALIGN_TOP_MID, 0, 24, "AUDIO TEST");
    make_label(_view, &lv_font_montserrat_14, _text_dim, LV_ALIGN_TOP_MID, 0, 52, "choose a test");

    struct CardCtx {
        AppAudioTest* app;
        int mode;
    };
    static CardCtx ctx_spectrum{nullptr, 0};
    static CardCtx ctx_tones{nullptr, 0};
    ctx_spectrum  = {this, (int)TaskMode::Spectrum};
    ctx_tones     = {this, (int)TaskMode::Tones};

    auto card_cb = [](lv_event_t* e) {
        auto* ctx = static_cast<CardCtx*>(lv_event_get_user_data(e));
        ctx->app->_task_mode.store(ctx->mode);
        ctx->app->_tones_done.store(false);
        ctx->app->_tone_note.store(-1);
        LvglLockGuard lock;
        if (ctx->mode == (int)TaskMode::Spectrum) {
            ctx->app->build_spectrum_view();
        } else {
            ctx->app->build_tones_view();
        }
    };
    // NOTE: card_cb is a captureless lambda used as a plain C callback.

    lv_obj_t* c1 = make_card(_view, 16, 138, card_cb, &ctx_spectrum);
    make_label(c1, &lv_font_montserrat_16, 0xFFFFFF, LV_ALIGN_TOP_MID, 0, 26, "SPECTRUM");
    make_label(c1, &lv_font_montserrat_14, _text_dim, LV_ALIGN_TOP_MID, 0, 54, "live mic");
    make_label(c1, &lv_font_montserrat_14, _text_dim, LV_ALIGN_TOP_MID, 0, 74, "FFT");

    lv_obj_t* c2 = make_card(_view, 166, 138, card_cb, &ctx_tones);
    make_label(c2, &lv_font_montserrat_16, 0xFFFFFF, LV_ALIGN_TOP_MID, 0, 26, "TONES");
    make_label(c2, &lv_font_montserrat_14, _text_dim, LV_ALIGN_TOP_MID, 0, 54, "do re mi");
    make_label(c2, &lv_font_montserrat_14, _text_dim, LV_ALIGN_TOP_MID, 0, 74, "fa sol");
}

void AppAudioTest::build_spectrum_view()
{
    destroy_view();
    _view_kind     = View::Spectrum;
    _painted_peak  = -1;

    _view = lv_obj_create(_root->get());
    lv_obj_set_size(_view, 320, 240);
    lv_obj_set_style_bg_opa(_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_view, 0, 0);
    lv_obj_set_style_pad_all(_view, 0, 0);
    lv_obj_clear_flag(_view, LV_OBJ_FLAG_SCROLLABLE);

    _spec_info = make_label(_view, &lv_font_montserrat_14, _text_dim, LV_ALIGN_TOP_MID, 0, 8,
                            "peak: ---");
    for (int i = 0; i < kBandCount; i++) {
        _bar_height[i] = 0;
        _bars[i]       = lv_obj_create(_view);
        lv_obj_set_size(_bars[i], 7, 2);
        lv_obj_align(_bars[i], LV_ALIGN_BOTTOM_LEFT, 5 + i * 10, -34);
        lv_obj_set_style_bg_color(_bars[i], lv_color_hex(_accent), 0);
        lv_obj_set_style_bg_opa(_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(_bars[i], 0, 0);
        lv_obj_set_style_radius(_bars[i], 2, 0);
        lv_obj_clear_flag(_bars[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(_bars[i], LV_OBJ_FLAG_CLICKABLE);
    }
    make_label(_view, &lv_font_montserrat_14, 0x8899AA, LV_ALIGN_BOTTOM_MID, 0, -14,
               "tap to stop");

    // Invisible full-screen overlay: tap anywhere ends the spectrum test.
    lv_obj_t* overlay = lv_obj_create(_view);
    lv_obj_set_size(overlay, 320, 240);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, [](lv_event_t* e) {
        auto* self = static_cast<AppAudioTest*>(lv_event_get_user_data(e));
        self->_task_mode.store((int)TaskMode::Idle);
        LvglLockGuard lock;
        self->build_menu_view();
    }, LV_EVENT_CLICKED, this);
}

void AppAudioTest::build_tones_view()
{
    destroy_view();
    _view_kind = View::Tones;

    _view = lv_obj_create(_root->get());
    lv_obj_set_size(_view, 320, 240);
    lv_obj_set_style_bg_opa(_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_view, 0, 0);
    lv_obj_set_style_pad_all(_view, 0, 0);
    lv_obj_clear_flag(_view, LV_OBJ_FLAG_SCROLLABLE);

    make_label(_view, &lv_font_montserrat_16, _text_dim, LV_ALIGN_TOP_MID, 0, 44,
               "speaker test @ 20%");
    _tone_label = make_label(_view, &lv_font_montserrat_24, 0xFFFFFF, LV_ALIGN_CENTER, 0, -8,
                             "...");
}

// ---------------------------------------------------------------- UI tick

void AppAudioTest::ui_timer_cb(lv_timer_t*)
{
    // One instance per install; the static sidesteps the opaque lv_timer_t.
    if (s_active_instance != nullptr) {
        s_active_instance->on_ui_tick();
    }
}

void AppAudioTest::on_ui_tick()
{
    if (_view_kind == View::Spectrum) {
        float targets[kBandCount];
        int peak_band = -1;
        {
            std::lock_guard<std::mutex> lock(_spec_mtx);
            memcpy(targets, _bands, sizeof(targets));
            peak_band = _peak_band;
        }
        for (int i = 0; i < kBandCount; i++) {
            // Fast attack, slower decay: transients punch, bars fall smoothly
            const float t = targets[i] * 150.0f;
            const float k = t > _bar_height[i] ? 0.7f : 0.30f;
            _bar_height[i] += (t - _bar_height[i]) * k;
            const float h = std::max(2.0f, _bar_height[i]);
            lv_obj_set_height(_bars[i], (int32_t)h);
        }
        // Highlight the dominant band in white (visible while humming a note)
        if (peak_band != _painted_peak) {
            for (int i = 0; i < kBandCount; i++) {
                if (_bars[i] != nullptr) {
                    lv_obj_set_style_bg_color(_bars[i],
                                              lv_color_hex(i == peak_band ? 0xFFFFFF : _accent), 0);
                }
            }
            _painted_peak = peak_band;
        }
        const uint32_t now = GetHAL().millis();
        if (now - _last_info_ms > 250) {
            _last_info_ms = now;
            float hz = 0, db = -100;
            {
                std::lock_guard<std::mutex> lock(_spec_mtx);
                hz = _peak_hz;
                db = _peak_db;
            }
            char info[48];
            snprintf(info, sizeof(info), "peak: %.2f kHz  %d dB  lvl %d%%",
                     hz / 1000.0f, (int)db, _mic_level.load());
            lv_label_set_text(_spec_info, info);
        }
    } else if (_view_kind == View::Tones) {
        static const char* kNoteNames[9] = {"C5", "D5", "E5", "F5", "G5",
                                            "F5", "E5", "D5", "C5"};
        const int note = _tone_note.load();
        if (_tone_label != nullptr && note >= 0 && note < 9) {
            lv_label_set_text(_tone_label, kNoteNames[note]);
        }
        if (_tones_done.load()) {
            _tones_done.store(false);
            build_menu_view();
        }
    }
}

void AppAudioTest::onRunning()
{
    if (_closing) {
        if (_task_exited.load()) {
            reclaim_audio_task();
            close();
        }
        return;
    }
    LvglLockGuard lock;

    view::update_home_indicator();

    if (_ui_timer == nullptr) {
        // 20 ms -> 50 FPS animation
        _ui_timer = lv_timer_create(ui_timer_cb, 20, this);
    }
}

void AppAudioTest::onSleeping()
{
    if (_closing && _task_exited.load()) {
        onClose();
    }
}

void AppAudioTest::reclaim_audio_task()
{
    if (_audio_task_handle != nullptr && _task_exited.load()) {
        vTaskDeleteWithCaps(_audio_task_handle);
        _audio_task_handle = nullptr;
    }
}

void AppAudioTest::onClose()
{
    _closing = true;
    _task_mode.store((int)TaskMode::Idle);
    _task_running.store(false);
    if (_audio_task_handle != nullptr && !_task_exited.load()) {
        return;
    }
    reclaim_audio_task();

    mclog::tagInfo(getAppInfo().name, "on close");

    _task_mode.store((int)TaskMode::Idle);
    _task_running.store(false);
    _fft.reset();

    {
        LvglLockGuard lock;
        if (_ui_timer != nullptr) {
            lv_timer_delete(_ui_timer);
            _ui_timer = nullptr;
        }
        destroy_view();
        _root.reset();
        view::destroy_home_indicator();
    }
    if (s_active_instance == this) {
        s_active_instance = nullptr;
    }
    _closing = false;
}

// ---------------------------------------------------------------- audio

// C5 D5 E5 F5 G5 F5 E5 D5 C5 (do re mi fa sol fa mi re do)
static constexpr float kScale[9] = {523.25f, 587.33f, 659.25f, 698.46f, 783.99f,
                                    698.46f, 659.25f, 587.33f, 523.25f};
static constexpr int kNoteMs = 300;
static constexpr int kGapMs  = 80;

void AppAudioTest::run_tones_mode()
{
    auto* codec = audio_test::codec();
    if (codec == nullptr) {
        _tones_done.store(true);
        return;
    }

    _saved_volume = GetHAL().getSpeakerVolume();
    _volume_saved = true;
    GetHAL().setSpeakerVolume(20);  // user-requested test volume

    {
        std::lock_guard<std::mutex> lock(audio_test::codec_mutex());
        codec->EnableOutput(true);
        codec->Start();
    }

    const int rate       = codec->output_sample_rate();
    const int note_len   = rate / 1000 * kNoteMs;
    const int gap_len    = rate / 1000 * kGapMs;
    const int envelope   = rate / 1000 * 25;  // 25 ms attack/release
    std::vector<int16_t> gap(gap_len, 0);

    for (int n = 0; n < 9 && _task_mode.load() == (int)TaskMode::Tones; n++) {
        _tone_note.store(n);
        std::vector<int16_t> pcm(note_len);
        const float step = 2.0f * (float)M_PI * kScale[n] / rate;
        float phase      = 0;
        for (int i = 0; i < note_len; i++) {
            float env = 1.0f;
            if (i < envelope) {
                env = (float)i / envelope;
            } else if (i > note_len - envelope) {
                env = (float)(note_len - i) / envelope;
            }
            const float s = sinf(phase) * env * 12000.0f;
            pcm[i]        = (int16_t)(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
            phase += step;
        }
        codec->OutputData(pcm);
        if (_task_mode.load() != (int)TaskMode::Tones) {
            break;
        }
        codec->OutputData(gap);
    }

    _tone_note.store(-1);
    {
        std::lock_guard<std::mutex> lock(audio_test::codec_mutex());
        codec->EnableOutput(false);
    }
    GetHAL().setSpeakerVolume(_saved_volume);
    _volume_saved = false;
    _tones_done.store(true);
}

void AppAudioTest::run_spectrum_mode()
{
    auto* codec = audio_test::codec();
    if (codec == nullptr) {
        return;
    }
    if (!_fft) {
        _fft = std::make_unique<AudioTestFft>();
    }

    {
        std::lock_guard<std::mutex> lock(audio_test::codec_mutex());
        codec->EnableInput(true);
        // The duplex I2S bus is clocked from the output side — opening the output
        // only starts the clocks, nothing is played (and nothing feeds back).
        codec->EnableOutput(true);
        codec->Start();
    }

    // Warm-up (mirrors the official AudioService): drop the first chunks
    // after enabling, the codec's power-on transient would dominate them.
    const int rate = codec->input_sample_rate();
    const size_t in_chunk = rate / 1000 * 30;   // interleaved stereo
    std::vector<int16_t> in_buf(in_chunk);
    for (int i = 0; i < 7 && _task_mode.load() == (int)TaskMode::Spectrum; i++) {
        codec->InputData(in_buf);
    }

    std::vector<float> window(kFftN);
    for (int i = 0; i < kFftN; i++) {
        window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (kFftN - 1)));  // Hann
    }
    float re[kFftN], im[kFftN];
    std::vector<float> mic;  // gained mono samples
    mic.reserve(kFftN * 2);

    while (_task_mode.load() == (int)TaskMode::Spectrum) {
        if (!codec->InputData(in_buf)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        // De-interleave slot 0 (mic), same x16 digital gain as the /mic stream
        const size_t half = in_buf.size() / 2;
        int peak          = 0;
        for (size_t i = 0; i < half; i++) {
            int32_t v = (int32_t)in_buf[i * 2] * 16;
            v         = std::min<int32_t>(32767, std::max<int32_t>(-32768, v));
            mic.push_back((float)v);
            const int a = (int)v < 0 ? -(int)v : (int)v;
            if (a > peak) {
                peak = a;
            }
        }
        _mic_level.store(std::min(100, peak / 328));

        while (mic.size() >= (size_t)kFftN) {
            // Remove DC before windowing: a DC offset would leak through the
            // window sidelobes into every band.
            float mean = 0;
            for (int i = 0; i < kFftN; i++) {
                mean += mic[i];
            }
            mean /= kFftN;
            for (int i = 0; i < kFftN; i++) {
                re[i] = (mic[i] - mean) * window[i];
                im[i] = 0;
            }
            mic.erase(mic.begin(), mic.begin() + kFftN);
            _fft->run(re, im);

            // 32 log-spaced bands. mag/(N/4) is the equivalent sine amplitude
            // (int16 scale), so dB = 20*log10(amplitude) spans roughly
            // 20 dB (silence, ~10 counts) .. 80 dB (loud, ~10k counts).
            float bands[kBandCount];
            float best_mag = 0;
            int best_bin   = 0;
            for (int b = 0; b < kBandCount; b++) {
                const float f_lo = band_edge(b);
                const float f_hi = band_edge(b + 1);
                const int bin_lo = std::max(1, (int)(f_lo * kFftN / rate));
                const int bin_hi = std::max(bin_lo + 1, (int)(f_hi * kFftN / rate));
                float m = 0;
                for (int k = bin_lo; k < bin_hi && k < kFftN / 2; k++) {
                    const float mag = sqrtf(re[k] * re[k] + im[k] * im[k]);
                    if (mag > m) {
                        m = mag;
                    }
                    if (mag > best_mag) {
                        best_mag = mag;
                        best_bin = k;
                    }
                }
                const float amp = m / (kFftN / 4.0f);
                const float db  = 20.0f * log10f(amp + 1.0f);
                // 30..78 dB window: narrower than a full dynamic range so a
                // hummed note's fundamental stands out sharply from the noise
                // floor around it.
                bands[b] = std::min(1.0f, std::max(0.0f, (db - 30.0f) / 48.0f));
            }

            {
                std::lock_guard<std::mutex> lock(_spec_mtx);
                memcpy(_bands, bands, sizeof(bands));
                int strongest = 0;
                for (int b = 1; b < kBandCount; b++) {
                    if (bands[b] > bands[strongest]) {
                        strongest = b;
                    }
                }
                _peak_band = bands[strongest] > 0.25f ? strongest : -1;
                _peak_hz   = (float)best_bin * rate / kFftN;
                _peak_db   = 20.0f * log10f(best_mag / (kFftN / 4.0f) + 1.0f);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(audio_test::codec_mutex());
        codec->EnableInput(false);
        codec->EnableOutput(false);
    }
}

void AppAudioTest::audio_task(void* arg)
{
    auto* self = static_cast<AppAudioTest*>(arg);
    while (self->_task_running.load()) {
        const int mode = self->_task_mode.load();
        if (mode == (int)TaskMode::Spectrum) {
            self->run_spectrum_mode();
        } else if (mode == (int)TaskMode::Tones) {
            self->run_tones_mode();
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    self->_task_exited.store(true);
    // WithCaps tasks must not self-delete: park, the owner frees us.
    vTaskDelay(portMAX_DELAY);
}
