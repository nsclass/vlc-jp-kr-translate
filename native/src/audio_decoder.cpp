#include "audio_decoder.hpp"

#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <cstdint>
#include <memory>

namespace vsg {

namespace {

constexpr int TARGET_SAMPLE_RATE = 16000;
constexpr int CHUNK_SECONDS = 30;
constexpr int CHUNK_SAMPLES = TARGET_SAMPLE_RATE * CHUNK_SECONDS;

// RAII wrappers for FFmpeg resources
struct FormatCtxDeleter {
    void operator()(AVFormatContext* p) const {
        if (p) avformat_close_input(&p);
    }
};

struct CodecCtxDeleter {
    void operator()(AVCodecContext* p) const {
        if (p) avcodec_free_context(&p);
    }
};

struct SwrCtxDeleter {
    void operator()(SwrContext* p) const {
        if (p) swr_free(&p);
    }
};

struct PacketDeleter {
    void operator()(AVPacket* p) const {
        if (p) av_packet_free(&p);
    }
};

struct FrameDeleter {
    void operator()(AVFrame* p) const {
        if (p) av_frame_free(&p);
    }
};

using FormatCtxPtr = std::unique_ptr<AVFormatContext, FormatCtxDeleter>;
using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxDeleter>;
using SwrCtxPtr = std::unique_ptr<SwrContext, SwrCtxDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

} // namespace

auto decode_audio(const std::filesystem::path& media_path,
                  ChunkCallback on_chunk) -> VoidResult {

    // Open input file
    AVFormatContext* raw_fmt = nullptr;
    if (avformat_open_input(&raw_fmt, media_path.c_str(), nullptr, nullptr) < 0) {
        return std::unexpected("Failed to open media file: " + media_path.string());
    }
    FormatCtxPtr fmt_ctx(raw_fmt);

    if (avformat_find_stream_info(fmt_ctx.get(), nullptr) < 0) {
        return std::unexpected("Failed to find stream info");
    }

    // Find audio stream
    int audio_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_idx = static_cast<int>(i);
            break;
        }
    }
    if (audio_idx < 0) {
        return std::unexpected("No audio stream found");
    }

    auto* codecpar = fmt_ctx->streams[audio_idx]->codecpar;

    // Open decoder
    const auto* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        return std::unexpected("Unsupported audio codec");
    }

    CodecCtxPtr codec_ctx(avcodec_alloc_context3(codec));
    if (!codec_ctx) {
        return std::unexpected("Failed to allocate codec context");
    }
    avcodec_parameters_to_context(codec_ctx.get(), codecpar);
    if (avcodec_open2(codec_ctx.get(), codec, nullptr) < 0) {
        return std::unexpected("Failed to open audio decoder");
    }

    // Set up resampler: source format → 16kHz mono float32
    SwrContext* raw_swr = nullptr;
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_MONO;

    if (swr_alloc_set_opts2(&raw_swr,
                            &out_layout, AV_SAMPLE_FMT_FLT, TARGET_SAMPLE_RATE,
                            &codec_ctx->ch_layout, codec_ctx->sample_fmt, codec_ctx->sample_rate,
                            0, nullptr) < 0 || !raw_swr) {
        return std::unexpected("Failed to configure resampler");
    }
    SwrCtxPtr swr_ctx(raw_swr);

    if (swr_init(swr_ctx.get()) < 0) {
        return std::unexpected("Failed to initialize resampler");
    }

    // Allocate packet and frame
    PacketPtr pkt(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    if (!pkt || !frame) {
        return std::unexpected("Failed to allocate packet/frame");
    }

    // Decode loop: accumulate into chunks
    AudioChunk current_chunk;
    current_chunk.samples.reserve(CHUNK_SAMPLES);
    current_chunk.offset_ms = 0;
    int64_t total_samples_produced = 0;

    auto flush_chunk = [&]() {
        if (!current_chunk.samples.empty()) {
            on_chunk(std::move(current_chunk));
            total_samples_produced += static_cast<int64_t>(current_chunk.samples.size());
            current_chunk = AudioChunk{};
            current_chunk.samples.reserve(CHUNK_SAMPLES);
            current_chunk.offset_ms = total_samples_produced * 1000 / TARGET_SAMPLE_RATE;
        }
    };

    auto process_frame = [&]() {
        // Estimate output samples
        int out_samples = swr_get_out_samples(swr_ctx.get(), frame->nb_samples);
        if (out_samples <= 0) return;

        std::vector<float> resampled(out_samples);
        auto* out_ptr = reinterpret_cast<uint8_t*>(resampled.data());

        int converted = swr_convert(swr_ctx.get(),
                                    &out_ptr, out_samples,
                                    const_cast<const uint8_t**>(frame->extended_data),
                                    frame->nb_samples);
        if (converted <= 0) return;
        resampled.resize(converted);

        // Append to current chunk, flush when full
        size_t pos = 0;
        while (pos < resampled.size()) {
            auto space = static_cast<size_t>(CHUNK_SAMPLES) - current_chunk.samples.size();
            auto to_copy = std::min(space, resampled.size() - pos);
            current_chunk.samples.insert(current_chunk.samples.end(),
                                         resampled.begin() + static_cast<ptrdiff_t>(pos),
                                         resampled.begin() + static_cast<ptrdiff_t>(pos + to_copy));
            pos += to_copy;

            if (current_chunk.samples.size() >= static_cast<size_t>(CHUNK_SAMPLES)) {
                flush_chunk();
            }
        }
    };

    while (av_read_frame(fmt_ctx.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == audio_idx) {
            if (avcodec_send_packet(codec_ctx.get(), pkt.get()) >= 0) {
                while (avcodec_receive_frame(codec_ctx.get(), frame.get()) >= 0) {
                    process_frame();
                }
            }
        }
        av_packet_unref(pkt.get());
    }

    // Flush decoder
    avcodec_send_packet(codec_ctx.get(), nullptr);
    while (avcodec_receive_frame(codec_ctx.get(), frame.get()) >= 0) {
        process_frame();
    }

    // Flush resampler
    {
        int out_samples = swr_get_out_samples(swr_ctx.get(), 0);
        if (out_samples > 0) {
            std::vector<float> tail(out_samples);
            auto* out_ptr = reinterpret_cast<uint8_t*>(tail.data());
            int converted = swr_convert(swr_ctx.get(), &out_ptr, out_samples, nullptr, 0);
            if (converted > 0) {
                tail.resize(converted);
                current_chunk.samples.insert(current_chunk.samples.end(), tail.begin(), tail.end());
            }
        }
    }

    // Flush remaining samples
    flush_chunk();

    return {};
}

