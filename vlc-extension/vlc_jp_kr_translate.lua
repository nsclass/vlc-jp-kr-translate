-- VLC Japanese-to-Korean Subtitle Translator Extension
-- Communicates with a Python FastAPI backend to translate subtitles

local BACKEND_URL = "http://localhost:8765"
local dlg = nil  -- dialog handle

function descriptor()
    return {
        title = "JP→KR 자막 번역기",
        version = "1.0.0",
        author = "nsclass",
        url = "https://github.com/nsclass/vlc-jp-kr-translate",
        capabilities = {"input-listener", "menu"},
        description = "일본어 자막을 한국어로 실시간 번역합니다 (Translates Japanese subtitles to Korean)"
    }
end

function activate()
    vlc.msg.info("[JP-KR] Extension activated")
end

function deactivate()
    if dlg then
        dlg:delete()
        dlg = nil
    end
    vlc.msg.info("[JP-KR] Extension deactivated")
end

function menu()
    return {
        "자막 번역 (Translate Subtitles)",
        "음성 인식 시작 (Start Audio Transcription)",
        "설정 (Settings)"
    }
end

function trigger_menu(id)
    if id == 1 then
        translate_subtitles()
    elseif id == 2 then
        start_transcription()
    elseif id == 3 then
        show_settings()
    end
end

function input_changed()
    -- Called when media input changes; could auto-detect subtitles here
end

-- ============================================================
-- Subtitle Translation Mode
-- ============================================================

function translate_subtitles()
    local item = vlc.input.item()
    if not item then
        show_message("오류", "재생 중인 미디어가 없습니다.\nNo media is currently playing.")
        return
    end

    local media_uri = item:uri()
    if not media_uri then
        show_message("오류", "미디어 경로를 가져올 수 없습니다.\nCannot get media path.")
        return
    end

    -- Check backend health first
    if not check_backend_health() then
        show_message("오류",
            "백엔드 서버에 연결할 수 없습니다.\nCannot connect to backend server.\n\n" ..
            "다음 명령으로 서버를 시작하세요:\n" ..
            "Start the server with:\n" ..
            "  cd vlc-jp-kr-translate/backend\n" ..
            "  uvicorn vlc_translate.main:app --port 8765")
        return
    end

    -- Show progress dialog
    show_message("번역 중...", "자막을 번역하고 있습니다. 잠시 기다려 주세요...\nTranslating subtitles, please wait...")

    -- Call the translation endpoint
    local encoded_path = url_encode(media_uri)
    local request_url = BACKEND_URL .. "/api/translate-subtitle?media_path=" .. encoded_path

    local stream = vlc.stream(request_url)
    if not stream then
        close_dialog()
        show_message("오류", "번역 요청에 실패했습니다.\nTranslation request failed.")
        return
    end

    local response = ""
    local block = stream:readline()
    while block do
        response = response .. block
        block = stream:readline()
    end

    -- Parse JSON response
    local srt_path = parse_srt_path(response)
    if not srt_path then
        close_dialog()
        show_message("오류", "번역 응답을 처리할 수 없습니다.\nFailed to process translation response.\n\n" .. response)
        return
    end

    -- Load the translated subtitle into VLC
    vlc.input.add_subtitle(srt_path)

    close_dialog()
    show_message("완료!", "한국어 자막이 추가되었습니다!\nKorean subtitles have been added!\n\n" ..
        "자막 트랙에서 선택하세요.\nSelect it from the subtitle track menu.")
end

-- ============================================================
-- Audio Transcription Mode
-- ============================================================

function start_transcription()
    local item = vlc.input.item()
    if not item then
        show_message("오류", "재생 중인 미디어가 없습니다.\nNo media is currently playing.")
        return
    end

    local media_uri = item:uri()

    if not check_backend_health() then
        show_message("오류",
            "백엔드 서버에 연결할 수 없습니다.\nCannot connect to backend server.")
        return
    end

    show_message("음성 인식 중...",
        "음성을 인식하고 번역하고 있습니다.\nTranscribing and translating audio...\n\n" ..
        "이 작업은 시간이 걸릴 수 있습니다.\nThis may take a while.")

    local encoded_path = url_encode(media_uri)
    local request_url = BACKEND_URL .. "/api/start-transcription?media_path=" .. encoded_path .. "&mode=batch"

    local stream = vlc.stream(request_url)
    if not stream then
        close_dialog()
        show_message("오류", "음성 인식 요청에 실패했습니다.\nTranscription request failed.")
        return
    end

    local response = ""
    local block = stream:readline()
    while block do
        response = response .. block
        block = stream:readline()
    end

    local task_id = parse_json_field(response, "task_id")
    if task_id then
        close_dialog()
        poll_transcription(task_id)
    else
        close_dialog()
        show_message("오류", "음성 인식 시작에 실패했습니다.\nFailed to start transcription.\n\n" .. response)
    end
