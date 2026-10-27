/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "speaker_ws_stream.h"

#if !CONFIG_HTTPD_WS_SUPPORT
#error "SpeakerWsStream needs CONFIG_HTTPD_WS_SUPPORT=y (see sdkconfig.defaults)"
#endif

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>
#include <sdkconfig.h>
#include <string.h>

#include <chrono>

#include "mic_ws_stream.h"
#include "mcp_lan/device_access.h"
#include "mcp_lan/lan_service.h"
#include "esp_opus_dec.h"
#include "ws_util.h"

static const char* TAG = "SpkWs";

// Backlog limit while the playback task is busy: ~0.5 s of 60 ms packets.
// Beyond that the sender is outrunning the speaker and frames are dropped.
constexpr size_t kMaxQueuedPackets = 8;

void SpeakerWsStream::PlaybackTask(void* arg) {
    auto& self = *static_cast<SpeakerWsStream*>(arg);

    auto* codec = mcp_dev::audio_codec();
    if (codec == nullptr) {
        ESP_LOGE(TAG, "board has no audio codec, speaker stream disabled");
        self.task_exited_ = true;
        vTaskDelay(portMAX_DELAY);  // WithCaps tasks must not self-delete
        return;
    }
    // The duplex I2S bus is clocked from the output side and the mic capture
    // task already opened it; these calls are idempotent covers for the case
    // where it has not. Control calls are serialized against the mic task.
    {
        std::lock_guard<std::mutex> lock(mcp_dev::codec_mutex());
        codec->EnableOutput(true);
        codec->Start();
    }

    // The decoder is opened lazily on the first packet: while the speaker is
    // unused the ~30 KB it needs stay available to the rest of the system.
    void* decoder = nullptr;
    // Opus frame of 60 ms at the codec output rate (24 kHz -> 1440 samples).
    const size_t frame_samples = codec->output_sample_rate() / 1000 * 60;
    std::vector<int16_t> pcm(frame_samples);
    uint32_t played = 0;
    int decode_errs = 0;

    // Decoder is opened lazily on the first packet; the task parks itself
    // after ~3 s without a client, returning its stack to the heap.
    int idle_ticks = 0;
    while (self.running_) {
        std::shared_ptr<std::vector<uint8_t>> packet;
        {
            std::unique_lock<std::mutex> lock(self.queue_mutex_);
            self.queue_cv_.wait_for(lock, std::chrono::seconds(1),
                                    [&self] { return !self.packet_queue_.empty() || !self.running_; });
            if (!self.running_) {
                break;
            }
            if (self.packet_queue_.empty()) {
                if (self.client_fd_.load() < 0 && ++idle_ticks >= 3) {
                    break;  // no client, no audio: park and free the stack
                }
                continue;  // idle tick: re-check running_
            }
            idle_ticks = 0;
            packet = std::move(self.packet_queue_.front());
            self.packet_queue_.pop_front();
        }

        if (decoder == nullptr) {
            // Same profile the /mic encoder produces: 16 kHz mono 60 ms.
            // The decoder resamples to cfg.sample_rate internally, so
            // decoding straight to the codec rate needs no separate
            // resampler.
            esp_opus_dec_cfg_t cfg = {};
            cfg.sample_rate        = codec->output_sample_rate();
            cfg.channel            = ESP_AUDIO_MONO;
            cfg.frame_duration     = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
            cfg.self_delimited     = false;
            if (esp_opus_dec_open(&cfg, sizeof(cfg), &decoder) != ESP_AUDIO_ERR_OK || decoder == nullptr) {
                ESP_LOGE(TAG, "failed to open opus decoder, packet dropped");
                decoder = nullptr;
                continue;
            }
            ESP_LOGI(TAG, "playback task ready (opus -> %d Hz)", codec->output_sample_rate());
        }

        esp_audio_dec_in_raw_t raw = {};
        raw.buffer                 = packet->data();
        raw.len                    = (uint32_t)packet->size();
        raw.frame_recover          = ESP_AUDIO_DEC_RECOVERY_NONE;
        esp_audio_dec_out_frame_t out = {};
        out.buffer                 = (uint8_t*)pcm.data();
        out.len                    = (uint32_t)(pcm.size() * sizeof(int16_t));
        esp_audio_dec_info_t info  = {};
        if (esp_opus_dec_decode(decoder, &raw, &out, &info) == ESP_AUDIO_ERR_OK && out.decoded_size > 0) {
            // OutputData writes exactly pcm.size() samples; trim to what the
            // decoder produced (constant for 60 ms frames, but stay safe).
            pcm.resize(out.decoded_size / sizeof(int16_t));
            codec->OutputData(pcm);
            pcm.resize(frame_samples);
            if (++played == 1) {
                ESP_LOGI(TAG, "first opus packet decoded and played (%u bytes)", (unsigned)packet->size());
            }
        } else {
            if (++decode_errs <= 3) {
                ESP_LOGW(TAG, "opus decode failed (%u bytes), packet skipped", (unsigned)packet->size());
            }
        }
    }

    if (decoder != nullptr) {
        esp_opus_dec_close(decoder);
    }
    // Only release the output device when the mic is not streaming: the
    // duplex I2S bus is clocked from the output side and the mic capture
    // would read back zeroes without it.
    if (!MicWsStream::IsActive()) {
        std::lock_guard<std::mutex> lock(mcp_dev::codec_mutex());
        codec->EnableOutput(false);
    }
    ESP_LOGI(TAG, "playback task stopped (%u packets played)", (unsigned)played);
    self.task_exited_ = true;
    // WithCaps tasks must NOT self-delete: park here, the owner deletes us.
    vTaskDelay(portMAX_DELAY);
}

