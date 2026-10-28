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
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

// Real-time speaker playback stream over WebSocket — the downstream mirror
// of MicWsStream.
//
// Endpoint : ws://<device-ip>:<port>/speaker — registered on the MCP Agent's
//            httpd instance, so playback only exists while the MCP Agent app
//            is open (see mcp_lan::lan_service). With a token configured the
//            query string must contain ?token=<token>.
// Protocol : the client sends the text frame {"type":"start"}; the server
//            replies with a text frame
//              {"type":"hello","sample_rate":16000,"channels":1,
//               "frame_ms":60,"codec":"opus"}
//            and then accepts binary frames, each one a self-contained Opus
//            packet (16 kHz mono, 60 ms — the exact format /mic produces, so
//            a recording captured from /mic replays verbatim). Pacing is the
//            sender's job (~16.7 frames/s); the device plays what arrives.
//            {"type":"stop"} or disconnecting ends playback.
// Audio    : decoded directly at the codec output rate (24 kHz on this
//            board) and written to the output channel. There is no echo
//            cancellation — playing audio while /mic streams will feed the
//            speaker back into the microphone.
// Limits   : one client at a time; a second client receives
//            {"type":"error","message":"busy"}. Incoming packets are
//            shallow-buffered (a fraction of a second); a sender that
//            outruns the speaker drops frames.
namespace mcp_lan {
class LanService;
}

class SpeakerWsStream {
public:
    static SpeakerWsStream& GetInstance()
    {
        static SpeakerWsStream instance;
        return instance;
    }

    // Registers the /speaker WebSocket handler on `server` and spawns the
    // playback task (idle until a client claims the stream).
    bool Start(httpd_handle_t server, mcp_lan::LanService& service);

    // Unregisters the handler, disconnects the client and stops playback.
    void Stop();

    static bool Stopped()
    {
        auto& self = GetInstance();
        return self.server_ == nullptr &&
               (self.playback_task_ == nullptr || self.task_exited_.load());
    }
    static void Reclaim();

    // True while a client holds the playback stream.
    static bool IsActive()
    {
        return GetInstance().client_fd_.load() >= 0;
    }

    // Remaining free stack of the playback task in bytes (-1 when not
    // running) — telemetry for GET /.
    static int StackHighWaterBytes();

    static void HandleDisconnect(httpd_handle_t server, int fd);

private:
    SpeakerWsStream() = default;
    ~SpeakerWsStream() { Stop(); }
    SpeakerWsStream(const SpeakerWsStream&) = delete;
    SpeakerWsStream& operator=(const SpeakerWsStream&) = delete;

    static esp_err_t WsHandler(httpd_req_t* req);
    static void PlaybackTask(void* arg);
    // Creates the playback task on the first client "start" (lazy: the
    // stack is only held while somebody streams). False when out of memory.
    bool EnsurePlaybackTask();

    httpd_handle_t server_ = nullptr;
    mcp_lan::LanService* mcp_service_ = nullptr;
    TaskHandle_t playback_task_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> task_exited_{false};

    // Hand-off queue from the httpd worker to the playback task. A FreeRTOS
    // queue memcpy would break shared_ptr's refcount, so this is a
    // std::deque + condition_variable hand-off.
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::shared_ptr<std::vector<uint8_t>>> packet_queue_;

    // Socket of the active playback client; -1 while idle. Claimed via
    // compare_exchange so exactly one client wins.
    std::atomic<int> client_fd_{-1};
};
