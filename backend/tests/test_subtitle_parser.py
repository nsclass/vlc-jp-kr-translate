import tempfile
from pathlib import Path

from vlc_translate.services.subtitle_parser import parse_subtitles, uri_to_path


def test_uri_to_path_file_uri():
    assert uri_to_path("file:///Users/test/movie.mkv") == "/Users/test/movie.mkv"


def test_uri_to_path_plain_path():
    assert uri_to_path("/Users/test/movie.mkv") == "/Users/test/movie.mkv"


def test_uri_to_path_encoded_spaces():
    result = uri_to_path("file:///Users/test/my%20movie.mkv")
    assert result == "/Users/test/my movie.mkv"


def test_parse_subtitles_srt():
    srt_content = """1
00:00:01,000 --> 00:00:03,000
こんにちは

2
00:00:05,000 --> 00:00:08,500
お元気ですか？

3
00:00:10,000 --> 00:00:12,000
はい、元気です。
"""
    with tempfile.NamedTemporaryFile(mode="w", suffix=".srt", delete=False, encoding="utf-8") as f:
        f.write(srt_content)
        f.flush()
        entries = parse_subtitles(f.name)

    assert len(entries) == 3
    assert entries[0].text == "こんにちは"
    assert entries[0].start_ms == 1000
    assert entries[0].end_ms == 3000
    assert entries[1].text == "お元気ですか？"
    assert entries[2].text == "はい、元気です。"


def test_parse_subtitles_empty_lines_skipped():
    srt_content = """1
00:00:01,000 --> 00:00:02,000
テスト

2
00:00:03,000 --> 00:00:04,000

"""
    with tempfile.NamedTemporaryFile(mode="w", suffix=".srt", delete=False, encoding="utf-8") as f:
        f.write(srt_content)
        f.flush()
        entries = parse_subtitles(f.name)

    # Second entry has empty text, should be skipped
    assert len(entries) == 1
    assert entries[0].text == "テスト"
