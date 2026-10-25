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
   Native unscaled YUYV frames can be written to a `v4l2loopback` node (`--v4l2 /dev/video10`) for OBS, Discord or FFmpeg, either alongside the gaming window or with `--headless` (no SDL window at all). Format renegotiation after a resolution change is retried automatically while a reader (OBS) still holds its buffers.

---

## Binaries & Source Modules

| Component | Description |
|:---|:---|
| **`gc573_user_streamer`** | The main capture and streaming engine (video DMA, audio DMA, EDID, LED, SDL2 display). |
| **`everlive_userd`** | Standby daemon that holds the VFIO container open between streaming sessions to avoid FLR resets. |
| **`gc573_user_probe`** | Diagnostic tool for testing MMIO, I2C discovery, and DMA mappings. |
| **`gc573_user_edid.[ch]`** | Hardware EDID profile generator and I2C programmer. |
| **`gc573_user_audio.[ch]`** | PCIe DMA audio ring buffer, IEC 61937 scan/mute guard, audio worker thread and SDL2 playback. |
| **`gc573_user_bitstream.[ch]`** | IEC 61937 unpacker plus AC-3/DTS decoder (`libavcodec`) and stereo downmix (`libswresample`). |
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
