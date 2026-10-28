#!/usr/bin/env bash
# ==============================================================================
# EverLive v1.2.1 - 1-Click Installer für AVerMedia Live Gamer 4K (GC573)
# ==============================================================================
# Installiert den 100% blob-freien Userspace VFIO-Treiber, udev-Hardware-Rechte,
# Speicher-Locking-Limits und die EverLive Desktop-App für HDMI Console Gaming.
#
# Kein DKMS, keine Kernel-Header, kein Kernel-Neukompilieren für EverLive selbst!
# Optional (nur Virtual Camera): v4l2loopback aus dem Distributions-Repo.
# 100% Secure Boot & Anti-Cheat (EAC v2) kompatibel.
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
USERD_DIR="${SCRIPT_DIR}/tools/opengc573-userd"
VIEWER_DIR="${SCRIPT_DIR}/tools/everlive"

# ANSI Farben
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${BLUE}===================================================================${NC}"
echo -e "${BLUE}     EverLive v1.2.1 - GC573 Userspace Gaming Suite Installer     ${NC}"
echo -e "${BLUE}===================================================================${NC}"

# Optionale Virtual Camera: standardmäßig ja, per --no-loopback abschaltbar.
WANT_LOOPBACK=1

usage() {
    cat <<EOF
Usage: sudo ./install.sh [OPTIONS]

  --no-loopback, --skip-loopback
      Do not install or configure v4l2loopback. The optional Virtual Camera
      (/dev/video10 for OBS) stays disabled and the system remains free of
      DKMS modules and kernel headers.

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

# Vor der Root-Prüfung parsen, damit --help kein sudo benötigt. Der
# sudo-Neustart übergibt "$@", wodurch das Kind erneut parst.
parse_args "$@"

# 1. Root-Prüfung / Sudo
if [ "$EUID" -ne 0 ]; then
    echo -e "${YELLOW}[*] Für die System- und Hardware-Rechte-Installation werden Root-Rechte benötigt.${NC}"
    echo -e "${YELLOW}[*] Starte mit sudo neu...${NC}"
    exec sudo bash "$0" "$@"
fi

# Echten Benutzer ermitteln (nicht root)
TARGET_USER="${SUDO_USER:-$USER}"
TARGET_HOME="$(getent passwd "${TARGET_USER}" 2>/dev/null | cut -d: -f6)"
[ -z "${TARGET_HOME}" ] && TARGET_HOME="${HOME}"

echo -e "[*] Ziel-Benutzer: ${GREEN}${TARGET_USER}${NC}"
echo -e "[*] Linux Kernel : ${GREEN}$(uname -r)${NC}"
echo -e "[*] Architektur  : ${GREEN}100% Userspace VFIO (Blob-Free Ring 3)${NC}"

ask_virtual_camera
if [ "$WANT_LOOPBACK" -eq 0 ]; then
    echo -e "${GREEN}[*] Virtual Camera installation skipped. EverLive remains 100% module-free.${NC}"
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

detect_and_install_deps() {
    echo -e "\n${BLUE}[1/5] Prüfe Software-Abhängigkeiten...${NC}"
    
    MISSING_PKGS=()
    
    if command -v pacman >/dev/null 2>&1; then
        # Arch / CachyOS / Manjaro
        ! command -v gcc >/dev/null 2>&1 && MISSING_PKGS+=(base-devel)
        ! command -v sdl2-config >/dev/null 2>&1 && MISSING_PKGS+=(sdl2)
        ! pkg-config --exists libavcodec libswresample libswscale >/dev/null 2>&1 && MISSING_PKGS+=(ffmpeg)
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
        ! pkg-config --exists libavcodec libswresample libswscale >/dev/null 2>&1 && MISSING_PKGS+=(libavcodec-dev libswresample-dev libswscale-dev libavutil-dev)
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
        ! pkg-config --exists libavcodec libswresample libswscale >/dev/null 2>&1 && MISSING_PKGS+=(ffmpeg-devel)
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
}

detect_and_install_deps

# 3. Userspace VFIO Engine kompilieren
echo -e "\n${BLUE}[2/5] Kompiliere EverLive Userspace VFIO Engine...${NC}"
make -C "${USERD_DIR}" clean >/dev/null 2>&1 || true
make -C "${USERD_DIR}" -j"$(nproc)"
chmod +x "${USERD_DIR}/gc573_user_streamer" "${USERD_DIR}/everlive_userd" "${USERD_DIR}/gc573_user_probe" "${SCRIPT_DIR}/play.sh"
setcap cap_ipc_lock=+ep "${USERD_DIR}/gc573_user_streamer" 2>/dev/null || true
echo -e "${GREEN}[+] Userspace VFIO Engine erfolgreich gebaut & berechtigt!${NC}"

# Python GUI-Umgebung (PySide6) für Benutzer prüfen und bei Bedarf einrichten
echo -e "\n[*] Prüfe Python-Umgebung für EverLive Studio..."
if ! sudo -u "${TARGET_USER}" python3 -c 'import PySide6' >/dev/null 2>&1; then
    echo -e "${YELLOW}[*] PySide6 nicht systemweit gefunden. Richte venv für EverLive Studio ein...${NC}"
    if [ ! -d "${VIEWER_DIR}/.venv" ]; then
        sudo -u "${TARGET_USER}" python3 -m venv "${VIEWER_DIR}/.venv" 2>/dev/null || python3 -m venv "${VIEWER_DIR}/.venv"
        chown -R "${TARGET_USER}:${TARGET_USER}" "${VIEWER_DIR}/.venv" 2>/dev/null || true
    fi
    if [ -x "${VIEWER_DIR}/.venv/bin/pip" ]; then
        sudo -u "${TARGET_USER}" "${VIEWER_DIR}/.venv/bin/pip" install --upgrade pip PySide6 2>/dev/null || "${VIEWER_DIR}/.venv/bin/pip" install --upgrade pip PySide6
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
# Managed by EverLive: virtual camera node for OBS / Discord / FFmpeg.
# exclusive_caps=0 keeps VIDEO_CAPTURE advertised while no writer is active,
# so OBS never drops the device from its list.
options v4l2loopback video_nr=10 card_label="EverLive Capture" exclusive_caps=0
EOF
    if ! lsmod | grep -q '^v4l2loopback'; then
        modprobe v4l2loopback 2>/dev/null || true
    fi
    if [ -e /dev/video10 ]; then
        if [ "$(cat /sys/module/v4l2loopback/parameters/exclusive_caps 2>/dev/null || echo 0)" = "1" ]; then
            echo -e "${YELLOW}[!] /dev/video10 existiert, aber die laufende Instanz nutzt noch exclusive_caps=1.${NC}"
            echo -e "${YELLOW}    Nach einem Neustart (oder 'sudo modprobe -r v4l2loopback && sudo modprobe v4l2loopback') gilt exclusive_caps=0.${NC}"
        else
            echo -e "${GREEN}[+] /dev/video10 'EverLive Capture' bereit (exclusive_caps=0).${NC}"
        fi
    else
        echo -e "${YELLOW}[!] v4l2loopback konfiguriert, aber /dev/video10 fehlt noch (Neustart nötig).${NC}"
    fi
else
    echo -e "${YELLOW}[!] v4l2loopback nicht verfügbar; Virtual Camera bleibt bis zur Installation aus.${NC}"
fi

# 5. Udev-Rechte für vfio-pci & unbegrenzten Memlock einrichten
echo -e "\n${BLUE}[4/5] Richte VFIO Hardware-Zugriffsrechte (udev) & Memlock ein...${NC}"

# Memlock Limits für native 4K DMA Mappings
mkdir -p /etc/security/limits.d
cat > /etc/security/limits.d/99-everlive-memlock.conf <<EOF
# EverLive GC573 VFIO DMA memory pinning
*       soft    memlock   unlimited
*       hard    memlock   unlimited
EOF

# Udev-Regel: Bindet AVerMedia GC573 (1461:0054 / 1461:5550) an vfio-pci
cat > /etc/udev/rules.d/70-everlive.rules <<EOF
# AVerMedia Live Gamer 4K (GC573) & Live Gamer BOLT (GC555) - Userspace VFIO Mode
ACTION=="add", SUBSYSTEM=="pci", ATTR{vendor}=="0x1461", ATTR{device}=="0x0054", ATTR{driver_override}="vfio-pci"
ACTION=="add", SUBSYSTEM=="pci", ATTR{vendor}=="0x1461", ATTR{device}=="0x0055", ATTR{driver_override}="vfio-pci"
SUBSYSTEM=="vfio", MODE="0660", TAG+="uaccess", GROUP="video"
EOF

udevadm control --reload-rules 2>/dev/null || true
udevadm trigger 2>/dev/null || true

# Bind direkt ausführen, falls Karte schon im Slot ist
"${USERD_DIR}/vfio_pci_bind.sh" --bind 2>/dev/null || true

# Benutzer zur video & audio Gruppe hinzufügen falls nötig
usermod -aG video,audio "${TARGET_USER}" 2>/dev/null || true

# 6. Desktop-Verknüpfung & Application-Icon (Startmenü)
echo -e "\n${BLUE}[5/5] Installiere EverLive Desktop-Starter & Anwendungs-Icon ins System...${NC}"
APPS_DIR="${TARGET_HOME}/.local/share/applications"
mkdir -p "${APPS_DIR}"

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
Comment=Ultra-Low-Latency HDMI Gaming Screen für PS5 & Konsolen (AVerMedia GC573 - 100% Userspace VFIO)
Exec=${SCRIPT_DIR}/play.sh
Icon=everlive
Terminal=false
Type=Application
Categories=Game;AudioVideo;Video;
Keywords=EverLive;PS5;Capture;AVerMedia;GC573;HDMI;Gaming;Console;VFIO;
StartupWMClass=EverLive
EOF
chmod +x "${APPS_DIR}/everlive.desktop"
chown -R "${TARGET_USER}:${TARGET_USER}" "${APPS_DIR}/everlive.desktop" 2>/dev/null || true

# Entferne ggf. veraltete Test-Starter
rm -f "${APPS_DIR}/everlive-intern.desktop" 2>/dev/null || true

echo -e "\n${GREEN}===================================================================${NC}"
echo -e "${GREEN}      EverLive v1.2.1 erfolgreich installiert & einsatzbereit!      ${NC}"
echo -e "${GREEN}===================================================================${NC}"
echo -e "Du kannst EverLive ab sofort direkt über dein Startmenü starten,"
echo -e "oder im Terminal mit: ${BLUE}./play.sh${NC}"
echo -e ""
echo -e "Viel Spaß beim Zocken mit 1440p120, nativem 4K und Null Kernel-Gefrickel!"
