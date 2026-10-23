#!/usr/bin/env bash
# play.sh - One-click launcher for EverLive Console Gaming Screen
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VIEWER="${SCRIPT_DIR}/tools/everlive/everlive_viewer.py"

MODULE="${SCRIPT_DIR}/open_gc573/open_gc573.ko"
CURRENT_VERMAGIC="$(modinfo -F vermagic "${MODULE}" 2>/dev/null | awk '{print $1}')"
if [[ ! -f "${MODULE}" ]] || [[ "${CURRENT_VERMAGIC}" != "$(uname -r)" ]]; then
    echo "[*] open_gc573.ko passt nicht zum laufenden Kernel ($(uname -r)). Kompiliere..."
    if grep -q '^CONFIG_CC_IS_CLANG=y' "/usr/lib/modules/$(uname -r)/build/.config" 2>/dev/null || grep -iq 'clang' /proc/version 2>/dev/null; then
        make -C "${SCRIPT_DIR}/open_gc573" -j"$(nproc)" LLVM=1 CC=clang
    else
        make -C "${SCRIPT_DIR}/open_gc573" -j"$(nproc)"
    fi
fi

# Check if open_gc573 is loaded
if ! grep -q "^open_gc573 " /proc/modules; then
    echo "[!] open_gc573 Treiber ist nicht geladen."
    echo "[*] Lade Treiber..."
    if [ ! -t 0 ]; then
        # Launched from application launcher / desktop menu without a terminal
        if command -v pkexec >/dev/null 2>&1; then
            if ! pkexec "${SCRIPT_DIR}/open_gc573/load.sh"; then
                if command -v kdialog >/dev/null 2>&1; then
                    kdialog --error "Authentifizierung abgebrochen: open_gc573 Treiber konnte nicht geladen werden."
                elif command -v zenity >/dev/null 2>&1; then
                    zenity --error --text="Authentifizierung abgebrochen: open_gc573 Treiber konnte nicht geladen werden."
                fi
                exit 1
            fi
        elif command -v kdesu >/dev/null 2>&1; then
            kdesu "${SCRIPT_DIR}/open_gc573/load.sh"
        else
            if command -v kdialog >/dev/null 2>&1; then
                kdialog --error "Der open_gc573 Treiber ist nicht geladen. Bitte führe im Terminal einmal 'sudo ./open_gc573/load.sh' aus."
            elif command -v zenity >/dev/null 2>&1; then
                zenity --error --text="Der open_gc573 Treiber ist nicht geladen. Bitte führe im Terminal einmal 'sudo ./open_gc573/load.sh' aus."
            fi
            exit 1
        fi
    else
        sudo "${SCRIPT_DIR}/open_gc573/load.sh"
    fi
fi

# Auto-compile native renderer if missing or source is newer
if [ ! -f "${SCRIPT_DIR}/tools/everlive/everlive_display" ] || [ "${SCRIPT_DIR}/tools/everlive/everlive_display.c" -nt "${SCRIPT_DIR}/tools/everlive/everlive_display" ]; then
    echo "[*] Kompiliere EverLive Direct-Renderer (everlive_display)..."
    if ! gcc -O3 -march=native -o "${SCRIPT_DIR}/tools/everlive/everlive_display" "${SCRIPT_DIR}/tools/everlive/everlive_display.c" $(sdl2-config --cflags --libs 2>/dev/null) -lGL -lm -lX11 -lpthread; then
        echo "[!] Warnung: Kompilierung von everlive_display fehlgeschlagen. Starte Viewer (nutzt ggf. mpv-Fallback)..."
    fi
    ln -sf everlive_display "${SCRIPT_DIR}/tools/everlive/gc573_display" 2>/dev/null || true
fi

if [ -x "${SCRIPT_DIR}/tools/everlive/.venv/bin/python3" ]; then
    exec "${SCRIPT_DIR}/tools/everlive/.venv/bin/python3" "${VIEWER}" "$@"
elif [ -x "${SCRIPT_DIR}/tools/everlive/.venv/bin/python" ]; then
    exec "${SCRIPT_DIR}/tools/everlive/.venv/bin/python" "${VIEWER}" "$@"
else
    exec "${VIEWER}" "$@"
fi