esp_err_t SpeakerWsStream::WsHandler(httpd_req_t* req) {
    auto& self = GetInstance();

    // NOTE: no method check here — same reasoning as MicWsStream.
    if (!mcp_lan::service_ws_authorized(req)) {
        wsutil::SendError(req, "unauthorized");
        return ESP_FAIL;  // close the session
    }

    httpd_ws_frame_t pkt;
    std::vector<uint8_t> payload;
    esp_err_t ret = wsutil::RecvFrame(req, &pkt, &payload);
    if (ret == ESP_ERR_INVALID_SIZE) {
        wsutil::SendError(req, "frame too large");
        return ESP_FAIL;
    }
    if (ret != ESP_OK) {
        return ret;  // transport-level failure, httpd closes the session
    }

    int fd = httpd_req_to_sockfd(req);

    // Control frames (e.g. an unexpected PONG) are consumed silently.
    if (pkt.type != HTTPD_WS_TYPE_TEXT && pkt.type != HTTPD_WS_TYPE_BINARY) {
        return ESP_OK;
    }

    if (pkt.type == HTTPD_WS_TYPE_BINARY) {
        if (self.client_fd_.load() != fd) {
            return wsutil::SendError(req, "not started (send {\"type\":\"start\"} first)");
        }
        if (payload.empty()) {
            return ESP_OK;  // zero-length keep-alive
        }
        {
            std::lock_guard<std::mutex> lock(self.queue_mutex_);
            if (self.packet_queue_.size() >= kMaxQueuedPackets) {
                return wsutil::SendError(req, "speaker backlog, frame dropped");
            }
            self.packet_queue_.push_back(
                std::make_shared<std::vector<uint8_t>>(std::move(payload)));
        }
        self.queue_cv_.notify_one();
        return ESP_OK;
    }

    payload.push_back('\0');
    const char* msg = (const char*)payload.data();

    if (strstr(msg, "\"start\"") != nullptr) {
        int expected = -1;
        if (self.client_fd_.compare_exchange_strong(expected, fd)) {
            // Playback task is created on demand — its stack is only worth
            // holding while a client actually streams.
            if (!self.EnsurePlaybackTask()) {
                self.client_fd_ = -1;
                return wsutil::SendError(req, "no memory for playback task");
            }
            ESP_LOGI(TAG, "client fd %d started playback", fd);
            mcp_lan::service_push_event("speaker: client started");
            {
                // Fresh session: nothing from a previous client may still be
                // waiting in the queue.
                std::lock_guard<std::mutex> lock(self.queue_mutex_);
                self.packet_queue_.clear();
            }
            static const char* kHello =
                "{\"type\":\"hello\",\"sample_rate\":16000,\"channels\":1,"
                "\"frame_ms\":60,\"codec\":\"opus\"}";
            if (wsutil::SendText(req, kHello) != ESP_OK) {
                self.client_fd_ = -1;
            }
            return ESP_OK;
        }
        if (self.client_fd_.load() == fd) {
            return ESP_OK;  // idempotent start by the active client
        }
        return wsutil::SendError(req, "busy");
    }

    if (strstr(msg, "\"stop\"") != nullptr) {
        int current = self.client_fd_.load();
        if (current == fd) {
            // Only the claim is released; packets already queued play out.
            self.client_fd_.compare_exchange_strong(current, -1);
            mcp_lan::service_push_event("speaker: client stopped");
        }
        return ESP_OK;
    }

    return wsutil::SendError(req, "unsupported message (expected start/stop)");
}

