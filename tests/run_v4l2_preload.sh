#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Run the preload-side V4L2 tests against the test-built DSOs.
# Usage: run_v4l2_preload.sh TEST_BIN GC573_DSO GC570D_DSO [SANITIZER_RUNTIME]

set -eu

# Bound the whole script so a regression can never wedge CI.
if [ "${EVERLIVE_V4L2_PRELOAD_TIMEOUT_WRAPPED:-0}" != 1 ]; then
	export EVERLIVE_V4L2_PRELOAD_TIMEOUT_WRAPPED=1
	exec timeout 20 "$0" "$@"
fi

test_bin=${1:?test binary required}
gc573=${2:?GC573 DSO required}
gc570d=${3:?GC570D DSO required}
runtime=${4:-}

if [ -n "$runtime" ]; then
	preload_prefix="$runtime:"
else
	preload_prefix=
fi

# Every invocation is individually bounded as well; the framestore test has
# its own SIGALRM cleanup so a pre-fix deadlock still removes /dev/shm.
run_preload() {
	preload=$1
	shift
	timeout 20 env LD_PRELOAD="${preload_prefix}${preload}" \
		"$test_bin" "$@"
}

export EVERLIVE_V4L2_TARGET=test_v4l2_preload

run_preload "$gc573" init
run_preload "$gc570d" dirent
run_preload "$gc573" dup
run_preload "${gc573}:${gc570d}" coload
run_preload "${gc570d}:${gc573}" coload

# Live frame-store + QUERYCAP regression: a single DSO only sees the node it
# owns, while the co-load run exercises both interposers chained together.
run_preload "$gc573" framestore gc573
run_preload "$gc570d" framestore gc570d
run_preload "${gc573}:${gc570d}" framestore gc573:gc570d
