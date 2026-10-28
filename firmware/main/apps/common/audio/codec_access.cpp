/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "codec_access.h"

#include <board.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

namespace common_audio {

std::mutex& codec_mutex()
{
    static std::mutex mutex;
    return mutex;
}

AudioCodec* codec(const char* consumer)
{
    static AudioCodec* cached_codec = nullptr;
    std::lock_guard<std::mutex> lock(codec_mutex());
    if (cached_codec != nullptr) {
        return cached_codec;
    }

    constexpr uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    constexpr size_t min_free = 48 * 1024;
    constexpr size_t min_largest = 16 * 1024;
    const size_t free = heap_caps_get_free_size(caps);
    const size_t largest = heap_caps_get_largest_free_block(caps);
    ESP_LOGI("CodecAccess", "%s: codec init preflight: internal free=%u, largest=%u", consumer,
             (unsigned)free, (unsigned)largest);
    if (free < min_free || largest < min_largest) {
        ESP_LOGE("CodecAccess",
                 "%s: refusing codec init: need at least %u free and a %u-byte contiguous block; "
                 "the HAL constructor aborts on allocation failure",
                 consumer, (unsigned)min_free, (unsigned)min_largest);
        return nullptr;
    }

    ESP_LOGI("CodecAccess", "%s: entering Board::GetAudioCodec()", consumer);
    cached_codec = Board::GetInstance().GetAudioCodec();
    if (cached_codec == nullptr) {
        ESP_LOGE("CodecAccess", "%s: board returned no audio codec", consumer);
        return nullptr;
    }
    ESP_LOGI("CodecAccess", "%s: codec init completed: internal free=%u, largest=%u", consumer,
             (unsigned)heap_caps_get_free_size(caps), (unsigned)heap_caps_get_largest_free_block(caps));
    return cached_codec;
}

}  // namespace common_audio
