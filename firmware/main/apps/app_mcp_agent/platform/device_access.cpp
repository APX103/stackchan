/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "device_access.h"

#include <board.h>
#include <esp_log.h>
#include <esp_system.h>
#include <wifi_manager.h>
#include <wifi_station.h>

namespace mcp_dev {

std::mutex& codec_mutex()
{
    static std::mutex mutex;
    return mutex;
}

AudioCodec* audio_codec()
{
    // Single vendored-repo call in the whole MCP Agent feature: the audio
    // codec instance is owned by the board singleton (both the class
    // CoreS3AudioCodec and this accessor live in this repo, see
    // hal/board/stackchan.cc M5StackCoreS3Board::GetAudioCodec).
    return Board::GetInstance().GetAudioCodec();
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

void wifi_set_no_power_save()
{
    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
}

}  // namespace mcp_dev
