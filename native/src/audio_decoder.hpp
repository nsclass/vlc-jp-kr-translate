#pragma once

#include "types.hpp"
#include <filesystem>
#include <functional>
#include <span>

namespace vsg {

// Callback invoked for each decoded audio chunk
using ChunkCallback = std::function<void(AudioChunk)>;

// Streaming audio decoder: decodes and resamples to 16kHz mono float.
// Invokes callback for each ~30-second chunk. No temp files.
auto decode_audio(const std::filesystem::path& media_path,
                  ChunkCallback on_chunk) -> VoidResult;

// Get media duration in milliseconds (for progress reporting)
auto get_media_duration_ms(const std::filesystem::path& media_path) -> Result<int64_t>;

// Energy-based Voice Activity Detection.
// Checks 0.5s sub-windows within the sample span; returns true if any
// window's RMS energy exceeds the threshold.
auto has_speech(std::span<const float> samples, float rms_threshold = 0.005f) -> bool;

} // namespace vsg
