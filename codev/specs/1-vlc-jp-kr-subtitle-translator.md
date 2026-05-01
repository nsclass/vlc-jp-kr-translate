# Spec 1 — VLC Japanese-to-Korean Subtitle Translator

Status: baseline (reverse-engineered from existing code on 2026-04-30)
Owner: nsclass
Protocol: SPIR

## 1. Problem

A Japanese-speaking video has either embedded/external Japanese subtitles or only audio. A Korean-speaking viewer needs Korean subtitles in VLC without manually running a translation pipeline.

The system must work with arbitrary local media (mp4/mkv), produce a Korean SRT, and load it back into the running VLC instance. It must also be usable offline as a CLI for batch generation.

## 2. Goals

- **In-VLC translation**: A user playing a Japanese video can pick a menu item in VLC and, after a wait, get a Korean subtitle track loaded automatically.
- **Two input modes**:
  - **Subtitle mode** — extract an embedded/external JP subtitle track and translate it.
  - **Audio mode** — transcribe Japanese audio via Whisper, then translate.
- **Offline batch CLI** — generate `<name>_ja.srt` and `<name>_ko.srt` from a video file without VLC running.
- **High-performance native path** — a C++23 CLI that decodes audio with FFmpeg and transcribes with whisper.cpp for users who want subtitle generation faster than the Python path.
- **No paid API required by default** — translation goes through the local `claude -p` CLI; DeepL is optional.

## 3. Non-goals

- Live, sentence-by-sentence streaming translation while playback continues. The current design is "request → wait → load complete SRT".
- Translation of languages other than Japanese → Korean.
- A hosted/multi-user service. The backend is a local single-user FastAPI server bound to localhost.
- Speaker diarization, subtitle restyling (font/color), or burning subtitles into video.
- Persisting transcription tasks across backend restarts (task registry is in-memory).

## 4. Users & UX

### Primary user: VLC viewer
1. Starts the backend (`uvicorn vlc_translate.main:app --port 8765`).
2. Plays a Japanese video in VLC.
3. Opens **View → JP→KR 자막 번역기** and picks one of:
   - **자막 번역 (Translate Subtitles)** — for media with a JP subtitle track.
   - **음성 인식 시작 (Start Audio Transcription)** — for media with only Japanese audio.
   - **설정 (Settings)** — change the backend URL.
4. A progress dialog appears. On completion, the Korean SRT is auto-added to the playing media's subtitle tracks; user picks it from the subtitle menu.

### Secondary user: CLI batch user
- `vlc-translate movie.mp4` → produces `movie_ja.srt` and `movie_ko.srt` next to the input.
- `vlc-translate --srt subs_ja.srt` → translation only, skip transcription.
- `./build/vlc-subtitle-gen movie.mp4` → native C++ path that produces only `movie_ja.srt` (translation is not yet wired into the native CLI).

## 5. System architecture

Three loosely coupled components, communicating only via HTTP and on-disk SRT paths.

```
┌─────────────────────┐    HTTP    ┌──────────────────────┐
│  VLC Lua Extension  │ ─────────► │  FastAPI Backend     │
│  (vlc-extension/)   │            │  (backend/)          │
│                     │ ◄───────── │                      │
└─────────────────────┘  srt_path  │   ┌────────────────┐ │
         │                         │   │ subtitle_parser│ │
         │ vlc.input.add_subtitle  │   │ translator     │ │
         │                         │   │ transcriber    │ │
         ▼                         │   │ srt_writer     │ │
   /tmp/vlc-translate/*.srt        │   └────────────────┘ │
                                   └──────────────────────┘
                                           │
                                           ├── ffmpeg (extract subs/audio)
                                           ├── faster-whisper (JP ASR)
                                           └── claude -p  /  DeepL  (translate)

┌──────────────────────────────────────┐
│  Native C++23 CLI (native/)          │  Standalone — no backend dependency.
│  FFmpeg C API + whisper.cpp + folly  │  Produces JA SRT only.
└──────────────────────────────────────┘
```

