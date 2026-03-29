from pathlib import Path

from vlc_translate import config
from vlc_translate.models import SubtitleEntry


def ms_to_srt_time(ms: int) -> str:
    """Convert milliseconds to SRT timestamp format (HH:MM:SS,mmm)."""
    hours = ms // 3_600_000
    ms %= 3_600_000
    minutes = ms // 60_000
    ms %= 60_000
    seconds = ms // 1_000
    millis = ms % 1_000
    return f"{hours:02d}:{minutes:02d}:{seconds:02d},{millis:03d}"


def write_srt(entries: list[SubtitleEntry], filename: str) -> str:
    """Write subtitle entries to an SRT file. Returns the file path."""
    output_path = config.OUTPUT_DIR / filename
    lines = []
    for i, entry in enumerate(entries, 1):
        lines.append(str(i))
        lines.append(f"{ms_to_srt_time(entry.start_ms)} --> {ms_to_srt_time(entry.end_ms)}")
        lines.append(entry.text)
        lines.append("")

    output_path.write_text("\n".join(lines), encoding="utf-8")
    return str(output_path)


def append_srt(entries: list[SubtitleEntry], filepath: str, start_index: int) -> None:
    """Append subtitle entries to an existing SRT file (for streaming mode)."""
    path = Path(filepath)
    lines = []
    for i, entry in enumerate(entries, start_index):
        lines.append(str(i))
        lines.append(f"{ms_to_srt_time(entry.start_ms)} --> {ms_to_srt_time(entry.end_ms)}")
        lines.append(entry.text)
        lines.append("")

    with path.open("a", encoding="utf-8") as f:
        f.write("\n".join(lines))
