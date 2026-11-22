// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 xiyufe
//
// Decoder factory and extension registry.
//
// The backends themselves live in src/backend/ffmpeg/ and are linked in by
// CMake when the FFmpeg dependency is available.  Until that backend exists,
// create_decoder() reports NotSupported -- the interface is final, only the
// implementation is pending.

#include "mp3/decoder.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace mp3 {

namespace {

/// Extensions the FFmpeg backend is expected to handle.  Kept in one place so
/// the file dialog and the library scanner cannot disagree with the backend.
constexpr std::array<std::string_view, 21> kKnownExtensions = {
    "mp3", "flac", "ogg", "oga", "opus", "m4a", "mp4", "aac", "wav", "aiff",
    "aif", "ape", "wv", "wma", "dsf", "dff", "mka", "m4b", "caf", "w64",
    "tta",
};

}  // namespace

std::unique_ptr<Decoder> create_decoder(const std::string& path,
                                        const DecoderOptions& options,
                                        Status& status)
{
    (void)path;
    (void)options;

    // TODO(backend): dispatch on the file's extension / container and hand off
    // to the FFmpeg-backed decoder once it is compiled in.  A compile-time
    // switch (MP3_HAVE_FFMPEG) picks between the real backend and this stub.
    status = make_error(ErrorCode::NotSupported,
                        "no decoder backend is compiled in yet");
    return nullptr;
}

std::vector<std::string_view> supported_extensions()
{
    return {kKnownExtensions.begin(), kKnownExtensions.end()};
}

bool is_extension_supported(std::string_view extension) noexcept
{
    if (extension.empty()) return false;
    // Tolerate a leading dot and mixed case from callers.
    if (extension.front() == '.') extension.remove_prefix(1);

    return std::any_of(
        kKnownExtensions.begin(), kKnownExtensions.end(),
        [extension](std::string_view known) noexcept {
            if (known.size() != extension.size()) return false;
            return std::equal(
                known.begin(), known.end(), extension.begin(),
                [](char a, char b) noexcept {
                    return a == b ||
                           a == static_cast<char>(std::tolower(
                                    static_cast<unsigned char>(b)));
                });
        });
}

}  // namespace mp3
