# EverLive GC575 user-space tools

The AVerMedia Live Gamer 4K 2.1 (GC575) is sold as a PCIe Gen 3 x4 card, but
its **capture and audio function enumerates as a USB 3.2 UVC device**:

| Property | Value |
|---|---|
| USB ID | `07ca:0575` |
| USB class | `0e/01/00` (Video, Video Control, UVC 1.0) |
| Kernel driver | `uvcvideo` (video), `snd_usb_audio` (audio) |
| Video node | `/dev/videoN`, bulk IN endpoint `0x82` |
| Private XU | entity/unit 4, GUID `961073c7-49f7-44f2-ab42-e940405940c2` |
| XU selector 2 | little-endian pending request/response length |
| XU selector 1 | AT request/response bytes |

There is no native PCIe video function to bind to `vfio-pci`. **Do not use the
GC573 VFIO engine for this card.** The upstream UVC driver already handles the
stream; EverLive only needs a UVC/V4L2 backend and, for vendor features, the
private XU control channel.

## Build targets

```sh
make            # builds gc575ctl, opengc575-userd and libgc575-v4l2.so
sudo make install
```

## `gc575ctl`

`gc575ctl` talks to the XU through `uvcvideo`'s `UVCIOC_CTRL_QUERY` ioctl, or
through raw `usbfs` control transfers when `-D` is supplied. It is a diagnostic
and profile-selection tool, not a capture engine.

```sh
make gc575ctl
sudo install -m 0755 gc575ctl /usr/local/bin/
```

List the UVC modes exposed by the kernel:

```sh
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --list-formats-ext
```

Query the device and the active descriptor profile:

```sh
gc575ctl -d /dev/video0 info          # AT23, expects DB2059#1605
gc575ctl -d /dev/video0 get-index     # ATb1, active profile (0 or 1)
```

Switch to the high-refresh descriptor profile:

```sh
gc575ctl -d /dev/video0 set-index 1   # 4K144 / 1440p144 MJPEG profile
gc575ctl -d /dev/video0 set-index 0   # standard 4K60 profile
```

The profile switch is a runtime AT command (`ATb0`, payload
`01 00 00 00 <index LE32>`). It is **not** a firmware or descriptor burn. The
device may re-enumerate; if it does, wait for `/dev/videoN` to reappear and
re-run `v4l2-ctl`.

If `UVCIOC_CTRL_QUERY` is rejected by the running kernel, use the raw backend:

```sh
# Find the USB bus/device path first (lsusb -d 07ca:0575, or /sys/bus/usb/devices)
sudo gc575ctl -D /dev/bus/usb/001/013 info
```

The raw backend needs write access to the USB device node. Prefer the UVC
backend (video group) for normal use.

## Safety boundaries

- The normal capture path uses standard V4L2 ioctls and read-only AT queries.
  It never writes flash, firmware, descriptors or EDID.
- `set-index` is restricted to `0` and `1`; these are runtime descriptor
  selections, not a flash operation.
- `gc575ctl raw ...` requires `-U` and can send arbitrary vendor AT commands.
  Treat it as unsafe: an unknown command may change EDID, MCU state or other
  persistent settings even though it is not a full firmware flash.
- The daemon only sends `AT b0` when `--profile` is explicitly requested.
- Nothing in this directory binds a PCIe function to VFIO or claims the USB
  interface away from `uvcvideo`.
- If the device stops responding during an experiment: stop streaming, unplug
  and replug the card, then select profile `0`. The Windows firmware updater
  and its rescue image remain the recovery path.

## Modes present in the firmware / live descriptors

The GC575 firmware contains two descriptor profiles per USB speed. Profile 0 is
the standard profile; profile 1 is the high-refresh profile used by AVerMedia
Streaming Center for 4K144/4K120 capture.

| Format | Profile 0 | Profile 1 |
|---|---|---|
| NV12 1920×1080 | up to 240 fps | up to 240 fps |
| NV12 2560×1440 | up to 144 fps | up to 144 fps |
| NV12 3840×2160 | up to 60 fps | up to 60 fps |
| YUY2 1920×1080 | up to 120 fps | up to 120 fps (240 only after a Windows descriptor burn) |
| YUY2 3840×2160 | up to 50 fps | up to 50 fps |
| MJPEG 1920×1080 | up to 240 fps | up to 240 fps |
| MJPEG 2560×1440 | up to 144 fps | up to 144 fps |
| MJPEG 3440×1440 | up to 60 fps | up to 144 fps |
| MJPEG 3840×2160 | up to 60 fps | up to 144 fps |
| RGB24 / P010 | resolution-dependent | resolution-dependent |

