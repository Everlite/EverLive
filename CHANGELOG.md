# Changelog

All notable changes to the **EverLive** project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### Fixed
- **GC573 green/white speckles and audio crackle on HDMI 2.0 sources (4K60, 1440p120):**
  - The IT6664 input receiver was left at its fixed power-on equaliser (RS `0x1f`, forced), so >340 MHz TMDS links decoded a steady stream of wrong characters. The errors were re-encoded by the IT6664, so they showed up on the HDMI OUT passthrough too and were invisible to the IT6805. Windows was clean on the same hardware and cable.
  - New `gc573_user_rx_eq.[ch]` drives both receivers (IT6664 input and IT6805 capture share the same ITE receiver core). On HDMI 2.0 lock it measures per-lane TMDS character errors (CED), leaves error-free links alone, and otherwise runs hardware auto-EQ with DFE taps, falling back to a per-lane RS sweep and skew retraining scored by measured errors.
  - Hardware result (RetroTINK 4K, 4K60 RGB, ~593 MHz): input errors went from ~25,500 per 100 ms to 0 on every lane. Tuning adds ~0.75 s at signal lock.

### Added
- `[HDMI] TMDS errors B/G/R` line in `streamer.log` with per-lane error counts for the card input and the internal capture hop: every second while errors occur, otherwise every 10 s.
- `EVERLIVE_NO_AUTO_EQ=1` disables link equalisation; `EVERLIVE_EQ_INPUT_DFE=0` keeps the input on RS-only EQ (for comparison and troubleshooting).

## [1.4.0] - 2026-11-06

### Added
- **2x Motion-Interpolation Frame Generation (Beta · GC573):**
  - Integrated 2x motion-interpolation frame generation powered by the Vulkan `lsfg-vk` layer (Lossless Scaling Frame Generation 2x on Linux).
  - Dedicated modes: `30 → 60 FPS` for 30 FPS console titles with real-time phase-locked cadence deduplication, and `60 → 120 FPS` for 60 FPS feeds on high-refresh displays.
  - Custom low-latency Vulkan swapchain presentation pipeline with optional `Low-Latency Swapchain` mode (`LSFGVK_PRESERVE_SWAPCHAIN_IMAGE_COUNT=1`).
  - Automatic Steam library discovery across all multi-disk Steam library paths (`libraryfolders.vdf`).
  - Graceful fallback: 100% standalone and optional; falls back to native ultra-low-latency presentation if components are absent.
