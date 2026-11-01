# Changelog

All notable changes to the **EverLive** project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.4.0-experimental] - 2026-10-30

EverLive Experimental ("1.4 Lab") is a side-by-side build: it installs as a
second application, keeps its own config directory and control socket, and
never touches the stable v1.3.0 installation.

### Added
- **lsfg-vk Frame Generation (`gc573_user_framegen.[ch]`):** the custom
OpenGL optical-flow shader pipeline has been replaced by the native
**Lossless Scaling Frame Generation** Vulkan implicit layer (`lsfg-vk`).
  - Three explicit, strictly 2x modes: `off`, `60to120` (60 FPS games) and
`30to60` (PS5 30 FPS games duplicated inside a 60 Hz HDMI container).
  - `off` presents through SDL's `opengl` renderer, so no Vulkan instance is
created and lsfg-vk is 100% bypassed with zero overhead.  The two LSFG modes use
SDL's `vulkan` renderer with `SDL_WINDOW_VULKAN` and VSync, and call
`SDL_RenderPresent()` exactly once per accepted frame - never on an idle
refresh, so the layer never interpolates a frozen pair.
  - Pure filesystem probe for the layer (`/usr/share`, `/etc` and
`$XDG_DATA_HOME/vulkan/implicit_layer.d`) and `lsfg-vk.dll` (`LSFGVK_DLL_PATH`,
`~/.config/lsfg-vk/conf.toml`, both default Steam roots and every extra
`libraryfolders.vdf` library; requires Steam's `lsfg-vk` beta branch of
*Lossless Scaling*).  No external binaries are executed.
  - Environment is exported before the Vulkan instance
(`LSFGVK_ENV`, `LSFGVK_MULTIPLIER=2`, locale-independent `LSFGVK_FLOW_SCALE`,
presence-based `LSFGVK_PERFORMANCE_MODE`, `LSFGVK_DLL_PATH`), FP16 stays
enabled, and a matching `~/.config/everlive-experimental/lsfg.toml` profile is
written for diagnostics.
  - Graceful fallback: if the Vulkan renderer cannot be created the engine logs
a clear warning, recreates an `opengl` window/renderer and never rebuilds in a
loop.
- **Phase-Locked 2:1 HDMI Parity Deduplicator (`gc573_fg_cadence_*`):**
  - 4x2 box-filtered 64x36 luma grid over the central 76% of the frame (letterbox
and ticker bars excluded).
  - Per-parity motion EMA: in `A, A, B, B` one raw-frame parity carries the real
transitions and the other only HDMI repeats, so the lock lands on ~30.0 FPS.
  - Instantaneous phase-slip recovery for `A, A, A, B, B` (and console frame
drops): a clearly larger MAD on the expected-duplicate parity flips the lock
immediately, so B/C are never delayed or lost.
  - Frozen scenes keep a steady 2:1 heartbeat (30.00 Hz) instead of jumping to
60 FPS.  A source already at <= 45 Hz is passed through 1:1.
- **Capture ring:** a 3-slot FIFO fed on every DMA frame; the render thread
coalesces to the newest frame so latency stays at one frame.  The HDR
tonemapper runs on the ring copy via a per-slot filter.
- **CLI / hotkey / IPC control:** `--framegen off|60to120|30to60` (`2x` accepted
as an alias), `--fg-flow-scale 0.5..1.0`, `--fg-perf` / `--no-fg-perf`; hotkey
`g` cycles Off -> 60->120 -> 30->60.  `framegen.set`
(`mode`, `flow_scale`, `perf_mode`) is queued lock-free to the render thread,
and `status.get` exposes `mode`, `present_fps`, `output_fps`, `unique_fps`,
`raw_fps`, `flow_scale`, `perf_mode`, `layer_found`, `dll_found`, `dll_path`,
`vulkan_active` and `deduplicator_active`.
- **Zero-contention V4L2 path:** in the lsfg-vk path the loopback sink keeps
running on its own consumer thread, so a 2560x1440 -> 1920x1080 sws_scale plus
`write()` never blocks the Vulkan swapchain.
- **Console Hub (Experimental):** dedicated
`EXPERIMENTAL · LSFG-VK FRAME GENERATION` card with an offline readiness probe
(green "LSFG-VK Ready · lsfg-vk.dll found" or an amber line naming the missing
package/Steam beta), a Re-check button, the three modes, Flow Scale 50-100% and
Performance Mode.  All settings persist in `~/.config/everlive-experimental`.

