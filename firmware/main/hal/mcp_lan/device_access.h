/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <string>

// Hardware facade for the MCP Agent (LAN MCP server).
//
// Isolation rule for this feature: MCP Agent code must not depend on the
// vendored xiaozhi-esp32 application layer. All hardware singletons are
// obtained here, and this is the ONLY mcp_lan file allowed to include
// vendored headers ("audio_codec.h" for the codec type; "board.h" inside
// device_access.cpp for the codec instance). Everything else goes through
// parent-repo APIs (hal_bridge / Hal / StackChan) or Espressif components.
#include "hal/board/hal_bridge.h"
#include "audio_codec.h"

namespace mcp_dev {

// The board's audio codec singleton (CoreS3AudioCodec, implemented in this
// repo). Returns nullptr if the board has none.
AudioCodec* audio_codec();

// The camera singleton (parent repo class, may be nullptr).
StackChanCamera* camera();

// Wi-Fi state via the 78/esp-wifi-connect managed component (not xiaozhi).
bool wifi_connected();
std::string wifi_ip();
std::string wifi_ssid();
int wifi_rssi();

}  // namespace mcp_dev
