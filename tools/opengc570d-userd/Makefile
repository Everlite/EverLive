# SPDX-License-Identifier: GPL-2.0-only
#
# EverLive GC570D (Live Gamer DUO) userspace VFIO engine.
#
# The hardware sources are derived from neoyagami/OpenGC570D and the
# PolygonPrincess/OpenGC570D-Userspace userspace port.  See NOTICE.md.

CC ?= gcc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g -Wall -Wextra -pthread
LDFLAGS ?= -pthread
DEPFLAGS := -MMD -MP

CORE_DIR := ../libeverlive-core
CORE_LIB := $(CORE_DIR)/libeverlive_core.a

USER_CFLAGS := $(CFLAGS) -O2 -g -Wall -Wextra -Wno-unused-parameter \
	-Wno-sign-compare -std=gnu11 \
	-D_GNU_SOURCE -DGC570D_USERSPACE -DGC570D_DEVELOPER_DEBUGFS \
	-Isrc -I. -I$(CORE_DIR)
override LDFLAGS += -pthread
USER_LDFLAGS := $(LDFLAGS) -lm -lrt

SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell $(PKG_CONFIG) --libs sdl2 2>/dev/null)
PW_CFLAGS  := $(shell $(PKG_CONFIG) --cflags libpipewire-0.3 2>/dev/null)
PW_LIBS    := $(shell $(PKG_CONFIG) --libs libpipewire-0.3 2>/dev/null)

ifneq ($(strip $(SDL_LIBS)),)
USER_CFLAGS += -DHAVE_SDL2 $(SDL_CFLAGS)
USER_LDFLAGS += $(SDL_LIBS)
endif

ifneq ($(strip $(PW_LIBS)),)
USER_CFLAGS += -DHAVE_LIBPIPEWIRE $(PW_CFLAGS)
USER_LDFLAGS += $(PW_LIBS)
endif

USERD_SRCS := \
	src/gc570d-userspace.c \
	src/gc570d-config.c \
	src/gc570d-main.c \
	src/gc570d-hw.c \
	src/gc570d-i2c.c \
	src/gc570d-led.c \
	src/gc570d-receiver.c \
	src/gc570d-splitter.c \
	src/gc570d-dma.c \
	src/gc570d-audio.c \
	src/gc570d-video.c \
	src/gc570d_everlive.c \
	src/gc570d-jsonrpc.c

USERD_OBJS := $(USERD_SRCS:.c=.o)

USER_HEADERS := \
	src/gc570d.h \
	src/gc570d-config.h \
	src/gc570d-user-compat.h \
	src/gc570d_everlive.h \
	src/gc570d-jsonrpc.h \
	src/gc570d_font8x8.h \
	data/gc570d_hdr_lut_3000.inc \
	data/gc570d_no_signal_640x360.inc

TARGETS := opengc570d-userd gc570d-ctl libgc570d-v4l2.so

all: $(TARGETS)

opengc570d-userd: $(USERD_OBJS) $(CORE_LIB)
	$(CC) $(USER_CFLAGS) -o $@ $(USERD_OBJS) $(USER_LDFLAGS) $(LDFLAGS) $(CORE_LIB)
	@chmod 0755 $@

$(CORE_LIB):
	$(MAKE) -C $(CORE_DIR)

src/%.o: src/%.c $(USER_HEADERS)
	$(CC) $(USER_CFLAGS) $(DEPFLAGS) -c -o $@ $<

gc570d-ctl: src/gc570d-ctl.c
	$(CC) -O2 -g -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -o $@ $<
	@chmod 0755 $@

libgc570d-v4l2.so: src/gc570d-v4l2-preload.c $(CORE_LIB)
	$(CC) -O2 -g -Wall -Wextra -Wno-unused-parameter \
		-std=gnu11 -D_GNU_SOURCE -shared -fPIC -Isrc -I$(CORE_DIR) \
		-o $@ src/gc570d-v4l2-preload.c $(CORE_LIB) \
		-ldl -lrt -pthread
	@chmod 0644 $@

test selftest:
	$(MAKE) -C ../../tests test

-include $(USERD_OBJS:.o=.d)

PREFIX ?= /usr/local
DESTDIR ?=
BINDIR := $(DESTDIR)$(PREFIX)/bin
LIBDIR := $(DESTDIR)$(PREFIX)/lib
LIBEXECDIR := $(DESTDIR)$(PREFIX)/libexec
SYSTEMD_USER_DIR := $(DESTDIR)$(PREFIX)/lib/systemd/user
SYSTEM_SLEEP_DIR := $(DESTDIR)/usr/lib/systemd/system-sleep
UDEV_RULES_DIR := $(DESTDIR)/etc/udev/rules.d
LIMITS_DIR := $(DESTDIR)/etc/security/limits.d

install-bin: all
	install -d $(BINDIR) $(LIBDIR)
	install -m 0755 opengc570d-userd gc570d-ctl $(BINDIR)/
	command -v setcap >/dev/null 2>&1 && setcap cap_ipc_lock=+ep $(BINDIR)/opengc570d-userd 2>/dev/null || true
	install -m 0644 libgc570d-v4l2.so $(LIBDIR)/

install-system:
	install -d $(LIBEXECDIR) $(SYSTEMD_USER_DIR) $(SYSTEM_SLEEP_DIR) $(UDEV_RULES_DIR) $(LIMITS_DIR)
	install -m 0755 gc570d-vfio-bind.sh $(LIBEXECDIR)/
	sed 's|/usr/local|$(PREFIX)|g' gc570d-sleep.sh > $(LIBEXECDIR)/gc570d-sleep.sh
	chmod 0755 $(LIBEXECDIR)/gc570d-sleep.sh
	install -m 0755 $(LIBEXECDIR)/gc570d-sleep.sh $(SYSTEM_SLEEP_DIR)/gc570d-sleep.sh
	sed 's|/usr/local|$(PREFIX)|g' 99-gc570d-vfio.rules > $(UDEV_RULES_DIR)/99-gc570d-vfio.rules
	chmod 0644 $(UDEV_RULES_DIR)/99-gc570d-vfio.rules
	install -m 0644 99-gc570d-memlock.conf $(LIMITS_DIR)/99-gc570d-memlock.conf
	sed 's|/usr/local|$(PREFIX)|g' opengc570d-userd.service > $(SYSTEMD_USER_DIR)/opengc570d-userd.service
	chmod 0644 $(SYSTEMD_USER_DIR)/opengc570d-userd.service
	install -m 0644 gc570d.conf.example $(DESTDIR)/etc/gc570d.conf.example

install: install-bin install-system

uninstall-bin:
	rm -f $(BINDIR)/opengc570d-userd $(BINDIR)/gc570d-ctl
	rm -f $(LIBDIR)/libgc570d-v4l2.so

uninstall-system:
	rm -f $(LIBEXECDIR)/gc570d-vfio-bind.sh $(LIBEXECDIR)/gc570d-sleep.sh
	rm -f $(SYSTEM_SLEEP_DIR)/gc570d-sleep.sh
	rm -f $(UDEV_RULES_DIR)/99-gc570d-vfio.rules
	rm -f $(LIMITS_DIR)/99-gc570d-memlock.conf
	rm -f $(SYSTEMD_USER_DIR)/opengc570d-userd.service
	rm -f $(DESTDIR)/etc/gc570d.conf.example

uninstall: uninstall-bin uninstall-system

clean:
	rm -f $(USERD_OBJS) $(USERD_OBJS:.o=.d) $(TARGETS)

.PHONY: all clean test selftest install uninstall
