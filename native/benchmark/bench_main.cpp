#include "pipeline.hpp"
#include "audio_decoder.hpp"
#include "transcriber.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <string_view>

namespace {

using Clock = std::chrono::steady_clock;

// Python baseline from profiling (4.9GB / 116-min video)
constexpr double PYTHON_AUDIO_EXTRACT_S = 304.02;
constexpr double PYTHON_TRANSCRIBE_S = 99.24;
constexpr double PYTHON_TOTAL_S = 405.83;

void print_comparison(std::string_view stage,
                      double python_s, double cpp_s) {
    double speedup = python_s / cpp_s;
    std::cerr << std::format("  {:<20s}  {:>8.2f}s  {:>8.2f}s  {:>6.1f}x\n",
                             stage, python_s, cpp_s, speedup);
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << std::format(
            "Usage: {} <input.mp4> <model_path>\n\n"
            "Benchmarks the C++ pipeline and compares with Python baseline.\n",
            argv[0]);
        return 1;
    }

    std::filesystem::path input(argv[1]);
    std::filesystem::path model(argv[2]);

    if (!std::filesystem::exists(input)) {
        std::cerr << "Error: Input file not found: " << input << "\n";
        return 1;
    }
    if (!std::filesystem::exists(model)) {
        std::cerr << "Error: Model file not found: " << model << "\n";
        return 1;
    }

    auto output_dir = std::filesystem::temp_directory_path() / "vlc-bench";
    std::filesystem::create_directories(output_dir);

    // ── Benchmark: Audio decode only ─────────────────────────────────────────
    std::cerr << "=== Benchmark: Audio Decode ===\n";
    int chunk_count = 0;
    int speech_count = 0;
    auto t0 = Clock::now();
    auto decode_result = vsg::decode_audio(input, [&](vsg::AudioChunk chunk) {
        ++chunk_count;
        if (vsg::has_speech(std::span<const float>(chunk.samples))) {
            ++speech_count;
        }
        std::cerr << std::format("  chunk {} ({:.1f}s audio)\r",
                                 chunk_count, chunk.duration_ms() / 1000.0);
    });
    auto t1 = Clock::now();
    double decode_s = std::chrono::duration<double>(t1 - t0).count();

    if (!decode_result) {
        std::cerr << "Decode failed: " << decode_result.error() << "\n";
        return 1;
    }
    std::cerr << std::format("\n  Decoded {} chunks ({} speech, {} silence) in {:.2f}s\n\n",
                             chunk_count, speech_count, chunk_count - speech_count, decode_s);

    // ── Benchmark: Full pipeline (with VAD + folly concurrency) ─────────────
    std::cerr << "=== Benchmark: Full Pipeline (VAD + folly) ===\n";
    vsg::PipelineConfig config{
        .input_path = input,
        .output_dir = output_dir,
        .model_path = model,
    };

    auto pipeline_result = vsg::run_pipeline(config);
    if (!pipeline_result) {
        std::cerr << "Pipeline failed: " << pipeline_result.error() << "\n";
        return 1;
    }

    // ── Results ──────────────────────────────────────────────────────────────
    std::cerr << "\n╔══════════════════════════════════════════════════════════╗\n";
    std::cerr <<   "║            BENCHMARK RESULTS: Python vs C++             ║\n";
    std::cerr <<   "╠══════════════════════════════════════════════════════════╣\n";
    std::cerr <<   "║  Stage                Python      C++      Speedup     ║\n";
    std::cerr <<   "╠══════════════════════════════════════════════════════════╣\n";

    print_comparison("Audio decode+VAD", PYTHON_AUDIO_EXTRACT_S, pipeline_result->decode_seconds);
    print_comparison("Transcription", PYTHON_TRANSCRIBE_S, pipeline_result->transcribe_seconds);
    print_comparison("TOTAL", PYTHON_TOTAL_S, pipeline_result->total_seconds);

    std::cerr << "╚══════════════════════════════════════════════════════════╝\n";
    std::cerr << std::format("\nVAD filtering: {}/{} chunks contain speech\n",
                             pipeline_result->speech_chunks, pipeline_result->total_chunks);
    std::cerr << std::format("Output: {}\n", pipeline_result->srt_path.string());
    std::cerr << std::format("Segments: {}\n", pipeline_result->store.count());

    return 0;
}
