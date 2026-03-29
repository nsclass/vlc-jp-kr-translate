import asyncio
import subprocess
import tempfile
import uuid
from pathlib import Path

from vlc_translate import config
from vlc_translate.models import SubtitleEntry, TranscriptionTask
from vlc_translate.services.subtitle_parser import uri_to_path
from vlc_translate.services.translator import translate_all
from vlc_translate.services.srt_writer import write_srt

# In-memory task registry
_tasks: dict[str, TranscriptionTask] = {}


def get_task(task_id: str) -> TranscriptionTask | None:
    return _tasks.get(task_id)


def extract_audio(media_path: str) -> str | None:
    """Extract audio from media file to WAV format for Whisper."""
    media_path = uri_to_path(media_path)
    output_path = Path(tempfile.mkdtemp()) / "audio.wav"

    cmd = [
        "ffmpeg", "-y", "-v", "quiet",
        "-i", media_path,
        "-vn",
        "-acodec", "pcm_s16le",
        "-ar", "16000",
        "-ac", "1",
        str(output_path),
    ]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        if result.returncode != 0:
            return None
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return None

    if output_path.exists() and output_path.stat().st_size > 0:
        return str(output_path)
    return None


async def transcribe_and_translate(task_id: str, media_path: str) -> None:
    """Background task: transcribe Japanese audio and translate to Korean."""
    task = _tasks[task_id]

    try:
        # Step 1: Extract audio
        task.progress = 0.1
        audio_path = await asyncio.to_thread(extract_audio, media_path)
        if not audio_path:
            task.status = "error"
            task.error = "Failed to extract audio from media file."
            return

        # Step 2: Transcribe with faster-whisper
        task.progress = 0.2
        try:
            from faster_whisper import WhisperModel
        except ImportError:
            task.status = "error"
            task.error = (
                "faster-whisper is not installed. "
                "Install it with: pip install faster-whisper"
            )
            return

        def run_whisper():
            model = WhisperModel(config.WHISPER_MODEL, compute_type="int8")
            segments, _ = model.transcribe(
                audio_path,
                language="ja",
                beam_size=5,
                vad_filter=True,
            )
            return list(segments)

        segments = await asyncio.to_thread(run_whisper)
        task.progress = 0.6

        if not segments:
            task.status = "error"
            task.error = "No speech detected in audio."
            return

        # Step 3: Build subtitle entries from segments
        entries = []
        for seg in segments:
            text = seg.text.strip()
            if text:
                entries.append(SubtitleEntry(
                    start_ms=int(seg.start * 1000),
                    end_ms=int(seg.end * 1000),
                    text=text,
                ))

        task.progress = 0.7

        # Step 4: Translate to Korean
        japanese_texts = [e.text for e in entries]
        korean_texts = await translate_all(japanese_texts)
        task.progress = 0.9

        # Step 5: Write Korean SRT
        translated_entries = [
            SubtitleEntry(start_ms=e.start_ms, end_ms=e.end_ms, text=kt)
            for e, kt in zip(entries, korean_texts)
            if kt
        ]

        media_stem = Path(uri_to_path(media_path)).stem
        srt_filename = f"{media_stem}_{task_id[:8]}_audio_ko.srt"
        srt_path = write_srt(translated_entries, srt_filename)

        task.srt_path = srt_path
        task.progress = 1.0
        task.status = "complete"

    except Exception as e:
        task.status = "error"
        task.error = str(e)


def create_transcription_task(media_path: str) -> str:
    """Create a new transcription task and return its ID."""
    task_id = uuid.uuid4().hex[:12]
    _tasks[task_id] = TranscriptionTask(
        task_id=task_id,
        status="running",
    )
    return task_id
