/*
 * SEP-0002: firmware automation interface
 */
#include "hal/sep/sep_automation.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "driver/usb_serial_jtag.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mooncake.h"
#include "settings.h"
#include "wifi_manager.h"

namespace sep {

static constexpr const char* kTag = "[sep]";
static QueueHandle_t s_launch_queue = nullptr;  // items: char[kNameMax]
static constexpr size_t kNameMax   = 48;

// ---------------------------------------------------------------- server URL

std::string serverUrlOverride()
{
    Settings settings("sep", true);
    return settings.GetString("server_url", "");
}

void setServerUrlOverride(const std::string& url)
{
    Settings settings("sep", true);  // read_write = true
    settings.SetString("server_url", url);
}

// ------------------------------------------------------------------ helpers

static std::string toLower(std::string s)
{
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static bool findAppByName(const std::string& name, int& outId, std::string& outName)
{
    const auto target = toLower(name);
    for (const auto& props : mooncake::GetMooncake().getAllAppProps()) {
        if (toLower(props.info.name) == target) {
            outId   = props.appID;
            outName = props.info.name;
            return true;
        }
    }
    return false;
}

static void queueLaunch(const std::string& name)
{
    if (name.empty() || name.size() >= kNameMax) {
        printf("%s ERROR bad app name\r\n", kTag);
        return;
    }
    static char item[kNameMax];
    strncpy(item, name.c_str(), kNameMax - 1);
    item[kNameMax - 1] = '\0';
    xQueueSend(s_launch_queue, item, 0);
}

// ------------------------------------------------------------ serial console

static void handleLine(const char* line)
{
    std::string cmd(line);
    while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == '\n' || cmd.back() == ' ')) cmd.pop_back();
    while (!cmd.empty() && cmd.front() == ' ') cmd.erase(cmd.begin());
    if (cmd.empty()) return;

    if (cmd == "apps") {
        printf("%s installed apps:", kTag);
        for (const auto& props : mooncake::GetMooncake().getAllAppProps())
            printf(" %s", props.info.name.c_str());
        printf("\r\n");
    } else if (cmd.rfind("launch ", 0) == 0) {
        queueLaunch(cmd.substr(7));
        printf("%s launch queued\r\n", kTag);
    } else if (cmd == "server?") {
        const auto url = serverUrlOverride();
        printf("%s server override: %s\r\n", kTag, url.empty() ? "<none, using build default>" : url.c_str());
    } else if (cmd.rfind("server ", 0) == 0) {
        const auto url = cmd.substr(7);
        setServerUrlOverride(url);
        printf("%s server override set: %s (takes effect on next WS service start)\r\n", kTag, url.c_str());
    } else if (cmd == "reset") {
        printf("%s rebooting\r\n", kTag);
        esp_restart();
    } else {
        printf("%s unknown command, try: apps | launch <NAME> | server <url> | server? | reset\r\n", kTag);
    }
}

static void serialTask(void*)
{
    // Coexist with the console: if the driver is already installed by the
    // secondary console, install fails and reads still work (IDF v6 API).
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        esp_err_t err                       = usb_serial_jtag_driver_install(&cfg);
        if (err != ESP_OK) {
            printf("%s usb_serial_jtag driver unavailable (%s), serial commands disabled\r\n", kTag,
                   esp_err_to_name(err));
            vTaskDelete(nullptr);
            return;
        }
    }

    static char line[128];
    size_t len = 0;
    printf("%s automation console ready (apps | launch <NAME> | server <url> | server? | reset)\r\n", kTag);
    while (true) {
        uint8_t byte = 0;
        const int n  = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(200));
        if (n <= 0) continue;
        if (byte == '\r' || byte == '\n') {
            line[len] = '\0';
            if (len > 0) handleLine(line);
            len = 0;
        } else if (len + 1 < sizeof(line)) {
            line[len++] = (char)byte;
        }
    }
}

// ------------------------------------------------------------------- HTTP

static esp_err_t handlerHealth(httpd_req_t* req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"service\":\"sep-automation\"}");
}

