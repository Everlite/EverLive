#!/usr/bin/env bash
# load.sh - Safely swap to open_gc573 driver for AVerMedia Live Gamer 4K GC573
set -euo pipefail

if [[ "$EUID" -ne 0 ]]; then
    echo "[!] Bitte mit sudo ausführen: sudo ./open_gc573/load.sh"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODULE="${SCRIPT_DIR}/open_gc573.ko"

if [[ ! -f "${MODULE}" ]]; then
    echo "[*] open_gc573.ko nicht gefunden. Kompiliere..."
    make -C "${SCRIPT_DIR}" LLVM=1 CC=clang
fi

echo "[*] Prüfe und bereinige geladene Module..."
pkill -f "everlive_display" 2>/dev/null || true
pkill -f "arecord.*CARD=A4K" 2>/dev/null || true
pkill -f "pw-cat.*GC573" 2>/dev/null || true
sleep 0.2

if grep -q "^open_gc573 " /proc/modules; then
    rmmod open_gc573 2>/dev/null || rmmod -f open_gc573 2>/dev/null || true
fi

if grep -q "^open_gc573 " /proc/modules; then
    echo "[!] Fehler: open_gc573 ist noch geladen und wird von einem Prozess blockiert."
    echo "    Bitte prüfen mit: fuser -v /dev/video* /dev/snd/pcm*"
    exit 1
fi

if grep -q "^cx511h " /proc/modules; then
    echo "[*] Entlade proprietären cx511h Treiber..."
    rmmod cx511h 2>/dev/null || true
fi

echo "[*] Lade Kernel-Abhängigkeiten (v4l2-dv-timings, videobuf2, etc.)..."
modprobe v4l2-dv-timings || true
modprobe videobuf2-dma-sg || true
modprobe videobuf2-v4l2 || true
modprobe led-class-multicolor || true
modprobe snd-pcm || true

echo "[*] Lade neuen open_gc573 Treiber (${MODULE})..."
insmod "${MODULE}"

# Sysfs RGB LED & HPD Re-Sync Zugriff für Desktop-Benutzer (Gruppe video) freigeben
chgrp video /sys/class/leds/gc573:rgb:status/brightness /sys/class/leds/gc573:rgb:status/multi_intensity /sys/bus/pci/drivers/open_gc573/*/hpd_pulse 2>/dev/null || true
chmod 0660 /sys/class/leds/gc573:rgb:status/brightness /sys/class/leds/gc573:rgb:status/multi_intensity /sys/bus/pci/drivers/open_gc573/*/hpd_pulse 2>/dev/null || true

echo "[+] open_gc573 erfolgreich geladen!"

# Dauerhafte Boot-Registrierung (damit nach jedem PC-Neustart der Treiber automatisch geladen ist)
MOD_DEST="/lib/modules/$(uname -r)/updates/open_gc573"
mkdir -p "${MOD_DEST}"
install -m 644 "${MODULE}" "${MOD_DEST}/" 2>/dev/null || true
depmod -a "$(uname -r)" 2>/dev/null || true

mkdir -p /etc/modules-load.d
echo "open_gc573" > /etc/modules-load.d/everlive.conf 2>/dev/null || true

mkdir -p /etc/udev/rules.d
cat > /etc/udev/rules.d/70-everlive.rules <<'UDEV_EOF' 2>/dev/null || true
# AVerMedia Live Gamer 4K (GC573) - EverLive Gaming Access
SUBSYSTEM=="video4linux", KERNEL=="video*", ATTRS{vendor}=="0x1461", ATTRS{device}=="0x0054", MODE="0660", TAG+="uaccess", GROUP="video"
SUBSYSTEM=="sound", KERNEL=="pcmC*D*c", ATTRS{vendor}=="0x1461", ATTRS{device}=="0x0054", MODE="0660", TAG+="uaccess", GROUP="audio"
SUBSYSTEM=="leds", KERNEL=="gc573:rgb:status", ACTION=="add", RUN+="/bin/chgrp video /sys/class/leds/%k/brightness /sys/class/leds/%k/multi_intensity", RUN+="/bin/chmod 0660 /sys/class/leds/%k/brightness /sys/class/leds/%k/multi_intensity"
SUBSYSTEM=="pci", ATTRS{vendor}=="0x1461", ATTRS{device}=="0x0054", TEST=="hpd_pulse", RUN+="/bin/chgrp video /sys%p/hpd_pulse", RUN+="/bin/chmod 0660 /sys%p/hpd_pulse"
UDEV_EOF
udevadm control --reload-rules 2>/dev/null || true
udevadm trigger 2>/dev/null || true

echo "[*] Warte auf HDMI-Signal-Lock (max. 5s)..."
SIGNAL_FOUND=0
for _ in $(seq 1 25); do
    VDEV="$(for d in /sys/class/video4linux/video*; do
        [[ "$(readlink -f "$d/device/driver")" == */open_gc573 ]] && echo "/dev/$(basename "$d")"
    done | head -n 1 || true)"
    if [[ -n "${VDEV}" ]] && v4l2-ctl -d "${VDEV}" --query-dv-timings >/dev/null 2>&1; then
        echo "[+] Signal erkannt auf ${VDEV}"
        SIGNAL_FOUND=1
        break
    fi
    sleep 0.2
done
if [[ "${SIGNAL_FOUND}" -eq 0 ]]; then
    echo "[!] Hinweis: Bisher kein HDMI-Signal-Lock erkannt (Quelle aus oder Handshake ausstehend)."
fi
echo ""
echo "=== PCI Treiber Zuordnung ==="
lspci -k -d 1461: || true

echo ""
echo "=== Kernel Meldungen (dmesg) ==="
dmesg | tail -n 25

echo ""
echo "=== V4L2 Geräte ==="
v4l2-ctl --list-devices || true
