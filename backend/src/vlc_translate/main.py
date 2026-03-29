from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from vlc_translate.routes import health, subtitle, audio

app = FastAPI(title="VLC JP→KR Translator", version="0.1.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

app.include_router(health.router, prefix="/api")
app.include_router(subtitle.router, prefix="/api")
app.include_router(audio.router, prefix="/api")
