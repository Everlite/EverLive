#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Compatibility shim for the old GC573 bind helper.  The shared implementation
# lives in packaging/libexec/everlive-vfio-bind.

set -eu

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

find_tool() {
	_name=$1
	for _path in \
		"$SELF_DIR/$_name" \
		"$SELF_DIR/../../packaging/libexec/$_name" \
		"$SELF_DIR/../../../../libexec/$_name" \
		"/usr/local/libexec/$_name" \
		"/usr/libexec/$_name"; do
		if [ -x "$_path" ]; then
			printf '%s\n' "$_path"
			return 0
		fi
	done
	return 1
}

HELPER=$(find_tool everlive-vfio-bind) || {
	echo "vfio_pci_bind.sh: everlive-vfio-bind helper not found" >&2
	exit 1
}

if SETCAP=$(find_tool everlive-setcap); then
	"$SETCAP" \
		"$SELF_DIR/gc573_user_streamer" \
		"$SELF_DIR/gc573_user_probe" \
		"$SELF_DIR/gc573_user_diag" 2>/dev/null || true
fi

exec "$HELPER" --card gc573 "$@"
