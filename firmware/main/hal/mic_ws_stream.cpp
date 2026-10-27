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

#include <memory>
#include <string>
#include <vector>
#include <cstdlib>
#include <algorithm>

#include "mcp_lan/device_access.h"
#include "mcp_lan/lan_service.h"
#include "esp_opus_enc.h"
#include "esp_ae_rate_cvt.h"
#include "ws_util.h"

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
    std::shared_ptr<std::vector<uint8_t>> packet;
};

void ws_send_job(void* arg) {
    auto* job = static_cast<WsSendJob*>(arg);
    httpd_ws_frame_t frame = {};
    frame.type             = HTTPD_WS_TYPE_BINARY;
    frame.payload          = job->packet->data();
    frame.len              = job->packet->size();
    if (httpd_ws_send_frame_async(job->server, job->fd, &frame) != ESP_OK) {
        // Session is gone (client disconnected): free the stream so the
        // capture task stops feeding a dead socket.
        MicWsStream::ReleaseClient(job->fd);
        mcp_lan::service_push_event("mic: client dropped");
    }
    delete job;
}
}  // namespace

void MicWsStream::ReleaseClient(int fd) {
    auto& self = GetInstance();
    int current = self.client_fd_.load();
    if (current == fd) {
        self.client_fd_.compare_exchange_strong(current, -1);
        ESP_LOGI(TAG, "client fd %d released", fd);
    }
}

esp_err_t MicWsStream::WsHandler(httpd_req_t* req) {
    auto& self = GetInstance();

    // NOTE: no method check here. This handler is invoked once per incoming
    // WebSocket frame (the HTTP GET handshake never reaches it), and in that
    // context req->method is not a reliable HTTP_GET — returning an error
    // would write a plain HTTP response into the stream and corrupt the
    // protocol.

    if (!mcp_lan::service_ws_authorized(req)) {
        wsutil::SendError(req, "unauthorized");
        return ESP_FAIL;  // close the session
    }

    httpd_ws_frame_t pkt;
    std::vector<uint8_t> payload;
    esp_err_t ret = wsutil::RecvFrame(req, &pkt, &payload);
    if (ret == ESP_ERR_INVALID_SIZE) {
        // Larger than the drain limit: the payload cannot be skipped, so
        // close rather than desynchronize every following frame.
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
            ESP_LOGI(TAG, "client fd %d started streaming", fd);
            mcp_lan::service_push_event("mic: client started");
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
        // Already serving a client: reject politely.
        return wsutil::SendError(req, "busy");
    }

    // stop: release the stream, capture task goes idle
    if (strstr(msg, "\"stop\"") != nullptr) {
        int current = self.client_fd_.load();
        if (current == fd) {
            self.client_fd_.compare_exchange_strong(current, -1);
            mcp_lan::service_push_event("mic: client stopped");
        }
        return ESP_OK;
    }

    return wsutil::SendError(req, "unsupported message (expected start/stop)");
}

