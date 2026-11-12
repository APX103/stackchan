/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "mic_ws_stream.h"

#if !CONFIG_HTTPD_WS_SUPPORT
#error "MicWsStream needs CONFIG_HTTPD_WS_SUPPORT=y (see sdkconfig.defaults)"
#endif

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>
#include <string.h>

#include <exception>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include <cstdlib>
#include <algorithm>

#include "../platform/device_access.h"
#include "../service/lan_service.h"
#include "codec_bus.h"
#include "esp_opus_enc.h"
#include "esp_ae_rate_cvt.h"
#include "../protocol/ws_util.h"

static const char* TAG = "MicWs";

// Stream geometry: Opus wants 16 kHz mono; the codec captures at 24 kHz
// interleaved stereo (slot 0 = mic).
constexpr int kOpusSampleRate = 16000;
constexpr int kFrameMs        = 60;
// Digital gain for the mic channel (see comment at the de-interleave step).
constexpr int32_t kMicDigitalGain = 16;

namespace {
// httpd_queue_work takes a plain function pointer, so the send job (with the
// heap-kept packet) is passed through a small struct.
struct WsSendJob {
    httpd_handle_t server;
    int fd;
    uint64_t session;
    std::shared_ptr<std::vector<uint8_t>> packet;
};

void ws_send_job(void* arg) {
    auto* job = static_cast<WsSendJob*>(arg);
    if (!MicWsStream::IsSessionActive(job->fd, job->session)) {
        delete job;
        return;
    }
    httpd_ws_frame_t frame = {};
    frame.type             = HTTPD_WS_TYPE_BINARY;
    frame.payload          = job->packet->data();
    frame.len              = job->packet->size();
    if (httpd_ws_send_frame_async(job->server, job->fd, &frame) != ESP_OK) {
        // Session is gone (client disconnected): free the stream so the
        // capture task stops feeding a dead socket.
        MicWsStream::ReleaseClient(job->fd, job->session);
    } else {
        httpd_sess_update_lru_counter(job->server, job->fd);
    }
    delete job;
}

struct OpusEncoderDeleter {
    void operator()(void* encoder) const { if (encoder != nullptr) esp_opus_enc_close(encoder); }
};
struct ResamplerDeleter {
    void operator()(esp_ae_rate_cvt_handle_t resampler) const {
        if (resampler != nullptr) esp_ae_rate_cvt_close(resampler);
    }
};

}  // namespace

int MicWsStream::StackHighWaterBytes() {
    auto& self = MicWsStream::GetInstance();
    if (self.capture_task_ == nullptr) {
        return -1;
    }
    return (int)(uxTaskGetStackHighWaterMark(self.capture_task_) * sizeof(StackType_t));
}

void MicWsStream::ReleaseClient(int fd, uint64_t session) {
    auto& self = MicWsStream::GetInstance();
    int current = self.client_fd_.load();
    if (current == fd && self.session_.load() == session) {
        self.client_fd_.compare_exchange_strong(current, -1);
        self.session_++;
        // SEP: the capture task is persistent — do not kill it here. It
        // notices the lost claim and releases its session (encoder,
        // resampler, codec input) after the idle grace.
        if (self.mcp_service_ != nullptr) {
            self.mcp_service_->push_event("mic: client dropped");
        }
        ESP_LOGI(TAG, "client fd %d released", fd);
    }
}

void MicWsStream::HandleDisconnect(httpd_handle_t server, int fd) {
    auto& self = GetInstance();
    if (self.server_ != server) {
        return;
    }
    ReleaseClient(fd, self.session_.load());
}

