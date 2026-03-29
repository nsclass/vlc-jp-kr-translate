from vlc_translate.models import SubtitleEntry
from vlc_translate.services.srt_writer import ms_to_srt_time, write_srt


def test_ms_to_srt_time_zero():
    assert ms_to_srt_time(0) == "00:00:00,000"


def test_ms_to_srt_time_complex():
    # 1 hour, 23 minutes, 45 seconds, 678 milliseconds
    ms = 1 * 3_600_000 + 23 * 60_000 + 45 * 1_000 + 678
    assert ms_to_srt_time(ms) == "01:23:45,678"


def test_ms_to_srt_time_minutes():
    ms = 5 * 60_000 + 30 * 1_000 + 100
    assert ms_to_srt_time(ms) == "00:05:30,100"


def test_write_srt(tmp_path, monkeypatch):
    import vlc_translate.config as cfg
    monkeypatch.setattr(cfg, "OUTPUT_DIR", tmp_path)

    entries = [
        SubtitleEntry(start_ms=0, end_ms=2000, text="안녕하세요"),
        SubtitleEntry(start_ms=3000, end_ms=5500, text="반갑습니다"),
    ]

    path = write_srt(entries, "test_output.srt")

    with open(path, encoding="utf-8") as f:
        content = f.read()

    assert "1\n00:00:00,000 --> 00:00:02,000\n안녕하세요" in content
    assert "2\n00:00:03,000 --> 00:00:05,500\n반갑습니다" in content
