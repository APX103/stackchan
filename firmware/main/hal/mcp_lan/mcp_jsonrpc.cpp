/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "mcp_jsonrpc.h"

#include <esp_log.h>
#include <sdkconfig.h>

#include <cstring>
#include <memory>
#include <string>

#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "lan_service.h"
#include "mcp_registry.h"

static const char* TAG = "McpRpc";

// Request body cap (httpd task's stack budget — internal RAM is scarce).
constexpr size_t kMaxBodySize = 32 * 1024;
// tools/call may legitimately take seconds (servo move, photo capture).
constexpr uint32_t kToolTimeoutMs = 30000;

namespace mcp_lan {

namespace {

const char* kServerName    = "stackchan-mcp";
const char* kServerVersion = "1.0.0";

// Sentinel markers so the dispatcher can pick the right JSON-RPC error code
// without hand-rolling a result struct through the cJSON world.
const char* const kUnknownTool = "\x01";
const char* const kTimeout     = "\x02";

bool token_configured()
{
    return strlen(CONFIG_MCP_AGENT_TOKEN) > 0;
}

// Constant-time-ish comparison for the bearer token.
bool token_matches(const char* header_value)
{
    const char* prefix = "Bearer ";
    if (header_value == nullptr) {
        return false;
    }
    if (strncmp(header_value, prefix, strlen(prefix)) != 0) {
        return false;
    }
    const char* presented = header_value + strlen(prefix);
    const char* expected  = CONFIG_MCP_AGENT_TOKEN;
    volatile char diff = 0;
    for (size_t i = 0; presented[i] != '\0' || expected[i] != '\0'; ++i) {
        diff |= (char)(presented[i] ^ expected[i]);
    }
    return diff == 0;
}

esp_err_t send_json(httpd_req_t* req, const std::string& body, const char* status = "200 OK")
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    return httpd_resp_send(req, body.c_str(), (ssize_t)body.size());
}

esp_err_t send_accepted(httpd_req_t* req)
{
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
    return httpd_resp_send(req, nullptr, 0);
}

std::string print(cJSON* node)
{
    char* s        = cJSON_PrintUnformatted(node);
    std::string out = s != nullptr ? s : "{}";
    cJSON_free(s);
    return out;
}

std::string build_error(cJSON* id, int code, const char* message)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddItemToObject(root, "id", id != nullptr ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON* err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(root, "error", err);
    std::string out = print(root);
    cJSON_Delete(root);
    return out;
}

// Wraps `result_json` (already a JSON value) into a full JSON-RPC response.
std::string wrap_result(cJSON* id, const std::string& result_json)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddItemToObject(root, "id", cJSON_Duplicate(id, 1));
    cJSON* parsed = cJSON_Parse(result_json.c_str());
    cJSON_AddItemToObject(root, "result", parsed != nullptr ? parsed : cJSON_CreateObject());
    std::string out = print(root);
    cJSON_Delete(root);
    return out;
}

std::string handle_initialize(const cJSON* params)
{
    static const char* kSupported[] = {"2024-11-05", "2025-03-26", "2025-06-18"};
    std::string version = "2025-03-26";
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(params, "protocolVersion");
    if (cJSON_IsString(v) && v->valuestring != nullptr) {
        for (const char* s : kSupported) {
            if (strcmp(s, v->valuestring) == 0) {
                version = s;
            }
        }
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", version.c_str());
    cJSON* caps      = cJSON_CreateObject();
    cJSON* tools_cap = cJSON_CreateObject();
    cJSON_AddBoolToObject(tools_cap, "listChanged", false);
    cJSON_AddItemToObject(caps, "tools", tools_cap);
    cJSON_AddItemToObject(result, "capabilities", caps);
    cJSON* info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", kServerName);
    cJSON_AddStringToObject(info, "version", kServerVersion);
    cJSON_AddItemToObject(result, "serverInfo", info);
    std::string out = print(result);
    cJSON_Delete(result);
    return out;
}

std::string handle_tools_list(const cJSON* params)
{
    int cursor = 0;
    const cJSON* c = cJSON_GetObjectItemCaseSensitive(params, "cursor");
    if (cJSON_IsString(c) && c->valuestring != nullptr) {
        cursor = atoi(c->valuestring);
    }
    int next  = -1;
    cJSON* tools = Registry::instance().build_tools_page(cursor, &next);
    cJSON* result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "tools", tools);
    if (next >= 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", next);
        cJSON_AddStringToObject(result, "nextCursor", buf);
    }
    std::string out = print(result);
    cJSON_Delete(result);
    return out;
}

