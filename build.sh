#!/usr/bin/env bash
# ==============================================================================
# build.sh - Build script for EverLive Userspace VFIO Engines
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GC573_DIR="${SCRIPT_DIR}/tools/opengc573-userd"
GC570D_DIR="${SCRIPT_DIR}/tools/opengc570d-userd"

echo "[*] Baue EverLive GC573 Userspace VFIO Engine (gc573_user_streamer & everlive_userd)..."
make -C "${GC573_DIR}" clean >/dev/null 2>&1 || true
make -C "${GC573_DIR}" -j"$(nproc)"

echo "[*] Baue EverLive GC570D (Live Gamer DUO) Userspace Engine..."
make -C "${GC570D_DIR}" clean >/dev/null 2>&1 || true
make -C "${GC570D_DIR}" -j"$(nproc)"

echo "[+] EverLive Userspace Engines erfolgreich gebaut!"

# Optional: Baue Legacy Direct-Renderer falls Quellcode vorhanden
if [ -f "${SCRIPT_DIR}/tools/everlive/everlive_display.c" ]; then
    echo "[*] Baue Legacy Direct-Renderer (everlive_display)..."
    gcc -O3 -march=native -o "${SCRIPT_DIR}/tools/everlive/everlive_display" "${SCRIPT_DIR}/tools/everlive/everlive_display.c" $(sdl2-config --cflags --libs 2>/dev/null || true) -lGL -lm -lX11 -lpthread 2>/dev/null || true
fi
