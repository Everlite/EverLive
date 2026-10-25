#!/usr/bin/env bash
# ==============================================================================
# EverLive - One-Click Launcher for Console Gaming Suite (v1.1.0)
# 100% Blob-Free Userspace VFIO Engine for AVerMedia Live Gamer 4K (GC573)
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
USERD_DIR="${SCRIPT_DIR}/tools/opengc573-userd"
VIEWER="${SCRIPT_DIR}/tools/everlive/everlive_viewer.py"

# 1. Ensure userspace engine binaries are built and up to date
if ! make -C "${USERD_DIR}" -q >/dev/null 2>&1; then
    echo "[*] Aktualisiere/Kompiliere EverLive Userspace VFIO Engine..."
    make -C "${USERD_DIR}" -j"$(nproc)"
    setcap cap_ipc_lock=+ep "${USERD_DIR}/gc573_user_streamer" 2>/dev/null || true
fi

# 2. Raise soft memlock limit to hard limit for native 4K DMA mapping
ulimit -S -l unlimited 2>/dev/null || true

# 3. Check PCI driver binding: if not bound to vfio-pci, bind seamlessly
IS_VFIO=0
for dev in /sys/bus/pci/devices/*; do
    if [ -f "$dev/vendor" ] && [ -f "$dev/device" ]; then
        vendor=$(cat "$dev/vendor" 2>/dev/null || true)
        device=$(cat "$dev/device" 2>/dev/null || true)
        if [ "$vendor" = "0x1461" ] && { [ "$device" = "0x0054" ] || [ "$device" = "0x0055" ]; }; then
            if [ -L "$dev/driver" ]; then
                drv=$(basename "$(readlink -f "$dev/driver")")
                if [ "$drv" = "vfio-pci" ]; then
                    IS_VFIO=1
                fi
            fi
            break
        fi
    fi
done

if [ "$IS_VFIO" -eq 0 ]; then
    if [ -f "${USERD_DIR}/vfio_pci_bind.sh" ]; then
        echo "[!] GC573 ist noch nicht an vfio-pci gebunden. Binde an Userspace-Treiber..."
        if [ "$EUID" -ne 0 ]; then
            if command -v pkexec >/dev/null 2>&1 && [ ! -t 0 ]; then
                pkexec "${USERD_DIR}/vfio_pci_bind.sh" --bind || true
            else
                sudo "${USERD_DIR}/vfio_pci_bind.sh" --bind || true
            fi
        else
            "${USERD_DIR}/vfio_pci_bind.sh" --bind || true
        fi
    fi
fi

# 4. Direct CLI passthrough: if --cli is passed, launch streamer directly
if [ "${1:-}" = "--cli" ]; then
    shift
    "${USERD_DIR}/gc573_user_streamer" "$@"
    exit $?
fi

# 5. Launch EverLive Console Hub GUI
if [ -x "${SCRIPT_DIR}/tools/everlive/.venv/bin/python3" ]; then
    exec "${SCRIPT_DIR}/tools/everlive/.venv/bin/python3" "${VIEWER}" "$@"
elif [ -x "${SCRIPT_DIR}/tools/everlive/.venv/bin/python" ]; then
    exec "${SCRIPT_DIR}/tools/everlive/.venv/bin/python" "${VIEWER}" "$@"
else
    exec "${VIEWER}" "$@"
fi
