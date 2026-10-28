/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "lan_service.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <unistd.h>

#include <cstdarg>
#include <vector>

#include <cJSON.h>

#include "../audio/mic_ws_stream.h"
#include "../audio/speaker_ws_stream.h"
#include "../platform/device_access.h"
#include "../protocol/mcp_jsonrpc.h"
#include "../protocol/http_guard.h"
#include "../tools/mcp_photo.h"

static const char* TAG = "McpSvc";

constexpr int kHttpdStackSize = 6144;
constexpr int kHttpdCtrlPort = 32767;
constexpr int kExecutorStack = 8192;
constexpr size_t kMaxQueuedTools = 4;
constexpr int64_t kToolTimeoutUs = 30000000;

namespace mcp_lan {

struct LanService::AsyncToolJob {
    const Tool* tool = nullptr;
    cJSON* args = nullptr;
    cJSON* id = nullptr;
    httpd_req_t* req = nullptr;
    ToolOutput result;
    int64_t deadline_us = 0;
    bool ready = false;
    std::atomic<bool> response_done{false};

    ~AsyncToolJob()
    {
        cJSON_Delete(args);
        cJSON_Delete(id);
        if (req != nullptr) {
            httpd_req_async_handler_complete(req);
            req = nullptr;
        }
    }
};

namespace {

esp_err_t send_json(httpd_req_t* req, const std::string& body, const char* status = "200 OK")
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, body.c_str(), (ssize_t)body.size());
}

esp_err_t status_handler_impl(httpd_req_t* req)
{
    auto& service = *static_cast<LanService*>(req->user_ctx);
    const auto st = service.status();
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return send_json(req, make_error_json(nullptr, -32603, "out of memory"), "500 Internal Server Error");
    }
    cJSON_AddStringToObject(root, "name", "stackchan-mcp");
    cJSON_AddStringToObject(root, "version", "1.0.0");
    cJSON_AddStringToObject(root, "firmware", FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "ip", st.ip.c_str());
    cJSON_AddNumberToObject(root, "port", st.port);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "free_psram", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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
    if (printed == nullptr) {
        cJSON_Delete(root);
        return send_json(req, make_error_json(nullptr, -32603, "out of memory"), "500 Internal Server Error");
    }
    const esp_err_t ret = send_json(req, printed);
    cJSON_free(printed);
    cJSON_Delete(root);
    return ret;
}

esp_err_t photo_handler_impl(httpd_req_t* req)
{
    auto& service = *static_cast<LanService*>(req->user_ctx);
    if (!service.http_authorized(req)) {
        return send_json(req, make_error_json(nullptr, -32001, "unauthorized"), "401 Unauthorized");
    }
    mcp_photo::PhotoSnapshot snapshot;
    if (!mcp_photo::get(&snapshot)) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
        return httpd_resp_send(req, "no photo captured yet", -1);
    }
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, (const char*)snapshot.bytes->data(), (ssize_t)snapshot.bytes->size());
}

esp_err_t status_handler(httpd_req_t* req)
{
    return GuardHttpHandler(req, [req] { return status_handler_impl(req); });
}

esp_err_t photo_handler(httpd_req_t* req)
{
    return GuardHttpHandler(req, [req] { return photo_handler_impl(req); });
}

}  // namespace

LanService::LanService() = default;

LanService::~LanService()
{
    if (running_.load() || !stopped()) {
        ESP_LOGW(TAG, "LanService destroyed before asynchronous stop completed");
        return;
    }
    reclaim();
}

