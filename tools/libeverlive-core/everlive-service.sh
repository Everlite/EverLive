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

# HWS UHD capture card (subsystem 0x0007): its own engine in
# tools/openhws-userd, auto-detected only when no AVerMedia card is installed
# (or chosen explicitly, see select_card_explicit).
detect_hwsuhd_sysfs() {
    local dev ids

    for dev in /sys/bus/pci/devices/*; do
        [ -f "$dev/subsystem_device" ] || continue
        ids="$(cat "$dev/vendor" "$dev/device" "$dev/subsystem_device" 2>/dev/null | tr '\n' ' ')"
        case "$ids" in
            "0x8888 0x8581 0x0007 "|"0x8888 0x85a1 0x0007 "|"0x8888 0x8591 0x0007 "|"0x1f33 0x8581 0x0007 ")
                CARD_MODEL="hwsuhd"
                CARD_PCI=$(basename "$dev")
                return 0
                ;;
        esac
    done
    return 1
}

# Explicit card choice, in order: EVERLIVE_CARD_PCI (the Console Hub sets it
# when switching cards), EVERLIVE_CARD (gc573|gc570d|hwsuhd), the card chosen
# in the Hub's card selector (card_pci in settings.json).  Without one, the
# auto-detection below applies unchanged.
card_model_for_bdf() {
    local dev="/sys/bus/pci/devices/${1:-}" ids

    [ -n "${1:-}" ] && [ -f "$dev/subsystem_device" ] || return 1
    ids="$(cat "$dev/vendor" "$dev/device" "$dev/subsystem_device" 2>/dev/null | tr '\n' ' ')"
    case "$ids" in
        "0x1461 0x0054 0x5700 ") echo gc570d ;;
        "0x1461 0x0054 "*|"0x1461 0x0055 "*) echo gc573 ;;
        "0x8888 0x8581 0x0007 "|"0x8888 0x85a1 0x0007 "|"0x8888 0x8591 0x0007 "|"0x1f33 0x8581 0x0007 ") echo hwsuhd ;;
        *) return 1 ;;
    esac
}

find_card_of_model() {
    local dev bdf

    for dev in /sys/bus/pci/devices/*; do
        bdf="$(basename "$dev")"
        if [ "$(card_model_for_bdf "$bdf" || true)" = "$1" ]; then
            echo "$bdf"
            return 0
        fi
    done
    return 1
}

use_card() {
    local model

    model="$(card_model_for_bdf "$1")" || return 1
    CARD_MODEL="$model"
    CARD_PCI="$1"
}

select_card_explicit() {
    local pci saved settings="${HOME:-}/.config/everlive/settings.json"

    if [ -n "${EVERLIVE_CARD_PCI:-}" ]; then
        use_card "${EVERLIVE_CARD_PCI}" && return 0
        echo "[!] EVERLIVE_CARD_PCI=${EVERLIVE_CARD_PCI} ist keine unterstützte Karte; automatische Erkennung." >&2
    fi
    case "${EVERLIVE_CARD:-}" in
        gc573|gc570d|hwsuhd)
            pci="$(find_card_of_model "${EVERLIVE_CARD}" || true)"
            if [ -z "$pci" ]; then
                echo "[!] EVERLIVE_CARD=${EVERLIVE_CARD}, aber keine solche Karte gefunden." >&2
                exit 1
            fi
            use_card "$pci"
            return 0
            ;;
    esac
    if [ -r "$settings" ]; then
        saved="$(sed -n 's/^[[:space:]]*"card_pci":[[:space:]]*"\([^"]*\)".*/\1/p' "$settings" | head -n 1)"
        if [ -n "$saved" ] && use_card "$saved"; then
            return 0
        fi
    fi
    return 1
}

select_card_explicit || true

if [ -z "$CARD_MODEL" ] && [ -n "${CTL_BIN}" ] && [ -x "${CTL_BIN}" ]; then
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
    [ -n "$CARD_MODEL" ] || detect_hwsuhd_sysfs || true
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
if [ "$CARD_MODEL" = "hwsuhd" ]; then
    if [ -x "${REPO_DIR}/tools/openhws-userd/hws_user_streamer" ]; then
        ENGINE_BIN="${REPO_DIR}/tools/openhws-userd/hws_user_streamer"
    else
        ENGINE_BIN="$(command -v hws_user_streamer 2>/dev/null || echo /usr/local/bin/hws_user_streamer)"
    fi
    BIND_SCRIPT=""
    for candidate in \
        "${REPO_DIR}/tools/openhws-userd/hws-vfio-bind.sh" \
        "/usr/local/libexec/hws-vfio-bind.sh" \
        "/usr/libexec/hws-vfio-bind.sh"; do
        [ -x "${candidate}" ] && BIND_SCRIPT="${candidate}" && break
    done
    ENGINE_ARGS=(--headless)
