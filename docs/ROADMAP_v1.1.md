# EverLive v1.1 Internal Engineering Roadmap ("The Zero-Friction Era")

> **Internal Working Document for EverLive Development & AI-Assisted Architecture Review**
> Target: EverLive v1.1.0
> Maintainer: @Everlite
> Collaborator / Review: DeepSeek & Community (PolygonPrincess, haxman, nepnep)
>
> **STATUS: HISTORICAL — v1.1.0 shipped.** Work Package 1 (`libeverlive-v4l2.so`)
> was **not** implemented; virtual-camera support was solved in v1.2.0 with a
> bundled `v4l2loopback` sink (`--v4l2` / `--headless`). The remaining work
> packages shipped across v1.1.0 and v1.2.0. See [`CHANGELOG.md`](../CHANGELOG.md)
> and [`TODO.md`](../TODO.md) for the current state.

---

## 🧭 Executive Summary & Core Objective

With EverLive v1.0.0, the core breakthrough was achieved: **100% blob-free, zero-DKMS PCIe capture running completely in userspace via VFIO**.

For **v1.1.0**, the objective is making EverLive the undisputed universal capture layer on Linux by solving the two biggest remaining friction points for gamers, retro enthusiasts, and streamers:
1. **Virtual Camera / Native App Compatibility without Kernel Modules:** Eliminating the need for window capture in Discord/OBS by implementing a userspace `LD_PRELOAD` V4L2 interception shim.
2. **RetroTINK 4K & Pro Scaler Specialization:** Native anamorphic scaling, dynamic OSD sync toasts, and multi-display 4K ingestion.
3. **Hardware Modularization:** Laying the architectural groundwork to support additional PCIe capture hardware (e.g., GC570D, GC575) under a unified EverLive engine.

---

## 🏗️ Work Packages & Technical Specifications

```
                     ┌──────────────────────────────────────────────┐
                     │          AVerMedia GC573 (Hardware)          │
                     └──────────────────────┬───────────────────────┘
                                            │ PCIe 2.0 x4 (VFIO)
                                            ▼
                     ┌──────────────────────────────────────────────┐
                     │          everlive_userd (Daemon)             │
                     │   - PCIe DMA Controller                      │
                     │   - Ring-Buffer Frame Dispatch               │
                     │   - Shared Memory Ring (/dev/shm/everlive)   │
                     └──────┬───────────────────────────────┬───────┘
                            │                               │
            POSIX Shared    │                               │ POSIX Shared
            Memory Buffer   │                               │ Memory Buffer
                            ▼                               ▼
    ┌───────────────────────────────┐     ┌─────────────────────────────────┐
    │     gc573_user_streamer       │     │      libeverlive-v4l2.so        │
    │   - SDL2 / OpenGL Display     │     │      (LD_PRELOAD Shim)          │
    │   - PipeWire L-PCM Audio Out  │     │   - Hooks open/ioctl/mmap       │
    │   - Anamorphic 2x Shader      │     │   - Emulates /dev/video*        │
    │   - OSD Sync Notification     │     │   - Feeds Discord, Chromium,    │
    │   - OBS PipeWire Window Cap   │     │     native OBS V4L2 Device      │
    └───────────────────────────────┘     └─────────────────────────────────┘
```

---

### Priority 1: `libeverlive-v4l2.so` — The Userspace V4L2 `LD_PRELOAD` Shim

* **Architectural Inspiration:** @PolygonPrincess (`libgc570d-v4l2.so`).
* **The Problem:** Many native Linux apps (Discord, Google Chrome, web-based conferencing, native V4L2 plugins in OBS) expect a classic `/dev/video*` V4L2 device. Forcing users to install `v4l2loopback` defeats the entire purpose of a zero-DKMS, 100% userspace stack.
* **The Solution:** A lightweight C shared library (`libeverlive-v4l2.so`) loaded via `LD_PRELOAD`:
  1. Intercepts standard libc file calls: `open()`, `open64()`, `close()`, `ioctl()`, `mmap()`, `poll()`, `select()`.
  2. When an application attempts to open a designated virtual camera node (e.g. `/dev/video99` or any requested `/dev/videoX`):
     - It redirects the read calls directly into the `everlive_userd` POSIX Shared Memory segment (`/dev/shm/everlive_frame_buffer`).
     - Implements standard V4L2 ioctl requests:
       - `VIDIOC_QUERYCAP` (Device capability flags)
       - `VIDIOC_ENUM_FMT` / `VIDIOC_G_FMT` / `VIDIOC_S_FMT` (Resolutions: 4K60, 1440p120, 1080p, YUYV / NV12 / RGB24 formats)
       - `VIDIOC_REQBUFS` / `VIDIOC_QUERYBUF` / `VIDIOC_QBUF` / `VIDIOC_DQBUF` (Buffer queue streaming)
       - `VIDIOC_STREAMON` / `VIDIOC_STREAMOFF`
  3. **Usage Flow:**
     ```bash
     # Launch Discord with native capture card camera access:
     LD_PRELOAD=/usr/local/lib/libeverlive-v4l2.so discord
     ```
  4. **Key Benefit:** Simultaneous zero-latency capture in OBS (via PipeWire) AND webcam streaming in Discord/browsers without running a single out-of-tree kernel module!

