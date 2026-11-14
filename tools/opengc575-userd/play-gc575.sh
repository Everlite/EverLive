#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# play-gc575.sh — theoretical end-to-end GC575 path:
#
#   uvcvideo -> opengc575-userd -> /gc575_v4l2_shm
#            -> libgc575-v4l2.so -> /dev/video64 -> everlive_display
#
# This script is intentionally small and observable.  It is not a replacement
# for the EverLive launcher yet; it is the reference path for hardware bring-up.
#
# Environment:
#   PROFILE=0|1       descriptor profile (default: 0)
#   MODE=nv12|yuyv|mjpeg|auto (default: nv12)
#   SIZE=WxH          requested size (default: 1920x1080)
#   FPS=N             requested frame rate (default: 240)
#   DEV=/dev/videoN   force a UVC node (default: auto-detect 07ca:0575)
#   DISPLAY_BIN=...   renderer binary (default: everlive_display)
#   NO_PRELOAD=1      open the real UVC node instead of /dev/video64

set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DAEMON="$DIR/opengc575-userd"
PRELOAD="$DIR/libgc575-v4l2.so"

PROFILE=${PROFILE:-0}
MODE=${MODE:-nv12}
SIZE=${SIZE:-1920x1080}
FPS=${FPS:-240}
DEV=${DEV:-}
DISPLAY_BIN=${DISPLAY_BIN:-}
NO_PRELOAD=${NO_PRELOAD:-0}

if [ ! -x "$DAEMON" ]; then
    echo "[-] $DAEMON not built; run make in $DIR" >&2
    exit 1
fi

if [ -z "$DISPLAY_BIN" ]; then
    for cand in everlive_display "$DIR/../everlive/everlive_display"; do
        if command -v "$cand" >/dev/null 2>&1; then
            DISPLAY_BIN=$(command -v "$cand")
            break
        fi
        if [ -x "$cand" ]; then
            DISPLAY_BIN="$cand"
            break
        fi
    done
fi

if [ -z "$DISPLAY_BIN" ]; then
    echo "[-] everlive_display not found; set DISPLAY_BIN=/path/to/everlive_display" >&2
    exit 1
fi

DAEMON_ARGS="--profile $PROFILE --format $MODE --size $SIZE --fps $FPS"
if [ -n "$DEV" ]; then
    DAEMON_ARGS="$DAEMON_ARGS --device $DEV"
fi

echo "[*] starting opengc575-userd ($DAEMON_ARGS)"
# shellcheck disable=SC2086
"$DAEMON" $DAEMON_ARGS &
DPID=$!

cleanup() {
    echo "[*] stopping opengc575-userd"
    kill "$DPID" 2>/dev/null || true
    wait "$DPID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# Give the daemon a moment to negotiate the UVC mode and create the store.
sleep 1
if ! kill -0 "$DPID" 2>/dev/null; then
    echo "[-] opengc575-userd exited during startup" >&2
    exit 1
fi

if [ "$NO_PRELOAD" = "0" ] && [ -f "$PRELOAD" ]; then
    echo "[*] starting $DISPLAY_BIN via /dev/video64 (YUY2 framestore)"
    LD_PRELOAD="$PRELOAD" "$DISPLAY_BIN" -d /dev/video64 -F yuyv
else
    if [ -z "$DEV" ]; then
        echo "[-] NO_PRELOAD=1 requires DEV=/dev/videoN" >&2
        exit 1
    fi
    echo "[*] starting $DISPLAY_BIN directly on $DEV ($MODE)"
    "$DISPLAY_BIN" -d "$DEV" -F "$MODE" --size "$SIZE" --fps "$FPS"
fi
