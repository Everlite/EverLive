# GC575 in EverLive (Experimental Branch)

> [!NOTE]
> **Branch Context: AVerMedia Live Gamer 4K 2.1 (GC575) Research & Implementation**  
> This branch implements experimental Linux userspace support for the AVerMedia GC575 based on community reverse-engineering findings.
> For the production EverLive capture suite (GC573 & GC570D), switch to the [`dev`](https://github.com/Everlite/EverLive/tree/dev) or [`main`](https://github.com/Everlite/EverLive/tree/main) branch.

Short integration notes for the **AVerMedia Live Gamer 4K 2.1 (GC575)**.
For the full reverse-engineering report see [`docs/GC575-LINUX-INTEGRATION.md`](docs/GC575-LINUX-INTEGRATION.md); for tool details see [`tools/opengc575-userd/README.md`](tools/opengc575-userd/README.md).

## What the card actually is

The GC575 is a PCIe Gen 3 x4 card, but its **capture and audio function enumerates as a USB 3.2 UVC device**:

| Property | Value |
|---|---|
| USB ID | `07ca:0575` |
| USB class | `0e/01/00` (UVC) |
| Kernel drivers | `uvcvideo` (video), `snd_usb_audio` (audio) |
| Video node | `/dev/videoN` |
| Private XU | entity 4, GUID `961073c7-49f7-44f2-ab42-e940405940c2` |

There is **no native PCIe video function to bind to VFIO**. EverLive must use the UVC/V4L2 path, not the GC573 VFIO engine. The card should stay bound to `uvcvideo`.

## Current status

| Part | State |
|---|---|
| USB discovery in `libeverlive-core` | implemented |
| `gc575ctl` (XU/AT diagnostics, profile select) | implemented, compile-tested |
| `opengc575-userd` (UVC capture → YUY2 framestore) | implemented, compile-tested |
| `libgc575-v4l2.so` (`/dev/video64` preload) | implemented, compile-tested |
| `everlive_display` UVC fallback | implemented, compile-tested |
| Hardware validation | **not done** (no physical card available) |
| Audio 5.1/non-PCM, EDID, HDR metadata, LED | not implemented |

## Data path

```text
HDMI source
  → GC575
    → internal USB controller
      → uvcvideo /dev/videoN  (NV12 / YUY2 / MJPEG)
        → opengc575-userd
          → NV12→YUY2 or MJPEG decode
            → /gc575_v4l2_shm  (YUY2 framestore)
              → libgc575-v4l2.so → /dev/video64
                → OBS / Discord / FFmpeg / everlive_display

Direct path without the daemon:
  uvcvideo /dev/videoN → everlive_display -F nv12 --fps 240
```

## Build

```sh
make -j"$(nproc)"
make test
```

The GC575 parts are built by the `gc575` target:

```sh
make gc575
```

## Try it

```sh
# List the UVC modes the kernel exposes
tools/opengc575-userd/opengc575-userd --list

# 1080p240 NV12 through the daemon + /dev/video64 preload
PROFILE=0 MODE=nv12 SIZE=1920x1080 FPS=240 \
  tools/opengc575-userd/play-gc575.sh

# 4K144 MJPEG through profile 1 (decoded in the daemon)
PROFILE=1 MODE=mjpeg SIZE=3840x2160 FPS=144 \
  tools/opengc575-userd/play-gc575.sh

# No preload: open the real UVC node directly
NO_PRELOAD=1 DEV=/dev/video0 MODE=nv12 SIZE=1920x1080 FPS=240 \
  tools/opengc575-userd/play-gc575.sh
```

Useful checks:

```sh
lsusb -d 07ca:0575
lsusb -t                      # must show 10000M for uncompressed 240 fps
v4l2-ctl --list-devices
v4l2-ctl -d /dev/videoN --list-formats-ext
```

## Capture modes

The stock firmware has two descriptor profiles:

| Mode | Profile 0 | Profile 1 |
|---|---|---|
| NV12 1080p240 | yes | yes |
| NV12 1440p144 | yes | yes |
| MJPEG 1080p240 | yes | yes |
| MJPEG 1440p144 | yes | yes |
| MJPEG 4K144 | no (4K60) | yes |
| YUY2 1080p240 | no (120 fps) | no (120 fps) |

- **1080p240 needs no flash.** Use NV12 or MJPEG.
- **Profile 1 enables 4K144 MJPEG.** Select it with `gc575ctl -d /dev/videoN set-index 1` or `opengc575-userd --profile 1`.
- The historical YUY2-1080p240 descriptor burn is not needed for Linux and should not be attempted from this project.
- Use a **10 Gbps USB 3.2 Gen 2** link. A 5 Gbps link lacks uncompressed 1080p240 and 1440p144; USB 2.0 exposes only small modes.

## Linux requirements

- `uvcvideo` and `snd_usb_audio` (standard upstream drivers)
- No IOMMU, VFIO or vendor driver required
- User in the `video` group for the UVC node
- For `/dev/video64`: `LD_PRELOAD=.../libgc575-v4l2.so` in the consuming app
- `gc575ctl` and `opengc575-userd` are optional diagnostics/engine tools

## Safety boundaries

- The normal path uses standard V4L2 ioctls and read-only AT queries. It does not write flash, firmware, USB descriptors or EDID.
- `set-index` accepts only `0` and `1`; these are runtime profile selections.
- `gc575ctl raw` requires `-U` and can send arbitrary vendor AT commands. Treat it as unsafe; unknown commands may change persistent device state.
- No VFIO bind, no PCIe configuration writes, no USB interface takeover.
- Recovery if an experiment leaves the device unresponsive: stop streaming, unplug/replug, select profile `0`, or use the Windows firmware updater and its rescue image.

## What is intentionally not done

- No descriptor burn, no firmware flash, no VFIO takeover.
- No raw MJPEG publishing into the framestore; the contract is YUY2.
- No claims of hardware validation; all current results are static analysis, descriptor parsing and compile tests.
- No automated recovery or flashing from Linux.
