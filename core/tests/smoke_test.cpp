// SPDX-License-Identifier: GPL-3.0-or-later
// Smoke test for the core skeleton: exercises the parts that work before any
// real backend exists (Status, AudioFormat, extension registry, NullOutput).
//
// Built and run by hand for now; becomes a ctest target once CMake is in.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "mp3/audio_format.h"
#include "mp3/audio_output.h"
#include "mp3/core.h"
#include "mp3/decoder.h"

namespace {

int g_failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::printf("  FAIL  %s\n", what);
        ++g_failures;
    } else {
        std::printf("  ok    %s\n", what);
    }
}

void test_version()
{
    std::printf("[version]\n");
    check(mp3::version_string() == "0.1.0", "version string is 0.1.0");
    check(mp3::to_string(mp3::ErrorCode::DecodeError) == "DecodeError",
          "error code names are stable");
}

void test_status()
{
    std::printf("[status]\n");
    auto good = mp3::Status::success();
    check(good.ok(), "success() is ok");
    check(static_cast<bool>(good), "success() converts to true");

    auto bad = mp3::make_error(mp3::ErrorCode::IoError, "disk on fire");
    check(!bad.ok(), "make_error() is not ok");
    check(bad.code() == mp3::ErrorCode::IoError, "error code preserved");
    check(bad.message() == "disk on fire", "error message preserved");
    check(bad.describe() == "IoError: disk on fire", "describe() composes");
}

void test_audio_format()
{
    std::printf("[audio_format]\n");
    mp3::AudioFormat f;
    f.sample_rate = 48000;
    f.channels = mp3::ChannelLayout::Stereo;
    f.sample_format = mp3::SampleFormat::F32;

    check(f.valid(), "48k/2ch/f32 is valid");
    check(f.channel_count() == 2, "channel_count is 2");
    check(f.bytes_per_sample() == 4, "bytes_per_sample is 4");
    check(f.bytes_per_frame() == 8, "bytes_per_frame is 8");
    check(f.bits_per_sample() == 32, "bits_per_sample is 32");
    check(f.bytes_per_second() == 384000, "bytes_per_second is 384000");

    mp3::AudioFormat bad;
    bad.sample_rate = 0;
    check(!bad.valid(), "zero sample rate is invalid");

    check(mp3::frames_to_ns(48000, 48000) == 1'000'000'000,
          "48000 frames at 48k is one second");
    check(mp3::ns_to_frames(1'000'000'000, 48000) == 48000,
          "one second at 48k is 48000 frames");
    check(std::fabs(mp3::frames_to_seconds(24000, 48000) - 0.5) < 1e-9,
          "24000 frames at 48k is 0.5 s");
}

void test_extensions()
{
    std::printf("[extensions]\n");
    const auto exts = mp3::supported_extensions();
    check(!exts.empty(), "extension list is not empty");
    check(mp3::is_extension_supported("flac"), "flac supported");
    check(mp3::is_extension_supported("FLAC"), "FLAC (upper) supported");
    check(mp3::is_extension_supported(".mp3"), "leading dot tolerated");
    check(!mp3::is_extension_supported("doc"), "doc not supported");
    check(!mp3::is_extension_supported(""), "empty extension not supported");
    check(!mp3::is_extension_supported("flacc"), "longer name not supported");
}

void test_decoder_stub()
{
    std::printf("[decoder stub]\n");
    mp3::Status st;
    auto dec = mp3::create_decoder("nope.flac", mp3::DecoderOptions{}, st);
    check(dec == nullptr, "no backend yet -> nullptr");
    check(st.code() == mp3::ErrorCode::NotSupported, "reports NotSupported");
}

void test_null_output()
{
    std::printf("[null output, 300 ms]\n");
    mp3::Status st;
    auto out = mp3::create_audio_output(mp3::OutputBackend::Null, st);
    check(out != nullptr, "null backend created");
    if (!out) return;

    mp3::AudioFormat fmt;
    fmt.sample_rate = 48000;
    fmt.channels = mp3::ChannelLayout::Stereo;
    fmt.sample_format = mp3::SampleFormat::F32;

    st = out->open("", fmt, mp3::OutputMode::Shared);
    check(st.ok(), "open() succeeds");
    check(out->is_open(), "is_open() is true after open()");

    const int block = out->buffer_frames();
    check(block == 480, "buffer size is 10 ms (480 frames at 48k)");

    std::atomic<int> callbacks{0};
    std::atomic<bool> bad_size{false};
    std::atomic<bool> saw_nan{false};

    st = out->set_render_callback(
        [&](uint8_t* dst, int frames, const mp3::AudioFormat& f) {
            if (frames != block) bad_size.store(true);
            if (f.sample_rate != 48000) bad_size.store(true);
            // Write a flat sine so the buffer is genuinely touched.
            auto* samples = reinterpret_cast<float*>(dst);
            const int total = frames * f.channel_count();
            for (int i = 0; i < total; ++i) {
                const float v = 0.25f;
                samples[i] = v;
                if (std::isnan(samples[i])) saw_nan.store(true);
            }
            callbacks.fetch_add(1);
        });
    check(st.ok(), "set_render_callback() succeeds");

    const auto t0 = std::chrono::steady_clock::now();
    st = out->start();
    check(st.ok(), "start() succeeds");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    out->stop();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();

    check(!bad_size.load(), "callback always got the advertised block size");
    check(!saw_nan.load(), "samples round-tripped intact");

    const auto stats = out->stats();
    const double rendered_s =
        mp3::frames_to_seconds(static_cast<mp3::FrameCount>(stats.frames_rendered),
                               48000);
    std::printf("        callbacks=%d rendered=%.3f s elapsed=%lld ms\n",
                callbacks.load(), rendered_s,
                static_cast<long long>(elapsed_ms));

    check(callbacks.load() > 10, "callback was actually called");
    check(stats.underruns == 0, "no underruns");
    // The null backend is the clock, so it should land within ~20% of the
    // wall-clock window it was asked to fill.
    check(rendered_s > 0.24 && rendered_s < 0.40,
          "rendered duration tracks wall clock");

    out->close();
    check(!out->is_open(), "close() resets the open state");
}

}  // namespace

int main()
{
    std::printf("MusicPlayer3 core smoke test (v%s)\n\n", mp3::version_string().data());

    test_version();
    test_status();
    test_audio_format();
    test_extensions();
    test_decoder_stub();
    test_null_output();

    std::printf("\n%s (%d failure%s)\n",
                g_failures == 0 ? "PASSED" : "FAILED", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
