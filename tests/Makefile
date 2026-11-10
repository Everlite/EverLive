# SPDX-License-Identifier: GPL-2.0-only
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -std=gnu11 -D_GNU_SOURCE
INCLUDES := -I../tools/opengc570d-userd/src -I../tools/opengc570d-userd -I../tools/libeverlive-core
DEFS := -DGC570D_USERSPACE

DRIVER_DIR := ../tools/opengc570d-userd/src
CORE_DIR := ../tools/libeverlive-core

TARGETS := test_edid test_config test_card_registry test_frame_convert test_ctl_proto

all: $(TARGETS)

test_edid: test_edid.c $(DRIVER_DIR)/gc570d-receiver.c $(DRIVER_DIR)/gc570d-splitter.c
	$(CC) $(CFLAGS) $(INCLUDES) $(DEFS) -o $@ $^

test_config: test_config.c $(DRIVER_DIR)/gc570d-config.c
	$(CC) $(CFLAGS) $(INCLUDES) $(DEFS) -o $@ $^

test_card_registry: test_card_registry.c $(CORE_DIR)/everlive_card.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

test_frame_convert: test_frame_convert.c $(CORE_DIR)/everlive_frame.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

test_ctl_proto: test_ctl_proto.c $(CORE_DIR)/everlive_ctl_proto.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: all

test: all
	@echo "=== Running Unit Test Suites ==="
	@./test_edid
	@./test_config
	@./test_card_registry
	@./test_frame_convert
	@./test_ctl_proto

clean:
	rm -f $(TARGETS)

.PHONY: all test clean asan