// Executes one tools/call. Returns a JSON "result" object, or a sentinel
// marker (kUnknownTool / kTimeout) for the dispatcher to turn into a
// JSON-RPC error.
std::string handle_tools_call(const cJSON* params)
{
    const cJSON* name = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (!cJSON_IsString(name) || name->valuestring == nullptr) {
        return std::string(kUnknownTool) + "params.name must be a string";
    }
    const Tool* tool = Registry::instance().find(name->valuestring);
    if (tool == nullptr) {
        return std::string(kUnknownTool) + "unknown tool: " + name->valuestring;
    }
    const cJSON* args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    if (args != nullptr && !cJSON_IsObject(args)) {
        args = nullptr;
    }

    // The closure outlives this frame when the tool exceeds the timeout, so
    // everything it touches is owned by value / shared_ptr.
    cJSON* args_copy          = args != nullptr ? cJSON_Duplicate(args, 1) : cJSON_CreateObject();
    auto result               = std::make_shared<ToolOutput>();
    const Tool* tool_captured = tool;
    const bool done           = service_execute([tool_captured, args_copy, result]() {
        std::string err;
        Registry::instance().call(tool_captured->name, args_copy, result.get(), &err);
        cJSON_Delete(args_copy);
    }, kToolTimeoutMs);
    if (!done) {
        cJSON_Delete(args_copy);  // executor never picked it up
        return std::string(kTimeout);
    }
    service_note_tool_result(result->is_error);

    cJSON* root    = cJSON_CreateObject();
    cJSON* content = cJSON_CreateArray();
    cJSON* text_part = cJSON_CreateObject();
    cJSON_AddStringToObject(text_part, "type", "text");
    cJSON_AddStringToObject(text_part, "text", result->text.c_str());
    cJSON_AddItemToArray(content, text_part);
    if (!result->image_b64.empty()) {
        cJSON* image_part = cJSON_CreateObject();
        cJSON_AddStringToObject(image_part, "type", "image");
        cJSON_AddStringToObject(image_part, "data", result->image_b64.c_str());
        cJSON_AddStringToObject(image_part, "mimeType", "image/jpeg");
        cJSON_AddItemToArray(content, image_part);
    }
    cJSON_AddItemToObject(root, "content", content);
    cJSON_AddBoolToObject(root, "isError", result->is_error);
    std::string out = print(root);
    cJSON_Delete(root);
    return out;
}

esp_err_t mcp_handler(httpd_req_t* req)
{
    // Optional auth: Authorization: Bearer <token>
    if (token_configured()) {
        char auth[160];
        const esp_err_t got = httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth));
        const bool ok      = got == ESP_OK && token_matches(auth);
        if (!ok) {
            std::string body = build_error(nullptr, -32001, "unauthorized");
            return send_json(req, body, "401 Unauthorized");
        }
    }
    service_note_request();

    // Read body (capped).
    size_t remaining = req->content_len;
    if (remaining > kMaxBodySize) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
        return httpd_resp_send(req, nullptr, 0);
    }
    std::string body(remaining, '\0');
    size_t received = 0;
    while (received < remaining) {
        int ret = httpd_req_recv(req, &body[received], (size_t)(remaining - received));
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }

    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (root == nullptr) {
        return send_json(req, build_error(nullptr, -32700, "parse error"));
    }

    cJSON* id     = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON* method = cJSON_GetObjectItemCaseSensitive(root, "method");
    cJSON* params = cJSON_GetObjectItemCaseSensitive(root, "params");

    // Notification (no id): nothing to reply to.
    if (id == nullptr) {
        cJSON_Delete(root);
        return send_accepted(req);
    }

    esp_err_t ret = ESP_OK;
    if (cJSON_IsString(method) && method->valuestring != nullptr) {
        const char* m = method->valuestring;
        if (strcmp(m, "initialize") == 0) {
            ret = send_json(req, wrap_result(id, handle_initialize(params)));
        } else if (strcmp(m, "ping") == 0) {
            ret = send_json(req, wrap_result(id, "{}"));
        } else if (strcmp(m, "tools/list") == 0) {
            ret = send_json(req, wrap_result(id, handle_tools_list(params)));
        } else if (strcmp(m, "tools/call") == 0) {
            std::string out = handle_tools_call(params);
            if (!out.empty() && out[0] == kUnknownTool[0]) {
                ret = send_json(req, build_error(id, -32602, out.c_str() + 1));
            } else if (!out.empty() && out[0] == kTimeout[0]) {
                ret = send_json(req, build_error(id, -32603, "tool execution timeout"));
            } else {
                ret = send_json(req, wrap_result(id, out));
            }
        } else if (strncmp(m, "notifications/", 14) == 0) {
            cJSON_Delete(root);
            return send_accepted(req);
        } else {
            ret = send_json(req, build_error(id, -32601, "method not implemented"));
        }
    } else {
        ret = send_json(req, build_error(id, -32600, "invalid request"));
    }

    cJSON_Delete(root);

    // Deferred reboot (owner tool): only after the reply went out.
    if (service_consume_reboot_request()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    return ret;
}

}  // namespace

bool register_mcp_endpoint(httpd_handle_t server)
{
    httpd_uri_t uri    = {};
    uri.uri            = "/mcp";
    uri.method         = HTTP_POST;
    uri.handler        = mcp_handler;
    if (httpd_register_uri_handler(server, &uri) != ESP_OK) {
        ESP_LOGE(TAG, "failed to register POST /mcp");
        return false;
    }
    return true;
}

}  // namespace mcp_lan
