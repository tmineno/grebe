#!/usr/bin/env bash
# Test script for UDP transport mode (loopback)
# Usage: ./scripts/test-udp.sh [--channels=N] [--datagram-size=N] [--port=PORT] [--windows]
#
# Starts grebe-sg in UDP mode and grebe-viewer listening on the same port.
# Both processes run in foreground; Ctrl+C stops everything.
#
# --windows: Use Windows native executables (MSVC build at /mnt/c/tmp/grebe/build/).
#            Requires prior build via: ./scripts/build-windows.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

UDP_PORT=5000
CHANNELS=1
BLOCK_SIZE=""
DATAGRAM_SIZE=""
USE_WINDOWS=false
WIN_BUILD_ROOT="${WIN_BUILD_ROOT:-/mnt/c/tmp/grebe}"
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
        --block-size=*)
            BLOCK_SIZE="${arg#--block-size=}"
            ;;
        --datagram-size=*)
            DATAGRAM_SIZE="${arg#--datagram-size=}"
            ;;
        --no-vsync)
            EXTRA_VIEWER_ARGS+=("--no-vsync")
            ;;
        --windows)
            USE_WINDOWS=true
            ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --channels=N        Number of channels (default: 1)"
            echo "  --block-size=N      Samples per channel per frame (default: auto)"
            echo "  --datagram-size=N   Max UDP datagram bytes (default: 1400, max: 65000)"
            echo "  --port=PORT         UDP port (default: 5000)"
            echo "  --no-vsync          Disable V-Sync on viewer"
            echo "  --windows           Use Windows native executables (MSVC build)"
            echo "  --help              Show this help"
            exit 0
            ;;
        *)
            EXTRA_SG_ARGS+=("$arg")
            ;;
    esac
done

# =========================================================================
# Windows mode: generate .bat and launch via cmd.exe
# =========================================================================
if [[ "$USE_WINDOWS" == true ]]; then
    SG_EXE="$WIN_BUILD_ROOT/build/grebe-sg.exe"
    VIEWER_EXE="$WIN_BUILD_ROOT/build/grebe-viewer.exe"

    if [[ ! -f "$SG_EXE" ]] || [[ ! -f "$VIEWER_EXE" ]]; then
        echo "Error: Windows binaries not found at $WIN_BUILD_ROOT/build/"
        echo "  Build first: ./scripts/build-windows.sh"
        exit 1
    fi

    WIN_SG_EXE=$(wslpath -w "$SG_EXE")
    WIN_VIEWER_EXE=$(wslpath -w "$VIEWER_EXE")
    WIN_CWD=$(wslpath -w "$WIN_BUILD_ROOT/build")

    # Build extra args strings for .bat
    SG_EXTRA=""
    for a in "${EXTRA_SG_ARGS[@]+"${EXTRA_SG_ARGS[@]}"}"; do
        SG_EXTRA="$SG_EXTRA $a"
    done
    VIEWER_EXTRA=""
    for a in "${EXTRA_VIEWER_ARGS[@]+"${EXTRA_VIEWER_ARGS[@]}"}"; do
        VIEWER_EXTRA="$VIEWER_EXTRA $a"
    done

    # Add block-size and datagram-size to sg args if specified
    if [[ -n "$BLOCK_SIZE" ]]; then
        SG_EXTRA="$SG_EXTRA --block-size=$BLOCK_SIZE"
    fi
    if [[ -n "$DATAGRAM_SIZE" ]]; then
        SG_EXTRA="$SG_EXTRA --datagram-size=$DATAGRAM_SIZE"
    fi

    echo "=== UDP loopback test (Windows native) ==="
    echo "  Port:       $UDP_PORT"
    echo "  Channels:   $CHANNELS"
    [[ -n "$BLOCK_SIZE" ]] && echo "  Block size: $BLOCK_SIZE"
    [[ -n "$DATAGRAM_SIZE" ]] && echo "  Datagram:   $DATAGRAM_SIZE bytes"
    echo "  Build:      $WIN_BUILD_ROOT/build/"
    echo ""

    # Generate .bat file that:
    #   1. Starts grebe-sg.exe in a separate window
    #   2. Waits 1 second
    #   3. Runs grebe-viewer.exe in foreground
    #   4. After viewer exits, kills grebe-sg.exe
    BAT_FILE="$WIN_BUILD_ROOT/test_udp.bat"
    cat > "$BAT_FILE" << BATEOF
@echo off
cd /d ${WIN_CWD}

echo [1] Starting grebe-sg (UDP -^> 127.0.0.1:${UDP_PORT})...
start "grebe-sg" ${WIN_SG_EXE} --transport=udp --udp-target=127.0.0.1:${UDP_PORT} --channels=${CHANNELS}${SG_EXTRA}

timeout /t 2 /nobreak >nul

echo [2] Starting grebe-viewer (UDP listen :${UDP_PORT})...
${WIN_VIEWER_EXE} --udp=${UDP_PORT} --channels=${CHANNELS}${VIEWER_EXTRA}

echo.
echo Stopping grebe-sg...
taskkill /IM grebe-sg.exe /F >nul 2>&1
echo Done.
BATEOF

    WIN_BAT=$(wslpath -w "$BAT_FILE")
    echo "[run] Executing: $WIN_BAT"
    echo ""
    cmd.exe /C "$WIN_BAT"
    exit $?
fi

# =========================================================================
# Linux mode: launch native binaries directly
# =========================================================================
SG_BIN="$BUILD_DIR/grebe-sg"
VIEWER_BIN="$BUILD_DIR/grebe-viewer"

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

# Add block-size and datagram-size to sg args if specified
if [[ -n "$BLOCK_SIZE" ]]; then
    EXTRA_SG_ARGS+=("--block-size=$BLOCK_SIZE")
fi
if [[ -n "$DATAGRAM_SIZE" ]]; then
    EXTRA_SG_ARGS+=("--datagram-size=$DATAGRAM_SIZE")
fi

echo "=== UDP loopback test ==="
echo "  Port:       $UDP_PORT"
echo "  Channels:   $CHANNELS"
[[ -n "$BLOCK_SIZE" ]] && echo "  Block size: $BLOCK_SIZE"
[[ -n "$DATAGRAM_SIZE" ]] && echo "  Datagram:   $DATAGRAM_SIZE bytes"
echo ""

# Start grebe-sg in UDP mode (background)
echo "[1] Starting grebe-sg (UDP -> 127.0.0.1:$UDP_PORT)..."
"$SG_BIN" \
    --transport=udp \
    --udp-target="127.0.0.1:$UDP_PORT" \
    --channels="$CHANNELS" \
    "${EXTRA_SG_ARGS[@]+"${EXTRA_SG_ARGS[@]}"}" &
SG_PID=$!

# Brief pause for sg to bind
sleep 1

# Start grebe-viewer in UDP mode (foreground)
echo "[2] Starting grebe-viewer (UDP listen :$UDP_PORT)..."
"$VIEWER_BIN" \
    --udp="$UDP_PORT" \
    --channels="$CHANNELS" \
    "${EXTRA_VIEWER_ARGS[@]+"${EXTRA_VIEWER_ARGS[@]}"}" &
VIEWER_PID=$!

# Wait for viewer to exit (user closes window or presses Esc)
wait "$VIEWER_PID" 2>/dev/null || true
VIEWER_PID=""
