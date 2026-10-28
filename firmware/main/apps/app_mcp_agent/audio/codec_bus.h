/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

class AudioCodec;

namespace mcp_audio {

// The duplex I2S clock belongs to both MCP streams. Only the last stream to
// finish may disable it; an individual client disconnect must not affect the
// other stream.
void acquire_output(AudioCodec* codec);
void release_output(AudioCodec* codec);

}  // namespace mcp_audio