elif [ "$CARD_MODEL" = "gc570d" ]; then
    if [ -x "${REPO_DIR}/tools/opengc570d-userd/opengc570d-userd" ]; then
        ENGINE_BIN="${REPO_DIR}/tools/opengc570d-userd/opengc570d-userd"
    elif [ -x "/usr/local/bin/opengc570d-userd" ]; then
        ENGINE_BIN="/usr/local/bin/opengc570d-userd"
    else
        ENGINE_BIN="$(command -v opengc570d-userd 2>/dev/null || echo /usr/local/bin/opengc570d-userd)"
    fi
    BIND_SCRIPT=""
    for candidate in \
        "${REPO_DIR}/tools/opengc570d-userd/gc570d-vfio-bind.sh" \
        "/usr/local/libexec/gc570d-vfio-bind.sh" \
        "/usr/local/bin/gc570d-vfio-bind.sh" \
        "/usr/libexec/gc570d-vfio-bind.sh" \
        "/usr/bin/gc570d-vfio-bind.sh"; do
        [ -x "${candidate}" ] && BIND_SCRIPT="${candidate}" && break
    done
    ENGINE_ARGS=(--headless)
else
    if [ -x "${REPO_DIR}/tools/opengc573-userd/gc573_user_streamer" ]; then
        ENGINE_BIN="${REPO_DIR}/tools/opengc573-userd/gc573_user_streamer"
    elif [ -x "/usr/local/bin/gc573_user_streamer" ]; then
        ENGINE_BIN="/usr/local/bin/gc573_user_streamer"
    else
        ENGINE_BIN="$(command -v gc573_user_streamer 2>/dev/null || echo /usr/local/bin/gc573_user_streamer)"
    fi
    BIND_SCRIPT=""
    for candidate in \
        "${REPO_DIR}/tools/opengc573-userd/vfio_pci_bind.sh" \
        "/usr/local/libexec/vfio_pci_bind.sh" \
        "/usr/local/bin/vfio_pci_bind.sh" \
        "/usr/libexec/vfio_pci_bind.sh" \
        "/usr/bin/vfio_pci_bind.sh"; do
        [ -x "${candidate}" ] && BIND_SCRIPT="${candidate}" && break
    done
    ENGINE_ARGS=(--headless)
fi

SERVICE_ENV_FILE="${XDG_CONFIG_HOME:-${HOME:-}/.config}/everlive/service.env"

append_service_args() {
    local -a raw_args=()
    local i=0 arg dev env_val

    if [ -f "${SERVICE_ENV_FILE}" ]; then
        env_val="$(sed -n 's/^[[:space:]]*EVERLIVE_SERVICE_ARGS=//p' "${SERVICE_ENV_FILE}" | head -n 1 || true)"
        env_val="${env_val%\"}"
        env_val="${env_val#\"}"
        env_val="${env_val%\'}"
        env_val="${env_val#\'}"
        if [ -n "${env_val}" ]; then
            read -r -a raw_args <<< "${env_val}"
        fi
    elif [ -e /dev/video10 ]; then
        # Feed /dev/video10 out of the box when v4l2loopback is loaded so OBS
        # never opens an empty /dev/video10 node before service.env is written.
        raw_args=(--v4l2 /dev/video10)
    fi

    while [ "$i" -lt "${#raw_args[@]}" ]; do
        arg="${raw_args[$i]}"
        case "$arg" in
            --v4l2)
                if [ $((i + 1)) -lt "${#raw_args[@]}" ]; then
                    dev="${raw_args[$((i + 1))]}"
                    if [ -e "${dev}" ]; then
                        ENGINE_ARGS+=(--v4l2 "${dev}")
                    else
                        echo "[!] everlive-service: Skipping --v4l2 ${dev} (device not present)." >&2
                    fi
                    i=$((i + 2))
                    continue
                fi
                ;;
            --v4l2=*)
                dev="${arg#--v4l2=}"
                if [ -e "${dev}" ]; then
                    ENGINE_ARGS+=("${arg}")
                else
                    echo "[!] everlive-service: Skipping ${arg} (device not present)." >&2
                fi
                i=$((i + 1))
                continue
                ;;
            --audio-tap)
                if [ $((i + 1)) -lt "${#raw_args[@]}" ]; then
                    if [ "$CARD_MODEL" = "gc573" ]; then
                        ENGINE_ARGS+=(--audio-tap "${raw_args[$((i + 1))]}")
                    fi
                    i=$((i + 2))
                    continue
                fi
                ;;
            *)
                ENGINE_ARGS+=("${arg}")
                ;;
        esac
        i=$((i + 1))
    done
}

append_service_args

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
