# SPDX-License-Identifier: GPL-2.0-only
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -std=gnu11 -D_GNU_SOURCE
INCLUDES := -I../tools/opengc570d-userd/src -I../tools/opengc570d-userd -I../tools/libeverlive-core -I../tools/opengc573-userd
DEFS := -DGC570D_USERSPACE

GC570D_DIR := ../tools/opengc570d-userd/src
CORE_DIR := ../tools/libeverlive-core
GC573_DIR := ../tools/opengc573-userd
CORE_LIB := $(CORE_DIR)/libeverlive_core.a

TARGETS := test_edid test_config test_card_registry test_frame_convert test_ctl_proto test_ipc_json

all: $(TARGETS)

test_edid: test_edid.c $(GC570D_DIR)/gc570d-receiver.c $(GC570D_DIR)/gc570d-splitter.c
	$(CC) $(CFLAGS) $(INCLUDES) $(DEFS) -o $@ $^

test_config: test_config.c $(GC570D_DIR)/gc570d-config.c
	$(CC) $(CFLAGS) $(INCLUDES) $(DEFS) -o $@ $^

test_card_registry: test_card_registry.c $(CORE_DIR)/everlive_card.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

test_frame_convert: test_frame_convert.c $(CORE_DIR)/everlive_frame.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

test_ctl_proto: test_ctl_proto.c $(CORE_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_ctl_proto.c $(CORE_LIB)

asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: all

test_ipc_json: test_ipc_json.c $(CORE_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(CORE_LIB) -pthread

$(CORE_LIB):
	$(MAKE) -C $(CORE_DIR)

test: all
	@echo "Running GC570D & GC573 unit tests..."
	@./test_edid
	@./test_config
	@./test_card_registry
	@./test_frame_convert
	@./test_ctl_proto
	@./test_ipc_json

clean:
	rm -f $(TARGETS)

.PHONY: all test clean asan
