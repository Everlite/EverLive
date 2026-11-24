#!/usr/bin/env bash
# ==============================================================================
# EverLive v2.0.1 - 1-Click Installer für AVerMedia Live Gamer 4K (GC573) & Live Gamer DUO (GC570D)
# ==============================================================================
# Installiert den 100% blob-freien Userspace VFIO-Treiber, udev-Hardware-Rechte,
# cap_ipc_lock für die Engines und die EverLive Desktop-App für HDMI Console Gaming.
#
# Kein DKMS, keine Kernel-Header, kein Kernel-Neukompilieren für EverLive selbst!
# Optional (nur Virtual Camera): v4l2loopback aus dem Distributions-Repo.
# 100% Secure Boot & Anti-Cheat (EAC v2) kompatibel.
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE_DIR="${SCRIPT_DIR}/tools/libeverlive-core"
USERD_DIR="${SCRIPT_DIR}/tools/opengc573-userd"
GC570D_DIR="${SCRIPT_DIR}/tools/opengc570d-userd"
HWS_DIR="${SCRIPT_DIR}/tools/openhws-userd"
VIEWER_DIR="${SCRIPT_DIR}/tools/everlive"

# System-wide application copy produced by `make install-app`.  Both the
# `everlive` launcher and the menu entry point here, so re-running install.sh
# always replaces a previously installed Hub instead of leaving it stale.
APP_LIBDIR="/usr/local/lib/everlive"
APP_BINDIR="/usr/local/bin"
APP_VIEWER_DIR="${APP_LIBDIR}/tools/everlive"

# ANSI Farben
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${BLUE}===================================================================${NC}"
echo -e "${BLUE}   EverLive (2.0.1) - GC573 & GC570D Gaming Suite Installer   ${NC}"
echo -e "${BLUE}===================================================================${NC}"

# Optionale Virtual Camera: standardmäßig ja, per --no-loopback abschaltbar.
WANT_LOOPBACK=1
# Optionale 2x Frame Generation (lsfg-vk 2.0): standardmäßig interaktiv [y/N],
# in nicht-interaktiven Läufen aus (oder explizit per --with-framegen).
WANT_FRAMEGEN="ask"
# Optionale LD_PRELOAD-Kamera-Starter für Discord, OBS und Browser:
# standardmäßig interaktiv [y/N] (oder explizit per --with-preload).
WANT_PRELOAD="ask"

usage() {
    cat <<EOF
Usage: sudo ./install.sh [OPTIONS]

  --no-loopback, --skip-loopback
      Do not install or configure v4l2loopback. The optional Virtual Camera
      (/dev/video10 for OBS) stays disabled and the system remains free of
      DKMS modules and kernel headers.

  --with-framegen
      Install the optional lsfg-vk 2.0 Vulkan layer for 2x Console Frame
      Generation (60->120 / 30->60 FPS) without prompting.

  --no-framegen, --skip-framegen
      Skip installing the optional lsfg-vk 2.0 Vulkan layer without prompting.

  --with-preload, --preload-apps
      Create reversible "(EverLive Camera)" application-menu launchers for
      Discord, OBS, Chrome, Chromium, Brave, Firefox and Zoom. Each launcher
      starts that app with the card-specific LD_PRELOAD virtual camera.

  --no-preload, --skip-preload
      Skip the LD_PRELOAD app launchers without prompting. Print manual
      terminal launch instructions instead.

  -h, --help
      Show this help and exit.
EOF
}

parse_args() {
    for arg in "$@"; do
        case "$arg" in
            --no-loopback|--skip-loopback)
                WANT_LOOPBACK=0
                ;;
            --with-framegen)
                WANT_FRAMEGEN=1
                ;;
            --no-framegen|--skip-framegen)
                WANT_FRAMEGEN=0
                ;;
            --with-preload|--preload-apps)
                WANT_PRELOAD=1
                ;;
            --no-preload|--skip-preload)
                WANT_PRELOAD=0
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                echo "Unbekannte Option: $arg" >&2
                usage >&2
                exit 2
                ;;
        esac
    done
}

# Interaktive Abfrage (nur mit TTY). Piped/CI-Läufe behalten den Default.
ask_virtual_camera() {
    if [ "$WANT_LOOPBACK" -ne 1 ] || [ ! -t 0 ]; then
        return 0
    fi

    local answer=""

    read -r -p "Install optional Virtual Camera (/dev/video10 for OBS) via distribution v4l2loopback? [Y/n] " answer || true
    case "${answer:-y}" in
        [nN]*) WANT_LOOPBACK=0 ;;
    esac
}

ask_framegen() {
    if [ "$WANT_FRAMEGEN" != "ask" ]; then
        return 0
    fi
    if [ ! -t 0 ]; then
        WANT_FRAMEGEN=0
        return 0
    fi

    local answer=""

    echo ""
    read -r -p "Install optional 2x Frame Generation layer (lsfg-vk 2.0 for 60->120 / 30->60 FPS)? [y/N] " answer || true
    case "${answer:-n}" in
        [yY]|[jJ]*) WANT_FRAMEGEN=1 ;;
        *)          WANT_FRAMEGEN=0 ;;
    esac
}

ask_preload() {
    if [ "$WANT_PRELOAD" != "ask" ]; then
        return 0
    fi
    if [ ! -t 0 ]; then
        WANT_PRELOAD=0
        return 0
    fi

    local answer=""

    echo ""
    read -r -p "Create LD_PRELOAD camera launchers for Discord, OBS and browsers? [y/N] " answer || true
    case "${answer:-n}" in
        [yY]|[jJ]*) WANT_PRELOAD=1 ;;
        *)          WANT_PRELOAD=0 ;;
    esac
}

# Vor der Root-Prüfung parsen, damit --help kein sudo benötigt. Der
# sudo-Neustart übergibt "$@", wodurch das Kind erneut parst.
parse_args "$@"

# 1. Root-Prüfung / Sudo
if [ "$EUID" -ne 0 ]; then
    echo -e "${YELLOW}[*] Für die System- und Hardware-Rechte-Installation werden Root-Rechte benötigt.${NC}"
    echo -e "${YELLOW}[*] Starte mit sudo neu...${NC}"
    if ! command -v sudo >/dev/null 2>&1; then
        echo -e "${RED}[!] sudo ist nicht installiert. Bitte als root ausführen.${NC}" >&2
        exit 1
    fi
    # Pass the invoking user explicitly: SUDO_USER is not set when the script
    # is started from a root shell or an environment that resets it.
    exec sudo env \
        "EVERLIVE_TARGET_USER=${USER}" \
        "EVERLIVE_TARGET_HOME=${HOME}" \
        bash "$0" "$@"
fi

