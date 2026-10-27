/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "lan_service.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <deque>
#include <memory>
#include <mutex>

#include <cJSON.h>

#include "device_access.h"
#include "hal/mic_ws_stream.h"
#include "mcp_jsonrpc.h"
#include "mcp_photo.h"
#include "mcp_registry.h"
#include "hal/speaker_ws_stream.h"

static const char* TAG = "McpSvc";

// The httpd task runs on the default socket task; 6 KB because internal RAM
// is scarce on this board.
constexpr int kHttpdStackSize = 6144;
constexpr int kHttpdCtrlPort  = 32767;
constexpr int kExecutorStack  = 8192;

namespace mcp_lan {

namespace {

struct Executor {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::shared_ptr<std::function<void()>>> jobs;
    bool running = false;
    TaskHandle_t task = nullptr;
    std::atomic<bool>* exited = nullptr;
};

struct Stats {
    std::mutex mtx;
    uint32_t requests      = 0;
    uint32_t tool_calls    = 0;
    uint32_t tool_errors   = 0;
    char last_event[96]    = {0};
    bool reboot_requested  = false;
};

Executor& executor()
{
    static Executor e;
    return e;
}

Stats& stats()
{
    static Stats s;
    return s;
}

httpd_handle_t g_httpd = nullptr;
bool g_running = false;

void executor_task(void*)
{
    auto& ex = executor();
    while (ex.running) {
        std::shared_ptr<std::function<void()>> job;
        {
            std::unique_lock<std::mutex> lock(ex.mtx);
            ex.cv.wait_for(lock, std::chrono::seconds(1), [&ex] { return !ex.jobs.empty() || !ex.running; });
            if (!ex.running) {
                break;
            }
            if (ex.jobs.empty()) {
                continue;
            }
            job = std::move(ex.jobs.front());
            ex.jobs.pop_front();
        }
        (*job)();
    }
    if (ex.exited != nullptr) {
        ex.exited->store(true);
    }
    // WithCaps tasks must not self-delete: park, the owner frees us.
    vTaskDelay(portMAX_DELAY);
}

struct Pending {
    SemaphoreHandle_t done;
    std::mutex mtx;
    bool finished = false;
};

std::string json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if ((unsigned char)c < 0x20) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

esp_err_t status_handler(httpd_req_t* req)
{
    auto st = service_status();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "stackchan-mcp");
    cJSON_AddStringToObject(root, "version", "1.0.0");
    cJSON_AddStringToObject(root, "firmware", FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "ip", st.ip.c_str());
    cJSON_AddNumberToObject(root, "port", st.port);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "free_psram", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    // Internal-RAM telemetry: the scarce resource on this board.
    cJSON_AddNumberToObject(root, "free_sram", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "largest_sram_block", (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "mic_stack_free", (double)MicWsStream::StackHighWaterBytes());
    cJSON_AddNumberToObject(root, "spk_stack_free", (double)SpeakerWsStream::StackHighWaterBytes());
    cJSON_AddNumberToObject(root, "tools", (double)st.tools);
    cJSON_AddNumberToObject(root, "tool_calls", (double)st.tool_calls);
    cJSON_AddNumberToObject(root, "tool_errors", (double)st.tool_errors);
    cJSON_AddBoolToObject(root, "mic_active", st.mic_active);
    cJSON_AddBoolToObject(root, "spk_active", st.spk_active);
    cJSON_AddNumberToObject(root, "photos", (double)st.photos);
    cJSON_AddStringToObject(root, "last_event", st.last_event.c_str());
    char* printed = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    httpd_resp_send(req, printed, (ssize_t)strlen(printed));
    cJSON_free(printed);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t photo_handler(httpd_req_t* req)
{
    const uint8_t* data = nullptr;
    size_t len          = 0;
    if (!mcp_photo::get(&data, &len)) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
        return httpd_resp_send(req, "no photo captured yet", -1);
    }
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char*)data, (ssize_t)len);
}

}  // namespace

bool service_execute(const std::function<void()>& body, uint32_t timeout_ms)
{
    if (!executor().running) {
        return false;
    }
    auto pending = std::make_shared<Pending>();
    pending->done = xSemaphoreCreateBinary();

    auto job = std::make_shared<std::function<void()>>([pending, body]() {
        body();
        std::lock_guard<std::mutex> lock(pending->mtx);
        pending->finished = true;
        xSemaphoreGive(pending->done);
    });

    {
        auto& ex = executor();
        std::lock_guard<std::mutex> lock(ex.mtx);
        if (!ex.running) {
            vSemaphoreDelete(pending->done);
            return false;
        }
        ex.jobs.push_back(job);
        if (ex.jobs.size() > 4) {
            ex.jobs.pop_front();  // bound the backlog, oldest dropped
        }
    }
    executor().cv.notify_one();

    const bool ok = xSemaphoreTake(pending->done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    if (ok) {
        vSemaphoreDelete(pending->done);
    }
    // On timeout the semaphore is deliberately leaked (a few bytes): the
    // executor may still be about to Give it, and deleting it then would
    // corrupt the FreeRTOS internal list. Timeouts are rare (30 s).
    return ok;
}

void service_note_request()
{
    auto& s = stats();
    std::lock_guard<std::mutex> lock(s.mtx);
    s.requests++;
}

void service_note_tool_result(bool is_error)
{
    auto& s = stats();
    std::lock_guard<std::mutex> lock(s.mtx);
    s.tool_calls++;
    if (is_error) {
        s.tool_errors++;
    }
}

void service_push_event(const char* fmt, ...)
{
    auto& s = stats();
    std::lock_guard<std::mutex> lock(s.mtx);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s.last_event, sizeof(s.last_event), fmt, ap);
    va_end(ap);
}

