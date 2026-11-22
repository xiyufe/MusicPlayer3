// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 xiyufe
//
// MusicPlayer3 core -- audio format primitives.
//
// Everything the pipeline needs to describe "what kind of samples are these".
// Deliberately plain data: trivially copyable, cheap to pass across threads.

#pragma once

#include <cstdint>
#include <string_view>

#include "mp3/core.h"

namespace mp3 {

/// Sample layout in memory.
enum class SampleFormat : uint8_t {
    Unknown = 0,

    U8,      ///< unsigned 8-bit, centred on 128
    S16,     ///< signed 16-bit, the most common interchange format
    S32,     ///< signed 32-bit integer
    F32,     ///< 32-bit float

    /// Planar variants keep one contiguous block per channel.  FFmpeg produces
    /// these natively, and DSP code often prefers them.
    S16P,
    S32P,
    F32P,

    Count_
};

/// Number of channels.  Channels are interleaved in rank order:
/// [FL, FR], [FL, FR, FC], [FL, FR, FC, LFE], ...
enum class ChannelLayout : uint8_t {
    Unknown = 0,

    Mono,        ///< 1
    Stereo,      ///< 2
    Surround30,  ///< 3.0  FL FR FC
    Quad,        ///< 4.0  FL FR BL BR
    Surround50,  ///< 5.0  FL FR FC BL BR
    Surround51,  ///< 5.1  FL FR FC LFE BL BR
    Surround71,  ///< 7.1  FL FR FC LFE BL BR SL SR

    Count_
};

/// Bytes occupied by a single sample of the given format.
/// Returns 0 for SampleFormat::Unknown.
constexpr int bytes_per_sample(SampleFormat fmt) noexcept
{
    switch (fmt) {
        case SampleFormat::U8:   return 1;
        case SampleFormat::S16:  return 2;
        case SampleFormat::S32:  return 4;
        case SampleFormat::F32:  return 4;
        case SampleFormat::S16P: return 2;
        case SampleFormat::S32P: return 4;
        case SampleFormat::F32P: return 4;
        default:                 return 0;
    }
}

/// True when the channels live in separate planes.
constexpr bool is_planar(SampleFormat fmt) noexcept
{
    return fmt == SampleFormat::S16P || fmt == SampleFormat::S32P ||
           fmt == SampleFormat::F32P;
}

/// True when samples are IEEE floats rather than integers.
constexpr bool is_float(SampleFormat fmt) noexcept
{
    return fmt == SampleFormat::F32 || fmt == SampleFormat::F32P;
}

/// Number of channels implied by a layout.
constexpr int channel_count(ChannelLayout layout) noexcept
{
    switch (layout) {
        case ChannelLayout::Mono:       return 1;
        case ChannelLayout::Stereo:     return 2;
        case ChannelLayout::Surround30: return 3;
        case ChannelLayout::Quad:       return 4;
        case ChannelLayout::Surround50: return 5;
        case ChannelLayout::Surround51: return 6;
        case ChannelLayout::Surround71: return 8;
        default:                        return 0;
    }
}

/// Stable, human readable name (never localised).
std::string_view to_string(SampleFormat fmt) noexcept;
std::string_view to_string(ChannelLayout layout) noexcept;

/// Complete description of a stream's sample geometry.
/// Trivially copyable on purpose: it is captured by value in many places.
struct AudioFormat {
    int sample_rate = 44100;                     ///< Hz
    ChannelLayout channels = ChannelLayout::Stereo;
    SampleFormat sample_format = SampleFormat::F32;

    [[nodiscard]] int channel_count() const noexcept
    {
        return mp3::channel_count(channels);
    }

    [[nodiscard]] int bytes_per_sample() const noexcept
    {
        return mp3::bytes_per_sample(sample_format);
    }

    /// Size of one interleaved frame across all channels.
    [[nodiscard]] int bytes_per_frame() const noexcept
    {
        return bytes_per_sample() * channel_count();
    }

    [[nodiscard]] int bits_per_sample() const noexcept
    {
        return bytes_per_sample() * 8;
    }

    /// Bytes per second across all channels, i.e. the raw bitrate.
    [[nodiscard]] int64_t bytes_per_second() const noexcept
    {
        return static_cast<int64_t>(sample_rate) * bytes_per_frame();
    }

    /// True when the format is usable by the pipeline.
    [[nodiscard]] bool valid() const noexcept
    {
        return sample_rate > 0 && channel_count() > 0 && bytes_per_sample() > 0;
    }

    friend bool operator==(const AudioFormat& a, const AudioFormat& b) noexcept
    {
        return a.sample_rate == b.sample_rate && a.channels == b.channels &&
               a.sample_format == b.sample_format;
    }

    friend bool operator!=(const AudioFormat& a, const AudioFormat& b) noexcept
    {
        return !(a == b);
    }
};

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

/// Frame counts are the canonical time unit inside the engine: they are exact,
/// they do not drift, and converting to seconds is a plain division.
using FrameCount = int64_t;

/// Convert a frame count to nanoseconds at the given sample rate.
constexpr int64_t frames_to_ns(FrameCount frames, int sample_rate) noexcept
{
    if (sample_rate <= 0) return 0;
    return frames * 1'000'000'000LL / sample_rate;
}

/// Convert nanoseconds to a frame count at the given sample rate.
constexpr FrameCount ns_to_frames(int64_t ns, int sample_rate) noexcept
{
    if (sample_rate <= 0) return 0;
    return ns * sample_rate / 1'000'000'000LL;
}

/// Convert a frame count to seconds.
constexpr double frames_to_seconds(FrameCount frames, int sample_rate) noexcept
{
    if (sample_rate <= 0) return 0.0;
    return static_cast<double>(frames) / static_cast<double>(sample_rate);
}

}  // namespace mp3
