from dataclasses import dataclass


@dataclass
class SubtitleEntry:
    start_ms: int
    end_ms: int
    text: str


@dataclass
class TranscriptionTask:
    task_id: str
    status: str  # "running", "complete", "error"
    srt_path: str | None = None
    progress: float = 0.0
    error: str | None = None