# Echten Benutzer ermitteln (nicht root).  Prefer the explicitly forwarded
# user, then sudo's SUDO_USER, then logname.  A root shell without SUDO_USER
# falls back to the checkout owner so the desktop entry does not land in /root.
TARGET_USER="${EVERLIVE_TARGET_USER:-${SUDO_USER:-$(logname 2>/dev/null || true)}}"
if [ -z "${TARGET_USER}" ] || { [ "${TARGET_USER}" = "root" ] && \
        [ -z "${EVERLIVE_TARGET_USER:-}" ] && [ -z "${SUDO_USER:-}" ]; }; then
    CHECKOUT_OWNER="$(stat -c '%U' "${SCRIPT_DIR}" 2>/dev/null || true)"
    if [ -n "${CHECKOUT_OWNER}" ] && [ "${CHECKOUT_OWNER}" != "root" ]; then
        TARGET_USER="${CHECKOUT_OWNER}"
    fi
fi
[ -n "${TARGET_USER}" ] || TARGET_USER="${USER}"
TARGET_HOME="$(getent passwd "${TARGET_USER}" 2>/dev/null | cut -d: -f6)"
[ -n "${TARGET_HOME}" ] || TARGET_HOME="${EVERLIVE_TARGET_HOME:-${HOME}}"

echo -e "[*] Ziel-Benutzer: ${GREEN}${TARGET_USER}${NC}"
echo -e "[*] Ziel-Home   : ${GREEN}${TARGET_HOME}${NC}"
echo -e "[*] Linux Kernel : ${GREEN}$(uname -r)${NC}"
echo -e "[*] Architektur  : ${GREEN}100% Userspace VFIO (Blob-Free Ring 3)${NC}"

ask_virtual_camera
if [ "$WANT_LOOPBACK" -eq 0 ]; then
    echo -e "${GREEN}[*] Virtual Camera installation skipped. EverLive remains 100% module-free.${NC}"
fi

ask_framegen
if [ "$WANT_FRAMEGEN" -eq 0 ]; then
    echo -e "${GREEN}[*] Optional lsfg-vk Frame Generation download skipped (can be installed anytime later).${NC}"
fi

ask_preload
if [ "$WANT_PRELOAD" -eq 0 ]; then
    echo -e "${GREEN}[*] LD_PRELOAD app launchers skipped (manual launch instructions will be shown).${NC}"
fi

# 2. Paketmanager & Abhängigkeiten erkennen

# Optionales Virtual-Camera-Modul. Ohne v4l2loopback funktioniert EverLive
# vollständig, nur der /dev/video10-Ausgang bleibt deaktiviert; der Installer
# darf daran niemals scheitern (set -e).
ensure_v4l2loopback() {
    echo -e "\n${BLUE}[*] Prüfe Virtual-Camera-Modul (v4l2loopback)...${NC}"

    if modinfo v4l2loopback >/dev/null 2>&1; then
        echo -e "${GREEN}[+] v4l2loopback ist bereits verfügbar.${NC}"
        return 0
    fi

    if command -v pacman >/dev/null 2>&1; then
        if pacman -Si v4l2loopback-dkms >/dev/null 2>&1; then
            local -a V4L2_PKGS=(v4l2loopback-dkms)
            # DKMS needs headers matching the running kernel flavour.
            for kernel_pkg in linux linux-lts linux-zen linux-hardened; do
                if pacman -Q "${kernel_pkg}" >/dev/null 2>&1 &&
                   ! pacman -Q "${kernel_pkg}-headers" >/dev/null 2>&1 &&
                   pacman -Si "${kernel_pkg}-headers" >/dev/null 2>&1; then
                    V4L2_PKGS+=("${kernel_pkg}-headers")
                fi
            done
            pacman -S --needed --noconfirm "${V4L2_PKGS[@]}" || true
        elif command -v yay >/dev/null 2>&1; then
            sudo -u "${TARGET_USER}" yay -S --needed --noconfirm v4l2loopback-dkms || true
        elif command -v paru >/dev/null 2>&1; then
            sudo -u "${TARGET_USER}" paru -S --needed --noconfirm v4l2loopback-dkms || true
        fi
    elif command -v apt-get >/dev/null 2>&1; then
        local -a V4L2_PKGS=(v4l2loopback-dkms v4l2loopback-utils)
        if apt-cache show "linux-headers-$(uname -r)" >/dev/null 2>&1; then
            V4L2_PKGS+=("linux-headers-$(uname -r)")
        fi
        apt-get install -y "${V4L2_PKGS[@]}" || true
    elif command -v dnf >/dev/null 2>&1; then
        # akmod-v4l2loopback kommt aus RPM Fusion.
        dnf install -y akmod-v4l2loopback || true
    fi

    if modinfo v4l2loopback >/dev/null 2>&1; then
        echo -e "${GREEN}[+] v4l2loopback installiert.${NC}"
        return 0
    fi

    echo -e "${YELLOW}[!] v4l2loopback konnte nicht automatisch installiert werden.${NC}"
    echo -e "${YELLOW}    EverLive läuft vollständig, nur die Virtual Camera (/dev/video10) bleibt aus.${NC}"
    echo -e "${YELLOW}    Debian/Ubuntu: sudo apt install v4l2loopback-dkms v4l2loopback-utils linux-headers-\$(uname -r)${NC}"
    echo -e "${YELLOW}    Fedora:        sudo dnf install akmod-v4l2loopback   (RPM Fusion)${NC}"
    echo -e "${YELLOW}    Arch:          yay -S v4l2loopback-dkms${NC}"
    return 0
}

# Prüft, ob das lsfg-vk 2.0 Implicit-Layer-Manifest bereits vorhanden ist.
has_lsfg_vk_layer() {
    [ -f "/usr/share/vulkan/implicit_layer.d/VkLayer_LSFGVK_frame_generation.json" ] ||
    [ -f "/etc/vulkan/implicit_layer.d/VkLayer_LSFGVK_frame_generation.json" ] ||
    [ -f "${TARGET_HOME}/.local/share/vulkan/implicit_layer.d/VkLayer_LSFGVK_frame_generation.json" ]
}

