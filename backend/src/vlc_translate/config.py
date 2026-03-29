import os
from pathlib import Path

from dotenv import load_dotenv

load_dotenv()

DEEPL_API_KEY = os.getenv("DEEPL_API_KEY", "")
TRANSLATION_BACKEND = os.getenv("TRANSLATION_BACKEND", "claude")  # "claude" uses `claude -p`, "deepl" uses DeepL API
WHISPER_MODEL = os.getenv("WHISPER_MODEL", "medium")
BACKEND_PORT = int(os.getenv("BACKEND_PORT", "8765"))

OUTPUT_DIR = Path("/tmp/vlc-translate")
OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
