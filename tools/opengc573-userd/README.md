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
6. **L-PCM PCIe DMA Audio:**  
   Uncompressed 16-bit / 48kHz audio is captured over DMA and passed to SDL2 / PipeWire.
7. **Hardware EDID Profile Management:**  
   Predefined and custom binary EDIDs can be flashed directly into the IT6805 receiver RAM over I2C.
8. **Standby Daemon (`everlive_userd`):**  
   Maintains card state and LED configuration via file descriptor handoff without triggering a PCIe Function Level Reset (FLR) when the streaming window closes.

---

## Binaries & Source Modules

| Component | Description |
|:---|:---|
| **`gc573_user_streamer`** | The main capture and streaming engine (video DMA, audio DMA, EDID, LED, SDL2 display). |
| **`everlive_userd`** | Standby daemon that holds the VFIO container open between streaming sessions to avoid FLR resets. |
| **`gc573_user_probe`** | Diagnostic tool for testing MMIO, I2C discovery, and DMA mappings. |
| **`gc573_user_edid.[ch]`** | Hardware EDID profile generator and I2C programmer. |
| **`gc573_user_audio.[ch]`** | PCIe DMA audio ring buffer and SDL2 playback integration. |
| **`gc573_user_video.[ch]`** | Mode table, timings verification, and FPGA VIP configuration. |
| **`gc573_user_fpga.[ch]`** | Artix-7 FPGA register definitions and RGB lighting control. |
| **`gc573_vfio.[ch]`** | Low-level VFIO container, group, device, and IOMMU DMA mapping primitives. |
| **`vfio_pci_bind.sh`** | Utility script to bind or unbind the capture card to/from `vfio-pci`. |

---

## Building

```bash
make -C tools/opengc573-userd -j"$(nproc)"
```
