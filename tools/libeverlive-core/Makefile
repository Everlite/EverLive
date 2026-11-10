# SPDX-License-Identifier: GPL-2.0-only
CC ?= gcc
AR ?= ar
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -D_GNU_SOURCE -fPIC

SOVERSION   := 0
VERSION     := 0.1.0
SONAME      := libeverlive_core.so.$(SOVERSION)
SHARED_REAL := libeverlive_core.so.$(VERSION)

TARGET_STATIC := libeverlive_core.a
TARGET_SHARED := libeverlive_core.so
TARGET_CLI    := everlive-ctl

PREFIX           ?= /usr/local
DESTDIR          ?=
BINDIR           ?= $(PREFIX)/bin
LIBDIR           ?= $(PREFIX)/lib
INCLUDEDIR       ?= $(PREFIX)/include
SYSTEMD_USER_DIR ?= $(PREFIX)/lib/systemd/user

OBJS     := everlive_card.o everlive_frame.o
CTL_OBJS := everlive_ctl.o everlive_ctl_proto.o

all: $(TARGET_STATIC) $(TARGET_SHARED) $(TARGET_CLI)

$(TARGET_STATIC): $(OBJS)
	$(AR) rcs $@ $^

$(SHARED_REAL): $(OBJS)
	$(CC) -shared -Wl,-soname,$(SONAME) -o $@ $^

$(SONAME): $(SHARED_REAL)
	ln -sf $(SHARED_REAL) $@

$(TARGET_SHARED): $(SONAME)
	ln -sf $(SONAME) $@

$(TARGET_CLI): $(CTL_OBJS) $(TARGET_STATIC)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c everlive_card.h everlive_frame.h everlive_ctl.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(LIBDIR) \
		$(DESTDIR)$(INCLUDEDIR) $(DESTDIR)$(SYSTEMD_USER_DIR)
	install -m 0755 $(TARGET_CLI) $(DESTDIR)$(BINDIR)/$(TARGET_CLI)
	install -m 0755 everlive-service.sh $(DESTDIR)$(BINDIR)/everlive-service
	install -m 0644 $(SHARED_REAL) $(DESTDIR)$(LIBDIR)/$(SHARED_REAL)
	ln -sf $(SHARED_REAL) $(DESTDIR)$(LIBDIR)/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(LIBDIR)/$(TARGET_SHARED)
	install -m 0644 $(TARGET_STATIC) $(DESTDIR)$(LIBDIR)/$(TARGET_STATIC)
	install -m 0644 everlive_card.h everlive_frame.h everlive_ctl.h \
		$(DESTDIR)$(INCLUDEDIR)/
	install -m 0644 everlive.service \
		$(DESTDIR)$(SYSTEMD_USER_DIR)/everlive.service

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET_CLI) \
		$(DESTDIR)$(BINDIR)/everlive-service \
		$(DESTDIR)$(LIBDIR)/$(SHARED_REAL) \
		$(DESTDIR)$(LIBDIR)/$(SONAME) \
		$(DESTDIR)$(LIBDIR)/$(TARGET_SHARED) \
		$(DESTDIR)$(LIBDIR)/$(TARGET_STATIC) \
		$(DESTDIR)$(INCLUDEDIR)/everlive_card.h \
		$(DESTDIR)$(INCLUDEDIR)/everlive_frame.h \
		$(DESTDIR)$(INCLUDEDIR)/everlive_ctl.h \
		$(DESTDIR)$(SYSTEMD_USER_DIR)/everlive.service

clean:
	rm -f $(OBJS) $(CTL_OBJS) $(TARGET_STATIC) $(TARGET_SHARED) \
		$(SONAME) $(SHARED_REAL) $(TARGET_CLI)

.PHONY: all clean install uninstall
