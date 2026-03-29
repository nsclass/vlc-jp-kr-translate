import subprocess
import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

from vlc_translate.services.srt_writer import write_srt_to
from vlc_translate.models import SubtitleEntry


def test_write_srt_to(tmp_path):
    entries = [
        SubtitleEntry(start_ms=1000, end_ms=3000, text="こんにちは"),
        SubtitleEntry(start_ms=4000, end_ms=6000, text="さようなら"),
    ]
    output = tmp_path / "test_ja.srt"
    result = write_srt_to(entries, output)

    assert result == str(output)
    content = output.read_text(encoding="utf-8")
    assert "1\n00:00:01,000 --> 00:00:03,000\nこんにちは" in content
    assert "2\n00:00:04,000 --> 00:00:06,000\nさようなら" in content


def test_cli_help():
    result = subprocess.run(
        [sys.executable, "-m", "vlc_translate.cli", "--help"],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0
    assert "Generate Japanese and Korean subtitles" in result.stdout


def test_cli_missing_file():
    result = subprocess.run(
        [sys.executable, "-m", "vlc_translate.cli", "/nonexistent/file.mp4"],
        capture_output=True,
        text=True,
    )
    assert result.returncode != 0
    assert "File not found" in result.stderr


def test_cli_pipeline(tmp_path, monkeypatch):
    """Test the full pipeline with mocked Whisper and translator."""
    # Create a fake input file
    fake_input = tmp_path / "test_movie.mp4"
    fake_input.write_bytes(b"fake video data")

    # Mock segment objects returned by Whisper
    seg1 = MagicMock()
    seg1.start = 1.0
    seg1.end = 3.0
    seg1.text = "こんにちは"

    seg2 = MagicMock()
    seg2.start = 4.0
    seg2.end = 6.0
    seg2.text = "さようなら"

    mock_model = MagicMock()
    mock_model.transcribe.return_value = ([seg1, seg2], None)

    with patch("shutil.which", return_value="/usr/bin/ffmpeg"), \
         patch("vlc_translate.services.transcriber.extract_audio", return_value="/tmp/fake_audio.wav"), \
         patch("faster_whisper.WhisperModel", return_value=mock_model), \
         patch("vlc_translate.services.translator.translate_all") as mock_translate:

        mock_translate.return_value = ["안녕하세요", "안녕히 가세요"]

        from vlc_translate.cli import main
        monkeypatch.setattr(
            "sys.argv",
            ["vlc-translate", str(fake_input), "-o", str(tmp_path)],
        )
        main()

    ja_srt = tmp_path / "test_movie_ja.srt"
    ko_srt = tmp_path / "test_movie_ko.srt"
    assert ja_srt.exists()
    assert ko_srt.exists()

    ja_content = ja_srt.read_text(encoding="utf-8")
    assert "こんにちは" in ja_content
    assert "さようなら" in ja_content

    ko_content = ko_srt.read_text(encoding="utf-8")
    assert "안녕하세요" in ko_content
    assert "안녕히 가세요" in ko_content
