/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD.
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
#include "http_guard.h"

static const char* TAG = "RestApi";

constexpr size_t kMaxBodySize = 32 * 1024;
constexpr uint32_t kMaxBodyTimeMs = 10000;

namespace mcp_lan {

std::string make_rest_result(const ToolOutput& output)
{
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return R"({"ok":false,"error":"out of memory"})";
    }
    if (output.is_error) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", output.text.c_str());
    } else {
        cJSON_AddBoolToObject(root, "ok", true);
        // 工具结果文本多为 JSON;能解析就内嵌为对象,否则按字符串返回
        cJSON* parsed = cJSON_Parse(output.text.c_str());
        if (parsed != nullptr) {
            cJSON_AddItemToObject(root, "result", parsed);
        } else {
            cJSON_AddStringToObject(root, "result", output.text.c_str());
        }
        if (!output.image_b64.empty()) {
            cJSON_AddStringToObject(root, "image", output.image_b64.c_str());
        }
    }
    char* text = cJSON_PrintUnformatted(root);
    std::string out = text != nullptr ? text : R"({"ok":false,"error":"print failed"})";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

std::string make_rest_error(const char* message)
{
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return R"({"ok":false,"error":"out of memory"})";
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    char* text = cJSON_PrintUnformatted(root);
    std::string out = text != nullptr ? text : R"({"ok":false,"error":"print failed"})";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

namespace {

esp_err_t send_json(httpd_req_t* req, const std::string& body, const char* status = "200 OK")
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, body.c_str(), (ssize_t)body.size());
}

// 读取请求体(带超时);空 body 返回空串由调用者决定语义
bool read_body(httpd_req_t* req, std::string* body)
{
    const size_t remaining = req->content_len;
    if (remaining == 0) {
        body->clear();
        return true;
    }
    if (remaining > kMaxBodySize) {
        return false;
    }
    body->assign(remaining, '\0');
    size_t received = 0;
    const int64_t started = esp_timer_get_time();
    while (received < remaining) {
        if ((esp_timer_get_time() - started) > (int64_t)kMaxBodyTimeMs * 1000) {
            return false;
        }
        const int ret = httpd_req_recv(req, &(*body)[received], remaining - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            return false;
        }
        received += (size_t)ret;
    }
    return true;
}

// 通用路由描述:一个 HTTP 方法+路径 → 一个工具(body 即参数对象)
struct Route {
    const char* uri;
    int method;             // httpd method
    const char* tool_name;  // registry 中的工具名
};

const Route kRoutes[] = {
    {"/display/emotion",   HTTP_POST,   "self.display.set_emotion"},
    {"/display/message",   HTTP_POST,   "self.display.show_message"},
    {"/display/message",   HTTP_DELETE, "self.display.clear_message"},
    {"/screen",            HTTP_GET,    "self.screen.get_info"},
    {"/screen/brightness", HTTP_POST,   "self.screen.set_brightness"},
    {"/robot/head",        HTTP_GET,    "self.robot.get_head_angles"},
    {"/led",               HTTP_POST,   "self.robot.set_led_color"},
    {"/speaker/volume",    HTTP_POST,   "self.audio_speaker.set_volume"},
    {"/speaker/play",      HTTP_POST,   "self.audio_speaker.play_opus"},
    {"/camera/photo",      HTTP_POST,   "self.camera.take_photo"},
    {"/reboot",            HTTP_POST,   "self.reboot"},
};

// POST /robot/head 按 body 形状分派到三个舵机工具之一
const char* pick_head_tool(const cJSON* args)
{
    if (args == nullptr) {
        return "self.robot.go_home";
    }
    if (cJSON_GetObjectItemCaseSensitive(args, "x") != nullptr ||
        cJSON_GetObjectItemCaseSensitive(args, "y") != nullptr) {
        return "self.robot.look_at";
    }
    if (cJSON_GetObjectItemCaseSensitive(args, "home") != nullptr) {
        return "self.robot.go_home";
    }
    return "self.robot.set_head_angles";
}

esp_err_t rest_handler_impl(httpd_req_t* req)
{
    auto& service = *static_cast<LanService*>(req->user_ctx);
    if (!service.http_authorized(req)) {
        return send_json(req, make_rest_error("unauthorized"), "401 Unauthorized");
    }

    const Route* route = nullptr;
    for (const Route& r : kRoutes) {
        if (r.method == (int)req->method && strcmp(r.uri, req->uri) == 0) {
            route = &r;
            break;
        }
    }
    // POST /robot/head 的形状分派(路径相同不在表内匹配工具)
    const bool head_post = req->method == HTTP_POST && strcmp("/robot/head", req->uri) == 0;
    if (route == nullptr && !head_post) {
        return send_json(req, make_rest_error("not found"), "404 Not Found");
    }

    std::string body;
    if (!read_body(req, &body)) {
        return send_json(req, make_rest_error("body too large or timed out"), "413 Payload Too Large");
    }

    cJSON* args = nullptr;
    if (!body.empty()) {
        args = cJSON_ParseWithLength(body.data(), body.size());
        if (args == nullptr || !cJSON_IsObject(args)) {
            cJSON_Delete(args);
            return send_json(req, make_rest_error("body must be a JSON object"), "400 Bad Request");
        }
    } else {
        args = cJSON_CreateObject();
    }
    if (args == nullptr) {
        return send_json(req, make_rest_error("out of memory"), "500 Internal Server Error");
    }

    service.note_request();
    const char* tool_name = head_post ? pick_head_tool(args) : route->tool_name;
    const Tool* tool = service.registry().find(tool_name);
    if (tool == nullptr) {
        cJSON_Delete(args);
        const std::string message = std::string("unknown tool: ") + tool_name;
        return send_json(req, make_rest_error(message.c_str()), "404 Not Found");
    }
    return service.submit_tool(tool, args, req);
}

esp_err_t rest_handler(httpd_req_t* req)
{
    return GuardHttpHandler(req, [req] { return rest_handler_impl(req); });
}

}  // namespace

bool register_rest_endpoints(httpd_handle_t server, LanService& service)
{
    size_t registered = 0;
    const size_t total = sizeof(kRoutes) / sizeof(kRoutes[0]) + 1;  // +1 = POST /robot/head
    for (const Route& r : kRoutes) {
        httpd_uri_t uri = {};
        uri.uri = r.uri;
        uri.method = (httpd_method_t)r.method;
        uri.handler = rest_handler;
        uri.user_ctx = &service;
        if (httpd_register_uri_handler(server, &uri) != ESP_OK) {
            ESP_LOGE(TAG, "failed to register %s %s", r.method == HTTP_GET ? "GET" : "REST", r.uri);
            return false;
        }
        ++registered;
    }
    httpd_uri_t head_post = {};
    head_post.uri = "/robot/head";
    head_post.method = HTTP_POST;
    head_post.handler = rest_handler;
    head_post.user_ctx = &service;
    if (httpd_register_uri_handler(server, &head_post) != ESP_OK) {
        ESP_LOGE(TAG, "failed to register POST /robot/head");
        return false;
    }
    ++registered;
    ESP_LOGI(TAG, "%zu REST endpoints registered", registered);
    (void)total;
    return true;
}

}  // namespace mcp_lan
