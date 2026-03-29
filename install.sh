#!/bin/bash
# Install VLC JP→KR Subtitle Translator Extension

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LUA_SRC="$SCRIPT_DIR/vlc-extension/vlc_jp_kr_translate.lua"

# Detect OS and set VLC extensions directory
case "$(uname -s)" in
    Darwin)
        VLC_EXT_DIR="$HOME/Library/Application Support/org.videolan.vlc/lua/extensions"
        ;;
    Linux)
        VLC_EXT_DIR="$HOME/.local/share/vlc/lua/extensions"
        ;;
    *)
        echo "Unsupported OS. Please manually copy the Lua extension."
        echo "Source: $LUA_SRC"
        exit 1
        ;;
esac

# Create extensions directory if needed
mkdir -p "$VLC_EXT_DIR"

# Symlink the extension
ln -sf "$LUA_SRC" "$VLC_EXT_DIR/vlc_jp_kr_translate.lua"

echo "✓ VLC extension installed to: $VLC_EXT_DIR"
echo ""
echo "Next steps:"
echo "  1. Start the backend:  cd backend && uvicorn vlc_translate.main:app --port 8765"
echo "  2. Restart VLC"
echo "  3. Go to View > JP→KR 자막 번역기 > 자막 번역 (Translate Subtitles)"
