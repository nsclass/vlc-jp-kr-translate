#include "srt_writer.hpp"

#include <fstream>
#include <format>

namespace vsg {

auto write_srt(const SubtitleStore& store, const std::filesystem::path& path) -> VoidResult {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return std::unexpected("Failed to open output file: " + path.string());
    }

    // Pre-size output buffer for efficiency
    std::string buffer;
    buffer.reserve(store.count() * 80);  // ~80 chars per entry estimate

    for (size_t i = 0; i < store.count(); ++i) {
        buffer += std::format("{}\n{} --> {}\n{}\n\n",
                              i + 1,
                              format_srt_time(store.start_ms[i]),
                              format_srt_time(store.end_ms[i]),
                              store.texts[i]);
    }

    out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (!out.good()) {
        return std::unexpected("Failed to write SRT file: " + path.string());
    }

    return {};
}

} // namespace vsg
