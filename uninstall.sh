#!/usr/bin/env bash
# ==============================================================================
# EverLive - Deinstaller (v1.2.2)
# ==============================================================================
set -euo pipefail

if [ "$EUID" -ne 0 ]; then
    echo "Bitte mit sudo ausführen: sudo ./uninstall.sh"
    exit 1
fi

TARGET_USER="${SUDO_USER:-$USER}"
TARGET_HOME=$(eval echo "~${TARGET_USER}")

echo "[*] Deinstalliere EverLive..."

# 1. Laufende Daemons beenden
killall -9 everlive_userd gc573_user_streamer 2>/dev/null || true

# 2. VFIO & Memlock Regeln entfernen
rm -f /etc/udev/rules.d/99-gc573-vfio.rules
rm -f /etc/security/limits.d/99-everlive-memlock.conf

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
rm -f /etc/udev/rules.d/70-everlive.rules 2>/dev/null || true
rm -f /usr/local/bin/everlive-audio 2>/dev/null || true

# Virtual Camera entladen (das Paket v4l2loopback bleibt installiert).
modprobe -r v4l2loopback 2>/dev/null || true

udevadm control --reload-rules 2>/dev/null || true
udevadm trigger 2>/dev/null || true

# 4. Desktop-Einträge & Icons entfernen
rm -f "${TARGET_HOME}/.local/share/applications/everlive.desktop"
rm -f "${TARGET_HOME}/.local/share/applications/everlive-intern.desktop"
rm -f "${TARGET_HOME}/.config/autostart/everlive.desktop"
rm -f "${TARGET_HOME}/.local/share/icons/hicolor/"*/apps/everlive.png
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "${TARGET_HOME}/.local/share/applications" 2>/dev/null || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "${TARGET_HOME}/.local/share/icons/hicolor" 2>/dev/null || true
fi

echo "[+] EverLive wurde sauber deinstalliert."
