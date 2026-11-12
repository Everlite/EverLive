#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Build and run the EverLive unit tests (libeverlive-core, GC570D, GC573).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODE="${1:-}"

case "${MODE}" in
    "")
        make -C "${SCRIPT_DIR}" clean
        make -C "${SCRIPT_DIR}" test
        ;;
    --asan)
        make -C "${SCRIPT_DIR}" asan
        ;;
    -h|--help)
        echo "Usage: $0 [--asan]"
        ;;
    *)
        echo "Unknown option: ${MODE}" >&2
        exit 2
        ;;
esac
