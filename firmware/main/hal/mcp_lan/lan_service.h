/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <esp_http_server.h>

#include <cstdint>
#include <functional>
#include <string>

// Lifecycle orchestrator of the MCP Agent app: one esp_http_server instance
// (POST /mcp, GET /, GET /photo.jpg, WS /mic, WS /speaker) plus a dedicated
// tool-executor task. Started when the MCP.AGENT app opens, stopped (and all
// resources released) when it closes.
namespace mcp_lan {

struct ServiceStatus {
    bool running      = false;
    int port          = 0;
    std::string ip;
    size_t tools      = 0;
    uint32_t tool_calls  = 0;
    uint32_t tool_errors = 0;
    bool mic_active   = false;
    bool spk_active   = false;
    uint32_t photos   = 0;
    std::string last_event;
};

// Idempotent. Returns false with `err` filled when the server could not be
// brought up (Wi-Fi does NOT need to be connected — the server survives
// outages and becomes reachable again on reconnect).
bool service_start(std::string* err = nullptr);
void service_stop();
bool service_running();

ServiceStatus service_status();

// Runs `body` serialized on the tool-executor task. Returns false on timeout
// (the body may still run afterwards — it must own everything it touches).
bool service_execute(const std::function<void()>& body, uint32_t timeout_ms);

// Stats hooks (called from the JSON-RPC layer).
void service_note_tool_result(bool is_error);

// Record a human-readable event line for the app status page.
void service_push_event(const char* fmt, ...);

// Owner tool (self.reboot) sets this; the JSON-RPC layer reboots after the
// HTTP reply has been sent.
void service_request_reboot();
bool service_consume_reboot_request();

// Shared auth check for the WebSocket endpoints (?token= query parameter).
bool service_ws_authorized(httpd_req_t* req);

// "http://<ip>:<port>" for tool result URLs (empty string when offline).
std::string service_base_url();

}  // namespace mcp_lan
