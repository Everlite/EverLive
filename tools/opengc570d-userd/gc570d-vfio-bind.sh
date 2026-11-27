#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Compatibility shim for the GC570D bind helper.  The shared implementation
# lives in packaging/libexec/everlive-vfio-bind.  The old script defaulted to
# binding without arguments; callers that relied on that must pass --bind.

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
	echo "gc570d-vfio-bind.sh: everlive-vfio-bind helper not found" >&2
	exit 1
}

if SETCAP=$(find_tool everlive-setcap); then
	"$SETCAP" "$SELF_DIR/opengc570d-userd" 2>/dev/null || true
fi

exec "$HELPER" --card gc570d "$@"