auto has_speech(std::span<const float> samples, float rms_threshold) -> bool {
    if (samples.empty()) return false;

    // Check 0.5-second sub-windows (8000 samples at 16kHz).
    // If ANY window exceeds the threshold, the chunk has speech.
    constexpr size_t FRAME_SIZE = 8000;

    for (size_t i = 0; i < samples.size(); i += FRAME_SIZE) {
        auto end = std::min(i + FRAME_SIZE, samples.size());
        double sum_sq = 0.0;
        for (size_t j = i; j < end; ++j) {
            sum_sq += static_cast<double>(samples[j]) * samples[j];
        }
        auto rms = static_cast<float>(std::sqrt(sum_sq / static_cast<double>(end - i)));
        if (rms > rms_threshold) return true;
    }
    return false;
}

auto get_media_duration_ms(const std::filesystem::path& media_path) -> Result<int64_t> {
    AVFormatContext* raw_fmt = nullptr;
    if (avformat_open_input(&raw_fmt, media_path.c_str(), nullptr, nullptr) < 0) {
        return std::unexpected("Failed to open media file");
    }
    FormatCtxPtr fmt_ctx(raw_fmt);

    if (avformat_find_stream_info(fmt_ctx.get(), nullptr) < 0) {
        return std::unexpected("Failed to find stream info");
    }

    if (fmt_ctx->duration <= 0) {
        return std::unexpected("Could not determine duration");
    }

    return fmt_ctx->duration / (AV_TIME_BASE / 1000);
}

} // namespace vsg
