/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <string>

// Diagnostics dump of both head servos (angles, runtime limits shrunk by the
// stall protection, torque state, raw positions). Implemented next to the
// servo driver where the concrete class is visible.
namespace hal_servo_debug {

std::string dump();

}  // namespace hal_servo_debug
