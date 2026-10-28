/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "mcp_photo.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <jpg/image_to_jpeg.h>
#include <mbedtls/base64.h>

#include <cstring>
#include <mutex>

#include "../platform/device_access.h"

static const char* TAG = "McpPhoto";

namespace {

struct PhotoStore {
    std::mutex mtx;
    std::shared_ptr<mcp_photo::PhotoBytes> jpeg;
    int w = 0;
    int h = 0;
    int quality_value = 0;
    uint32_t captures = 0;
};

PhotoStore& store()
{
    static PhotoStore s;
    return s;
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
    const size_t frame_size = camera->GetFrameSize();
    const int w = camera->GetFrameWidth();
    const int h = camera->GetFrameHeight();
    const int format = camera->GetFrameFormat();
    if (frame == nullptr || frame_size == 0) {
        *err = "camera returned an empty frame";
        return false;
    }

    uint8_t* jpeg = nullptr;
    size_t jpeg_len = 0;
    const bool ok = image_to_jpeg((uint8_t*)frame, frame_size, w, h, (v4l2_pix_fmt_t)format, quality,
                                  &jpeg, &jpeg_len);
    if (!ok || jpeg == nullptr || jpeg_len == 0) {
        free(jpeg);
        *err = "jpeg encode failed";
        return false;
    }

    std::shared_ptr<mcp_photo::PhotoBytes> copy;
    try {
        copy = std::make_shared<PhotoBytes>(jpeg_len);
    } catch (const std::bad_alloc&) {
        free(jpeg);
        *err = "out of memory for photo buffer";
        return false;
    }
    memcpy(copy->data(), jpeg, jpeg_len);
    free(jpeg);

    auto& s = store();
    std::lock_guard<std::mutex> lock(s.mtx);
    s.jpeg = std::move(copy);
    s.w = w;
    s.h = h;
    s.quality_value = quality;
    s.captures++;
    return true;
}

bool get(PhotoSnapshot* out)
{
    if (out == nullptr) {
        return false;
    }
    auto& s = store();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (!s.jpeg) {
        return false;
    }
    out->bytes = s.jpeg;
    out->width = s.w;
    out->height = s.h;
    out->quality = s.quality_value;
    out->captures = s.captures;
    return true;
}

void clear()
{
    auto& s = store();
    std::lock_guard<std::mutex> lock(s.mtx);
    s.jpeg.reset();
    s.w = 0;
    s.h = 0;
    s.quality_value = 0;
}

int width()
{
    auto& s = store();
    std::lock_guard<std::mutex> lock(s.mtx);
    return s.w;
}

int height()
{
    auto& s = store();
    std::lock_guard<std::mutex> lock(s.mtx);
    return s.h;
}

int quality()
{
    auto& s = store();
    std::lock_guard<std::mutex> lock(s.mtx);
    return s.quality_value;
}

uint32_t count()
{
    auto& s = store();
    std::lock_guard<std::mutex> lock(s.mtx);
    return s.captures;
}

std::string base64(std::string* err)
{
    auto& s = store();
    std::lock_guard<std::mutex> lock(s.mtx);
    if (!s.jpeg || s.jpeg->empty()) {
        *err = "no photo captured yet";
        return {};
    }
    size_t need = 0;
    if (mbedtls_base64_encode(nullptr, 0, &need, s.jpeg->data(), s.jpeg->size()) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        *err = "base64 size check failed";
        return {};
    }
    std::string out(need, '\0');
    size_t written = 0;
    if (mbedtls_base64_encode((uint8_t*)out.data(), out.size(), &written, s.jpeg->data(), s.jpeg->size()) != 0) {
        *err = "base64 encode failed";
        return {};
    }
    out.resize(written);
    return out;
}

}  // namespace mcp_photo
