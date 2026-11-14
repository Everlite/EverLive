# SPDX-License-Identifier: GPL-2.0-only
#
# EverLive GC575 (Live Gamer 4K 2.1) UVC engine.
#
# The GC575 capture function is a USB UVC device (07ca:0575) driven by the
# upstream uvcvideo driver.  This directory builds:
#   - gc575ctl:          private XU/AT diagnostics and profile selection
#   - opengc575-userd:   experimental UVC -> YUY2 framestore publisher
#   - libgc575-v4l2.so:  optional module-free /dev/video64 preload bridge

CC ?= cc
PKG_CONFIG ?= pkg-config
DEPFLAGS := -MMD -MP

BASE_CFLAGS := $(CFLAGS) -O2 -g -Wall -Wextra -Wno-unused-parameter \
	-std=gnu11 -D_GNU_SOURCE -Isrc -I../libeverlive-core
override LDFLAGS += -pthread

CORE_LIB := ../libeverlive-core/libeverlive_core.a
CORE_V4L2_DIR := ../libeverlive-core/v4l2

AV_CFLAGS := $(shell $(PKG_CONFIG) --cflags libavcodec libavutil libswscale 2>/dev/null)
AV_LIBS   := $(shell $(PKG_CONFIG) --libs libavcodec libavutil libswscale 2>/dev/null)
HAVE_AV   := $(shell $(PKG_CONFIG) --exists libavcodec && echo 1 || echo 0)

ifeq ($(HAVE_AV),1)
BASE_CFLAGS += -DHAVE_LIBAVCODEC $(AV_CFLAGS)
endif

TARGETS := gc575ctl opengc575-userd libgc575-v4l2.so

USERD_OBJS := \
	src/gc575_userd.o \
	src/gc575_v4l2.o \
	src/gc575_camera.o \
	src/gc575_xu.o

all: $(TARGETS)

gc575ctl: gc575ctl.c
	$(CC) $(BASE_CFLAGS) -o $@ $< $(LDFLAGS)

opengc575-userd: $(USERD_OBJS) $(CORE_LIB)
	$(CC) $(BASE_CFLAGS) -o $@ $(USERD_OBJS) $(CORE_LIB) \
		$(LDFLAGS) $(AV_LIBS) -lm

libgc575-v4l2.so: src/gc575-v4l2.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_card.h \
		$(CORE_V4L2_DIR)/everlive_v4l2_state.h \
		$(CORE_V4L2_DIR)/everlive_v4l2_state.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_preload.c \
		$(CORE_LIB)
	$(CC) -O2 -g -Wall -Wextra -Wno-unused-parameter \
		-std=gnu11 -D_GNU_SOURCE -shared -fPIC \
		-Isrc -I../libeverlive-core -I$(CORE_V4L2_DIR) \
		-o $@ src/gc575-v4l2.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_state.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_preload.c \
		$(CORE_LIB) -ldl -lrt -pthread
	@chmod 0644 $@

$(CORE_LIB):
	$(MAKE) -C ../libeverlive-core

src/%.o: src/%.c
	$(CC) $(BASE_CFLAGS) $(DEPFLAGS) -c -o $@ $<

PREFIX ?= /usr/local
DESTDIR ?=
BINDIR := $(DESTDIR)$(PREFIX)/bin
LIBDIR := $(DESTDIR)$(PREFIX)/lib

install: all
	install -d $(BINDIR) $(LIBDIR)
	install -m 0755 gc575ctl opengc575-userd play-gc575.sh $(BINDIR)/
	install -m 0644 libgc575-v4l2.so $(LIBDIR)/

uninstall:
	rm -f $(BINDIR)/gc575ctl $(BINDIR)/opengc575-userd
	rm -f $(BINDIR)/play-gc575.sh $(LIBDIR)/libgc575-v4l2.so

clean:
	rm -f src/*.o src/*.d $(TARGETS)

-include $(USERD_OBJS:.o=.d)

.PHONY: all clean install uninstall
