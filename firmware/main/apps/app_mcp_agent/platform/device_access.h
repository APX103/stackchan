/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <mutex>
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

// Serializes all codec CONTROL calls (EnableInput/EnableOutput/Start and
// friends). The mic and speaker stream tasks initialize their codec sides
// concurrently after service start; without this lock their I2C register
// sequences interleave, a codec command fails, and the ESP_ERROR_CHECKs in
// the codec driver abort the system (observed as a silent reboot with a
// core dump right after Wi-Fi connects). Data calls (InputData/OutputData)
// use separate I2S rx/tx channels and do NOT need this lock.
std::mutex& codec_mutex();

// The camera singleton (parent repo class, may be nullptr).
StackChanCamera* camera();

// Starts network initialization without waiting for a connection.
void wifi_start();
// Used only during the MCP-to-AI handoff. A fresh StartNetwork() is needed
// because an already-active Station does not emit Connected again.
void wifi_stop_for_ai_handoff();
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
