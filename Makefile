# SPDX-License-Identifier: GPL-2.0-only
#
# EverLive — top-level orchestration.
# Build order: libeverlive-core, then both engines, then tests.

CORE_DIR   := tools/libeverlive-core
GC573_DIR  := tools/opengc573-userd
GC570D_DIR := tools/opengc570d-userd
TESTS_DIR  := tests

.PHONY: all core gc573 gc570d test check asan clean install uninstall

all: core gc573 gc570d

core:
	+$(MAKE) -C $(CORE_DIR)

gc573: core
	+$(MAKE) -C $(GC573_DIR)

gc570d: core
	+$(MAKE) -C $(GC570D_DIR)

test: all
	+$(MAKE) -C $(TESTS_DIR) test

check: test

asan: all
	+$(MAKE) -C $(TESTS_DIR) asan

clean:
	+$(MAKE) -C $(CORE_DIR) clean
	+$(MAKE) -C $(GC573_DIR) clean
	+$(MAKE) -C $(GC570D_DIR) clean
	+$(MAKE) -C $(TESTS_DIR) clean

install: all
	+$(MAKE) -C $(CORE_DIR) install
	+$(MAKE) -C $(GC573_DIR) install
	+$(MAKE) -C $(GC570D_DIR) install

uninstall:
	+$(MAKE) -C $(CORE_DIR) uninstall
	+$(MAKE) -C $(GC573_DIR) uninstall
	+$(MAKE) -C $(GC570D_DIR) uninstall
