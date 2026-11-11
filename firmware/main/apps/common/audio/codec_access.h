/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <mutex>

#include "audio_codec.h"

namespace common_audio {

AudioCodec* codec(const char* consumer);
std::mutex& codec_mutex();

}  // namespace common_audio
