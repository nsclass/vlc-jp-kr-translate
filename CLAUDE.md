# VLC JP-KR Subtitle Translator

VLC plugin that translates Japanese subtitles/audio to Korean in real-time.

## Architecture

Two components communicating over HTTP (localhost:8765):

- **VLC Lua Extension** (`vlc-extension/vlc_jp_kr_translate.lua`) — UI inside VLC, triggers translation, loads Korean SRT
- **Python FastAPI Backend** (`backend/`) — subtitle parsing, translation via `claude -p`, Whisper transcription

## Prerequisites

- Python 3.11+
- VLC 3.0+
- FFmpeg (for subtitle extraction and audio processing)
- Claude Code CLI (`claude` command available in PATH)

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

## Running Tests

```bash
cd backend
source .venv/bin/activate
python -m pytest tests/ -v
```

## Project Structure

```
backend/src/vlc_translate/
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
```

## Configuration

Copy `backend/.env.example` to `backend/.env`. Translation defaults to `claude -p` CLI (no API key needed). Set `TRANSLATION_BACKEND=deepl` with a `DEEPL_API_KEY` for DeepL fallback.
