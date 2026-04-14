import asyncio
import subprocess
from typing import Protocol

import httpx

from vlc_translate import config

CLAUDE_SYSTEM_PROMPT = """You are a professional Japanese-to-Korean translator specializing in movie and drama subtitles.

Rules:
- Translate each numbered line from Japanese to Korean
- Preserve the original numbering exactly
- Use natural, conversational Korean appropriate for subtitles
- Preserve honorifics and speech levels (존댓말/반말) matching the original tone
- Keep translations concise for readability as subtitles
- Do not add explanations or notes
- Return ONLY the numbered translations, one per line

Example input:
1: お兄ちゃん、待って！
2: 何だよ、急に。

Example output:
1: 오빠, 잠깐만!
2: 뭐야, 갑자기."""


class TranslatorBackend(Protocol):
    async def translate_batch(self, texts: list[str]) -> list[str]: ...


class ClaudeCLITranslator:
    """Uses `claude -p` CLI for translation (no API key needed)."""

    async def translate_batch(self, texts: list[str]) -> list[str]:
        numbered = "\n".join(f"{i+1}: {t}" for i, t in enumerate(texts))
        prompt = f"{CLAUDE_SYSTEM_PROMPT}\n\nTranslate the following:\n{numbered}"

        result = await asyncio.to_thread(self._run_claude, prompt)
        return self._parse_numbered_response(result, len(texts))

    def _run_claude(self, prompt: str) -> str:
        result = subprocess.run(
            ["claude", "-p"],
            input=prompt,
            capture_output=True,
            text=True,
            timeout=600,
        )
        if result.returncode != 0:
            raise RuntimeError(f"claude CLI failed: {result.stderr}")
        return result.stdout.strip()

    def _parse_numbered_response(self, response: str, expected_count: int) -> list[str]:
        """Parse numbered response lines back into a list."""
        translations = [""] * expected_count
        for line in response.strip().split("\n"):
            line = line.strip()
            if not line:
                continue
            colon_pos = line.find(":")
            if colon_pos == -1:
                continue
            num_str = line[:colon_pos].strip()
            try:
                idx = int(num_str) - 1
                if 0 <= idx < expected_count:
                    translations[idx] = line[colon_pos + 1:].strip()
            except ValueError:
                continue
        return translations


class DeepLTranslator:
    API_URL = "https://api-free.deepl.com/v2/translate"

    def __init__(self):
        self.api_key = config.DEEPL_API_KEY
        self.client = httpx.AsyncClient(timeout=30.0)

    async def translate_batch(self, texts: list[str]) -> list[str]:
        response = await self.client.post(
            self.API_URL,
            data={
                "auth_key": self.api_key,
                "text": texts,
                "source_lang": "JA",
                "target_lang": "KO",
            },
        )
        response.raise_for_status()
        data = response.json()
        return [t["text"] for t in data["translations"]]


def get_translator() -> TranslatorBackend:
    if config.TRANSLATION_BACKEND == "deepl":
        return DeepLTranslator()
    return ClaudeCLITranslator()


async def translate_all(texts: list[str], batch_size: int = 50) -> list[str]:
    """Translate a list of Japanese texts to Korean in batches."""
    translator = get_translator()
    # Use lower concurrency for CLI calls to avoid overwhelming the system
    semaphore = asyncio.Semaphore(2)

    batches = [texts[i:i + batch_size] for i in range(0, len(texts), batch_size)]
    results: list[list[str]] = [[] for _ in batches]

    async def process_batch(idx: int, batch: list[str]):
        async with semaphore:
            results[idx] = await translator.translate_batch(batch)

    await asyncio.gather(*(process_batch(i, b) for i, b in enumerate(batches)))

    translated = []
    for batch_result in results:
        translated.extend(batch_result)
    return translated
