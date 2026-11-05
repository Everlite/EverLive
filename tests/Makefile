# SPDX-License-Identifier: GPL-2.0-only
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -std=gnu11
INCLUDES := -I../tools/opengc570d-userd/src -I../tools/opengc570d-userd
DEFS := -DGC570D_USERSPACE

DRIVER_DIR := ../tools/opengc570d-userd/src

TARGETS := test_edid test_config

all: $(TARGETS)

test_edid: test_edid.c $(DRIVER_DIR)/gc570d-receiver.c $(DRIVER_DIR)/gc570d-splitter.c
	$(CC) $(CFLAGS) $(INCLUDES) $(DEFS) -o $@ $^

test_config: test_config.c $(DRIVER_DIR)/gc570d-config.c
	$(CC) $(CFLAGS) $(INCLUDES) $(DEFS) -o $@ $^

test: all
	@echo "Running GC570D unit tests..."
	@./test_edid
	@./test_config

clean:
	rm -f $(TARGETS)

.PHONY: all test clean
