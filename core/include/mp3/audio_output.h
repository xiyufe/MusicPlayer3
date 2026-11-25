// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 xiyufe
//
// MusicPlayer3 core -- audio output abstraction.
//
// The output is the boundary between "our clock" and "the hardware clock".
// Everything upstream (decoding, DSP) must follow the device, never the other
// way round: we pull frames on the device's callback, so a slow decoder shows
// up as an underrun, not as drift between two clocks.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mp3/audio_format.h"
#include "mp3/core.h"

namespace mp3 {

/// Which sharing mode to request from the OS.
enum class OutputMode : uint8_t {
    /// Mix through the OS mixer.  Always works, allows other apps to play at
    /// the same time, but the OS may resample and apply its own effects.
    ///
    /// The device is opened at the OS mix format, not at the source rate, so
    /// this mode can never be bit-perfect -- and reopening the stream because
    /// the next track has a different rate buys nothing, since the OS would
    /// resample it anyway.
    Shared = 0,

    /// Take exclusive control of the device.  WASAPI exclusive mode on
    /// Windows, AAudio MMAP exclusive on Android.  Fails when another
    /// application holds the device.
    ///
    /// Exclusive is not a promise of bit-perfect: the driver may still
    /// convert the format internally and the OS volume may still be in the
    /// path.
    Exclusive,

    /// Exclusive access *and* a promise that nothing between us and the DAC
    /// touches the samples: no software volume, no resampling, and the stream
    /// format must equal the device's native format exactly.
    ///
    /// This is a request, not a hint.  If the backend cannot honour it,
    /// open() MUST fail -- a silent fallback is worse than a refusal, because
    /// the user would believe they are hearing an untouched signal while the
    /// OS quietly resamples it.  On Android this maps to
    /// setPreferredMixerAttributes() with MIXER_BEHAVIOR_BIT_PERFECT, which
    /// is an optional device feature (Android 14+), so expect it to fail on
    /// most phones and only work with some USB DACs.
    BitPerfect,
};

/// Real-world description of an output device.
struct DeviceInfo {
    std::string id;            ///< opaque, stable identifier
    std::string name;          ///< human readable, from the OS
    bool is_default = false;
    int max_channels = 2;
    int min_sample_rate = 8000;
    int max_sample_rate = 192000;
    /// Device's own preferred format, when the backend knows it.
    AudioFormat preferred_format{};
};

/// Callback invoked by the backend when it needs the next block of audio.
///
/// Contract:
///   - Runs on a realtime-priority thread owned by the OS audio stack.
///   - MUST NOT allocate, lock, block, log, or throw.
///   - MUST fill exactly `frame_count` frames in the format passed to start().
///   - MUST return quickly.  On underrun it is expected to write silence and
///     report it through the stats, not to stall.
using RenderCallback =
    std::function<void(uint8_t* dst, int frame_count, const AudioFormat& format)>;

/// Statistics reported by the backend.  Cheap to query, safe from any thread.
struct OutputStats {
    uint64_t frames_rendered = 0;
    uint64_t underruns = 0;        ///< callbacks we could not fill in time
    uint64_t xruns = 0;            ///< driver-reported glitches
    int64_t latency_frames = 0;    ///< device buffer size in frames
    double clock_drift_ppm = 0.0;  ///< measured device vs. nominal rate
};

/// Abstract audio output device.
///
/// Threading: configure/start/stop are called from the control thread; the
/// render callback runs on the audio thread.  Implementations must tolerate
/// start()/stop() from the control thread concurrently with the callback.
class MP3_API AudioOutput {
public:
    virtual ~AudioOutput() = default;

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    /// Enumerate the devices the backend can see.
    /// Device hot-plug is reported through the device-changed hook below.
    virtual std::vector<DeviceInfo> devices() const = 0;

    /// Open a device.  Empty id means "system default".
    ///
    /// In `Shared` and `BitPerfect` modes the requested format is honoured
    /// exactly or the call fails; there is no silent resampling -- the mixer
    /// upstream is responsible for conforming.  `Exclusive` may fall back to
    /// the device's own mix format, because in that mode we ask for control,
    /// not for fidelity.
    ///
    /// Because `BitPerfect` cannot resample, a caller that wants every track
    /// at its native rate must reopen the stream whenever the rate changes.
    /// That is deliberate: the gap is short and a resampled track defeats the
    /// entire purpose of the mode.
    virtual Status open(const std::string& device_id,
                        const AudioFormat& format,
                        OutputMode mode) = 0;

    /// Undo open().  Stops playback first if running.
    virtual void close() = 0;

    /// True between a successful open() and the following close().
    [[nodiscard]] virtual bool is_open() const noexcept = 0;

    /// Install the render callback.  Must be called before start().
    virtual Status set_render_callback(RenderCallback callback) = 0;

    /// Begin pulling frames from the callback.
    virtual Status start() = 0;

    /// Stop pulling frames.  The callback is guaranteed not to be running when
    /// this returns.  Safe to call when already stopped.
    virtual void stop() = 0;

    /// Discard everything currently queued in the device buffer.
    /// Used when seeking so stale audio is not heard.
    virtual Status flush() = 0;

    /// The format the device actually accepted (may differ from the request
    /// only in `Shared` mode, where the OS may impose its own mix format).
    [[nodiscard]] virtual const AudioFormat& format() const noexcept = 0;

    /// Buffer size the backend chose, in frames.  Lower means less latency but
    /// a higher risk of underruns.
    [[nodiscard]] virtual int buffer_frames() const noexcept = 0;

    [[nodiscard]] virtual OutputStats stats() const noexcept = 0;

    /// Set once after construction; invoked on device hot-plug or when the
    /// device is taken away by another application.
    virtual void set_device_changed_callback(std::function<void()> callback) = 0;

protected:
    AudioOutput() = default;
};

/// Backend selection.  Both are compiled in when the platform supports them.
enum class OutputBackend : uint8_t {
    Auto = 0,
    Null,      ///< silent backend, renders at wall-clock speed; for tests
    Wasapi,    ///< Windows (shared + exclusive)
    AAudio,    ///< Android 8.0+
    OpenSL,    ///< Android legacy fallback
};

/// Create the platform's default output backend.
MP3_API std::unique_ptr<AudioOutput> create_audio_output(OutputBackend backend,
                                                         Status& status);

}  // namespace mp3