### 5.1 VLC Lua extension (`vlc-extension/vlc_jp_kr_translate.lua`)
- Registers a menu under VLC's View menu.
- Calls backend over HTTP using `vlc.stream`.
- For audio mode, polls `/api/transcription-status` every 5s, up to 30 minutes.
- Loads returned `srt_path` via `vlc.input.add_subtitle`.

### 5.2 FastAPI backend (`backend/src/vlc_translate/`)
- `main.py` — wires three routers under `/api`.
- `routes/health.py` — `GET /api/health`.
- `routes/subtitle.py` — `GET /api/translate-subtitle?media_path=…[&subtitle_path=…]`. Synchronous; returns `{srt_path, subtitle_count}`.
- `routes/audio.py` — `GET /api/start-transcription?media_path=…&mode=batch` and `GET /api/transcription-status?task_id=…`. Asynchronous; spawns a background task and tracks state in an in-memory dict.
- `services/subtitle_parser.py` — ffprobe to find JP track (`ja`/`jpn`/`japanese`), ffmpeg to extract to SRT, chardet + pysubs2 to parse (handles Shift-JIS/EUC-JP). Falls back to first subtitle stream if no JP tag.
- `services/transcriber.py` — extracts audio to 16kHz mono PCM via ffmpeg, runs faster-whisper with `language="ja"`, `vad_filter=True`, `compute_type="int8"`, then translates and writes SRT.
- `services/translator.py` — `ClaudeCLITranslator` (default, shells out to `claude -p` with a structured numbered-line prompt) or `DeepLTranslator` (HTTP). Translation is batched (size 50) with concurrency 2 and a numbered-line round-trip format that preserves ordering.
- `services/srt_writer.py` — emits SRT to `/tmp/vlc-translate/`.
- `models.py` — `SubtitleEntry(start_ms, end_ms, text)` and `TranscriptionTask(task_id, status, progress, srt_path, error)`.
- `config.py` — env-driven (`TRANSLATION_BACKEND`, `WHISPER_MODEL`, `BACKEND_PORT`, `DEEPL_API_KEY`).

### 5.3 Python offline CLI (`backend/src/vlc_translate/cli.py`)
- Reuses the same services. Two modes:
  - Full pipeline: video → JA SRT (whisper) → KO SRT (translator).
  - `--srt` mode: existing JA SRT → KO SRT.
- Outputs to input directory by default; `-o` overrides.

### 5.4 Native C++23 CLI (`native/`)
- Three-stage folly coroutine pipeline:
  1. Concurrent audio decode (FFmpeg C API → 16kHz mono float, 30s chunks) and whisper model load (`folly::coro::collectAll`).
  2. Transcription. **Note:** despite the description in CLAUDE.md, the current code in `pipeline.cpp` runs transcription **sequentially on a single whisper state** — recent commits ("Fix Metal GPU contention") deliberately collapsed the parallel path because concurrent `whisper_full_with_state` calls produced empty results on Apple Metal. CLAUDE.md describes the original intent; `pipeline.cpp` is authoritative.
  3. Build `SubtitleStore` (SoA), trim whitespace, write SRT.
- Currently produces JA SRT only — translation is not yet integrated into the native binary.
- Includes a benchmark binary (`bench_main`) that compares against the Python pipeline.

## 6. Functional requirements

### F1. Backend health
`GET /api/health` returns a JSON body that contains the substring `ok`. The Lua extension uses this to decide whether to show a "start the server" error before kicking off any real work.

### F2. Subtitle-mode translation
Given a `media_path` (file path or `file://` URI):
1. ffprobe lists subtitle streams; pick the first stream tagged `ja`/`jpn`/`japanese`, else the first subtitle stream.
2. Extract that stream to a temp SRT via ffmpeg.
3. Parse with pysubs2, after detecting encoding via chardet (subs may be Shift-JIS / EUC-JP / UTF-8).
4. Translate all entries via the configured translator.
5. Write `<media_stem>_<8-hex>_ko.srt` to `/tmp/vlc-translate/`.
6. Return `{status: "ok", srt_path, subtitle_count}`.

If no subtitle track is found → 404. If parsing fails → 400. If translation fails → 500.

