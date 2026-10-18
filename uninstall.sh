#!/usr/bin/env bash
# ==============================================================================
# EverLive - Deinstaller
# ==============================================================================
set -euo pipefail

if [ "$EUID" -ne 0 ]; then
    echo "Bitte mit sudo ausführen: sudo ./uninstall.sh"
    exit 1
fi

TARGET_USER="${SUDO_USER:-$USER}"
TARGET_HOME=$(eval echo "~${TARGET_USER}")

echo "[*] Deinstalliere EverLive..."

# 1. Modul entladen
rmmod open_gc573 2>/dev/null || true

# 2. Modul-Dateien entfernen
rm -rf "/lib/modules/$(uname -r)/updates/open_gc573"
depmod -a "$(uname -r)"

# 3. DKMS entfernen falls vorhanden
if command -v dkms >/dev/null 2>&1; then
    dkms remove -m everlive -v 0.2.0 --all 2>/dev/null || true
    dkms remove -m everlive -v 0.3.0 --all 2>/dev/null || true
    dkms remove -m everlive -v 0.4.0 --all 2>/dev/null || true
    rm -rf /usr/src/everlive-0.2.0
    rm -rf /usr/src/everlive-0.3.0
    rm -rf /usr/src/everlive-0.4.0
fi

# 4. Autostart & udev Regeln entfernen
rm -f /etc/modules-load.d/everlive.conf
rm -f /etc/udev/rules.d/70-everlive.rules
udevadm control --reload-rules 2>/dev/null || true
udevadm trigger 2>/dev/null || true

# 5. Desktop-Einträge & Icons entfernen
rm -f "${TARGET_HOME}/.local/share/applications/everlive.desktop"
rm -f "${TARGET_HOME}/.local/share/icons/hicolor/"*/apps/everlive.png
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "${TARGET_HOME}/.local/share/applications" 2>/dev/null || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "${TARGET_HOME}/.local/share/icons/hicolor" 2>/dev/null || true
fi

echo "[+] EverLive wurde sauber deinstalliert."