void MicWsStream::CaptureTask(void* arg) {
    auto& self = *static_cast<MicWsStream*>(arg);

    auto* codec = mcp_dev::audio_codec();
    if (codec == nullptr) {
        ESP_LOGE(TAG, "board has no audio codec, mic stream disabled");
        self.task_exited_ = true;
        vTaskDelay(portMAX_DELAY);  // WithCaps tasks must not self-delete
        return;
    }
    codec->EnableInput(true);
    // The duplex I2S bus is clocked from the output side — without the
    // output device open the ES7210 never gets a BCLK and reads back all
    // zeroes. Nothing is played here, this only starts the clocks.
    codec->EnableOutput(true);
    codec->Start();

    // The I2S stream is interleaved stereo: slot 0 = microphone, slot 1 =
    // playback reference (measured: [mic, -2, mic, -2...]). Effective mic
    // rate is the configured 24 kHz (720 mic samples arrive every 30 ms
    // inside 1440 interleaved samples).
    const uint32_t in_rate_config = codec->input_sample_rate();
    const bool need_resample      = in_rate_config != kOpusSampleRate;
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    if (need_resample) {
        esp_ae_rate_cvt_cfg_t rs_cfg  = {};
        rs_cfg.src_rate               = in_rate_config;
        rs_cfg.dest_rate              = kOpusSampleRate;
        rs_cfg.channel                = 1;
        rs_cfg.bits_per_sample        = ESP_AUDIO_BIT16;
        rs_cfg.complexity             = 2;
        rs_cfg.perf_type              = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
        if (esp_ae_rate_cvt_open(&rs_cfg, &resampler) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "failed to open resampler %lu->%d", in_rate_config, kOpusSampleRate);
            self.task_exited_ = true;
            vTaskDelay(portMAX_DELAY);  // WithCaps tasks must not self-delete
            return;
        }
    }

    // Opus encoder, same profile the cloud agent's audio service encodes
    // with: 16k mono, 60 ms frames, complexity 0, DTX+VBR.
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
    void* encoder = nullptr;
    if (esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &encoder) != ESP_AUDIO_ERR_OK || encoder == nullptr) {
        ESP_LOGE(TAG, "failed to open opus encoder");
        if (resampler != nullptr) {
            esp_ae_rate_cvt_close(resampler);
        }
        self.task_exited_ = true;
        vTaskDelay(portMAX_DELAY);  // WithCaps tasks must not self-delete
        return;
    }
    int frame_bytes = 0;
    int out_bytes   = 0;
    esp_opus_enc_get_frame_size(encoder, &frame_bytes, &out_bytes);
    const size_t frame_samples = frame_bytes / sizeof(int16_t);  // 960 @ 60 ms

    // 60 ms capture chunk at the codec rate, resampled output, and a small
    // accumulator so resampler jitter never breaks frame alignment.
    const size_t in_chunk     = in_rate_config / 1000 * kFrameMs;  // 1440 interleaved samples per read
    std::vector<int16_t> in_buf(in_chunk);
    std::vector<int16_t> mic_buf(in_chunk / 2);  // de-interleaved mic channel
    std::vector<int16_t> rs_buf(in_chunk);
    std::vector<int16_t> pcm;
    pcm.reserve(frame_samples * 2);
    std::vector<uint8_t> enc_buf(out_bytes);

    ESP_LOGI(TAG, "capture task ready (in %lu Hz interleaved stereo, resample %d, opus %d samples/frame)",
             in_rate_config, need_resample, frame_samples);

    // One-shot progress logs: enough to tell where the pipeline stalls
    // without flooding the console.
    bool logged_first_read = false;
    bool logged_first_enc  = false;
    int rs_err_count       = 0;
    int queue_full_count   = 0;
    // 10s statistics to diagnose rate mismatches
    uint32_t stat_tick     = 0;
    uint32_t stat_reads    = 0;
    uint32_t stat_produced = 0;
    uint32_t stat_frames   = 0;

    while (self.running_) {
        const int fd = self.client_fd_;
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));  // idle, no client
            continue;
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

        // De-interleave: keep slot 0 (mic), drop slot 1 (playback
        // reference, a constant DC when idle). The board's hardware input
        // gain path is ineffective on this codec stack (measured peak
        // ~241), so apply a fixed digital gain (x16 = +24 dB) with
        // saturation to reach usable levels.
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
            pcm.insert(pcm.end(), in_buf.begin(), in_buf.end());
        }

        if (stat_tick >= 10000) {
            ESP_LOGI(TAG, "stats: reads=%u produced=%u frames=%u (expect reads~167 produced~16000 frames~167 per 10s)",
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
                auto* job   = new WsSendJob{self.server_, fd, packet};
                if (httpd_queue_work(self.server_, ws_send_job, job) != ESP_OK) {
                    // Work queue full: client is too slow, drop the frame.
                    delete job;
                    if (++queue_full_count >= 10) {
                        // Persistently stalled: release the stream instead of
                        // spinning forever against a stuck client.
                        ESP_LOGW(TAG, "httpd queue kept full, releasing client");
                        self.ReleaseClient(fd);
                    }
                } else {
                    queue_full_count = 0;
                }
            }
            pcm.erase(pcm.begin(), pcm.begin() + frame_samples);
        }
    }

    esp_opus_enc_close(encoder);
    if (resampler != nullptr) {
        esp_ae_rate_cvt_close(resampler);
    }
    codec->EnableInput(false);
    ESP_LOGI(TAG, "capture task stopped");
    self.task_exited_ = true;
    // WithCaps tasks must NOT self-delete: park here, the owner deletes us
    // via vTaskDeleteWithCaps once it sees task_exited_.
    vTaskDelay(portMAX_DELAY);
}

bool MicWsStream::Start(httpd_handle_t server) {
    if (server == nullptr) {
        return false;
    }
    if (server_ != nullptr) {
        return true;  // already running
    }
    server_       = server;
    running_      = true;
    task_exited_  = false;
    // 24 KB stack in PSRAM. NOTE: the WithCaps variants take the depth in
    // BYTES (the classic xTaskCreate takes words — passing 24 K there meant
    // 96 KB of internal RAM per task, which is what starved the system).
    if (xTaskCreatePinnedToCoreWithCaps(CaptureTask, "mic_ws", 2048 * 12, this, 4, &capture_task_, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "failed to create capture task");
        running_ = false;
        server_  = nullptr;
        return false;
    }

    httpd_uri_t ws_mic              = {};
    ws_mic.uri                      = "/mic";
    ws_mic.method                   = HTTP_GET;
    ws_mic.handler                  = WsHandler;
    ws_mic.is_websocket             = true;
    ws_mic.handle_ws_control_frames = false;
    if (httpd_register_uri_handler(server_, &ws_mic) != ESP_OK) {
        ESP_LOGE(TAG, "failed to register /mic ws handler");
        Stop();
        return false;
    }
    ESP_LOGI(TAG, "microphone stream ready at ws://<device-ip>:%d/mic", CONFIG_MCP_AGENT_PORT);
    return true;
}

void MicWsStream::Stop() {
    if (server_ == nullptr) {
        return;
    }
    client_fd_ = -1;
    httpd_unregister_uri(server_, "/mic");
    running_ = false;
    if (capture_task_ != nullptr) {
        // The task parks itself (vTaskDelay(portMAX_DELAY)) after setting
        // task_exited_; once parked it is safe for this task to free it.
        for (int i = 0; i < 15 && !task_exited_.load(); i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!task_exited_.load()) {
            ESP_LOGW(TAG, "capture task did not exit in time, force-deleting (encoder leaked)");
        }
        vTaskDeleteWithCaps(capture_task_);
        capture_task_ = nullptr;
    }
    server_ = nullptr;
    ESP_LOGI(TAG, "microphone stream stopped");
}
