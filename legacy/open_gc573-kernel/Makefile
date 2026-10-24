# SPDX-License-Identifier: GPL-2.0-only

KDIR ?= $(shell if [ -d "/lib/modules/$$(uname -r)/build" ]; then echo "/lib/modules/$$(uname -r)/build"; else ls -d /lib/modules/*/build 2>/dev/null | tail -n1; fi)

# Automatically detect if the running/target kernel was built with Clang/LLVM (e.g. CachyOS)
ifeq ($(origin LLVM),undefined)
  IS_CLANG_KERNEL := $(shell (grep -q '^CONFIG_CC_IS_CLANG=y' $(KDIR)/.config 2>/dev/null || grep -iq 'clang' /proc/version 2>/dev/null) && echo 1 || echo 0)
  ifeq ($(IS_CLANG_KERNEL),1)
    export LLVM = 1
    export CC = clang
  endif
endif

.PHONY: all modules clean

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
