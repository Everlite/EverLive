#!/usr/bin/env bash
# ==============================================================================
# build.sh - Build script for EverLive Userspace VFIO Engine
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
USERD_DIR="${SCRIPT_DIR}/tools/opengc573-userd"

echo "[*] Baue EverLive Userspace VFIO Engine (gc573_user_streamer & everlive_userd)..."
make -C "${USERD_DIR}" clean >/dev/null 2>&1 || true
make -C "${USERD_DIR}" -j"$(nproc)"
echo "[+] EverLive Userspace Engine erfolgreich gebaut!"

# Optional: Baue Legacy Direct-Renderer falls Quellcode vorhanden
if [ -f "${SCRIPT_DIR}/tools/everlive/everlive_display.c" ]; then
    echo "[*] Baue Legacy Direct-Renderer (everlive_display)..."
    gcc -O3 -march=native -o "${SCRIPT_DIR}/tools/everlive/everlive_display" "${SCRIPT_DIR}/tools/everlive/everlive_display.c" $(sdl2-config --cflags --libs 2>/dev/null || true) -lGL -lm -lX11 -lpthread 2>/dev/null || true
fi
