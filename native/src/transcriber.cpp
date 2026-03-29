#include "transcriber.hpp"
#include <whisper.h>

namespace vsg {

Transcriber::Transcriber(void* ctx) : ctx_(ctx) {}

Transcriber::~Transcriber() {
    if (ctx_) {
        whisper_free(static_cast<whisper_context*>(ctx_));
    }
}

Transcriber::Transcriber(Transcriber&& other) noexcept : ctx_(other.ctx_) {
    other.ctx_ = nullptr;
}

Transcriber& Transcriber::operator=(Transcriber&& other) noexcept {
    if (this != &other) {
        if (ctx_) whisper_free(static_cast<whisper_context*>(ctx_));
        ctx_ = other.ctx_;
        other.ctx_ = nullptr;
    }
    return *this;
}

auto Transcriber::create(const std::filesystem::path& model_path) -> Result<Transcriber> {
    if (!std::filesystem::exists(model_path)) {
        return std::unexpected("Model file not found: " + model_path.string());
    }

    auto params = whisper_context_default_params();
    auto* ctx = whisper_init_from_file_with_params(model_path.c_str(), params);
    if (!ctx) {
        return std::unexpected("Failed to initialize whisper model from: " + model_path.string());
    }

    return Transcriber(ctx);
}

auto Transcriber::transcribe_chunk(const AudioChunk& chunk) -> Result<std::vector<Segment>> {
    if (!ctx_) {
        return std::unexpected("Transcriber not initialized");
    }
    if (chunk.empty()) {
        return std::vector<Segment>{};
    }

    auto* wctx = static_cast<whisper_context*>(ctx_);

    auto params = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    params.language = "ja";
    params.n_threads = 4;
    params.beam_search.beam_size = 5;
    params.no_timestamps = false;
    params.print_progress = false;
    params.print_realtime = false;
    params.print_special = false;
    params.print_timestamps = false;

    int ret = whisper_full(wctx, params, chunk.samples.data(),
                           static_cast<int>(chunk.samples.size()));
    if (ret != 0) {
        return std::unexpected("Whisper transcription failed with code: " + std::to_string(ret));
    }

    int n_segments = whisper_full_n_segments(wctx);
    std::vector<Segment> segments;
    segments.reserve(n_segments);

    for (int i = 0; i < n_segments; ++i) {
        const char* text = whisper_full_get_segment_text(wctx, i);
        if (!text || text[0] == '\0') continue;

        // whisper returns timestamps in centiseconds (10ms units)
        auto t0 = whisper_full_get_segment_t0(wctx, i) * 10;  // to ms
        auto t1 = whisper_full_get_segment_t1(wctx, i) * 10;

        // Adjust to absolute time using chunk offset
        segments.push_back(Segment{
            .start_ms = chunk.offset_ms + t0,
            .end_ms = chunk.offset_ms + t1,
            .text = std::string(text),
        });
    }

    return segments;
}

auto Transcriber::transcribe_full(const std::vector<float>& samples) -> Result<std::vector<Segment>> {
    if (!ctx_) {
        return std::unexpected("Transcriber not initialized");
    }
    if (samples.empty()) {
        return std::vector<Segment>{};
    }

    auto* wctx = static_cast<whisper_context*>(ctx_);

    auto params = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    params.language = "ja";
    params.n_threads = 8;
    params.beam_search.beam_size = 5;
    params.no_timestamps = false;
    params.print_progress = true;
    params.print_realtime = false;
    params.print_special = false;
    params.print_timestamps = false;

    int ret = whisper_full(wctx, params, samples.data(),
                           static_cast<int>(samples.size()));
    if (ret != 0) {
        return std::unexpected("Whisper transcription failed with code: " + std::to_string(ret));
    }

    int n_segments = whisper_full_n_segments(wctx);
    std::vector<Segment> segments;
    segments.reserve(n_segments);

    for (int i = 0; i < n_segments; ++i) {
        const char* text = whisper_full_get_segment_text(wctx, i);
        if (!text || text[0] == '\0') continue;

        auto t0 = whisper_full_get_segment_t0(wctx, i) * 10;
        auto t1 = whisper_full_get_segment_t1(wctx, i) * 10;

        segments.push_back(Segment{
            .start_ms = t0,
            .end_ms = t1,
            .text = std::string(text),
        });
    }

    return segments;
}

} // namespace vsg
