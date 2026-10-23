# SPDX-License-Identifier: GPL-2.0-only
# Makefile for OpenGC573 Userspace Driver Tools

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -pthread -D_GNU_SOURCE
LDFLAGS ?= -pthread

TARGETS = gc573_user_probe

all: $(TARGETS)

gc573_user_probe: gc573_user_probe.o gc573_vfio.o gc573_user_i2c.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c gc573-user-compat.h gc573_vfio.h gc573_user_i2c.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o $(TARGETS)

.PHONY: all clean
