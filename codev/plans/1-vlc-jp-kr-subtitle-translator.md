# Plan 1 — VLC Japanese-to-Korean Subtitle Translator (current-state)

Status: baseline (reverse-engineered from existing code on 2026-04-30)
Spec: `codev/specs/1-vlc-jp-kr-subtitle-translator.md`
Protocol: SPIR

## Purpose of this document

This is **not** an "implement from scratch" plan. The system already exists; this plan describes the implementation as it stands so that future SPIR cycles have a stable HOW reference to compare against. New work should be tracked under spec/plan numbers ≥ 2.

The map below is in spec → code form: for each spec requirement, where it lives in the tree.

---

## 1. Repository layout

```
vlc-jp-kr-translate/
├── vlc-extension/
│   └── vlc_jp_kr_translate.lua          ← VLC menu UI + HTTP client
├── backend/
│   ├── pyproject.toml                   ← packaging, `vlc-translate` entry point
│   ├── src/vlc_translate/
│   │   ├── main.py                      ← FastAPI app, router wiring, CORS
│   │   ├── config.py                    ← env-driven config (.env via dotenv)
│   │   ├── models.py                    ← SubtitleEntry, TranscriptionTask
│   │   ├── cli.py                       ← offline `vlc-translate` CLI
│   │   ├── __main__.py                  ← `python -m vlc_translate.cli`
│   │   ├── routes/
│   │   │   ├── health.py                ← GET /api/health
│   │   │   ├── subtitle.py              ← GET /api/translate-subtitle
│   │   │   └── audio.py                 ← GET /api/start-transcription, /api/transcription-status
│   │   └── services/
│   │       ├── subtitle_parser.py       ← ffprobe + ffmpeg + chardet + pysubs2
│   │       ├── transcriber.py           ← faster-whisper + task registry
│   │       ├── translator.py            ← claude -p / DeepL backends
│   │       └── srt_writer.py            ← SRT emission
│   └── tests/
│       ├── test_cli.py
│       ├── test_srt_writer.py
│       ├── test_subtitle_parser.py
│       └── test_translator.py
├── native/
│   ├── CMakeLists.txt                   ← whisper.cpp FetchContent + FFmpeg pkg-config + folly
│   ├── src/
│   │   ├── main.cpp                     ← arg parse + model search
│   │   ├── audio_decoder.{hpp,cpp}      ← FFmpeg C API → 16kHz mono float chunks
│   │   ├── transcriber.{hpp,cpp}        ← whisper.cpp wrapper (shared model + per-state)
│   │   ├── pipeline.{hpp,cpp}           ← folly coroutine pipeline (3 stages)
│   │   ├── srt_writer.{hpp,cpp}         ← SoA SubtitleStore → SRT
│   │   └── types.hpp                    ← AudioChunk, Segment, SubtitleStore, TimeMap
│   ├── benchmark/
│   │   └── bench_main.cpp               ← C++ vs Python timing comparison
│   └── models/                          ← user-downloaded ggml-*.bin
├── scripts/start.sh                     ← convenience launcher
├── install.sh                           ← symlinks the Lua extension into VLC
├── CLAUDE.md / AGENTS.md                ← AI assistant instructions
└── codev/                               ← specs, plans, reviews (this file)
```

---

## 2. Spec → implementation map

### F1 — Backend health
- Defined: `backend/src/vlc_translate/routes/health.py` (router only).
- Wired: `main.py:15` includes `health.router` under `/api`.
- Consumed: `vlc_jp_kr_translate.lua:241-248` (`check_backend_health`) — substring match on `"ok"` in the response.

### F2 — Subtitle-mode translation (synchronous)
End-to-end path:
1. **Trigger**: Lua menu item 1 → `translate_subtitles()` at `vlc_jp_kr_translate.lua:56`.
2. **Transport**: `vlc.stream(BACKEND_URL .. "/api/translate-subtitle?media_path=…")` at `vlc_jp_kr_translate.lua:84-99`. Note: `vlc.stream:readline()` blocks the VLC UI thread until the backend returns the full SRT path.
3. **Backend route**: `routes/subtitle.py:18-68`.
4. **Subtitle source resolution**:
   - If client passed `subtitle_path`, use it.
   - Else `extract_subtitle_track()` at `services/subtitle_parser.py:29` — ffprobe lists `s` streams; pick first `ja`/`jpn`/`japanese`, falling back to first subtitle stream; ffmpeg copies to a temp SRT.
