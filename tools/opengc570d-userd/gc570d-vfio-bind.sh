#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Bind AVerMedia Live Gamer DUO GC570D (1461:0054) to vfio-pci.

set -eu

VENDOR="1461"
DEVICE="0054"

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

for devnode in /dev/vfio/*; do
    if [ -e "$devnode" ] && [ "$devnode" != "/dev/vfio/vfio" ]; then
        chmod 0666 "$devnode" 2>/dev/null || true
    fi
done
