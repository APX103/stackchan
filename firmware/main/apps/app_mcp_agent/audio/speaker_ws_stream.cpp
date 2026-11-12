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
#include <exception>
#include <new>

#include "codec_bus.h"
#include "../platform/device_access.h"
#include "../service/lan_service.h"
#include "esp_opus_dec.h"
#include "../protocol/ws_util.h"

static const char* TAG = "SpkWs";

// Backlog limit while the playback task is busy: ~0.5 s of 60 ms packets.
// Beyond that the sender is outrunning the speaker and frames are dropped.
constexpr size_t kMaxQueuedPackets = 100;  // SEP: ~6s buffer, absorb LAN bursts
constexpr int kFrameMs = 60;

namespace {
struct OpusDecoderDeleter {
    void operator()(void* decoder) const { if (decoder != nullptr) esp_opus_dec_close(decoder); }
};
}  // namespace

void SpeakerWsStream::PlaybackTask(void* arg) {
    auto& self = *static_cast<SpeakerWsStream*>(arg);
    try {
        PlaybackTaskImpl(arg);
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "playback task aborted: %s", e.what());
    } catch (...) {
        ESP_LOGE(TAG, "playback task aborted: unknown exception");
    }
    self.running_ = false;
    self.client_fd_ = -1;
    self.task_exited_ = true;
    vTaskDelay(portMAX_DELAY);
}

void SpeakerWsStream::PlaybackTaskImpl(void* arg) {
    auto& self = *static_cast<SpeakerWsStream*>(arg);

    // SEP: this task is persistent — pre-spawned at service start (see
    // lan_service.cpp), while internal RAM is still contiguous. Creating it
    // lazily inside a play_opus request loses the memory lottery: the
    // request's own ~28 KB JSON body occupies the largest free block first
    // (observed: largest 10240 < 12288-byte stack -> "playback task
    // unavailable"). Because it now outlives every client and every tool
    // call, it must not hold the codec output or the ~30 KB decoder while
    // idle: both are acquired on the first packet and released after
    // sustained idle, so the mic side is never blocked either.
    void* decoder = nullptr;
    std::unique_ptr<void, OpusDecoderDeleter> decoder_guard(nullptr);
    AudioCodec* codec = nullptr;
    bool output_acquired = false;
    size_t frame_samples = 0;
    std::vector<int16_t> pcm;
    uint32_t played = 0;
    int decode_errs = 0;
    int idle_secs = 0;
    bool codec_warned = false;
    constexpr int kIdleReleaseSecs = 5;

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
                // No packets: after a quiet spell with no WS client, hand the
                // decoder (~30 KB) and the codec output back. The task itself
                // stays parked on the condition variable.
                if (self.client_fd_.load() < 0 && ++idle_secs >= kIdleReleaseSecs &&
                    (decoder != nullptr || output_acquired)) {
                    lock.unlock();  // releases below take their own locks
                    decoder_guard.reset();
                    decoder = nullptr;  // reset() alone leaves the raw pointer stale
                    if (output_acquired) {
                        mcp_audio::release_output(codec);
                        output_acquired = false;
                    }
                    self.session_busy_.store(false);
                    ESP_LOGI(TAG, "playback idle: decoder/output released");
                }
                continue;  // idle tick: re-check running_
            }
            idle_secs = 0;
            packet = std::move(self.packet_queue_.front());
            self.packet_queue_.pop_front();
        }

        if (!output_acquired) {
            // The duplex I2S bus is clocked from the output side and the mic
            // capture task may already have opened it; control calls are
            // serialized against the mic task through codec_bus.
            codec = mcp_dev::audio_codec("speaker playback");
            if (codec == nullptr) {
                if (!codec_warned) {
                    ESP_LOGE(TAG, "audio codec unavailable, packets dropped");
                    codec_warned = true;
                }
                continue;
            }
            mcp_audio::acquire_output(codec);
            {
                std::lock_guard<std::mutex> lock(mcp_dev::codec_mutex());
                codec->Start();
            }
            output_acquired = true;
            self.session_busy_.store(true);
            // Opus frame of 60 ms at the codec output rate (24 kHz -> 1440
            // samples); sized once the codec is known.
            frame_samples = codec->output_sample_rate() / 1000 * kFrameMs;
            pcm.assign(frame_samples, 0);
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
            decoder_guard.reset(decoder);
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

    decoder_guard.reset();
    if (output_acquired) {
        mcp_audio::release_output(codec);
    }
    self.session_busy_.store(false);
    ESP_LOGI(TAG, "playback task stopped (%u packets played)", (unsigned)played);
}

