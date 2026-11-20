# SPDX-License-Identifier: GPL-2.0-only
#
# EverLive webcam bridge: everlive_camd publishes a V4L2 webcam into an
# EverLive frame store; libeverlive-cam-v4l2.so presents it as /dev/video65
# so several programs (Console Hub PiP, recorder, OBS) can use the camera at
# once.  Shares only libeverlive-core with the capture engines.

CC ?= gcc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
DESTDIR ?=
LIBDIR ?= $(PREFIX)/lib
BINDIR ?= $(PREFIX)/bin

CORE_DIR      := ../libeverlive-core
CORE_LIB      := $(CORE_DIR)/libeverlive_core.a
CORE_V4L2_DIR := $(CORE_DIR)/v4l2

CFLAGS_BASE := $(CFLAGS) -O2 -g -Wall -Wextra -Wno-unused-parameter \
	-Wno-sign-compare -std=gnu11 -D_GNU_SOURCE -I$(CORE_DIR)

# MJPEG decoding is optional (YUYV and NV12 cameras work without it).
AV_LIBS := $(shell $(PKG_CONFIG) --libs libavcodec libavutil libswscale 2>/dev/null)
AV_CFLAGS := $(shell $(PKG_CONFIG) --cflags libavcodec libavutil libswscale 2>/dev/null)
ifneq ($(strip $(AV_LIBS)),)
CFLAGS_BASE += -DHAVE_LIBAV $(AV_CFLAGS)
endif

TARGETS := everlive_camd libeverlive-cam-v4l2.so

all: $(TARGETS)

everlive_camd: src/everlive_camd.c $(CORE_DIR)/everlive_framestore.h $(CORE_LIB)
	$(CC) $(CFLAGS_BASE) -o $@ src/everlive_camd.c $(CORE_LIB) $(AV_LIBS) -lrt -pthread

# -Bsymbolic: each preload DSO binds its own descriptor and state, so it can
# be stacked with a capture card's DSO in one process (PiP recording).
libeverlive-cam-v4l2.so: src/everlive-cam-v4l2.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_card.h \
		$(CORE_V4L2_DIR)/everlive_v4l2_state.h \
		$(CORE_V4L2_DIR)/everlive_v4l2_state.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_preload.c \
		$(CORE_DIR)/everlive_framestore.h \
		$(CORE_LIB)
	$(CC) -O2 -g -Wall -Wextra -Wno-unused-parameter \
		-std=gnu11 -D_GNU_SOURCE -shared -fPIC -Wl,-Bsymbolic \
		-Isrc -I$(CORE_DIR) -I$(CORE_V4L2_DIR) \
		-o $@ src/everlive-cam-v4l2.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_state.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_preload.c \
		$(CORE_LIB) -ldl -lrt -pthread
	@chmod 0644 $@

$(CORE_LIB):
	$(MAKE) -C $(CORE_DIR)

install: all
	install -Dm0755 everlive_camd $(DESTDIR)$(BINDIR)/everlive_camd
	install -Dm0644 libeverlive-cam-v4l2.so $(DESTDIR)$(LIBDIR)/libeverlive-cam-v4l2.so

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/everlive_camd $(DESTDIR)$(LIBDIR)/libeverlive-cam-v4l2.so

clean:
	rm -f $(TARGETS)

.PHONY: all install uninstall clean
