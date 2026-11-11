#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# ==============================================================================
# everlive-service.sh — Universal Background Capture Service for EverLive
#
# Automatically detects the installed card (GC570D, GC573), ensures vfio-pci
# binding, raises DMA memlock limits, and runs the capture engine in headless mode.
# Keeps HDMI passthrough, PipeWire audio, and virtual cameras active at all times.
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

CTL_BIN="${SCRIPT_DIR}/everlive-ctl"
if [ ! -x "${CTL_BIN}" ]; then
    CTL_BIN="$(command -v everlive-ctl 2>/dev/null || true)"
fi

CARD_MODEL=""
CARD_PCI=""
IS_BOUND=0

# 1. Detect the installed card.  Prefer the core registry (everlive-ctl),
#    then fall back to sysfs when the CLI is unavailable.
detect_card_sysfs() {
    local dev vendor device subsystem

    CARD_MODEL=""
    CARD_PCI=""
    for dev in /sys/bus/pci/devices/*; do
        [ -f "$dev/vendor" ] || continue
        [ -f "$dev/device" ] || continue
        vendor=$(cat "$dev/vendor" 2>/dev/null || true)
        device=$(cat "$dev/device" 2>/dev/null || true)
        if [ "$vendor" != "0x1461" ]; then
            continue
        fi
        if [ "$device" = "0x0054" ]; then
            subsystem=$(cat "$dev/subsystem_device" 2>/dev/null || true)
            if [ "$subsystem" = "0x5700" ]; then
                CARD_MODEL="gc570d"
            else
                CARD_MODEL="gc573"
            fi
            CARD_PCI=$(basename "$dev")
            break
        elif [ "$device" = "0x0055" ]; then
            CARD_MODEL="gc573"
            CARD_PCI=$(basename "$dev")
            break
        fi
    done
}

if [ -n "${CTL_BIN}" ] && [ -x "${CTL_BIN}" ]; then
    detect_out="$("${CTL_BIN}" detect 2>/dev/null || true)"
    CARD_MODEL="$(printf '%s\n' "${detect_out}" \
        | sed -n 's/.*Model Slug:[[:space:]]*//p' | head -n 1)"
    CARD_PCI="$(printf '%s\n' "${detect_out}" \
        | sed -n 's/.*PCI BDF:[[:space:]]*//p' | head -n 1)"
    case "${CARD_MODEL}" in
        gc573|gc570d) ;;
        *) CARD_MODEL="" ;;
    esac
    if printf '%s\n' "${detect_out}" | grep -q "Driver Bound: vfio-pci"; then
        IS_BOUND=1
    fi
fi

if [ -z "$CARD_MODEL" ]; then
    detect_card_sysfs
    if [ -n "$CARD_PCI" ]; then
        driver_link="/sys/bus/pci/devices/${CARD_PCI}/driver"
        if [ -L "$driver_link" ] &&
           [ "$(basename "$(readlink -f "$driver_link")")" = "vfio-pci" ]; then
            IS_BOUND=1
        fi
    fi
fi

if [ -z "$CARD_MODEL" ]; then
    echo "[!] everlive-service: No supported EverLive capture card detected." >&2
    exit 1
fi

echo "[*] everlive-service: Initializing ${CARD_MODEL} (${CARD_PCI:-unknown BDF})..."

# 2. Select binaries and paths
if [ "$CARD_MODEL" = "gc570d" ]; then
    if [ -x "${REPO_DIR}/tools/opengc570d-userd/opengc570d-userd" ]; then
        ENGINE_BIN="${REPO_DIR}/tools/opengc570d-userd/opengc570d-userd"
    else
        ENGINE_BIN="/usr/local/bin/opengc570d-userd"
    fi
    BIND_SCRIPT=""
    for candidate in \
        "${REPO_DIR}/tools/opengc570d-userd/gc570d-vfio-bind.sh" \
        "/usr/local/libexec/gc570d-vfio-bind.sh" \
        "/usr/local/bin/gc570d-vfio-bind.sh"; do
        [ -x "${candidate}" ] && BIND_SCRIPT="${candidate}" && break
    done
    ENGINE_ARGS=(--headless --loopback)
else
    if [ -x "${REPO_DIR}/tools/opengc573-userd/gc573_user_streamer" ]; then
        ENGINE_BIN="${REPO_DIR}/tools/opengc573-userd/gc573_user_streamer"
    else
        ENGINE_BIN="/usr/local/bin/gc573_user_streamer"
    fi
    BIND_SCRIPT=""
    for candidate in \
        "${REPO_DIR}/tools/opengc573-userd/vfio_pci_bind.sh" \
        "/usr/local/bin/vfio_pci_bind.sh"; do
        [ -x "${candidate}" ] && BIND_SCRIPT="${candidate}" && break
    done
    ENGINE_ARGS=(--headless)
fi

# 3. Ensure PCI device is bound to vfio-pci
if [ "$IS_BOUND" -eq 0 ] && [ -n "$CARD_PCI" ]; then
    driver_link="/sys/bus/pci/devices/${CARD_PCI}/driver"
    if [ ! -L "$driver_link" ] || [ "$(basename "$(readlink -f "$driver_link")")" != "vfio-pci" ]; then
        echo "[*] everlive-service: Device not bound to vfio-pci. Executing bind script..."
        if [ -x "${BIND_SCRIPT}" ]; then
            if [ "$EUID" -ne 0 ]; then
                if command -v pkexec >/dev/null 2>&1 && [ ! -t 0 ]; then
                    pkexec "${BIND_SCRIPT}" --bind || true
                else
                    sudo "${BIND_SCRIPT}" --bind || true
                fi
            else
                "${BIND_SCRIPT}" --bind || true
            fi
        fi
    fi
fi

# 4. Set memlock ulimit
ulimit -S -l unlimited 2>/dev/null || ulimit -S -l "$(ulimit -H -l)" 2>/dev/null || true

# 5. Ensure cap_ipc_lock on engine binary
if [ -f "${ENGINE_BIN}" ] && command -v setcap >/dev/null 2>&1; then
    if ! getcap "${ENGINE_BIN}" 2>/dev/null | grep -q "cap_ipc_lock"; then
        echo "[*] everlive-service: Granting cap_ipc_lock=+ep to $(basename "${ENGINE_BIN}")..."
        if [ "$EUID" -eq 0 ]; then
            setcap cap_ipc_lock=+ep "${ENGINE_BIN}" || true
        elif command -v pkexec >/dev/null 2>&1 && [ ! -t 0 ]; then
            pkexec setcap cap_ipc_lock=+ep "${ENGINE_BIN}" || true
        else
            sudo setcap cap_ipc_lock=+ep "${ENGINE_BIN}" || true
        fi
    fi
fi

# 6. Execute engine daemon
echo "[+] everlive-service: Launching ${ENGINE_BIN} ${ENGINE_ARGS[*]}..."
exec "${ENGINE_BIN}" "${ENGINE_ARGS[@]}" "$@"
