# SPDX-License-Identifier: GPL-2.0-only
#
# EverLive unit tests: libeverlive-core, GC570D, and GC573.
# Core tests link libeverlive_core.a; engine tests compile only the modules
# they exercise.

CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
override CFLAGS += -std=gnu11 -D_GNU_SOURCE
DEPFLAGS := -MMD -MP

CORE_DIR   := ../tools/libeverlive-core
CORE_V4L2_DIR := $(CORE_DIR)/v4l2
GC573_DIR  := ../tools/opengc573-userd
GC570D_DIR := ../tools/opengc570d-userd
GC570D_SRC := $(GC570D_DIR)/src
HWS_DIR    := ../tools/openhws-userd
HWS_SRC    := $(HWS_DIR)/src
EVERLIVE_DIR := ../tools/everlive
V4L2_TEST_DSOS := test_gc573-v4l2.so test_gc570d-v4l2.so

INCLUDES := -I$(CORE_DIR) -I$(GC570D_SRC) -I$(GC570D_DIR) -I$(GC573_DIR)/src -I$(HWS_SRC)
DEFS_GC570D := -DGC570D_USERSPACE

CORE_LIB := $(CORE_DIR)/libeverlive_core.a
JSMN_SRC := $(CORE_DIR)/third_party/jsmn.c
AUDIO_PROBE_SRC := $(EVERLIVE_DIR)/everlive_audio_probe.c

# One sanitizer switch drives the compiler flags, the runtime preload, and the
# post-build instrumentation check.  Values: none (default), address, thread.
sanitize ?= none

ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
TSAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer

ifeq ($(filter $(sanitize),none address thread),)
$(error unknown sanitize='$(sanitize)' (expected none, address, or thread))
endif

ifeq ($(sanitize),address)
SANITIZE_CFLAGS := $(ASAN_FLAGS)
else ifeq ($(sanitize),thread)
SANITIZE_CFLAGS := $(TSAN_FLAGS)
else
SANITIZE_CFLAGS :=
endif

override CFLAGS += $(SANITIZE_CFLAGS)

# Strict sanitizer runtime options.  ?= lets callers override them temporarily
# for debugging, and exporting makes them reach every test subshell (including
# run_v4l2_preload.sh).
ASAN_OPTIONS ?= detect_leaks=1:abort_on_error=1:strict_string_checks=1
UBSAN_OPTIONS ?= halt_on_error=1:print_stacktrace=1
TSAN_OPTIONS ?= halt_on_error=1
# The framestore seqlock is intentionally racy; see tests/tsan.supp.
ifeq ($(findstring suppressions=,$(TSAN_OPTIONS)),)
override TSAN_OPTIONS := $(TSAN_OPTIONS):suppressions=$(abspath tsan.supp)
endif
export ASAN_OPTIONS
export UBSAN_OPTIONS
export TSAN_OPTIONS

SANITIZER_RUNTIME :=
ifneq (,$(findstring -fsanitize=address,$(CFLAGS)))
SANITIZER_RUNTIME := $(shell $(CC) -print-file-name=libasan.so)
else ifneq (,$(findstring -fsanitize=thread,$(CFLAGS)))
SANITIZER_RUNTIME := $(shell $(CC) -print-file-name=libtsan.so)
endif

TARGETS := test_card_registry test_frame_convert test_framestore test_ctl_proto \
           test_json_rpc test_v4l2_shim test_v4l2_preload test_handoff \
           test_ipc test_edid test_config test_framegen test_audio_probe \
           test_gc573_vfio test_hws_vfio

# Every target here is host-testable (no capture hardware required), so the
# whole suite can run under TSan.  The V4L2 DSOs are included explicitly so the
# interposer is instrumented as well.
TSAN_TARGETS := $(TARGETS) $(V4L2_TEST_DSOS)
V4L2_TSAN_TARGETS := test_v4l2_shim test_v4l2_preload $(V4L2_TEST_DSOS)

all: $(TARGETS)

# Build only the static archive: the shared library and everlive-ctl are not
# needed by the tests, and the core Makefile's shared link step does not take
# CFLAGS.  Passing CC/CFLAGS keeps the archive instrumented for sanitizer runs.
$(CORE_LIB): FORCE
	$(MAKE) -C $(CORE_DIR) libeverlive_core.a CC="$(CC)" CFLAGS="$(CFLAGS)"

.PHONY: FORCE

