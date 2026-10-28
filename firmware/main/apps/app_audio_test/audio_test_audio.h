/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <mutex>

#include "audio_codec.h"

namespace audio_test {

AudioCodec* codec();
std::mutex& codec_mutex();

}  // namespace audio_test
