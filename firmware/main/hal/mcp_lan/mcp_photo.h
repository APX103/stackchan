/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mcp_photo {

// Captures one frame (lean path: no shutter sound, no on-screen preview)
// and stores it as JPEG in PSRAM. Runs on the tool executor task.
// `quality` 1-100. Returns false with `err` filled on failure.
bool capture(int quality, std::string* err);

// Snapshot access for the GET /photo.jpg handler (valid until the next
// capture; caller must not free).
bool get(const uint8_t** data, size_t* len);

// Last capture metadata (0 / false when no photo yet).
int width();
int height();
int quality();
uint32_t count();

// Base64 of the stored JPEG (for the take_photo embed=true mode).
std::string base64(std::string* err);

}  // namespace mcp_photo
