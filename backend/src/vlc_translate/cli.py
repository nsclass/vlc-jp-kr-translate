"""Offline CLI tool: MP4 → Japanese SRT → Korean SRT.

Usage:
    python -m vlc_translate.cli <input.mp4> [options]
    vlc-translate <input.mp4> [options]
"""

import argparse
import asyncio
import shutil
import sys
from pathlib import Path


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate Japanese and Korean subtitles from a video file.",
    )
    parser.add_argument("input", help="Path to MP4/MKV media file")
    parser.add_argument(
        "-o", "--output",
        help="Output directory (default: same directory as input file)",
    )
    parser.add_argument(
        "-m", "--model",
        default="medium",
        help="Whisper model size: tiny, base, small, medium, large-v3 (default: medium)",
    )
    parser.add_argument(
        "-b", "--backend",
        default="claude",
        choices=["claude", "deepl"],
        help="Translation backend (default: claude, uses `claude -p` CLI)",
    )
    args = parser.parse_args()

    input_path = Path(args.input).resolve()
    if not input_path.exists():
        log(f"Error: File not found: {input_path}")
        sys.exit(1)

    if not shutil.which("ffmpeg"):
        log("Error: ffmpeg is not installed or not in PATH.")
        sys.exit(1)

    output_dir = Path(args.output).resolve() if args.output else input_path.parent
    output_dir.mkdir(parents=True, exist_ok=True)

    stem = input_path.stem
    ja_srt_path = output_dir / f"{stem}_ja.srt"
    ko_srt_path = output_dir / f"{stem}_ko.srt"

    # Override config before importing services
    import vlc_translate.config as cfg
    cfg.WHISPER_MODEL = args.model
    cfg.TRANSLATION_BACKEND = args.backend

    from vlc_translate.models import SubtitleEntry
    from vlc_translate.services.transcriber import extract_audio
    from vlc_translate.services.translator import translate_all
    from vlc_translate.services.srt_writer import write_srt_to

    # Step 1: Extract audio
    log("[1/5] Extracting audio from video...")
    audio_path = extract_audio(str(input_path))
    if not audio_path:
        log("Error: Failed to extract audio. Is the file a valid video?")
        sys.exit(1)

    # Step 2: Transcribe Japanese with Whisper
    log(f"[2/5] Transcribing Japanese audio (model: {args.model})...")
    try:
        from faster_whisper import WhisperModel
    except ImportError:
        log("Error: faster-whisper is not installed. Run: pip install faster-whisper")
        sys.exit(1)

    model = WhisperModel(args.model, compute_type="int8")
    segments, _ = model.transcribe(
        audio_path,
        language="ja",
        beam_size=5,
        vad_filter=True,
    )
    segments = list(segments)

    if not segments:
        log("Error: No speech detected in audio.")
        sys.exit(1)

    log(f"    Found {len(segments)} segments.")

    # Step 3: Build Japanese subtitle entries and write SRT
    log("[3/5] Writing Japanese subtitles...")
    ja_entries = []
    for seg in segments:
        text = seg.text.strip()
        if text:
            ja_entries.append(SubtitleEntry(
                start_ms=int(seg.start * 1000),
                end_ms=int(seg.end * 1000),
                text=text,
            ))

    write_srt_to(ja_entries, ja_srt_path)
    log(f"    Wrote {len(ja_entries)} entries → {ja_srt_path}")

    # Step 4: Translate to Korean
    log("[4/5] Translating to Korean...")
    japanese_texts = [e.text for e in ja_entries]
    korean_texts = asyncio.run(translate_all(japanese_texts))

    # Step 5: Write Korean SRT
    log("[5/5] Writing Korean subtitles...")
    ko_entries = [
        SubtitleEntry(start_ms=e.start_ms, end_ms=e.end_ms, text=kt)
        for e, kt in zip(ja_entries, korean_texts)
        if kt
    ]

    write_srt_to(ko_entries, ko_srt_path)
    log(f"    Wrote {len(ko_entries)} entries → {ko_srt_path}")

    log("")
    log("Done! Generated files:")
    print(str(ja_srt_path))
    print(str(ko_srt_path))


if __name__ == "__main__":
    main()