void SpeakerWsStream::HandleDisconnect(httpd_handle_t server, int fd) {
    auto& self = GetInstance();
    if (self.server_ != server) {
        return;
    }
    int current = self.client_fd_.load();
    if (current == fd && self.client_fd_.compare_exchange_strong(current, -1)) {
        // SEP: the playback task is persistent — a client leaving must not
        // kill it (recreating it mid-request loses the memory lottery; see
        // PlaybackTaskImpl). Drop buffered packets so the next session starts
        // clean; the task parks and releases decoder/output after idle.
        {
            std::lock_guard<std::mutex> lock(self.queue_mutex_);
            self.packet_queue_.clear();
        }
        self.queue_cv_.notify_all();
        if (self.mcp_service_ != nullptr) {
            self.mcp_service_->push_event("speaker: client disconnected");
        }
    }
}

esp_err_t SpeakerWsStream::WsPreHandshake(httpd_req_t* req) {
    auto& self = GetInstance();
    if (self.mcp_service_ == nullptr || !self.mcp_service_->running() ||
        !self.mcp_service_->ws_authorized(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t SpeakerWsStream::WsHandler(httpd_req_t* req) {
    try {
        return WsHandlerImpl(req);
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "speaker WS request aborted: out of memory");
        wsutil::SendError(req, "out of memory");
    } catch (...) {
        ESP_LOGE(TAG, "speaker WS request aborted: unknown exception");
        wsutil::SendError(req, "internal error");
    }
    return ESP_FAIL;
}

esp_err_t SpeakerWsStream::WsHandlerImpl(httpd_req_t* req) {
    auto& self = GetInstance();

    // NOTE: no method check here — same reasoning as MicWsStream.
    if (self.mcp_service_ == nullptr || !self.mcp_service_->running() ||
        !self.mcp_service_->ws_authorized(req)) {
        wsutil::SendError(req, "unauthorized");
        return ESP_FAIL;  // close the session
    }

    httpd_ws_frame_t pkt;
    std::vector<uint8_t> payload;
    const int fd = httpd_req_to_sockfd(req);
    esp_err_t ret = wsutil::RecvFrame(req, &pkt, &payload);
    if (ret == ESP_ERR_INVALID_SIZE) {
        wsutil::SendError(req, "frame too large");
        return ESP_FAIL;
    }
    if (ret != ESP_OK) {
        HandleDisconnect(self.server_, fd);
        return ret;
    }

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
            // The playback task is normally already alive (pre-spawned at
            // service start); this is only a safety net if it died.
            if (!self.EnsurePlaybackTask()) {
                const bool restarting = self.playback_task_ != nullptr && !self.task_exited_.load();
                self.client_fd_ = -1;
                if (self.mcp_service_ != nullptr) {
                    self.mcp_service_->push_event(restarting ? "speaker: playback task is restarting" :
                                                               "speaker: audio init failed; see serial log");
                }
                return wsutil::SendError(req, restarting ? "playback task is restarting" :
                                                          "audio unavailable or no memory for playback task");
            }
            ESP_LOGI(TAG, "client fd %d started playback", fd);
            if (self.mcp_service_ != nullptr) {
                self.mcp_service_->push_event("speaker: client started");
            }
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
                self.queue_cv_.notify_all();
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
            // SEP: 打断/停播。常驻播放任务不许杀——只释放客户端并清空
            // 队列,任务回到驻留待命(见 PlaybackTaskImpl)。
            self.client_fd_.compare_exchange_strong(current, -1);
            {
                std::lock_guard<std::mutex> lock(self.queue_mutex_);
                self.packet_queue_.clear();
            }
            self.queue_cv_.notify_all();
            if (self.mcp_service_ != nullptr) {
                self.mcp_service_->push_event("speaker: client stopped");
            }
        }
        return ESP_OK;
    }

    return wsutil::SendError(req, "unsupported message (expected start/stop)");
}

