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
GC575_DIR  := ../tools/opengc575-userd
GC570D_SRC := $(GC570D_DIR)/src
GC575_SRC := $(GC575_DIR)/src

INCLUDES := -I$(CORE_DIR) -I$(GC570D_SRC) -I$(GC570D_DIR) -I$(GC573_DIR)/src
DEFS_GC570D := -DGC570D_USERSPACE

CORE_LIB := $(CORE_DIR)/libeverlive_core.a
SANITIZE := -fsanitize=address,undefined -fno-omit-frame-pointer

TARGETS := test_card_registry test_frame_convert test_framestore test_ctl_proto \
           test_json_rpc test_v4l2_shim test_handoff test_edid test_config test_framegen \
           test_gc575_uvc

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
	$(CC) $(CFLAGS) $(INCLUDES) -I$(CORE_V4L2_DIR) $(DEPFLAGS) \
		-o $@ $< $(CORE_V4L2_DIR)/everlive_v4l2_state.c \
		$(CORE_LIB) -pthread

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
test_framegen: test_framegen.c $(GC573_DIR)/src/gc573_user_framegen.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ -pthread -lm

# --- GC575 --------------------------------------------------------------
test_gc575_uvc: test_gc575_uvc.c \
		$(GC575_SRC)/gc575_v4l2.c $(GC575_SRC)/gc575_xu.c
	$(CC) $(CFLAGS) -I$(GC575_SRC) $(DEPFLAGS) -o $@ \
		$(filter %.c,$^)

-include $(TARGETS:=.d)

test: all
	@echo "=== libeverlive-core ==="
	@./test_card_registry
	@./test_frame_convert
	@./test_framestore
	@./test_ctl_proto
	@./test_json_rpc
	@./test_v4l2_shim
	@./test_handoff
	@echo "=== GC570D (Live Gamer DUO) ==="
	@./test_edid
	@./test_config
	@echo "=== GC573 (Live Gamer 4K) ==="
	@./test_framegen
	@echo "=== GC575 (Live Gamer 4K 2.1) ==="
	@./test_gc575_uvc
	@echo "=== All EverLive tests passed ==="

asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) $(SANITIZE)" all
	$(MAKE) CFLAGS="$(CFLAGS) $(SANITIZE)" test

clean:
	rm -f $(TARGETS) $(TARGETS:=.d)

.PHONY: all test asan clean
