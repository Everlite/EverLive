# EverLive - Future Roadmap & Development Plan (TODO)

This document tracks completed milestones, upcoming architectural goals, and research items for future releases of EverLive.

---

## 🎯 Completed in v1.0.0 ("The Userspace Era")

### ✅ 100% Blob-Free Userspace VFIO Driver (Ring 3)
- **Initial Inspiration:** @PolygonPrincess (OpenGC570D)
- **Status:** **Fully implemented & hardware-verified in v1.0.0!**
- Completely eliminated out-of-tree kernel modules and DKMS build failures across kernel updates.
- 100% compatible with UEFI Secure Boot (operates on standard upstream `vfio-pci`).
- Low-latency hardware DMA for video (PS5 1440p120 & native 4K60) and L-PCM 48kHz stereo audio (SDL2 / PipeWire).
- Hardware EDID profile injection (4K60, 1440p120, custom `.bin` via I2C).
- FPGA 24-bit RGB hardware lighting control.
- Standby daemon handoff (`everlive_userd`) without PCIe Function Level Reset (FLR).

---

## ✅ Completed in v1.0.1

- Live stats OSD overlay (`o` / `Tab` / `i`), position cycling (`p`, default Top-Right) and aspect-ratio override (`a`, `--aspect`).
- Audio handshake recovery across console mode changes.

---

## ✅ Completed in v1.1.0

- **Real-time bitstream decoding:** IEC 61937 scanner/unpacker for 16-bit and 24-bit carriers, AC-3 and DTS 5.1 decoded via `libavcodec`, stereo downmix via `libswresample`.
- **Mute guard:** content-driven preamble detection including type-0x00 NULL bursts; LPCM only resumes after a confirmation window, so compressed data can never reach the speakers.
- **Frame pacing:** resolution-dependent DMA ring depth (6/5/4/3 buffers), dedicated audio worker thread with 60 ms cushion.
- **Interrupt correctness:** channels retire only on `GC573_DMA_IRQ_COMPLETE`, shared IRQ lock between video and audio threads (no more phantom frames or inflated FPS).
- **Timing:** blocking 10-second rate probe removed, initial frame-rate detection stabilised for 59.72/59.94 Hz scalers.
- **Desktop integration:** PipeWire app identity and window icon (`EverLive`).

---

## ✅ Completed in v1.2.0

- **Native virtual camera:** raw YUYV output to `/dev/video10` ("EverLive Capture") via `v4l2loopback` (`--v4l2` / `--loopback`), including automatic format renegotiation when the input resolution changes while OBS holds buffers.
- **Headless Studio mode:** `--headless` captures and streams without opening an SDL window; selectable from the Console Hub.
- **Seamless mode handover:** Gaming Screen ⇄ Studio switching without a manual stop.
- **Universal EDID audio:** every built-in profile advertises L-PCM + AC-3 + DTS 5.1; `bitstream` is now a legacy alias for `default`.
- **Console Hub modernization:** aspect-ratio override, audio-format status, virtual-camera checkbox, removed obsolete render-engine selector.
- **Installer integration:** `install.sh` installs `v4l2loopback` where packaged and preconfigures `/dev/video10` (`"EverLive Capture"`, `exclusive_caps=0`) via `/etc/modprobe.d` + `/etc/modules-load.d`; `uninstall.sh` removes both files again.
- **Hardware verification (PS5):** 1440p120 with the OBS virtual camera, the Gaming $\leftrightarrow$ Studio handover, and Dolby Digital / DTS decoding.

---

## 🚀 Planned for v1.3+ (Detailed Specification in `docs/ROADMAP_v1.1.md`)

> `docs/ROADMAP_v1.1.md` is a historical engineering document. Its `LD_PRELOAD`
> shim was **not** implemented; v1.2.0 solved virtual-camera support with the
> `v4l2loopback` sink instead. The shim remains an optional zero-dependency idea.

### 1. Zero-Dependency V4L2 Interception (`libeverlive-v4l2.so` via `LD_PRELOAD`)
- **Concept:** Inspired by @PolygonPrincess (`libgc570d-v4l2.so`).
- **Goal:** Intercept V4L2 libc calls (`open`, `ioctl`, `mmap`) in userspace to expose capture frames without the `v4l2loopback` kernel module.
- **Advantage:** Native webcam/capture support in Discord, Chromium, and OBS with no additional kernel module at all.

### 2. Installer & Distribution Polish
- Optional Flatpak/AppImage packaging for the Console Hub.
- Migration helper that unloads a stale `v4l2loopback` instance loaded with `exclusive_caps=1` (currently only reported by `play.sh`).

### 3. RetroTINK 4K Pro & Custom Modelines
- **Sync Lock OSD Toast:** Subtle 3-second status toast upon signal lock and mode changes (the stats overlay already shows the live state).
- **Custom Modeline Helper:** Import/validation UI for scaler timings beyond the built-in presets.

### 4. GUI Polish & Multi-Card Core Abstraction (`libeverlive-core`)
- **RGB Color Picker:** Full 24-bit hex color wheel via `QColorDialog` in `everlive_viewer.py`.
- **Modular Hardware Abstraction:** Decouple the PCIe DMA streaming engine from card-specific register tables (laying the groundwork for GC570D Live Gamer DUO and GC575 4K 2.1).

### 5. Audio Subsystem
- **Multichannel Output:** Optional native 5.1/7.1 playback instead of the stereo downmix (today AC-3/DTS are decoded and downmixed via `libswresample`).
- **Streamer-Side Volume/Mute:** The Console Hub controls volume and mute via PipeWire; a keyboard shortcut inside the SDL window could mirror this.
