#include "transcriber.hpp"
#include <whisper.h>

namespace vsg {

// ── WhisperStateDeleter ─────────────────────────────────────────────────────

void WhisperStateDeleter::operator()(void* state) const {
    if (state) {
        whisper_free_state(static_cast<whisper_state*>(state));
    }
}

// ── Transcriber lifecycle ───────────────────────────────────────────────────

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
    // Load model without creating a default state — we create per-thread states
    auto* ctx = whisper_init_from_file_with_params_no_state(model_path.c_str(), params);
    if (!ctx) {
        return std::unexpected("Failed to initialize whisper model from: " + model_path.string());
    }

    return Transcriber(ctx);
}

// ── State management ────────────────────────────────────────────────────────

auto Transcriber::create_state() -> Result<WhisperStatePtr> {
    if (!ctx_) {
        return std::unexpected("Transcriber not initialized");
    }

    auto* wctx = static_cast<whisper_context*>(ctx_);
    auto* state = whisper_init_state(wctx);
    if (!state) {
        return std::unexpected("Failed to create whisper state");
    }

    return WhisperStatePtr(state, WhisperStateDeleter{ctx_});
}

// ── Parallel-safe chunk transcription ───────────────────────────────────────

auto Transcriber::transcribe_chunk_with_state(void* state, const AudioChunk& chunk)
    -> Result<std::vector<Segment>> {
    if (!ctx_ || !state) {
        return std::unexpected("Transcriber or state not initialized");
    }
    if (chunk.empty()) {
        return std::vector<Segment>{};
    }

    auto* wctx = static_cast<whisper_context*>(ctx_);
    auto* wstate = static_cast<whisper_state*>(state);

    auto params = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    params.language = "ja";
    params.n_threads = 1;  // single thread per worker — parallelism is across chunks
    params.beam_search.beam_size = 5;
    params.no_timestamps = false;
    params.print_progress = false;
    params.print_realtime = false;
    params.print_special = false;
    params.print_timestamps = false;

    int ret = whisper_full_with_state(wctx, wstate, params,
                                      chunk.samples.data(),
                                      static_cast<int>(chunk.samples.size()));
    if (ret != 0) {
        return std::unexpected("Whisper transcription failed with code: " + std::to_string(ret));
    }

    int n_segments = whisper_full_n_segments_from_state(wstate);
    std::vector<Segment> segments;
    segments.reserve(n_segments);

    for (int i = 0; i < n_segments; ++i) {
        const char* text = whisper_full_get_segment_text_from_state(wstate, i);
        if (!text || text[0] == '\0') continue;

        // whisper returns timestamps in centiseconds (10ms units)
        auto t0 = whisper_full_get_segment_t0_from_state(wstate, i) * 10;
        auto t1 = whisper_full_get_segment_t1_from_state(wstate, i) * 10;

        // Adjust to absolute time using chunk offset
        segments.push_back(Segment{
            .start_ms = chunk.offset_ms + t0,
            .end_ms = chunk.offset_ms + t1,
            .text = std::string(text),
        });
    }

    return segments;
}

// ── Single-pass transcription (backwards compat) ────────────────────────────

auto Transcriber::transcribe_full(const std::vector<float>& samples) -> Result<std::vector<Segment>> {
    if (!ctx_) {
        return std::unexpected("Transcriber not initialized");
    }
    if (samples.empty()) {
        return std::vector<Segment>{};
    }

    // Create a temporary state for single-pass use
    auto state_result = create_state();
    if (!state_result) {
        return std::unexpected(state_result.error());
    }

    auto* wctx = static_cast<whisper_context*>(ctx_);
    auto* wstate = static_cast<whisper_state*>(state_result->get());

    auto params = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    params.language = "ja";
    params.n_threads = 8;
    params.beam_search.beam_size = 5;
    params.no_timestamps = false;
    params.print_progress = true;
    params.print_realtime = false;
    params.print_special = false;
    params.print_timestamps = false;

    int ret = whisper_full_with_state(wctx, wstate, params,
                                      samples.data(),
                                      static_cast<int>(samples.size()));
    if (ret != 0) {
        return std::unexpected("Whisper transcription failed with code: " + std::to_string(ret));
    }

    int n_segments = whisper_full_n_segments_from_state(wstate);
    std::vector<Segment> segments;
    segments.reserve(n_segments);

    for (int i = 0; i < n_segments; ++i) {
        const char* text = whisper_full_get_segment_text_from_state(wstate, i);
        if (!text || text[0] == '\0') continue;

        auto t0 = whisper_full_get_segment_t0_from_state(wstate, i) * 10;
        auto t1 = whisper_full_get_segment_t1_from_state(wstate, i) * 10;

        segments.push_back(Segment{
            .start_ms = t0,
            .end_ms = t1,
            .text = std::string(text),
        });
    }

    return segments;
}

} // namespace vsg
