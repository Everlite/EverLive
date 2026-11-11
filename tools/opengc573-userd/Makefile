# SPDX-License-Identifier: GPL-2.0-only
#
# EverLive GC573 (Live Gamer 4K) userspace VFIO engine.
#
# Sources live in src/.  Binaries stay in this directory: play.sh, install.sh,
# uninstall.sh and the systemd units resolve them by path.

CC ?= gcc
PKG_CONFIG ?= pkg-config
DEPFLAGS := -MMD -MP

# Flags every translation unit needs.
BASE_CFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
	-std=gnu11 -D_GNU_SOURCE
BASE_CFLAGS += -Isrc -I../libeverlive-core
LDFLAGS ?= -pthread

SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell $(PKG_CONFIG) --libs sdl2 2>/dev/null)
AV_CFLAGS  := $(shell $(PKG_CONFIG) --cflags libavcodec libavutil libswresample libswscale 2>/dev/null)
AV_LIBS    := $(shell $(PKG_CONFIG) --libs libavcodec libavutil libswresample libswscale 2>/dev/null)
PW_CFLAGS  := $(shell $(PKG_CONFIG) --cflags libpipewire-0.3 2>/dev/null)
PW_LIBS    := $(shell $(PKG_CONFIG) --libs libpipewire-0.3 2>/dev/null)

CORE_LIB := ../libeverlive-core/libeverlive_core.a

# Hardware / register layer: no SDL, no FFmpeg, no PipeWire.
HW_OBJS := \
	src/gc573_vfio.o \
	src/gc573_user_i2c.o \
	src/gc573_user_hw.o \
	src/gc573_user_edid.o \
	src/gc573_user_it6805.o \
	src/gc573_user_it6664.o \
	src/gc573_user_rx_eq.o

# Capture / presentation pipeline.
STREAM_OBJS := \
	src/gc573_user_log.o \
	src/gc573_user_fpga.o \
	src/gc573_user_video.o \
	src/gc573_user_audio.o \
	src/gc573_user_bitstream.o \
	src/gc573_user_nosignal.o \
	src/gc573_user_ipc.o \
	src/gc573_user_audio_tap.o \
	src/gc573_user_hdr.o \
	src/gc573_user_framegen.o

ENTRY_OBJS := \
	src/gc573_user_probe.o \
	src/gc573_user_streamer.o \
	src/everlive_userd.o \
	src/gc573_user_diag.o

TARGETS := gc573_user_probe gc573_user_streamer everlive_userd gc573_user_diag

all: $(TARGETS)

gc573_user_probe: src/gc573_user_probe.o $(HW_OBJS) $(CORE_LIB)
	$(CC) $(BASE_CFLAGS) -o $@ $^ $(LDFLAGS)

gc573_user_streamer: src/gc573_user_streamer.o $(HW_OBJS) $(STREAM_OBJS) $(CORE_LIB)
	$(CC) $(BASE_CFLAGS) -o $@ $^ $(LDFLAGS) $(SDL_LIBS) $(AV_LIBS) $(PW_LIBS) -lm

everlive_userd: src/everlive_userd.o $(HW_OBJS) $(CORE_LIB)
	$(CC) $(BASE_CFLAGS) -o $@ $^ $(LDFLAGS)

gc573_user_diag: src/gc573_user_diag.o $(HW_OBJS) $(CORE_LIB)
	$(CC) $(BASE_CFLAGS) -o $@ $^ $(LDFLAGS)

$(CORE_LIB):
	$(MAKE) -C ../libeverlive-core

# Headless/hardware TUs: base flags only.
$(HW_OBJS) src/gc573_user_probe.o src/gc573_user_diag.o src/everlive_userd.o: \
		src/%.o: src/%.c
	$(CC) $(BASE_CFLAGS) $(DEPFLAGS) -c -o $@ $<

# Capture pipeline: FFmpeg libav* and PipeWire headers.
$(STREAM_OBJS): src/%.o: src/%.c
	$(CC) $(BASE_CFLAGS) $(AV_CFLAGS) $(PW_CFLAGS) $(DEPFLAGS) -c -o $@ $<

# Only the SDL2 renderer needs SDL headers.
src/gc573_user_streamer.o: src/gc573_user_streamer.c
	$(CC) $(BASE_CFLAGS) $(SDL_CFLAGS) $(AV_CFLAGS) $(PW_CFLAGS) $(DEPFLAGS) -c -o $@ $<

# Headless framegen self-test.  Task 4 moves this fully into tests/.
gc573_framegen_selftest: src/gc573_user_framegen.o ../../tests/test_framegen.c
	$(CC) $(BASE_CFLAGS) -o $@ ../../tests/test_framegen.c \
		src/gc573_user_framegen.o $(LDFLAGS) -lm

selftest: gc573_framegen_selftest
	./gc573_framegen_selftest

test: all
	$(MAKE) -C ../../tests test

-include $(HW_OBJS:.o=.d) $(STREAM_OBJS:.o=.d) $(ENTRY_OBJS:.o=.d)

clean:
	rm -f src/*.o src/*.d $(TARGETS) gc573_framegen_selftest

.PHONY: all test selftest clean
