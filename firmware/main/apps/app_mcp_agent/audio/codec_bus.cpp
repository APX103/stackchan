/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "codec_bus.h"

#include <mutex>

#include "../platform/device_access.h"

namespace mcp_audio {
namespace {
int output_users = 0;
}  // namespace

void acquire_output(AudioCodec* codec)
{
    std::lock_guard<std::mutex> lock(mcp_dev::codec_mutex());
    codec->EnableOutput(true);
    output_users++;
}

void release_output(AudioCodec* codec)
{
    std::lock_guard<std::mutex> lock(mcp_dev::codec_mutex());
    if (output_users <= 0) {
        return;
    }
    output_users--;
    if (output_users == 0) {
        codec->EnableOutput(false);
    }
}

}  // namespace mcp_audio
