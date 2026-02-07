#!/usr/bin/env bash
# setup-windows.sh — Install Windows-side dependencies for MSVC build via winget
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ---------- helpers ----------
info()  { echo "[setup] $*"; }
error() { echo "[setup] ERROR: $*" >&2; }
warn()  { echo "[setup] WARNING: $*" >&2; }

win_cmd() {
    cmd.exe /C "$@" 2>/dev/null
}

# ---------- Check winget availability ----------
info "Checking winget availability..."
if ! win_cmd "winget --version" | grep -q "v"; then
    error "winget.exe not available from WSL2."
    error "Ensure Windows App Installer is installed on the Windows side."
    exit 1
fi
WINGET_VER=$(win_cmd "winget --version" | tr -d '\r')
info "Found winget ${WINGET_VER}"

# ---------- Check Visual Studio 2022 ----------
info "Checking Visual Studio 2022..."
VS_FOUND=false
for edition in Community Professional Enterprise; do
    VCVARSALL="/mnt/c/Program Files/Microsoft Visual Studio/2022/${edition}/VC/Auxiliary/Build/vcvarsall.bat"
    if [ -f "${VCVARSALL}" ]; then
        info "Found VS2022 ${edition}"
        VS_FOUND=true
        break
    fi
done

if [ "${VS_FOUND}" = false ]; then
    error "Visual Studio 2022 not found."
    error "Install with: winget install Microsoft.VisualStudio.2022.Community"
    error "  (include 'Desktop development with C++' workload)"
    exit 1
fi

# ---------- Install packages ----------
install_if_needed() {
    local pkg_id="$1"
    local pkg_name="$2"

    info "Checking ${pkg_name} (${pkg_id})..."
    if win_cmd "winget list --id ${pkg_id}" 2>/dev/null | grep -q "${pkg_id}"; then
        info "  ${pkg_name} already installed."
        return 0
    fi

    info "  Installing ${pkg_name}..."
    if win_cmd "winget install --id ${pkg_id} --silent --accept-package-agreements --accept-source-agreements"; then
        info "  ${pkg_name} installed successfully."
    else
        warn "  winget install returned non-zero. Verifying..."
    fi
}

install_if_needed "KhronosGroup.VulkanSDK" "Vulkan SDK"
install_if_needed "Git.Git" "Git for Windows"

# ---------- Verify Vulkan SDK ----------
info "Verifying Vulkan SDK installation..."

VULKAN_BASE="/mnt/c/VulkanSDK"
if [ ! -d "${VULKAN_BASE}" ]; then
    error "Vulkan SDK directory not found at ${VULKAN_BASE}"
    error "Try installing manually: https://vulkan.lunarg.com/sdk/home"
    exit 1
fi

# Find the latest version
VULKAN_VERSION=$(ls -1 "${VULKAN_BASE}" | sort -V | tail -1)
VULKAN_SDK="${VULKAN_BASE}/${VULKAN_VERSION}"
GLSLC="${VULKAN_SDK}/Bin/glslc.exe"

if [ ! -f "${GLSLC}" ]; then
    error "glslc.exe not found at ${GLSLC}"
    error "Vulkan SDK may be partially installed. Try reinstalling."
    exit 1
fi
info "Vulkan SDK ${VULKAN_VERSION} verified (glslc.exe found)"

# ---------- Verify Git ----------
info "Verifying Git for Windows..."
if win_cmd "git --version" | grep -q "git version"; then
    GIT_VER=$(win_cmd "git --version" | tr -d '\r')
    info "Git verified: ${GIT_VER}"
else
    # Try standard install path
    if [ -f "/mnt/c/Program Files/Git/cmd/git.exe" ]; then
        info "Git found at standard path (may need PATH refresh)"
    else
        error "Git for Windows not found."
        error "Try: cmd.exe /C \"winget install --id Git.Git --silent\""
        exit 1
    fi
fi

# ---------- Summary ----------
echo ""
info "============================================"
info "Setup complete! Summary:"
info "  VS2022:     $(basename "$(dirname "$(dirname "$(dirname "${VCVARSALL}")")")")"
info "  Vulkan SDK: ${VULKAN_VERSION}"
info "  glslc:      $(wslpath -w "${GLSLC}")"
info "  Git:        OK"
info "============================================"
info ""
info "Next step: ./scripts/build-windows.sh"