bool SpeakerWsStream::Start(httpd_handle_t server, mcp_lan::LanService& service) {
    if (server == nullptr) {
        return false;
    }
    mcp_service_ = &service;
    if (server_ != nullptr) {
        return true;  // already running
    }
    server_ = server;
    // The playback task is created lazily on the first client "start" (see
    // WsHandler): its 16 KB stack must not sit in the heap while nobody
    // streams — internal RAM is the scarcest resource on this board.
    httpd_uri_t ws_speaker              = {};
    ws_speaker.uri                      = "/speaker";
    ws_speaker.method                   = HTTP_GET;
    ws_speaker.handler                  = WsHandler;
    ws_speaker.is_websocket             = true;
    ws_speaker.handle_ws_control_frames = false;
#if CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
    ws_speaker.ws_pre_handshake_cb = WsPreHandshake;
#endif
    if (httpd_register_uri_handler(server, &ws_speaker) != ESP_OK) {
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

void SpeakerWsStream::Reclaim() {
    auto& self = GetInstance();
    if (self.playback_task_ != nullptr && self.task_exited_.load()) {
        vTaskDeleteWithCaps(self.playback_task_);
        self.playback_task_ = nullptr;
    }
    self.mcp_service_ = nullptr;
}

bool SpeakerWsStream::EnsurePlaybackTask() {
    if (mcp_dev::audio_codec("speaker playback") == nullptr) {
        return false;
    }
    if (playback_task_ != nullptr) {
        if (!task_exited_.load()) {
            // SEP: the task is persistent — alive means parked on the queue
            // and ready to play, not "still stopping".
            return true;
        }
        vTaskDeleteWithCaps(playback_task_);
        playback_task_ = nullptr;
    }
    running_     = true;
    task_exited_ = false;
    // 12 KB internal stack: decode needs far less than 16 KB and the caps
    // allocator fails on a busy device when the largest internal block drops
    // below the requested size (~14 KB observed). Watch spk_stack_free.
    // BYTES with WithCaps; must not be PSRAM (flash-write cache assert).
    if (xTaskCreatePinnedToCoreWithCaps(PlaybackTask, "spk_ws", 2048 * 6, this, 4, &playback_task_, 1,
                                        MALLOC_CAP_INTERNAL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create playback task: free internal=%u, largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
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
    // SEP 常驻任务:服务停止不杀任务,只卸载服务端与客户端;进行中的
    // 播放会话由任务在空闲宽限后自行释放(decoder/output)。
    queue_cv_.notify_all();
    // The playback task owns decoder and codec cleanup. It is reclaimed by
    // EnsurePlaybackTask only after task_exited_ becomes true.
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        packet_queue_.clear();
    }
    server_ = nullptr;
    ESP_LOGI(TAG, "speaker stream stopped");
}

bool SpeakerWsStream::EnqueueOpusFrames(const std::vector<std::vector<uint8_t>>& frames, std::string* err) {
    auto fail = [err](const char* why) {
        if (err != nullptr) *err = why;
        ESP_LOGW(TAG, "EnqueueOpusFrames rejected: %s", why);
        return false;
    };
    if (frames.empty()) return fail("no frames");
    if (IsActive()) return fail("a websocket client owns the speaker stream");
    if (!EnsurePlaybackTask()) return fail("playback task unavailable (codec busy or no memory)");

    size_t queued = 0, dropped = 0;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (const auto& f : frames) {
            if (f.empty()) continue;
            if (packet_queue_.size() >= kMaxQueuedPackets) {
                ++dropped;
                continue;
            }
            packet_queue_.push_back(std::make_shared<std::vector<uint8_t>>(f));
            ++queued;
        }
    }
    queue_cv_.notify_all();
    ESP_LOGI(TAG, "MCP play_opus: %u frames queued (%u dropped)", (unsigned)queued, (unsigned)dropped);
    return queued > 0;
}