esp_err_t MicWsStream::WsPreHandshake(httpd_req_t* req) {
    auto& self = MicWsStream::GetInstance();
    if (self.mcp_service_ == nullptr || !self.mcp_service_->running() ||
        !self.mcp_service_->ws_authorized(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t MicWsStream::WsHandler(httpd_req_t* req) {
    try {
        return WsHandlerImpl(req);
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "mic WS request aborted: out of memory");
        wsutil::SendError(req, "out of memory");
    } catch (...) {
        ESP_LOGE(TAG, "mic WS request aborted: unknown exception");
        wsutil::SendError(req, "internal error");
    }
    return ESP_FAIL;
}

esp_err_t MicWsStream::WsHandlerImpl(httpd_req_t* req) {
    auto& self = MicWsStream::GetInstance();

    // NOTE: no method check here. This handler is invoked once per incoming
    // WebSocket frame (the HTTP GET handshake never reaches it), and in that
    // context req->method is not a reliable HTTP_GET — returning an error
    // would write a plain HTTP response into the stream and corrupt the
    // protocol.

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
        // Larger than the drain limit: the payload cannot be skipped, so
        // close rather than desynchronize every following frame.
        wsutil::SendError(req, "frame too large");
        return ESP_FAIL;
    }
    if (ret != ESP_OK) {
        ReleaseClient(fd, self.session_.load());
        return ret;
    }

    // Control frames (e.g. an unexpected PONG) are consumed silently.
    if (pkt.type != HTTPD_WS_TYPE_TEXT && pkt.type != HTTPD_WS_TYPE_BINARY) {
        return ESP_OK;
    }
    // /mic is upstream-only: binary input is a protocol mistake, not a
    // reason to drop the session.
    if (pkt.type == HTTPD_WS_TYPE_BINARY) {
        return wsutil::SendError(req, "binary frames are not accepted on /mic");
    }

    payload.push_back('\0');
    const char* msg = (const char*)payload.data();

    // start: claim the stream, reply with hello, capture task starts pushing
    if (strstr(msg, "\"start\"") != nullptr) {
        int expected = -1;
        if (self.client_fd_.compare_exchange_strong(expected, fd)) {
            self.session_++;
            // The capture task is normally already alive (pre-spawned at
            // service start); this is only a safety net if it died.
            if (!self.EnsureCaptureTask()) {
                const bool restarting = self.capture_task_ != nullptr && !self.task_exited_.load();
                self.client_fd_ = -1;
                self.session_++;
                if (self.mcp_service_ != nullptr) {
                    self.mcp_service_->push_event(restarting ? "mic: capture task is restarting" :
                                                               "mic: audio init failed; see serial log");
                }
                return wsutil::SendError(req, restarting ? "capture task is restarting" :
                                                          "audio unavailable or no memory for capture task");
            }
            ESP_LOGI(TAG, "client fd %d started streaming", fd);
            if (self.mcp_service_ != nullptr) {
                self.mcp_service_->push_event("mic: client started");
            }
            static const char* kHello =
                "{\"type\":\"hello\",\"sample_rate\":16000,\"channels\":1,"
                "\"frame_ms\":60,\"codec\":\"opus\"}";
            if (wsutil::SendText(req, kHello) != ESP_OK) {
                ReleaseClient(fd, self.session_.load());
            }
            return ESP_OK;
        }
        if (self.client_fd_.load() == fd) {
            return ESP_OK;  // idempotent start by the active client
        }
        // Already serving a client: reject politely.
        return wsutil::SendError(req, "busy");
    }

    // stop: release the client claim; the persistent capture task releases
    // its session after the idle grace.
    if (strstr(msg, "\"stop\"") != nullptr) {
        int current = self.client_fd_.load();
        if (current == fd) {
            self.client_fd_.compare_exchange_strong(current, -1);
            self.session_++;
            if (self.mcp_service_ != nullptr) {
                self.mcp_service_->push_event("mic: client stopped");
            }
        }
        return ESP_OK;
    }

    return wsutil::SendError(req, "unsupported message (expected start/stop)");
}

void MicWsStream::CaptureTask(void* arg) {
    auto& self = *static_cast<MicWsStream*>(arg);
    try {
        CaptureTaskImpl(arg);
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "capture task aborted: %s", e.what());
    } catch (...) {
        ESP_LOGE(TAG, "capture task aborted: unknown exception");
    }
    self.running_ = false;
    self.client_fd_ = -1;
    self.session_++;
    self.task_exited_ = true;
    vTaskDelay(portMAX_DELAY);
}

