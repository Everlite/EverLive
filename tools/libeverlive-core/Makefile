# SPDX-License-Identifier: GPL-2.0-only
CC ?= gcc
AR ?= ar
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -D_GNU_SOURCE -fPIC

TARGET_STATIC := libeverlive_core.a
TARGET_SHARED := libeverlive_core.so
TARGET_CLI    := everlive-ctl

OBJS := everlive_card.o everlive_frame.o

all: $(TARGET_STATIC) $(TARGET_SHARED) $(TARGET_CLI)

$(TARGET_STATIC): $(OBJS)
	$(AR) rcs $@ $^

$(TARGET_SHARED): $(OBJS)
	$(CC) -shared -o $@ $^

$(TARGET_CLI): everlive_ctl.o $(TARGET_STATIC)
	$(CC) $(CFLAGS) -o $@ everlive_ctl.o $(TARGET_STATIC)

%.o: %.c everlive_card.h everlive_frame.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) everlive_ctl.o $(TARGET_STATIC) $(TARGET_SHARED) $(TARGET_CLI)

.PHONY: all clean
