#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# hws-vfio-bind.sh — move an HWS UHD capture card (ACASIS VS2583 / AVMatrix
# VC12-4K) between its kernel driver and vfio-pci at runtime.
#
#   hws-vfio-bind.sh --status            show the card and its driver
#   hws-vfio-bind.sh --bind   [BDF]      hand the card to vfio-pci
#   hws-vfio-bind.sh --restore [BDF]     hand it back to the driver it had before
#   hws-vfio-bind.sh --udev    BDF       bind from 99-hwsuhd-vfio.rules (root;
#                                        /dev/vfio/N goes to group video)
#
# Binding uses driver_override, so only this one device moves; other cards
# with the same PCI ID are not touched.  Nothing persists across a reboot.

set -euo pipefail

IDS="0x8888:0x8581 0x8888:0x85a1 0x8888:0x8591 0x1f33:0x8581"
STATE_DIR="/run/hws-vfio-bind"

find_card() {
    local dev id
    for dev in /sys/bus/pci/devices/*; do
        [ -r "$dev/vendor" ] && [ -r "$dev/device" ] || continue
        id="$(cat "$dev/vendor"):$(cat "$dev/device")"
        # Subsystem 0x0007 is the 4K60 HDMI model the engine supports.
        [ "$(cat "$dev/subsystem_device" 2>/dev/null)" = "0x0007" ] || continue
        case " $IDS " in
            *" $id "*) basename "$dev"; return 0 ;;
        esac
    done
    return 1
}

BDF="${2:-}"
[ -n "$BDF" ] || BDF="$(find_card || true)"
if [ -z "$BDF" ] || [ ! -d "/sys/bus/pci/devices/$BDF" ]; then
    echo "No HWS UHD capture card (8888:8581/85A1/8591, 1F33:8581) found." >&2
    exit 1
fi
DEV="/sys/bus/pci/devices/$BDF"
GROUP="$(basename "$(readlink -f "$DEV/iommu_group")" 2>/dev/null || true)"

current_driver() {
    if [ -L "$DEV/driver" ]; then basename "$(readlink -f "$DEV/driver")"; else echo none; fi
}

need_root() {
    if [ "$EUID" -ne 0 ]; then
        exec sudo --preserve-env=SUDO_USER "$0" "$1" "$BDF"
    fi
}

status() {
    echo "Card        : $BDF ($(cat "$DEV/vendor"):$(cat "$DEV/device"), subsystem $(cat "$DEV/subsystem_vendor"):$(cat "$DEV/subsystem_device"))"
    echo "IOMMU group : ${GROUP:-none}"
    echo "Driver      : $(current_driver)"
    echo "Reset method: $(cat "$DEV/reset_method" 2>/dev/null || echo n/a)"
    echo "Power       : $(cat "$DEV/power_state" 2>/dev/null || echo n/a) (runtime PM: $(cat "$DEV/power/control" 2>/dev/null || echo n/a))"
    if [ -n "$GROUP" ]; then
        echo "Group peers : $(ls "/sys/kernel/iommu_groups/$GROUP/devices" | tr '\n' ' ')"
    fi
}

bind_vfio() {
    need_root --bind
    if [ -z "$GROUP" ]; then
        echo "The card has no IOMMU group; enable the IOMMU (intel_iommu=on / amd_iommu=on)." >&2
        exit 1
    fi
    modprobe vfio-pci
    if [ "$(current_driver)" = "vfio-pci" ]; then
        echo "$BDF is already bound to vfio-pci."
    else
        echo vfio-pci > "$DEV/driver_override"
        if [ -L "$DEV/driver" ]; then
            mkdir -p "$STATE_DIR"
            current_driver > "$STATE_DIR/$BDF"
            echo "Unbinding $BDF from $(current_driver) (stop OBS/PipeWire users first if this hangs)..."
            echo -n "$BDF" > "$DEV/driver/unbind"
        fi
        echo -n "$BDF" > /sys/bus/pci/drivers_probe
    fi
    if [ "$(current_driver)" != "vfio-pci" ]; then
        echo "Binding to vfio-pci failed (driver: $(current_driver))." >&2
        exit 1
    fi
    # vfio-pci resets the function on every open and close.  This card can
    # only do a secondary bus reset, which restarts its HDMI path: the signal
    # drops for ~3 s and the source renegotiates its mode.  The engine
    # programs every register it uses, so run it without resets.
    if [ -w "$DEV/reset_method" ] && echo "" > "$DEV/reset_method" 2>/dev/null; then
        echo "PCI reset disabled for $BDF (restored by --restore or a reboot)."
    fi
    # While no process holds the device, vfio-pci runtime-suspends it, and
    # this card drops to D3cold: power is cut, the FPGA reloads on the next
    # open and the HDMI path relocks.  Keep it powered instead.
    if [ -w "$DEV/power/control" ]; then
        echo on > "$DEV/power/control"
        echo "Runtime power management disabled for $BDF (card stays in D0)."
    fi
    # Development access for the invoking user; packaged installs use the
    # udev rule (GROUP=video) instead.
    OWNER="${SUDO_USER:-}"
    if [ -z "$OWNER" ] && [ -n "${PKEXEC_UID:-}" ]; then
        OWNER="$(id -nu "$PKEXEC_UID" 2>/dev/null || true)"
    fi
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        [ -e "/dev/vfio/$GROUP" ] && break
        sleep 0.1
    done
    if [ "${UDEV_MODE:-0}" = 1 ] && [ -e "/dev/vfio/$GROUP" ]; then
        chgrp video "/dev/vfio/$GROUP" && chmod 0660 "/dev/vfio/$GROUP"
    elif [ -n "$OWNER" ] && [ -e "/dev/vfio/$GROUP" ]; then
        chown "$OWNER" "/dev/vfio/$GROUP"
    fi
    DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    if command -v setcap >/dev/null 2>&1; then
        for bin in hws_user_probe hws_user_streamer; do
            [ -f "$DIR/$bin" ] && setcap cap_ipc_lock=+ep "$DIR/$bin" 2>/dev/null || true
        done
    fi
    echo "$BDF bound to vfio-pci (IOMMU group $GROUP)."
}

restore() {
    need_root --restore
    [ -w "$DEV/reset_method" ] && echo default > "$DEV/reset_method" 2>/dev/null || true
    [ -w "$DEV/power/control" ] && echo auto > "$DEV/power/control" 2>/dev/null || true
    echo > "$DEV/driver_override"
    if [ -L "$DEV/driver" ]; then
        echo -n "$BDF" > "$DEV/driver/unbind"
    fi
    # Reload the driver the card had before --bind, or whatever matches its modalias.
    if [ -s "$STATE_DIR/$BDF" ]; then
        modprobe "$(cat "$STATE_DIR/$BDF")" 2>/dev/null || true
        rm -f "$STATE_DIR/$BDF"
    else
        modprobe "$(cat "$DEV/modalias")" 2>/dev/null || true
    fi
    echo -n "$BDF" > /sys/bus/pci/drivers_probe
    echo "$BDF driver: $(current_driver)"
}

case "${1:---status}" in
    --bind|-b) bind_vfio ;;
    --udev) UDEV_MODE=1 bind_vfio ;;
    --restore|--restore-kernel|-r) restore ;;
    --status|-s) status ;;
    *) echo "Usage: $0 {--status|--bind|--restore|--udev} [BDF]" >&2; exit 2 ;;
esac
