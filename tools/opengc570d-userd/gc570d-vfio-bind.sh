#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Bind AVerMedia Live Gamer DUO GC570D (1461:0054, subsystem 1461:5700) to vfio-pci
# and grant cap_ipc_lock=+ep to opengc570d-userd so VFIO_IOMMU_MAP_DMA can pin
# all dual-HDMI DMA rings above the default 8 MiB RLIMIT_MEMLOCK.

set -eu

VENDOR="1461"
DEVICE="0054"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

modprobe vfio-pci 2>/dev/null || true

for devpath in /sys/bus/pci/devices/*; do
    [ -e "$devpath/vendor" ] || continue
    v=$(cat "$devpath/vendor" 2>/dev/null || true)
    d=$(cat "$devpath/device" 2>/dev/null || true)
    if [ "$v" = "0x$VENDOR" ] && [ "$d" = "0x$DEVICE" ]; then
        bdf=$(basename "$devpath")
        echo "vfio-pci" > "$devpath/driver_override" 2>/dev/null || true
        if [ -L "$devpath/driver" ]; then
            cur_drv=$(basename "$(readlink "$devpath/driver")")
            if [ "$cur_drv" != "vfio-pci" ]; then
                echo "$bdf" > "$devpath/driver/unbind" 2>/dev/null || true
            fi
        fi
        if [ ! -L "$devpath/driver" ]; then
            echo "$bdf" > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null || \
            echo "$VENDOR $DEVICE" > /sys/bus/pci/drivers/vfio-pci/new_id 2>/dev/null || true
        fi
    fi
done

if [ -e "/dev/vfio/vfio" ]; then
    chown root:video /dev/vfio/vfio 2>/dev/null || true
    chmod 0660 /dev/vfio/vfio 2>/dev/null || true
fi
for devnode in /dev/vfio/*; do
    if [ -e "$devnode" ] && [ "$devnode" != "/dev/vfio/vfio" ]; then
        chown root:video "$devnode" 2>/dev/null || true
        chmod 0660 "$devnode" 2>/dev/null || true
    fi
done

if command -v setcap >/dev/null 2>&1 && [ -f "$SCRIPT_DIR/opengc570d-userd" ]; then
    setcap cap_ipc_lock=+ep "$SCRIPT_DIR/opengc570d-userd" 2>/dev/null || true
fi
