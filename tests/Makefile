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
V4L2_TEST_DSOS := test_gc573-v4l2.so test_gc570d-v4l2.so

INCLUDES := -I$(CORE_DIR) -I$(GC570D_SRC) -I$(GC570D_DIR) -I$(GC573_DIR)/src
DEFS_GC570D := -DGC570D_USERSPACE

CORE_LIB := $(CORE_DIR)/libeverlive_core.a
SANITIZE := -fsanitize=address,undefined -fno-omit-frame-pointer
TSAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer

SANITIZER_RUNTIME :=
ifneq (,$(findstring -fsanitize=address,$(CFLAGS)))
SANITIZER_RUNTIME := $(shell $(CC) -print-file-name=libasan.so)
else ifneq (,$(findstring -fsanitize=thread,$(CFLAGS)))
SANITIZER_RUNTIME := $(shell $(CC) -print-file-name=libtsan.so)
endif

TARGETS := test_card_registry test_frame_convert test_framestore test_ctl_proto \
           test_json_rpc test_v4l2_shim test_v4l2_preload test_handoff \
           test_edid test_config test_framegen

all: $(TARGETS)

$(CORE_LIB): FORCE
	$(MAKE) -C $(CORE_DIR)

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

# --- GC570D -------------------------------------------------------------
# These two provide their own kernel-shim stubs, so they must compile the
# engine modules directly and must NOT link the core archive.
test_edid: test_edid.c $(GC570D_SRC)/gc570d-receiver.c \
                        $(GC570D_SRC)/gc570d-splitter.c
	$(CC) $(CFLAGS) $(INCLUDES) $(DEFS_GC570D) -o $@ $^

test_config: test_config.c $(GC570D_SRC)/gc570d-config.c
	$(CC) $(CFLAGS) $(INCLUDES) $(DEFS_GC570D) -o $@ $^

# --- GC573 --------------------------------------------------------------
PACER_LIB := $(GC573_DIR)/libeverlive-vk-pacer.so

$(PACER_LIB): FORCE
	$(MAKE) -C $(GC573_DIR) libeverlive-vk-pacer.so

test_framegen: test_framegen.c $(GC573_DIR)/src/gc573_user_framegen.c $(PACER_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_framegen.c $(GC573_DIR)/src/gc573_user_framegen.c -pthread -lm -ldl

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
	@echo "=== GC570D (Live Gamer DUO) ==="
	@./test_edid
	@./test_config
	@echo "=== GC573 (Live Gamer 4K) ==="
	@./test_framegen
	@echo "=== All EverLive tests passed ==="

asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) $(SANITIZE)" all
	$(MAKE) CFLAGS="$(CFLAGS) $(SANITIZE)" test

tsan tsan-v4l2:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) $(TSAN_FLAGS)" v4l2-tsan

v4l2-tsan: test_v4l2_shim test_v4l2_preload $(V4L2_TEST_DSOS)
	@./test_v4l2_shim
	@./run_v4l2_preload.sh ./test_v4l2_preload \
		"$(abspath test_gc573-v4l2.so)" \
		"$(abspath test_gc570d-v4l2.so)" \
		"$(SANITIZER_RUNTIME)"

clean:
	rm -f $(TARGETS) $(TARGETS:=.d) $(V4L2_TEST_DSOS) \
		$(V4L2_TEST_DSOS:.so=.d)

.PHONY: all test asan tsan tsan-v4l2 v4l2-preload v4l2-tsan clean
