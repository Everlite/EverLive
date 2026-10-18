#!/usr/bin/env bash
# ==============================================================================
# EverLive - 1-Click Installer für AVerMedia Live Gamer 4K (GC573)
# ==============================================================================
# Installiert den nativen open_gc573 Kernel-Treiber, udev-Zugriffsrechte,
# automatischen Boot-Start und die EverLive Desktop-App für HDMI Console Gaming.
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DRIVER_DIR="${SCRIPT_DIR}/open_gc573"
VIEWER_DIR="${SCRIPT_DIR}/tools/everlive"

# ANSI Farben
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${BLUE}===================================================================${NC}"
echo -e "${BLUE}             EverLive - GC573 Linux Gaming Suite Installer          ${NC}"
echo -e "${BLUE}===================================================================${NC}"

# 1. Root-Prüfung / Sudo
if [ "$EUID" -ne 0 ]; then
    echo -e "${YELLOW}[*] Für die System- und Treiber-Installation werden Root-Rechte benötigt.${NC}"
    echo -e "${YELLOW}[*] Starte mit sudo neu...${NC}"
    exec sudo bash "$0" "$@"
fi

# Echten Benutzer ermitteln (nicht root)
TARGET_USER="${SUDO_USER:-$USER}"
TARGET_HOME="$(getent passwd "${TARGET_USER}" 2>/dev/null | cut -d: -f6)"
[ -z "${TARGET_HOME}" ] && TARGET_HOME="${HOME}"

echo -e "[*] Ziel-Benutzer: ${GREEN}${TARGET_USER}${NC}"
echo -e "[*] Linux Kernel : ${GREEN}$(uname -r)${NC}"

