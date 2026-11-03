#!/usr/bin/env bash
# ==============================================================================
# EverLive Experimental - One-Click Launcher for the 1.4 Lab Console Suite
# 100% Blob-Free Userspace VFIO Engine + lsfg-vk Frame Generation (v1.4.0-experimental)
# Supports AVerMedia Live Gamer 4K (GC573) and Live Gamer DUO (GC570D)
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GC573_DIR="${SCRIPT_DIR}/tools/opengc573-userd"
GC570D_DIR="${SCRIPT_DIR}/tools/opengc570d-userd"
VIEWER="${SCRIPT_DIR}/tools/everlive/everlive_viewer.py"

# 0. Detect the installed capture card.  GC573 and GC570D share PCI device
# 1461:0054; only the subsystem device ID distinguishes them.
CARD_MODEL="gc573"
CARD_PCI=""
for dev in /sys/bus/pci/devices/*; do
    [ -f "$dev/vendor" ] || continue
    [ -f "$dev/device" ] || continue
    vendor=$(cat "$dev/vendor" 2>/dev/null || true)
    device=$(cat "$dev/device" 2>/dev/null || true)
    if [ "$vendor" != "0x1461" ]; then
        continue
    fi
    if [ "$device" != "0x0054" ] && [ "$device" != "0x0055" ]; then
        continue
    fi
    CARD_PCI=$(basename "$dev")
    subsystem=$(cat "$dev/subsystem_device" 2>/dev/null || true)
    if [ "$device" = "0x0054" ] && [ "$subsystem" = "0x5700" ]; then
        CARD_MODEL="gc570d"
    fi
    break
done

if [ "$CARD_MODEL" = "gc570d" ]; then
    USERD_DIR="${GC570D_DIR}"
    STREAMER_BIN="${USERD_DIR}/opengc570d-userd"
    BIND_SCRIPT="${USERD_DIR}/gc570d-vfio-bind.sh"
    LOOPBACK_LABEL="EverLive Capture"
    LOOPBACK_LABEL_2="EverLive Capture HDMI 2"
else
    USERD_DIR="${GC573_DIR}"
    STREAMER_BIN="${USERD_DIR}/gc573_user_streamer"
    BIND_SCRIPT="${USERD_DIR}/vfio_pci_bind.sh"
    LOOPBACK_LABEL="EverLive Capture"
    LOOPBACK_LABEL_2=""
fi

# 1. Ensure userspace engine binaries are built and up to date
if ! make -C "${USERD_DIR}" -q >/dev/null 2>&1; then
    echo "[*] Aktualisiere/Kompiliere EverLive Userspace VFIO Engine..."
    make -C "${USERD_DIR}" -j"$(nproc)"
    setcap cap_ipc_lock=+ep "${STREAMER_BIN}" 2>/dev/null || true
fi

# 2. Raise soft memlock limit to hard limit for native 4K DMA mapping
ulimit -S -l unlimited 2>/dev/null || true

# 3. Check PCI driver binding: if not bound to vfio-pci, bind seamlessly
IS_VFIO=0
if [ -n "$CARD_PCI" ]; then
    dev="/sys/bus/pci/devices/${CARD_PCI}"
    if [ -L "$dev/driver" ]; then
        drv=$(basename "$(readlink -f "$dev/driver")")
        if [ "$drv" = "vfio-pci" ]; then
            IS_VFIO=1
        fi
    fi
fi

if [ "$IS_VFIO" -eq 0 ]; then
    if [ -x "${BIND_SCRIPT}" ]; then
        if [ "$CARD_MODEL" = "gc570d" ]; then
            echo "[!] GC570D ist noch nicht an vfio-pci gebunden. Binde an Userspace-Treiber..."
        else
            echo "[!] GC573 ist noch nicht an vfio-pci gebunden. Binde an Userspace-Treiber..."
        fi
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

# 3b. Optional V4L2 loopback sink (--v4l2 / --loopback)
V4L2_DEV=""
V4L2_DEV_2=""
WANT_V4L2=0
ARGS=("$@")
for ((i = 0; i < ${#ARGS[@]}; i++)); do
    arg="${ARGS[$i]}"
    case "$arg" in
        --v4l2)
            WANT_V4L2=1
            if [ $((i + 1)) -lt ${#ARGS[@]} ] && [[ "${ARGS[$((i + 1))]}" == /dev/* ]]; then
                V4L2_DEV="${ARGS[$((i + 1))]}"
            fi
            ;;
        --v4l2=*)
            WANT_V4L2=1
            V4L2_DEV="${arg#--v4l2=}"
            ;;
        --v4l2-2)
            WANT_V4L2=1
            if [ $((i + 1)) -lt ${#ARGS[@]} ] && [[ "${ARGS[$((i + 1))]}" == /dev/* ]]; then
                V4L2_DEV_2="${ARGS[$((i + 1))]}"
            fi
            ;;
        --v4l2-2=*)
            WANT_V4L2=1
            V4L2_DEV_2="${arg#--v4l2-2=}"
            ;;
        --loopback)
            WANT_V4L2=1
            if [ $((i + 1)) -lt ${#ARGS[@]} ] && [[ "${ARGS[$((i + 1))]}" == /dev/* ]]; then
                V4L2_DEV="${ARGS[$((i + 1))]}"
            fi
            ;;
        --loopback=*)
            WANT_V4L2=1
            V4L2_DEV="${arg#--loopback=}"
            ;;
        --loopback-2)
            WANT_V4L2=1
            if [ $((i + 1)) -lt ${#ARGS[@]} ] && [[ "${ARGS[$((i + 1))]}" == /dev/* ]]; then
                V4L2_DEV_2="${ARGS[$((i + 1))]}"
            fi
            ;;
        --loopback-2=*)
            WANT_V4L2=1
            V4L2_DEV_2="${arg#--loopback-2=}"
            ;;
    esac
done

# If launching the GUI (no CLI args) and the expected node is not present,
# attempt to load v4l2loopback.  The DUO publishes HDMI 1 to /dev/video10 and
# HDMI 2 to /dev/video11 so OBS can capture both sources independently.
if [ "$WANT_V4L2" -eq 0 ] && [ $# -eq 0 ]; then
    if [ "$CARD_MODEL" = "gc570d" ]; then
        if [ ! -e "/dev/video10" ] || [ ! -e "/dev/video11" ]; then
            if modprobe -n v4l2loopback >/dev/null 2>&1; then
                WANT_V4L2=1
                V4L2_DEV="/dev/video10"
                V4L2_DEV_2="/dev/video11"
            fi
        fi
    elif [ ! -e "/dev/video10" ]; then
        if modprobe -n v4l2loopback >/dev/null 2>&1; then
            WANT_V4L2=1
            V4L2_DEV="/dev/video10"
        fi
    fi
fi

load_v4l2loopback() {
    local -a MODPROBE_ARGS=(v4l2loopback "$@")
    if [ "$EUID" -eq 0 ]; then
        modprobe "${MODPROBE_ARGS[@]}" 2>/dev/null || true
    elif command -v pkexec >/dev/null 2>&1 && [ ! -t 0 ]; then
        pkexec modprobe "${MODPROBE_ARGS[@]}" 2>/dev/null || true
    else
        sudo modprobe "${MODPROBE_ARGS[@]}" 2>/dev/null || true
    fi
}

if [ "$WANT_V4L2" -eq 1 ]; then
    if [ -z "$V4L2_DEV" ]; then
        V4L2_DEV="/dev/video10"
    fi

    if [ "$CARD_MODEL" = "gc570d" ]; then
        if [ -z "${V4L2_DEV_2:-}" ]; then
            V4L2_DEV_2="/dev/video11"
        fi
        if [ ! -e "$V4L2_DEV" ] || [ ! -e "$V4L2_DEV_2" ]; then
            if [ ! -e "/dev/video10" ] && [ ! -e "/dev/video11" ]; then
                echo "[*] V4L2-Loopback-Knoten fehlen; lade v4l2loopback (video_nr=10,11)..."
                # exclusive_caps=0 keeps VIDEO_CAPTURE advertised even without
                # an active writer, so OBS never drops the devices.
                load_v4l2loopback "video_nr=10,11" \
                    "card_label=${LOOPBACK_LABEL},${LOOPBACK_LABEL_2}" \
                    "exclusive_caps=0"
                for ((try = 0; try < 15; try++)); do
                    if [ -e "$V4L2_DEV" ] && [ -e "$V4L2_DEV_2" ]; then
                        break
                    fi
                    sleep 0.1
                done
            else
                echo "[!] Nur einer der DUO-Loopback-Knoten existiert (${V4L2_DEV}, ${V4L2_DEV_2})." >&2
                echo "    Neu laden: sudo modprobe -r v4l2loopback && sudo modprobe v4l2loopback video_nr=10,11 card_label=\"${LOOPBACK_LABEL},${LOOPBACK_LABEL_2}\" exclusive_caps=0" >&2
            fi
        fi

        for pair in "${V4L2_DEV}:${LOOPBACK_LABEL}" "${V4L2_DEV_2}:${LOOPBACK_LABEL_2}"; do
            node="${pair%%:*}"
            label="${pair#*:}"
            if [ ! -e "$node" ]; then
                echo "[!] ${node} existiert nicht; OBS kann diesen HDMI-Eingang nicht empfangen." >&2
                continue
            fi
            sys_name="/sys/class/video4linux/$(basename "$node")/name"
            if [ -r "$sys_name" ]; then
                card_name="$(cat "$sys_name")"
                if [ "$card_name" != "$label" ]; then
                    echo "[!] ${node} meldet sich als '${card_name}', nicht '${label}'." >&2
                fi
            fi
        done
    else
        if [ ! -e "$V4L2_DEV" ]; then
            VIDEO_NR="${V4L2_DEV##*/video}"
            if ! [[ "$VIDEO_NR" =~ ^[0-9]+$ ]]; then
                VIDEO_NR=10
            fi
            echo "[*] V4L2-Loopback ${V4L2_DEV} nicht gefunden; lade v4l2loopback (video_nr=${VIDEO_NR})..."
            load_v4l2loopback "video_nr=${VIDEO_NR}" \
                "card_label=${LOOPBACK_LABEL}" "exclusive_caps=0"
            for ((try = 0; try < 10; try++)); do
                if [ -e "$V4L2_DEV" ]; then
                    break
                fi
                sleep 0.1
            done
            if [ ! -e "$V4L2_DEV" ]; then
                echo "[!] ${V4L2_DEV} existiert weiterhin nicht." >&2
                echo "    Prüfe: sudo modprobe -r v4l2loopback && sudo modprobe v4l2loopback video_nr=${VIDEO_NR} card_label=\"${LOOPBACK_LABEL}\" exclusive_caps=0" >&2
                if [ $# -gt 0 ]; then
                    exit 1
                fi
            fi
        fi

        V4L2_SYS_NAME="/sys/class/video4linux/$(basename "$V4L2_DEV")/name"
        if [ -r "$V4L2_SYS_NAME" ]; then
            V4L2_CARD_NAME="$(cat "$V4L2_SYS_NAME")"
            if [ "$V4L2_CARD_NAME" != "$LOOPBACK_LABEL" ]; then
                echo "[!] ${V4L2_DEV} meldet sich als '${V4L2_CARD_NAME}', nicht '${LOOPBACK_LABEL}'." >&2
                echo "    Für den korrekten Namen: sudo modprobe -r v4l2loopback && sudo modprobe v4l2loopback video_nr=10 card_label=\"${LOOPBACK_LABEL}\" exclusive_caps=0" >&2
            fi
        fi
    fi

    # A module instance loaded by an older EverLive release still hides the
    # capture caps until a writer appears, which drops the node in OBS.
    V4L2_CAPS_PARAM="/sys/module/v4l2loopback/parameters/exclusive_caps"
    if [ -r "$V4L2_CAPS_PARAM" ] && [ "$(cat "$V4L2_CAPS_PARAM")" = "1" ]; then
        echo "[!] v4l2loopback läuft mit exclusive_caps=1; der Knoten verschwindet in OBS ohne aktiven Writer." >&2
        echo "    Neu laden: sudo modprobe -r v4l2loopback && sudo modprobe v4l2loopback video_nr=10,11 exclusive_caps=0" >&2
    fi
fi

# 4. Direct CLI passthrough: if --cli or streamer-specific flags are passed (without --tray), launch streamer directly.
# The GC570D daemon defaults to headless, so add --sdl unless the caller
# explicitly asked for headless/studio mode.
streamer_exec() {
    local -a extra=()
    if [ "$CARD_MODEL" = "gc570d" ]; then
        local headless=0 has_sdl=0 has_v4l2_2=0
        local a
        for a in "$@"; do
            case "$a" in
                --headless|--studio|-s) headless=1 ;;
                --sdl) has_sdl=1 ;;
                --v4l2-2|--v4l2-2=*|--loopback-2|--loopback-2=*) has_v4l2_2=1 ;;
            esac
        done
        if [ "$headless" -eq 0 ] && [ "$has_sdl" -eq 0 ]; then
            extra+=(--sdl)
        fi
        if [ "$WANT_V4L2" -eq 1 ] && [ "$has_v4l2_2" -eq 0 ] && [ -n "${V4L2_DEV_2:-}" ] && [ -e "$V4L2_DEV_2" ]; then
            extra+=(--v4l2-2 "$V4L2_DEV_2")
        fi
    fi
    exec "${STREAMER_BIN}" "${extra[@]}" "$@"
}

