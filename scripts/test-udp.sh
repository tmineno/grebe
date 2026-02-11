#!/usr/bin/env bash
# Test script for UDP transport mode (loopback)
# Usage: ./scripts/test-udp.sh [--channels=N] [--rate=RATE]
#
# Starts grebe-sg in UDP mode and grebe-viewer listening on the same port.
# Both processes run in foreground; Ctrl+C stops everything.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

SG_BIN="$BUILD_DIR/grebe-sg"
VIEWER_BIN="$BUILD_DIR/grebe-viewer"

UDP_PORT=5000
CHANNELS=1
EXTRA_SG_ARGS=()
EXTRA_VIEWER_ARGS=()

for arg in "$@"; do
    case "$arg" in
        --channels=*)
            CHANNELS="${arg#--channels=}"
            ;;
        --port=*)
            UDP_PORT="${arg#--port=}"
            ;;
        --no-vsync)
            EXTRA_VIEWER_ARGS+=("--no-vsync")
            ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --channels=N   Number of channels (default: 1)"
            echo "  --port=PORT    UDP port (default: 5000)"
            echo "  --no-vsync     Disable V-Sync on viewer"
            echo "  --help         Show this help"
            exit 0
            ;;
        *)
            EXTRA_SG_ARGS+=("$arg")
            ;;
    esac
done

if [[ ! -x "$SG_BIN" ]] || [[ ! -x "$VIEWER_BIN" ]]; then
    echo "Error: binaries not found. Build first:"
    echo "  cmake --preset linux-release && cmake --build build"
    exit 1
fi

cleanup() {
    echo ""
    echo "Stopping..."
    [[ -n "${SG_PID:-}" ]] && kill "$SG_PID" 2>/dev/null || true
    [[ -n "${VIEWER_PID:-}" ]] && kill "$VIEWER_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    echo "Done."
}
trap cleanup EXIT INT TERM

echo "=== UDP loopback test ==="
echo "  Port:     $UDP_PORT"
echo "  Channels: $CHANNELS"
echo ""

# Start grebe-sg in UDP mode (background)
echo "[1] Starting grebe-sg (UDP -> 127.0.0.1:$UDP_PORT)..."
"$SG_BIN" \
    --transport=udp \
    --udp-target="127.0.0.1:$UDP_PORT" \
    --channels="$CHANNELS" \
    "${EXTRA_SG_ARGS[@]}" &
SG_PID=$!

# Brief pause for sg to bind
sleep 1

# Start grebe-viewer in UDP mode (foreground)
echo "[2] Starting grebe-viewer (UDP listen :$UDP_PORT)..."
"$VIEWER_BIN" \
    --udp="$UDP_PORT" \
    --channels="$CHANNELS" \
    "${EXTRA_VIEWER_ARGS[@]}" &
VIEWER_PID=$!

# Wait for viewer to exit (user closes window or presses Esc)
wait "$VIEWER_PID" 2>/dev/null || true
VIEWER_PID=""
