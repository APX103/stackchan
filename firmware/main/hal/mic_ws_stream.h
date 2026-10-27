/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

// Real-time microphone stream over WebSocket for the MCP Agent app.
//
// Endpoint : ws://<device-ip>:<port>/mic — registered on the MCP Agent's
//            httpd instance, so the stream only exists while the MCP Agent
//            app is open (see mcp_lan::lan_service).
// Protocol : client connects, sends the text frame {"type":"start"};
//            server replies with a text frame
//              {"type":"hello","sample_rate":16000,"channels":1,
//               "frame_ms":60,"codec":"opus"}
//            followed by continuous binary frames, each a self-contained
//            Opus packet. {"type":"stop"} or disconnecting ends the stream.
//            When a token is configured, the query string must contain
//            ?token=<token>.
// Audio    : raw microphone input (no AEC / noise suppression) captured at
//            the codec rate (24 kHz on this board), resampled to 16 kHz
//            mono before encoding.
// Limits   : one client at a time; a second client receives
//            {"type":"error","message":"busy"}.
class MicWsStream {
public:
    static MicWsStream& GetInstance()
    {
        static MicWsStream instance;
        return instance;
    }

    // Registers the /mic WebSocket handler on `server` and spawns the
    // capture task (idle until a client claims the stream).
    bool Start(httpd_handle_t server);

    // Unregisters the handler, disconnects the client and stops capturing.
    void Stop();

    // True while a client holds the stream.
    static bool IsActive()
    {
        return GetInstance().client_fd_.load() >= 0;
    }

    // Releases the streaming claim of `fd` (if it is the active client).
    // Called when an async send fails — the only reliable way to notice a
    // client that vanished without a stop frame.
    static void ReleaseClient(int fd);

private:
    MicWsStream() = default;
    ~MicWsStream() { Stop(); }
    MicWsStream(const MicWsStream&) = delete;
    MicWsStream& operator=(const MicWsStream&) = delete;

    static esp_err_t WsHandler(httpd_req_t* req);
    static void CaptureTask(void* arg);
    // Creates the capture task on the first client "start" (lazy: the stack
    // is only held while somebody streams). False when out of memory.
    bool EnsureCaptureTask();

    httpd_handle_t server_ = nullptr;
    TaskHandle_t capture_task_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> task_exited_{false};
    // Socket of the active streaming client; -1 while idle. Claimed via
    // compare_exchange so exactly one client wins.
    std::atomic<int> client_fd_{-1};
};
