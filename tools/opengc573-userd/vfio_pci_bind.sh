#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# vfio_pci_bind.sh — Seamless driver switching between kernel driver and userspace VFIO

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Find GC573 PCI address
find_gc573() {
    for dev in /sys/bus/pci/devices/*; do
        if [ -f "$dev/vendor" ] && [ -f "$dev/device" ]; then
            vendor=$(cat "$dev/vendor")
            device=$(cat "$dev/device")
            if [ "$vendor" = "0x1461" ] && { [ "$device" = "0x0054" ] || [ "$device" = "0x0055" ]; }; then
                basename "$dev"
                return 0
            fi
        fi
    done
    return 1
}

PCI_DEV=$(find_gc573 || true)

if [ -z "$PCI_DEV" ]; then
    echo -e "${RED}[ERROR] No AVerMedia GC573 capture card (1461:0054/0055) found on PCIe bus!${NC}"
    exit 1
fi

IOMMU_GROUP=$(basename "$(readlink -f "/sys/bus/pci/devices/$PCI_DEV/iommu_group")" || echo "")

status() {
    echo -e "${BLUE}=== AVerMedia GC573 PCIe Driver Status ===${NC}"
    echo "  PCI Device  : $PCI_DEV"
    echo "  IOMMU Group : $IOMMU_GROUP"
    current_drv="none"
    if [ -L "/sys/bus/pci/devices/$PCI_DEV/driver" ]; then
        current_drv=$(basename "$(readlink -f "/sys/bus/pci/devices/$PCI_DEV/driver")")
    fi
    if [ "$current_drv" = "vfio-pci" ]; then
        echo -e "  Current Mode: ${GREEN}USERSPACE (bound to vfio-pci)${NC}"
    elif [ "$current_drv" = "open_gc573" ]; then
        echo -e "  Current Mode: ${BLUE}KERNEL DRIVER (bound to open_gc573.ko)${NC}"
    else
        echo -e "  Current Mode: ${YELLOW}UNBOUND ($current_drv)${NC}"
    fi
}

bind_vfio() {
    if [ "$EUID" -ne 0 ]; then
        echo -e "${YELLOW}[!] Root privileges required to rebind PCIe driver. Re-running with sudo...${NC}"
        exec sudo "$0" --bind
    fi

    echo -e "${YELLOW}[*] Switching GC573 ($PCI_DEV) to Userspace VFIO Mode...${NC}"

    # Load vfio-pci
    modprobe vfio-pci

    # Unbind from current driver if bound
    if [ -L "/sys/bus/pci/devices/$PCI_DEV/driver" ]; then
        cur_drv=$(basename "$(readlink -f "/sys/bus/pci/devices/$PCI_DEV/driver")")
        if [ "$cur_drv" != "vfio-pci" ]; then
            echo -e "  Unbinding from $cur_drv..."
            echo -n "$PCI_DEV" > "/sys/bus/pci/devices/$PCI_DEV/driver/unbind"
        fi
    fi

    # Register vendor/device ID with vfio-pci
    if [ -d /sys/bus/pci/drivers/vfio-pci ]; then
        echo "1461 0054" > /sys/bus/pci/drivers/vfio-pci/new_id 2>/dev/null || true
        echo -n "$PCI_DEV" > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null || true
    fi

    # Set accessible permissions for userspace testing
    if [ -e "/dev/vfio/vfio" ]; then
        chmod 0666 /dev/vfio/vfio
    fi
    if [ -n "$IOMMU_GROUP" ] && [ -e "/dev/vfio/$IOMMU_GROUP" ]; then
        chmod 0666 "/dev/vfio/$IOMMU_GROUP"
    fi

    # Grant memory locking capability for DMA page pinning (needed for >1080p frames)
    DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    if command -v setcap >/dev/null 2>&1; then
        for bin in "$DIR/gc573_user_streamer" "$DIR/gc573_user_diag" "$DIR/gc573_user_probe" "$DIR/everlive_userd"; do
            if [ -f "$bin" ]; then
                setcap cap_ipc_lock=+ep "$bin" 2>/dev/null || true
            fi
        done
    fi

    echo -e "${GREEN}[✓] GC573 successfully bound to vfio-pci in IOMMU group $IOMMU_GROUP!${NC}"
    echo -e "    You can now run userspace tests without root: ${BLUE}./tools/opengc573-userd/gc573_user_probe${NC}"
}

restore_kernel() {
    if [ "$EUID" -ne 0 ]; then
        echo -e "${YELLOW}[!] Root privileges required. Re-running with sudo...${NC}"
        exec sudo "$0" --restore-kernel
    fi

    echo -e "${YELLOW}[*] Restoring GC573 ($PCI_DEV) to Kernel Module (open_gc573.ko)...${NC}"

    if [ -L "/sys/bus/pci/devices/$PCI_DEV/driver" ]; then
        cur_drv=$(basename "$(readlink -f "/sys/bus/pci/devices/$PCI_DEV/driver")")
        if [ "$cur_drv" = "vfio-pci" ]; then
            echo "  Unbinding from vfio-pci..."
            echo -n "$PCI_DEV" > "/sys/bus/pci/drivers/vfio-pci/unbind"
        fi
    fi

    modprobe open_gc573 2>/dev/null || true

    if [ -d /sys/bus/pci/drivers/open_gc573 ]; then
        echo -n "$PCI_DEV" > /sys/bus/pci/drivers/open_gc573/bind 2>/dev/null || true
    fi

    echo -e "${GREEN}[✓] GC573 successfully restored to standard kernel driver!${NC}"
}

case "${1:-}" in
    --bind|-b)
        bind_vfio
        ;;
    --restore-kernel|--restore|-r|--unbind|-u)
        restore_kernel
        ;;
    --status|-s|"")
        status
        ;;
    *)
        echo "Usage: $0 {--bind|--restore-kernel|--status}"
        exit 1
        ;;
esac
