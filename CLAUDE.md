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

The native CLI decodes audio via FFmpeg C API, applies energy-based VAD (Voice Activity Detection) to skip silence, and transcribes speech using whisper.cpp with Metal GPU acceleration. Uses folly coroutines for structured concurrency (concurrent audio decode + model loading).

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

- **audio_decoder** — FFmpeg C API streaming decode to 16kHz mono float, 30s chunks, energy-based VAD filtering
- **transcriber** — whisper.cpp wrapper with beam search, single-pass transcription
- **pipeline** — folly::coro::Task stages: decode+VAD || model load (concurrent via collectAll), then transcribe, then write SRT
- **srt_writer** — bulk SRT output with SoA (Structure of Arrays) subtitle store
- **types** — AudioChunk, Segment, SubtitleStore (SoA), TimeMap (VAD timestamp remapping)

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
