/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "mcp_registry.h"

#include <esp_log.h>

#include <exception>

static const char* TAG = "McpReg";

// tools/list page cap. ~8 tools per page keeps a page well under 3 KB.
constexpr size_t kToolsPerPage = 8;

namespace mcp_lan {

void Registry::add(Tool tool)
{
    if (tool.name == nullptr || tool.invoke == nullptr) {
        ESP_LOGE(TAG, "refusing malformed tool registration");
        return;
    }
    if (find(tool.name) != nullptr) {
        // Idempotent registration: replace in place.
        for (auto& t : tools_) {
            if (std::string(t.name) == tool.name) {
                t = std::move(tool);
                return;
            }
        }
    }
    tools_.push_back(std::move(tool));
}

const Tool* Registry::find(const char* name) const
{
    for (const auto& t : tools_) {
        if (std::string(t.name) == name) {
            return &t;
        }
    }
    return nullptr;
}

bool Registry::call(const char* name, const cJSON* args, ToolOutput* out, std::string* err)
{
    const Tool* tool = find(name);
    if (tool == nullptr) {
        if (err != nullptr) {
            *err = std::string("unknown tool: ") + name;
        }
        return false;
    }
    try {
        *out = tool->invoke(args);
    } catch (const std::exception& e) {
        out->is_error = true;
        out->text     = std::string("tool exception: ") + e.what();
        out->image_b64.clear();
    } catch (...) {
        out->is_error = true;
        out->text     = "tool exception: unknown";
        out->image_b64.clear();
    }
    return true;
}

cJSON* Registry::build_tools_page(int cursor, int* next_cursor) const
{
    *next_cursor = -1;
    cJSON* array = cJSON_CreateArray();
    if (cursor < 0 || (size_t)cursor >= tools_.size()) {
        return array;
    }

    size_t end = (size_t)cursor + kToolsPerPage;
    if (end > tools_.size()) {
        end = tools_.size();
    } else {
        *next_cursor = (int)end;
    }

    for (size_t i = (size_t)cursor; i < end; ++i) {
        const auto& t = tools_[i];
        cJSON* item   = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", t.name);
        cJSON_AddStringToObject(item, "description", t.description);
        cJSON* schema = cJSON_CreateObject();
        cJSON_AddStringToObject(schema, "type", "object");
        cJSON* props = cJSON_Parse(t.input_schema.c_str());
        if (props == nullptr) {
            props = cJSON_CreateObject();
        }
        cJSON_AddItemToObject(schema, "properties", props);
        cJSON_AddItemToObject(item, "inputSchema", schema);
        cJSON_AddItemToArray(array, item);
    }
    return array;
}

}  // namespace mcp_lan