# Prüft den Steam-Status von Lossless Scaling (lsfg-vk.dll vs. Windows Lossless.dll)
# für eine präzise Rückmeldung am Ende der Installation.
check_steam_lsfg_dll() {
    LSFG_STEAM_STATUS="missing"
    LSFG_STEAM_DLL_PATH=""

    local -a roots=(
        "${TARGET_HOME}/.local/share/Steam"
        "${TARGET_HOME}/.steam/steam"
    )
    local -a libs=()
    local root vdf line lib game_dir

    for root in "${roots[@]}"; do
        [ -d "${root}" ] && libs+=("${root}")
        vdf="${root}/steamapps/libraryfolders.vdf"
        if [ -r "${vdf}" ]; then
            while IFS= read -r line; do
                lib="$(printf '%s\n' "${line}" | sed -n 's/.*"path"[[:space:]]*"\([^"]*\)".*/\1/p' | sed 's/\\\\/\//g')"
                [ -n "${lib}" ] && libs+=("${lib}")
            done < "${vdf}"
        fi
    done

    for lib in "${libs[@]}"; do
        game_dir="${lib}/steamapps/common/Lossless Scaling"
        if [ -r "${game_dir}/lsfg-vk.dll" ]; then
            LSFG_STEAM_STATUS="ready"
            LSFG_STEAM_DLL_PATH="${game_dir}/lsfg-vk.dll"
            return 0
        fi
        if [ -r "${game_dir}/Lossless.dll" ]; then
            LSFG_STEAM_STATUS="wrong_branch"
        fi
    done
    return 0
}

# Gepinnte optionale lsfg-vk Release-Version (Version + SHA-256 immer gemeinsam aktualisieren; siehe docs/INSTALL.md)
LSFG_VK_VERSION="2.0.0"
LSFG_VK_ARCHIVE="lsfg-vk-${LSFG_VK_VERSION}.tar.xz"
LSFG_VK_URL="https://builds.lsfg-vk.dev/${LSFG_VK_ARCHIVE}"
LSFG_VK_SHA256="08bdbdf373a111022df87dac7aa87e3b564bb841f961552e3ca85fea12b5aa74"

_install_pinned_lsfg_vk() {
    local archive_url="${LSFG_VK_URL}"
    local expected_sha="${LSFG_VK_SHA256}"
    local archive_name="${LSFG_VK_ARCHIVE}"
    local tmp_dir=""
    local tmp_tar=""
    local tar_list=""

    if ! [[ "${expected_sha}" =~ ^[a-fA-F0-9]{64}$ ]]; then
        echo -e "${RED}[!] Ungültige gepinnte SHA-256-Prüfsumme für ${archive_name} — überspringe lsfg-vk.${NC}"
        return 1
    fi

    if ! command -v sha256sum >/dev/null 2>&1; then
        echo -e "${YELLOW}[!] sha256sum nicht verfügbar — überspringe unverifizierten Download von ${archive_name}.${NC}"
        return 1
    fi

    tmp_dir="$(mktemp -d)"
    chmod 700 "${tmp_dir}"
    trap 'rm -rf -- "${tmp_dir}"' RETURN
    tmp_tar="${tmp_dir}/${archive_name}"

    echo -e "[*] Lade gepinntes lsfg-vk Paket (${archive_name})..."
    if ! curl --proto '=https' --tlsv1.2 --fail --location --silent --show-error \
            --max-time 60 "${archive_url}" -o "${tmp_tar}"; then
        echo -e "${YELLOW}[!] Download von ${archive_name} fehlgeschlagen — überspringe lsfg-vk.${NC}"
        return 1
    fi

    if ! printf '%s  %s\n' "${expected_sha}" "${tmp_tar}" | sha256sum -c - >/dev/null 2>&1; then
        echo -e "${RED}[!] SHA-256-Prüfung für ${archive_name} fehlgeschlagen — überspringe lsfg-vk.${NC}"
        return 1
    fi

    if ! tar_list="$(tar -tJf "${tmp_tar}" 2>/dev/null)" || [ -z "${tar_list}" ] ||
       printf '%s\n' "${tar_list}" | grep -qE '(^/|(^|/)\.\.(/|$))'; then
        echo -e "${RED}[!] Ungültige Archivpfade in ${archive_name} — überspringe lsfg-vk.${NC}"
        return 1
    fi

    if [ "${TARGET_USER}" != "root" ]; then
        chown -R "${TARGET_USER}" "${tmp_dir}"
        sudo -u "${TARGET_USER}" mkdir -p "${TARGET_HOME}/.local"
        sudo -u "${TARGET_USER}" tar --no-same-owner --no-same-permissions -xJf "${tmp_tar}" -C "${TARGET_HOME}/.local"
    else
        mkdir -p "${TARGET_HOME}/.local"
        tar --no-same-owner --no-same-permissions -xJf "${tmp_tar}" -C "${TARGET_HOME}/.local"
    fi
}

# Optionaler 2x Frame-Generation-Layer (lsfg-vk 2.0). Darf den Installer niemals
# abbrechen: bei Fehlern bleibt EverLive im nativen Modus voll funktionsfähig.
ensure_lsfg_vk() {
    echo -e "\n${BLUE}[*] Prüfe optionalen Frame-Generation-Layer (lsfg-vk ${LSFG_VK_VERSION})...${NC}"

    if has_lsfg_vk_layer; then
        echo -e "${GREEN}[+] lsfg-vk 2.0 Vulkan-Layer ist bereits installiert.${NC}"
    else
        # 1. Vulkan-Loader + Download/Entpack-Werkzeuge sicherstellen
        if command -v pacman >/dev/null 2>&1; then
            pacman -S --needed --noconfirm vulkan-icd-loader curl xz >/dev/null 2>&1 || true
        elif command -v apt-get >/dev/null 2>&1; then
            apt-get install -y libvulkan1 curl xz-utils >/dev/null 2>&1 || true
        elif command -v dnf >/dev/null 2>&1; then
            dnf install -y vulkan-loader curl xz >/dev/null 2>&1 || true
        fi

        # 2. Distributions-Paket bevorzugen (z. B. CachyOS Repo oder AUR auf Arch)
        if command -v pacman >/dev/null 2>&1; then
            if pacman -Si lsfg-vk >/dev/null 2>&1; then
                echo -e "[*] Installiere lsfg-vk direkt aus dem Paket-Repository..."
                pacman -S --needed --noconfirm lsfg-vk || true
            elif [ "${TARGET_USER}" != "root" ] && command -v yay >/dev/null 2>&1; then
                echo -e "[*] Installiere lsfg-vk über yay (AUR)..."
                sudo -u "${TARGET_USER}" yay -S --needed --noconfirm lsfg-vk-bin ||
                sudo -u "${TARGET_USER}" yay -S --needed --noconfirm lsfg-vk || true
            elif [ "${TARGET_USER}" != "root" ] && command -v paru >/dev/null 2>&1; then
                echo -e "[*] Installiere lsfg-vk über paru (AUR)..."
                sudo -u "${TARGET_USER}" paru -S --needed --noconfirm lsfg-vk-bin ||
                sudo -u "${TARGET_USER}" paru -S --needed --noconfirm lsfg-vk || true
            fi
        fi

        # 3. Universeller Fallback (Ubuntu/Debian/Fedora/Arch ohne AUR-Helper):
        #    Gepinntes lsfg-vk Release-Archiv mit harter SHA-256-Prüfung (fail-closed)
        if ! has_lsfg_vk_layer && command -v curl >/dev/null 2>&1 && command -v tar >/dev/null 2>&1; then
            _install_pinned_lsfg_vk || true
        fi

        if has_lsfg_vk_layer; then
            echo -e "${GREEN}[+] lsfg-vk 2.0 Vulkan-Layer erfolgreich eingerichtet!${NC}"
        else
            echo -e "${YELLOW}[!] lsfg-vk 2.0 konnte nicht automatisch geladen werden.${NC}"
            echo -e "${YELLOW}    Manuelle Anleitung: https://lsfg-vk.dev/docs/installation/${NC}"
            return 0
        fi
    fi

    check_steam_lsfg_dll
    case "${LSFG_STEAM_STATUS}" in
        ready)
            echo -e "${GREEN}[+] Steam-Shader gefunden: ${LSFG_STEAM_DLL_PATH}${NC}"
            ;;
        wrong_branch)
            echo -e "${YELLOW}[!] Lossless Scaling in Steam gefunden, aber 'lsfg-vk.dll' fehlt noch:${NC}"
            echo -e "${YELLOW}    Steam -> Rechtsklick auf 'Lossless Scaling' -> Eigenschaften -> Betas -> 'lsfg-vk' auswählen.${NC}"
            ;;
        *)
            echo -e "${YELLOW}[*] Hinweis für Frame Generation: 'Lossless Scaling' in Steam installieren und${NC}"
            echo -e "${YELLOW}    unter Eigenschaften -> Betas den Branch 'lsfg-vk' aktivieren (danach im Hub 'Re-check' klicken).${NC}"
            ;;
    esac
    return 0
}

