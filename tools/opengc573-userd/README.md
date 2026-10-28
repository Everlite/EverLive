# OpenGC573 Userspace VFIO Engine

This directory contains the 100% blob-free, Ring 3 userspace driver engine for the **AVerMedia Live Gamer 4K (GC573)** and **Live Gamer BOLT (GC555)**.

As of **EverLive v1.0.0**, this userspace engine is the default, primary driver for EverLive, eliminating the need for out-of-tree kernel modules and DKMS rebuilds.

---

## Architecture

1. **PCIe Hardware Access via `vfio-pci`:**  
   The capture card is attached to Linux's standard upstream `vfio-pci` driver.
2. **Userspace MMIO via `mmap()`:**  
   The Xilinx Artix-7 FPGA BAR0 register space is directly mapped into the process virtual address space.
3. **Userspace I2C Subsystem:**  
   The IT6805 (HDMI 2.0 Receiver), IT6664 (Transmitter / Passthrough), and Artix-7 RGB lighting registers are driven via hardware I2C master blocks.
4. **Hardware DMA via IOMMU:**  
   Scatter-Gather video and audio DMA ring buffers are pinned and mapped directly into userspace via `VFIO_IOMMU_MAP_DMA`.
5. **Direct Video Presentation:**  
   Frames are presented directly via an SDL2 OpenGL rendering context.
6. **HDMI Audio DMA & Decoding:**
   Linear PCM (up to 8 channels) as well as Dolby Digital (AC-3) and DTS 5.1 carried as IEC 61937 bursts are captured over DMA. Compressed streams are decoded with `libavcodec` and downmixed to 48 kHz stereo via `libswresample`; a content-driven mute guard keeps raw bitstreams away from the speakers.
7. **Hardware EDID Profile Management:**
   Predefined and custom binary EDIDs can be flashed directly into the IT6805 receiver RAM over I2C. Every built-in profile advertises L-PCM, AC-3 and DTS 5.1.
8. **Standby Daemon (`everlive_userd`):**
   Maintains card state and LED configuration via file descriptor handoff without triggering a PCIe Function Level Reset (FLR) when the streaming window closes.
9. **Virtual Camera & Headless Mode:**
   Native unscaled YUYV frames can be written to a `v4l2loopback` node (`--v4l2 /dev/video10`) for OBS, Discord or FFmpeg, either alongside the gaming window or with `--headless` (no SDL window at all). The writer probes the node with `VIDIOC_TRY_FMT` before every negotiation: if a reader (OBS) already holds a fixed format, that geometry is adopted and every frame is rescaled into it with `libswscale`, so the picture appears immediately instead of staying black. The native mode is retried every 2 s and picked up automatically as soon as the reader releases the format.
10. **Procedural No-Signal Screen:**
   When no HDMI signal is present (console off, cable pulled, signal loss mid-session), the engine renders a generated "NO HDMI SIGNAL" card — dark gradient, subtle grid, crossed-out monitor icon, bitmap-font text — and pushes it at ~30 fps to both the SDL window and the loopback node, so OBS never sees a frozen or black feed. The picture is generated once per geometry (`gc573_user_nosignal.[ch]`), costs no CPU while cached, and is replaced seamlessly by the live DMA feed as soon as the signal locks.

---

## Binaries & Source Modules

| Component | Description |
|:---|:---|
| **`gc573_user_streamer`** | The main capture and streaming engine (video DMA, audio DMA, EDID, LED, SDL2 display). |
| **`everlive_userd`** | Standby daemon that holds the VFIO container open between streaming sessions to avoid FLR resets. Drives the RGB lighting: static `--color R G B`, `--off`, or a smooth `--rainbow` / `--cycle SECONDS` spectrum. |
| **`gc573_user_probe`** | Diagnostic tool for testing MMIO, I2C discovery, and DMA mappings. |
| **`gc573_user_edid.[ch]`** | Hardware EDID profile generator and I2C programmer. |
| **`gc573_user_audio.[ch]`** | PCIe DMA audio ring buffer, IEC 61937 scan/mute guard, audio worker thread and SDL2 playback. |
| **`gc573_user_bitstream.[ch]`** | IEC 61937 unpacker plus AC-3/DTS decoder (`libavcodec`) and stereo downmix (`libswresample`). |
| **`gc573_user_nosignal.[ch]`** | Procedural YUYV standby screen (gradient, grid, icon, bitmap-font text) for the window and the V4L2 sink. |
| **`gc573_user_video.[ch]`** | Mode table, timings verification, and FPGA VIP configuration. |
| **`gc573_user_fpga.[ch]`** | Artix-7 FPGA register definitions and RGB lighting control. |
| **`gc573_vfio.[ch]`** | Low-level VFIO container, group, device, and IOMMU DMA mapping primitives. |
| **`vfio_pci_bind.sh`** | Utility script to bind or unbind the capture card to/from `vfio-pci`. |

---

## Building

```bash
make -C tools/opengc573-userd -j"$(nproc)"
```

---

## Command Line

```text
gc573_user_streamer [PCI_ADDRESS] [OPTIONS]

  --led COLOR          off|blue|green|red|cyan|magenta|yellow|white|#RRGGBB
  --edid VALUE         default|1440p120|1080p240|/path/to/custom.edid
  --raw-edid           Load custom EDID files bit-exact (no AC-3/DTS injection)
                       alias: --no-unify-edid
  -f, --fullscreen     Start the SDL2 window in borderless fullscreen
  --vsync/--no-vsync   Pace presentation to the display refresh (default: off)
  --aspect MODE        auto|16:9|4:3|stretch
  --osd / --no-osd     Stats overlay on/off (--streamer-mode = hidden overlay + cursor)
  --osd-pos POS        tr|br|bl|tl (default: tr)
  --v4l2 DEVICE        Write native YUYV frames to a v4l2loopback device
  --loopback [DEVICE]  Shorthand for --v4l2 (default: /dev/video10)
  --headless           No SDL window; capture, V4L2 output and audio only
```

Game-window hotkeys: <kbd>f</kbd>/<kbd>F11</kbd> fullscreen, <kbd>a</kbd> aspect ratio,
<kbd>p</kbd> OSD position, <kbd>o</kbd>/<kbd>i</kbd>/<kbd>Tab</kbd> stats overlay,
<kbd>s</kbd>/<kbd>F10</kbd> overlay + cursor, <kbd>Esc</kbd>/<kbd>q</kbd> quit.

---

## Custom EDID Requirements & Audio Unification

Custom EDIDs must be exactly **256 bytes** (base block + one CTA-861 extension) and pass the header/checksum check in `edid_validate()`.

By default EverLive **unifies the audio block** of every custom EDID: the CTA-861 Audio Data Block is rewritten to advertise L-PCM (8 ch), AC-3 5.1 and DTS 5.1. If the file has no audio block, a complete one is appended. Replacing an L-PCM-only block needs 6 spare bytes, appending one needs 10; the optional Video Capability Data Block is dropped first if that is the only way to retain all detailed timings.

If the extension block has no room at all, the unmodified file is used and the streamer logs:
`[EDID] Using raw audio block from '<path>' (unification bypassed: no space for SADs)`

Pass `--raw-edid` (alias `--no-unify-edid`) to load custom files **bit-exact** without any modification.
