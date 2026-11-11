/*
 * SEP-0008: camera preview app — live view of what the GC0308 sees.
 *
 * No storage, no streaming: capture -> YUV422->RGB565 -> LVGL image, ~10 fps.
 */
#include "app_camera_test.h"

#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <inttypes.h>
#include <lvgl.h>

#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <assets/assets.h>
#include <apps/common/common.h>
#include <smooth_ui_toolkit.hpp>
#include <mooncake_log.h>

using namespace mooncake;
using namespace smooth_ui_toolkit;

static uint32_t now_ms()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// YUYV(YUV422 packed) -> RGB565, two pixels per iteration.
static void yuyv_to_rgb565(const uint8_t* src, uint16_t* dst, int w, int h)
{
    const size_t pixels = (size_t)w * h;
    for (size_t i = 0; i < pixels; i += 2) {
        const int y0 = src[i * 2 + 0];
        const int u  = src[i * 2 + 1] - 128;
        const int y1 = src[i * 2 + 2];
        const int v  = src[i * 2 + 3] - 128;

        int r = y0 + (91881 * v >> 16);
        int g = y0 - ((22554 * u + 46802 * v) >> 16);
        int b = y0 + (116130 * u >> 16);
        uint16_t p0 = (uint16_t)(((r > 255 ? 255 : r < 0 ? 0 : r) & 0xF8) << 8 |
                                 ((g > 255 ? 255 : g < 0 ? 0 : g) & 0xFC) << 3 |
                                 ((b > 255 ? 255 : b < 0 ? 0 : b) >> 3));

        r = y1 + (91881 * v >> 16);
        g = y1 - ((22554 * u + 46802 * v) >> 16);
        b = y1 + (116130 * u >> 16);
        uint16_t p1 = (uint16_t)(((r > 255 ? 255 : r < 0 ? 0 : r) & 0xF8) << 8 |
                                 ((g > 255 ? 255 : g < 0 ? 0 : g) & 0xFC) << 3 |
                                 ((b > 255 ? 255 : b < 0 ? 0 : b) >> 3));

        dst[i]     = p0;
        dst[i + 1] = p1;
    }
}

AppCameraTest::AppCameraTest()
{
    setAppInfo().name = "CAMERA";
    static auto icon  = assets::get_image("icon_camera_test.bin");
    setAppInfo().icon = (void*)&icon;
    static uint32_t theme_color = 0x39C5BB;
    setAppInfo().userData       = (void*)&theme_color;
}

void AppCameraTest::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    auto camera = hal_bridge::board_get_camera();
    const int w = camera ? camera->GetFrameWidth() : 0;
    const int h = camera ? camera->GetFrameHeight() : 0;

    {
        LvglLockGuard lock;
        view::create_home_indicator([this]() { GetMooncake().closeApp(getID()); },
                                    0x39C5BB, 0x062A28);
        view::create_status_bar(0x39C5BB, 0x062A28);

        _img = lv_image_create(lv_screen_active());
        lv_obj_center((lv_obj_t*)_img);
        lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x000000), 0);

        _fps_label = lv_label_create(lv_screen_active());
        lv_label_set_text((lv_obj_t*)_fps_label, camera ? "..." : "camera unavailable");
        lv_obj_set_style_text_color((lv_obj_t*)_fps_label, lv_color_hex(0x8899AA), 0);
        lv_obj_align((lv_obj_t*)_fps_label, LV_ALIGN_BOTTOM_LEFT, 8, -8);

        if (camera && w > 0 && h > 0) {
            _rgb565  = heap_caps_calloc(1, (size_t)w * h * 2, MALLOC_CAP_SPIRAM);
            _img_dsc = heap_caps_calloc(1, sizeof(lv_image_dsc_t), MALLOC_CAP_INTERNAL);
            auto* dsc     = (lv_image_dsc_t*)_img_dsc;
            dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
            dsc->header.cf    = LV_COLOR_FORMAT_RGB565;
            dsc->header.w     = (uint32_t)w;
            dsc->header.h     = (uint32_t)h;
            dsc->data         = (const uint8_t*)_rgb565;
            dsc->data_size    = (uint32_t)((size_t)w * h * 2);
            lv_image_set_src((lv_obj_t*)_img, dsc);
            lv_obj_invalidate((lv_obj_t*)_img);
        }
    }

    _last_ms     = now_ms();
    _fps_tick_ms = _last_ms;
}

void AppCameraTest::onRunning()
{
    if (_rgb565 == nullptr) return;

    // ~10 fps: conversion + LVGL invalidate at this pace is plenty for a test view
    const uint32_t t = now_ms();
    if (t - _last_ms < 100) return;
    _last_ms = t;

    auto camera = hal_bridge::board_get_camera();
    if (camera == nullptr) return;

    {
        auto frame_lock = camera->TryLockFrame();
        if (!frame_lock.owns_lock()) return;
        if (!camera->StreamCaptures()) return;
        yuyv_to_rgb565(camera->GetFrameData(), (uint16_t*)_rgb565,
                       camera->GetFrameWidth(), camera->GetFrameHeight());
    }

    {
        LvglLockGuard lock;
        lv_obj_invalidate((lv_obj_t*)_img);
        ++_frames;
        ++_fps_count;
        if (t - _fps_tick_ms >= 1000) {
            lv_label_set_text_fmt((lv_obj_t*)_fps_label, "%" PRIu32 " fps (frames %" PRIu32 ")",
                                  _fps_count, _frames);
            _fps_count  = 0;
            _fps_tick_ms = t;
        }
    }
}

void AppCameraTest::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close, %" PRIu32 " frames shown", _frames);

    {
        LvglLockGuard lock;
        view::destroy_home_indicator();
        view::destroy_status_bar();
    }

    if (_rgb565 != nullptr) {
        heap_caps_free(_rgb565);
        _rgb565 = nullptr;
    }
    if (_img_dsc != nullptr) {
        heap_caps_free(_img_dsc);
        _img_dsc = nullptr;
    }
    _img = _fps_label = nullptr;
    _frames           = 0;
}