### Changed
- **Side-by-side isolation:** config/log now live in
`~/.config/everlive-experimental` (seeded once, read-only, from the stable
`~/.config/everlive/settings.json`); the control socket is
`everlive-experimental.sock`; window titles are `EverLive Experimental` and the
WM class is `EverLiveExperimental`.
- **Installer:** `install.sh` now writes `everlive-experimental.desktop` and
offers an optional `[y/N]` prompt (`--with-framegen` / `--no-framegen`) to
automatically install `lsfg-vk` 2.0 via the distribution package manager
(`pacman` / `yay` / `paru`) or the official `builds.lsfg-vk.dev` archive into
`~/.local`, plus a Steam `lsfg-vk.dll` beta-branch check; the user-local
`install-experimental.sh` mirrors the same optional `--with-framegen` setup
without needing sudo.
- **Removed:** the OpenGL 3.3 optical-flow module, `-lGL`, the CAS shader and
the `gc573_framegen_gltest` target.  Old settings values migrate automatically
(`2x` -> `60to120`, `adaptive` -> `30to60`).

### Fixed (Studio / OBS / lsfg-vk mode switches)
- **`/dev/video10` self-lock:** once the writer fd had called `write()`,
`v4l2loopback` refused `VIDIOC_S_FMT` on that same fd with `EBUSY` even with no
capture reader, so a 1440p console stayed rescaled into a stale 1080p node.
`v4l2_sink_configure()` now detects a geometry transition and, when
`/sys/class/video4linux/<dev>/state` is not `capture`, closes and reopens the
writer fd once and retries `S_FMT` natively.  A real reader keeps its buffers
across the reopen and the scaler fallback still applies.  While a reader is
capturing, the 2 s native upgrade probe is skipped entirely (no fd churn).
- **Cold-start 1080p fallback:** `standby_present()` now leaves the loopback
node alone for a 600 ms grace window (or until a signal has ever been seen), so
a console that is already connected negotiates `/dev/video10` directly at its
native 2560x1440 / 1920x1080 / 3840x2160 geometry.
- **Studio mode now feeds the async sink pump:** `v4l2_pump` runs whenever
`streaming && (fg_live || g_headless)` and a loopback sink is attached, keeping
HDR tonemapping, `sws_scale` and `write()` off the IPC/IT6664 service loop.
The pump is joined before the sink is closed on signal loss, standby and the
`GC573_TEARDOWN_TRACE` shutdown path.
- **Studio -> Gaming lost frame generation:** `switch_output_mode()` now clears
`g_headless` and `g_fg_fallback` *before* `presentation_open()`, so the window
is created as a Vulkan/lsfg-vk surface instead of silently falling back to
OpenGL (restoring headless if the open fails).
- **Frame generation dropped in Studio:** `--framegen` / `--fg-flow-scale` /
`--fg-perf` are now passed in headless mode too; the engine keeps LSFG idle
until `mode.set("gaming")` opens the window.  `start_studio()` persists and
forwards the Hub's Frame Generation selection, and the live Studio -> Gaming
switch re-sends `framegen.set` over IPC.  A headless engine reporting `off` no
longer downgrades the Hub's saved mode, and the card shows an "Armed" state
while in Studio.

### Performance (input-to-photon latency)
- **Low-latency Vulkan swapchain (default ON):** `gc573_fg_apply_env()` now
sets `LSFGVK_PRESERVE_SWAPCHAIN_IMAGE_COUNT=1`, so lsfg-vk keeps the minimal
swapchain image count instead of adding 1-2 buffered frames (8-17 ms at
120 Hz).  Available as `--fg-low-latency` / `--no-fg-low-latency`, the
`low_latency` field in `framegen.set` / `status.get.framegen`, and the new
"Low-Latency Swapchain" checkbox in the Hub; the diagnostic `lsfg.toml`
emits `preserve_swapchain_image_count` and toggling it recreates the swapchain
immediately while streaming.
- **Event-driven render wakeup:** the framegen ring now owns a
`CLOCK_MONOTONIC` condition variable.  `gc573_framegen_input()` signals it as
soon as a DMA frame is queued, and the render loop replaces `SDL_Delay(1)`
(1.2-2.2 ms of Linux timer slack) with `gc573_framegen_wait(1.5 ms)`, waking
within microseconds of the PCIe MSI completion while still servicing SDL
events, hotkeys and IPC.
- **Idle `/dev/video10` throttling:** when lsfg-vk is live and no capture
reader holds the loopback node (`/sys/class/video4linux/<dev>/state` polled
every 500 ms, never per frame), the per-frame 1440p `g_preview` copy and
`write()` are reduced to a 4 Hz keepalive.  The format stays seeded, so OBS
"Leave Unchanged" still opens instantly, and the first poll that sees
`state == "capture"` restores the full frame rate.  Studio/headless output is
never throttled.

### Diagnostic notes
- `lsfg-vk` 2.0 requires `lsfg-vk.dll` from Steam's `lsfg-vk` beta branch of
*Lossless Scaling* (`Lossless.dll` from the default Windows branch lacks the
precompiled Vulkan SPIR-V shaders).  When only `Lossless.dll` is present, the
Hub flags the missing beta branch explicitly; if Vulkan swapchain creation still
fails at runtime, the renderer falls back to OpenGL and the OSD shows
`LSFG 2x [MISSING VULKAN]` instead of claiming a 2x result.

---

## [1.3.0] - 2026-10-30

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