void LanService::executor_entry(void* arg)
{
    auto* self = static_cast<LanService*>(arg);
    try {
        self->executor_loop();
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "executor aborted: %s", e.what());
    } catch (...) {
        ESP_LOGE(TAG, "executor aborted: unknown exception");
    }

    self->running_.store(false);
    {
        std::unique_lock<std::mutex> lock(self->executor_.mtx);
        self->executor_.closing = true;
        self->executor_.cv.wait(lock, [self] { return self->executor_.beginning == 0; });
    }
    std::map<uint64_t, JobPtr> jobs;
    {
        std::lock_guard<std::mutex> lock(self->executor_.mtx);
        jobs.swap(self->executor_.active);
        self->executor_.queue.clear();
    }
    for (auto& [id, job] : jobs) {
        if (job->req != nullptr) {
            httpd_req_async_handler_complete(job->req);
            job->req = nullptr;
        }
    }
    jobs.clear();
    self->stop_httpd_and_streams();
    self->executor_.exited.store(true);
    vTaskDelay(portMAX_DELAY);
}

void LanService::watchdog_entry(void* arg)
{
    auto* self = static_cast<LanService*>(arg);
    while (true) {
        {
            std::lock_guard<std::mutex> lock(self->executor_.mtx);
            if (self->executor_.closing) {
                break;
            }
        }
        try {
            self->check_timeouts();
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "timeout check aborted: %s", e.what());
        } catch (...) {
            ESP_LOGE(TAG, "timeout check aborted: unknown exception");
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    self->executor_.timeout_exited.store(true);
    vTaskDelay(portMAX_DELAY);
}

bool LanService::start(std::string* err)
{
    if (!mcp_dev::wifi_connected()) {
        if (err != nullptr) *err = "Wi-Fi is not connected";
        return false;
    }
    if (running_.load()) {
        return true;
    }
    if (!stopped()) {
        if (err != nullptr) *err = "previous service stop is still in progress";
        return false;
    }
    reclaim();

    register_all_tools(*this);
    {
        std::lock_guard<std::mutex> lock(executor_.mtx);
        executor_.closing = false;
        executor_.started = true;
        executor_.queue.clear();
        executor_.active.clear();
        executor_.beginning = 0;
        next_job_id_ = 1;
    }
    executor_.exited.store(false);
    if (xTaskCreatePinnedToCoreWithCaps(executor_entry, "mcp_tool", kExecutorStack, this, 3, &executor_.task, 1,
                                        MALLOC_CAP_INTERNAL) != pdPASS) {
        executor_.task = nullptr;
        {
            std::lock_guard<std::mutex> lock(executor_.mtx);
            executor_.started = false;
        }
        executor_.exited.store(true);
        mark_start_failure("failed to create mcp_tool task");
        if (err != nullptr) *err = "failed to create mcp_tool task";
        return false;
    }

    executor_.timeout_exited.store(false);
    if (xTaskCreatePinnedToCoreWithCaps(watchdog_entry, "mcp_timeout", 4096, this, 3,
                                        &executor_.timeout_task, 1, MALLOC_CAP_INTERNAL) != pdPASS) {
        executor_.timeout_task = nullptr;
        executor_.timeout_exited.store(true);
        mark_start_failure("failed to create mcp_timeout task");
        if (err != nullptr) *err = "failed to create mcp_timeout task";
        return false;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_MCP_AGENT_PORT;
    config.ctrl_port = kHttpdCtrlPort;
    config.stack_size = kHttpdStackSize;
    config.lru_purge_enable = true;
    // LWIP_MAX_SOCKETS=10 leaves seven client sockets after HTTPD's three
    // internal sockets. JSON responses use Connection: close; async jobs and
    // the two WebSocket streams remain explicitly bounded.
    config.max_open_sockets = 7;
    config.close_fn = [](httpd_handle_t server, int fd) {
        MicWsStream::HandleDisconnect(server, fd);
        SpeakerWsStream::HandleDisconnect(server, fd);
        // httpd_sess_delete() delegates the actual close to close_fn too.
        close(fd);
    };
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK || server == nullptr) {
        mark_start_failure("httpd_start failed");
        if (err != nullptr) *err = "httpd_start failed";
        return false;
    }
    httpd_.store(server);

    httpd_uri_t status_uri = {};
    status_uri.uri = "/";
    status_uri.method = HTTP_GET;
    status_uri.handler = status_handler;
    status_uri.user_ctx = this;
    httpd_uri_t photo_uri = {};
    photo_uri.uri = "/photo.jpg";
    photo_uri.method = HTTP_GET;
    photo_uri.handler = photo_handler;
    photo_uri.user_ctx = this;

    const bool ok = httpd_register_uri_handler(server, &status_uri) == ESP_OK &&
                    httpd_register_uri_handler(server, &photo_uri) == ESP_OK &&
                    register_mcp_endpoint(server, *this) &&
                    MicWsStream::GetInstance().Start(server, *this) &&
                    SpeakerWsStream::GetInstance().Start(server, *this);
    if (!ok) {
        mark_start_failure("failed to register endpoints");
        if (err != nullptr) *err = "failed to register endpoints";
        return false;
    }

    running_.store(true);
    push_event("service started on port %d", CONFIG_MCP_AGENT_PORT);
    ESP_LOGI(TAG, "MCP Agent service on http://<ip>:%d/mcp", CONFIG_MCP_AGENT_PORT);
    return true;
}

void LanService::mark_start_failure(const std::string& message)
{
    push_event("start failed: %s", message.c_str());
    request_stop();
}

void LanService::request_stop()
{
    running_.store(false);
    {
        std::lock_guard<std::mutex> lock(executor_.mtx);
        if (!executor_.started) {
            executor_.exited.store(true);
            return;
        }
        executor_.closing = true;
    }
    executor_.cv.notify_all();
}

bool LanService::stopped() const
{
    return !running_.load() && httpd_.load() == nullptr && executor_.exited.load() &&
           executor_.timeout_exited.load() && MicWsStream::Stopped() && SpeakerWsStream::Stopped();
}

void LanService::reclaim()
{
    if (!stopped()) {
        return;
    }
    if (executor_.task != nullptr) {
        vTaskDeleteWithCaps(executor_.task);
        executor_.task = nullptr;
    }
    if (executor_.timeout_task != nullptr) {
        vTaskDeleteWithCaps(executor_.timeout_task);
        executor_.timeout_task = nullptr;
    }
    MicWsStream::Reclaim();
    SpeakerWsStream::Reclaim();
    std::lock_guard<std::mutex> lock(executor_.mtx);
    executor_.started = false;
    executor_.closing = false;
    executor_.queue.clear();
    executor_.active.clear();
}

esp_err_t LanService::submit_tool(const Tool* tool, cJSON* args, cJSON* id, httpd_req_t* req)
{
    JobPtr job;
    try {
        job = std::make_shared<AsyncToolJob>();
    } catch (...) {
        cJSON_Delete(args);
        cJSON_Delete(id);
        throw;
    }
    job->tool = tool;
    job->args = args;
    job->id = id;
    job->deadline_us = esp_timer_get_time() + kToolTimeoutUs;

    uint64_t job_id = 0;
    FinishKind immediate = FinishKind::StartFailure;
    {
        std::lock_guard<std::mutex> lock(executor_.mtx);
        if (running_.load() && !executor_.closing && executor_.started &&
            executor_.queue.size() < kMaxQueuedTools) {
            job_id = next_job_id_++;
            try {
                executor_.active.emplace(job_id, job);
                executor_.queue.push_back(job_id);
                ++executor_.beginning;
            } catch (...) {
                executor_.active.erase(job_id);
                job_id = 0;
                throw;
            }
        } else if (executor_.queue.size() >= kMaxQueuedTools) {
            immediate = FinishKind::QueueFull;
        }
    }
    if (job_id == 0) {
        const bool queue_full = immediate == FinishKind::QueueFull;
        return send_json(req,
                         make_error_json(id, -32603,
                                         queue_full ? "tool queue is full" : "tool executor is unavailable"),
                         "503 Service Unavailable");
    }

    struct BeginCompletion {
        LanService* service;
        ~BeginCompletion()
        {
            {
                std::lock_guard<std::mutex> lock(service->executor_.mtx);
                --service->executor_.beginning;
            }
            service->executor_.cv.notify_all();
        }
    } begin_completion{this};

    httpd_req_t* async_req = nullptr;
    const esp_err_t begin = httpd_req_async_handler_begin(req, &async_req);
    if (begin != ESP_OK || async_req == nullptr) {
        {
            std::lock_guard<std::mutex> lock(executor_.mtx);
            executor_.active.erase(job_id);
            for (auto it = executor_.queue.begin(); it != executor_.queue.end(); ++it) {
                if (*it == job_id) {
                    executor_.queue.erase(it);
                    break;
                }
            }
        }
        return send_json(req, make_error_json(id, -32603, "failed to start async request"),
                         "500 Internal Server Error");
    }

    {
        std::lock_guard<std::mutex> lock(executor_.mtx);
        job->req = async_req;
        job->ready = true;
    }
    return ESP_OK;
}

void LanService::finish_job(uint64_t id, FinishKind kind)
{
    JobPtr job;
    {
        std::lock_guard<std::mutex> lock(executor_.mtx);
        auto it = executor_.active.find(id);
        if (it == executor_.active.end()) {
            return;
        }
        job = it->second;
        executor_.active.erase(it);
        if (job->response_done.exchange(true)) {
            return;
        }
    }
    try {
        finish_request(job, kind);
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "tool response aborted: out of memory");
        if (job->req != nullptr) {
            SendHandlerError(job->req, true);
            httpd_req_async_handler_complete(job->req);
            job->req = nullptr;
        }
    } catch (...) {
        ESP_LOGE(TAG, "tool response aborted: unknown exception");
        if (job->req != nullptr) {
            SendHandlerError(job->req, false);
            httpd_req_async_handler_complete(job->req);
            job->req = nullptr;
        }
    }
}

