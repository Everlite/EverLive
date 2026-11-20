# SPDX-License-Identifier: GPL-2.0-only
#
# EverLive — top-level orchestration and system-wide installation.
#
#   make                                        # build core, engines, and CLI
#   make hwsuhd                                 # HWS UHD engine (opt-in, see
#                                               # tools/openhws-userd/README.md)
#   make test                                   # run all unit tests
#   sudo make install                           # system-wide install (PREFIX=/usr/local)
#   make install DESTDIR="$pkgdir" PREFIX=/usr  # distro packaging
#   sudo make uninstall                         # remove system-wide install

PYTHON          ?= python3
PREFIX          ?= /usr/local
DESTDIR         ?=
BINDIR          ?= $(PREFIX)/bin
DATADIR         ?= $(PREFIX)/share
LIBDIR          ?= $(PREFIX)/lib
INCLUDEDIR      ?= $(PREFIX)/include
EVERLIVE_LIBDIR ?= $(LIBDIR)/everlive
DOCDIR          ?= $(DATADIR)/doc/everlive

# System directories (systemd, udev, kmod, pipewire, limits)
# These are always read from /usr and /etc by OS daemons regardless of PREFIX.
SYSLIBDIR       ?= /usr/lib
SYSDATADIR      ?= /usr/share
SYSCONFDIR      ?= /etc

CORE_DIR   := tools/libeverlive-core
GC573_DIR  := tools/opengc573-userd
GC570D_DIR := tools/opengc570d-userd
HWSUHD_DIR := tools/openhws-userd
CAM_DIR    := tools/everlive-cam
AFMF_DIR   := tools/everlive-fg-afmf
VIEWER_DIR := tools/everlive
TESTS_DIR  := tests
PKG_DIR    := packaging

GC573_BINS  := gc573_user_streamer everlive_userd gc573_user_probe gc573_user_diag
GC570D_BINS := opengc570d-userd gc570d-ctl
ICON_SIZES  := 16 32 48 64 128 256 512

# Python modules imported by everlive_viewer.py.
VIEWER_PY := everlive_client.py card_profile.py service_manager.py \
             app_integrations.py everlive_recorder.py gc553g2_control.py \
             ui_theme.py ui_widgets.py fa_icons.py everlive_multi.py \
             everlive_cam.py
VIEWER_ASSETS := fonts/fa-solid-900.ttf fonts/fa-brands-400.ttf \
                 fonts/LICENSE.txt fonts/chevron-down.png

GC570D_RUNDIR := $(EVERLIVE_LIBDIR)/$(GC570D_DIR)
GC570D_SED := sed -e 's|/usr/local/libexec/|$(GC570D_RUNDIR)/|g' \
		  -e 's|/usr/local/bin/|$(GC570D_RUNDIR)/|g'

L := $(DESTDIR)$(EVERLIVE_LIBDIR)

.PHONY: all core gc573 gc570d hwsuhd cam afmf test check asan clean install uninstall

all: core gc573 gc570d cam afmf

core:
	+$(MAKE) -C $(CORE_DIR)

gc573: core
	+$(MAKE) -C $(GC573_DIR)

gc570d: core
	+$(MAKE) -C $(GC570D_DIR)

# Webcam bridge (everlive_camd + libeverlive-cam-v4l2.so) for the Hub's
# picture-in-picture overlay.
cam: core
	+$(MAKE) -C $(CAM_DIR)

# AMD Fluid Motion Frames (AFMF) FidelityFX Optical Flow Vulkan Layer
afmf:
	@if command -v cmake >/dev/null 2>&1 && (command -v glslang >/dev/null 2>&1 || command -v glslangValidator >/dev/null 2>&1); then \
		echo "=== Building AFMF GPU Frame Generation (FidelityFX Vulkan Layer) ==="; \
		mkdir -p $(AFMF_DIR)/build && \
		(cd $(AFMF_DIR) && cmake -B build -GNinja -DCMAKE_BUILD_TYPE=Release && ninja -C build); \
	else \
		echo "[-] cmake or glslang not found; skipping AFMF layer build."; \
	fi

# Not part of `all`: the HWS UHD engine is built and installed on request.
hwsuhd: core
	+$(MAKE) -C $(HWSUHD_DIR)

test: all
	+$(MAKE) -C $(TESTS_DIR) test
	$(PYTHON) -m unittest discover -s tools/everlive/tests

check: test

asan: all
	+$(MAKE) -C $(TESTS_DIR) asan

