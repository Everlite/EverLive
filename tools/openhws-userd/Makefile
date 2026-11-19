# SPDX-License-Identifier: GPL-2.0-only
#
# EverLive HWS UHD (ACASIS VS2583 / AVMatrix VC12-4K) userspace VFIO engine.
#
# Self-contained: only libeverlive-core (frame store, JSON-RPC helpers, socket
# path resolver, V4L2 preload engine) is shared with the other engines.

CC ?= gcc
PKG_CONFIG ?= pkg-config
DEPFLAGS := -MMD -MP

CORE_DIR      := ../libeverlive-core
CORE_LIB      := $(CORE_DIR)/libeverlive_core.a
CORE_V4L2_DIR := $(CORE_DIR)/v4l2

VERSION ?= 1.0.0

BASE_CFLAGS := $(CFLAGS) -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
	-std=gnu11 -D_GNU_SOURCE -DHWS_ENGINE_VERSION='"$(VERSION)"' -Isrc -I$(CORE_DIR)
override LDFLAGS += -pthread

SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell $(PKG_CONFIG) --libs sdl2 2>/dev/null)
X11_CFLAGS := $(shell $(PKG_CONFIG) --cflags x11 2>/dev/null)
X11_LIBS   := $(shell $(PKG_CONFIG) --libs x11 2>/dev/null)
PW_CFLAGS  := $(shell $(PKG_CONFIG) --cflags libpipewire-0.3 2>/dev/null)
PW_LIBS    := $(shell $(PKG_CONFIG) --libs libpipewire-0.3 2>/dev/null)

STREAMER_CFLAGS := $(BASE_CFLAGS) $(PW_CFLAGS)
STREAMER_LIBS   := $(PW_LIBS) -lm
ifneq ($(strip $(SDL_LIBS)),)
STREAMER_CFLAGS += -DHAVE_SDL2 $(SDL_CFLAGS)
STREAMER_LIBS   += $(SDL_LIBS)
endif
ifneq ($(strip $(X11_LIBS)),)
STREAMER_CFLAGS += -DHAVE_X11 $(X11_CFLAGS)
STREAMER_LIBS   += $(X11_LIBS)
endif

HW_OBJS := src/hws_user_hw.o src/hws_vfio.o
STREAM_OBJS := src/hws_capture.o src/hws_camera.o src/hws_v4l2out.o \
	src/hws_ipc.o src/hws_nosignal.o src/hws_audio_tap.o src/hws_log.o

TARGETS := hws_user_probe hws_user_streamer libhwsuhd-v4l2.so

all: $(TARGETS)

hws_user_probe: src/hws_user_probe.o $(HW_OBJS)
	$(CC) $(BASE_CFLAGS) -o $@ $^ $(LDFLAGS) -lm

hws_user_streamer: src/hws_user_streamer.o $(STREAM_OBJS) $(HW_OBJS) $(CORE_LIB)
	$(CC) $(STREAMER_CFLAGS) -o $@ $^ $(LDFLAGS) $(STREAMER_LIBS) -lrt

libhwsuhd-v4l2.so: src/hwsuhd-v4l2.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_card.h \
		$(CORE_V4L2_DIR)/everlive_v4l2_state.h \
		$(CORE_V4L2_DIR)/everlive_v4l2_state.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_preload.c \
		$(CORE_DIR)/everlive_framestore.h \
		$(CORE_LIB)
	$(CC) -O2 -g -Wall -Wextra -Wno-unused-parameter \
		-std=gnu11 -D_GNU_SOURCE -shared -fPIC \
		-Isrc -I$(CORE_DIR) -I$(CORE_V4L2_DIR) \
		-o $@ src/hwsuhd-v4l2.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_state.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_preload.c \
		$(CORE_LIB) -ldl -lrt -pthread
	@chmod 0644 $@

$(CORE_LIB):
	$(MAKE) -C $(CORE_DIR)

src/hws_user_streamer.o src/hws_audio_tap.o: CFLAGS_EXTRA = $(STREAMER_CFLAGS)
src/%.o: src/%.c
	$(CC) $(if $(CFLAGS_EXTRA),$(CFLAGS_EXTRA),$(BASE_CFLAGS)) $(DEPFLAGS) -c -o $@ $<

-include $(wildcard src/*.d)

PREFIX ?= /usr/local
DESTDIR ?=
BINDIR := $(DESTDIR)$(PREFIX)/bin
LIBDIR := $(DESTDIR)$(PREFIX)/lib
LIBEXECDIR := $(DESTDIR)$(PREFIX)/libexec
UDEV_RULES_DIR := $(DESTDIR)/etc/udev/rules.d

install-bin: all
	install -d $(BINDIR) $(LIBDIR) $(LIBEXECDIR)
	install -m 0755 hws_user_streamer hws_user_probe $(BINDIR)/
	command -v setcap >/dev/null 2>&1 && setcap cap_ipc_lock=+ep $(BINDIR)/hws_user_streamer 2>/dev/null || true
	command -v setcap >/dev/null 2>&1 && setcap cap_ipc_lock=+ep $(BINDIR)/hws_user_probe 2>/dev/null || true
	install -m 0644 libhwsuhd-v4l2.so $(LIBDIR)/
	install -m 0755 hws-vfio-bind.sh $(LIBEXECDIR)/

install-system:
	install -d $(UDEV_RULES_DIR)
	install -m 0644 99-hwsuhd-vfio.rules $(UDEV_RULES_DIR)/99-hwsuhd-vfio.rules

install: install-bin install-system

uninstall-bin:
	rm -f $(BINDIR)/hws_user_streamer $(BINDIR)/hws_user_probe
	rm -f $(LIBDIR)/libhwsuhd-v4l2.so $(LIBEXECDIR)/hws-vfio-bind.sh

uninstall-system:
	rm -f $(UDEV_RULES_DIR)/99-hwsuhd-vfio.rules

uninstall: uninstall-bin uninstall-system

clean:
	rm -f src/*.o src/*.d $(TARGETS)

.PHONY: all clean install install-bin install-system uninstall uninstall-bin uninstall-system
