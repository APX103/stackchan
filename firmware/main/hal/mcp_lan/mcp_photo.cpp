/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "mcp_photo.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <jpg/image_to_jpeg.h>
#include <mbedtls/base64.h>

#include <cstring>
#include <mutex>

#include "device_access.h"

static const char* TAG = "McpPhoto";

namespace {

struct PhotoStore {
    std::mutex mtx;
    uint8_t* jpeg       = nullptr;  // PSRAM
    size_t len          = 0;
    int w               = 0;
    int h               = 0;
    int quality_value   = 0;
    uint32_t captures   = 0;
};

PhotoStore& store()
{
    static PhotoStore s;
    return s;
}

void free_photo_locked(PhotoStore& s)
{
    if (s.jpeg != nullptr) {
        heap_caps_free(s.jpeg);
        s.jpeg = nullptr;
    }
    s.len = 0;
}

}  // namespace

namespace mcp_photo {

bool capture(int quality, std::string* err)
{
    auto* camera = mcp_dev::camera();
    if (camera == nullptr) {
        *err = "camera not available on this board";
        return false;
    }
    if (!camera->StreamCaptures()) {
        *err = "camera capture failed";
        return false;
    }

    const uint8_t* frame = camera->GetFrameData();
    size_t frame_size    = camera->GetFrameSize();
    const int w          = camera->GetFrameWidth();
    const int h          = camera->GetFrameHeight();
    const int format     = camera->GetFrameFormat();
    if (frame == nullptr || frame_size == 0) {
        *err = "camera returned an empty frame";
        return false;
    }

    uint8_t* jpeg  = nullptr;
    size_t jpeg_len = 0;
    const bool ok  = image_to_jpeg((uint8_t*)frame, frame_size, w, h, (v4l2_pix_fmt_t)format, quality,
                                   &jpeg, &jpeg_len);
    if (!ok || jpeg == nullptr || jpeg_len == 0) {
        free(jpeg);
        *err = "jpeg encode failed";
        return false;
    }

    auto& s = store();
    uint8_t* copy = (uint8_t*)heap_caps_malloc(jpeg_len, MALLOC_CAP_SPIRAM);
    if (copy == nullptr) {
        free(jpeg);
        *err = "out of memory for photo buffer";
        return false;
    }
    memcpy(copy, jpeg, jpeg_len);
    free(jpeg);

    std::lock_guard<std::mutex> lock(s.mtx);
    free_photo_locked(s);
    s.jpeg          = copy;
    s.len           = jpeg_len;
    s.w             = w;
    s.h             = h;
    s.quality_value = quality;
    s.captures++;
    return true;
}

bool get(const uint8_t** data, size_t* len)
{
    auto& s = store();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (s.jpeg == nullptr) {
        return false;
    }
    *data = s.jpeg;
    *len  = s.len;
    return true;
}

int width()
{
    return store().w;
}

int height()
{
    return store().h;
}

int quality()
{
    return store().quality_value;
}

uint32_t count()
{
    return store().captures;
}

std::string base64(std::string* err)
{
    auto& s = store();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (s.jpeg == nullptr || s.len == 0) {
        *err = "no photo captured yet";
        return {};
    }
    size_t need = 0;
    if (mbedtls_base64_encode(nullptr, 0, &need, s.jpeg, s.len) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        *err = "base64 size check failed";
        return {};
    }
    std::string out(need, '\0');
    size_t written = 0;
    if (mbedtls_base64_encode((uint8_t*)out.data(), out.size(), &written, s.jpeg, s.len) != 0) {
        *err = "base64 encode failed";
        return {};
    }
    out.resize(written);
    return out;
}

}  // namespace mcp_photo