bool SpeakerWsStream::Start(httpd_handle_t server) {
    if (server == nullptr) {
        return false;
    }
    if (server_ != nullptr) {
        return true;  // already running
    }
    server_ = server;
    // The playback task is created lazily on the first client "start" (see
    // WsHandler): its 24 KB stack must not sit in the heap while nobody
    // streams — internal RAM is the scarcest resource on this board.
    httpd_uri_t ws_speaker              = {};
    ws_speaker.uri                      = "/speaker";
    ws_speaker.method                   = HTTP_GET;
    ws_speaker.handler                  = WsHandler;
    ws_speaker.is_websocket             = true;
    ws_speaker.handle_ws_control_frames = false;
    if (httpd_register_uri_handler(server_, &ws_speaker) != ESP_OK) {
        ESP_LOGE(TAG, "failed to register /speaker ws handler");
        server_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "speaker stream ready at ws://<device-ip>:%d/speaker", CONFIG_MCP_AGENT_PORT);
    return true;
}

int SpeakerWsStream::StackHighWaterBytes() {
    auto& self = GetInstance();
    if (self.playback_task_ == nullptr) {
        return -1;
    }
    return (int)(uxTaskGetStackHighWaterMark(self.playback_task_) * sizeof(StackType_t));
}

bool SpeakerWsStream::EnsurePlaybackTask() {
    // Clean up a previously parked task first.
    if (playback_task_ != nullptr && task_exited_.load()) {
        vTaskDeleteWithCaps(playback_task_);
        playback_task_ = nullptr;
    }
    if (playback_task_ != nullptr) {
        return true;
    }
    running_     = true;
    task_exited_ = false;
    // 12 KB internal stack (tuned via the spk_stack_free telemetry on GET /);
    // BYTES with WithCaps; must not be PSRAM (flash-write cache assert).
    if (xTaskCreatePinnedToCoreWithCaps(PlaybackTask, "spk_ws", 2048 * 6, this, 4, &playback_task_, 1,
                                        MALLOC_CAP_INTERNAL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create playback task (out of internal RAM?)");
        playback_task_ = nullptr;
        running_       = false;
        return false;
    }
    return true;
}

void SpeakerWsStream::Stop() {
    if (server_ == nullptr) {
        return;
    }
    client_fd_ = -1;
    httpd_unregister_uri(server_, "/speaker");
    running_ = false;
    queue_cv_.notify_all();
    if (playback_task_ != nullptr) {
        // The queue wait wakes on running_=false within a second, so the
        // task normally parks itself quickly; then we free it.
        for (int i = 0; i < 15 && !task_exited_.load(); i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!task_exited_.load()) {
            ESP_LOGW(TAG, "playback task did not exit in time, force-deleting (decoder leaked)");
        }
        vTaskDeleteWithCaps(playback_task_);
        playback_task_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        packet_queue_.clear();
    }
    server_ = nullptr;
    ESP_LOGI(TAG, "speaker stream stopped");
}