if [ "${1:-}" = "--cli" ]; then
    shift
    streamer_exec "$@"
fi

HAS_TRAY=0
for arg in "$@"; do
    case "$arg" in
        --tray|--minimized)
            HAS_TRAY=1
            break
            ;;
    esac
done

if [ "$HAS_TRAY" -eq 0 ]; then
    for arg in "$@"; do
        case "$arg" in
            --aspect|--aspect=*|--scale|--scale=*|--integer-scale|--edid|--edid=*|--v4l2|--loopback|--headless|--v4l2-2|--loopback-2|--v4l2=*|--loopback=*|--v4l2-2=*|--loopback-2=*|--osd|--no-osd|--osd-pos|--osd-pos=*)
                streamer_exec "$@"
                ;;
            --sdl|--port|--port=*|--pip|--studio|-s|--copy-edid|--no-auto-hdmi1)
                if [ "$CARD_MODEL" = "gc570d" ]; then
                    streamer_exec "$@"
                fi
                ;;
        esac
    done
fi

# 5. Launch EverLive Console Hub GUI
if [ -x "${SCRIPT_DIR}/tools/everlive/.venv/bin/python3" ]; then
    exec "${SCRIPT_DIR}/tools/everlive/.venv/bin/python3" "${VIEWER}" "$@"
elif [ -x "${SCRIPT_DIR}/tools/everlive/.venv/bin/python" ]; then
    exec "${SCRIPT_DIR}/tools/everlive/.venv/bin/python" "${VIEWER}" "$@"
else
    exec "${VIEWER}" "$@"
fi
