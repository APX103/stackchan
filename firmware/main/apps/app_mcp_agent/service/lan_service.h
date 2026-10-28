/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <esp_http_server.h>
#include <esp_timer.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "../protocol/mcp_registry.h"
#include "reminder_store.h"

namespace mcp_lan {

struct ServiceStatus {
    bool running = false;
    int port = 0;
    std::string ip;
    size_t tools = 0;
    uint32_t requests = 0;
    uint32_t tool_calls = 0;
    uint32_t tool_errors = 0;
    bool mic_active = false;
    bool spk_active = false;
    uint32_t photos = 0;
    std::string last_event;
};

// App-owned lifecycle and execution context. No method in this class is a
// process-global singleton: the application decides when it is created and
// destroyed. Hardware facades in platform/ remain stateless API adapters.
class LanService {
public:
    using UiAction = std::function<void()>;
    struct UiHooks {
        std::function<bool(const UiAction&, uint32_t)> execute;
        std::function<void(const std::string&, uint32_t)> show_message;
        std::function<void()> clear_message;
        std::function<int(uint32_t, const std::string&, bool)> create_reminder;
        std::function<std::vector<ReminderInfo>()> get_reminders;
        std::function<void(int)> stop_reminder;
    };

    LanService();
    ~LanService();

    void set_ui_hooks(UiHooks hooks) { ui_hooks_ = std::move(hooks); }
    bool ui_execute(const UiAction& action, uint32_t timeout_ms = 5000);
    bool show_message(const std::string& text, uint32_t duration_ms);
    bool clear_message();
    int create_reminder(uint32_t duration_ms, const std::string& message, bool repeat);
    std::vector<ReminderInfo> get_reminders();
    bool stop_reminder(int id);

    // Startup is idempotent. Initial HTTPD creation requires an active Wi-Fi
    // connection and initialized LwIP; later outages do not force a restart.
    // Stop is deliberately split into request / stopped / reclaim so the
    // Mooncake thread never waits for HTTPD, drivers, or worker teardown.
    bool start(std::string* err = nullptr);
    void request_stop();
    bool stopped() const;
    void reclaim();
    bool running() const { return running_.load(); }

    ServiceStatus status();
    Registry& registry() { return registry_; }

    // Called by the JSON-RPC endpoint after it has created an async request.
    // Ownership of args, id, and async_req transfers to the service on every
    // path, including queue-full and startup-failure responses.
    void submit_tool(const Tool* tool, cJSON* args, cJSON* id, httpd_req_t* async_req);

    void note_request();
    void note_tool_result(bool is_error);
    void push_event(const char* fmt, ...);
    void request_reboot();
    bool consume_reboot_request();
    bool ws_authorized(httpd_req_t* req);
    std::string base_url();

private:
    struct AsyncToolJob;
    using JobPtr = std::shared_ptr<AsyncToolJob>;

    enum class FinishKind { Success, Timeout, Cancelled, QueueFull, StartFailure };

    struct Executor {
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<uint64_t> queue;
        std::map<uint64_t, JobPtr> active;
        bool closing = false;
        bool started = false;
        std::atomic<bool> exited{true};
        TaskHandle_t task = nullptr;
    };

    struct Stats {
        std::mutex mtx;
        uint32_t requests = 0;
        uint32_t tool_calls = 0;
        uint32_t tool_errors = 0;
        char last_event[96] = {0};
        bool reboot_requested = false;
    };

    static void executor_entry(void* arg);
    static void watchdog_entry(void* arg);
    void executor_loop();
    void check_timeouts();
    void finish_job(uint64_t id, FinishKind kind);
    void finish_request(const JobPtr& job, FinishKind kind);
    void cancel_queued_jobs();
    void stop_httpd_and_streams();
    void mark_start_failure(const std::string& message);

    Registry registry_;
    UiHooks ui_hooks_;
    Executor executor_;
    Stats stats_;
    std::atomic<httpd_handle_t> httpd_{nullptr};
    esp_timer_handle_t watchdog_ = nullptr;
    std::atomic<bool> running_{false};
    uint64_t next_job_id_ = 1;
};

}  // namespace mcp_lan