Consequences:

- **1080p240 does not require a descriptor burn.** Use NV12 or MJPEG.
- YUY2 1080p240 is the only mode that the historical Windows burn unlocked.
  It is also the most bandwidth-hungry mode (~7.96 Gbps) and measured below
  240 unique frames/s in the research. Prefer NV12.
- 4K144 capture is exposed by profile 1 as MJPEG. Select profile 1 via
  `gc575ctl set-index 1`; do not flash the card.
- The descriptors under-report `dwMaxBitRate` for high-rate modes because the
  nominal rate exceeds the 32-bit UVC field. Some applications may hide a mode
  even though `v4l2-ctl` lists it. Use `ffmpeg` or `v4l2-ctl` to verify.

## `opengc575-userd` (experimental UVC engine)

The daemon does **not** claim a PCIe function, does **not** use VFIO and does
**not** take the USB interface away from `uvcvideo`. It opens the existing UVC
node, converts NV12/YUY2/MJPEG frames to YUY2 and publishes them to
`/gc575_v4l2_shm` for the preload DSO.

```sh
# Discover the UVC node and list its modes
./opengc575-userd --list

# 1080p240 NV12, publish to the framestore
./opengc575-userd --format nv12 --size 1920x1080 --fps 240

# Select profile 1 (4K144 MJPEG), then capture 4K144 MJPEG
./opengc575-userd --profile 1 --format mjpeg --size 3840x2160 --fps 144

# Use the kernel's current format/interval
./opengc575-userd --format auto

# Capture without a framestore (diagnostics only)
./opengc575-userd --no-camera --frames 240
```

Options:

```text
-d, --device PATH     V4L2 node (default: auto-detect 07ca:0575)
-f, --format FMT      nv12 | yuyv | mjpeg | auto
-s, --size WxH        requested size
-r, --fps N           requested frame rate
-p, --profile 0|1     select USB descriptor profile via AT b0
-l, --list            list modes
-n, --no-camera       do not create the framestore
-N, --frames N        stop after N frames
-t, --timeout MS      dequeue timeout
-v, --verbose         per-frame logging
```

When built with libavcodec, MJPEG frames are decoded to YUY2. Without
libavcodec, NV12 and YUY2 still work and MJPEG is rejected with `-ENOTSUP`.

### Preload virtual camera (`/dev/video64`)

```sh
LD_PRELOAD=/usr/local/lib/libgc575-v4l2.so obs
LD_PRELOAD=/usr/local/lib/libgc575-v4l2.so ffplay /dev/video64
```

The preload node uses the shared EverLive V4L2 engine and the framestore
contract. It is separate from the real uvcvideo node; OBS can also open the
real `/dev/videoN` directly.

### End-to-end reference launcher

```sh
# 1080p240 NV12 via the daemon + /dev/video64 preload
PROFILE=0 MODE=nv12 SIZE=1920x1080 FPS=240 ./play-gc575.sh

# 4K144 MJPEG via profile 1 (MJPEG is decoded in the daemon)
PROFILE=1 MODE=mjpeg SIZE=3840x2160 FPS=144 ./play-gc575.sh

# No preload: open the real UVC node directly
NO_PRELOAD=1 DEV=/dev/video0 MODE=nv12 SIZE=1920x1080 FPS=240 ./play-gc575.sh
```

The script starts `opengc575-userd`, waits for the framestore, then launches
`everlive_display` against `/dev/video64` (or directly against `$DEV`). It is a
bring-up/reference path, not the final EverLive launcher integration.

### Native renderer (UVC fallback)

`everlive_display` now falls back to plain V4L2 when
`VIDIOC_QUERY_DV_TIMINGS` is not supported (UVC devices):

```sh
./everlive_display -d /dev/videoN -F nv12 --size 1920x1080 --fps 240
```

For the preload node use `-F yuyv` instead, because the framestore publishes
YUY2.

## Capture examples

