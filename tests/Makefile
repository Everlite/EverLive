# SPDX-License-Identifier: GPL-2.0-only
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -std=gnu11 -D_GNU_SOURCE
INCLUDES := -I../tools/opengc570d-userd/src -I../tools/opengc570d-userd -I../tools/opengc573-userd
DEFS := -DGC570D_USERSPACE

GC570D_DIR := ../tools/opengc570d-userd/src
GC573_DIR := ../tools/opengc573-userd

TARGETS := test_edid test_config test_framegen test_ipc_json test_edid_unifier test_hdr

all: $(TARGETS)

test_edid: test_edid.c $(GC570D_DIR)/gc570d-receiver.c $(GC570D_DIR)/gc570d-splitter.c
	$(CC) $(CFLAGS) $(INCLUDES) $(DEFS) -o $@ $^

test_config: test_config.c $(GC570D_DIR)/gc570d-config.c
	$(CC) $(CFLAGS) $(INCLUDES) $(DEFS) -o $@ $^

test_framegen: $(GC573_DIR)/test_framegen.c $(GC573_DIR)/gc573_user_framegen.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ -pthread -lm

test_ipc_json: test_ipc_json.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ ../tools/opengc573-userd/third_party/jsmn.o -pthread

test_edid_unifier: test_edid_unifier.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

test_hdr: test_hdr.c $(GC573_DIR)/gc573_user_hdr.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ -lm

asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: all

test: all
	@echo "=== Running GC570D & GC573 Unit Test Suites ==="
	@./test_edid
	@./test_config
	@./test_framegen
	@./test_ipc_json
	@./test_edid_unifier
	@./test_hdr

clean:
	rm -f $(TARGETS)

.PHONY: all test clean asan