5. **Parsing**: `parse_subtitles()` at `services/subtitle_parser.py:93` — `chardet.detect()` then `pysubs2.load(..., encoding=...)`. Skips `is_comment` events. Result: `list[SubtitleEntry(start_ms, end_ms, text)]`.
6. **Translation**: `translate_all()` at `services/translator.py:103` (see F4).
7. **Write**: `write_srt()` at `services/srt_writer.py:31` → `/tmp/vlc-translate/<media_stem>_<8-hex>_ko.srt` (hash from `media_path`, `routes/subtitle.py:60`).
8. **Response**: `{status, srt_path, subtitle_count}` at `routes/subtitle.py:64-68`.
9. **Load**: Lua calls `vlc.input.add_subtitle(srt_path)` at `vlc_jp_kr_translate.lua:110`.

Error mapping (per spec §6 F2):
- `extract_subtitle_track` returns `None` → 404 (`routes/subtitle.py:30-33`).
- `parse_subtitles` raises → 400 (`routes/subtitle.py:38`).
- `translate_all` raises → 500 (`routes/subtitle.py:48`).

### F3 — Audio-mode transcription (async, polled)
Two endpoints + an in-memory registry.

1. **Start**: `POST`-shaped over GET — `routes/audio.py:14-31`. Calls `create_transcription_task()` at `services/transcriber.py:129` (uuid4 hex truncated to 12 chars), then `asyncio.create_task(transcribe_and_translate(...))`. Returns `task_id` immediately.
2. **Background work**: `transcribe_and_translate()` at `services/transcriber.py:48`.
   - 0.1: `extract_audio()` at `services/transcriber.py:21` — ffmpeg → 16kHz mono PCM WAV in a temp dir.
   - 0.2 → 0.6: faster-whisper `WhisperModel(model, compute_type="int8")`, `transcribe(..., language="ja", beam_size=5, vad_filter=True)`.
   - 0.7: build `SubtitleEntry`s.
   - 0.9: `translate_all()`.
   - 1.0: `write_srt(..., f"{stem}_{task_id[:8]}_audio_ko.srt")`. Set `status="complete"`.
   - On any exception: `status="error"`, `error=str(e)`.
3. **Status**: `routes/audio.py:34-47` returns `{task_id, status, progress, srt_path, error}`.
4. **Registry**: module-level `_tasks: dict[str, TranscriptionTask]` at `services/transcriber.py:14`. **Lost on backend restart** (spec §3 non-goal, §9 open question 5).
5. **Client polling**: `poll_transcription()` at `vlc_jp_kr_translate.lua:167` — every 5s, up to 360 attempts (30 min wall clock).

### F4 — Translation backends
- Selector: `get_translator()` at `services/translator.py:97`, driven by `config.TRANSLATION_BACKEND` (env `TRANSLATION_BACKEND`, default `"claude"`).
- **`ClaudeCLITranslator`** at `services/translator.py:33-72`:
  - System prompt is the constant at `services/translator.py:9-26`.
  - Input format: `f"{i+1}: {t}"` per line.
  - Subprocess: `["claude", "-p"]`, stdin = prompt, 600s timeout.
  - Output parsing: `_parse_numbered_response()` at `services/translator.py:55` — uses the `N:` prefix to write back into a pre-sized list, so missing/reordered lines don't shift later entries (spec §7 "translation alignment").
- **`DeepLTranslator`** at `services/translator.py:75-94`:
  - `https://api-free.deepl.com/v2/translate`, `JA → KO`, `auth_key=DEEPL_API_KEY`.
  - `httpx.AsyncClient`, 30s timeout.
- **Batching**: `translate_all()` at `services/translator.py:103` — chunks of 50, `asyncio.Semaphore(2)` for concurrency, results merged in input order.

