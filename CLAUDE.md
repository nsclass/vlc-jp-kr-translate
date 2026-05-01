# VLC JP-KR Subtitle Translator

VLC plugin that translates Japanese subtitles/audio to Korean in real-time.

## Architecture

Three components:

- **VLC Lua Extension** (`vlc-extension/vlc_jp_kr_translate.lua`) — UI inside VLC, triggers translation, loads Korean SRT
- **Python FastAPI Backend** (`backend/`) — subtitle parsing, translation via `claude -p`, Whisper transcription
- **C++23 Native CLI** (`native/`) — high-performance subtitle generation using whisper.cpp + FFmpeg C API + folly structured concurrency

## Prerequisites

- Python 3.11+
- VLC 3.0+
- FFmpeg (for subtitle extraction and audio processing)
- Claude Code CLI (`claude` command available in PATH)

### Native CLI additional prerequisites

- CMake 3.24+
- C++23 compiler (Apple Clang 15+ or equivalent)
- folly (`brew install folly`)
- pkg-config (`brew install pkg-config`)

## Setup

```bash
# 1. Install Python dependencies
cd backend
python3 -m venv .venv
source .venv/bin/activate
pip install -e ".[dev]"

# 2. Install VLC Lua extension (symlinks into VLC extensions dir)
cd ..
bash install.sh
```

## Running

```bash
# Start the backend server
cd backend
source .venv/bin/activate
uvicorn vlc_translate.main:app --port 8765

# Then open VLC and play a Japanese movie
# Go to: View > JP-KR 자막 번역기
#   - "자막 번역 (Translate Subtitles)" — translates embedded/external Japanese subtitles
#   - "음성 인식 시작 (Start Audio Transcription)" — transcribes Japanese audio with Whisper, then translates
```

## Offline CLI Tool (Python)

Generate Japanese and Korean SRT files from a video file ahead of time, then load them into VLC.

```bash
cd backend
source .venv/bin/activate

# Basic usage — outputs movie_ja.srt and movie_ko.srt next to the input file
vlc-translate /path/to/japanese-movie.mp4

# Specify output directory
vlc-translate /path/to/movie.mp4 -o /tmp/subtitles

# Use a different Whisper model (tiny/base/small/medium/large-v3)
vlc-translate /path/to/movie.mp4 -m large-v3

# Use DeepL instead of Claude CLI for translation
vlc-translate /path/to/movie.mp4 -b deepl

# Also works as a module
python -m vlc_translate.cli /path/to/movie.mp4 --help
```

Then load the generated SRT files in VLC: Subtitle > Add Subtitle File.

## Native CLI (C++23) — High-Performance Subtitle Generation

The native CLI decodes audio via FFmpeg C API and transcribes using whisper.cpp with parallel chunked transcription across all CPU cores. Uses folly coroutines for structured concurrency:
- Stage 1: Concurrent audio decode + model loading (folly::coro::collectAll)
- Stage 2: Parallel transcription — one whisper state per core, chunks distributed round-robin (folly::coro::collectAllRange on CPUThreadPoolExecutor with hardware_concurrency threads)
- Stage 3: Merge segments and write SRT

This approach follows the faster-whisper pattern: shared model weights with per-worker compute state, maximizing all available CPU cores without needing VAD silence filtering.

### Building

```bash
cd native

# Download whisper model (one-time)
mkdir -p models
curl -L -o models/ggml-medium.bin \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.bin

# Build (whisper.cpp is fetched automatically via CMake FetchContent)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build build -j$(sysctl -n hw.ncpu)
```

### Running

```bash
# Generate Japanese SRT from video
./build/vlc-subtitle-gen /path/to/movie.mp4

# Specify output directory and model path
./build/vlc-subtitle-gen /path/to/movie.mp4 -o /tmp/subs -m models/ggml-medium.bin
```

### Benchmarking

```bash
./build/bench_main /path/to/movie.mp4 models/ggml-medium.bin
```

Compares C++ pipeline performance against Python baseline, showing per-stage timing and speedups.

### Native Architecture

- **audio_decoder** — FFmpeg C API streaming decode to 16kHz mono float, 30s chunks
- **transcriber** — whisper.cpp wrapper: shared model (no_state), per-worker state for parallel transcription
- **pipeline** — folly::coro::Task stages: decode || model load (collectAll), then parallel chunk transcription (collectAllRange, thread pool = hardware_concurrency)
- **srt_writer** — bulk SRT output with SoA (Structure of Arrays) subtitle store
- **types** — AudioChunk, Segment, SubtitleStore (SoA), TimeMap

## Running Tests

