# Changelog

All notable changes to the **EverLive** project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.1.0] - 2026-10-25

### Added
- **Compressed Surround Bitstream Decoding (AC-3 5.1 & DTS 5.1):**
  - Live, real-time IEC 61937 bitstream parser and unpacker for compressed surround audio across 16-bit and 24-bit HDMI audio carriers.
  - Automatic alignment detection (`MSB` vs `LSB`) for 24-bit PCM carriers.
  - Hardware-accelerated decoding via `libavcodec` (`AV_CODEC_ID_AC3` and `AV_CODEC_ID_DTS`).
  - Studio-grade stereo downmixing via `libswresample` with ITU-R BS.775 compliance.
  - High-durability error concealment and sync recovery for continuous play across mode transitions.
- **Dynamic Bitstream EDID Profile:**
  - Added new `--edid bitstream` profile to `gc573_user_streamer` advertising AC-3 and DTS 5.1 Short Audio Descriptors (SADs) in CTA-861 Extension blocks.
  - Added "Bitstream (Dolby Digital / DTS 5.1)" directly to the Console Hub GUI dropdown.
- **Triple Buffering (3-Buffer Ring) for High-Resolution Capture:**
  - Enabled true triple buffering (3 DMA buffers, 2 active hardware DMA channels) for 1440p and 4K signals, eliminating dropped frames and presentation micro-stutters under CPU scheduling jitter.
- **Legacy Kernel Module Blacklisting:**
  - Added `/etc/modprobe.d/blacklist-everlive-legacy.conf` to `install.sh` to prevent old out-of-tree `open_gc573.ko` modules from racing with `vfio-pci` at early boot.
  - Automatic removal during `uninstall.sh`.
- **IOMMU Documentation in README:**
  - Added detailed kernel boot parameter documentation (`intel_iommu=on iommu=pt` / `amd_iommu=on iommu=pt`) and diagnosis for VFIO Error -22.
- **Audio Transition Silence:**
  - Purged transitional samples via `SDL_ClearQueuedAudio()` on HDMI format changes to ensure silent mode transitions between PCM and Bitstream.
  - Extended LPCM confirmation window to 800 ms (40 blocks) to prevent false unmuting during console audio renegotiations.
  - Relaxed SDL audio queue cushion to 200 ms to absorb display compositor fluctuations.

---

## [1.0.1] - 2026-10-25

### Added
- **Live Stats OSD Overlay:**
  - Real-time in-game on-screen display (OSD) showing input resolution, refresh rate, delivered FPS, aspect ratio, and audio stream state.
  - Toggled via `Tab` or `'o'`, cycle position via `'p'`.
- **Aspect Ratio Control:**
  - Keyboard toggle `'a'` and CLI flag `--aspect auto|16:9|4:3|stretch`.
  - Native 2x vertical stretch for RetroTINK 4K anamorphic 3840x1080 half-vertical 4K120 signals.
- **Console Hub GUI Controls:**
  - Checkboxes for Fullscreen, VSync, and Streamer Mode.
  - Direct preset selection and persistent settings.
- **Audio Handshake Recovery:**
  - Non-blocking asynchronous audio status probing to survive game-to-dashboard mode transitions.

---

## [1.0.0] - 2026-10-25

### Initial Release - "The Userspace Era"
- **100% Blob-Free Userspace VFIO Engine:**
  - Zero out-of-tree kernel modules, zero DKMS rebuilds.
  - Direct Ring-3 MMIO and DMA control of AVerMedia Live Gamer 4K (GC573) via standard Linux `vfio-pci`.
  - 100% Secure Boot and Anti-Cheat (EAC v2) compatible.
- **Hardware-Verified Video & Audio:**
  - 1440p @ 120 FPS native zero-latency capture.
  - 4K @ 60 FPS HDR10 passthrough and capture.
  - Synchronous 48 kHz L-PCM audio DMA direct to SDL2 / PipeWire.
- **Hardware Control:**
  - Complete userspace initialization and calibration for ITE IT6805 (HDMI 2.0 receiver) and IT6664 (HDMI 2.0 splitter).
  - Programmable on-board RGB LED controller.
  - 1-Click GUI launcher and seamless driver binding.
