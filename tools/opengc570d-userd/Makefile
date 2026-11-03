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

USER_CFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter \
	-Wno-sign-compare -Wno-format-truncation -std=gnu11 \
	-D_GNU_SOURCE -DGC570D_USERSPACE -DGC570D_DEVELOPER_DEBUGFS \
	-Isrc -I.
USER_LDFLAGS := -pthread -lm -lrt

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
	src/gc570d-main.c \
	src/gc570d-hw.c \
	src/gc570d-i2c.c \
	src/gc570d-led.c \
	src/gc570d-receiver.c \
	src/gc570d-splitter.c \
	src/gc570d-dma.c \
	src/gc570d-audio.c \
	src/gc570d-video.c \
	src/gc570d_everlive.c

USER_HEADERS := \
	src/gc570d.h \
	src/gc570d-user-compat.h \
	src/gc570d-shm.h \
	src/gc570d_everlive.h \
	src/gc570d_font8x8.h \
	data/gc570d_hdr_lut_3000.inc \
	data/gc570d_no_signal_640x360.inc

TARGETS := opengc570d-userd gc570d-ctl libgc570d-v4l2.so

all: $(TARGETS)

opengc570d-userd: $(USERD_SRCS) $(USER_HEADERS)
	$(CC) $(USER_CFLAGS) -o $@ $(USERD_SRCS) $(USER_LDFLAGS) $(LDFLAGS)

gc570d-ctl: src/gc570d-ctl.c
	$(CC) -O2 -g -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -o $@ $<

libgc570d-v4l2.so: src/gc570d-v4l2-preload.c src/gc570d-shm.h
	$(CC) -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-nonnull-compare \
		-std=gnu11 -D_GNU_SOURCE -shared -fPIC -Isrc -o $@ $< -ldl -lrt -pthread

clean:
	rm -f $(TARGETS)

.PHONY: all clean
