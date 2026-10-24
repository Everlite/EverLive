#!/usr/bin/env bash
# unload.sh - Safely unload open_gc573 driver
set -euo pipefail

if [[ "$EUID" -ne 0 ]]; then
    echo "[!] Bitte mit sudo ausführen: sudo ./open_gc573/unload.sh"
    exit 1
fi

if lsmod | grep -q -w "open_gc573"; then
    echo "[*] Entlade open_gc573 Modul..."
    rmmod open_gc573 || true
    echo "[+] open_gc573 erfolgreich entladen."
else
    echo "[*] open_gc573 ist aktuell nicht geladen."
fi
