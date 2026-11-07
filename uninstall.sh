#!/usr/bin/env bash
# ==============================================================================
# EverLive - Deinstaller (v1.4.0)
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$EUID" -ne 0 ]; then
    echo "Bitte mit sudo ausführen: sudo ./uninstall.sh"
    exit 1
fi

TARGET_USER="${SUDO_USER:-$USER}"
# Resolve the home directory without eval-ing user-controlled text.
TARGET_HOME="$(getent passwd "${TARGET_USER}" | cut -d: -f6)"
[ -n "${TARGET_HOME}" ] || TARGET_HOME="/home/${TARGET_USER}"

echo "[*] Deinstalliere EverLive..."

# 1. Laufende Daemons beenden
killall -9 everlive_userd gc573_user_streamer opengc570d-userd gc570d-ctl 2>/dev/null || true
rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/gc570d/control.sock" 2>/dev/null || true
rm -f /dev/shm/gc570d_v4l2_shm 2>/dev/null || true

# 2. VFIO & Memlock Regeln entfernen
rm -f /etc/udev/rules.d/99-gc573-vfio.rules
rm -f /etc/security/limits.d/99-everlive-memlock.conf
# GC570D (DUO) packaging installed through tools/opengc570d-userd/Makefile:
# binaries, udev rule, memlock limits, systemd user unit and sleep hook.
if [ -d "${SCRIPT_DIR}/tools/opengc570d-userd" ]; then
    make -C "${SCRIPT_DIR}/tools/opengc570d-userd" uninstall >/dev/null 2>&1 || true
fi
rm -f /etc/security/limits.d/99-gc570d-memlock.conf
rm -f /usr/lib/systemd/system-sleep/gc570d-sleep.sh
rm -f /etc/gc570d.conf.example
rm -f /usr/local/bin/opengc570d-userd /usr/local/bin/gc570d-ctl
rm -f /usr/local/lib/libgc570d-v4l2.so
rm -f /usr/local/libexec/gc570d-vfio-bind.sh /usr/local/libexec/gc570d-sleep.sh
rm -f /usr/local/lib/systemd/user/opengc570d-userd.service
rm -f /etc/udev/rules.d/99-gc570d-vfio.rules

# 3. Legacy Kernel-Treiber & DKMS Reste entfernen (falls vorhanden)
rmmod open_gc573 2>/dev/null || true
rm -rf "/lib/modules/$(uname -r)/updates/open_gc573" 2>/dev/null || true
depmod -a "$(uname -r)" 2>/dev/null || true
if command -v dkms >/dev/null 2>&1; then
    dkms remove -m everlive --all 2>/dev/null || true
    rm -rf /usr/src/everlive-* 2>/dev/null || true
fi
rm -f /etc/modules-load.d/everlive.conf 2>/dev/null || true
rm -f /etc/modules-load.d/everlive-vfio.conf 2>/dev/null || true
rm -f /etc/modprobe.d/everlive-vfio.conf 2>/dev/null || true
rm -f /etc/modules-load.d/everlive-v4l2loopback.conf 2>/dev/null || true
rm -f /etc/modprobe.d/everlive-v4l2loopback.conf 2>/dev/null || true
rm -f /etc/modprobe.d/blacklist-everlive-legacy.conf 2>/dev/null || true
rm -f /etc/pipewire/pipewire.conf.d/50-everlive-capture.conf 2>/dev/null || true
rm -f /etc/udev/rules.d/70-everlive.rules 2>/dev/null || true
rm -f /usr/local/bin/everlive-audio 2>/dev/null || true

# Virtual Camera entladen (das Paket v4l2loopback bleibt installiert).
modprobe -r v4l2loopback 2>/dev/null || true

udevadm control --reload-rules 2>/dev/null || true
udevadm trigger 2>/dev/null || true

# 4. Desktop-Einträge & Icons entfernen
#
# Remove both the current launcher and the leftover entry from the earlier
# "experimental" naming phase of this project.
rm -f "${TARGET_HOME}/.local/share/applications/everlive.desktop"
rm -f "${TARGET_HOME}/.local/share/applications/everlive-experimental.desktop"
rm -f "${TARGET_HOME}/.local/share/applications/everlive-intern.desktop"
rm -f "${TARGET_HOME}/.config/autostart/everlive.desktop"
rmdir "${TARGET_HOME}/.config/everlive-experimental" 2>/dev/null || true
rm -f "${TARGET_HOME}/.local/share/icons/hicolor/"*/apps/everlive.png
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "${TARGET_HOME}/.local/share/applications" 2>/dev/null || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "${TARGET_HOME}/.local/share/icons/hicolor" 2>/dev/null || true
fi

echo "[+] EverLive wurde sauber deinstalliert."
