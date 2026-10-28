/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <esp_http_server.h>

#include <string>

#include "mcp_registry.h"

namespace mcp_lan {

class LanService;

// Registers POST /mcp with the service instance as the request user context.
bool register_mcp_endpoint(httpd_handle_t server, LanService& service);

// Shared response builders used by the HTTP parser and the async executor.
std::string make_tool_result_json(cJSON* id, const ToolOutput& output);
std::string make_error_json(cJSON* id, int code, const char* message);

}  // namespace mcp_lan