void LanService::finish_request(const JobPtr& job, FinishKind kind)
{
    std::string body;
    const char* status = "200 OK";
    switch (kind) {
        case FinishKind::Success:
            note_tool_result(job->result.is_error);
            body = make_tool_result_json(job->id, job->result);
            break;
        case FinishKind::Timeout:
            body = make_error_json(job->id, -32603, "tool execution timeout");
            break;
        case FinishKind::Cancelled:
            body = make_error_json(job->id, -32603, "service is stopping");
            break;
        case FinishKind::QueueFull:
            body = make_error_json(job->id, -32603, "tool queue is full");
            status = "503 Service Unavailable";
            break;
        case FinishKind::StartFailure:
            body = make_error_json(job->id, -32603, "tool executor is unavailable");
            status = "503 Service Unavailable";
            break;
    }

    if (job->req != nullptr) {
        send_json(job->req, body, status);
        httpd_req_async_handler_complete(job->req);
        job->req = nullptr;
    }
    if (kind == FinishKind::Success && consume_reboot_request()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
}

void LanService::check_timeouts()
{
    std::vector<uint64_t> expired;
    const int64_t now = esp_timer_get_time();
    {
        std::lock_guard<std::mutex> lock(executor_.mtx);
        for (const auto& [id, job] : executor_.active) {
            if (job->ready && !job->response_done.load() && now >= job->deadline_us) {
                expired.push_back(id);
            }
        }
    }
    for (uint64_t id : expired) {
        finish_job(id, FinishKind::Timeout);
    }
}

void LanService::cancel_queued_jobs()
{
    std::vector<uint64_t> cancelled;
    {
        std::lock_guard<std::mutex> lock(executor_.mtx);
        cancelled.assign(executor_.queue.begin(), executor_.queue.end());
        executor_.queue.clear();
    }
    for (uint64_t id : cancelled) {
        finish_job(id, FinishKind::Cancelled);
    }
}

void LanService::stop_httpd_and_streams()
{
    {
        std::unique_lock<std::mutex> lock(executor_.mtx);
        executor_.cv.wait(lock, [this] { return executor_.beginning == 0; });
    }
    while (!executor_.timeout_exited.load()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    MicWsStream::GetInstance().Stop();
    SpeakerWsStream::GetInstance().Stop();
    uint32_t wait_ms = 0;
    while (!MicWsStream::Stopped() || !SpeakerWsStream::Stopped()) {
        if (wait_ms % 1000 == 0) {
            ESP_LOGW(TAG, "waiting for audio tasks to exit before stopping HTTPD");
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms += 10;
    }
    httpd_handle_t server = httpd_.exchange(nullptr);
    if (server != nullptr) {
        httpd_stop(server);
    }
    mcp_photo::clear();
}

void LanService::executor_loop()
{
    while (true) {
        uint64_t id = 0;
        JobPtr job;
        {
            std::unique_lock<std::mutex> lock(executor_.mtx);
            executor_.cv.wait_for(lock, std::chrono::seconds(1), [this] {
                if (executor_.closing) return true;
                for (const uint64_t queued : executor_.queue) {
                    const auto it = executor_.active.find(queued);
                    if (it != executor_.active.end() && it->second->ready) return true;
                }
                return false;
            });
            if (executor_.closing) {
                break;
            }
            for (auto queued = executor_.queue.begin(); queued != executor_.queue.end(); ++queued) {
                auto it = executor_.active.find(*queued);
                if (it != executor_.active.end() && it->second->ready) {
                    id = *queued;
                    job = it->second;
                    executor_.queue.erase(queued);
                    break;
                }
            }
        }

        if (job != nullptr && !job->response_done.load()) {
            std::string error;
            registry_.call(job->tool->name, job->args, &job->result, &error);
        }
        if (id != 0) {
            finish_job(id, FinishKind::Success);
        }
    }

    {
        std::unique_lock<std::mutex> lock(executor_.mtx);
        executor_.cv.wait(lock, [this] { return executor_.beginning == 0; });
    }
    cancel_queued_jobs();
    stop_httpd_and_streams();
    {
        std::lock_guard<std::mutex> lock(executor_.mtx);
        executor_.queue.clear();
        executor_.active.clear();
    }
    executor_.exited.store(true);
    // WithCaps tasks are reclaimed by reclaim() after the application observes
    // stopped(). They must not delete themselves here.
    vTaskDelay(portMAX_DELAY);
}

bool LanService::ui_execute(const UiAction& action, uint32_t timeout_ms)
{
    if (!ui_hooks_.execute) {
        return false;
    }
    return ui_hooks_.execute(action, timeout_ms);
}

bool LanService::show_message(const std::string& text, uint32_t duration_ms)
{
    return ui_execute([this, text, duration_ms] {
        if (ui_hooks_.show_message) {
            ui_hooks_.show_message(text, duration_ms);
        }
    });
}

bool LanService::clear_message()
{
    return ui_execute([this] {
        if (ui_hooks_.clear_message) {
            ui_hooks_.clear_message();
        }
    });
}

int LanService::create_reminder(uint32_t duration_ms, const std::string& message, bool repeat)
{
    if (!ui_hooks_.create_reminder) {
        return -1;
    }
    auto id = std::make_shared<int>(-1);
    const bool ok = ui_execute([this, duration_ms, message, repeat, id] {
        *id = ui_hooks_.create_reminder(duration_ms, message, repeat);
    });
    return ok ? *id : -1;
}

std::vector<ReminderInfo> LanService::get_reminders()
{
    auto result = std::make_shared<std::vector<ReminderInfo>>();
    if (!ui_hooks_.get_reminders) {
        return *result;
    }
    ui_execute([this, result] {
        *result = ui_hooks_.get_reminders();
    });
    return *result;
}

bool LanService::stop_reminder(int id)
{
    if (!ui_hooks_.stop_reminder) {
        return false;
    }
    auto stopped = std::make_shared<bool>(false);
    const bool ok = ui_execute([this, id, stopped] {
        ui_hooks_.stop_reminder(id);
        *stopped = true;
    });
    return ok && *stopped;
}

ServiceStatus LanService::status()
{
    ServiceStatus st;
    st.running = running_.load();
    st.port = CONFIG_MCP_AGENT_PORT;
    st.tools = registry_.size();
    {
        std::lock_guard<std::mutex> lock(stats_.mtx);
        st.requests = stats_.requests;
        st.tool_calls = stats_.tool_calls;
        st.tool_errors = stats_.tool_errors;
        st.last_event = stats_.last_event;
    }
    st.ip = mcp_dev::wifi_ip();
    st.mic_active = MicWsStream::IsActive();
    st.spk_active = SpeakerWsStream::IsActive();
    st.photos = mcp_photo::count();
    return st;
}

void LanService::note_request()
{
    std::lock_guard<std::mutex> lock(stats_.mtx);
    stats_.requests++;
}

void LanService::note_tool_result(bool is_error)
{
    std::lock_guard<std::mutex> lock(stats_.mtx);
    stats_.tool_calls++;
    if (is_error) {
        stats_.tool_errors++;
    }
}

void LanService::push_event(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(stats_.mtx);
    va_list args;
    va_start(args, fmt);
    vsnprintf(stats_.last_event, sizeof(stats_.last_event), fmt, args);
    va_end(args);
}

void LanService::request_reboot()
{
    std::lock_guard<std::mutex> lock(stats_.mtx);
    stats_.reboot_requested = true;
}

bool LanService::consume_reboot_request()
{
    std::lock_guard<std::mutex> lock(stats_.mtx);
    const bool requested = stats_.reboot_requested;
    stats_.reboot_requested = false;
    return requested;
}

bool LanService::http_authorized(httpd_req_t* req)
{
    if (strlen(CONFIG_MCP_AGENT_TOKEN) == 0) {
        return true;
    }
    char auth[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK ||
        strncmp(auth, "Bearer ", 7) != 0) {
        return false;
    }
    const char* presented = auth + 7;
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

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decoded_token_matches(const char* value, size_t value_len, const char* expected)
{
    const size_t expected_len = strlen(expected);
    volatile unsigned char diff = 0;
    size_t decoded_len = 0;
    for (size_t i = 0; i < value_len; ++i) {
        char decoded = value[i];
        if (decoded == '%') {
            if (i + 2 >= value_len) return false;
            const int high = hex_value(value[i + 1]);
            const int low = hex_value(value[i + 2]);
            if (high < 0 || low < 0) return false;
            decoded = (char)((high << 4) | low);
            i += 2;
        } else if (decoded == '+') {
            decoded = ' ';
        }
        if (decoded_len >= expected_len) {
            diff = 1;
        } else {
            diff |= (unsigned char)(decoded ^ expected[decoded_len]);
        }
        ++decoded_len;
    }
    return decoded_len == expected_len && diff == 0;
}

bool LanService::ws_authorized(httpd_req_t* req)
{
    if (strlen(CONFIG_MCP_AGENT_TOKEN) == 0) {
        return true;
    }
    const char* query = strchr(req->uri, '?');
    while (query != nullptr) {
        const char* part = query + 1;
        const char* end = strchr(part, '&');
        const size_t part_len = end != nullptr ? (size_t)(end - part) : strlen(part);
        if (part_len >= 6 && strncmp(part, "token=", 6) == 0) {
            return decoded_token_matches(part + 6, part_len - 6, CONFIG_MCP_AGENT_TOKEN);
        }
        query = end;
    }
    return false;
}

std::string LanService::base_url()
{
    const std::string ip = mcp_dev::wifi_ip();
    if (ip.empty() || ip == "0.0.0.0") {
        return {};
    }
    char buf[40];
    snprintf(buf, sizeof(buf), "http://%s:%d", ip.c_str(), CONFIG_MCP_AGENT_PORT);
    return buf;
}

}  // namespace mcp_lan
