#!/usr/bin/env bash
# ==============================================================================
# EverLive Experimental (1.4 Lab) - user-local, side-by-side installer
# ==============================================================================
# Installs ONLY into the current user's home directory:
#
#   ~/.local/share/applications/everlive-experimental.desktop
#
# It never touches:
#
#   ~/.local/share/applications/everlive.desktop   (stable EverLive launcher)
#   ~/.config/everlive/                            (stable settings + log)
#   the stable everlive.sock / everlive.sock.lock  (Experimental uses
#                                                   everlive-experimental.sock)
#
# No sudo is required: udev/VFIO/PipeWire rules and the memlock limits were
# already installed by the stable EverLive release.  Running the script with
# sudo is supported; the real user is then resolved from SUDO_USER.
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
USERD_DIR="${SCRIPT_DIR}/tools/opengc573-userd"
VIEWER_DIR="${SCRIPT_DIR}/tools/everlive"

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

WANT_FRAMEGEN="ask"

usage() {
    cat <<EOF
Usage: ./install-experimental.sh [OPTIONS]

  --with-framegen
      Download and install the optional lsfg-vk 2.0 Vulkan layer into
      ~/.local without prompting (skipped automatically if already installed).

  --no-framegen, --skip-framegen
      Skip installing the optional lsfg-vk 2.0 Vulkan layer without prompting.

  -h, --help
      Show this help and exit.
EOF
}

for arg in "$@"; do
    case "${arg}" in
        --with-framegen)               WANT_FRAMEGEN=1 ;;
        --no-framegen|--skip-framegen) WANT_FRAMEGEN=0 ;;
        -h|--help)                     usage; exit 0 ;;
        *)
            echo "Unknown option: ${arg}" >&2
            usage >&2
            exit 2
            ;;
    esac
done

echo -e "${BLUE}===================================================================${NC}"
echo -e "${BLUE}   EverLive Experimental (1.4 Lab) - user-local installer         ${NC}"
echo -e "${BLUE}===================================================================${NC}"

# 1. Resolve the real user.  sudo changes HOME to /root, which would put the
#    launcher into the wrong application menu.
if [ "${EUID}" -eq 0 ]; then
    TARGET_USER="${SUDO_USER:-root}"
else
    TARGET_USER="${USER:-$(id -un)}"
fi
TARGET_HOME="$(getent passwd "${TARGET_USER}" 2>/dev/null | cut -d: -f6 || true)"
if [ -z "${TARGET_HOME}" ]; then
    TARGET_HOME="${HOME}"
fi

echo -e "[*] Repository : ${SCRIPT_DIR}"
echo -e "[*] Target user: ${GREEN}${TARGET_USER}${NC} (${TARGET_HOME})"

if [ "${WANT_FRAMEGEN}" = "ask" ]; then
    if [ -t 0 ]; then
        echo ""
        read -r -p "Install optional 2x Frame Generation layer (lsfg-vk 2.0 into ~/.local if missing)? [y/N] " answer || true
        case "${answer:-n}" in
            [yY]|[jJ]*) WANT_FRAMEGEN=1 ;;
            *)          WANT_FRAMEGEN=0 ;;
        esac
    else
        WANT_FRAMEGEN=0
    fi
fi

has_lsfg_vk_layer() {
    [ -f "/usr/share/vulkan/implicit_layer.d/VkLayer_LSFGVK_frame_generation.json" ] ||
    [ -f "/etc/vulkan/implicit_layer.d/VkLayer_LSFGVK_frame_generation.json" ] ||
    [ -f "${TARGET_HOME}/.local/share/vulkan/implicit_layer.d/VkLayer_LSFGVK_frame_generation.json" ]
}

