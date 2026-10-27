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

// Forces construction of the audio codec singleton NOW. The codec (and its
// I2S channels + DMA buffers) is created lazily on first use; if that first
// use happens late in the boot (e.g. the MCP Agent app opening after Wi-Fi
// is up), the I2S channel allocation fails with NO_MEM and the
// ESP_ERROR_CHECK inside aborts the whole system. Called once from app_main.
void audio_codec_warmup();

// The camera singleton (parent repo class, may be nullptr).
StackChanCamera* camera();

// Wi-Fi state via the 78/esp-wifi-connect managed component (not xiaozhi).
bool wifi_connected();
std::string wifi_ip();
std::string wifi_ssid();
int wifi_rssi();
// Disables Wi-Fi modem sleep. While a LAN server must stay reachable, the
// default power save makes the device stop answering ARP within a minute
// of going idle.
void wifi_set_no_power_save();

}  // namespace mcp_dev
