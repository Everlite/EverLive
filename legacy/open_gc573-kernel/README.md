# open_gc573 — Blob-Free Open Source Linux Driver for AVerMedia Live Gamer 4K (GC573)

> [!NOTE]
> ### 🏛️ Legacy Kernel Module (Historical Reference)
> As of **EverLive v1.0.0**, the project has transitioned to a **100% Userspace VFIO Architecture** located in [`tools/opengc573-userd/`](../../tools/opengc573-userd/).
> This kernel module directory is preserved for historical reference and educational study.
> For the modern, blob-free userspace driver that requires no DKMS and works out of the box with Secure Boot, please refer to the main repository [README.md](../../README.md).

[![Driver](https://img.shields.io/badge/driver-open__gc573%20(legacy)-grey.svg)](#)
[![License](https://img.shields.io/badge/license-GPL--2.0--only-blue.svg)](#licensing--attribution)
[![AI Assisted](https://img.shields.io/badge/code-heavily%20AI%20assisted-ff69b4.svg)](#)

A fully open-source, blob-free Linux kernel driver (V4L2 + ALSA) for the **AVerMedia Live Gamer 4K GC573** (`1461:0054`, Subsystem `1461:5730`) and **AVerMedia Live Gamer BOLT GC555** (`1461:0054`, Subsystem `1461:5550`).

Unlike the proprietary vendor driver package, `open_gc573` requires **no binary blobs** (`aver_xilinx.o` / `ite6805.o`) to build or load. It compiles directly against modern Linux kernels using pure C and standard kernel frameworks (`videobuf2-dma-sg`, `v4l2-dv-timings`, `sound/pcm`).

---

## Key Features

- **1440p @ 120 FPS Native Capture:** Verified on real hardware (PlayStation 5 / PC). EDID and V4L2 mode tables advertise 2560×1440 @ 120 Hz CVT-RB. The ITE6805 under-read (~100 fps at ~413 MHz) is dynamically rescaled to 120, and VIP register `0x1080[31:16]` maintains unthrottled 120 FPS capture.
- **Dynamic FPGA Clocking:** Automatically calculates and programs internal VIP Frame Period (`0x103c` @ 148.5 MHz) to prevent hardware frame rate decimation.
- **High-Bandwidth DDR Dual-Pixel Mode:** Enables dual-pixel DDR mode (`0x3` in VIP Pixel Mode `0x1088`) for >400 MHz pixel clocks (1440p120, 4K60).
- **Format Flexibility:** Supports NV12, YUYV (YUY2), RGB32, and P010 (10-bit).
- **Extensive Mode & Refresh Support:** Supports standard resolutions from 480p up to 4K60, 1440p120, 1080p up to 240Hz (including 180Hz), ultrawide 3840×1080 (RetroTINK 4K 120Hz BFI mode), and 1920×1200 WUXGA. Non-integer refresh rates within ±2 Hz (such as 59.94 Hz NTSC and 119.88 Hz) are seamlessly supported.
- **Live HDR metadata:** PQ and HLG are classified from the DRM InfoFrame. A colorimetry or HDR change at the same resolution updates the color matrix and does not reset the VIP or DMA. Userspace reads the read-only V4L2 control `hdr_mode` (`0` SDR, `1` PQ, `2` PQ BT.2020, `3` HLG). YUYV PQ is reported as transfer function SMPTE 2084.
- **ALSA Multi-Channel & Stereo HDMI Audio:** Native ALSA capture device supporting 8-channel (7.1 surround), 6-channel (5.1 surround), and 2-channel stereo LPCM.
- **On-the-Fly Hardware RGB Control:** Sysfs attribute `led_mode` (BAR 0 `0x0800`–`0x086c` & script RAM `0x2008`) permanently solves the factory blinking loop and enables instant color switching (Off, Blue, Green, Red, Cyan, Magenta, Yellow, White) without stream restarts.
- **Subsystem Auto-Detection:** Automatically binds to both PCIe GC573 (`1461:5730`) and Thunderbolt GC555 (`1461:5550`).

---

## Architecture Overview

```
                      +-----------------------------+
                      |   HDMI In (PS5 / PC / etc)  |
                      +--------------+--------------+
                                     |
                                     v
                       ITE IT6805 Dual HDMI 2.0 Rx
                         (gc573-it6805-core.c)
                                     |
                  [LVDS / TTL DDR Dual-Pixel @ ~420 MHz]
                                     v
                       Xilinx Artix-7 FPGA (VIP)
                         (gc573-fpga.c)
                         - Reg 0x103c: Frame Period (148.5 MHz / rate; fractional rate when known)
                         - Reg 0x1080: Target FPS (rate << 16)
                         - Reg 0x1088: Pixel Mode (0x3 = Dual-Px DDR)
                                     |
                                     v
                       PCIe Scatter-Gather DMA
                         (gc573-dma.c / gc573-video-dma.c)
                                     |
                     +---------------+---------------+
                     |                               |
                     v                               v
             V4L2 Video Node                 ALSA Audio Device
              /dev/video*                       hw:CARD=A4K,DEV=0
```

---

## Building and Testing

### Build
```bash
make
# Or with Clang/LLVM:
# make LLVM=1 CC=clang
```

### Safe Load / Test
```bash
./load.sh
```
The script will unload any previously running driver instance and cleanly load `open_gc573.ko`.

### Test Capture (1440p @ 120 FPS)
```bash
v4l2-ctl --stream-mmap --stream-count=240 -d /dev/videoX
```

### Unload
```bash
./unload.sh
```

---

## Licensing & Attribution

- **License:** GPL-2.0-only
- Based on the pioneering reverse-engineering and architecture by **ostrich** ([github.com/ostrich/gc555](https://github.com/ostrich/gc555)), released under GPL-2.0-only.
- Extended with:
  - PCI Subsystem `1461:5730` GC573 support and soft-failing LED detection.
  - Native 2560×1440 @ 120 Hz CVT-RB EDID injection and RGB Limited color pipeline validation.
  - FPGA `VIP_FRAME_PERIOD` (0x103c) / `VIP_PIXEL_MODE` (0x1088) tuning for unthrottled 120 FPS capture.

### Disclaimers & Trademark Notice
- **Trademarks:** AVerMedia, Live Gamer 4K, and Live Gamer BOLT are trademarks of AVerMedia Technologies, Inc. This independent project is not affiliated with or endorsed by AVerMedia.
- **Warranty:** Distributed under the GNU General Public License v2 without warranties of any kind. Use at your own risk.
