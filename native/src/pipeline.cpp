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

#include <algorithm>
#include <chrono>
#include <format>
#include <iostream>
#include <thread>

namespace vsg {

namespace {

using Clock = std::chrono::steady_clock;

} // namespace

auto run_pipeline(const PipelineConfig& config) -> Result<PipelineResult> {
    auto pipeline_start = Clock::now();

    // Validate inputs
    if (!std::filesystem::exists(config.input_path)) {
        return std::unexpected("Input file not found: " + config.input_path.string());
    }
    std::filesystem::create_directories(config.output_dir);

    const auto num_workers = static_cast<int>(std::thread::hardware_concurrency());

    // ── Stage 1: Decode audio + Load model (concurrent) ─────────────────────
    std::cerr << std::format("[1/3] Decoding audio + loading model ({} cores)...\n",
                             num_workers);
    auto stage1_start = Clock::now();

    // Decode audio into chunks (runs on one thread of the pool)
    std::vector<AudioChunk> chunks;
    chunks.reserve(256);
    Result<Transcriber> model_result = std::unexpected("not started");

    {
        folly::CPUThreadPoolExecutor executor(2);

        auto [decode_res, model_res] = folly::coro::blockingWait(
            folly::coro::collectAll(
                // Decode all audio into chunks
                folly::coro::co_withExecutor(&executor,
                    [&]() -> folly::coro::Task<VoidResult> {
                        auto r = decode_audio(config.input_path, [&](AudioChunk chunk) {
                            chunks.push_back(std::move(chunk));
                            if (chunks.size() % 10 == 0) {
                                std::cerr << std::format(
                                    "  decoded {} chunks ({:.0f}s audio)\r",
                                    chunks.size(),
                                    static_cast<double>(chunks.back().offset_ms +
                                        chunks.back().duration_ms()) / 1000.0);
                            }
                        });
                        co_return r;
                    }()),
                // Load whisper model concurrently
                folly::coro::co_withExecutor(&executor,
                    [&]() -> folly::coro::Task<Result<Transcriber>> {
                        co_return Transcriber::create(config.model_path);
                    }())));

        if (!decode_res) {
            return std::unexpected(decode_res.error());
        }
        model_result = std::move(model_res);
    }

    if (!model_result) {
        return std::unexpected(model_result.error());
    }
    auto transcriber = std::move(*model_result);

    auto stage1_end = Clock::now();
    double decode_seconds = std::chrono::duration<double>(stage1_end - stage1_start).count();

    double total_audio_s = chunks.empty() ? 0.0 :
        static_cast<double>(chunks.back().offset_ms + chunks.back().duration_ms()) / 1000.0;
    std::cerr << std::format("\n  decoded {} chunks ({:.0f}s audio) + model loaded in {:.1f}s\n",
                             chunks.size(), total_audio_s, decode_seconds);

    if (chunks.empty()) {
        return std::unexpected("No audio decoded");
    }

    // ── Stage 2: Parallel transcription across all cores ────────────────────
    std::cerr << std::format("[2/3] Transcribing {} chunks on {} workers...\n",
                             chunks.size(), num_workers);
    auto transcribe_start = Clock::now();

    // Create per-worker whisper states (shared model, independent state)
    std::vector<WhisperStatePtr> states;
    states.reserve(num_workers);
    for (int i = 0; i < num_workers; ++i) {
        auto state = transcriber.create_state();
        if (!state) {
            return std::unexpected("Failed to create whisper state: " + state.error());
        }
        states.push_back(std::move(*state));
    }

    // Dispatch all chunks to the thread pool for parallel transcription
    std::vector<std::vector<Segment>> all_segments(chunks.size());
    std::atomic<int> completed{0};

    {
        folly::CPUThreadPoolExecutor executor(num_workers);

        // Run all chunks concurrently under the executor
        folly::coro::blockingWait(
            folly::coro::co_withExecutor(&executor,
                [&]() -> folly::coro::Task<void> {
                    std::vector<folly::coro::Task<void>> tasks;
                    tasks.reserve(chunks.size());

                    for (size_t i = 0; i < chunks.size(); ++i) {
                        auto worker_idx = i % static_cast<size_t>(num_workers);
                        tasks.push_back(
                            [&, i, worker_idx]() -> folly::coro::Task<void> {
                                auto result = transcriber.transcribe_chunk_with_state(
                                    states[worker_idx].get(), chunks[i]);
                                if (result) {
                                    all_segments[i] = std::move(*result);
                                }
                                auto done = completed.fetch_add(1,
                                    std::memory_order_relaxed) + 1;
                                if (done % 10 == 0 ||
                                    done == static_cast<int>(chunks.size())) {
                                    std::cerr << std::format(
                                        "  transcribed {}/{} chunks\r",
                                        done, chunks.size());
                                }
                                co_return;
                            }());
                    }

                    co_await folly::coro::collectAllRange(std::move(tasks));
                }()));
    }

    auto transcribe_end = Clock::now();
    double transcribe_seconds =
        std::chrono::duration<double>(transcribe_end - transcribe_start).count();

    // Merge segments from all chunks (already in order since chunks are sequential)
    SubtitleStore store;
    for (auto& chunk_segments : all_segments) {
        for (auto& seg : chunk_segments) {
            auto text = seg.text;
            auto first = text.find_first_not_of(" \t\n\r");
            auto last  = text.find_last_not_of(" \t\n\r");
            if (first != std::string::npos) {
                text = text.substr(first, last - first + 1);
            }
            if (!text.empty()) {
                store.push_back(seg.start_ms, seg.end_ms, std::move(text));
            }
        }
    }

    std::cerr << std::format("\n  transcribed {} segments in {:.1f}s "
                             "({:.1f}x realtime)\n",
                             store.count(), transcribe_seconds,
                             total_audio_s / transcribe_seconds);

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
        .total_chunks = static_cast<int>(chunks.size()),
        .speech_chunks = static_cast<int>(chunks.size()),
    };
}

} // namespace vsg
