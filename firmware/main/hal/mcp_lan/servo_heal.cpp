/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "servo_heal.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../drivers/FTServo_Arduino/src/SCSCL.h"

static const char* TAG = "ServoHeal";

namespace mcp_lan {

namespace {

// Same bus geometry as the official Hal::servo_init() (hal_servo.cpp).
constexpr uart_port_t kUartPort = UART_NUM_1;
constexpr int kBaud             = 1000000;
constexpr int kTxPin            = 6;
constexpr int kRxPin            = 7;

// Servo IDs and operating range from the official servo config.
constexpr uint8_t kServoIds[] = {1, 2};
constexpr uint16_t kLimitMin  = 0;
constexpr uint16_t kLimitMax  = 1000;

bool heal_one(SCSCL& bus, uint8_t id)
{
    int lo = bus.readRegister(id, SCSCL_MIN_ANGLE_LIMIT_L);
    int hi = bus.readRegister(id, SCSCL_MAX_ANGLE_LIMIT_L);
    if (lo < 0 || hi < 0) {
        ESP_LOGW(TAG, "servo %u not responding (lo=%d hi=%d), skipping", id, lo, hi);
        return false;
    }
    if (!(lo == 0 && hi == 0)) {
        ESP_LOGI(TAG, "servo %u angle limits ok: (%d,%d)", id, lo, hi);
        return true;
    }

    ESP_LOGW(TAG, "servo %u in MOTOR MODE (limits 0/0), restoring (%u,%u]", id, kLimitMin, kLimitMax);
    bus.unLockEprom(id);
    vTaskDelay(pdMS_TO_TICKS(20));
    bus.writeRegister(id, SCSCL_MIN_ANGLE_LIMIT_L, kLimitMin);
    vTaskDelay(pdMS_TO_TICKS(50));  // EPROM commit time between writes
    bus.writeRegister(id, SCSCL_MAX_ANGLE_LIMIT_L, kLimitMax);
    vTaskDelay(pdMS_TO_TICKS(50));
    bus.LockEprom(id);

    lo = bus.readRegister(id, SCSCL_MIN_ANGLE_LIMIT_L);
    hi = bus.readRegister(id, SCSCL_MAX_ANGLE_LIMIT_L);
    const bool ok = (lo == kLimitMin && hi == kLimitMax);
    if (ok) {
        ESP_LOGI(TAG, "servo %u healed, limits now (%d,%d)", id, lo, hi);
    } else {
        ESP_LOGE(TAG, "servo %u heal FAILED, limits read back (%d,%d)", id, lo, hi);
    }
    return ok;
}

}  // namespace

void heal_servos_before_hal_init()
{
    // Give the servos time to come out of their own power-on reset.
    vTaskDelay(pdMS_TO_TICKS(200));

    SCSCL bus;
    if (!bus.begin(kUartPort, kBaud, kTxPin, kRxPin)) {
        ESP_LOGE(TAG, "failed to open the servo bus for healing");
        return;
    }

    for (uint8_t id : kServoIds) {
        heal_one(bus, id);
    }

    // Hand the UART back so the official Hal::servo_init() can open it.
    bus.end();
}

}  // namespace mcp_lan
