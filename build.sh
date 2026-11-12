#!/usr/bin/env bash
# ==============================================================================
# build.sh - Build script for EverLive Userspace VFIO Engines
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "[*] Building EverLive core library (libeverlive-core & everlive-ctl)..."
echo "[*] Building GC573 (Live Gamer 4K) userspace VFIO engine..."
echo "[*] Building GC570D (Live Gamer DUO) userspace engine..."
make -C "${SCRIPT_DIR}" -j"$(nproc)"

echo "[+] EverLive Userspace Engines built successfully!"

# Optional: build the legacy direct renderer if its source is present
if [ -f "${SCRIPT_DIR}/tools/everlive/everlive_display.c" ]; then
    echo "[*] Building legacy direct renderer (everlive_display)..."
    SDL_FLAGS="$(pkg-config --cflags --libs sdl2 2>/dev/null || sdl2-config --cflags --libs 2>/dev/null || true)"
    if [ -n "${SDL_FLAGS}" ]; then
        gcc -O3 -o "${SCRIPT_DIR}/tools/everlive/everlive_display" \
            "${SCRIPT_DIR}/tools/everlive/everlive_display.c" ${SDL_FLAGS} \
            -lGL -lm -lX11 -lpthread
        chmod 0755 "${SCRIPT_DIR}/tools/everlive/everlive_display" 2>/dev/null || true
    fi
fi
