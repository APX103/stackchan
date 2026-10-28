/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <esp_http_server.h>
#include <esp_log.h>

#include <exception>
#include <new>
#include <utility>

namespace mcp_lan {

inline esp_err_t SendHandlerError(httpd_req_t* req, bool out_of_memory) noexcept
{
    static const char kOom[] = "{\"error\":\"out of memory\"}";
    static const char kFailure[] = "{\"error\":\"internal error\"}";
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, out_of_memory ? kOom : kFailure, -1);
}

template <typename Handler>
esp_err_t GuardHttpHandler(httpd_req_t* req, Handler&& handler) noexcept
{
    try {
        return std::forward<Handler>(handler)();
    } catch (const std::bad_alloc&) {
        ESP_LOGE("McpHttp", "request aborted: out of memory");
        return SendHandlerError(req, true);
    } catch (const std::exception& e) {
        ESP_LOGE("McpHttp", "request aborted: %s", e.what());
        return SendHandlerError(req, false);
    } catch (...) {
        ESP_LOGE("McpHttp", "request aborted: unknown exception");
        return SendHandlerError(req, false);
    }
}

}  // namespace mcp_lan
