#!/usr/bin/env bash
# ==============================================================================
# EverLive - One-Click Launcher for Console Gaming Suite (v1.2.1)
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

# 3b. Optional V4L2 loopback sink (--v4l2 / --loopback)
V4L2_DEV=""
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
    esac
done

# If launching the GUI (no CLI args) and /dev/video10 is not present, attempt to load v4l2loopback
if [ "$WANT_V4L2" -eq 0 ] && [ $# -eq 0 ] && [ ! -e "/dev/video10" ]; then
    if modprobe -n v4l2loopback >/dev/null 2>&1; then
        WANT_V4L2=1
        V4L2_DEV="/dev/video10"
    fi
fi

if [ "$WANT_V4L2" -eq 1 ]; then
    if [ -z "$V4L2_DEV" ]; then
        V4L2_DEV="/dev/video10"
    fi
    if [ ! -e "$V4L2_DEV" ]; then
        VIDEO_NR="${V4L2_DEV##*/video}"
        if ! [[ "$VIDEO_NR" =~ ^[0-9]+$ ]]; then
            VIDEO_NR=10
        fi
        echo "[*] V4L2-Loopback ${V4L2_DEV} nicht gefunden; lade v4l2loopback (video_nr=${VIDEO_NR})..."
        # exclusive_caps=0 keeps VIDEO_CAPTURE advertised even without an active
        # writer, so OBS never drops the node from its device list.
        MODPROBE_ARGS=(v4l2loopback "video_nr=${VIDEO_NR}" "card_label=EverLive Capture" "exclusive_caps=0")
        if [ "$EUID" -eq 0 ]; then
            modprobe "${MODPROBE_ARGS[@]}" 2>/dev/null || true
        else
            if command -v pkexec >/dev/null 2>&1 && [ ! -t 0 ]; then
                pkexec modprobe "${MODPROBE_ARGS[@]}" 2>/dev/null || true
            else
                sudo modprobe "${MODPROBE_ARGS[@]}" 2>/dev/null || true
            fi
        fi
        for ((try = 0; try < 10; try++)); do
            if [ -e "$V4L2_DEV" ]; then
                break
            fi
            sleep 0.1
        done
        if [ ! -e "$V4L2_DEV" ]; then
            echo "[!] ${V4L2_DEV} existiert weiterhin nicht." >&2
            echo "    Prüfe: sudo modprobe -r v4l2loopback && sudo modprobe v4l2loopback video_nr=${VIDEO_NR} card_label=\"EverLive Capture\" exclusive_caps=0" >&2
            if [ $# -gt 0 ]; then
                exit 1
            fi
        fi
    fi

    V4L2_SYS_NAME="/sys/class/video4linux/$(basename "$V4L2_DEV")/name"
    if [ -r "$V4L2_SYS_NAME" ]; then
        V4L2_CARD_NAME="$(cat "$V4L2_SYS_NAME")"
        if [ "$V4L2_CARD_NAME" != "EverLive Capture" ]; then
            echo "[!] ${V4L2_DEV} meldet sich als '${V4L2_CARD_NAME}', nicht 'EverLive Capture'." >&2
            echo "    Für den korrekten Namen: sudo modprobe -r v4l2loopback && sudo modprobe v4l2loopback video_nr=${VIDEO_NR} card_label=\"EverLive Capture\" exclusive_caps=0" >&2
        fi
    fi

    # A module instance loaded by an older EverLive release still hides the
    # capture caps until a writer appears, which drops the node in OBS.
    V4L2_CAPS_PARAM="/sys/module/v4l2loopback/parameters/exclusive_caps"
    if [ -r "$V4L2_CAPS_PARAM" ] && [ "$(cat "$V4L2_CAPS_PARAM")" = "1" ]; then
        echo "[!] v4l2loopback läuft mit exclusive_caps=1; ${V4L2_DEV} verschwindet in OBS ohne aktiven Writer." >&2
        echo "    Neu laden: sudo modprobe -r v4l2loopback && sudo modprobe v4l2loopback video_nr=${VIDEO_NR} card_label=\"EverLive Capture\" exclusive_caps=0" >&2
    fi
fi

# 4. Direct CLI passthrough: if --cli or streamer-specific flags are passed, launch streamer directly
if [ "${1:-}" = "--cli" ]; then
    shift
    "${USERD_DIR}/gc573_user_streamer" "$@"
    exit $?
fi

for arg in "$@"; do
    case "$arg" in
        --aspect|--edid|--v4l2|--loopback|--headless|--v4l2=*|--loopback=*)
            exec "${USERD_DIR}/gc573_user_streamer" "$@"
            ;;
    esac
done

# 5. Launch EverLive Console Hub GUI
if [ -x "${SCRIPT_DIR}/tools/everlive/.venv/bin/python3" ]; then
    exec "${SCRIPT_DIR}/tools/everlive/.venv/bin/python3" "${VIEWER}" "$@"
elif [ -x "${SCRIPT_DIR}/tools/everlive/.venv/bin/python" ]; then
    exec "${SCRIPT_DIR}/tools/everlive/.venv/bin/python" "${VIEWER}" "$@"
else
    exec "${VIEWER}" "$@"
fi