- **AVerMedia Live Gamer DUO (`GC570D`) Userspace VFIO Engine (`tools/opengc570d-userd`):**
  - Added full blob-free userspace `vfio-pci` capture support for the **AVerMedia Live Gamer DUO (`GC570D`, PCI `1461:0054`, subsystem `1461:5700`)**, ported from [`neoyagami/OpenGC570D`](https://github.com/neoyagami/OpenGC570D) (commit `4673bb9`, GPL-2.0-only).
  - Automatic PCI subsystem disambiguation (`0x5730` Live Gamer 4K `GC573` vs. `0x5700` Live Gamer DUO `GC570D`) in `play.sh` and `tools/everlive/everlive_viewer.py`.
  - Simultaneous dual-HDMI capture pipeline:
    - **HDMI IN 1 (Passthrough + Capture):** ITE IT68051 receiver + `VideoSplitter` microcontroller + Xilinx 3000-nit BT.2020 HDR-to-SDR 3D LUT preprocessor + VIP0 hardware scaler (`4K60 HDR -> 1080p60 YUYV`, plus native `1080p`, `720p`, `1024x768`, `576p`, `480p`, and `640x480`).
    - **HDMI IN 2 (Secondary Capture):** ITE IT6802 receiver + VIP1 hardware scaler (`1080p60 YUYV`) for facecam or secondary console capture.
  - Hardware-validated `HDMI IN 1` 2.0 Stereo and 5.1/7.1 Multichannel L-PCM passthrough (`PR #6` / `484aa8d`) and active non-blocking audio recovery (`4673bb9`) across live `2.0 <-> 5.1` format switches without disturbing HDMI OUT video passthrough.
- **EverLive SDL2 Gaming Preview, Picture-in-Picture & Dual Virtual Cameras for DUO:**
  - Integrated `gc570d_everlive.[ch]` frontend directly into `opengc570d-userd` reading lock-free seqlock frames from `/dev/shm/gc570d_v4l2_shm`.
  - Interactive SDL2 gaming window (`--sdl`) with live `1`/`2` input switching, Picture-in-Picture (`--pip` / `P`), aspect ratio overrides (`--aspect` / `A`), integer scaling (`--scale` / `L`), live stats OSD (`Tab`/`O`, corner cycle `C`), and low-latency SDL2 desktop audio monitoring (`M` mute, `↑`/`↓` volume).
  - Dual `v4l2loopback` virtual camera outputs: `/dev/video10` (`"EverLive Capture"`, following the selected DUO input) and `/dev/video11` (`"EverLive Capture HDMI 2"`, dedicated to HDMI IN 2), plus module-free PipeWire camera/audio source nodes and `libgc570d-v4l2.so` (`LD_PRELOAD`) virtual V4L2 fallback (`/dev/video60` and `/dev/video61`).
- **Console Hub DUO Controls & Runtime CLI (`gc570d-ctl`):**
  - Added Live Gamer DUO controls to `everlive_viewer.py`: **DUO Input** selector (`HDMI 1` / `HDMI 2`), **EDID: Internal Bridge** vs. **EDID: Downstream Copy** policy buttons, **Pulse HPD** hot-plug button, and DUO RGB LED effect mapping (`colorful`, `breathing`, `static`, `off`) over `$XDG_RUNTIME_DIR/gc570d/control.sock`.
  - Updated `build.sh`, `install.sh`, `uninstall.sh`, and `.github/workflows/ci.yml` to build, configure, and verify both `opengc573-userd` and `opengc570d-userd`.

### Fixed
- **Instant 4K Capture via CAP_IPC_LOCK (PR #53 by Lou Perret):**
  - Implemented raw `capget` syscall query in `gc573_user_streamer.c` to detect active `CAP_IPC_LOCK`. When present, EverLive bypasses `RLIMIT_MEMLOCK` accounting immediately after installation without requiring a session logout/relogin.
- **Direct Headless / Studio Mode Launch:**
  - Resolved unassigned volume variable in Console Hub GUI (`start_studio()`) that prevented launching directly into Headless/OBS mode.
- **PipeWire Virtual Audio Tap Isolation:**
  - Added `PW_STREAM_FLAG_DONT_RECONNECT` and `"node.dont-reconnect" = "true"` to prevent tap fallback to default playback devices.
  - Added persistent null sink provisioning via `pactl load-module module-null-sink` fallback.
- **GC570D VFIO interrupt allocation:** Restored MSI-first ordering in `pci_alloc_irq_vectors()`. The INTx-first sequence introduced during the 1.4.0 hardening work could stall the bridge I2C engine, causing the HDMI IN 2 receiver probe to time out at register `0x0f` (`-110`) and aborting probe on real GC570D hardware. INTx remains the fallback, and the 2 ms MMIO status poll covers INTx and eventfd-less setups.
- **GC570D hardware validation:** VFIO probe, MSI interrupt delivery, IT68051/IT6802 initialization and IRQ handling verified on a physical Live Gamer DUO.

---

## [1.3.2] - 2026-11-03

### Fixed
- **Direct Headless / Studio Mode Launch:**
  - Resolved an unassigned volume variable in Console Hub GUI (`start_studio()`) that prevented launching directly into Headless/OBS mode without starting the Gaming Screen first.
- **PipeWire Virtual Audio Tap Isolation:**
  - Added `PW_STREAM_FLAG_DONT_RECONNECT` and `"node.dont-reconnect" = "true"` to the in-process audio tap stream, preventing WirePlumber from falling back to default playback devices (speakers/headphones).
  - Hardened runtime sink provisioning via `pactl load-module module-null-sink` fallback so the virtual capture sink remains persistent across client lifecycles, eliminating duplicate audio playback when Virtual Audio Track is enabled.

---

## [1.3.1] - 2026-11-01

### Added
- **Pixel-Perfect Integer Scaling Mode:**
  - Added zero-blur integer scaling mode (`SCALE_INTEGER`) for retro and fixed-resolution console sources (e.g., 720p PS3/Xbox 360/Switch or 1080p sources on 1440p/4K panels). Preserves square Pixel Aspect Ratio (PAR) with nearest-neighbor texture sampling, 4:2:2 chroma alignment (`dst.x &= ~1`), and automatic fallback to bilinear aspect-correct fit when `factor < 1` or aspect ratio is anamorphic.
  - Switchable via `1` or `l` keyboard hotkeys in the SDL2 gaming window, CLI argument `--scale integer|fit`, and dynamic dropdown in the Console Hub GUI.
  - Controllable dynamically over JSON-RPC IPC (`scale.set` and `status.get`).
- **In-Game Audio Controls & 2-Way GUI Synchronization:**
  - Added keyboard hotkeys `↑` / `↓` (+5% / -5%) and `+` / `-` (including Numpad) for live volume adjustment, and `m` for instant audio mute in the gaming window.
  - Automatic unmute when raising volume from a muted state.
  - Real-time 2-way IPC sync: Console Hub GUI continuously polls and reflects keyboard hotkey volume and mute changes on its sliders and buttons.
  - Added `--volume <0-100>` / `-v` CLI flag to configure startup audio level.
- **Jitter-Free Stats OSD with Live DMA Latency:**
  - Added live DMA capture latency measurement (`DMA: %4.1fms`) sourced from DMA ring buffer completion to frame display.
  - Added active integer scaling factor indicator (e.g., `2x Integer`).
  - Stabilized OSD layout width to a padded 4-character grid with generous minimum width (`max_len = ((max_len + 3) / 4) * 4 + 2`) to eliminate box twitching/wobbling on fractional latency changes.
- **F1 / ? Hotkey Help Card Overlay:**
  - Added on-screen keyboard shortcut reference card centered in the viewport, toggled via `F1` or `?`.
  - `Esc` cleanly dismisses the help card first before quitting the streamer.

---

## [1.3.0] - 2026-10-31

### Added
- **HDR10 & HLG Detection + Real-Time Filmic SDR Tonemapper:**
  - Added IT6805 DRM InfoFrame decoding (`0x87`, Bank 2) in `gc573_user_it6805.[ch]` with 1 Hz live polling to detect SMPTE ST 2084 (PQ / HDR10) and HLG signals, `MaxCLL`, and `MaxFALL`.
  - Added in-place YUYV tonemapper (`gc573_user_hdr.[ch]`) using a John Hable filmic curve calibrated to ITU-R BT.2408 reference diffuse white (`203 cd/m²`), BT.709 OETF, limited-range scaling (`16..235`), and a 2x2 BT.2020-to-BT.709 chroma matrix. Both the SDL2 gaming window and OBS (`/dev/video10`) receive accurate SDR colors, while native SDR signals stay on the zero-copy fast path.
  - Added runtime toggle (`h` hotkey in game window, `--no-hdr`, IPC `hdr.set`) and linear exposure bias (`--hdr-exposure -2.0..+2.0`, live EV steppers in Console Hub).
- **Native PlayStation 5 1440p60 & 1440p120 SDR/HDR EDID Support:**
  - Added VESA CVT-RB `2560x1440 @ 59.95 Hz` (`241.50 MHz`) Detailed Timing Descriptors alongside `1440p120` in both `Default (4K60 / 1440p120 HDR)` and `Force 1440p @ 120 Hz` hardware EDID profiles (`gc573_user_edid.c`).
  - Hardware-verified on PlayStation 5 via *Test 1440p Output*: unlocks both `60 Hz SDR/HDR` and `120 Hz SDR/HDR`.
- **Dedicated PipeWire Virtual Audio Track for OBS / Broadcasting:**
  - Added in-process PipeWire audio tap (`gc573_user_audio_tap.[ch]`) publishing mixed, decoded HDMI audio directly to a dedicated virtual audio sink named `"EverLive Capture Audio"`.
  - Enables clean, isolated game audio recording in OBS (`Monitor of EverLive Capture Audio`), Discord, or FFmpeg without capturing desktop noise, system sounds, Discord chat, or background music.
  - Configured seamlessly in `install.sh` via `/etc/pipewire/pipewire.conf.d/50-everlive-capture.conf` (zero kernel modules, zero DKMS dependencies). Cleanly removed by `uninstall.sh`.
  - Supports both interactive Gaming Screen and Headless Studio modes.
- **Control-IPC Engine (Zero-Interrupt Runtime Mutations):**
  - High-performance, lock-free JSON-RPC 2.0 control socket (`$XDG_RUNTIME_DIR/everlive.sock`) powered by `gc573_user_ipc.[ch]` with `epoll` and `SO_PEERCRED` security.
  - Control commands communicate via a lock-free Single-Producer Single-Consumer (SPSC) command ring directly to the streamer thread without tearing down DMA buffers, dropping frames, or stalling hardware pipelines.
  - Python GUI automatically docks to the control socket (`everlive_client.py`) using non-blocking `QSocketNotifier`.
- **Zero-Interruption Live Mode Switch & Dynamic Virtual Camera:**
  - **Live Gaming <-> Studio Switch (`mode.set`):** Swaps between the interactive SDL2 gaming window and Headless Studio mode in place without closing the VFIO container, DMA ring, audio stream, or active OBS recording.
  - **Live Virtual Camera Toggle (`v4l2.set`):** Dynamically attaches or detaches `/dev/video10` (`v4l2loopback`) on a running engine with immediate format negotiation when a signal is already locked.
  - **Volume, Mute, RGB, Aspect & OSD:** Live gain adjustments (20 ms linear fade), instant mute, on-the-fly RGB lighting modes (wave, rainbow, static, off), aspect ratio switching, and OSD overlay controls without stream restarts.
- **Redesigned Console Hub GUI:**
  - Clean dark hardware-utility interface (`everlive_viewer.py`) with a single horizontal runtime action bar (`Start Gaming Screen`, `Start Studio / Headless (OBS)`, `Restart Engine`), live signal/audio telemetry badges, and structured `VIDEO & COLOR`, `AUDIO & HARDWARE`, and `OBS OUTPUTS, OVERLAY & APP` sections.

### Fixed
- **V4L2 Virtual Camera Self-Lock & Studio Thread Offload:** Added a 600 ms startup grace window before publishing standby frames to `/dev/video10` so cold-starting into an active HDMI source never pre-locks `v4l2loopback` at `1080p`. Added sysfs `consumers` detection (`v4l2_reader_active`) to reopen self-locked writer buffers cleanly on native geometry changes without churning active OBS captures, and offloaded Headless Studio V4L2 writes onto a dedicated worker thread.
- **PS5 Cold-Start HDMI Handshake:** Unconditionally power and unblank the internal IT6664 TX1 capture port upon RX lock and track 5V transitions cleanly across `gc573_user_it6664.c` and `gc573_user_it6805.c`, fixing "No HDMI Signal" when a console boots after EverLive is already running.
- **Local IPC Docking:** Resolved an issue where local sessions started from the GUI did not attach to the engine's IPC socket, allowing live adjustments to take effect immediately without needing a mode switch or stream restart.
- **24-bit Audio Gain Normalization:** Fixed sample alignment in the audio worker gain pass to support both S16 and 4-byte widened S32 formats correctly.
- **Dependencies:** Added `libpipewire-0.3-dev` (Ubuntu/Debian), `pipewire-devel` (Fedora), and `pipewire` (Arch) to `install.sh` automated package verification and CI workflow.

---

## [1.2.3] - 2026-10-28

### Added
- **Zero-Sudo Boot Auto-Binding:** `install.sh` now configures `/etc/modules-load.d/everlive-vfio.conf` (`vfio`, `vfio_pci`, `vfio_iommu_type1`) and `/etc/modprobe.d/everlive-vfio.conf` (`options vfio-pci ids=1461:0054,1461:0055`). The Linux kernel automatically binds the AVerMedia GC573 and GC555 to `vfio-pci` right during early boot, completely eliminating password prompts when launching EverLive or running it via desktop autostart (`~/.config/autostart/everlive.desktop`).
- **Live RGB Worker Thread:** `gc573_user_streamer` now runs a dedicated 20 Hz pthread worker to continuously animate the 5-zone traveling rainbow wave (`--wave`) and whole-card breathing spectrum cycle (`--rainbow`) while active capturing is running.
- **Combined CLI & Tray Execution:** `--tray` (`--minimized`) can now be combined seamlessly with `--headless`, `--loopback`, and `--wave`/`--rainbow` in a single command, docking EverLive directly into the system tray while capturing to `/dev/video10` without launching conflicting daemon instances.

### Fixed
- **Startup LED Red Blinking Gap:** moved streamer LED preset application to millisecond 10 (immediately after FPGA bridge initialization) and prevented redundant standby daemon launches when starting directly into stream mode, eliminating the ~3s unprogrammed red-flashing gap during HDMI receiver init and EDID handshakes.
- **Console Power-On After EverLive (5V Rise Detection):** added 5V power-rise detection in `gc573_user_it6664.c`. When a console (PS5, Xbox, Switch) is powered on after EverLive is already active, EverLive detects the 5V HDMI line and automatically triggers an RX HPD pulse so the console immediately locks onto video without needing a cable replug or app restart.
- **GUI Headless Autostart Initialization:** fixed an `AttributeError` on `_headless_start` in `MainWindow` during UI initialization.

---

## [1.2.2] - 2026-10-28

### Added
- **Procedural "NO HDMI SIGNAL" standby screen:** with no HDMI signal (console off, cable pulled, signal lost mid-session) the engine now renders a generated dark-gaming card — gradient background with a subtle grid and vignette, a crossed-out monitor icon, the `EVERLIVE` wordmark, `NO HDMI SIGNAL` and `Waiting for console / input...` in the built-in 8x8 bitmap font — and pushes it at ~30 fps to both the SDL window and the `v4l2loopback` node, so OBS never shows a frozen or black feed. The YUYV frame is generated once per geometry (`gc573_user_nosignal.[ch]`, pure C, no image libraries) and cached, so an idle session costs no CPU for rendering; the live DMA feed takes over again as soon as the signal locks.
- **Multi-Zone RGB Rainbow Wave & Smooth Spectrum Cycle:** `everlive_userd` gained `--wave` (5-zone traveling rainbow wave with 72° phase shift across the physical LED blocks) and `--rainbow` (whole-card spectrum cycle with gentle breathing), configurable via `--cycle [SECONDS]` (1-120 s, default 3.0 s for wave, 8.0 s for breathing). The Console Hub GUI includes presets for both effects.
- **CLI flag `--tray` (`--minimized`):** launches EverLive directly docked into the system tray without popping up the main window, ideal for desktop autostart scripts.

### Fixed
- **OBS black screen when the virtual camera is opened first:** `v4l2loopback` lets a reader hold the format, so the writer's `VIDIOC_S_FMT` was either refused with `-EBUSY` or silently answered with OBS's geometry — while the streamer kept writing native-size frames, which the sink dropped (`bytes != frame_bytes`). The writer now probes with `VIDIOC_TRY_FMT`, verifies what `VIDIOC_S_FMT` actually granted, and falls back to `VIDIOC_G_FMT` plus the loopback's `/sys/class/video4linux/<node>/format` attribute. If the reader's mode differs, frames are rescaled into it with `libswscale` (`SWS_BILINEAR`), so the picture appears immediately without toggling the OBS source. The native mode is retried every 2 s and restored automatically once the reader releases it. Non-YUYV reader formats are detected and reported instead of feeding corrupt frames.

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