clean:
	+$(MAKE) -C $(CORE_DIR) clean
	+$(MAKE) -C $(GC573_DIR) clean
	+$(MAKE) -C $(GC570D_DIR) clean
	+$(MAKE) -C $(HWSUHD_DIR) clean
	+$(MAKE) -C $(CAM_DIR) clean
	+$(MAKE) -C $(TESTS_DIR) clean
	rm -rf $(AFMF_DIR)/build

install: all
	# 1. C library, headers, engine binaries and CLI tools
	+$(MAKE) -C $(CORE_DIR) install PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)
	+$(MAKE) -C $(GC573_DIR) install PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)
	+$(MAKE) -C $(GC570D_DIR) install-bin PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)
	install -Dm0644 $(CAM_DIR)/libeverlive-cam-v4l2.so $(DESTDIR)$(LIBDIR)/libeverlive-cam-v4l2.so
	# 2. Application runtime tree under $(EVERLIVE_LIBDIR)
	install -Dm0755 play.sh $(L)/play.sh
	install -Dm0755 $(VIEWER_DIR)/everlive_viewer.py $(L)/$(VIEWER_DIR)/everlive_viewer.py
	install -m0644 $(addprefix $(VIEWER_DIR)/,$(VIEWER_PY)) $(L)/$(VIEWER_DIR)/
	install -d $(L)/$(VIEWER_DIR)/fonts
	install -m0644 $(addprefix $(VIEWER_DIR)/,$(VIEWER_ASSETS)) $(L)/$(VIEWER_DIR)/fonts/
	install -Dm0644 $(VIEWER_DIR)/everlive.png $(L)/$(VIEWER_DIR)/everlive.png
	install -d $(L)/$(GC573_DIR) $(L)/$(GC570D_DIR) $(L)/$(CORE_DIR)
	install -m0755 $(addprefix $(GC573_DIR)/,$(GC573_BINS)) $(GC573_DIR)/vfio_pci_bind.sh $(L)/$(GC573_DIR)/
	install -m0755 $(addprefix $(GC570D_DIR)/,$(GC570D_BINS)) $(GC570D_DIR)/gc570d-vfio-bind.sh $(L)/$(GC570D_DIR)/
	install -m0755 $(CORE_DIR)/everlive-ctl $(CORE_DIR)/everlive-service.sh $(L)/$(CORE_DIR)/
	install -Dm0755 $(CAM_DIR)/everlive_camd $(L)/$(CAM_DIR)/everlive_camd
	# AFMF GPU Frame Generation layer
	if [ -f $(AFMF_DIR)/build/layer/libafmf-linux.so ]; then \
		install -d $(L)/afmf; \
		install -m0755 $(AFMF_DIR)/build/layer/libafmf-linux.so $(L)/afmf/libafmf-linux.so; \
		install -m0644 $(AFMF_DIR)/build/layer/afmf-linux.json $(L)/afmf/afmf-linux.json; \
	fi
	$(GC570D_SED) $(GC570D_DIR)/gc570d-sleep.sh > $(L)/$(GC570D_DIR)/gc570d-sleep.sh
	chmod 0755 $(L)/$(GC570D_DIR)/gc570d-sleep.sh
	# 3. Main launcher, desktop entry and icons
	install -d $(DESTDIR)$(BINDIR)
	sed 's|@EVERLIVE_LIBDIR@|$(EVERLIVE_LIBDIR)|g' $(PKG_DIR)/everlive.in > $(DESTDIR)$(BINDIR)/everlive
	chmod 0755 $(DESTDIR)$(BINDIR)/everlive
	install -Dm0644 $(PKG_DIR)/everlive.desktop $(DESTDIR)$(DATADIR)/applications/everlive.desktop
	for sz in $(ICON_SIZES); do \
		install -Dm0644 $(VIEWER_DIR)/icons/$${sz}x$${sz}/apps/everlive.png \
			$(DESTDIR)$(DATADIR)/icons/hicolor/$${sz}x$${sz}/apps/everlive.png; \
	done
	# 4. VFIO binding, udev rules, modprobe and memlock limits
	install -Dm0644 $(PKG_DIR)/70-everlive.rules $(DESTDIR)$(SYSLIBDIR)/udev/rules.d/70-everlive.rules
	install -d $(DESTDIR)$(SYSLIBDIR)/udev/rules.d
	$(GC570D_SED) $(GC570D_DIR)/99-gc570d-vfio.rules > $(DESTDIR)$(SYSLIBDIR)/udev/rules.d/99-gc570d-vfio.rules
	chmod 0644 $(DESTDIR)$(SYSLIBDIR)/udev/rules.d/99-gc570d-vfio.rules
	install -Dm0644 $(PKG_DIR)/everlive-vfio.modprobe.conf $(DESTDIR)$(SYSLIBDIR)/modprobe.d/everlive-vfio.conf
	install -Dm0644 $(PKG_DIR)/everlive-v4l2loopback.modprobe.conf $(DESTDIR)$(SYSLIBDIR)/modprobe.d/everlive-v4l2loopback.conf
	install -Dm0644 $(PKG_DIR)/everlive-vfio.modules-load.conf $(DESTDIR)$(SYSLIBDIR)/modules-load.d/everlive-vfio.conf
	install -Dm0644 $(PKG_DIR)/99-everlive-memlock.conf $(DESTDIR)$(SYSCONFDIR)/security/limits.d/99-everlive-memlock.conf
	# 5. GC570D resume hook and per-user service
	install -d $(DESTDIR)$(SYSLIBDIR)/systemd/system-sleep $(DESTDIR)$(SYSLIBDIR)/systemd/user
	ln -sf $(GC570D_RUNDIR)/gc570d-sleep.sh $(DESTDIR)$(SYSLIBDIR)/systemd/system-sleep/gc570d-sleep.sh
	$(GC570D_SED) $(GC570D_DIR)/opengc570d-userd.service > $(DESTDIR)$(SYSLIBDIR)/systemd/user/opengc570d-userd.service
	chmod 0644 $(DESTDIR)$(SYSLIBDIR)/systemd/user/opengc570d-userd.service
	# 6. PipeWire dedicated capture sink (EverLive Capture Audio)
	install -Dm0644 $(PKG_DIR)/50-everlive-capture.conf $(DESTDIR)$(SYSDATADIR)/pipewire/pipewire.conf.d/50-everlive-capture.conf
	# 7. Documentation
	install -Dm0644 README.md $(DESTDIR)$(DOCDIR)/README.md
	install -Dm0644 CHANGELOG.md $(DESTDIR)$(DOCDIR)/CHANGELOG.md
	install -Dm0644 $(GC570D_DIR)/gc570d.conf.example $(DESTDIR)$(DOCDIR)/gc570d.conf.example

