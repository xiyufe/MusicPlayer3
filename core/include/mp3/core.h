// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 xiyufe
//
// MusicPlayer3 core -- common definitions shared by every module.
//
// This header (and everything else under core/) must stay free of any UI or
// platform framework dependency so that the exact same sources build for
// Windows (MSVC / MinGW) and Android (NDK) without modification.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------

// The build system passes these as compile definitions so the library version
// tracks CMakeLists.txt.  The fallbacks keep the headers usable standalone,
// e.g. when compiling a single source file by hand.
#ifndef MP3_VERSION_MAJOR
#    define MP3_VERSION_MAJOR 0
#endif
#ifndef MP3_VERSION_MINOR
#    define MP3_VERSION_MINOR 1
#endif
#ifndef MP3_VERSION_PATCH
#    define MP3_VERSION_PATCH 0
#endif

#define MP3_STRINGIFY_IMPL(x) #x
#define MP3_STRINGIFY(x) MP3_STRINGIFY_IMPL(x)

/// Packed numeric version, comparable with a single integer compare.
#define MP3_VERSION \
    ((MP3_VERSION_MAJOR << 16) | (MP3_VERSION_MINOR << 8) | (MP3_VERSION_PATCH))

/// Human readable version, e.g. "0.1.0".
#define MP3_VERSION_STRING                              \
    MP3_STRINGIFY(MP3_VERSION_MAJOR)                    \
    "." MP3_STRINGIFY(MP3_VERSION_MINOR)                \
    "." MP3_STRINGIFY(MP3_VERSION_PATCH)

namespace mp3 {

/// Runtime counterpart of MP3_VERSION_STRING.
/// Useful for log files and for the about dialog in the UI layer.
std::string_view version_string() noexcept;

// ---------------------------------------------------------------------------
// Export macro
// ---------------------------------------------------------------------------

#if defined(_WIN32) && defined(MP3_CORE_BUILD_SHARED)
#    define MP3_API __declspec(dllexport)
#elif defined(_WIN32) && defined(MP3_CORE_USE_SHARED)
#    define MP3_API __declspec(dllimport)
#elif defined(__GNUC__) && defined(MP3_CORE_BUILD_SHARED)
#    define MP3_API __attribute__((visibility("default")))
#else
#    define MP3_API
#endif

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------
//
// The audio pipeline runs on realtime threads where exceptions are a bad idea
// (they allocate, they unwind unwinding-hostile frames, and some embedded
// libc++ builds on Android are configured with -fno-exceptions).  So every
// fallible operation returns a Status instead of throwing.

enum class ErrorCode : uint8_t {
    Ok = 0,

    // Caller mistakes.
    InvalidArgument,
    InvalidState,

    // Capability / resource problems.
    NotSupported,
    NotFound,
    Busy,
    OutOfMemory,

    // I/O and decoding.
    IoError,
    DecodeError,
    EndOfStream,

    // Audio device problems.
    DeviceError,
    DeviceDisconnected,

    Internal,
};

/// Stable, human readable name of an error code (never localised).
MP3_API std::string_view to_string(ErrorCode code) noexcept;

/// Result of a fallible operation.
///
/// Cheap on the success path: a single enum value and an empty string, with
/// the small-string optimisation keeping it allocation free.
class MP3_API Status {
public:
    /// Success.
    Status() = default;

    Status(ErrorCode code, std::string message)
        : code_(code), message_(std::move(message))
    {
    }

    static Status success() noexcept { return Status{}; }

    static Status failure(ErrorCode code, std::string message)
    {
        return Status{code, std::move(message)};
    }

    [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::Ok; }

    /// True when the operation succeeded; lets callers write `if (!st) { ... }`.
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    /// "<code>: <message>" -- convenient for logging.
    [[nodiscard]] std::string describe() const;

private:
    ErrorCode code_ = ErrorCode::Ok;
    std::string message_;
};

// Convenience constructors used throughout the code base.
inline Status make_error(ErrorCode code, std::string message)
{
    return Status::failure(code, std::move(message));
}

/// Marker returned by decoders when the stream is exhausted normally.
inline Status end_of_stream() { return Status::failure(ErrorCode::EndOfStream, {}); }

}  // namespace mp3
