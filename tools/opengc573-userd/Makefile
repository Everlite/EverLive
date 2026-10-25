# SPDX-License-Identifier: GPL-2.0-only
# Makefile for OpenGC573 Userspace Driver Tools & EverLive User Daemon

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -pthread -D_GNU_SOURCE
LDFLAGS ?= -pthread
SDL_FLAGS = $(shell pkg-config --cflags --libs sdl2)
# libavcodec/libswresample are only needed by the streamer's bitstream decoder.
AV_CFLAGS = $(shell pkg-config --cflags libavcodec libavutil libswresample)
AV_LIBS = $(shell pkg-config --libs libavcodec libavutil libswresample)

COMMON_OBJS = gc573_vfio.o gc573_user_i2c.o gc573_user_hw.o gc573_user_edid.o gc573_user_it6805.o gc573_user_it6664.o
STREAM_OBJS = gc573_user_fpga.o gc573_user_video.o gc573_user_audio.o gc573_user_bitstream.o
TARGETS = gc573_user_probe gc573_user_streamer everlive_userd gc573_user_diag

all: $(TARGETS)

gc573_user_probe: gc573_user_probe.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

gc573_user_streamer: gc573_user_streamer.o $(COMMON_OBJS) $(STREAM_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(SDL_FLAGS) $(AV_LIBS)

everlive_userd: everlive_userd.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

gc573_user_diag: gc573_user_diag.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c gc573-user-compat.h gc573_vfio.h gc573_user_i2c.h gc573_user_hw.h gc573_user_edid.h gc573_user_it6805.h gc573_user_it6664.h gc573_user_fpga.h gc573_user_video.h gc573_user_audio.h gc573_user_bitstream.h
	$(CC) $(CFLAGS) $(SDL_FLAGS) $(AV_CFLAGS) -c -o $@ $<

clean:
	rm -f *.o $(TARGETS)

.PHONY: all clean
