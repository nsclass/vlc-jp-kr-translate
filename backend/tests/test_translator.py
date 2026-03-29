from vlc_translate.services.translator import ClaudeCLITranslator as ClaudeTranslator


def test_parse_numbered_response():
    translator = ClaudeTranslator.__new__(ClaudeTranslator)
    response = """1: 안녕하세요
2: 잠깐만요
3: 고마워요"""

    result = translator._parse_numbered_response(response, 3)
    assert result == ["안녕하세요", "잠깐만요", "고마워요"]


def test_parse_numbered_response_with_extra_whitespace():
    translator = ClaudeTranslator.__new__(ClaudeTranslator)
    response = """  1:  안녕하세요
  2: 잠깐만요
"""
    result = translator._parse_numbered_response(response, 2)
    assert result == ["안녕하세요", "잠깐만요"]


def test_parse_numbered_response_missing_lines():
    translator = ClaudeTranslator.__new__(ClaudeTranslator)
    response = """1: 안녕하세요
3: 고마워요"""

    result = translator._parse_numbered_response(response, 3)
    assert result[0] == "안녕하세요"
    assert result[1] == ""  # missing line 2
    assert result[2] == "고마워요"


def test_parse_numbered_response_with_colons_in_text():
    translator = ClaudeTranslator.__new__(ClaudeTranslator)
    response = """1: 시간: 오후 3시
2: 장소: 학교"""

    result = translator._parse_numbered_response(response, 2)
    assert result[0] == "시간: 오후 3시"
    assert result[1] == "장소: 학교"
