/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <esp_http_server.h>

namespace mcp_lan {

// Registers POST /mcp (MCP Streamable HTTP, stateless JSON-RPC 2.0) on the
// service's httpd instance. Returns false if registration fails.
bool register_mcp_endpoint(httpd_handle_t server);

}  // namespace mcp_lan
