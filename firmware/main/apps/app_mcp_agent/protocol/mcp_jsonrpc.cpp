/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "mcp_jsonrpc.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <sdkconfig.h>

#include <cstring>
#include <memory>
#include <string>

#include <cJSON.h>

#include "../service/lan_service.h"

static const char* TAG = "McpRpc";

constexpr size_t kMaxBodySize = 32 * 1024;
constexpr uint32_t kMaxBodyTimeMs = 10000;

namespace mcp_lan {
namespace {

const char* kServerName = "stackchan-mcp";
const char* kServerVersion = "1.0.0";

bool token_configured()
{
    return strlen(CONFIG_MCP_AGENT_TOKEN) > 0;
}

bool token_matches(const char* header_value)
{
    if (header_value == nullptr || strncmp(header_value, "Bearer ", 7) != 0) {
        return false;
    }
    const char* presented = header_value + 7;
    const char* expected = CONFIG_MCP_AGENT_TOKEN;
    const size_t presented_len = strlen(presented);
    const size_t expected_len = strlen(expected);
    volatile unsigned char diff = presented_len == expected_len ? 0 : 1;
    for (size_t i = 0; i < expected_len; ++i) {
        if (i < presented_len) {
            diff |= (unsigned char)(presented[i] ^ expected[i]);
        }
    }
    return diff == 0;
}

esp_err_t send_json(httpd_req_t* req, const std::string& body, const char* status = "200 OK")
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, body.c_str(), (ssize_t)body.size());
}

esp_err_t send_accepted(httpd_req_t* req)
{
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, nullptr, 0);
}

std::string print(cJSON* node)
{
    char* text = cJSON_PrintUnformatted(node);
    std::string out = text != nullptr ? text : "{}";
    cJSON_free(text);
    return out;
}

std::string wrap_result(cJSON* id, const std::string& result_json)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddItemToObject(root, "id", id != nullptr ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON* parsed = cJSON_Parse(result_json.c_str());
    cJSON_AddItemToObject(root, "result", parsed != nullptr ? parsed : cJSON_CreateObject());
    std::string out = print(root);
    cJSON_Delete(root);
    return out;
}

std::string handle_initialize(const cJSON* params)
{
    static const char* supported[] = {"2024-11-05", "2025-03-26", "2025-06-18"};
    std::string version = "2025-03-26";
    const cJSON* requested = cJSON_GetObjectItemCaseSensitive(params, "protocolVersion");
    if (cJSON_IsString(requested) && requested->valuestring != nullptr) {
        for (const char* candidate : supported) {
            if (strcmp(candidate, requested->valuestring) == 0) {
                version = candidate;
            }
        }
    }

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", version.c_str());
    cJSON* capabilities = cJSON_CreateObject();
    cJSON* tools = cJSON_CreateObject();
    cJSON_AddBoolToObject(tools, "listChanged", false);
    cJSON_AddItemToObject(capabilities, "tools", tools);
    cJSON_AddItemToObject(result, "capabilities", capabilities);
    cJSON* info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", kServerName);
    cJSON_AddStringToObject(info, "version", kServerVersion);
    cJSON_AddItemToObject(result, "serverInfo", info);
    std::string out = print(result);
    cJSON_Delete(result);
    return out;
}

std::string handle_tools_list(LanService& service, const cJSON* params)
{
    int cursor = 0;
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(params, "cursor");
    if (cJSON_IsString(value) && value->valuestring != nullptr) {
        cursor = atoi(value->valuestring);
    }
    int next = -1;
    cJSON* result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "tools", service.registry().build_tools_page(cursor, &next));
    if (next >= 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", next);
        cJSON_AddStringToObject(result, "nextCursor", buf);
    }
    std::string out = print(result);
    cJSON_Delete(result);
    return out;
}

