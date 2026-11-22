// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 xiyufe
//
// Audio output backend dispatch.
//
// Only the Null backend is implemented at this stage.  It is not a stub: it
// renders at wall-clock speed on its own thread, honours the render callback
// contract, and reports underruns.  That makes the whole pipeline (decoder ->
// mixer -> DSP -> output) runnable and testable before WASAPI or AAudio exist,
// including on a CI machine with no sound card.

#include "mp3/audio_output.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace mp3 {

namespace {

// ---------------------------------------------------------------------------
// Null backend
// ---------------------------------------------------------------------------

class NullOutput final : public AudioOutput {
public:
    NullOutput() = default;
    ~NullOutput() override { NullOutput::close(); }

    std::vector<DeviceInfo> devices() const override
    {
        DeviceInfo d;
        d.id = "null";
        d.name = "Null Output (silent)";
        d.is_default = true;
        d.max_channels = 8;
        d.min_sample_rate = 8000;
        d.max_sample_rate = 384000;
        d.preferred_format = AudioFormat{48000, ChannelLayout::Stereo,
                                         SampleFormat::F32};
        return {d};
    }

    Status open(const std::string& device_id,
                const AudioFormat& format,
                OutputMode mode) override
    {
        (void)mode;
        if (!device_id.empty() && device_id != "null") {
            return make_error(ErrorCode::NotFound,
                              "unknown null-output device: " + device_id);
        }
        if (!format.valid()) {
            return make_error(ErrorCode::InvalidArgument,
                              "invalid audio format requested");
        }
        if (is_open_) {
            return make_error(ErrorCode::InvalidState, "device already open");
        }

        format_ = format;
        // 10 ms of audio, clamped to something sane.  Matches what a real
        // low-latency backend would pick so tests see realistic block sizes.
        buffer_frames_ = std::clamp(format.sample_rate / 100, 64, 8192);
        buffer_.assign(
            static_cast<size_t>(buffer_frames_) *
                static_cast<size_t>(format.bytes_per_frame()),
            0);
        is_open_ = true;
        return Status::success();
    }

    void close() override
    {
        stop();
        is_open_ = false;
        format_ = AudioFormat{};
        buffer_frames_ = 0;
        buffer_.clear();
        render_ = nullptr;
    }

    [[nodiscard]] bool is_open() const noexcept override { return is_open_; }

    Status set_render_callback(RenderCallback callback) override
    {
        if (callback == nullptr) {
            return make_error(ErrorCode::InvalidArgument,
                              "render callback must not be empty");
        }
        if (is_running_.load(std::memory_order_acquire)) {
            return make_error(ErrorCode::InvalidState,
                              "cannot change callback while running");
        }
        render_ = std::move(callback);
        return Status::success();
    }

    Status start() override
    {
        if (!is_open_) {
            return make_error(ErrorCode::InvalidState, "device is not open");
        }
        if (render_ == nullptr) {
            return make_error(ErrorCode::InvalidState,
                              "no render callback installed");
        }
        if (is_running_.exchange(true, std::memory_order_acq_rel)) {
            return Status::success();  // already running
        }

        frames_rendered_.store(0, std::memory_order_relaxed);
        underruns_.store(0, std::memory_order_relaxed);
        thread_ = std::thread(&NullOutput::thread_main, this);
        return Status::success();
    }

    void stop() override
    {
        if (!is_running_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        if (thread_.joinable()) thread_.join();
    }

    Status flush() override
    {
        // Nothing is queued anywhere: the callback renders straight into a
        // throwaway buffer at wall-clock pace.
        return Status::success();
    }

    [[nodiscard]] const AudioFormat& format() const noexcept override
    {
        return format_;
    }

    [[nodiscard]] int buffer_frames() const noexcept override
    {
        return buffer_frames_;
    }

    [[nodiscard]] OutputStats stats() const noexcept override
    {
        OutputStats s;
        s.frames_rendered = frames_rendered_.load(std::memory_order_relaxed);
        s.underruns = underruns_.load(std::memory_order_relaxed);
        s.xruns = 0;
        s.latency_frames = buffer_frames_;
        s.clock_drift_ppm = 0.0;  // we are the clock; no drift by definition
        return s;
    }

    void set_device_changed_callback(std::function<void()> callback) override
    {
        device_changed_ = std::move(callback);
    }

private:
    void thread_main()
    {
        using clock = std::chrono::steady_clock;

        const int rate = format_.sample_rate;
        const auto start_time = clock::now();
        int64_t frames_done = 0;

        while (is_running_.load(std::memory_order_acquire)) {
            // Reset the scratch buffer so a callback that only writes part of
            // it still produces defined output.
            std::fill(buffer_.begin(), buffer_.end(), uint8_t{0});

            render_(buffer_.data(), buffer_frames_, format_);

            frames_done += buffer_frames_;
            frames_rendered_.store(static_cast<uint64_t>(frames_done),
                                   std::memory_order_relaxed);

            // Sleep until we should have finished this block; that is the
            // definition of "real time" for a silent backend.
            const auto target =
                start_time + std::chrono::nanoseconds(frames_to_ns(frames_done, rate));
            std::this_thread::sleep_until(target);
        }
    }

    AudioFormat format_{};
    int buffer_frames_ = 0;
    std::vector<uint8_t> buffer_;
    RenderCallback render_;
    std::function<void()> device_changed_;
    std::thread thread_;
    std::atomic<bool> is_running_{false};
    std::atomic<uint64_t> frames_rendered_{0};
    std::atomic<uint64_t> underruns_{0};
    bool is_open_ = false;
};

}  // namespace

std::unique_ptr<AudioOutput> create_audio_output(OutputBackend backend,
                                                 Status& status)
{
    switch (backend) {
        case OutputBackend::Null:
            status = Status::success();
            return std::make_unique<NullOutput>();

        case OutputBackend::Auto:
            // TODO(platform): try WASAPI on Windows, AAudio then OpenSL on
            // Android, and fall back to null so the app still starts.
            status = make_error(ErrorCode::NotSupported,
                                "no real audio backend is compiled in yet");
            return nullptr;

        case OutputBackend::Wasapi:
        case OutputBackend::AAudio:
        case OutputBackend::OpenSL:
            status = make_error(ErrorCode::NotSupported,
                                "this backend is not compiled in yet");
            return nullptr;
    }

    status = make_error(ErrorCode::InvalidArgument, "unknown output backend");
    return nullptr;
}

}  // namespace mp3
