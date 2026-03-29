#include "pipeline.hpp"
#include "audio_decoder.hpp"
#include "transcriber.hpp"
#include "srt_writer.hpp"

// macOS math.h macros clash with fmtlib (pulled in by folly)
#include <cmath>
#undef isfinite
#undef isinf
#undef isnan
#undef isnormal
#undef signbit
#undef fpclassify

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Task.h>
#include <folly/executors/CPUThreadPoolExecutor.h>

#include <chrono>
#include <format>
#include <iostream>
#include <span>

namespace vsg {

namespace {

using Clock = std::chrono::steady_clock;

// Result of decode + VAD stage
struct DecodeStageResult {
    std::vector<float> speech_samples;
    TimeMap time_map;
    int total_chunks = 0;
    int speech_chunks = 0;
    double audio_seconds = 0.0;
    double speech_seconds = 0.0;
};

// ── Coroutine: decode audio and filter silence via VAD ──────────────────────

auto decode_and_filter(const std::filesystem::path& input_path)
    -> folly::coro::Task<Result<DecodeStageResult>> {

    DecodeStageResult result;
    result.speech_samples.reserve(16000 * 60 * 60);  // ~1 hour pre-alloc

    int64_t buffer_offset_ms = 0;

    auto decode_result = decode_audio(input_path, [&](AudioChunk chunk) {
        ++result.total_chunks;
        result.audio_seconds =
            static_cast<double>(chunk.offset_ms + chunk.duration_ms()) / 1000.0;

        if (has_speech(std::span<const float>(chunk.samples))) {
            ++result.speech_chunks;

            auto chunk_duration_ms = chunk.duration_ms();

            // Record mapping: buffer position → original time
            result.time_map.entries.push_back({
                .buffer_start_ms = buffer_offset_ms,
                .original_start_ms = chunk.offset_ms,
                .duration_ms = chunk_duration_ms,
            });

            result.speech_samples.insert(
                result.speech_samples.end(),
                chunk.samples.begin(),
                chunk.samples.end());

            buffer_offset_ms += chunk_duration_ms;
        }

        if (result.total_chunks % 10 == 0) {
            std::cerr << std::format(
                "  decoded {:.0f}s, speech {:.0f}s ({} / {} chunks)\r",
                result.audio_seconds,
                static_cast<double>(result.speech_samples.size()) / 16000.0,
                result.speech_chunks, result.total_chunks);
        }
    });

    if (!decode_result) {
        co_return std::unexpected(decode_result.error());
    }

    result.speech_seconds =
        static_cast<double>(result.speech_samples.size()) / 16000.0;

    co_return result;
}

// ── Coroutine: load whisper model ───────────────────────────────────────────

auto load_model(const std::filesystem::path& model_path)
    -> folly::coro::Task<Result<Transcriber>> {

    co_return Transcriber::create(model_path);
}

} // namespace

auto run_pipeline(const PipelineConfig& config) -> Result<PipelineResult> {
    auto pipeline_start = Clock::now();

    // Validate inputs
    if (!std::filesystem::exists(config.input_path)) {
        return std::unexpected("Input file not found: " + config.input_path.string());
    }
    std::filesystem::create_directories(config.output_dir);

    // ── Stage 1: Decode + VAD filter  ‖  Load whisper model (concurrent) ────
    std::cerr << "[1/3] Decoding audio + loading model (concurrent)...\n";
    auto decode_start = Clock::now();

    folly::CPUThreadPoolExecutor executor(2);

    auto [decode_result, model_result] = folly::coro::blockingWait(
        folly::coro::collectAll(
            folly::coro::co_withExecutor(
                &executor, decode_and_filter(config.input_path)),
            folly::coro::co_withExecutor(
                &executor, load_model(config.model_path))));

    auto decode_end = Clock::now();
    double decode_seconds = std::chrono::duration<double>(decode_end - decode_start).count();

    if (!decode_result) {
        return std::unexpected(decode_result.error());
    }
    if (!model_result) {
        return std::unexpected(model_result.error());
    }

    auto& decoded = *decode_result;
    auto transcriber = std::move(*model_result);

    std::cerr << std::format(
        "\n  decoded {:.0f}s audio, {:.0f}s speech ({}/{} chunks) in {:.1f}s\n",
        decoded.audio_seconds, decoded.speech_seconds,
        decoded.speech_chunks, decoded.total_chunks, decode_seconds);

    if (decoded.speech_samples.empty()) {
        return std::unexpected("No speech detected in audio");
    }

    // ── Stage 2: Transcribe speech-only buffer ──────────────────────────────
    std::cerr << std::format("[2/3] Transcribing {:.0f}s of speech...\n",
                             decoded.speech_seconds);
    auto transcribe_start = Clock::now();

    auto segments_result = transcriber.transcribe_full(decoded.speech_samples);
    if (!segments_result) {
        return std::unexpected(segments_result.error());
    }

    auto transcribe_end = Clock::now();
    double transcribe_seconds =
        std::chrono::duration<double>(transcribe_end - transcribe_start).count();

    // Remap timestamps from speech-buffer space → original-audio space
    SubtitleStore store;
    for (auto& seg : *segments_result) {
        auto start = decoded.time_map.remap(seg.start_ms);
        auto end   = decoded.time_map.remap(seg.end_ms);

        auto text = seg.text;
        auto first = text.find_first_not_of(" \t\n\r");
        auto last  = text.find_last_not_of(" \t\n\r");
        if (first != std::string::npos) {
            text = text.substr(first, last - first + 1);
        }
        if (!text.empty()) {
            store.push_back(start, end, std::move(text));
        }
    }

    std::cerr << std::format("  transcribed {} segments in {:.1f}s\n",
                             store.count(), transcribe_seconds);

    if (store.count() == 0) {
        return std::unexpected("No speech segments detected");
    }

    // ── Stage 3: Write SRT ──────────────────────────────────────────────────
    auto stem = config.input_path.stem().string();
    auto srt_path = config.output_dir / (stem + "_ja.srt");

    std::cerr << std::format("[3/3] Writing {} segments to SRT...\n", store.count());
    auto write_result = write_srt(store, srt_path);
    if (!write_result) {
        return std::unexpected(write_result.error());
    }

    auto total = std::chrono::duration<double>(Clock::now() - pipeline_start).count();

    return PipelineResult{
        .store = std::move(store),
        .srt_path = srt_path,
        .decode_seconds = decode_seconds,
        .transcribe_seconds = transcribe_seconds,
        .total_seconds = total,
        .total_chunks = decoded.total_chunks,
        .speech_chunks = decoded.speech_chunks,
    };
}

} // namespace vsg
