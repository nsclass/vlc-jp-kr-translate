#pragma once

#include "types.hpp"
#include <filesystem>
#include <string>

namespace vsg {

struct PipelineConfig {
    std::filesystem::path input_path;
    std::filesystem::path output_dir;
    std::filesystem::path model_path;
};

struct PipelineResult {
    SubtitleStore store;
    std::filesystem::path srt_path;
    double decode_seconds = 0.0;
    double transcribe_seconds = 0.0;
    double total_seconds = 0.0;
    int total_chunks = 0;
    int speech_chunks = 0;
};

// Run the full pipeline with folly structured concurrency:
//   1. Decode audio + load whisper model (concurrent via folly::coro::collectAll)
//   2. Parallel transcription: all chunks across N workers (N = hardware_concurrency)
//      Each worker has its own whisper_state sharing the same model weights.
//   3. Merge segments and write SRT
auto run_pipeline(const PipelineConfig& config) -> Result<PipelineResult>;

} // namespace vsg