void MicWsStream::CaptureTaskImpl(void* arg) {
    auto& self = *static_cast<MicWsStream*>(arg);

    // SEP: this task is persistent — pre-spawned at service start (see
    // lan_service.cpp). Lazily creating its 22 KB stack inside a client
    // "start" loses the memory lottery exactly like the speaker playback
    // task did (observed: largest 21504 < 22528 right after a speaker
    // session). The streaming session — codec input bus, resampler,
    // encoder, buffers — is acquired when a client claims the stream and
    // released after the idle grace, so a parked task costs only its stack.
    AudioCodec* codec = nullptr;
    bool bus_acquired = false;
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    std::unique_ptr<void, ResamplerDeleter> resampler_guard(nullptr);
    bool need_resample = false;
    void* encoder = nullptr;
    std::unique_ptr<void, OpusEncoderDeleter> encoder_guard(nullptr);
    size_t frame_samples = 0;
    std::vector<int16_t> in_buf, mic_buf, rs_buf, pcm;
    std::vector<uint8_t> enc_buf;

    auto release_session = [&]() {
        encoder_guard.reset();
        encoder = nullptr;
        resampler_guard.reset();
        resampler = nullptr;
        if (bus_acquired) {
            {
                std::lock_guard<std::mutex> lock(mcp_dev::codec_mutex());
                codec->EnableInput(false);
            }
            mcp_audio::release_output(codec);
            bus_acquired = false;
        }
        self.session_busy_.store(false);
    };
    // One-shot progress logs: enough to tell where the pipeline stalls
    // without flooding the console.
    bool logged_session   = false;
    bool logged_first_read = false;
    bool logged_first_enc  = false;
    int rs_err_count       = 0;
    int queue_full_count   = 0;
    // 10s statistics to diagnose rate mismatches
    uint32_t stat_tick     = 0;
    uint32_t stat_reads    = 0;
    uint32_t stat_produced = 0;
    uint32_t stat_frames   = 0;

    int idle_ticks = 0;
    while (self.running_) {
        const int fd = self.client_fd_;
        if (fd < 0) {
            // No client: after the idle grace, hand the session (encoder,
            // resampler, codec input) back. The task itself stays parked.
            if ((bus_acquired || encoder != nullptr) && ++idle_ticks >= 30) {
                release_session();
                logged_session = false;
                ESP_LOGI(TAG, "mic idle: session released");
            }
            vTaskDelay(pdMS_TO_TICKS(100));  // idle, no client
            continue;
        }
        idle_ticks = 0;

        if (!bus_acquired) {
            codec = mcp_dev::audio_codec("mic capture");
            if (codec == nullptr) {
                ESP_LOGE(TAG, "board has no audio codec, mic stream disabled");
                self.client_fd_ = -1;
                self.session_++;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            mcp_audio::acquire_output(codec);
            {
                // The speaker task initializes its codec side at the same
                // time; control calls must not interleave (see
                // device_access.h).
                std::lock_guard<std::mutex> lock(mcp_dev::codec_mutex());
                codec->EnableInput(true);
                codec->Start();
            }
            bus_acquired = true;
            self.session_busy_.store(true);

            // SEP 2026-11-12 终审定案(1kHz 音 + 256 样本分相位能量实测):
            // DMA 2-slot 交错 [参考通道, 麦克风] @48k 总率,即每槽 24k
            //(=配置值)。麦克风在奇数索引(offset1/3 peak~31k,偶数位只有
            // 参考串音 ~4k/静音)。旧代码取偶数位 = 从第一天起就取错了
            // 通道:ASR 乱码(いや、わ)、唤醒词永不命中皆源于此。
            const uint32_t in_rate_config = codec->input_sample_rate();
            need_resample = in_rate_config != kOpusSampleRate;
            if (need_resample) {
                esp_ae_rate_cvt_cfg_t rs_cfg  = {};
                rs_cfg.src_rate               = in_rate_config;
                rs_cfg.dest_rate              = kOpusSampleRate;
                rs_cfg.channel                = 1;
                rs_cfg.bits_per_sample        = ESP_AUDIO_BIT16;
                rs_cfg.complexity             = 2;
                rs_cfg.perf_type              = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
                if (esp_ae_rate_cvt_open(&rs_cfg, &resampler) != ESP_AE_ERR_OK) {
                    ESP_LOGE(TAG, "failed to open resampler %lu->%d", in_rate_config, kOpusSampleRate);
                    release_session();
                    self.client_fd_ = -1;
                    self.session_++;
                    continue;
                }
                resampler_guard.reset(resampler);
            }

            // Opus encoder, same profile the cloud agent's audio service
            // encodes with: 16k mono, 60 ms frames, complexity 0, DTX+VBR.
            esp_opus_enc_config_t enc_cfg = {};
            enc_cfg.sample_rate           = ESP_AUDIO_SAMPLE_RATE_16K;
            enc_cfg.channel               = ESP_AUDIO_MONO;
            enc_cfg.bits_per_sample       = ESP_AUDIO_BIT16;
            enc_cfg.bitrate               = ESP_OPUS_BITRATE_AUTO;
            enc_cfg.frame_duration        = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
            enc_cfg.application_mode      = ESP_OPUS_ENC_APPLICATION_AUDIO;
            enc_cfg.complexity            = 0;
            enc_cfg.enable_fec            = false;
            enc_cfg.enable_dtx            = true;
            enc_cfg.enable_vbr            = true;
            if (esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &encoder) != ESP_AUDIO_ERR_OK || encoder == nullptr) {
                ESP_LOGE(TAG, "failed to open opus encoder");
                release_session();
                self.client_fd_ = -1;
                self.session_++;
                continue;
            }
            encoder_guard.reset(encoder);

            int frame_bytes = 0;
            int out_bytes   = 0;
            esp_opus_enc_get_frame_size(encoder, &frame_bytes, &out_bytes);
            frame_samples = frame_bytes / sizeof(int16_t);  // 960 @ 60 ms

            // One 60 ms chunk at the codec rate, both interleaved slots.
            // 一个读取块 = 60ms 的 2-slot 交错全量样本
            const size_t in_chunk = in_rate_config / 1000 * kFrameMs * 2;
            in_buf.assign(in_chunk, 0);
            mic_buf.assign(in_chunk / 2, 0);  // de-interleaved mic channel
            rs_buf.assign(in_chunk, 0);
            pcm.clear();
            pcm.reserve(frame_samples * 2);
            enc_buf.assign(out_bytes, 0);

            if (!logged_session) {
                ESP_LOGI(TAG, "capture session ready (2-slot in %lu Hz, mic=ch0(even), resample %d, opus %d samples/frame)",
                         (unsigned long)in_rate_config, need_resample, (int)frame_samples);
                logged_session = true;
            }
        }

        // Blocks until the codec delivers a chunk (~60 ms) — natural pacing.
        if (!codec->InputData(in_buf)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!logged_first_read) {
            ESP_LOGI(TAG, "first audio chunk read ok (%d samples)", (int)in_buf.size());
            logged_first_read = true;
        }
        stat_reads++;
        stat_tick += kFrameMs;

        // De-interleave: 2-slot 交错 [麦克风(ch0), 回采参考(ch1)],取偶数位。
        // 2026-11-12 晚定案(真声判据):奇数槽在用户说话时峰值 0.002(死静音
        // =参考通道,只在喇叭播放时有电信号),偶数槽承载真人声(实测峰值
        // 0.4~0.76)。早先的 1kHz 分相位实验把参考通道误判为麦克风——因为
        // 测试音在播,参考通道电气直通自然最强。
        // Hardware input gain is ineffective on this codec stack (measured
        // peak ~241), so apply fixed digital gain (x16 = +24 dB) with
        // saturation.
        const size_t mic_samples = in_buf.size() / 2;
        for (size_t i = 0; i < mic_samples; i++) {
            int32_t v = (int32_t)in_buf[i * 2] * kMicDigitalGain;
            mic_buf[i] = (int16_t)std::min<int32_t>(32767, std::max<int32_t>(-32768, v));
        }

        if (need_resample) {
            // The last argument is in/out: pass the output buffer capacity,
            // the call replaces it with the number of samples produced.
            uint32_t produced = rs_buf.size();
            if (esp_ae_rate_cvt_process(resampler, (esp_ae_sample_t*)mic_buf.data(), mic_samples,
                                        (esp_ae_sample_t*)rs_buf.data(),
                                        &produced) != ESP_AE_ERR_OK) {
                if (++rs_err_count <= 3) {
                    ESP_LOGE(TAG, "resample failed, chunk dropped");
                }
                continue;
            }
            stat_produced += produced;
            pcm.insert(pcm.end(), rs_buf.begin(), rs_buf.begin() + produced);
        } else {
            pcm.insert(pcm.end(), mic_buf.begin(), mic_buf.end());
        }

        if (stat_tick >= 10000) {
            ESP_LOGI(TAG, "stats: reads=%u produced=%u frames=%u (2-slot odd: expect reads~167 produced~16000 frames~167 per 10s)",
                     stat_reads, stat_produced, stat_frames);
            stat_tick = 0; stat_reads = 0; stat_produced = 0; stat_frames = 0;
        }

        while (pcm.size() >= frame_samples) {
            esp_audio_enc_in_frame_t in = {
                .buffer = (uint8_t*)pcm.data(),
                .len    = (uint32_t)(frame_samples * sizeof(int16_t)),
            };
            esp_audio_enc_out_frame_t out = {
                .buffer        = enc_buf.data(),
                .len           = (uint32_t)enc_buf.size(),
                .encoded_bytes = 0,
            };
            if (esp_opus_enc_process(encoder, &in, &out) == ESP_AUDIO_ERR_OK && out.encoded_bytes > 0) {
                stat_frames++;
                if (!logged_first_enc) {
                    ESP_LOGI(TAG, "first opus packet encoded (%d bytes)", (int)out.encoded_bytes);
                    logged_first_enc = true;
                }
                // Copy the packet: the queued send outlives this iteration.
                auto packet = std::make_shared<std::vector<uint8_t>>(enc_buf.begin(),
                                                                     enc_buf.begin() + out.encoded_bytes);
                httpd_handle_t server = self.server_.load();
                if (server != nullptr) {
                    auto* job = new WsSendJob{server, fd, self.session_.load(), packet};
                    if (httpd_queue_work(server, ws_send_job, job) != ESP_OK) {
                        // Work queue full: client is too slow, drop the frame.
                        delete job;
                        if (++queue_full_count >= 10) {
                            // Persistently stalled: release the stream instead of
                            // spinning forever against a stuck client.
                            ESP_LOGW(TAG, "httpd queue kept full, releasing client");
                            self.ReleaseClient(fd, self.session_.load());
                        }
                    } else {
                        queue_full_count = 0;
                    }
                }
            }
            pcm.erase(pcm.begin(), pcm.begin() + frame_samples);
        }
    }

    release_session();
    ESP_LOGI(TAG, "capture task stopped");
}