```sh
# 1080p240 uncompressed NV12 (about 5.97 Gbps payload)
v4l2-ctl -d /dev/video0 \
  --set-fmt-video=width=1920,height=1080,pixelformat=NV12 \
  --set-parm=240

ffmpeg -f v4l2 -input_format nv12 -video_size 1920x1080 -framerate 240 \
  -i /dev/video0 -c:v libx264 -preset ultrafast -f null -

# 1080p240 MJPEG
ffmpeg -f v4l2 -input_format mjpeg -video_size 1920x1080 -framerate 240 \
  -i /dev/video0 -c:v copy -f matroska /tmp/gc575-1080p240.mkv

# 4K144 MJPEG after selecting profile 1
gc575ctl -d /dev/video0 set-index 1
v4l2-ctl -d /dev/video0 --list-formats-ext
ffmpeg -f v4l2 -input_format mjpeg -video_size 3840x2160 -framerate 144 \
  -i /dev/video0 -c:v copy -f matroska /tmp/gc575-4k144.mkv
```

Check the USB link speed first. A 5 Gbps port cannot carry uncompressed
1080p240; the card must be on a USB 3.2 Gen 2 (10 Gbps) link. `lsusb -t` shows
`5000M` or `10000M`.

## Integration notes for EverLive

The existing GC573/GC570D engines are VFIO-based and own a PCIe function. The
GC575 path is different:

1. **Discovery:** match USB `07ca:0575` on `/sys/bus/usb/devices`, not a PCIe
   BDF. `libeverlive-core` now has `everlive_match_card_bus()` and
   `everlive_detect_installed_usb_cards()` for this.
2. **Capture:** open the `uvcvideo` node directly. The EverLive display already
   accepts NV12 and YUY2, so a first integration can pass `-F nv12 -d
   /dev/videoN` without a daemon.
3. **Virtual camera / preload:** if a stable YUY2 frame store is required,
   capture NV12 (or decode MJPEG with libavcodec), convert to YUY2, and publish
   to `/gc575_v4l2_shm`; expose it as `/dev/video64` with a preload descriptor.
   Do not publish raw MJPEG.
4. **Audio:** `snd_usb_audio` provides ALSA/PipeWire nodes. LPCM stereo works
   without vendor commands. 5.1/non-PCM requires the vendor AT94/AT95 path and
   is not part of this tool yet.
5. **Vendor features:** profile selection is implemented. HDMI timing, format,
   VRR/color and EDID commands exist in the AT catalog but are not validated on
   Linux; keep them behind feature probes.

## Why not burn the descriptor from Linux

The Windows updater exposes `burnUSBDesription(path, speed, index)` in
`RTK_IO_x86.dll`. It writes a 4096-byte descriptor profile and can leave the
card on profile 0 after the burn. The only gain over the stock firmware is YUY2
1080p240 (and historical MJPEG 1440p240 experiments). That is not worth the
brick/recovery risk for a Linux capture path that can use NV12 or MJPEG. Use
the Windows updater if a burn is ever needed, and keep profile 0 as fallback.

## Status

- Static analysis and descriptor extraction: complete.
- Linux XU transport: implemented, compile-tested, not hardware-tested.
- UVC capture: expected to work through upstream `uvcvideo`; no special quirk
  for `07ca:0575` is present in the current mainline driver.
- `opengc575-userd`: implemented and compile-tested. It has not been run
  against physical GC575 hardware. The theoretical path is: uvcvideo node →
  V4L2 MMAP capture → YUY2 conversion/decode → `/gc575_v4l2_shm` →
  `/dev/video64`.
- `everlive_display` UVC fallback: implemented and compile-tested, not
  hardware-tested.
- Audio 5.1/non-PCM, EDID, HDR metadata and LED: not implemented.

## Theoretical data path

```text
HDMI source
  → GC575 PCIe card
    → on-board USB controller
      → uvcvideo /dev/videoN (NV12, YUY2, MJPEG)
        → opengc575-userd
          → NV12→YUY2 or MJPEG decode
            → /gc575_v4l2_shm (YUY2 framestore)
              → libgc575-v4l2.so → /dev/video64
                → OBS / Discord / FFmpeg / everlive_display

Alternative, no daemon:
  uvcvideo /dev/videoN → everlive_display -F nv12 --fps 240
```
