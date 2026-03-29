#!/bin/bash
# Start the VLC JP→KR Translation backend server

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BACKEND_DIR="$SCRIPT_DIR/../backend"

cd "$BACKEND_DIR"

# Load .env if it exists
if [ -f .env ]; then
    echo "Loading .env configuration..."
fi

PORT="${BACKEND_PORT:-8765}"

echo "Starting VLC Translation Backend on port $PORT..."
echo "Health check: http://localhost:$PORT/api/health"
echo ""

uvicorn vlc_translate.main:app --host 0.0.0.0 --port "$PORT" --reload