ensure_user_lsfg_vk() {
    echo -e "\n${BLUE}[*] Checking optional lsfg-vk 2.0 layer...${NC}"
    if has_lsfg_vk_layer; then
        echo -e "${GREEN}[+] lsfg-vk 2.0 layer already present.${NC}"
        return 0
    fi
    if ! command -v curl >/dev/null 2>&1 || ! command -v tar >/dev/null 2>&1; then
        echo -e "${YELLOW}[!] curl or tar missing; install lsfg-vk via package manager or https://lsfg-vk.dev/docs/installation/${NC}"
        return 0
    fi

    local archive_rel=""
    local tmp_tar=""
    archive_rel="$(curl -fsSL --max-time 10 https://builds.lsfg-vk.dev/ 2>/dev/null | \
        sed -n 's/.*href="\([^"]*\.tar\.xz\)">&gt;&gt; Latest release.*/\1/p' | head -n 1 || true)"
    [ -z "${archive_rel}" ] && archive_rel="lsfg-vk-2.0.0.tar.xz"

    echo -e "[*] Downloading official ${archive_rel} into ${TARGET_HOME}/.local..."
    tmp_tar="$(mktemp /tmp/everlive-lsfg-vk.XXXXXX.tar.xz)"
    if curl -fL --max-time 60 "https://builds.lsfg-vk.dev/${archive_rel}" -o "${tmp_tar}" &&
       tar -tJf "${tmp_tar}" >/dev/null 2>&1; then
        chmod 644 "${tmp_tar}"
        if [ "${EUID}" -eq 0 ] && [ "${TARGET_USER}" != "root" ]; then
            sudo -u "${TARGET_USER}" mkdir -p "${TARGET_HOME}/.local"
            sudo -u "${TARGET_USER}" tar -xJf "${tmp_tar}" -C "${TARGET_HOME}/.local" || true
        else
            mkdir -p "${TARGET_HOME}/.local"
            tar -xJf "${tmp_tar}" -C "${TARGET_HOME}/.local" || true
        fi
    fi
    rm -f "${tmp_tar}"

    if has_lsfg_vk_layer; then
        echo -e "${GREEN}[+] lsfg-vk 2.0 extracted to ${TARGET_HOME}/.local.${NC}"
    else
        echo -e "${YELLOW}[!] Automatic lsfg-vk download failed; see https://lsfg-vk.dev/docs/installation/${NC}"
    fi
}

if [ "${WANT_FRAMEGEN}" -eq 1 ]; then
    ensure_user_lsfg_vk
fi

# 2. Build the userspace VFIO engine.  This is the same blob-free engine the
#    stable release uses; the Experimental branch adds lsfg-vk frame
#    generation on top, so both can use the hardware (never at the same time).
echo -e "\n${BLUE}[1/3] Building the userspace VFIO engine...${NC}"
make -C "${USERD_DIR}" -j"$(nproc)"
chmod +x "${SCRIPT_DIR}/play.sh" 2>/dev/null || true

if [ "${EUID}" -eq 0 ]; then
    # Only meaningful when run as root; the stable install already set this.
    setcap cap_ipc_lock=+ep "${USERD_DIR}/gc573_user_streamer" 2>/dev/null || true
fi
echo -e "${GREEN}[+] Engine built.${NC}"

# 3. Icons.  Icon=everlive resolves through the stable release's icon theme
#    entries, so only install the copies that are still missing.
echo -e "\n${BLUE}[2/3] Installing application icon (missing entries only)...${NC}"
ICONS_BASE="${TARGET_HOME}/.local/share/icons/hicolor"
for sz in 16 32 48 64 128 256 512; do
    src="${VIEWER_DIR}/icons/${sz}x${sz}/apps/everlive.png"
    if [ -f "${src}" ] && [ ! -f "${ICONS_BASE}/${sz}x${sz}/apps/everlive.png" ]; then
        mkdir -p "${ICONS_BASE}/${sz}x${sz}/apps"
        cp -f "${src}" "${ICONS_BASE}/${sz}x${sz}/apps/everlive.png"
        if [ "${EUID}" -eq 0 ]; then
            chown "${TARGET_USER}:${TARGET_USER}" \
                "${ICONS_BASE}/${sz}x${sz}/apps/everlive.png" 2>/dev/null || true
        fi
    fi
done
src="${VIEWER_DIR}/icons/scalable/apps/everlive.png"
if [ -f "${src}" ] && [ ! -f "${ICONS_BASE}/scalable/apps/everlive.png" ]; then
    mkdir -p "${ICONS_BASE}/scalable/apps"
    cp -f "${src}" "${ICONS_BASE}/scalable/apps/everlive.png"
    if [ "${EUID}" -eq 0 ]; then
        chown "${TARGET_USER}:${TARGET_USER}" \
            "${ICONS_BASE}/scalable/apps/everlive.png" 2>/dev/null || true
    fi
fi
echo -e "${GREEN}[+] Icons ready.${NC}"

# 4. The launcher.  A distinct filename, a distinct WM class and a distinct
#    display name keep both apps apart in every application menu.
echo -e "\n${BLUE}[3/3] Installing the Experimental desktop starter...${NC}"
APPS_DIR="${TARGET_HOME}/.local/share/applications"
mkdir -p "${APPS_DIR}"
DESKTOP_FILE="${APPS_DIR}/everlive-experimental.desktop"

case "${SCRIPT_DIR}" in
    *" "*)
        echo -e "${YELLOW}[!] The repository path contains a space; quoting it in Exec=.${NC}"
        EXEC_CMD="\"${SCRIPT_DIR}/play.sh\""
        ;;
    *)
        EXEC_CMD="${SCRIPT_DIR}/play.sh"
        ;;