static esp_err_t handlerApps(httpd_req_t* req)
{
    std::string body = "{\"apps\":[";
    bool first       = true;
    for (const auto& props : mooncake::GetMooncake().getAllAppProps()) {
        if (!first) body += ",";
        first = false;
        body += "{\"id\":" + std::to_string(props.appID) + ",\"name\":\"" + props.info.name + "\"}";
    }
    body += "]}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body.c_str());
}

static esp_err_t handlerAppLaunch(httpd_req_t* req)
{
    char name[64] = {0};
    size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len > 1) {
        char query[128] = {0};
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
            httpd_query_key_value(query, "name", name, sizeof(name)) == ESP_OK) {
            queueLaunch(name);
            httpd_resp_set_status(req, "202 Accepted");
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, "{\"queued\":true}");
        }
    }
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "missing ?name=");
}

static esp_err_t handlerServerGet(httpd_req_t* req)
{
    const auto url = serverUrlOverride();
    std::string body = "{\"server\":\"" + (url.empty() ? std::string("") : url) + "\"}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body.c_str());
}

static esp_err_t handlerServerPut(httpd_req_t* req)
{
    char url[128] = {0};
    int remaining = req->content_len;
    if (remaining <= 0 || (size_t)remaining >= sizeof(url)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "body must be the url");
    }
    int ret = httpd_req_recv(req, url, remaining);
    if (ret <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "recv failed");
    }
    setServerUrlOverride(std::string(url, ret));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static void startHttpServer()
{
    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.server_port      = 80;
    config.ctrl_port        = 32768;
    httpd_handle_t server   = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        printf("%s http server start failed (port busy?), HTTP API disabled\r\n", kTag);
        return;
    }
    const httpd_uri_t health = {.uri = "/sep/health", .method = HTTP_GET, .handler = handlerHealth, .user_ctx = nullptr};
    const httpd_uri_t apps   = {.uri = "/sep/apps", .method = HTTP_GET, .handler = handlerApps, .user_ctx = nullptr};
    const httpd_uri_t launch = {.uri = "/sep/app", .method = HTTP_GET, .handler = handlerAppLaunch,
                               .user_ctx = nullptr};
    const httpd_uri_t srv_get = {.uri = "/sep/server", .method = HTTP_GET, .handler = handlerServerGet,
                                 .user_ctx = nullptr};
    const httpd_uri_t srv_put = {.uri = "/sep/server", .method = HTTP_PUT, .handler = handlerServerPut,
                                 .user_ctx = nullptr};
    httpd_register_uri_handler(server, &health);
    httpd_register_uri_handler(server, &apps);
    httpd_register_uri_handler(server, &launch);
    httpd_register_uri_handler(server, &srv_get);
    httpd_register_uri_handler(server, &srv_put);
    printf("%s http api ready on port 80 (/sep/health /sep/apps /sep/app?name= /sep/server)\r\n", kTag);
}

// -------------------------------------------------------------------- lifecycle

void init()
{
    printf("%s init (serial console + lazy httpd)\r\n", kTag);
    s_launch_queue = xQueueCreate(4, kNameMax);
    xTaskCreate(serialTask, "sep_serial", 4096, nullptr, 5, nullptr);
    // httpd is started lazily in poll() once WiFi/lwIP is actually up:
    // calling httpd_start() before esp_netif_init() trips the lwIP
    // "Invalid mbox" assert (found by bisect, 2026-11-11).
}

static bool s_http_started = false;

void poll()
{
    // Lazy httpd start: only after the network stack is live.
    if (!s_http_started && WifiManager::GetInstance().IsConnected()) {
        s_http_started = true;
        startHttpServer();
    }

    char name[kNameMax];
    while (xQueueReceive(s_launch_queue, name, 0) == pdTRUE) {
        int id          = -1;
        std::string hit;
        if (findAppByName(name, id, hit)) {
            printf("%s opening app '%s' (id %d)\r\n", kTag, hit.c_str(), id);
            mooncake::GetMooncake().openApp(id);
        } else {
            printf("%s app '%s' not found\r\n", kTag, name);
        }
    }
}

}  // namespace sep