end

function poll_transcription(task_id)
    -- Poll for transcription completion
    local request_url = BACKEND_URL .. "/api/transcription-status?task_id=" .. url_encode(task_id)

    show_message("처리 중...", "음성 인식 진행 중...\nTranscription in progress...")

    local max_attempts = 360  -- up to 30 minutes (5s interval)
    for i = 1, max_attempts do
        vlc.misc.mwait(vlc.misc.mdate() + 5000000)  -- wait 5 seconds

        local stream = vlc.stream(request_url)
        if stream then
            local response = ""
            local block = stream:readline()
            while block do
                response = response .. block
                block = stream:readline()
            end

            local status = parse_json_field(response, "status")
            local progress = parse_json_field(response, "progress") or "0"

            if status == "complete" then
                local srt_path = parse_srt_path(response)
                if srt_path then
                    vlc.input.add_subtitle(srt_path)
                    close_dialog()
                    show_message("완료!",
                        "한국어 자막이 추가되었습니다!\nKorean subtitles have been added!")
                    return
                end
            elseif status == "error" then
                close_dialog()
                local err = parse_json_field(response, "error") or "Unknown error"
                show_message("오류", "음성 인식 실패: " .. err)
                return
            end
            -- Still running, continue polling
        end
    end

    close_dialog()
    show_message("시간 초과", "음성 인식 시간이 초과되었습니다.\nTranscription timed out.")
end

-- ============================================================
-- Settings
-- ============================================================

function show_settings()
    if dlg then
        dlg:delete()
    end

    dlg = vlc.dialog("JP→KR 설정 (Settings)")
    dlg:add_label("백엔드 URL (Backend URL):", 1, 1, 1, 1)
    local url_input = dlg:add_text_input(BACKEND_URL, 2, 1, 2, 1)
    dlg:add_button("저장 (Save)", function()
        BACKEND_URL = url_input:get_text()
        vlc.msg.info("[JP-KR] Backend URL updated to: " .. BACKEND_URL)
        dlg:delete()
        dlg = nil
    end, 1, 2, 1, 1)
    dlg:add_button("취소 (Cancel)", function()
        dlg:delete()
        dlg = nil
    end, 2, 2, 1, 1)
    dlg:show()
end

-- ============================================================
-- Utility Functions
-- ============================================================

function check_backend_health()
    local stream = vlc.stream(BACKEND_URL .. "/api/health")
    if not stream then
        return false
    end
    local response = stream:readline()
    return response and response:find("ok") ~= nil
end

function show_message(title, message)
    if dlg then
        dlg:delete()
    end
    dlg = vlc.dialog(title)
    dlg:add_label(message, 1, 1, 1, 1)
    dlg:add_button("확인 (OK)", function()
        dlg:delete()
        dlg = nil
    end, 1, 2, 1, 1)
    dlg:show()
end

function close_dialog()
    if dlg then
        dlg:delete()
        dlg = nil
    end
end

function parse_srt_path(json_str)
    -- Simple JSON field extraction for "srt_path"
    return parse_json_field(json_str, "srt_path")
end

function parse_json_field(json_str, field)
    -- Simple pattern-based JSON field extraction
    -- Matches "field": "value" or "field":"value"
    local pattern = '"' .. field .. '"%s*:%s*"([^"]*)"'
    local value = json_str:match(pattern)
    if value then
        return value
    end
    -- Try numeric/boolean values
    pattern = '"' .. field .. '"%s*:%s*([%w%.]+)'
    return json_str:match(pattern)
end

function url_encode(str)
    str = string.gsub(str, "([^%w%-%.%_%~%/])", function(c)
        return string.format("%%%02X", string.byte(c))
    end)
    return str
end
