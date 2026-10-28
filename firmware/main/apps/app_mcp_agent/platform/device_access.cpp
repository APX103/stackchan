/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "device_access.h"

#include <board.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <wifi_manager.h>
#include <wifi_station.h>

#include "apps/common/audio/codec_access.h"

namespace mcp_dev {

std::mutex& codec_mutex()
{
    return common_audio::codec_mutex();
}

AudioCodec* audio_codec(const char* consumer)
{
    return common_audio::codec(consumer);
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