esp_err_t handle_tools_call(LanService& service, httpd_req_t* req, cJSON* id, const cJSON* params)
{
    const cJSON* name = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (!cJSON_IsString(name) || name->valuestring == nullptr) {
        return send_json(req, make_error_json(id, -32602, "params.name must be a string"));
    }
    const Tool* tool = service.registry().find(name->valuestring);
    if (tool == nullptr) {
        const std::string message = std::string("unknown tool: ") + name->valuestring;
        return send_json(req, make_error_json(id, -32602, message.c_str()));
    }

    const cJSON* args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    if (args != nullptr && !cJSON_IsObject(args)) {
        return send_json(req, make_error_json(id, -32602, "params.arguments must be an object"));
    }

    cJSON* args_copy = args != nullptr ? cJSON_Duplicate(args, 1) : cJSON_CreateObject();
    cJSON* id_copy = cJSON_Duplicate(id, 1);
    if (args_copy == nullptr || id_copy == nullptr) {
        cJSON_Delete(args_copy);
        cJSON_Delete(id_copy);
        return send_json(req, make_error_json(id, -32603, "out of memory"));
    }

    httpd_req_t* async_req = nullptr;
    const esp_err_t begin = httpd_req_async_handler_begin(req, &async_req);
    if (begin != ESP_OK || async_req == nullptr) {
        cJSON_Delete(args_copy);
        cJSON_Delete(id_copy);
        return send_json(req, make_error_json(id, -32603, "failed to start async request"));
    }

    service.submit_tool(tool, args_copy, id_copy, async_req);
    return ESP_OK;
}

esp_err_t mcp_handler(httpd_req_t* req)
{
    auto& service = *static_cast<LanService*>(req->user_ctx);
    if (token_configured()) {
        char auth[160];
        const esp_err_t got = httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth));
        if (got != ESP_OK || !token_matches(auth)) {
            return send_json(req, make_error_json(nullptr, -32001, "unauthorized"), "401 Unauthorized");
        }
    }
    service.note_request();

    if (req->content_len > kMaxBodySize) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
        return httpd_resp_send(req, nullptr, 0);
    }

    const size_t remaining = req->content_len;
    std::string body(remaining, '\0');
    size_t received = 0;
    const int64_t started = esp_timer_get_time();
    while (received < remaining) {
        if ((esp_timer_get_time() - started) > (int64_t)kMaxBodyTimeMs * 1000) {
            return send_json(req, make_error_json(nullptr, -32000, "request body timeout"), "408 Request Timeout");
        }
        const int ret = httpd_req_recv(req, &body[received], remaining - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }

    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (root == nullptr) {
        return send_json(req, make_error_json(nullptr, -32700, "parse error"));
    }

    cJSON* id = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON* method = cJSON_GetObjectItemCaseSensitive(root, "method");
    cJSON* params = cJSON_GetObjectItemCaseSensitive(root, "params");
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
            ret = send_json(req, wrap_result(id, handle_tools_list(service, params)));
        } else if (strcmp(m, "tools/call") == 0) {
            ret = handle_tools_call(service, req, id, params);
        } else if (strncmp(m, "notifications/", 14) == 0) {
            cJSON_Delete(root);
            return send_accepted(req);
        } else {
            ret = send_json(req, make_error_json(id, -32601, "method not implemented"));
        }
    } else {
        ret = send_json(req, make_error_json(id, -32600, "invalid request"));
    }

    cJSON_Delete(root);
    return ret;
}

}  // namespace

std::string make_tool_result_json(cJSON* id, const ToolOutput& output)
{
    cJSON* root = cJSON_CreateObject();
    cJSON* content = cJSON_CreateArray();
    cJSON* text = cJSON_CreateObject();
    cJSON_AddStringToObject(text, "type", "text");
    cJSON_AddStringToObject(text, "text", output.text.c_str());
    cJSON_AddItemToArray(content, text);
    if (!output.image_b64.empty()) {
        cJSON* image = cJSON_CreateObject();
        cJSON_AddStringToObject(image, "type", "image");
        cJSON_AddStringToObject(image, "data", output.image_b64.c_str());
        cJSON_AddStringToObject(image, "mimeType", "image/jpeg");
        cJSON_AddItemToArray(content, image);
    }
    cJSON_AddItemToObject(root, "content", content);
    cJSON_AddBoolToObject(root, "isError", output.is_error);
    std::string out = wrap_result(id, print(root));
    cJSON_Delete(root);
    return out;
}

std::string make_error_json(cJSON* id, int code, const char* message)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddItemToObject(root, "id", id != nullptr ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON* error = cJSON_CreateObject();
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(root, "error", error);
    std::string out = print(root);
    cJSON_Delete(root);
    return out;
}

bool register_mcp_endpoint(httpd_handle_t server, LanService& service)
{
    httpd_uri_t uri = {};
    uri.uri = "/mcp";
    uri.method = HTTP_POST;
    uri.handler = mcp_handler;
    uri.user_ctx = &service;
    if (httpd_register_uri_handler(server, &uri) != ESP_OK) {
        ESP_LOGE(TAG, "failed to register POST /mcp");
        return false;
    }
    return true;
}

}  // namespace mcp_lan