### F3. Audio-mode transcription (async)
`POST`-style flow done over `GET` (Lua extension limitation):
1. `start-transcription` creates a `task_id`, returns it immediately, kicks off background work.
2. Background work: extract audio → whisper transcribe (medium model, int8) → translate → write `<media_stem>_<task_id_prefix>_audio_ko.srt`.
3. `transcription-status` returns `{status, progress, srt_path, error}`. `status ∈ {running, complete, error}`. `progress` is a float 0..1 advanced at fixed checkpoints (0.1, 0.2, 0.6, 0.7, 0.9, 1.0).
4. The Lua client polls every 5s for up to 30 minutes.

### F4. Translation backends
- `claude` (default): shells out to `claude -p` with a fixed Japanese-to-Korean subtitle system prompt. Inputs are formatted as numbered lines `N: <text>`; outputs are parsed by matching numeric prefixes back to indices, so dropped/reordered lines do not corrupt alignment.
- `deepl`: posts to `https://api-free.deepl.com/v2/translate` with `JA` → `KO`. Requires `DEEPL_API_KEY`.
- Batches of 50 entries, max concurrency 2.

### F5. Offline CLI
- `vlc-translate <video>` runs the full pipeline; `--srt <ja.srt>` runs translation only. Backend selectable via `-b {claude,deepl}`. Whisper model selectable via `-m`.
- Stderr gets human progress (`[1/5] ...`); stdout gets the final SRT path(s) so callers can pipe them.

### F6. Native CLI
- `vlc-subtitle-gen <video> [-o dir] [-m model.bin]` runs the C++ pipeline and prints the JA SRT path to stdout, with timing breakdown on stderr.
- Searches for a default model at `models/ggml-medium.bin` next to the binary or in its parent.

## 7. Non-functional requirements

- **Localhost-only by default.** Backend binds to a local port; no auth. CORS is wide open intentionally to allow the VLC Lua HTTP client.
- **No long-lived state.** The SRT files in `/tmp/vlc-translate/` are the only persistent output; transcription tasks live only in process memory and are lost on backend restart.
- **Encoding robustness.** External JP subtitle files are commonly Shift-JIS or EUC-JP, not UTF-8. The parser must detect and handle these without corrupting characters.
- **Translation alignment.** A translator that returns a different number of lines must not silently shift later lines. The numbered-line round trip is the mechanism that enforces this.
- **GPU contention.** On Apple Metal, the native pipeline uses a single whisper state; do not regress this without testing on Metal hardware.

## 8. Out of scope (explicit)

- Translation in the native C++ CLI.
- Streaming/incremental subtitle delivery during playback.
- Speaker labels, profanity filtering, terminology glossaries.
- Anything other than `ja → ko`.

## 9. Open questions

1. Should the native CLI gain a translation step (calling `claude -p` itself or POSTing to the FastAPI backend) so it can be a drop-in replacement for the Python pipeline?
2. Whisper model size is currently `medium` everywhere — is that the right default given the `large-v3` vs. `medium` quality/speed trade-off for movie-length audio?
3. Should `/tmp/vlc-translate/` be reaped on backend startup, or do we want to keep generated SRTs around as a cache keyed by media hash?
4. The Lua extension blocks the VLC UI thread on `vlc.stream:readline()`. For long subtitle tracks, is this acceptable or does it need to move to the same task-poll pattern that audio mode uses?
5. Should the in-memory `_tasks` registry be replaced with on-disk persistence so a backend restart during a long transcription doesn't strand the user?

## 10. Acceptance criteria for "the system works"

- A user can start the backend, play a Japanese mkv with embedded JP subs in VLC, click **자막 번역**, wait, and see a Korean subtitle track auto-appear in VLC's subtitle menu.
- A user can play a Japanese mp4 with no subtitle tracks, click **음성 인식 시작**, and after the transcribe+translate window the Korean track auto-appears.
- `vlc-translate movie.mp4` produces `movie_ja.srt` and `movie_ko.srt` and exits 0.
- `./build/vlc-subtitle-gen movie.mp4` produces `movie_ja.srt` and exits 0.
- `pytest -v` in `backend/` passes.
