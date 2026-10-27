/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace mcp_lan {

// Pre-HAL servo healing for the MCP Agent build.
//
// Feetech SCS servos whose angle-limit EPROM registers (min/max) are BOTH
// zero are in "motor mode" and silently ignore goal-position writes while
// torque and position reads keep working. The zeroing is reachable through
// the official driver's PWM-mode path (PWMMode() writes zeros), and an
// interrupted mode switch can leave a servo stuck there across reboots.
//
// Per the HAL-zero-modification policy this runs OUTSIDE the official HAL:
// it opens the servo bus with its own driver instance before Hal::init()
// takes it, restores the limits if needed, and closes the bus again.
// Call exactly once from app_main BEFORE GetHAL().init().
void heal_servos_before_hal_init();

}  // namespace mcp_lan
