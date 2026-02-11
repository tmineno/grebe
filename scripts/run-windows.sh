#!/usr/bin/env bash
# run-windows.sh — Run the Windows-native .exe from WSL2
set -euo pipefail

WIN_BUILD_ROOT="${WIN_BUILD_ROOT:-/mnt/c/tmp/grebe}"

# Support different executables: run-windows.sh [bench|sg|viewer] [args...]
# Default: grebe-viewer
EXE_NAME="grebe-viewer"
ARGS=("$@")
if [ $# -gt 0 ]; then
    case "$1" in
        bench)  EXE_NAME="grebe-bench"; ARGS=("${@:2}") ;;
        sg)     EXE_NAME="grebe-sg";    ARGS=("${@:2}") ;;
        viewer) EXE_NAME="grebe-viewer"; ARGS=("${@:2}") ;;
    esac
fi

EXE_PATH="${WIN_BUILD_ROOT}/build/${EXE_NAME}.exe"

if [ ! -f "${EXE_PATH}" ]; then
    echo "ERROR: Executable not found at ${EXE_PATH}" >&2
    echo "Run ./scripts/build-windows.sh first." >&2
    exit 1
fi

WIN_EXE=$(wslpath -w "${EXE_PATH}")
WIN_CWD=$(wslpath -w "${WIN_BUILD_ROOT}/build")

echo "[run] Executable: ${WIN_EXE}"
echo "[run] Working dir: ${WIN_CWD}"
echo "[run] Arguments: ${ARGS[*]:-}"
echo ""

# Generate a small .bat to avoid cmd.exe quoting issues
RUN_BAT="${WIN_BUILD_ROOT}/run_grebe.bat"
cat > "${RUN_BAT}" << BATEOF
@echo off
cd /d ${WIN_CWD}
${WIN_EXE} ${ARGS[*]:-}
BATEOF

WIN_BAT=$(wslpath -w "${RUN_BAT}")
cmd.exe /C "${WIN_BAT}"
EXIT_CODE=$?

# Copy any generated reports back to WSL project tmp/
BUILD_TMP="${WIN_BUILD_ROOT}/build/tmp"
PROJECT_TMP="$(cd "$(dirname "$0")/.." && pwd)/tmp"
if [ -d "${BUILD_TMP}" ]; then
    mkdir -p "${PROJECT_TMP}"
    # Copy new files (profile/bench/telemetry JSONs and CSVs)
    for f in "${BUILD_TMP}"/*.json "${BUILD_TMP}"/*.csv; do
        if [ -f "$f" ]; then
            cp "$f" "${PROJECT_TMP}/"
            echo "[run] Copied: $(basename "$f") → ./tmp/"
        fi
    done
fi

exit ${EXIT_CODE}