detect_and_install_deps() {
    echo -e "\n${BLUE}[1/5] Prüfe Software-Abhängigkeiten...${NC}"
    
    MISSING_PKGS=()
    
    if command -v pacman >/dev/null 2>&1; then
        # Arch / CachyOS / Manjaro
        ! command -v gcc >/dev/null 2>&1 && MISSING_PKGS+=(base-devel)
        ! command -v sdl2-config >/dev/null 2>&1 && MISSING_PKGS+=(sdl2)
        ! pkg-config --exists SDL2_ttf >/dev/null 2>&1 && MISSING_PKGS+=(sdl2_ttf)
        ! pkg-config --exists libavcodec libswresample libswscale >/dev/null 2>&1 && MISSING_PKGS+=(ffmpeg)
        ! pkg-config --exists libpipewire-0.3 >/dev/null 2>&1 && MISSING_PKGS+=(pipewire)
        ! command -v setcap >/dev/null 2>&1 && MISSING_PKGS+=(libcap)
        ! command -v wpctl >/dev/null 2>&1 && MISSING_PKGS+=(wireplumber)
        ! pacman -Q xcb-util-cursor >/dev/null 2>&1 && MISSING_PKGS+=(xcb-util-cursor)
        ! sudo -u "${TARGET_USER}" python3 -c 'import PySide6' >/dev/null 2>&1 && MISSING_PKGS+=(pyside6)
        
        if [ ${#MISSING_PKGS[@]} -gt 0 ]; then
            echo -e "${YELLOW}[!] Fehlende Pakete gefunden: ${MISSING_PKGS[*]}${NC}"
            pacman -S --needed --noconfirm "${MISSING_PKGS[@]}"
        fi
    elif command -v apt-get >/dev/null 2>&1; then
        # Ubuntu / Debian / Pop!_OS / Mint
        ! command -v gcc >/dev/null 2>&1 && MISSING_PKGS+=(build-essential pkg-config)
        ! command -v sdl2-config >/dev/null 2>&1 && MISSING_PKGS+=(libsdl2-dev)
        ! pkg-config --exists SDL2_ttf >/dev/null 2>&1 && MISSING_PKGS+=(libsdl2-ttf-dev)
        ! pkg-config --exists libavcodec libswresample libswscale >/dev/null 2>&1 && MISSING_PKGS+=(libavcodec-dev libswresample-dev libswscale-dev libavutil-dev)
        ! pkg-config --exists libpipewire-0.3 >/dev/null 2>&1 && MISSING_PKGS+=(libpipewire-0.3-dev)
        ! command -v setcap >/dev/null 2>&1 && MISSING_PKGS+=(libcap2-bin)
        ! command -v wpctl >/dev/null 2>&1 && MISSING_PKGS+=(wireplumber)
        ! dpkg -s libxcb-cursor0 >/dev/null 2>&1 && MISSING_PKGS+=(libxcb-cursor0)
        ! dpkg -s python3-pip >/dev/null 2>&1 && MISSING_PKGS+=(python3-pip)
        ! dpkg -s python3-venv >/dev/null 2>&1 && MISSING_PKGS+=(python3-venv)
        
        if [ ${#MISSING_PKGS[@]} -gt 0 ]; then
            echo -e "${YELLOW}[!] Fehlende Pakete gefunden: ${MISSING_PKGS[*]}${NC}"
            apt-get update && apt-get install -y "${MISSING_PKGS[@]}"
        fi
    elif command -v dnf >/dev/null 2>&1; then
        # Fedora / Nobara
        ! command -v gcc >/dev/null 2>&1 && MISSING_PKGS+=(gcc make pkgconf-pkg-config)
        ! command -v sdl2-config >/dev/null 2>&1 && MISSING_PKGS+=(SDL2-devel)
        ! pkg-config --exists SDL2_ttf >/dev/null 2>&1 && MISSING_PKGS+=(SDL2_ttf-devel)
        ! pkg-config --exists libavcodec libswresample libswscale >/dev/null 2>&1 && MISSING_PKGS+=(ffmpeg-devel)
        ! pkg-config --exists libpipewire-0.3 >/dev/null 2>&1 && MISSING_PKGS+=(pipewire-devel)
        ! command -v setcap >/dev/null 2>&1 && MISSING_PKGS+=(libcap)
        ! command -v wpctl >/dev/null 2>&1 && MISSING_PKGS+=(wireplumber)
        ! rpm -q xcb-util-cursor >/dev/null 2>&1 && MISSING_PKGS+=(xcb-util-cursor)
        ! sudo -u "${TARGET_USER}" python3 -c 'import PySide6' >/dev/null 2>&1 && MISSING_PKGS+=(python3-pyside6)
        
        if [ ${#MISSING_PKGS[@]} -gt 0 ]; then
            echo -e "${YELLOW}[!] Fehlende Pakete gefunden: ${MISSING_PKGS[*]}${NC}"
            dnf install -y "${MISSING_PKGS[@]}"
        fi
    fi
    echo -e "${GREEN}[+] Alle benötigten Bibliotheken vorhanden!${NC}"

    if [ "$WANT_LOOPBACK" -eq 1 ]; then
        ensure_v4l2loopback
    fi

    if [ "$WANT_FRAMEGEN" -eq 1 ]; then
        ensure_lsfg_vk
    fi
}

detect_and_install_deps

# 3. Userspace VFIO Engine kompilieren
echo -e "\n${BLUE}[2/5] Kompiliere EverLive Userspace VFIO Engine...${NC}"
# Build as the checkout owner so install.sh does not leave root-owned object
# files behind.  Fall back to a root build when the checkout is not writable.
if [ "${TARGET_USER}" = "root" ]; then
    make -C "${SCRIPT_DIR}" -j"$(nproc)"
elif ! sudo -u "${TARGET_USER}" env "HOME=${TARGET_HOME}" \
        make -C "${SCRIPT_DIR}" -j"$(nproc)"; then
    make -C "${SCRIPT_DIR}" -j"$(nproc)"
fi

# The Hub itself is Python, so it needs an explicit install step.  This
# replaces the system-wide player (and the everlive launcher) with the current
# checkout instead of leaving an older copy from a previous install behind.
make -C "${SCRIPT_DIR}" install-app

# `install-app` may still touch build targets as root; hand any root-owned
# checkout files back to the invoking user.
if [ "${TARGET_USER}" != "root" ]; then
    find "${SCRIPT_DIR}" -xdev -user root \
        -exec chown "${TARGET_USER}:${TARGET_USER}" {} + 2>/dev/null || true
fi

# Never leave build artifacts world-writable, and keep binaries setcap-safe.
chmod 0755 "${USERD_DIR}/gc573_user_streamer" "${USERD_DIR}/everlive_userd" \
    "${USERD_DIR}/gc573_user_probe" "${USERD_DIR}/gc573_user_diag" "${CORE_DIR}/everlive-ctl" \
    "${GC570D_DIR}/opengc570d-userd" "${GC570D_DIR}/gc570d-ctl" \
    "${SCRIPT_DIR}/play.sh"
chmod 0644 "${GC570D_DIR}/libgc570d-v4l2.so" 2>/dev/null || true
chmod 0644 "${USERD_DIR}/libgc573-v4l2.so" 2>/dev/null || true
chmod 0644 "${USERD_DIR}/libeverlive-vk-pacer.so" 2>/dev/null || true

# Core library, everlive-ctl, everlive-service, headers, and systemd unit.
make -C "${CORE_DIR}" install

# Install the GC573 engine binaries into /usr/local/bin with cap_ipc_lock.
make -C "${USERD_DIR}" install

# Install the GC570D helpers (udev rule, systemd unit, sleep hook) through the
# engine's own Makefile so both installers stay in sync.
make -C "${GC570D_DIR}" install

# Install the shared bind helpers and compatibility wrappers.   udev rules use
# the libexec copies; the /usr/local/bin copies keep old path lookups working.
install -d /usr/local/libexec
install -m 0755 "${SCRIPT_DIR}/packaging/libexec/everlive-vfio-bind" \
    "${SCRIPT_DIR}/packaging/libexec/everlive-setcap" /usr/local/libexec/
install -m 0755 "${USERD_DIR}/vfio_pci_bind.sh" \
    "${GC570D_DIR}/gc570d-vfio-bind.sh" /usr/local/libexec/
install -m 0755 "${USERD_DIR}/vfio_pci_bind.sh" /usr/local/bin/vfio_pci_bind.sh
install -m 0755 "${GC570D_DIR}/gc570d-vfio-bind.sh" /usr/local/bin/gc570d-vfio-bind.sh

if [ -x "${SCRIPT_DIR}/packaging/libexec/everlive-setcap" ]; then
    "${SCRIPT_DIR}/packaging/libexec/everlive-setcap" \
        "${USERD_DIR}/gc573_user_streamer" \
        "${USERD_DIR}/gc573_user_probe" \
        "${USERD_DIR}/gc573_user_diag" \
        /usr/local/bin/gc573_user_streamer \
        /usr/local/bin/gc573_user_probe \
        /usr/local/bin/gc573_user_diag \
        "${GC570D_DIR}/opengc570d-userd" \
        /usr/local/bin/opengc570d-userd \
        "${HWS_DIR}/hws_user_streamer" \
        "${HWS_DIR}/hws_user_probe" \
        /usr/local/bin/hws_user_streamer \
        /usr/local/bin/hws_user_probe 2>/dev/null || true
else
    setcap cap_ipc_lock=+ep /usr/local/bin/gc573_user_streamer 2>/dev/null || true
    setcap cap_ipc_lock=+ep /usr/local/bin/opengc570d-userd 2>/dev/null || true
fi
echo -e "${GREEN}[+] Userspace VFIO Engines erfolgreich gebaut & berechtigt!${NC}"

# Python GUI-Umgebung (PySide6) für Benutzer prüfen und bei Bedarf einrichten
echo -e "\n[*] Prüfe Python-Umgebung für EverLive Studio..."
if ! sudo -u "${TARGET_USER}" python3 -c 'import PySide6' >/dev/null 2>&1; then
    echo -e "${YELLOW}[*] PySide6 nicht systemweit gefunden. Richte venv für EverLive Studio ein...${NC}"

    # The installed Hub is the canonical app, so give it its own environment.
    # Drop a dangling venv symlink left by a removed checkout first.
    if [ -L "${APP_VIEWER_DIR}/.venv" ] && [ ! -e "${APP_VIEWER_DIR}/.venv" ]; then
        rm -f "${APP_VIEWER_DIR}/.venv"
    fi
    if [ ! -d "${APP_VIEWER_DIR}/.venv" ]; then
        sudo -u "${TARGET_USER}" python3 -m venv "${APP_VIEWER_DIR}/.venv" 2>/dev/null || python3 -m venv "${APP_VIEWER_DIR}/.venv"
        chown -R "${TARGET_USER}:${TARGET_USER}" "${APP_VIEWER_DIR}/.venv" 2>/dev/null || true
    fi
    if [ -x "${APP_VIEWER_DIR}/.venv/bin/pip" ]; then
        sudo -u "${TARGET_USER}" "${APP_VIEWER_DIR}/.venv/bin/pip" install --upgrade pip PySide6 2>/dev/null || "${APP_VIEWER_DIR}/.venv/bin/pip" install --upgrade pip PySide6
    fi
    # Let `./play.sh` reuse the installed environment unless the checkout
    # already has its own venv.
    if [ -L "${VIEWER_DIR}/.venv" ] && [ ! -e "${VIEWER_DIR}/.venv" ]; then
        rm -f "${VIEWER_DIR}/.venv"
    fi
    if [ ! -e "${VIEWER_DIR}/.venv" ]; then
        ln -sfn "${APP_VIEWER_DIR}/.venv" "${VIEWER_DIR}/.venv" 2>/dev/null || true
    fi
fi
echo -e "${GREEN}[+] Python GUI-Umgebung bereit!${NC}"

# 4. Alte Kernel-Treiber Artefakte bereinigen (Migration) & Virtual Camera konfigurieren
echo -e "\n${BLUE}[3/5] Bereinige Legacy-Artefakte & konfiguriere Virtual Camera...${NC}"
rm -f /etc/modules-load.d/everlive.conf
if command -v dkms >/dev/null 2>&1; then
    dkms remove -m everlive --all 2>/dev/null || true
    rm -rf /usr/src/everlive-* 2>/dev/null || true
fi
modprobe -r open_gc573 2>/dev/null || rmmod open_gc573 2>/dev/null || true
mkdir -p /etc/modprobe.d
echo "blacklist open_gc573" > /etc/modprobe.d/blacklist-everlive-legacy.conf
echo -e "${GREEN}[+] Altes Kernel-Modul deregistriert & geblacklistet.${NC}"

# Virtual Camera: fester Node /dev/video10 mit dauerhaft sichtbaren Caps.
if [ "$WANT_LOOPBACK" -eq 0 ]; then
    # --no-loopback: keine Konfiguration hinterlassen, damit der nächste Boot
    # wirklich ohne das Modul startet.
    rm -f /etc/modules-load.d/everlive-v4l2loopback.conf
    rm -f /etc/modprobe.d/everlive-v4l2loopback.conf
    echo -e "${GREEN}[*] v4l2loopback-Konfiguration entfernt (module-free Setup).${NC}"
    echo -e "${YELLOW}    Ein bereits geladenes Modul wird erst beim nächsten Neustart freigegeben.${NC}"
elif modinfo v4l2loopback >/dev/null 2>&1; then
    mkdir -p /etc/modules-load.d /etc/modprobe.d
    echo "v4l2loopback" > /etc/modules-load.d/everlive-v4l2loopback.conf
    cat > /etc/modprobe.d/everlive-v4l2loopback.conf <<EOF
# Managed by EverLive: virtual camera nodes for OBS / Discord / FFmpeg.
# /dev/video10 carries HDMI 1 (GC570D) or the GC573 feed; /dev/video11 is
# HDMI 2 on the Live Gamer DUO.  exclusive_caps=0 keeps VIDEO_CAPTURE
# advertised while no writer is active, so OBS never drops the devices.
options v4l2loopback video_nr=10,11 card_label="EverLive Capture,EverLive Capture HDMI 2" exclusive_caps=0
EOF
    if ! lsmod | grep -q '^v4l2loopback'; then
        modprobe v4l2loopback 2>/dev/null || true
    fi
    if [ -e /dev/video10 ] && [ -e /dev/video11 ]; then
        if [ "$(cat /sys/module/v4l2loopback/parameters/exclusive_caps 2>/dev/null || echo 0)" = "1" ]; then
            echo -e "${YELLOW}[!] /dev/video10 and /dev/video11 exist, but the running instance still uses exclusive_caps=1.${NC}"
            echo -e "${YELLOW}    After a reboot (or 'sudo modprobe -r v4l2loopback && sudo modprobe v4l2loopback') exclusive_caps=0 applies.${NC}"
        else
            echo -e "${GREEN}[+] /dev/video10 'EverLive Capture' and /dev/video11 'EverLive Capture HDMI 2' ready (exclusive_caps=0).${NC}"
        fi
    elif [ -e /dev/video10 ]; then
        echo -e "${YELLOW}[!] /dev/video10 exists, but /dev/video11 (DUO HDMI 2) is missing.${NC}"
        echo -e "${YELLOW}    Reload: sudo modprobe -r v4l2loopback && sudo modprobe v4l2loopback video_nr=10,11 card_label=\"EverLive Capture,EverLive Capture HDMI 2\" exclusive_caps=0${NC}"
    else
        echo -e "${YELLOW}[!] v4l2loopback configured, but /dev/video10 and /dev/video11 are still missing (reboot required).${NC}"
    fi
else
    echo -e "${YELLOW}[!] v4l2loopback nicht verfügbar; Virtual Camera bleibt bis zur Installation aus.${NC}"
fi

# PipeWire-Sink für OBS: eigene Tonspur "EverLive Capture Audio".
# Kein Kernelmodul und keine DKMS-Abhängigkeit - reine PipeWire-Konfiguration.
ensure_pipewire_capture_sink() {
    local conf_dir="/etc/pipewire/pipewire.conf.d"

    if ! command -v pipewire >/dev/null 2>&1; then
        echo -e "${YELLOW}[!] PipeWire nicht gefunden; der OBS-Audio-Track wird übersprungen.${NC}"
        return 0
    fi

    mkdir -p "$conf_dir"

    cat > "$conf_dir/50-everlive-capture.conf" <<'EOF'
# Managed by EverLive: dedicated capture sink for OBS / Discord / FFmpeg.
# Add "Monitor of EverLive Capture Audio" as an audio source in OBS.
context.objects = [
    { factory = adapter
      args = {
          factory.name            = support.null-audio-sink
          node.name               = "everlive-capture-audio"
          node.description        = "EverLive Capture Audio"
          media.class             = Audio/Sink
          audio.position          = [ FL FR FC LFE RL RR ]
          monitor.channel-volumes = true
          node.virtual            = false
      }
    }
]
EOF

    echo -e "${GREEN}[+] PipeWire-Sink 'EverLive Capture Audio' konfiguriert.${NC}"
    echo -e "${YELLOW}    Aktivieren mit: systemctl --user restart pipewire pipewire-pulse${NC}"
}

ensure_pipewire_capture_sink

# 5. Udev-Rechte für vfio-pci einrichten (per-Karte, ohne globales new_id)
echo -e "\n${BLUE}[4/5] Richte VFIO Hardware-Zugriffsrechte (udev) & Boot-Autobind ein...${NC}"

# Boot-Autobind: vfio-pci beim Kernel-Start laden.  Die udev-Regeln binden
# danach nur die passende Karte per BDF und Subsystem an vfio-pci.
mkdir -p /etc/modules-load.d /etc/modprobe.d
cat > /etc/modules-load.d/everlive-vfio.conf <<EOF
vfio
vfio_pci
vfio_iommu_type1
EOF

# Alte globale ID-Bindung aus früheren Installationen entfernen.
rm -f /etc/modprobe.d/everlive-vfio.conf /usr/lib/modprobe.d/everlive-vfio.conf

# Kein unbegrenztes memlock mehr.  cap_ipc_lock ist der primäre Mechanismus;
# ohne setcap wird eine begrenzte Fallback-Datei installiert.
rm -f /etc/security/limits.d/99-gc570d-memlock.conf
if command -v setcap >/dev/null 2>&1; then
    rm -f /etc/security/limits.d/99-everlive-memlock.conf
    rm -f /etc/systemd/user/everlive.service.d/10-memlock.conf \
          /etc/systemd/user/opengc570d-userd.service.d/10-memlock.conf
    rm -f /usr/lib/systemd/user/everlive.service.d/10-memlock.conf \
          /usr/lib/systemd/user/opengc570d-userd.service.d/10-memlock.conf
    rmdir /etc/systemd/user/everlive.service.d \
          /etc/systemd/user/opengc570d-userd.service.d \
          /usr/lib/systemd/user/everlive.service.d \
          /usr/lib/systemd/user/opengc570d-userd.service.d 2>/dev/null || true
else
    install -Dm0644 "${SCRIPT_DIR}/packaging/99-everlive-memlock-fallback.conf" \
        /etc/security/limits.d/99-everlive-memlock.conf
    install -d /etc/systemd/user/everlive.service.d \
        /etc/systemd/user/opengc570d-userd.service.d
    printf '[Service]\nLimitMEMLOCK=1G\n' > /etc/systemd/user/everlive.service.d/10-memlock.conf
    printf '[Service]\nLimitMEMLOCK=1G\n' > /etc/systemd/user/opengc570d-userd.service.d/10-memlock.conf
fi

# Udev-Regeln: GC573/BOLT und GC570D werden über Vendor/Device/Subsystem
# getrennt; der gemeinsame Container-Knoten wird einmal generisch gesetzt.
cat > /etc/udev/rules.d/70-everlive.rules <<EOF
# AVerMedia Live Gamer 4K (GC573) & Live Gamer BOLT (GC555) - Userspace VFIO Mode
ACTION=="add", SUBSYSTEM=="pci", ATTR{vendor}=="0x1461", ATTR{device}=="0x0054", ATTR{subsystem_vendor}=="0x1461", ATTR{subsystem_device}=="0x5730", ATTR{driver_override}="vfio-pci", RUN+="/usr/local/libexec/vfio_pci_bind.sh --udev %k"
ACTION=="add", SUBSYSTEM=="pci", ATTR{vendor}=="0x1461", ATTR{device}=="0x0055", ATTR{driver_override}="vfio-pci", RUN+="/usr/local/libexec/vfio_pci_bind.sh --udev %k"
# AVerMedia Live Gamer ULTRA 2.1 (GC553G2, USB 07ca:2553 + HID LampArray 07ca:d553)
SUBSYSTEM=="usb", ATTR{idVendor}=="07ca", ATTR{idProduct}=="2553", MODE="0660", TAG+="uaccess", GROUP="video"
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", ATTRS{idVendor}=="07ca", ATTRS{idProduct}=="d553", MODE="0660", TAG+="uaccess", GROUP="video"
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", ATTRS{idVendor}=="07ca", ATTRS{idProduct}=="2553", MODE="0660", TAG+="uaccess", GROUP="video"
EOF

# /dev/vfio/vfio is a misc device; keep it out of the per-card scripts.
cat > /etc/udev/rules.d/70-everlive-vfio.rules <<EOF
# The shared VFIO container node; group nodes are set per card after binding.
SUBSYSTEM=="misc", KERNEL=="vfio", GROUP="video", MODE="0660"
EOF

udevadm control --reload-rules 2>/dev/null || true
udevadm trigger --action=add --subsystem-match=pci --attr-match=vendor=0x1461 2>/dev/null || \
    udevadm trigger 2>/dev/null || true
udevadm settle 2>/dev/null || true

# Bind direkt ausführen, falls die Karte schon im Slot ist.  Beide Helfer
# prüfen Vendor/Device/Subsystem und binden nur ihre eigene Karte.
"${USERD_DIR}/vfio_pci_bind.sh" --bind 2>/dev/null || true
"${GC570D_DIR}/gc570d-vfio-bind.sh" --bind 2>/dev/null || true

# Benutzer zur video & audio Gruppe hinzufügen falls nötig
usermod -aG video,audio "${TARGET_USER}" 2>/dev/null || true

# 6. Desktop-Verknüpfung & Application-Icon (Startmenü)
echo -e "\n${BLUE}[5/5] Installiere EverLive Desktop-Starter & Anwendungs-Icon ins System...${NC}"
APPS_DIR="${TARGET_HOME}/.local/share/applications"
mkdir -p "${APPS_DIR}"

# The system-wide launcher is refreshed by `make install-app`; point the user
# entry at it so the menu never falls back to an older checkout/app copy.
case "${APP_BINDIR}/everlive" in
    *" "*) DESKTOP_EXEC="\"${APP_BINDIR}/everlive\"" ;;
    *)     DESKTOP_EXEC="${APP_BINDIR}/everlive" ;;
esac

ICONS_BASE="${TARGET_HOME}/.local/share/icons/hicolor"
for sz in 16 32 48 64 128 256 512; do
    if [ -f "${VIEWER_DIR}/icons/${sz}x${sz}/apps/everlive.png" ]; then
        mkdir -p "${ICONS_BASE}/${sz}x${sz}/apps"
        cp -f "${VIEWER_DIR}/icons/${sz}x${sz}/apps/everlive.png" "${ICONS_BASE}/${sz}x${sz}/apps/everlive.png"
        mkdir -p "/usr/share/icons/hicolor/${sz}x${sz}/apps" 2>/dev/null || true
        cp -f "${VIEWER_DIR}/icons/${sz}x${sz}/apps/everlive.png" "/usr/share/icons/hicolor/${sz}x${sz}/apps/everlive.png" 2>/dev/null || true
    fi
done
if [ -f "${VIEWER_DIR}/icons/scalable/apps/everlive.png" ]; then
    mkdir -p "${ICONS_BASE}/scalable/apps"
    cp -f "${VIEWER_DIR}/icons/scalable/apps/everlive.png" "${ICONS_BASE}/scalable/apps/everlive.png"
    mkdir -p "/usr/share/icons/hicolor/scalable/apps" 2>/dev/null || true
    cp -f "${VIEWER_DIR}/icons/scalable/apps/everlive.png" "/usr/share/icons/hicolor/scalable/apps" 2>/dev/null || true
    cp -f "${VIEWER_DIR}/everlive.bmp" "/usr/share/pixmaps/everlive.bmp" 2>/dev/null || true
fi

cat > "${APPS_DIR}/everlive.desktop" <<EOF
[Desktop Entry]
Name=EverLive
GenericName=Console Gaming Screen
Comment=Console gaming suite with userspace VFIO capture and optional 2x frame generation
Exec=${DESKTOP_EXEC}
Icon=everlive
Terminal=false
Type=Application
Categories=Game;AudioVideo;Video;
Keywords=EverLive;FrameGen;LSFG;Vulkan;PS5;Capture;AVerMedia;GC573;GC570D;HDMI;Gaming;Console;VFIO;
StartupWMClass=EverLive
EOF
chmod +x "${APPS_DIR}/everlive.desktop"
chown -R "${TARGET_USER}:${TARGET_USER}" "${APPS_DIR}/everlive.desktop" 2>/dev/null || true

# Entferne Starter aus der früheren "Experimental"-Phase dieses Projekts, damit
# im Startmenü keine verwaisten Doppel-Einträge zurückbleiben.
rm -f "${APPS_DIR}/everlive-experimental.desktop" 2>/dev/null || true
rm -f "${APPS_DIR}/everlive-intern.desktop" 2>/dev/null || true
rmdir "${TARGET_HOME}/.config/everlive-experimental" 2>/dev/null || true

# Refresh menu/icon caches so desktops pick up the replaced entry immediately.
# User caches must be rewritten as the target user, not root.
run_as_target() {
    if [ "${TARGET_USER}" = "root" ]; then
        "$@"
    else
        sudo -u "${TARGET_USER}" env "HOME=${TARGET_HOME}" "$@"
    fi
}

refresh_desktop_caches() {
    local dir cache

    # Older installers could leave root-owned caches in the user tree, which
    # makes the user-level update below fail silently.
    for cache in "${APPS_DIR}/mimeinfo.cache" "${APPS_DIR}/intent.cache" \
                 "${ICONS_BASE}/icon-theme.cache"; do
        if [ -e "$cache" ]; then
            chown "${TARGET_USER}:${TARGET_USER}" "$cache" 2>/dev/null || true
        fi
    done

    if command -v update-desktop-database >/dev/null 2>&1; then
        run_as_target update-desktop-database "${APPS_DIR}" >/dev/null 2>&1 || true
        for dir in /usr/local/share/applications /usr/share/applications; do
            if [ -d "$dir" ]; then
                update-desktop-database "$dir" >/dev/null 2>&1 || true
            fi
        done
    fi

    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        run_as_target gtk-update-icon-cache -f -t "${ICONS_BASE}" >/dev/null 2>&1 || true
        for dir in /usr/local/share/icons/hicolor /usr/share/icons/hicolor; do
            if [ -d "$dir" ]; then
                gtk-update-icon-cache -f -t "$dir" >/dev/null 2>&1 || true
            fi
        done
    fi

    # KDE's application launcher has its own ksycoca cache; without this the
    # new entry is searchable but can be missing from its categories.
    if command -v kbuildsycoca6 >/dev/null 2>&1; then
        run_as_target kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
    elif command -v kbuildsycoca5 >/dev/null 2>&1; then
        run_as_target kbuildsycoca5 --noincremental >/dev/null 2>&1 || true
    fi
}
refresh_desktop_caches

# Optional: reversible, per-process LD_PRELOAD camera launchers. These are
# separate "(EverLive Camera)" entries, so the original application launchers
# and any user overrides are never modified.
install_preload_launchers() {
    echo -e "\n${BLUE}[*] Konfiguriere LD_PRELOAD-Kamera-Starter (Discord, OBS, Browser)...${NC}"

    local py_bin="python3"
    if [ -x "${VIEWER_DIR}/.venv/bin/python" ]; then
        py_bin="${VIEWER_DIR}/.venv/bin/python"
    fi

    if ! sudo -u "${TARGET_USER}" env HOME="${TARGET_HOME}" \
            EVERLIVE_VIEWER_DIR="${VIEWER_DIR}" \
            EVERLIVE_PRELOAD_LIB="${EVERLIVE_PRELOAD_LIB:-}" \
            "${py_bin}" - <<'PY'
import os
import sys

sys.path.insert(0, os.environ["EVERLIVE_VIEWER_DIR"])
import app_integrations
import card_profile

override = os.environ.get("EVERLIVE_PRELOAD_LIB", "").strip()
profile = card_profile.detect_card_profile()
if not override and (getattr(profile, "is_uvc", False) or not profile.preload_library):
    print(f"[*] {profile.display_name} is a native USB/UVC device — "
          "LD_PRELOAD launchers are not needed (select the camera directly in OBS/Discord).")
    sys.exit(0)
system_lib = f"/usr/local/lib/{profile.preload_library}"
library = override
if not library and os.path.isfile(system_lib):
    library = system_lib
if not library:
    library = app_integrations.find_preload_library(profile) or ""
if not library or not os.path.isfile(library):
    print("[!] EverLive preload library not found. Build/install it first.")
    sys.exit(3)

apps = app_integrations.find_camera_apps()
if not apps:
    print("[*] No supported native apps found "
          "(Discord, OBS, Chrome, Chromium, Brave, Firefox, Zoom).")
    sys.exit(0)

created = 0
for app, ok, message in app_integrations.install_launchers(library):
    print(("[+] " if ok else "[-] ") + f"{app.name}: {message}")
    if ok and message.startswith("Created"):
        created += 1
print(f"[*] {library} -> {created} new launcher(s).")
PY
    then
        echo -e "${YELLOW}[!] Kamera-Starter konnten nicht eingerichtet werden; EverLive bleibt funktionsfähig.${NC}"
        echo -e "    Manuell: ${GREEN}LD_PRELOAD=/usr/local/lib/lib<karte>-v4l2.so <app>${NC}"
        return 0
    fi
}

print_manual_preload_hint() {
    echo -e "\n${GREEN}[*] Module-free camera launch examples:${NC}"
    echo -e "    GC573 : ${BLUE}LD_PRELOAD=/usr/local/lib/libgc573-v4l2.so obs${NC}"
    echo -e "    GC570D: ${BLUE}LD_PRELOAD=/usr/local/lib/libgc570d-v4l2.so obs${NC}"
}

if [ "$WANT_PRELOAD" -eq 1 ]; then
    install_preload_launchers
else
    print_manual_preload_hint
fi

INSTALLED_VERSION="$(sed -n 's/^__version__ = "\([^"]*\)".*/\1/p' "${APP_VIEWER_DIR}/everlive_viewer.py" 2>/dev/null | head -n 1 || true)"
INSTALLED_VERSION="${INSTALLED_VERSION:-unknown}"

echo -e "\n${GREEN}===================================================================${NC}"
echo -e "${GREEN}   EverLive v2.0.1 erfolgreich installiert!                        ${NC}"
echo -e "${GREEN}===================================================================${NC}"
echo -e "Du kannst EverLive ab sofort direkt über dein Startmenü"
echo -e "starten (Eintrag \"EverLive\"), oder im Terminal mit:"
echo -e "  ${BLUE}everlive${NC}       ${GREEN}# installierte Hub-Version${NC}"
echo -e "  ${BLUE}./play.sh${NC}      ${GREEN}# direkt aus diesem Checkout${NC}"
echo -e ""
echo -e "Hub      : ${APP_BINDIR}/everlive (v${INSTALLED_VERSION})"
echo -e "Desktop  : ${APPS_DIR}/everlive.desktop"
echo -e "Config   : ~/.config/everlive/"
echo -e "Socket   : \$XDG_RUNTIME_DIR/everlive.sock"
echo -e ""
echo -e "Frame Generation im Spielfenster: 'g' = Off -> 60->120 -> 30->60."
echo -e "Voraussetzung: lsfg-vk 2.0 + Lossless Scaling in Steam (Beta-Branch 'lsfg-vk')."
if command -v pgrep >/dev/null 2>&1 && pgrep -f "tools/everlive/everlive_viewer.py" >/dev/null 2>&1; then
    echo -e ""
    echo -e "${YELLOW}[!] Eine EverLive-Instanz läuft noch. Bitte beenden und neu starten, damit die neue UI geladen wird.${NC}"
fi
echo -e "Viel Spaß beim Zocken mit 60->120 / 30->60 FPS!"