# 2. Paketmanager & Abhängigkeiten erkennen
detect_and_install_deps() {
    echo -e "\n${BLUE}[1/5] Prüfe Build-Abhängigkeiten...${NC}"
    
    MISSING_PKGS=()
    
    if command -v pacman >/dev/null 2>&1; then
        # Arch / CachyOS / Manjaro
        if [ ! -d "/usr/lib/modules/$(uname -r)/build" ]; then
            KERNEL_PKG=$(pacman -Qoq "/usr/lib/modules/$(uname -r)" 2>/dev/null | grep -E '^linux' | head -n 1)
            if [ -n "${KERNEL_PKG}" ] && pacman -Si "${KERNEL_PKG}-headers" >/dev/null 2>&1; then
                MISSING_PKGS+=("${KERNEL_PKG}-headers")
            elif [[ "$(uname -r)" =~ cachyos ]]; then
                MISSING_PKGS+=(linux-cachyos-headers)
            elif [[ "$(uname -r)" =~ zen ]]; then
                MISSING_PKGS+=(linux-zen-headers)
            elif [[ "$(uname -r)" =~ lts ]]; then
                MISSING_PKGS+=(linux-lts-headers)
            else
                MISSING_PKGS+=(linux-headers)
            fi
        fi
        ! command -v gcc >/dev/null 2>&1 && MISSING_PKGS+=(base-devel)
        ! command -v sdl2-config >/dev/null 2>&1 && MISSING_PKGS+=(sdl2)
        ! command -v arecord >/dev/null 2>&1 && MISSING_PKGS+=(alsa-utils)
        ! command -v v4l2-ctl >/dev/null 2>&1 && MISSING_PKGS+=(v4l-utils)
        ! command -v pw-play >/dev/null 2>&1 && MISSING_PKGS+=(pipewire)
        ! command -v wpctl >/dev/null 2>&1 && MISSING_PKGS+=(wireplumber)
        ! command -v mpv >/dev/null 2>&1 && MISSING_PKGS+=(mpv)
        ! pacman -Q xcb-util-cursor >/dev/null 2>&1 && MISSING_PKGS+=(xcb-util-cursor)
        ! sudo -u "${TARGET_USER}" python3 -c 'import PySide6' >/dev/null 2>&1 && MISSING_PKGS+=(pyside6)
        
        # Clang/LLVM Prüfen falls Kernel mit Clang gebaut wurde (z.B. CachyOS)
        if grep -q '^CONFIG_CC_IS_CLANG=y' "/usr/lib/modules/$(uname -r)/build/.config" 2>/dev/null || grep -iq 'clang' /proc/version 2>/dev/null; then
            ! command -v clang >/dev/null 2>&1 && MISSING_PKGS+=(clang)
            ! command -v lld >/dev/null 2>&1 && MISSING_PKGS+=(lld)
            ! command -v llvm-ar >/dev/null 2>&1 && MISSING_PKGS+=(llvm)
        fi
        
        if [ ${#MISSING_PKGS[@]} -gt 0 ]; then
            echo -e "${YELLOW}[!] Fehlende Pakete gefunden: ${MISSING_PKGS[*]}${NC}"
            read -p "Sollen diese Pakete jetzt automatisch via pacman installiert werden? (j/N) " -n 1 -r
            echo
            if [[ $REPLY =~ ^[JjYy]$ ]]; then
                pacman -S --needed "${MISSING_PKGS[@]}"
            else
                echo -e "${RED}[!] Bitte installiere die benötigten Pakete manuell und starte den Installer erneut.${NC}"
                exit 1
            fi
        fi
    elif command -v apt-get >/dev/null 2>&1; then
        # Ubuntu / Debian / Pop!_OS / Mint
        [ ! -d "/lib/modules/$(uname -r)/build" ] && MISSING_PKGS+=("linux-headers-$(uname -r)")
        ! command -v gcc >/dev/null 2>&1 && MISSING_PKGS+=(build-essential)
        ! command -v sdl2-config >/dev/null 2>&1 && MISSING_PKGS+=(libsdl2-dev)
        ! command -v arecord >/dev/null 2>&1 && MISSING_PKGS+=(alsa-utils)
        ! command -v v4l2-ctl >/dev/null 2>&1 && MISSING_PKGS+=(v4l-utils)
        ! command -v pw-play >/dev/null 2>&1 && MISSING_PKGS+=(pipewire-bin)
        ! command -v wpctl >/dev/null 2>&1 && MISSING_PKGS+=(wireplumber)
        ! command -v mpv >/dev/null 2>&1 && MISSING_PKGS+=(mpv)
        ! dpkg -s libxcb-cursor0 >/dev/null 2>&1 && MISSING_PKGS+=(libxcb-cursor0)
        ! dpkg -s python3-pip >/dev/null 2>&1 && MISSING_PKGS+=(python3-pip)
        ! dpkg -s python3-venv >/dev/null 2>&1 && MISSING_PKGS+=(python3-venv)
        
        if grep -q '^CONFIG_CC_IS_CLANG=y' "/lib/modules/$(uname -r)/build/.config" 2>/dev/null || grep -iq 'clang' /proc/version 2>/dev/null; then
            ! command -v clang >/dev/null 2>&1 && MISSING_PKGS+=(clang lld llvm)
        fi
        
        if [ ${#MISSING_PKGS[@]} -gt 0 ]; then
            echo -e "${YELLOW}[!] Fehlende Pakete gefunden: ${MISSING_PKGS[*]}${NC}"
            apt-get update && apt-get install -y "${MISSING_PKGS[@]}"
        fi
    elif command -v dnf >/dev/null 2>&1; then
        # Fedora / Nobara
        [ ! -d "/usr/src/kernels/$(uname -r)" ] && MISSING_PKGS+=("kernel-devel-$(uname -r)" kernel-headers)
        ! command -v gcc >/dev/null 2>&1 && MISSING_PKGS+=(gcc make)
        ! command -v sdl2-config >/dev/null 2>&1 && MISSING_PKGS+=(SDL2-devel)
        ! command -v arecord >/dev/null 2>&1 && MISSING_PKGS+=(alsa-utils)
        ! command -v v4l2-ctl >/dev/null 2>&1 && MISSING_PKGS+=(v4l-utils)
        ! command -v pw-play >/dev/null 2>&1 && MISSING_PKGS+=(pipewire-utils)
        ! command -v wpctl >/dev/null 2>&1 && MISSING_PKGS+=(wireplumber)
        ! command -v mpv >/dev/null 2>&1 && MISSING_PKGS+=(mpv)
        ! rpm -q xcb-util-cursor >/dev/null 2>&1 && MISSING_PKGS+=(xcb-util-cursor)
        ! sudo -u "${TARGET_USER}" python3 -c 'import PySide6' >/dev/null 2>&1 && MISSING_PKGS+=(python3-pyside6)
        
        if grep -q '^CONFIG_CC_IS_CLANG=y' "/usr/src/kernels/$(uname -r)/.config" 2>/dev/null || grep -iq 'clang' /proc/version 2>/dev/null; then
            ! command -v clang >/dev/null 2>&1 && MISSING_PKGS+=(clang lld llvm)
        fi
        
        if [ ${#MISSING_PKGS[@]} -gt 0 ]; then
            echo -e "${YELLOW}[!] Fehlende Pakete gefunden: ${MISSING_PKGS[*]}${NC}"
            dnf install -y "${MISSING_PKGS[@]}"
        fi
    fi
    echo -e "${GREEN}[+] Alle benötigten Build-Tools und Bibliotheken vorhanden!${NC}"
}

detect_and_install_deps

# 3. Kernel-Treiber und Display-App kompilieren
echo -e "\n${BLUE}[2/5] Kompiliere EverLive Kernel-Treiber und Display-App...${NC}"
make -C "${DRIVER_DIR}" clean >/dev/null 2>&1 || true
make -C "${DRIVER_DIR}" -j"$(nproc)"
echo -e "${GREEN}[+] open_gc573.ko erfolgreich gebaut!${NC}"

# Display-App kompilieren
gcc -O3 -march=native -o "${VIEWER_DIR}/everlive_display" "${VIEWER_DIR}/everlive_display.c" $(sdl2-config --cflags --libs) -lX11 -lpthread
ln -sf everlive_display "${VIEWER_DIR}/gc573_display"
chmod +x "${VIEWER_DIR}/everlive_display" "${VIEWER_DIR}/everlive_viewer.py" "${SCRIPT_DIR}/play.sh"
echo -e "${GREEN}[+] EverLive Direct Display Engine erfolgreich kompiliert!${NC}"

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
    if sudo -u "${TARGET_USER}" "${VIEWER_DIR}/.venv/bin/python3" -c 'import PySide6' >/dev/null 2>&1; then
        echo -e "${GREEN}[+] PySide6 erfolgreich in ${VIEWER_DIR}/.venv eingerichtet!${NC}"
    else
        echo -e "${YELLOW}[!] Hinweis: PySide6 konnte nicht automatisch via pip bezogen werden. EverLive läuft zuverlässig im nativen Direct-Renderer (everlive_display).${NC}"
    fi
else
    echo -e "${GREEN}[+] Systemweites PySide6 vorhanden!${NC}"
fi

# 4. Modul im System registrieren
echo -e "\n${BLUE}[3/5] Installiere Kernel-Modul ins System...${NC}"
MOD_DEST="/lib/modules/$(uname -r)/updates/open_gc573"
mkdir -p "${MOD_DEST}"
install -m 644 "${DRIVER_DIR}/open_gc573.ko" "${MOD_DEST}/"
depmod -a "$(uname -r)"

# DKMS Support falls dkms installiert ist
if command -v dkms >/dev/null 2>&1; then
    echo -e "[*] DKMS gefunden. Richte automatische Kernel-Update-Wiederherstellung ein..."
    DKMS_SRC="/usr/src/everlive-0.4.0"
    mkdir -p "${DKMS_SRC}"
    cp -r "${DRIVER_DIR}"/* "${DKMS_SRC}/"
    cat > "${DKMS_SRC}/dkms.conf" <<EOF
PACKAGE_NAME="everlive"
PACKAGE_VERSION="0.4.0"
BUILT_MODULE_NAME[0]="open_gc573"
DEST_MODULE_LOCATION[0]="/updates/open_gc573"
AUTOINSTALL="yes"
CLEAN="make -C \${dkms_tree}/\${PACKAGE_NAME}/\${PACKAGE_VERSION}/build KDIR=\${kernel_source_dir} clean"
MAKE[0]="make -C \${dkms_tree}/\${PACKAGE_NAME}/\${PACKAGE_VERSION}/build KDIR=\${kernel_source_dir} modules"
EOF
    dkms remove -m everlive -v 0.2.0 --all 2>/dev/null || true
    dkms remove -m everlive -v 0.3.0 --all 2>/dev/null || true
    dkms remove -m everlive -v 0.4.0 --all 2>/dev/null || true
    dkms add -m everlive -v 0.4.0 2>/dev/null || true
    if dkms build -m everlive -v 0.4.0 2>/dev/null && dkms install -m everlive -v 0.4.0 --force 2>/dev/null; then
        echo -e "${GREEN}[+] DKMS registriert: Treiber aktualisiert sich bei Kernel-Updates automatisch!${NC}"
    else
        echo -e "${YELLOW}[!] Hinweis: DKMS-Registrierung übersprungen oder nicht möglich. Der direkt installierte Treiber funktioniert trotzdem einwandfrei.${NC}"
    fi
fi

# Autostart beim Systemstart
mkdir -p /etc/modules-load.d
echo "open_gc573" > /etc/modules-load.d/everlive.conf

# 5. Udev-Rechte (Rechte für Benutzer ohne root/sudo)
echo -e "\n${BLUE}[4/5] Richte Geräte-Zugriffsrechte (udev) ein...${NC}"
cat > /etc/udev/rules.d/70-everlive.rules <<EOF
# AVerMedia Live Gamer 4K (GC573) - EverLive Gaming Access
SUBSYSTEM=="video4linux", KERNEL=="video*", ATTRS{vendor}=="0x1461", ATTRS{device}=="0x0054", MODE="0660", TAG+="uaccess", GROUP="video"
SUBSYSTEM=="sound", KERNEL=="pcmC*D*c", ATTRS{vendor}=="0x1461", ATTRS{device}=="0x0054", MODE="0660", TAG+="uaccess", GROUP="audio"
SUBSYSTEM=="leds", KERNEL=="gc573:rgb:status", ACTION=="add", RUN+="/bin/chgrp video /sys/class/leds/%k/brightness /sys/class/leds/%k/multi_intensity", RUN+="/bin/chmod 0660 /sys/class/leds/%k/brightness /sys/class/leds/%k/multi_intensity"
SUBSYSTEM=="pci", ATTRS{vendor}=="0x1461", ATTRS{device}=="0x0054", TEST=="hpd_pulse", RUN+="/bin/chgrp video /sys%p/hpd_pulse", RUN+="/bin/chmod 0660 /sys%p/hpd_pulse"
EOF
udevadm control --reload-rules 2>/dev/null || true
udevadm trigger 2>/dev/null || true

# Benutzer zur video & audio Gruppe hinzufügen falls nötig
usermod -aG video,audio "${TARGET_USER}" 2>/dev/null || true

# 6. Desktop-Verknüpfung & Application-Icon (Startmenü)
echo -e "\n${BLUE}[5/5] Installiere EverLive Desktop-Starter & Anwendungs-Icon ins System...${NC}"
APPS_DIR="${TARGET_HOME}/.local/share/applications"
mkdir -p "${APPS_DIR}"
rm -f "${APPS_DIR}/gc573-ps5.desktop"

# Installiere hochauflösende Icons in alle Standard-Größen
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
    cp -f "${VIEWER_DIR}/icons/scalable/apps/everlive.png" "/usr/share/icons/hicolor/scalable/apps/everlive.png" 2>/dev/null || true
fi
chown -R "${TARGET_USER}:${TARGET_USER}" "${ICONS_BASE}" 2>/dev/null || true
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "${ICONS_BASE}" 2>/dev/null || true
    gtk-update-icon-cache -f -t "/usr/share/icons/hicolor" 2>/dev/null || true
fi

cat > "${APPS_DIR}/everlive.desktop" <<EOF
[Desktop Entry]
Name=EverLive
GenericName=Console Gaming Screen
Comment=Ultra-Low-Latency HDMI Gaming Screen für PS5 & Konsolen (AVerMedia GC573)
Exec=${SCRIPT_DIR}/play.sh
Icon=everlive
Terminal=false
Type=Application
Categories=Game;AudioVideo;Video;
Keywords=EverLive;PS5;Capture;AVerMedia;GC573;HDMI;Gaming;Console;
StartupWMClass=EverLive
EOF

rm -f "${APPS_DIR}/everlive-mainline.desktop" 2>/dev/null || true
chown -R "${TARGET_USER}:${TARGET_USER}" "${APPS_DIR}/everlive.desktop" 2>/dev/null || true
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "${APPS_DIR}" 2>/dev/null || true
fi

# 7. Treiber direkt laden
echo -e "\n${BLUE}[*] Lade neuen Treiber...${NC}"
if bash "${DRIVER_DIR}/load.sh"; then
    echo -e "\n${GREEN}===================================================================${NC}"
    echo -e "${GREEN}     EverLive wurde erfolgreich auf deinem System installiert!     ${NC}"
    echo -e "${GREEN}===================================================================${NC}"
    echo -e "Du kannst das Spiel jetzt auf zwei Arten starten:"
    echo -e " 1. Im Startmenü / Application Launcher nach '${YELLOW}EverLive${NC}' suchen und anklicken."
    echo -e " 2. Oder im Terminal einfach '${YELLOW}./play.sh${NC}' ausführen (kein sudo nötig!)."
    echo
else
    echo -e "\n${RED}===================================================================${NC}"
    echo -e "${RED}[!] FEHLER: open_gc573 Treiber konnte nicht geladen werden!       ${NC}"
    echo -e "${RED}===================================================================${NC}"
    echo -e "Bitte prüfe 'dmesg | tail -n 20' oder deaktiviere Secure Boot im UEFI/BIOS."
    exit 1
fi