bool MicWsStream::Start(httpd_handle_t server, mcp_lan::LanService& service) {
    if (server == nullptr) {
        return false;
    }
    mcp_service_ = &service;
    if (server_ != nullptr) {
        return true;  // already running
    }
    server_ = server;
    // SEP: the capture task is pre-spawned at service start (see
    // lan_service.cpp) and parks with no session while nobody streams, so
    // its 22 KB stack is the only cost of keeping it alive.
    httpd_uri_t ws_mic              = {};
    ws_mic.uri                      = "/mic";
    ws_mic.method                   = HTTP_GET;
    ws_mic.handler                  = WsHandler;
    ws_mic.is_websocket             = true;
    ws_mic.handle_ws_control_frames = false;
#if CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
    ws_mic.ws_pre_handshake_cb = WsPreHandshake;
#endif
    if (httpd_register_uri_handler(server, &ws_mic) != ESP_OK) {
        ESP_LOGE(TAG, "failed to register /mic ws handler");
        server_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "microphone stream ready at ws://<device-ip>:%d/mic", CONFIG_MCP_AGENT_PORT);
    return true;
}

void MicWsStream::Reclaim() {
    auto& self = GetInstance();
    if (self.capture_task_ != nullptr && self.task_exited_.load()) {
        vTaskDeleteWithCaps(self.capture_task_);
        self.capture_task_ = nullptr;
    }
    self.mcp_service_ = nullptr;
}

