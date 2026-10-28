/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "device_access.h"

#include <board.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <wifi_manager.h>
#include <wifi_station.h>

namespace mcp_dev {

std::mutex& codec_mutex()
{
    static std::mutex mutex;
    return mutex;
}

AudioCodec* audio_codec(const char* consumer)
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
    ESP_LOGI("McpDevice", "%s: codec init preflight: internal free=%u, largest=%u", consumer,
             (unsigned)free, (unsigned)largest);
    if (free < min_free || largest < min_largest) {
        ESP_LOGE("McpDevice",
                 "%s: refusing codec init: need at least %u free and a %u-byte contiguous block; "
                 "the HAL constructor aborts on allocation failure",
                 consumer, (unsigned)min_free, (unsigned)min_largest);
        return nullptr;
    }

    ESP_LOGI("McpDevice", "%s: entering Board::GetAudioCodec()", consumer);
    cached_codec = Board::GetInstance().GetAudioCodec();
    if (cached_codec == nullptr) {
        ESP_LOGE("McpDevice", "%s: board returned no audio codec", consumer);
        return nullptr;
    }
    ESP_LOGI("McpDevice", "%s: codec init completed: internal free=%u, largest=%u", consumer,
             (unsigned)heap_caps_get_free_size(caps), (unsigned)heap_caps_get_largest_free_block(caps));
    return cached_codec;
}

StackChanCamera* camera()
{
    return hal_bridge::board_get_camera();
}

void wifi_start()
{
    Board::GetInstance().StartNetwork();
}

void wifi_stop_for_ai_handoff()
{
    WifiManager::GetInstance().StopStation();
}

bool wifi_connected()
{
    return WifiManager::GetInstance().IsConnected();
}

std::string wifi_ip()
{
    return std::string(WifiManager::GetInstance().GetIpAddress());
}

std::string wifi_ssid()
{
    return std::string(WifiManager::GetInstance().GetSsid());
}

int wifi_rssi()
{
    return WifiManager::GetInstance().GetRssi();
}

static wifi_ps_mode_t saved_power_save_mode = WIFI_PS_NONE;
static bool has_saved_power_save_mode = false;

void wifi_set_no_power_save()
{
    if (!has_saved_power_save_mode) {
        const esp_err_t err = esp_wifi_get_ps(&saved_power_save_mode);
        if (err == ESP_OK) {
            has_saved_power_save_mode = true;
            ESP_LOGI("McpDevice", "saved Wi-Fi power-save mode %d", (int)saved_power_save_mode);
        } else {
            ESP_LOGW("McpDevice", "failed to read Wi-Fi power-save mode: %s", esp_err_to_name(err));
        }
    }
    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
}

void wifi_restore_power_save()
{
    if (!has_saved_power_save_mode) {
        return;
    }
    const wifi_ps_mode_t mode = saved_power_save_mode;
    has_saved_power_save_mode = false;
    const esp_err_t err = esp_wifi_set_ps(mode);
    if (err == ESP_OK) {
        ESP_LOGI("McpDevice", "restored Wi-Fi power-save mode %d", (int)mode);
    } else {
        ESP_LOGW("McpDevice", "failed to restore Wi-Fi power-save mode %d: %s", (int)mode, esp_err_to_name(err));
    }
}

}  // namespace mcp_dev
