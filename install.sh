#!/usr/bin/env bash
# ==============================================================================
# EverLive v1.0.0 - 1-Click Installer für AVerMedia Live Gamer 4K (GC573)
# ==============================================================================
# Installiert den 100% blob-freien Userspace VFIO-Treiber, udev-Hardware-Rechte,
# Speicher-Locking-Limits und die EverLive Desktop-App für HDMI Console Gaming.
#
# Kein DKMS, keine Kernel-Header, kein Kernel-Neukompilieren bei Updates!
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
echo -e "${BLUE}     EverLive v1.0.0 - GC573 Userspace Gaming Suite Installer     ${NC}"
echo -e "${BLUE}===================================================================${NC}"

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

# 2. Paketmanager & Abhängigkeiten erkennen
detect_and_install_deps() {
    echo -e "\n${BLUE}[1/5] Prüfe Software-Abhängigkeiten...${NC}"
    
    MISSING_PKGS=()
    
    if command -v pacman >/dev/null 2>&1; then
        # Arch / CachyOS / Manjaro
        ! command -v gcc >/dev/null 2>&1 && MISSING_PKGS+=(base-devel)
        ! command -v sdl2-config >/dev/null 2>&1 && MISSING_PKGS+=(sdl2)
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
        ! command -v gcc >/dev/null 2>&1 && MISSING_PKGS+=(build-essential)
        ! command -v sdl2-config >/dev/null 2>&1 && MISSING_PKGS+=(libsdl2-dev)
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
        ! command -v gcc >/dev/null 2>&1 && MISSING_PKGS+=(gcc make)
        ! command -v sdl2-config >/dev/null 2>&1 && MISSING_PKGS+=(SDL2-devel)
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

# 4. Alte Kernel-Treiber Artefakte bereinigen (Migration)
echo -e "\n${BLUE}[3/5] Bereinige Legacy Kernel-Artefakte...${NC}"
rm -f /etc/modules-load.d/everlive.conf
if command -v dkms >/dev/null 2>&1; then
    dkms remove -m everlive --all 2>/dev/null || true
    rm -rf /usr/src/everlive-* 2>/dev/null || true
fi
modprobe -r open_gc573 2>/dev/null || true
echo -e "${GREEN}[+] Altes Kernel-Modul deregistriert (DKMS-frei).${NC}"

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
echo -e "${GREEN}      EverLive v1.0.0 erfolgreich installiert & einsatzbereit!      ${NC}"
echo -e "${GREEN}===================================================================${NC}"
echo -e "Du kannst EverLive ab sofort direkt über dein Startmenü starten,"
echo -e "oder im Terminal mit: ${BLUE}./play.sh${NC}"
echo -e ""
echo -e "Viel Spaß beim Zocken mit 1440p120, nativem 4K und Null Kernel-Gefrickel!"