# --- libeverlive-core ---------------------------------------------------
test_card_registry: test_card_registry.c $(CORE_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $(DEPFLAGS) -o $@ $< $(CORE_LIB)

test_frame_convert: test_frame_convert.c $(CORE_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $(DEPFLAGS) -o $@ $< $(CORE_LIB)

test_framestore: test_framestore.c $(CORE_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $(DEPFLAGS) -o $@ $< $(CORE_LIB) -pthread

test_ctl_proto: test_ctl_proto.c $(CORE_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $(DEPFLAGS) -o $@ $< $(CORE_LIB)

test_json_rpc: test_json_rpc.c $(CORE_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $(DEPFLAGS) -o $@ $< $(CORE_LIB)

test_v4l2_shim: test_v4l2_shim.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_card.h \
		$(CORE_V4L2_DIR)/everlive_v4l2_state.h \
		$(CORE_V4L2_DIR)/everlive_v4l2_state.c $(CORE_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(CORE_V4L2_DIR) \
		-DEVERLIVE_V4L2_TESTING $(DEPFLAGS) \
		-o $@ $< $(CORE_V4L2_DIR)/everlive_v4l2_state.c \
		$(CORE_LIB) -pthread

test_v4l2_preload: test_v4l2_preload.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -o $@ $< -ldl -pthread

# Build test-local DSOs so sanitizer runs instrument the interposer as well.
# The production DSOs are still built by their normal engine Makefiles.
test_gc573-v4l2.so: $(GC573_DIR)/src/gc573-v4l2.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_card.h \
		$(CORE_V4L2_DIR)/everlive_v4l2_state.h \
		$(CORE_V4L2_DIR)/everlive_v4l2_state.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_preload.c $(CORE_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(CORE_V4L2_DIR) $(DEPFLAGS) \
		-shared -fPIC -Wl,-Bsymbolic -o $@ \
		$(GC573_DIR)/src/gc573-v4l2.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_state.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_preload.c \
		$(CORE_LIB) -ldl -lrt -pthread

test_gc570d-v4l2.so: $(GC570D_DIR)/src/gc570d-v4l2.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_card.h \
		$(CORE_V4L2_DIR)/everlive_v4l2_state.h \
		$(CORE_V4L2_DIR)/everlive_v4l2_state.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_preload.c $(CORE_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(CORE_V4L2_DIR) $(DEPFLAGS) \
		-shared -fPIC -Wl,-Bsymbolic -o $@ \
		$(GC570D_DIR)/src/gc570d-v4l2.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_state.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_preload.c \
		$(CORE_LIB) -ldl -lrt -pthread

v4l2-preload: test_v4l2_preload $(V4L2_TEST_DSOS)
	@./run_v4l2_preload.sh ./test_v4l2_preload \
		"$(abspath test_gc573-v4l2.so)" \
		"$(abspath test_gc570d-v4l2.so)" \
		"$(SANITIZER_RUNTIME)"

test_handoff: test_handoff.c $(CORE_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $(DEPFLAGS) -o $@ $< $(CORE_LIB) -pthread

test_ipc: test_ipc.c $(CORE_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $(DEPFLAGS) -o $@ $< $(CORE_LIB)

# --- GC570D -------------------------------------------------------------
# These two provide their own kernel-shim stubs, so they must compile the
# engine modules directly and must NOT link the core archive.
test_edid: test_edid.c $(GC570D_SRC)/gc570d-receiver.c \
                        $(GC570D_SRC)/gc570d-splitter.c
	$(CC) $(CFLAGS) $(INCLUDES) $(DEFS_GC570D) -o $@ $^

test_config: test_config.c $(GC570D_SRC)/gc570d-config.c
	$(CC) $(CFLAGS) $(INCLUDES) $(DEFS_GC570D) -o $@ $^

# --- everlive_display ----------------------------------------------------
# The audio probe is pure parsing/validation: no SDL, no PipeWire, no root.
test_audio_probe: test_audio_probe.c $(AUDIO_PROBE_SRC) $(JSMN_SRC)
	$(CC) $(CFLAGS) -o $@ test_audio_probe.c $(AUDIO_PROBE_SRC) $(JSMN_SRC)

# --- GC573 --------------------------------------------------------------
PACER_LIB := $(GC573_DIR)/libeverlive-vk-pacer.so

$(PACER_LIB): FORCE
	$(MAKE) -C $(GC573_DIR) libeverlive-vk-pacer.so

test_framegen: test_framegen.c $(GC573_DIR)/src/gc573_user_framegen.c $(PACER_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_framegen.c $(GC573_DIR)/src/gc573_user_framegen.c -pthread -lm -ldl

# --- VFIO lifecycle ------------------------------------------------------
# One source, two binaries: each engine has its own compat header, so they
# cannot be compiled into the same translation unit.
test_gc573_vfio: test_vfio_teardown.c $(GC573_DIR)/src/gc573_vfio.c $(CORE_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $(DEPFLAGS) -DENGINE_GC573 \
		-o $@ test_vfio_teardown.c $(GC573_DIR)/src/gc573_vfio.c \
		$(CORE_LIB)

test_hws_vfio: test_vfio_teardown.c $(HWS_SRC)/hws_vfio.c
	$(CC) $(CFLAGS) $(INCLUDES) $(DEPFLAGS) -DENGINE_HWS \
		-o $@ test_vfio_teardown.c $(HWS_SRC)/hws_vfio.c

-include $(TARGETS:=.d)

test: all
	@echo "=== libeverlive-core ==="
	@./test_card_registry
	@./test_frame_convert
	@./test_framestore
	@./test_ctl_proto
	@./test_json_rpc
	@./test_v4l2_shim
	@$(MAKE) --no-print-directory v4l2-preload
	@./test_handoff
	@./test_ipc
	@echo "=== everlive_display ==="
	@./test_audio_probe
	@echo "=== GC570D (Live Gamer DUO) ==="
	@./test_edid
	@./test_config
	@echo "=== GC573 (Live Gamer 4K) ==="
	@./test_framegen
	@echo "=== VFIO lifecycle ==="
	@./test_gc573_vfio
	@./test_hws_vfio
	@echo "=== All EverLive tests passed ==="

# Shared sanitizer flow:
#   $(1) sanitizer kind (address|thread)
#   $(2) build targets
#   $(3) test entry point (default: test)
# The core is cleaned before and after so an instrumented archive never leaks
# into a normal build and the objects are always compiled with active flags.
define run-sanitized
+$(MAKE) clean
+@status=0; \
	$(MAKE) sanitize=$(1) $(2) && \
	$(MAKE) sanitize=$(1) verify-sanitizer && \
	$(MAKE) sanitize=$(1) $(if $(3),$(3),test) || status=$$?; \
	$(MAKE) -C $(CORE_DIR) clean; \
	exit $$status
endef

asan:
	$(call run-sanitized,address,all,test)

tsan:
	$(call run-sanitized,thread,$(TSAN_TARGETS),test)

# TSan-only V4L2 subset, kept as direct entry points.
tsan-v4l2:
	$(call run-sanitized,thread,$(V4L2_TSAN_TARGETS),v4l2-tsan-run)

v4l2-tsan:
	$(call run-sanitized,thread,$(V4L2_TSAN_TARGETS),v4l2-tsan-run)

v4l2-tsan-run: test_v4l2_shim test_v4l2_preload $(V4L2_TEST_DSOS)
	@./test_v4l2_shim
	@./run_v4l2_preload.sh ./test_v4l2_preload \
		"$(abspath test_gc573-v4l2.so)" \
		"$(abspath test_gc570d-v4l2.so)" \
		"$(SANITIZER_RUNTIME)"

# Fail loudly if the core archive was not actually instrumented by the
# selected sanitizer.  nm is cheap and catches flag-plumbing regressions.
verify-sanitizer: $(CORE_LIB)
ifeq ($(sanitize),address)
	@echo "==> Verifying ASan/UBSan instrumentation in $(CORE_LIB)"
	@nm -A $(CORE_LIB) | grep -q '__asan_' || \
		{ echo "ERROR: $(CORE_LIB) is not ASan-instrumented" >&2; exit 1; }
	@nm -A $(CORE_LIB) | grep -q '__ubsan_' || \
		{ echo "ERROR: $(CORE_LIB) is not UBSan-instrumented" >&2; exit 1; }
	@echo "OK: $(CORE_LIB) contains ASan/UBSan instrumentation"
else ifeq ($(sanitize),thread)
	@echo "==> Verifying TSan instrumentation in $(CORE_LIB)"
	@nm -A $(CORE_LIB) | grep -q '__tsan_' || \
		{ echo "ERROR: $(CORE_LIB) is not TSan-instrumented" >&2; exit 1; }
	@echo "OK: $(CORE_LIB) contains TSan instrumentation"
else
	@echo "ERROR: verify-sanitizer needs sanitize=address or sanitize=thread" >&2
	@exit 1
endif

clean:
	rm -f $(TARGETS) $(TARGETS:=.d) $(V4L2_TEST_DSOS) \
		$(V4L2_TEST_DSOS:.so=.d)
	$(MAKE) -C $(CORE_DIR) clean

.PHONY: all test asan tsan tsan-v4l2 v4l2-preload v4l2-tsan v4l2-tsan-run \
	verify-sanitizer clean