---

### Priority 2: RetroTINK 4K Pro & Custom Modelines Polish

* **Collaborator Testing:** @haxman & @nepnep
* **Features:**
  1. **Anamorphic 2x Vertical Stretch (`3840x1080` $\to$ `3840x2160`):**
     - When RetroTINK 4K runs in 120Hz BFI or specialized CRT modelines, it frequently outputs a 32:9 half-height signal (`3840x1080`).
     - Add auto-detection or a hotkey (<kbd>a</kbd>) in `gc573_user_streamer.c` to adjust the texture coordinate mapping or OpenGL viewport, smoothly scaling by 2.0x vert to restore exact 16:9 4K presentation.
     - Add `--vf-add=scale=3840:2160` automatically in the `play.sh --mpv` script whenever `3840x1080` is negotiated.
  2. **Subtle Sync Lock OSD Toast:**
     - On signal acquisition or dynamic mode hot-switch, render a sleek, semi-transparent 3-second on-screen toast in the top-right corner:
       ```
       [ EverLive ⚡ ]
       Mode: 3840x2160 @ 59.94 Hz (Sync Locked)
       Audio: L-PCM 48.0 kHz 16-Bit Stereo
       ```
     - Toggleable via hotkey (<kbd>o</kbd>) or CLI flag (`--no-osd`).
  3. **Audio Volume & Direct Mute:**
     - Add hotkey <kbd>m</kbd> for instant software mute in `gc573_user_streamer.c`.
     - Add hotkeys <kbd>+</kbd> / <kbd>-</kbd> (or mouse wheel support) for digital gain adjustment.

---

### Priority 3: GUI ("EverLive Console Hub") Polish

* **Location:** `tools/everlive/everlive_viewer.py`
* **Features:**
  1. **RGB Color Wheel / Custom Hex Input:**
     - Replace the hardcoded 8-color dropdown with an intuitive QColorDialog picker, allowing any 24-bit Hex RGB code to be written to FPGA register `0x1000` / `0x1004`.
  2. **Profile Quick Presets:**
     - Save favorite configurations (e.g. *"PS5 1440p120"*, *"RetroTINK 240p CRT"*, *"Switch 1080p60"*) into `~/.config/everlive/profiles.json`.
  3. **Status Indicator Widget:**
     - Live display of PCI link speed (Gen2 x4), temperature/FPGA clock state, and active audio buffer headroom.

---

### Priority 4: Multi-Card Architecture Preparation (`libeverlive-core`)

* **Target Hardware:** AVerMedia Live Gamer DUO (GC570D), Live Gamer 4K 2.1 (GC575), and future PCIe cards.
* **Refactoring:**
  - Decouple device-specific register offsets and I2C address definitions into distinct device definition headers:
    - `hw_gc573.h` (Current implementation: IT6805 HDMI Receiver, Artix-7 FPGA)
    - `hw_gc570d.h` (Dual ITE receivers, cross-referencing OpenGC570D findings)
  - Common hardware abstraction interface (`everlive_hw_ops`):
    - `.probe(pci_dev)`
    - `.init_hdmi(dev)`
    - `.set_edid(dev, edid_data)`
    - `.set_led(dev, r, g, b)`
    - `.start_dma(dev, ring_buf)`
    - `.stop_dma(dev)`

---

## 📋 DeepSeek Pair-Programming Prompt Template (For Tomorrow's Session)

```markdown
Hello DeepSeek! We are continuing development on EverLive (https://github.com/Everlite/EverLive).
We just shipped EverLive v1.0.0 with 100% blob-free Ring-3 userspace VFIO streaming (1440p120, 4K60, L-PCM audio DMA).

Today our mission is implementing Work Package 1 for EverLive v1.1.0 as specified in docs/ROADMAP_v1.1.md:
"Build libeverlive-v4l2.so: an LD_PRELOAD userspace library that intercepts V4L2 ioctl/open/mmap system calls and reads uncompressed frame data directly from everlive_userd's POSIX shared memory ring buffer."

Current working directory: /home/everlite/Dokumente/Projekte/Avermedia-GC573-Linux
Relevant files:
- tools/opengc573-userd/everlive_userd.c (Daemon managing PCIe VFIO and shared memory)
- tools/opengc573-userd/gc573_user_streamer.c (Current SDL2 rendering client)
- docs/ROADMAP_v1.1.md (Architecture specification)

Let's begin by reviewing the shared memory structure in everlive_userd and designing the minimalist C LD_PRELOAD shim.
```

---

*Document created: 2026-10-25 by @Everlite & Antigravity Agent*
