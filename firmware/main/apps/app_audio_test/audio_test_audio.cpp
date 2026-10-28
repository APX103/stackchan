/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "audio_test_audio.h"

#include <board.h>

namespace audio_test {

AudioCodec* codec()
{
    return Board::GetInstance().GetAudioCodec();
}

std::mutex& codec_mutex()
{
    static std::mutex mutex;
    return mutex;
}

}  // namespace audio_test
