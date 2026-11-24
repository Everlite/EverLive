#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Run the preload-side V4L2 tests against the test-built DSOs.
# Usage: run_v4l2_preload.sh TEST_BIN GC573_DSO GC570D_DSO [SANITIZER_RUNTIME]

set -eu

test_bin=${1:?test binary required}
gc573=${2:?GC573 DSO required}
gc570d=${3:?GC570D DSO required}
runtime=${4:-}

if [ -n "$runtime" ]; then
	preload_prefix="$runtime:"
else
	preload_prefix=
fi

export EVERLIVE_V4L2_TARGET=test_v4l2_preload

env LD_PRELOAD="${preload_prefix}${gc573}" "$test_bin" init
env LD_PRELOAD="${preload_prefix}${gc570d}" "$test_bin" dirent
env LD_PRELOAD="${preload_prefix}${gc573}" "$test_bin" dup
env LD_PRELOAD="${preload_prefix}${gc573}:${gc570d}" "$test_bin" coload
env LD_PRELOAD="${preload_prefix}${gc570d}:${gc573}" "$test_bin" coload
