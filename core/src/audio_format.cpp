// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 xiyufe

#include "mp3/audio_format.h"

namespace mp3 {

std::string_view to_string(SampleFormat fmt) noexcept
{
    switch (fmt) {
        case SampleFormat::U8:   return "U8";
        case SampleFormat::S16:  return "S16";
        case SampleFormat::S32:  return "S32";
        case SampleFormat::F32:  return "F32";
        case SampleFormat::S16P: return "S16P";
        case SampleFormat::S32P: return "S32P";
        case SampleFormat::F32P: return "F32P";
        default:                 return "Unknown";
    }
}

std::string_view to_string(ChannelLayout layout) noexcept
{
    switch (layout) {
        case ChannelLayout::Mono:       return "Mono";
        case ChannelLayout::Stereo:     return "Stereo";
        case ChannelLayout::Surround30: return "3.0";
        case ChannelLayout::Quad:       return "4.0";
        case ChannelLayout::Surround50: return "5.0";
        case ChannelLayout::Surround51: return "5.1";
        case ChannelLayout::Surround71: return "7.1";
        default:                        return "Unknown";
    }
}

}  // namespace mp3
