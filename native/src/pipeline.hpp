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
//   2. VAD-filter speech chunks, concatenate speech-only samples
//   3. Transcribe speech buffer in one whisper_full pass
//   4. Remap timestamps back to original-audio time
//   5. Write SRT
auto run_pipeline(const PipelineConfig& config) -> Result<PipelineResult>;

} // namespace vsg
