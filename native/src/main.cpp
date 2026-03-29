#include "pipeline.hpp"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct CliArgs {
    std::filesystem::path input;
    std::filesystem::path output_dir;
    std::filesystem::path model_path;
};

void print_usage(std::string_view program) {
    std::cerr << std::format(
        "Usage: {} <input.mp4> [options]\n\n"
        "Generate Japanese SRT subtitles from a video file using whisper.cpp.\n\n"
        "Options:\n"
        "  -o, --output <dir>    Output directory (default: same as input file)\n"
        "  -m, --model <path>    Path to whisper GGML model file\n"
        "  -h, --help            Show this help message\n",
        program);
}

auto parse_args(int argc, char* argv[]) -> std::expected<CliArgs, std::string> {
    if (argc < 2) {
        return std::unexpected("No input file specified");
    }

    CliArgs args;
    std::vector<std::string_view> positional;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            args.output_dir = argv[++i];
        } else if ((arg == "-m" || arg == "--model") && i + 1 < argc) {
            args.model_path = argv[++i];
        } else if (arg[0] != '-') {
            positional.emplace_back(arg);
        } else {
            return std::unexpected(std::format("Unknown option: {}", arg));
        }
    }

    if (positional.empty()) {
        return std::unexpected("No input file specified");
    }
    args.input = positional[0];

    if (args.output_dir.empty()) {
        args.output_dir = args.input.parent_path();
    }

    // Default model path: look in standard locations
    if (args.model_path.empty()) {
        auto exe_dir = std::filesystem::path(argv[0]).parent_path();
        std::vector<std::filesystem::path> search_paths = {
            "models/ggml-medium.bin",
            exe_dir / "models" / "ggml-medium.bin",
            exe_dir.parent_path() / "models" / "ggml-medium.bin",
        };
        for (const auto& p : search_paths) {
            if (std::filesystem::exists(p)) {
                args.model_path = p;
                break;
            }
        }
        if (args.model_path.empty()) {
            return std::unexpected(
                "No whisper model found. Download one with:\n"
                "  curl -L -o models/ggml-medium.bin "
                "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.bin\n"
                "Or specify a path with -m <model_path>");
        }
    }

    return args;
}

} // namespace

int main(int argc, char* argv[]) {
    auto args_result = parse_args(argc, argv);
    if (!args_result) {
        std::cerr << "Error: " << args_result.error() << "\n\n";
        print_usage(argv[0]);
        return 1;
    }

    auto& args = *args_result;

    std::cerr << std::format("Input:  {}\n", args.input.string());
    std::cerr << std::format("Output: {}\n", args.output_dir.string());
    std::cerr << std::format("Model:  {}\n\n", args.model_path.string());

    vsg::PipelineConfig config{
        .input_path = args.input,
        .output_dir = args.output_dir,
        .model_path = args.model_path,
    };

    auto result = vsg::run_pipeline(config);
    if (!result) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    std::cerr << std::format(
        "\nDone! {} segments written to: {}\n\n"
        "Performance:\n"
        "  Audio decode:    {:.2f}s\n"
        "  Transcription:   {:.2f}s\n"
        "  Total:           {:.2f}s\n",
        result->store.count(),
        result->srt_path.string(),
        result->decode_seconds,
        result->transcribe_seconds,
        result->total_seconds);

    // Print output path to stdout for scripting
    std::cout << result->srt_path.string() << "\n";
    return 0;
}
