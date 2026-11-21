# Architecture & Design

EverLive runs its PCIe capture drivers in **Ring 3 (userspace)** via Linux's in-tree `vfio-pci` subsystem, avoiding out-of-tree kernel modules, DKMS rebuilds, and UEFI Secure Boot MOK enrollment.

## Key Components

- **`vfio-pci` (Kernel, Ring 0):** Isolates the capture card inside an IOMMU container and maps BAR0 MMIO + scatter-gather DMA buffers directly into userspace.
- **`libeverlive-core` (`tools/libeverlive-core/`):** Shared framework providing PCI card autodiscovery, `everlive_framestore` (lock-free triple-buffered POSIX shared memory `/dev/shm/*_v4l2_shm`), reusable `LD_PRELOAD` V4L2 camera shims, cross-user handoff (`\0everlive-handoff-<bdf>` via `SO_PEERCRED`), `everlive_osd`, `everlive-ctl`, and `everlive.service`.
- **Per-Card Userspace Engines (Ring 3):**
  - `tools/opengc573-userd/`: `gc573_user_streamer` (video/audio DMA, Hable HDR→SDR, AC-3/DTS decode, S3 suspend/resume, LSFG-VK path) + `everlive_userd` (FD-preserving standby daemon to avoid PCIe FLR).
  - `tools/opengc570d-userd/`: `opengc570d-userd` (dual HDMI DMA rings, BT.2020 LUT, stereo LPCM).
  - `tools/openhws-userd/`: `hws_user_streamer` (quarter-DMA 4K60, stereo LPCM).
- **Webcam Bridge (`tools/everlive-cam/`):** `everlive_camd` opens the physical webcam once and publishes frames to `/dev/shm/everlive_cam_v4l2_shm` for Console Hub PiP, `ffmpeg` recordings, and `/dev/video65` (`libeverlive-cam-v4l2.so`).
- **Console Hub (`tools/everlive/`):** Python/Qt frontend managing card selection, Multiview, recordings, background service, and JSON-RPC IPC (`everlive.sock`).

## Architecture Diagram

```mermaid
flowchart TD
  subgraph Hardware["HDMI Sources & Capture Hardware"]
    source["Consoles / PCs / Retro Scalers"]
    gc573_hw["AVerMedia GC573 (1461:0054, subsys 5730)"]
    gc570d_hw["AVerMedia GC570D (1461:0054, subsys 5700)"]
    hwsuhd_hw["HWS UHD (8888:8581, subsys 0007)"]
    gc553_hw["AVerMedia GC553G2 (USB 07ca:2553)"]
    webcam_hw["USB Webcam"]
    source --> gc573_hw & gc570d_hw & hwsuhd_hw & gc553_hw
  end

  subgraph Kernel["Linux Kernel (Ring 0)"]
    vfio["vfio-pci (In-Tree IOMMU + DMA)"]
    uvc["uvcvideo + snd-usb-audio"]
    v4l2loop["v4l2loopback (Optional /dev/video10,11)"]
    gc573_hw & gc570d_hw & hwsuhd_hw --> vfio
    gc553_hw --> uvc
  end

  subgraph Core["libeverlive-core & Shared Memory"]
    registry["Card Registry & Autodiscovery"]
    ctl["everlive-ctl / everlive.service"]
    handoff["Cross-User Handoff Socket"]
    shm["POSIX Frame Stores (/dev/shm/*_v4l2_shm)"]
    cam_shm["Webcam Frame Store (/dev/shm/everlive_cam_v4l2_shm)"]
  end

  subgraph Engines["Userspace Engines (Ring 3)"]
    s573["gc573_user_streamer + everlive_userd"]
    s570["opengc570d-userd"]
    shws["hws_user_streamer"]
    camd["everlive_camd"]
  end

  subgraph UI["Desktop, Audio & Recording"]
    hub["EverLive Console Hub (Qt / Multiview / Recorder)"]
    win["SDL2 OpenGL / Vulkan (LSFG-VK) Viewport"]
    pw["PipeWire Audio (EverLive Capture Audio)"]
    obs["OBS / Discord / Browsers (Loopback, LD_PRELOAD /dev/video60..65, PipeWire)"]
  end

  vfio --> registry --> s573 & s570 & shws
  s573 <--> handoff
  webcam_hw --> camd --> cam_shm
  s573 & s570 & shws --> shm --> obs
  s573 & s570 & shws -.-> v4l2loop --> obs
  s573 & s570 & shws --> win & pw
  pw --> obs
  cam_shm --> hub & obs
  uvc --> hub
  hub & ctl -.->|"JSON-RPC"| s573 & s570 & shws
```
