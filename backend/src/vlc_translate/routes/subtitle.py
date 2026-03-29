import hashlib
from pathlib import Path

from fastapi import APIRouter, HTTPException

from vlc_translate.services.subtitle_parser import (
    extract_subtitle_track,
    parse_subtitles,
    uri_to_path,
)
from vlc_translate.services.translator import translate_all
from vlc_translate.services.srt_writer import write_srt
from vlc_translate.models import SubtitleEntry

router = APIRouter()


@router.get("/translate-subtitle")
async def translate_subtitle(media_path: str, subtitle_path: str | None = None):
    """Translate Japanese subtitles to Korean.

    If subtitle_path is provided, uses that file directly.
    Otherwise, extracts Japanese subtitles from the media file.
    """
    # Resolve subtitle source
    sub_file = subtitle_path
    if not sub_file:
        sub_file = extract_subtitle_track(media_path)
        if not sub_file:
            raise HTTPException(
                status_code=404,
                detail="No Japanese subtitle track found in the media file.",
            )

    # Parse subtitles
    try:
        entries = parse_subtitles(sub_file)
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"Failed to parse subtitles: {e}")

    if not entries:
        raise HTTPException(status_code=400, detail="No subtitle entries found.")

    # Translate
    japanese_texts = [e.text for e in entries]
    try:
        korean_texts = await translate_all(japanese_texts)
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Translation failed: {e}")

    # Build translated entries
    translated_entries = [
        SubtitleEntry(start_ms=e.start_ms, end_ms=e.end_ms, text=kt)
        for e, kt in zip(entries, korean_texts)
        if kt  # skip empty translations
    ]

    # Write SRT
    media_name = Path(uri_to_path(media_path)).stem
    filename_hash = hashlib.md5(media_path.encode()).hexdigest()[:8]
    srt_filename = f"{media_name}_{filename_hash}_ko.srt"
    srt_path = write_srt(translated_entries, srt_filename)

    return {
        "status": "ok",
        "srt_path": srt_path,
        "subtitle_count": len(translated_entries),
    }