esac

cat > "${DESKTOP_FILE}" <<EOF
[Desktop Entry]
Name=EverLive Experimental
GenericName=Console Gaming Screen (Experimental FrameGen)
Comment=EverLive 1.4 Lab - Lossless Scaling 2x Console Frame Generation
Exec=${EXEC_CMD}
Icon=everlive
Terminal=false
Type=Application
Categories=Game;AudioVideo;Video;
Keywords=EverLive;Experimental;FrameGen;LSFG;Vulkan;PS5;Capture;AVerMedia;GC573;HDMI;Gaming;Console;VFIO;
StartupWMClass=EverLiveExperimental
EOF
chmod +x "${DESKTOP_FILE}"
if [ "${EUID}" -eq 0 ]; then
    chown "${TARGET_USER}:${TARGET_USER}" "${DESKTOP_FILE}" 2>/dev/null || true
fi

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "${APPS_DIR}" >/dev/null 2>&1 || true
fi

# 5. Never, ever remove or rewrite the stable launcher.
if [ -f "${APPS_DIR}/everlive.desktop" ]; then
    echo -e "${GREEN}[+] Stable launcher left untouched: ${APPS_DIR}/everlive.desktop${NC}"
else
    echo -e "${YELLOW}[*] No stable everlive.desktop found; this is a standalone install.${NC}"
fi

echo -e "\n${GREEN}===================================================================${NC}"
echo -e "${GREEN}      EverLive Experimental is ready in your application menu.     ${NC}"
echo -e "${GREEN}===================================================================${NC}"
echo -e "Launcher : ${BLUE}${DESKTOP_FILE}${NC}"
echo -e "Config   : ${BLUE}${TARGET_HOME}/.config/everlive-experimental/${NC}"
echo -e "Socket   : ${BLUE}\$XDG_RUNTIME_DIR/everlive-experimental.sock${NC}"
echo -e ""
echo -e "Frame generation hotkeys in the game window:"
echo -e "  ${GREEN}g${NC}  cycle Off -> 60->120 FPS -> 30->60 FPS"
echo -e ""
echo -e "Requires the lsfg-vk 2.0 Vulkan layer and Lossless Scaling in Steam"
echo -e "(Properties -> Betas -> 'lsfg-vk'); the Hub shows a readiness line."
echo -e ""
echo -e "The stable EverLive launcher, config and socket remain untouched."
