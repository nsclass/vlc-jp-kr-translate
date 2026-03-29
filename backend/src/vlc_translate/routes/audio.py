import asyncio

from fastapi import APIRouter, HTTPException

from vlc_translate.services.transcriber import (
    create_transcription_task,
    get_task,
    transcribe_and_translate,
)

router = APIRouter()


@router.get("/start-transcription")
async def start_transcription(media_path: str, mode: str = "batch"):
    """Start audio transcription and translation.

    Kicks off a background task that:
    1. Extracts audio from the media file
    2. Transcribes Japanese speech with Whisper
    3. Translates to Korean
    4. Generates an SRT file
    """
    task_id = create_transcription_task(media_path)
    asyncio.create_task(transcribe_and_translate(task_id, media_path))

    return {
        "status": "started",
        "task_id": task_id,
        "message": "Transcription started. Poll /api/transcription-status for progress.",
    }


@router.get("/transcription-status")
async def transcription_status(task_id: str):
    """Check the status of a transcription task."""
    task = get_task(task_id)
    if not task:
        raise HTTPException(status_code=404, detail="Task not found.")

    return {
        "task_id": task.task_id,
        "status": task.status,
        "progress": task.progress,
        "srt_path": task.srt_path,
        "error": task.error,
    }
