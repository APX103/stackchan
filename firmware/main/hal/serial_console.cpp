/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "serial_console.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_manager.h>

#include <stdio.h>
#include <string.h>

static const char* TAG = "SerCon";

static void console_task(void*)
{
    char line[64];
    while (true) {
        // fgets on the console VFS returns nullptr immediately when no line
        // is pending, so pace the loop.
        if (fgets(line, sizeof(line), stdin) == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (strncmp(line, "next", 4) == 0) {
            hal::launcherNavRequest().store(1);
        } else if (strncmp(line, "prev", 4) == 0) {
            hal::launcherNavRequest().store(2);
        } else if (strncmp(line, "go", 2) == 0) {
            hal::launcherNavRequest().store(3);
        } else if (strncmp(line, "page", 4) == 0) {
            hal::launcherNavRequest().store(4);
        } else if (strncmp(line, "wifi", 4) == 0) {
            auto& wifi = WifiManager::GetInstance();
            wifi_ps_type_t ps = WIFI_PS_NONE;
            esp_wifi_get_ps(&ps);
            printf("WIFI: connected=%d ip=%s ssid=%s rssi=%d ps=%d heap=%u\n",
                   (int)wifi.IsConnected(), wifi.GetIpAddress().c_str(), wifi.GetSsid().c_str(),
                   wifi.GetRssi(), (int)ps, (unsigned)esp_get_free_heap_size());
            fflush(stdout);
        } else if (strncmp(line, "reason", 6) == 0) {
            esp_reset_reason_t reason = esp_reset_reason();
            const char* name          = "UNKNOWN";
            switch (reason) {
                case ESP_RST_POWERON: name = "POWERON"; break;
                case ESP_RST_SW: name = "SW_RESTART"; break;
                case ESP_RST_PANIC: name = "PANIC"; break;
                case ESP_RST_INT_WDT: name = "INT_WDT"; break;
                case ESP_RST_TASK_WDT: name = "TASK_WDT"; break;
                case ESP_RST_WDT: name = "RTC_WDT"; break;
                case ESP_RST_BROWNOUT: name = "BROWNOUT"; break;
                default: break;
            }
            printf("last reset: %d (%s)\n", (int)reason, name);
            fflush(stdout);
        } else if (strncmp(line, "reboot", 6) == 0) {
            ESP_LOGW(TAG, "rebooting");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        } else if (strncmp(line, "help", 4) == 0) {
            printf("commands: next | prev | go | page | wifi | reason | reboot\n");
            fflush(stdout);
        }
    }
}

namespace hal {

void start_serial_console()
{
    xTaskCreate(console_task, "sercon", 4096, nullptr, 2, nullptr);
    ESP_LOGI(TAG, "serial console ready: next | prev | go | page | reboot");
}

}  // namespace hal
