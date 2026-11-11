/*
 * SEP-0002: firmware automation interface
 *  - serial (USB-JTAG) line commands: apps / launch <NAME> / server <url> / server? / reset
 *  - HTTP:  GET /sep/health  GET /sep/apps  GET /sep/app?name=X  GET/PUT /sep/server
 *  - StackChan server URL override in NVS (namespace "sep", key "server_url")
 * See keps/0002 in the sep workspace for the design.
 */
#pragma once

#include <string>

namespace sep {

// Call once after all mooncake apps are installed (main.cpp).
void init();

// Call from the launcher main loop; executes queued app-launch requests
// on the mooncake-owned thread.
void poll();

// NVS-backed server URL override ("" = not set). Used by secret_logic.
std::string serverUrlOverride();
void setServerUrlOverride(const std::string& url);

}  // namespace sep
