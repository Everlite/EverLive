#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=== Building and running GC570D Unit Tests ==="
make -C "${SCRIPT_DIR}" clean
make -C "${SCRIPT_DIR}" test
echo "=== All tests completed successfully! ==="

