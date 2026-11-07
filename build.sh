#!/usr/bin/env bash
# ==============================================================================
# build.sh - Build script for EverLive Userspace VFIO Engines
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE_DIR="${SCRIPT_DIR}/tools/libeverlive-core"
GC573_DIR="${SCRIPT_DIR}/tools/opengc573-userd"
GC570D_DIR="${SCRIPT_DIR}/tools/opengc570d-userd"

echo "[*] Baue EverLive Core Hardware Abstraction Library (libeverlive-core & everlive-ctl)..."
make -C "${CORE_DIR}" clean >/dev/null 2>&1 || true
make -C "${CORE_DIR}" -j"$(nproc)"

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
    SDL_FLAGS="$(pkg-config --cflags --libs sdl2 2>/dev/null || sdl2-config --cflags --libs 2>/dev/null || true)"
    if [ -n "${SDL_FLAGS}" ]; then
        gcc -O3 -o "${SCRIPT_DIR}/tools/everlive/everlive_display" "${SCRIPT_DIR}/tools/everlive/everlive_display.c" ${SDL_FLAGS} -lGL -lm -lX11 -lpthread
        chmod 0755 "${SCRIPT_DIR}/tools/everlive/everlive_display" 2>/dev/null || true
    fi
fi
