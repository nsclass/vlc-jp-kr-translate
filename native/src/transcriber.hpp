#pragma once

#include "types.hpp"
#include <filesystem>

namespace vsg {

class Transcriber {
public:
    // Load whisper model from file
    static auto create(const std::filesystem::path& model_path) -> Result<Transcriber>;

    ~Transcriber();

    Transcriber(const Transcriber&) = delete;
    Transcriber& operator=(const Transcriber&) = delete;
    Transcriber(Transcriber&& other) noexcept;
    Transcriber& operator=(Transcriber&& other) noexcept;

    // Transcribe a single audio chunk, returning segments with absolute timestamps
    auto transcribe_chunk(const AudioChunk& chunk) -> Result<std::vector<Segment>>;

    // Transcribe full audio buffer in one pass (much faster than chunked)
    auto transcribe_full(const std::vector<float>& samples) -> Result<std::vector<Segment>>;

private:
    explicit Transcriber(void* ctx);
    void* ctx_ = nullptr;  // whisper_context*, opaque to avoid header leak
};

} // namespace vsg
