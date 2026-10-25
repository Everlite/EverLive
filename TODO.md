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

## 🚀 Planned for v1.1+ (Detailed Specification in `docs/ROADMAP_v1.1.md`)

A comprehensive engineering specification with data-flow diagrams and implementation steps is documented in [docs/ROADMAP_v1.1.md](docs/ROADMAP_v1.1.md).

### 1. Zero-DKMS Userspace V4L2 Interception (`libeverlive-v4l2.so` via `LD_PRELOAD`)
- **Concept:** Inspired by @PolygonPrincess (`libgc570d-v4l2.so`).
- **Goal:** Intercept V4L2 libc calls (`open`, `ioctl`, `mmap`) in userspace to stream raw frames directly from `everlive_userd`'s POSIX shared memory ring buffer.
- **Advantage:** Enables native webcam/capture support in Discord, Chromium, and OBS without running a single out-of-tree kernel module or installing `v4l2loopback`.

### 2. RetroTINK 4K Pro & Custom Modelines
- **Anamorphic 2x Vertical Scaling:** Automatic 2x vertical stretch for `3840x1080` RetroTINK 120Hz/BFI signals back to 16:9 4K.
- **Sync Lock OSD Toast:** Subtle 3-second on-screen status toast upon signal lock and mode changes.
- **Audio Controls:** Direct software mute (<kbd>m</kbd>) and gain adjustments in the C streamer.

### 3. GUI Polish & Multi-Card Core Abstraction (`libeverlive-core`)
- **RGB Color Picker:** Full 24-bit hex color wheel via `QColorDialog` in `everlive_viewer.py`.
- **Modular Hardware Abstraction:** Decouple PCIe DMA streaming engine from card-specific register tables (laying the groundwork for GC570D Live Gamer DUO and GC575 4K 2.1).
