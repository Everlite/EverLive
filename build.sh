#!/usr/bin/env bash
# build.sh - Build script for open_gc573 kernel driver and everlive_display renderer
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "[*] Baue open_gc573 Kernel-Treiber..."
make -C "${SCRIPT_DIR}/open_gc573" clean >/dev/null 2>&1 || true
make -C "${SCRIPT_DIR}/open_gc573" -j"$(nproc)"
echo "[+] open_gc573.ko erfolgreich gebaut!"

echo "[*] Baue EverLive Direct-Renderer (everlive_display)..."
gcc -O3 -march=native -o "${SCRIPT_DIR}/tools/everlive/everlive_display" "${SCRIPT_DIR}/tools/everlive/everlive_display.c" $(sdl2-config --cflags --libs) -lX11 -lpthread
ln -sf everlive_display "${SCRIPT_DIR}/tools/everlive/gc573_display"
chmod +x "${SCRIPT_DIR}/tools/everlive/everlive_display"
if command -v pw-loopback >/dev/null 2>&1; then
    cp -f "$(command -v pw-loopback)" "${SCRIPT_DIR}/tools/everlive/everlive-audio"
    chmod +x "${SCRIPT_DIR}/tools/everlive/everlive-audio"
fi
echo "[+] everlive_display erfolgreich kompiliert!"
