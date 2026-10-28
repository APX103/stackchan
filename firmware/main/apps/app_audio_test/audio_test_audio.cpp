/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "audio_test_audio.h"

#include "apps/common/audio/codec_access.h"

namespace audio_test {

AudioCodec* codec()
{
    return common_audio::codec("audio test");
}

std::mutex& codec_mutex()
{
    return common_audio::codec_mutex();
}

}  // namespace audio_test