void service_request_reboot()
{
    auto& s = stats();
    std::lock_guard<std::mutex> lock(s.mtx);
    s.reboot_requested = true;
}

bool service_consume_reboot_request()
{
    auto& s = stats();
    std::lock_guard<std::mutex> lock(s.mtx);
    bool r       = s.reboot_requested;
    s.reboot_requested = false;
    return r;
}

bool service_ws_authorized(httpd_req_t* req)
{
    if (strlen(CONFIG_MCP_AGENT_TOKEN) == 0) {
        return true;
    }
    const char* token = CONFIG_MCP_AGENT_TOKEN;
    // The raw URI carries the query string in WS handlers.
    const char* q = strchr(req->uri, '?');
    while (q != nullptr) {
        const char* kv = q + 1;
        if (strncmp(kv, "token=", 6) == 0) {
            const char* val = kv + 6;
            const size_t len = strlen(val);
            if (len == strlen(token) && strncmp(val, token, len) == 0) {
                return true;
            }
            return false;
        }
        q = strchr(kv, '&');
    }
    return false;
}

std::string service_base_url()
{
    const std::string ip = mcp_dev::wifi_ip();
    if (ip.empty() || ip == "0.0.0.0") {
        return {};
    }
    char buf[40];
    snprintf(buf, sizeof(buf), "http://%s:%d", ip.c_str(), CONFIG_MCP_AGENT_PORT);
    return buf;
}

ServiceStatus service_status()
{
    ServiceStatus st;
    st.running  = g_running;
    st.port     = CONFIG_MCP_AGENT_PORT;
    st.tools    = Registry::instance().size();
    {
        auto& s = stats();
        std::lock_guard<std::mutex> lock(s.mtx);
        st.requests    = s.requests;
        st.tool_calls  = s.tool_calls;
        st.tool_errors = s.tool_errors;
        st.last_event  = s.last_event;
    }
    st.ip         = mcp_dev::wifi_ip();
    st.mic_active = MicWsStream::IsActive();
    st.spk_active = SpeakerWsStream::IsActive();
    st.photos     = mcp_photo::count();
    return st;
}

bool service_start(std::string* err)
{
    if (g_running) {
        return true;
    }

    register_all_tools();

    // Tool executor (tools/call bodies run serialized here, never on httpd).
    // 8 KB internal stack (BYTES with WithCaps; tool bodies write NVS, so
    // a PSRAM stack would hit the cache-disabled assert).
    auto& ex = executor();
    ex.running = true;
    static std::atomic<bool> exited{false};
    exited.store(false);
    ex.exited = &exited;
    if (xTaskCreatePinnedToCoreWithCaps(executor_task, "mcp_tool", kExecutorStack, nullptr, 3, &ex.task, 1,
                                        MALLOC_CAP_INTERNAL) != pdPASS) {
        ex.running = false;
        if (err != nullptr) *err = "failed to create mcp_tool task";
        return false;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port        = CONFIG_MCP_AGENT_PORT;
    config.ctrl_port          = kHttpdCtrlPort;
    config.stack_size         = kHttpdStackSize;
    config.lru_purge_enable   = true;
    config.max_open_sockets   = 7;
    if (httpd_start(&g_httpd, &config) != ESP_OK || g_httpd == nullptr) {
        ex.running = false;
        g_httpd    = nullptr;
        if (err != nullptr) *err = "httpd_start failed";
        return false;
    }

    httpd_uri_t status_uri = {};
    status_uri.uri         = "/";
    status_uri.method      = HTTP_GET;
    status_uri.handler     = status_handler;
    httpd_uri_t photo_uri  = {};
    photo_uri.uri          = "/photo.jpg";
    photo_uri.method       = HTTP_GET;
    photo_uri.handler      = photo_handler;

    bool ok = httpd_register_uri_handler(g_httpd, &status_uri) == ESP_OK &&
              httpd_register_uri_handler(g_httpd, &photo_uri) == ESP_OK &&
              register_mcp_endpoint(g_httpd) && MicWsStream::GetInstance().Start(g_httpd) &&
              SpeakerWsStream::GetInstance().Start(g_httpd);
    if (!ok) {
        if (err != nullptr) *err = "failed to register endpoints";
        service_stop();
        return false;
    }

    g_running = true;
    service_push_event("service started on port %d", CONFIG_MCP_AGENT_PORT);
    ESP_LOGI(TAG, "MCP Agent service on http://<ip>:%d/mcp", CONFIG_MCP_AGENT_PORT);
    return true;
}

void service_stop()
{
    if (!g_running && g_httpd == nullptr && !executor().running) {
        return;
    }
    g_running = false;

    // Streams first (unregister + stop their tasks), then the server.
    MicWsStream::GetInstance().Stop();
    SpeakerWsStream::GetInstance().Stop();
    if (g_httpd != nullptr) {
        httpd_stop(g_httpd);  // closes all sockets
        g_httpd = nullptr;
    }

    auto& ex = executor();
    {
        std::lock_guard<std::mutex> lock(ex.mtx);
        ex.running = false;
        ex.jobs.clear();
    }
    ex.cv.notify_all();
    for (int i = 0; i < 15 && ex.exited != nullptr && !ex.exited->load(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (ex.task != nullptr) {
        vTaskDeleteWithCaps(ex.task);
        ex.task = nullptr;
    }
    ex.exited = nullptr;
    service_push_event("service stopped");
    ESP_LOGI(TAG, "MCP Agent service stopped");
}

bool service_running()
{
    return g_running;
}

}  // namespace mcp_lan
