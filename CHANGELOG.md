# Changelog

All notable changes to the **EverLive** project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.2.1] - 2026-10-27

### Added
- **Custom EDID audio unification (#47):** Custom 256-byte `.bin` EDIDs now receive the unified L-PCM + Dolby Digital (AC-3 5.1) + DTS 5.1 Audio Data Block automatically, so consoles no longer grey out bitstream audio. Files without an audio block get one appended; when the CTA-861 extension has no spare bytes, EverLive keeps the file unmodified and reports the bypass in the log.
- **HDMI passthrough via IT6664 TX2 (#48):** the physical HDMI OUT connector is now powered, routed to the RX stream (switch register `0x0d`, port 2 = direct) and enabled. TX2 reuses TX1's measured pixel clock (its own counter only sees noise before the port is routed) and applies the port-2 AFE clamp, so the TX2 video PLL locks. Capture on TX1 is unaffected when no passthrough sink is connected or the passthrough path fails.
  - **TX2 transmits continuously, independent of the downstream HPD:** several monitors only assert HPD after they see a TMDS clock, so gating the unblank on HPD deadlocked the link (monitor showed "no signal" and fell back to DisplayPort while TX2 stayed muted at `path=0x00`). The output stage (`0x18`, `0x85`, `0x1a`, SCDC, `0xc1`/`0x19`/`0x88`) is now applied as soon as the RX locks and stays live across monitor input switches; a sink HPD edge only re-applies it. The port is never power-cycled while live, so the video PLL/AFE is not reset.
  - **TX2 is always driven in HDMI mode:** the passthrough port no longer inherits the RX DVI/HDMI flag (a port stuck at `0xc0` bit 0 = 0 withholds AVI InfoFrames and audio, so HDMI monitors report "no signal"). Both TX HPD gates (GPIO7/GPIO6) are released together with a settling delay before the splitter status is sampled, and TX2 state transitions are logged with status/`0xc0`/`0x85`. Splitter service interval tightened to 250 ms.
- **`--raw-edid` (`--no-unify-edid`)** CLI opt-out for bit-exact custom EDID passthrough.
- **System Tray integration (Close to Tray):** Clicking `[X]` docks the Console Hub into the system tray and removes it completely from Alt-Tab while streaming continues in the background. Left-clicking the tray icon restores the window; right-clicking opens quick controls and exit.

### Changed
- `edid_build_unified_profile()` appends a CTA-861 Audio Data Block instead of rejecting custom EDIDs that carry none, and falls back cleanly when the extension block is completely full.

---

## [1.2.0] - 2026-10-25

### Added
- **Native Virtual Camera (V4L2 Loopback Sink):**
  - Direct raw YUYV frame output to `/dev/video10` (`"EverLive Capture"`) via `v4l2loopback` (`--v4l2 <dev>` / `--loopback [dev]`).
  - Native unscaled feed for OBS Studio, Discord, or FFmpeg simultaneously with the interactive gaming window or completely headless.
  - Automatic module loading in `play.sh` with `exclusive_caps=0` so `/dev/video10` permanently remains visible in OBS.
  - `install.sh` now installs `v4l2loopback` where the distribution packages it and preconfigures the node via `/etc/modprobe.d/everlive-v4l2loopback.conf` (`video_nr=10 card_label="EverLive Capture" exclusive_caps=0`) and `/etc/modules-load.d/`; `uninstall.sh` removes both again.
  - New `install.sh --no-loopback` (`--skip-loopback`) opt-out for strictly module-free systems: skips the `v4l2loopback` package, removes any leftover EverLive loopback configuration, and leaves no DKMS module or kernel headers behind.
  - Robust auto-retry for `VIDIOC_S_FMT` (throttled to one attempt per 250 ms) when the input resolution changes while an active reader (OBS) still holds buffers. The writer fd is reopened on `-EBUSY` and frame delivery resumes automatically once OBS renegotiates.
- **Headless Studio Mode:**
  - Added `--headless` CLI flag and dedicated Control Hub action ("Start Studio / Headless (OBS)").
  - Captures video over VFIO, pumps frames to the V4L2 loopback sink, and plays HDMI audio without opening an SDL2 window.
  - Ideal for TV / monitor hardware passthrough gaming and dedicated Linux streaming rigs.
- **Seamless Mode Handover in Console Hub:**
  - Added on-the-fly switching between Gaming Mode and Headless Studio Mode without manual stops (`Switch to Gaming Screen` $\leftrightarrow$ `Switch to Studio (Headless)`).
  - Protected with clean DMA and VFIO device handover, retry loops, and double-click prevention.
- **Universal Bitstream Audio (Dolby Digital & DTS 5.1 across all profiles):**
  - All standard EDID profiles (`Default`, `1440p120`, and `1080p240`) now advertise both Linear PCM (up to 8 channels) and compressed Dolby Digital (AC-3 5.1) and DTS 5.1.
  - Continuous content-driven IEC 61937 preamble scanner active by default; zero manual profile switching required when toggling audio formats on PlayStation 5 or Xbox.
  - Consolidated Control Hub EDID dropdown and seamless migration for legacy `settings.json` profiles.
- **Console Hub Modernization:**
  - Added dedicated **Aspect Ratio Override** dropdown (`Auto`, `16:9 Anamorphic`, `4:3 Retro`, `Stretch to Fill`).
  - Added real-time **Audio Format** status indicator (`L-PCM`, `Dolby Digital 5.1 -> Stereo`, `DTS 5.1 -> Stereo`, `(Muted)`).
  - Removed obsolete "Render Engine" dropdown and cleaned up redundant status badges.

### Verified on Hardware
- PlayStation 5 at 1440p120, both in the interactive Gaming Screen and in Headless Studio mode.
- OBS Studio capturing `/dev/video10` (`"EverLive Capture"`) as a regular Video Capture Device.
- On-the-fly Gaming $\leftrightarrow$ Studio handover without restarting the streamer.
- Dolby Digital (AC-3) and DTS 5.1 decoding to stereo, including the mute guard across console format switches.

---

## [1.1.0] - 2026-10-25

### Added
- **Compressed Surround Bitstream Decoding (AC-3 5.1 & DTS 5.1):**
  - Live, real-time IEC 61937 bitstream parser and unpacker for compressed surround audio across 16-bit and 24-bit HDMI audio carriers.
  - Automatic alignment detection (`MSB` vs `LSB`) for 24-bit PCM carriers.
  - Hardware-accelerated decoding via `libavcodec` (`AV_CODEC_ID_AC3` and `AV_CODEC_ID_DTS`).
  - Studio-grade stereo downmixing via `libswresample` (FFmpeg's default downmix matrix).
  - High-durability error concealment and sync recovery for continuous play across mode transitions.
- **Dynamic Bitstream EDID Profile:**
  - Added new `--edid bitstream` profile to `gc573_user_streamer` advertising AC-3 and DTS 5.1 Short Audio Descriptors (SADs) in CTA-861 Extension blocks.
  - Added "Bitstream (Dolby Digital / DTS 5.1)" directly to the Console Hub GUI dropdown.
  - *(Superseded in v1.2.0: every built-in EDID profile now advertises these SADs and `bitstream` remains as a legacy alias.)*
- **Dynamic DMA Ring Depth & Frame Pacing:**
  - Resolution-dependent ring depth (6 buffers at 720p, 5 at 1080p, 4 at 1440p, 3 at 4K, capped by `RLIMIT_MEMLOCK`) keeps at least two FPGA DMA channels armed while a frame is copied in software.
  - Removed the artificial single-buffer fallback for large (1440p/4K) frames and decoupled the audio DMA drain into its own worker thread with a 60 ms cushion, so `SDL_RenderPresent()` can no longer starve audio.
- **Legacy Kernel Module Blacklisting:**
  - Added `/etc/modprobe.d/blacklist-everlive-legacy.conf` to `install.sh` to prevent old out-of-tree `open_gc573.ko` modules from racing with `vfio-pci` at early boot.
  - Automatic removal during `uninstall.sh`.
- **IOMMU Documentation in README:**
  - Added detailed kernel boot parameter documentation (`intel_iommu=on iommu=pt` / `amd_iommu=on iommu=pt`) and diagnosis for VFIO Error -22.
- **Audio Transition Silence:**
  - Purged transitional samples via `SDL_ClearQueuedAudio()` on HDMI format changes to ensure silent mode transitions between PCM and Bitstream.
  - Extended LPCM confirmation window to 800 ms (40 blocks) to prevent false unmuting during console audio renegotiations.
  - Relaxed SDL audio queue cushion to 200 ms to absorb display compositor fluctuations.

### Fixed
- **Phantom DMA channel retirements** that inflated the FPS counter (e.g. ~61 FPS on a 59.94 Hz signal) and occasionally presented half-finished frames. Channels are now retired only on `GC573_DMA_IRQ_COMPLETE`, and the shared interrupt-status register is protected by a lock between the video and audio threads.
- **10-second frame hitch** caused by a blocking frame-rate probe inside the render loop; the probe now runs only on signal re-lock.
- **Unstable frame-rate detection at startup** (59.72 / 59.94 Hz scalers snapping to 60.00 Hz), now measured with a multi-sample pixel-clock median before the FPGA VIP period is programmed.
- **Aspect auto-detection** for 3840×1080 (RetroTINK half-vertical 4K120) as 16:9, plus `--aspect` pass-through in `play.sh` and the Console Hub.
- **App identity in PipeWire and the window manager**: the streamer now reports `EverLive` / `EverLive Audio` and sets its window icon (`SDL_HINT_APP_NAME`, `PULSE_PROP_*`, `SDL_VIDEO_*_WMCLASS`) (fixes #46).

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
