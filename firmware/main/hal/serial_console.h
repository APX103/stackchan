/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <atomic>

namespace hal {

// Head-less operation over the USB console so the robot can be driven
// without touching the screen (PC-side helper: scripts/sercon.py).
// Navigation commands mimic human gestures: the launcher scrolls one icon
// per 'next'/'prev' and "taps" the centered icon on 'go'.
//
//   next | prev -> scroll launcher one icon
//   go         -> open the centered app (same path as a real tap)
//   page       -> launcher answers "PAGE: <id> <name>"
//   reason     -> last reset reason
//   reboot     -> plain restart
void start_serial_console();

// Launcher navigation request mailbox (consumed by AppLauncher).
// 0 = none, 1 = next, 2 = prev, 3 = go, 4 = report page.
inline std::atomic<int>& launcherNavRequest()
{
    static std::atomic<int> box{0};
    return box;
}

}  // namespace hal