bool MicWsStream::EnsureCaptureTask() {
    // Construct the public codec before reserving the large task stack. Its
    // DMA allocation uses ESP_ERROR_CHECK; doing it afterwards can reset the
    // device instead of returning a stream-start error on a busy heap.
    if (mcp_dev::audio_codec("mic capture") == nullptr) {
        return false;
    }
    if (capture_task_ != nullptr) {
        if (!task_exited_.load()) {
            // SEP: the task is persistent — alive means parked waiting for a
            // client and ready to stream, not "still stopping".
            return true;
        }
        vTaskDeleteWithCaps(capture_task_);
        capture_task_ = nullptr;
    }
    running_     = true;
    task_exited_ = false;
    // 22 KB internal stack — 16 KB overflowed in both the original 60 ms
    // configuration and the tested 20 ms configuration (core dump confirmed).
    // WithCaps takes the depth in BYTES; the stack
    // must stay in INTERNAL RAM: flash writes (NVS etc.) briefly disable
    // the cache, and a task executing on a PSRAM stack hits
    // "assert failed: esp_task_stack_is_sane_cache_disabled" and reboots.
    if (xTaskCreatePinnedToCoreWithCaps(CaptureTask, "mic_ws", 2048 * 11, this, 4, &capture_task_, 1,
                                        MALLOC_CAP_INTERNAL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create capture task: free internal=%u, largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        capture_task_ = nullptr;
        running_      = false;
        return false;
    }
    return true;
}

void MicWsStream::Stop() {
    if (server_ == nullptr) {
        return;
    }
    session_++;
    client_fd_ = -1;
    // SEP 常驻任务:服务停止不杀任务(预建于 App 打开,内存最充裕时),
    // 只卸载服务端;进行中的会话由任务在空闲宽限后自行释放。
    server_ = nullptr;
    ESP_LOGI(TAG, "microphone stream stopped");
}
