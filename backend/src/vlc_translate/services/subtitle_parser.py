import subprocess
import json
import tempfile
from pathlib import Path
from urllib.parse import unquote, urlparse

import chardet
import pysubs2

from vlc_translate.models import SubtitleEntry


def uri_to_path(uri: str) -> str:
    """Convert a file:// URI to a local path."""
    if uri.startswith("file://"):
        parsed = urlparse(uri)
        return unquote(parsed.path)
    return uri


def detect_encoding(file_path: str) -> str:
    """Detect the encoding of a subtitle file."""
    with open(file_path, "rb") as f:
        raw = f.read()
    result = chardet.detect(raw)
    return result.get("encoding", "utf-8") or "utf-8"


def extract_subtitle_track(media_path: str) -> str | None:
    """Extract Japanese subtitle track from a media file using FFmpeg.

    Returns path to extracted SRT file, or None if no Japanese subtitle found.
    """
    media_path = uri_to_path(media_path)

    # Probe for subtitle streams
    probe_cmd = [
        "ffprobe", "-v", "quiet",
        "-print_format", "json",
        "-show_streams",
        "-select_streams", "s",
        media_path,
    ]
    try:
        result = subprocess.run(probe_cmd, capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            return None
        probe_data = json.loads(result.stdout)
    except (subprocess.TimeoutExpired, json.JSONDecodeError, FileNotFoundError):
        return None

    streams = probe_data.get("streams", [])
    if not streams:
        return None

    # Find Japanese subtitle track (by language tag or first available)
    target_index = None
    for stream in streams:
        tags = stream.get("tags", {})
        lang = tags.get("language", "").lower()
        if lang in ("ja", "jpn", "japanese"):
            target_index = stream["index"]
            break

    # Fallback: use the first subtitle stream
    if target_index is None and streams:
        target_index = streams[0]["index"]

    if target_index is None:
        return None

    # Extract to a temp SRT file
    output_path = Path(tempfile.mkdtemp()) / "extracted.srt"
    extract_cmd = [
        "ffmpeg", "-y", "-v", "quiet",
        "-i", media_path,
        "-map", f"0:{target_index}",
        "-c:s", "srt",
        str(output_path),
    ]
    try:
        result = subprocess.run(extract_cmd, capture_output=True, text=True, timeout=60)
        if result.returncode != 0:
            return None
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return None

    if output_path.exists() and output_path.stat().st_size > 0:
        return str(output_path)
    return None


def parse_subtitles(subtitle_path: str) -> list[SubtitleEntry]:
    """Parse an SRT/ASS/SSA subtitle file into a list of SubtitleEntry."""
    subtitle_path = uri_to_path(subtitle_path)
    encoding = detect_encoding(subtitle_path)

    subs = pysubs2.load(subtitle_path, encoding=encoding)
    entries = []
    for event in subs:
        if event.is_comment:
            continue
        text = event.plaintext.strip()
        if text:
            entries.append(SubtitleEntry(
                start_ms=event.start,
                end_ms=event.end,
                text=text,
            ))
    return entries