uninstall:
	+$(MAKE) -C $(CORE_DIR) uninstall PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)
	+$(MAKE) -C $(GC573_DIR) uninstall PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)
	+$(MAKE) -C $(GC570D_DIR) uninstall-bin PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)
	rm -f $(DESTDIR)$(LIBDIR)/libeverlive-cam-v4l2.so
	rm -f $(DESTDIR)$(LIBDIR)/libgc570d-v4l2.so \
	      $(DESTDIR)$(LIBDIR)/libgc573-v4l2.so \
	      $(DESTDIR)$(LIBDIR)/libeverlive-vk-pacer.so
	rm -rf $(L)
	rm -f $(DESTDIR)$(BINDIR)/everlive
	rm -f $(DESTDIR)$(DATADIR)/applications/everlive.desktop
	for sz in $(ICON_SIZES); do \
		rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/$${sz}x$${sz}/apps/everlive.png; \
	done
	rm -f $(DESTDIR)$(SYSLIBDIR)/udev/rules.d/70-everlive.rules
	rm -f $(DESTDIR)$(SYSLIBDIR)/udev/rules.d/99-gc570d-vfio.rules
	rm -f $(DESTDIR)$(SYSLIBDIR)/modprobe.d/everlive-vfio.conf
	rm -f $(DESTDIR)$(SYSLIBDIR)/modprobe.d/everlive-v4l2loopback.conf
	rm -f $(DESTDIR)$(SYSLIBDIR)/modules-load.d/everlive-vfio.conf
	rm -f $(DESTDIR)$(SYSCONFDIR)/security/limits.d/99-everlive-memlock.conf
	rm -f $(DESTDIR)$(SYSLIBDIR)/systemd/system-sleep/gc570d-sleep.sh
	rm -f $(DESTDIR)$(SYSLIBDIR)/systemd/user/opengc570d-userd.service
	rm -f $(DESTDIR)$(SYSDATADIR)/pipewire/pipewire.conf.d/50-everlive-capture.conf
	rm -rf $(DESTDIR)$(DOCDIR)
