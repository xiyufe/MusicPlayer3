// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 xiyufe

#include "mp3/core.h"

namespace mp3 {

std::string_view version_string() noexcept
{
    return MP3_VERSION_STRING;
}

std::string_view to_string(ErrorCode code) noexcept
{
    switch (code) {
        case ErrorCode::Ok:                return "Ok";
        case ErrorCode::InvalidArgument:   return "InvalidArgument";
        case ErrorCode::InvalidState:      return "InvalidState";
        case ErrorCode::NotSupported:      return "NotSupported";
        case ErrorCode::NotFound:          return "NotFound";
        case ErrorCode::Busy:              return "Busy";
        case ErrorCode::OutOfMemory:       return "OutOfMemory";
        case ErrorCode::IoError:           return "IoError";
        case ErrorCode::DecodeError:       return "DecodeError";
        case ErrorCode::EndOfStream:       return "EndOfStream";
        case ErrorCode::DeviceError:       return "DeviceError";
        case ErrorCode::DeviceDisconnected:return "DeviceDisconnected";
        case ErrorCode::Internal:          return "Internal";
    }
    return "Unknown";
}

std::string Status::describe() const
{
    if (code_ == ErrorCode::Ok) return "Ok";
    if (message_.empty()) return std::string{to_string(code_)};
    return std::string{to_string(code_)} + ": " + message_;
}

}  // namespace mp3
