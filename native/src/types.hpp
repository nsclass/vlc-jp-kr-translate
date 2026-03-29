#pragma once

#include <cstdint>
#include <condition_variable>
#include <expected>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <vector>

namespace vsg {

// ── Audio chunk: 30 seconds of 16kHz mono float PCM ─────────────────────────

struct AudioChunk {
    std::vector<float> samples;    // Contiguous PCM data
    int64_t offset_ms = 0;        // Timestamp offset of this chunk in the full file

    [[nodiscard]] auto duration_ms() const noexcept -> int64_t {
        return static_cast<int64_t>(samples.size()) * 1000 / 16000;
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        return samples.empty();
    }
};

// ── Transcript segment from whisper ─────────────────────────────────────────

struct Segment {
    int64_t start_ms;
    int64_t end_ms;
    std::string text;
};

// ── Subtitle store: Structure of Arrays for cache-friendly iteration ────────

struct SubtitleStore {
    std::vector<int64_t> start_ms;
    std::vector<int64_t> end_ms;
    std::vector<std::string> texts;

    [[nodiscard]] auto count() const noexcept -> size_t {
        return start_ms.size();
    }

    void push_back(int64_t start, int64_t end, std::string text) {
        start_ms.push_back(start);
        end_ms.push_back(end);
        texts.push_back(std::move(text));
    }

    void reserve(size_t n) {
        start_ms.reserve(n);
        end_ms.reserve(n);
        texts.reserve(n);
    }
};

// ── Thread-safe bounded queue with done signaling ───────────────────────────

template <typename T>
class ThreadSafeQueue {
public:
    explicit ThreadSafeQueue(size_t max_size = 8) : max_size_(max_size) {}

    // Block until space is available or done. Returns false if done.
    auto push(T item) -> bool {
        std::unique_lock lock(mutex_);
        not_full_.wait(lock, [this] { return queue_.size() < max_size_ || done_; });
        if (done_) return false;
        queue_.push(std::move(item));
        not_empty_.notify_one();
        return true;
    }

    // Block until item is available. Returns nullopt when done and queue is empty.
    auto pop() -> std::optional<T> {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [this] { return !queue_.empty() || done_; });
        if (queue_.empty()) return std::nullopt;
        auto item = std::move(queue_.front());
        queue_.pop();
        not_full_.notify_one();
        return item;
    }

    void signal_done() {
        std::lock_guard lock(mutex_);
        done_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    [[nodiscard]] auto is_done() const -> bool {
        std::lock_guard lock(mutex_);
        return done_ && queue_.empty();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::queue<T> queue_;
    size_t max_size_;
    bool done_ = false;
};

// ── Timestamp remapping for VAD-filtered audio ─────────────────────────────
// When silence is stripped, whisper timestamps reference the concatenated
// speech buffer.  TimeMap translates those back to original-audio time.

struct TimeMap {
    struct Entry {
        int64_t buffer_start_ms;    // offset in the concatenated speech buffer
        int64_t original_start_ms;  // corresponding offset in original audio
        int64_t duration_ms;        // length of this speech chunk
    };
    std::vector<Entry> entries;

    // Map a timestamp from speech-buffer space → original-audio space.
    [[nodiscard]] auto remap(int64_t buffer_ms) const noexcept -> int64_t {
        // Walk backwards to find the containing entry.
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (buffer_ms >= it->buffer_start_ms) {
                auto offset = buffer_ms - it->buffer_start_ms;
                // Clamp to chunk duration to avoid overrun
                if (offset > it->duration_ms) offset = it->duration_ms;
                return it->original_start_ms + offset;
            }
        }
        return buffer_ms;  // fallback: identity
    }
};

// ── Error type ──────────────────────────────────────────────────────────────

using Error = std::string;

template <typename T>
using Result = std::expected<T, Error>;

using VoidResult = std::expected<void, Error>;

} // namespace vsg
