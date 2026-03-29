#pragma once

#include "types.hpp"
#include <filesystem>
#include <memory>

namespace vsg {

// Forward declaration for whisper state RAII wrapper
struct WhisperStateDeleter {
    void* ctx;  // whisper_context*
    void operator()(void* state) const;
};
using WhisperStatePtr = std::unique_ptr<void, WhisperStateDeleter>;

class Transcriber {
public:
    // Load whisper model from file (no default state — create states explicitly)
    static auto create(const std::filesystem::path& model_path) -> Result<Transcriber>;

    ~Transcriber();

    Transcriber(const Transcriber&) = delete;
    Transcriber& operator=(const Transcriber&) = delete;
    Transcriber(Transcriber&& other) noexcept;
    Transcriber& operator=(Transcriber&& other) noexcept;

    // Create an independent state for parallel transcription.
    // Each state can be used on a separate thread concurrently.
    auto create_state() -> Result<WhisperStatePtr>;

    // Transcribe a single audio chunk using an independent state.
    // Thread-safe: multiple calls with different states can run in parallel.
    auto transcribe_chunk_with_state(void* state, const AudioChunk& chunk)
        -> Result<std::vector<Segment>>;

    // Transcribe full audio buffer in one pass (uses internal default state)
    auto transcribe_full(const std::vector<float>& samples) -> Result<std::vector<Segment>>;

private:
    explicit Transcriber(void* ctx);
    void* ctx_ = nullptr;  // whisper_context*, opaque to avoid header leak
};

} // namespace vsg
