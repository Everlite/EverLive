#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# play-intern.sh — Smart launcher for EverLive Intern Test / Userspace Mode

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
USERD_DIR="${SCRIPT_DIR}/tools/opengc573-userd"

echo -e "\033[1;33m[*] EverLive (Intern Test - Experimentell)\033[0m"

# Check if card is bound to vfio-pci
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

if [ "$IS_VFIO" -eq 1 ]; then
    echo -e "\033[1;32m[✓] Hardware is bound to vfio-pci (Userspace Mode)!\033[0m"
    echo -e "[*] Launching EverLive Userspace Streamer..."

    # Ensure binaries are built
    if [ ! -f "${USERD_DIR}/gc573_user_streamer" ] || [ ! -f "${USERD_DIR}/everlive_userd" ]; then
        make -C "${USERD_DIR}"
    fi

    	# Stop background daemon so streamer has exclusive VFIO access
    	systemctl --user stop everlive-userd >/dev/null 2>&1 || true
    	killall -q everlive_userd || true
    	sleep 0.2

	# VFIO pins DMA frames against RLIMIT_MEMLOCK.  A native 3840x2160
	# YUYV frame needs ~15.8 MiB, so raise the soft limit to the hard
	# limit.  The streamer also attempts this itself and reports a clean
	# error if the hard limit is still too small.
	ulimit -S -l unlimited 2>/dev/null || true

    	# Run streamer
    	"${USERD_DIR}/gc573_user_streamer" "$@" || true

    # The streamer hands its VFIO context to everlive_userd before tearing
    # down the video DMA.  Only start the systemd fallback if that handoff
    # did not leave a daemon behind (otherwise the service would race for
    # the same VFIO device).
    if ! pgrep -x everlive_userd >/dev/null 2>&1; then
        systemctl --user start everlive-userd >/dev/null 2>&1 || true
    fi
else
    echo -e "\033[1;34m[*] Hardware is bound to Kernel driver. Launching EverLive Viewer...\033[0m"
    export EVERLIVE_TEST_BRANCH=1
    exec "${SCRIPT_DIR}/play.sh" "$@"
fi
