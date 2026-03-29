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

#include <atomic>
#include <chrono>
#include <format>
#include <iostream>
#include <thread>

namespace vsg {

namespace {

using Clock = std::chrono::steady_clock;

// ── Stage 1: decode audio and load model concurrently ───────────────────────

struct DecodeAndLoadResult {
    std::vector<AudioChunk> chunks;
    Transcriber transcriber;
    double elapsed_seconds;
    double total_audio_seconds;
};

auto decode_and_load_model(const std::filesystem::path& input_path,
                           const std::filesystem::path& model_path)
    -> Result<DecodeAndLoadResult> {

    auto start = Clock::now();

    std::vector<AudioChunk> chunks;
    chunks.reserve(256);
    Result<Transcriber> model_result = std::unexpected("not started");

    {
        folly::CPUThreadPoolExecutor executor(2);

        auto [decode_res, model_res] = folly::coro::blockingWait(
            folly::coro::collectAll(
                folly::coro::co_withExecutor(&executor,
                    [&]() -> folly::coro::Task<VoidResult> {
                        auto r = decode_audio(input_path, [&](AudioChunk chunk) {
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
                folly::coro::co_withExecutor(&executor,
                    [&]() -> folly::coro::Task<Result<Transcriber>> {
                        co_return Transcriber::create(model_path);
                    }())));

        if (!decode_res) {
            return std::unexpected(decode_res.error());
        }
        model_result = std::move(model_res);
    }

    if (!model_result) {
        return std::unexpected(model_result.error());
    }
    if (chunks.empty()) {
        return std::unexpected("No audio decoded");
    }

    double total_audio_s =
        static_cast<double>(chunks.back().offset_ms + chunks.back().duration_ms()) / 1000.0;
    double elapsed = std::chrono::duration<double>(Clock::now() - start).count();

    return DecodeAndLoadResult{
        .chunks = std::move(chunks),
        .transcriber = std::move(*model_result),
        .elapsed_seconds = elapsed,
        .total_audio_seconds = total_audio_s,
    };
}

// ── Stage 2: parallel transcription across all cores ────────────────────────

// Metal GPU handles internal parallelism, so too many concurrent whisper
// states fight over GPU resources and produce empty results.  Cap workers
// and spread CPU threads across them so total threads ≈ hardware_concurrency.
constexpr int MAX_WORKERS = 4;

struct WorkerConfig {
    int num_workers;
    int threads_per_worker;
};

auto compute_worker_config() -> WorkerConfig {
    auto hw = static_cast<int>(std::thread::hardware_concurrency());
    int workers = std::min(hw, MAX_WORKERS);
    int threads = std::max(1, hw / workers);
    return {workers, threads};
}

auto create_worker_states(Transcriber& transcriber, int num_workers)
    -> Result<std::vector<WhisperStatePtr>> {

    std::vector<WhisperStatePtr> states;
    states.reserve(num_workers);
    for (int i = 0; i < num_workers; ++i) {
        auto state = transcriber.create_state();
        if (!state) {
            return std::unexpected("Failed to create whisper state: " + state.error());
        }
        states.push_back(std::move(*state));
    }
    return states;
}

auto transcribe_chunks_parallel(Transcriber& transcriber,
                                std::vector<WhisperStatePtr>& states,
                                const std::vector<AudioChunk>& chunks,
                                const WorkerConfig& wc)
    -> Result<std::vector<std::vector<Segment>>> {

    std::vector<std::vector<Segment>> all_segments(chunks.size());
    std::atomic<int> completed{0};
    std::atomic<int> errors{0};
    auto total = static_cast<int>(chunks.size());

    folly::CPUThreadPoolExecutor executor(wc.num_workers);

    folly::coro::blockingWait(
        folly::coro::co_withExecutor(&executor,
            [&]() -> folly::coro::Task<void> {
                // One coroutine per worker — each owns its state exclusively.
                // Worker w processes chunks w, w+N, w+2N, ... sequentially.
                std::vector<folly::coro::Task<void>> worker_tasks;
                worker_tasks.reserve(wc.num_workers);

                for (int w = 0; w < wc.num_workers; ++w) {
                    worker_tasks.push_back(
                        [&, w]() -> folly::coro::Task<void> {
                            for (auto i = static_cast<size_t>(w);
                                 i < chunks.size();
                                 i += static_cast<size_t>(wc.num_workers)) {
                                auto result = transcriber.transcribe_chunk_with_state(
                                    states[w].get(), chunks[i], wc.threads_per_worker);
                                if (result) {
                                    all_segments[i] = std::move(*result);
                                } else {
                                    errors.fetch_add(1, std::memory_order_relaxed);
                                    std::cerr << std::format(
                                        "\n  [worker {}] chunk {} failed: {}\n",
                                        w, i, result.error());
                                }
                                auto done = completed.fetch_add(1,
                                    std::memory_order_relaxed) + 1;
                                if (done % 10 == 0 || done == total) {
                                    std::cerr << std::format(
                                        "  transcribed {}/{} chunks\r",
                                        done, total);
                                }
                            }
                            co_return;
                        }());
                }

                co_await folly::coro::collectAllRange(std::move(worker_tasks));
            }()));

    if (errors.load() == total) {
        return std::unexpected("All chunks failed transcription");
    }

    return all_segments;
}

// ── Build SubtitleStore from raw segments ────────────────────────────────────

auto build_subtitle_store(std::vector<std::vector<Segment>>& all_segments)
    -> SubtitleStore {

    SubtitleStore store;
    for (auto& chunk_segments : all_segments) {
        for (auto& seg : chunk_segments) {
            auto& text = seg.text;
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
    return store;
}

} // namespace

// ── Public entry point ──────────────────────────────────────────────────────

auto run_pipeline(const PipelineConfig& config) -> Result<PipelineResult> {
    auto pipeline_start = Clock::now();

    if (!std::filesystem::exists(config.input_path)) {
        return std::unexpected("Input file not found: " + config.input_path.string());
    }
    std::filesystem::create_directories(config.output_dir);

    auto wc = compute_worker_config();

    // ── Stage 1 ─────────────────────────────────────────────────────────────
    std::cerr << std::format("[1/3] Decoding audio + loading model...\n");

    auto stage1 = decode_and_load_model(config.input_path, config.model_path);
    if (!stage1) {
        return std::unexpected(stage1.error());
    }

    std::cerr << std::format(
        "\n  decoded {} chunks ({:.0f}s audio) + model loaded in {:.1f}s\n",
        stage1->chunks.size(), stage1->total_audio_seconds, stage1->elapsed_seconds);

    // ── Stage 2 ─────────────────────────────────────────────────────────────
    std::cerr << std::format(
        "[2/3] Transcribing {} chunks ({} workers x {} threads)...\n",
        stage1->chunks.size(), wc.num_workers, wc.threads_per_worker);
    auto transcribe_start = Clock::now();

    auto states = create_worker_states(stage1->transcriber, wc.num_workers);
    if (!states) {
        return std::unexpected(states.error());
    }

    auto segments_result = transcribe_chunks_parallel(
        stage1->transcriber, *states, stage1->chunks, wc);
    if (!segments_result) {
        return std::unexpected(segments_result.error());
    }
    auto all_segments = std::move(*segments_result);

    double transcribe_seconds =
        std::chrono::duration<double>(Clock::now() - transcribe_start).count();

    auto store = build_subtitle_store(all_segments);

    std::cerr << std::format("\n  transcribed {} segments in {:.1f}s ({:.1f}x realtime)\n",
                             store.count(), transcribe_seconds,
                             stage1->total_audio_seconds / transcribe_seconds);

    if (store.count() == 0) {
        return std::unexpected("No speech segments detected");
    }

    // ── Stage 3 ─────────────────────────────────────────────────────────────
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
        .decode_seconds = stage1->elapsed_seconds,
        .transcribe_seconds = transcribe_seconds,
        .total_seconds = total,
        .total_chunks = static_cast<int>(stage1->chunks.size()),
        .speech_chunks = static_cast<int>(stage1->chunks.size()),
    };
}

} // namespace vsg
