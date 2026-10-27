/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "device_access.h"

#include <board.h>
#include <wifi_manager.h>

namespace mcp_dev {

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

}  // namespace mcp_dev
