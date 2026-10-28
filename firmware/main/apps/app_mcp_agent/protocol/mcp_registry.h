/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cJSON.h>

#include <functional>
#include <string>
#include <vector>

namespace mcp_lan {

// Result of one tool invocation. Tool bodies run serialized on the executor
// task (see lan_service), never on the httpd task.
struct ToolOutput {
    bool is_error = false;
    std::string text;       // text content, usually a JSON string
    std::string image_b64;  // optional base64 JPEG, becomes an image content
};

// One MCP tool. `input_schema` is the JSON *properties object* for
// inputSchema, e.g. R"({"yaw":{"type":"integer","minimum":-128,"maximum":128}})".
// Keep it terse: tools/list pages are capped (internal RAM pressure makes
// large single HTTP responses hit EAGAIN mid-send on this board).
struct Tool {
    const char* name        = nullptr;
    const char* description = nullptr;
    std::string input_schema;
    std::function<ToolOutput(const cJSON* args)> invoke;
};

// Standalone MCP tool registry (deliberately independent from the vendored
// xiaozhi McpServer singleton — the cloud agent keeps its own).
class Registry {
public:
    void add(Tool tool);
    const Tool* find(const char* name) const;

    // Runs a tool by name on the caller's thread with exception containment.
    // Returns false + fills `err` when the tool is unknown.
    bool call(const char* name, const cJSON* args, ToolOutput* out, std::string* err);

    // Builds one tools/list "tools" array page starting at `cursor` (tool
    // index). Sets *next_cursor to the next index or -1 when done.
    cJSON* build_tools_page(int cursor, int* next_cursor) const;

    size_t size() const
    {
        return tools_.size();
    }

private:
    std::vector<Tool> tools_;
};

class LanService;

// Registers the full LAN tool set into the service-owned registry.
void register_all_tools(LanService& service);

}  // namespace mcp_lan
