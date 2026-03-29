#pragma once

#include "types.hpp"
#include <filesystem>
#include <format>
#include <string>

namespace vsg {

// Format milliseconds to SRT timestamp: HH:MM:SS,mmm
// Uses constexpr where possible for compile-time evaluation
constexpr auto format_srt_time(int64_t ms) -> std::string {
    auto hours = ms / 3'600'000;
    ms %= 3'600'000;
    auto minutes = ms / 60'000;
    ms %= 60'000;
    auto seconds = ms / 1'000;
    auto millis = ms % 1'000;

    return std::format("{:02d}:{:02d}:{:02d},{:03d}", hours, minutes, seconds, millis);
}

// Write SubtitleStore to SRT file. SoA layout means timestamps are iterated contiguously.
auto write_srt(const SubtitleStore& store, const std::filesystem::path& path) -> VoidResult;

} // namespace vsg
