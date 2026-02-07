#!/usr/bin/env bash
# build-windows.sh — Sync source + build Windows native .exe via MSVC from WSL2
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ---------- Configuration ----------
BUILD_TYPE="${BUILD_TYPE:-Release}"
WIN_BUILD_ROOT="${WIN_BUILD_ROOT:-/mnt/c/tmp/grebe}"

# ---------- helpers ----------
info()  { echo "[build] $*"; }
error() { echo "[build] ERROR: $*" >&2; }

# ---------- Detect VS2022 ----------
info "Detecting Visual Studio 2022..."
VCVARSALL=""
VS_EDITION=""
for edition in Community Professional Enterprise; do
    candidate="/mnt/c/Program Files/Microsoft Visual Studio/2022/${edition}/VC/Auxiliary/Build/vcvarsall.bat"
    if [ -f "${candidate}" ]; then
        VCVARSALL="${candidate}"
        VS_EDITION="${edition}"
        break
    fi
done

if [ -z "${VCVARSALL}" ]; then
    error "VS2022 not found. Run: ./scripts/setup-windows.sh"
    exit 1
fi
info "Found VS2022 ${VS_EDITION}"

# ---------- Detect Vulkan SDK ----------
info "Detecting Vulkan SDK..."
VULKAN_BASE="/mnt/c/VulkanSDK"
if [ ! -d "${VULKAN_BASE}" ]; then
    error "Vulkan SDK not found. Run: ./scripts/setup-windows.sh"
    exit 1
fi

VULKAN_VERSION=$(ls -1 "${VULKAN_BASE}" | sort -V | tail -1)
VULKAN_SDK_WSL="${VULKAN_BASE}/${VULKAN_VERSION}"

if [ ! -f "${VULKAN_SDK_WSL}/Bin/glslc.exe" ]; then
    error "glslc.exe not found in Vulkan SDK ${VULKAN_VERSION}"
    exit 1
fi
info "Found Vulkan SDK ${VULKAN_VERSION}"

# ---------- Convert paths to Windows format ----------
WIN_VCVARSALL=$(wslpath -w "${VCVARSALL}")
WIN_VULKAN_SDK=$(wslpath -w "${VULKAN_SDK_WSL}")
WIN_SOURCE=$(wslpath -w "${WIN_BUILD_ROOT}/src")
WIN_BUILD=$(wslpath -w "${WIN_BUILD_ROOT}/build")

# ---------- Sync source ----------
info "Syncing source to ${WIN_BUILD_ROOT}/src/ ..."
mkdir -p "${WIN_BUILD_ROOT}/src"
rsync -a --delete \
    --exclude='build/' \
    --exclude='.git/' \
    --exclude='tmp/' \
    --exclude='.claude/' \
    --exclude='cmake-build-*/' \
    "${PROJECT_DIR}/" "${WIN_BUILD_ROOT}/src/"
info "Source synced."

# ---------- Generate batch file ----------
BAT_FILE="${WIN_BUILD_ROOT}/build_grebe.bat"
WIN_BAT=$(wslpath -w "${BAT_FILE}")

info "Generating build batch file..."
cat > "${BAT_FILE}" << BATEOF
@echo off
setlocal enabledelayedexpansion

echo === Setting up MSVC x64 environment ===
call "${WIN_VCVARSALL}" x64
if errorlevel 1 (
    echo ERROR: vcvarsall.bat failed
    exit /b 1
)

echo === Setting up Vulkan SDK ===
set VULKAN_SDK=${WIN_VULKAN_SDK}
set PATH=%VULKAN_SDK%\\Bin;%PATH%

REM Add Git for Windows to PATH if available
if exist "C:\\Program Files\\Git\\cmd\\git.exe" (
    set "PATH=C:\\Program Files\\Git\\cmd;%PATH%"
)

REM Verify required tools
echo === Verifying tools ===
where cmake >nul 2>&1
if errorlevel 1 (
    echo ERROR: cmake.exe not found
    exit /b 1
)
echo   cmake: OK

where ninja >nul 2>&1
if errorlevel 1 (
    echo ERROR: ninja.exe not found
    exit /b 1
)
echo   ninja: OK

where glslc >nul 2>&1
if errorlevel 1 (
    echo ERROR: glslc.exe not found. Check VULKAN_SDK path.
    exit /b 1
)
echo   glslc: OK

where git >nul 2>&1
if errorlevel 1 (
    echo ERROR: git.exe not found. Install Git for Windows.
    exit /b 1
)
echo   git: OK

echo === CMake Configure (${BUILD_TYPE}) ===
cmake -G Ninja ^
    -S "${WIN_SOURCE}" ^
    -B "${WIN_BUILD}" ^
    -DCMAKE_BUILD_TYPE=${BUILD_TYPE} ^
    -DCMAKE_C_COMPILER=cl ^
    -DCMAKE_CXX_COMPILER=cl
if errorlevel 1 (
    echo ERROR: CMake configure failed
    exit /b 1
)

echo === CMake Build ===
cmake --build "${WIN_BUILD}" --config ${BUILD_TYPE}
if errorlevel 1 (
    echo ERROR: Build failed
    exit /b 1
)

echo === Build succeeded ===
BATEOF

# ---------- Execute build ----------
info "Building (MSVC ${BUILD_TYPE})..."
info "Executing: ${WIN_BAT}"
echo ""

cmd.exe /C "${WIN_BAT}"
BUILD_EXIT=$?

echo ""
if [ ${BUILD_EXIT} -ne 0 ]; then
    error "Windows build failed with exit code ${BUILD_EXIT}"
    exit 1
fi

# ---------- Report ----------
EXE_PATH="${WIN_BUILD_ROOT}/build/grebe.exe"
if [ -f "${EXE_PATH}" ]; then
    EXE_SIZE=$(du -h "${EXE_PATH}" | cut -f1)
    info "============================================"
    info "Build successful!"
    info "  Executable: $(wslpath -w "${EXE_PATH}") (${EXE_SIZE})"
    info "  Build type: ${BUILD_TYPE}"
    info "  Run with:   ./scripts/run-windows.sh [args]"
    info "============================================"
else
    warn "Build reported success but .exe not found at expected path."
    warn "Check: ${WIN_BUILD_ROOT}/build/"
    # Try to find it
    find "${WIN_BUILD_ROOT}/build" -name "grebe.exe" 2>/dev/null || true
fi
