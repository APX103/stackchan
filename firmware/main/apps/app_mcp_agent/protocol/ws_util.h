/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <esp_http_server.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// Small helpers shared by the /mic and /speaker WebSocket endpoints.
//
// The historical /mic bug this header exists for: httpd_ws_recv_frame with a
// fixed small buffer returns ESP_ERR_INVALID_SIZE for larger frames WITHOUT
// consuming the payload, and a handler error closes the session — so any
// "play"-style message with a payload (or any binary frame) killed the
// connection (client saw an abrupt 1006). The receive path below always
// drains complete frames up to kMaxFrameBytes so protocol mistakes can be
// answered with a JSON error instead of a disconnect.
namespace wsutil {

// Largest client frame we are willing to receive and drain inside one handler
// call. A larger frame cannot be skipped (httpd cannot partially consume a
// frame), so the caller reports the error and closes the session instead of
// desynchronizing the stream for every following frame.
constexpr size_t kMaxFrameBytes = 4096;

inline esp_err_t SendText(httpd_req_t* req, const char* payload) {
    httpd_ws_frame_t frame = {};
    frame.type    = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t*)payload;
    frame.len     = strlen(payload);
    return httpd_ws_send_frame(req, &frame);
}

// `message` must not need JSON escaping (caller-controlled literals).
inline esp_err_t SendError(httpd_req_t* req, const char* message) {
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"type\":\"error\",\"message\":\"%s\"}", message);
    return SendText(req, buf);
}

// Receives one complete WebSocket frame into `payload` (sized to the frame).
//
// Returns:
//   ESP_OK               - frame received and fully drained; `pkt->type` and
//                          `payload` are valid (payload may be empty).
//   ESP_ERR_INVALID_SIZE - frame larger than kMaxFrameBytes; payload NOT
//                          drained — caller must report and close.
//   other                - transport failure, caller should propagate so
//                          httpd tears the session down.
inline esp_err_t RecvFrame(httpd_req_t* req, httpd_ws_frame_t* pkt, std::vector<uint8_t>* payload) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->type = HTTPD_WS_TYPE_TEXT;
    // First call with max_len 0: parses only the frame header and reports the
    // length (payload stays in the socket buffer).
    esp_err_t ret = httpd_ws_recv_frame(req, pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    if (pkt->len > kMaxFrameBytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (pkt->len == 0) {
        payload->clear();
        return ESP_OK;
    }
    payload->assign(pkt->len, 0);
    pkt->payload = payload->data();
    // Second call: frame->len is already known, so only the payload is read.
    return httpd_ws_recv_frame(req, pkt, pkt->len);
}

}  // namespace wsutil