```bash
cd backend
source .venv/bin/activate
python -m pytest tests/ -v
```

## Project Structure

```
backend/src/vlc_translate/
  cli.py               — Offline CLI: MP4 -> Japanese SRT -> Korean SRT
  __main__.py          — python -m vlc_translate.cli entry point
  main.py              — FastAPI app
  config.py            — environment config
  models.py            — SubtitleEntry, TranscriptionTask dataclasses
  routes/
    health.py          — GET /api/health
    subtitle.py        — GET /api/translate-subtitle
    audio.py           — GET /api/start-transcription, GET /api/transcription-status
  services/
    subtitle_parser.py — FFmpeg extraction + pysubs2 parsing (handles Shift-JIS/EUC-JP)
    translator.py      — ClaudeCLITranslator (claude -p) and DeepLTranslator backends
    transcriber.py     — faster-whisper Japanese ASR + translation pipeline
    srt_writer.py      — SRT file generation

native/
  CMakeLists.txt       — Build system (whisper.cpp FetchContent, FFmpeg pkg-config, folly)
  src/
    main.cpp           — CLI with arg parsing and model path search
    audio_decoder.cpp  — FFmpeg C API decode + VAD filtering
    transcriber.cpp    — whisper.cpp transcription wrapper
    pipeline.cpp       — folly coroutine pipeline (decode||load -> transcribe -> write)
    srt_writer.cpp     — SRT file output
    types.hpp          — Core types: AudioChunk, Segment, SubtitleStore, TimeMap
  benchmark/
    bench_main.cpp     — Performance comparison vs Python baseline
```

## Configuration

Copy `backend/.env.example` to `backend/.env`. Translation defaults to `claude -p` CLI (no API key needed). Set `TRANSLATION_BACKEND=deepl` with a `DEEPL_API_KEY` for DeepL fallback.

## Codev — AI-Assisted Development

This project uses **Codev** for AI-assisted development.

### Available Protocols

- **SPIR**: Multi-phase development with consultation (`codev/protocols/spir/protocol.md`)
- **ASPIR**: Autonomous SPIR — no human gates on spec/plan (`codev/protocols/aspir/protocol.md`)
- **AIR**: Autonomous Implement & Review for small features (`codev/protocols/air/protocol.md`)
- **BUGFIX**: Bug fixes from GitHub issues (`codev/protocols/bugfix/protocol.md`)
- **EXPERIMENT**: Disciplined experimentation (`codev/protocols/experiment/protocol.md`)
- **MAINTAIN**: Codebase maintenance (`codev/protocols/maintain/protocol.md`)
- **RESEARCH**: Multi-agent research with 3-way investigation, synthesis, and critique (`codev/protocols/research/protocol.md`)

### Key Locations

- **Specs**: `codev/specs/` - Feature specifications (WHAT to build)
- **Plans**: `codev/plans/` - Implementation plans (HOW to build)
- **Reviews**: `codev/reviews/` - Reviews and lessons learned
- **Protocols**: `codev/protocols/` - Development protocols

### Quick Start

1. For new features, start with the Specification phase
2. Create exactly THREE documents per feature: spec, plan, and review
3. Follow the protocol phases as defined in the protocol files
4. Use multi-agent consultation when specified

### File Naming Convention

Use sequential numbering with descriptive names:
- Specification: `codev/specs/1-feature-name.md`
- Plan: `codev/plans/1-feature-name.md`
- Review: `codev/reviews/1-feature-name.md`

### Git Workflow

**NEVER use `git add -A` or `git add .`** - Always add files explicitly.

Commit messages format:
```
[Spec 1] Description of change
[Spec 1][Phase: implement] feat: Add feature
```

### CLI Commands

Codev provides three CLI tools:

- **codev**: Project management (init, adopt, update, doctor)
- **afx**: Agent Farm orchestration (start, spawn, status, cleanup)
- **consult**: AI consultation for reviews (general, protocol, stats)

For complete reference, see `codev/resources/commands/`:
- `codev/resources/commands/overview.md` - Quick start
- `codev/resources/commands/codev.md` - Project commands
- `codev/resources/commands/agent-farm.md` - Agent Farm commands
- `codev/resources/commands/consult.md` - Consultation commands

### Codev Configuration

Agent Farm is configured via `.codev/config.json` at the project root. Created during `codev init` or `codev adopt`. Override via CLI flags: `--architect-cmd`, `--builder-cmd`, `--shell-cmd`.

```json
{
  "shell": {
    "architect": "claude",
    "builder": "claude",
    "shell": "bash"
  }
}
```

For more info, read the full protocol documentation in `codev/protocols/`.
