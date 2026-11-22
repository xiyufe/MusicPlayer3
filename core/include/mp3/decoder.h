// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 xiyufe
//
// MusicPlayer3 core -- decoder abstraction.
//
// A decoder turns a compressed file into a stream of PCM frames.  The
// interface is pull-based and single-threaded per instance: the audio thread
// calls read() and must never block for long, so implementations buffer ahead
// internally (see the decode-ahead design in docs/ARCHITECTURE.md).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mp3/audio_format.h"
#include "mp3/core.h"

namespace mp3 {

/// A block of decoded PCM.
///
/// Interleaved formats store frames back to back:
///   [L0 R0][L1 R1][L2 R2]...
/// Planar formats store `planes[ch]` pointing at plane `ch`, each holding
/// `frame_count` samples.  The buffer does not own the memory; it points into
/// a Decoder-owned pool that stays valid until the next read() call.
struct AudioBuffer {
    /// Interleaved pointer (null for planar buffers).
    const uint8_t* data = nullptr;
    /// Per-plane pointers (empty for interleaved buffers).
    const uint8_t* planes[8] = {};

    FrameCount frame_count = 0;  ///< frames in this block
    AudioFormat format;          ///< format of the samples in this block

    /// First frame of this block in stream coordinates.
    /// Set by the decoder so downstream stages never have to count.
    FrameCount stream_position = 0;

    [[nodiscard]] bool empty() const noexcept { return frame_count == 0; }
};

/// Static description of a decodable stream, available right after open().
struct StreamInfo {
    AudioFormat format;            ///< native format as stored in the file
    FrameCount total_frames = 0;   ///< 0 when the container does not say
    double duration_seconds = 0.0; ///< 0.0 when unknown
    int64_t bitrate = 0;           ///< average bits per second, 0 if unknown
    std::string codec_name;        ///< e.g. "flac", "mp3", "aac"
    std::string container_name;    ///< e.g. "flac", "mp3", "mov"
    bool seekable = false;

    /// True when the container reports an exact frame count.
    [[nodiscard]] bool has_exact_length() const noexcept
    {
        return total_frames > 0;
    }
};

/// Abstract decoder.
///
/// Threading contract: an instance is owned by exactly one decoding thread.
/// Construct on the owning thread, call open/read/seek/close on that same
/// thread, destroy it there too.  No internal locking is performed.
class MP3_API Decoder {
public:
    virtual ~Decoder() = default;

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    /// Open a file for decoding.  On success, info() returns valid data.
    virtual Status open(const std::string& path) = 0;

    /// Release the file handle.  Safe to call on a closed decoder.
    virtual void close() = 0;

    [[nodiscard]] virtual bool is_open() const noexcept = 0;

    /// Description of the stream.  Only valid while is_open().
    [[nodiscard]] virtual const StreamInfo& info() const noexcept = 0;

    /// Decode the next block of frames.
    ///
    /// Returns:
    ///   - ok()               -> `out` holds frame_count > 0 frames
    ///   - end_of_stream()    -> the stream ended cleanly
    ///   - any other failure  -> `out` is untouched
    ///
    /// The returned buffer is valid until the next call to read() or close().
    virtual Status read(AudioBuffer& out) = 0;

    /// Seek to an absolute frame position.
    ///
    /// The decoder lands on the nearest frame it can resume from at or before
    /// `target` and reports where it actually landed via `landed`.  Callers
    /// that need sample accuracy must discard the difference.
    virtual Status seek(FrameCount target, FrameCount& landed) = 0;

protected:
    Decoder() = default;
};

/// Options for creating a decoder.
struct DecoderOptions {
    /// Format the caller wants out of read().  The decoder inserts a
    /// conversion stage when the file's native format differs.  Leave
    /// `sample_format` as F32 and set `sample_rate` to 0 to request the
    /// file's native rate with float samples -- the pipeline's default.
    AudioFormat output_format{};

    /// Maximum frames per read() call.  0 means "decoder's choice".
    int max_frames_per_read = 0;
};

/// Create a decoder for a file.
///
/// Implementations are registered at build time: the default build wires in
/// the FFmpeg backend on desktop and Android alike.  Returns nullptr and sets
/// `status` when no backend understands the file.
MP3_API std::unique_ptr<Decoder> create_decoder(const std::string& path,
                                                const DecoderOptions& options,
                                                Status& status);

/// File extensions the current build can open, lower case, no leading dot.
/// Intended for file dialogs and library scanners.
MP3_API std::vector<std::string_view> supported_extensions();

/// True when the extension is listed by supported_extensions().
MP3_API bool is_extension_supported(std::string_view extension) noexcept;

}  // namespace mp3