### F5 — Offline Python CLI
- Entry point: `pyproject.toml:23` registers `vlc-translate = "vlc_translate.cli:main"`.
- Two modes branched at `cli.py:214-219`:
  - `run_full_pipeline()` at `cli.py:92` — extract audio → faster-whisper directly (does not import `transcriber.py`'s task wrapper) → write `<stem>_ja.srt` → translate → write `<stem>_ko.srt`. Five `[N/5]` log lines on stderr; final SRT paths on stdout.
  - `run_translate_only()` at `cli.py:40` — pysubs2 → translate → write `<stem ending _ko>.srt`. Two `[N/2]` log lines on stderr.
- Flags: `-o/--output`, `-m/--model` (whisper), `-b/--backend {claude,deepl}`, `--srt`. Config is overridden by mutating `vlc_translate.config` module attributes **before** the services are imported (`cli.py:58-59, 111-113`) — this ordering matters because `services/translator.py` reads `config.TRANSLATION_BACKEND` at call time; do not move the imports above the config writes.

### F6 — Native C++23 CLI
- Entry: `native/src/main.cpp:92`. Arg parsing at `main.cpp:30-88`; default model search order at `main.cpp:67-71` (`models/ggml-medium.bin` cwd, exe-relative, exe-parent-relative).
- Pipeline: `pipeline.cpp:165` `run_pipeline()`.
  - **Stage 1**: `decode_and_load_model()` at `pipeline.cpp:42` — `folly::coro::collectAll` runs decode + model load on a 2-thread `CPUThreadPoolExecutor`. Decode pushes 30s `AudioChunk`s into a vector via callback; model load returns a `Transcriber`.
  - **Stage 2**: `transcribe_chunks()` at `pipeline.cpp:109` — **single state, sequential loop**. The comment at `pipeline.cpp:103-107` records the reason: concurrent `whisper_full_with_state` calls on Apple Metal silently produce empty output (commits `4b33c24`, `e72f1b7`). `n_threads = std::thread::hardware_concurrency()` is passed *into* whisper, so Metal still parallelizes internally. **Do not regress this without re-testing on Metal.**
  - **Stage 3**: `build_subtitle_store()` at `pipeline.cpp:142` trims whitespace and pushes into the SoA `SubtitleStore`; `write_srt()` writes `<stem>_ja.srt`.
- Build: `native/CMakeLists.txt`. whisper.cpp via FetchContent (`v1.7.5`, `GGML_METAL=ON`); FFmpeg via `pkg-config` (`libavformat`, `libavcodec`, `libavutil`, `libswresample`); folly via `find_package(folly CONFIG)`. The `CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES` workaround at `CMakeLists.txt:34-43` exists because folly's CMake config emits `-isystem <SDK>/usr/include`, which breaks libc++ wrapper headers on macOS — leave it in place.
- Benchmark: `bench_main` target shares all pipeline sources, defined at `CMakeLists.txt:70-90`.
- **Translation step is not implemented in the native CLI** — this is the only F6→spec gap and is captured as spec §9 open question 1.

---

## 3. Configuration surface

| Knob | Source | Default | Read at |
|---|---|---|---|
| `TRANSLATION_BACKEND` | env / `.env` | `claude` | `services/translator.py:97` |
| `DEEPL_API_KEY` | env / `.env` | `""` | `services/translator.py:79` |
| `WHISPER_MODEL` | env / `.env` | `medium` | `services/transcriber.py:74`, `cli.py:135` |
| `BACKEND_PORT` | env / `.env` | `8765` | unused at runtime — uvicorn gets `--port` from CLI; the constant exists for symmetry |
| `OUTPUT_DIR` | hardcoded | `/tmp/vlc-translate` | `config.py:13` (mkdir at import time) |
| `BACKEND_URL` | Lua local | `http://localhost:8765` | `vlc_jp_kr_translate.lua:4`, mutable via Settings dialog |
| Whisper model path (native) | CLI `-m` or search | `models/ggml-medium.bin` | `native/src/main.cpp:67` |

`config.py` calls `OUTPUT_DIR.mkdir(parents=True, exist_ok=True)` at import time — this is what guarantees `/tmp/vlc-translate/` exists before any route writes. Don't move it without finding another mkdir site.

---

## 4. Data shapes

`models.py:`
- `SubtitleEntry(start_ms: int, end_ms: int, text: str)` — the lingua franca between parser, transcriber, translator, and writer. Times are integer milliseconds; `srt_writer.ms_to_srt_time` is the only formatter.
- `TranscriptionTask(task_id, status: "running"|"complete"|"error", progress: float, srt_path: str|None, error: str|None)` — mutated in place by the background coroutine.

Native (`native/src/types.hpp`):
- `AudioChunk` — 16kHz mono float samples + `offset_ms` + computed `duration_ms()`.
- `Segment` — `{start_ms, end_ms, text}`.
- `SubtitleStore` — Structure-of-Arrays of segments; `push_back(start, end, text)` appends.

---

## 5. Test coverage (current)

- `tests/test_cli.py`
- `tests/test_srt_writer.py`
- `tests/test_subtitle_parser.py`
- `tests/test_translator.py`

Run: `cd backend && source .venv/bin/activate && python -m pytest tests/ -v` (per CLAUDE.md). Native side has no automated tests — `bench_main` is timing-only and requires a real video + model.

---

## 6. Known deltas between docs and code

These are recorded so future readers don't get misled:

1. **CLAUDE.md** still describes the native pipeline as "parallel transcription — one whisper state per core, chunks distributed round-robin (`folly::coro::collectAllRange` …)". `pipeline.cpp` no longer does this; it runs a single state sequentially because of the Apple Metal contention fix (commits `4b33c24` "Fix empty transcription: cap workers to avoid Metal GPU contention" and `e72f1b7` "Fix Metal GPU contention: use single state for transcription"). Either CLAUDE.md should be updated, or the parallel path should be restored behind a non-Metal guard. Tracked as a follow-up, not part of this baseline plan.
2. **`BACKEND_PORT`** in `config.py` is read into a constant but never referenced. The actual port comes from the uvicorn CLI invocation in CLAUDE.md / `scripts/start.sh`. Either wire it through or delete it.
3. **`append_srt`** in `srt_writer.py:36-47` exists but has no callers (a relic from a streaming-mode draft). Safe to delete in a maintenance pass.

These are cataloged here, not changed, because this plan is a snapshot — fixes should land under their own spec/plan numbers.

---

## 7. How future SPIR cycles should extend this

Each spec §9 open question is a candidate next cycle. Suggested first issues:

| Spec Q | Suggested protocol | Touches |
|---|---|---|
| Q1 — translation in native CLI | SPIR | `native/src/pipeline.cpp`, new `translator.{hpp,cpp}` (likely shells out to `claude -p` or POSTs to backend) |
| Q2 — whisper model default | EXPERIMENT | `config.py`, `cli.py`, native `main.cpp` model search |
| Q3 — SRT cache vs. reap | SPIR | `services/srt_writer.py`, possibly a hash-keyed lookup before re-translating |
| Q4 — Lua UI blocking on subtitle-mode | SPIR | `vlc_jp_kr_translate.lua`, possibly a new `routes/subtitle.py` async/poll variant mirroring audio-mode |
| Q5 — task persistence across restarts | SPIR | `services/transcriber.py` `_tasks` dict → SQLite/JSON file under `OUTPUT_DIR` |

Pre-flight before any new builder:
1. Add a GitHub Issue.
2. Allocate the next sequential number (next is 2).
3. Spec, plan, review under matching numbers.
4. Builders branch from a clean main; any uncommitted spec/plan files must be committed first.

---

## 8. Acceptance for "this baseline plan is correct"

This plan is correct if a reader can:
- For every spec line item, find the file:line referenced here and see the behavior.
- Reproduce a working build of all three components from CLAUDE.md alone (i.e., the spec/plan don't reveal hidden setup steps).
- Run `pytest -v` in `backend/` green.
- Build `native/build/vlc-subtitle-gen` and `native/build/bench_main` against a downloaded ggml model.

No code changes are part of this plan.
