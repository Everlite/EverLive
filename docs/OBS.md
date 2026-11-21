# OBS Studio, Virtual Cameras & Webcam Picture-in-Picture

EverLive supports four ways to feed video and audio into OBS Studio, Discord, or browsers.

---

## 1. System-Wide Loopback (`/dev/video10` & `/dev/video11`)

Creates standard V4L2 device nodes visible to any application launched normally:
1. Enable **Legacy V4L2 Loopback** (and optionally **Virtual Audio Track**) in the Console Hub, or pass `--loopback --audio-tap` to `play.sh`.
2. In OBS, add **Video Capture Device (V4L2)** → `EverLive Capture (/dev/video10)` (on GC570D, `/dev/video11` is HDMI 2). Leave resolution at **Device Default**.
3. For isolated game audio without desktop notifications or voice chat, add **Audio Input Capture (PipeWire)** → **`Monitor of EverLive Capture Audio`**.

> **Launch order:** If OBS was opened before EverLive, toggle the Video Capture source visibility or properties in OBS once EverLive starts so OBS negotiates the stream. When no HDMI signal is present, EverLive outputs a 30 FPS standby card so OBS does not freeze.

---

## 2. Module-Free `LD_PRELOAD` Virtual Camera (No `v4l2loopback`)

For DKMS-free / module-free setups (`--no-loopback`), preload shims expose virtual `/dev/video60..65` nodes inside the launched process directly from the POSIX shared-memory frame store:

| Library | Virtual Node(s) | Card / Source |
|:---|:---|:---|
| `/usr/local/lib/libgc573-v4l2.so` | `/dev/video63` | GC573 (post-tonemap) |
| `/usr/local/lib/libgc570d-v4l2.so` | `/dev/video60`, `/dev/video61` | GC570D HDMI 1 & HDMI 2 *(in community validation)* |
| `/usr/local/lib/libhwsuhd-v4l2.so` | `/dev/video64` | HWS UHD |
| `/usr/local/lib/libeverlive-cam-v4l2.so` | `/dev/video65` | Webcam bridge (`everlive_camd`) |

Example:

```bash
LD_PRELOAD=/usr/local/lib/libgc573-v4l2.so obs
# Combine card + webcam bridge in one process:
LD_PRELOAD=/usr/local/lib/libgc573-v4l2.so:/usr/local/lib/libeverlive-cam-v4l2.so obs
```

Use `sudo ./install.sh --with-preload` or **System → App Camera Shortcuts** in the Console Hub to create reversible `<App> (EverLive Camera)` menu launchers for native Discord, OBS, Chrome, Chromium, Brave, Firefox, and Zoom (Flatpak/Snap sandboxes cannot load `LD_PRELOAD`). Enable **Background Service** in the Hub to keep the frame store active when the Hub window is closed.

---

## 3. Studio / Headless Mode

If you play on a TV/monitor connected to the card's HDMI OUT passthrough, click **Start Studio / Headless (OBS)** in the Hub (or run `./play.sh --loopback --headless`) to stream to OBS and audio without opening a desktop video window.

---

## 4. Direct Wayland Window Capture (PipeWire)

Capture the interactive EverLive window directly in OBS via **Window Capture (PipeWire)** without enabling any virtual camera.

---

## Webcam Picture-in-Picture (`everlive_camd`)

Enable the **Picture-in-Picture** button in the Hub header (or **Capture → Picture-in-Picture**) to overlay a webcam in the Hub preview and in **Record** takes (`ffmpeg -filter_complex overlay`). `everlive_camd` opens the physical webcam once (YUYV, NV12, or MJPEG) and publishes it to `/dev/shm/everlive_cam_v4l2_shm` so the Hub, recorder, and `LD_PRELOAD` OBS/Discord clients can share it simultaneously. Standalone CLI:

```bash
everlive_camd --list
everlive_camd --device /dev/video0 --size 1280x720 --fps 60
```

Log file: `~/.config/everlive/webcam.log`. Standalone `everlive_camd` and `libeverlive-cam-v4l2.so` are installed system-wide by `sudo make install` (or run directly from `tools/everlive-cam/`).
