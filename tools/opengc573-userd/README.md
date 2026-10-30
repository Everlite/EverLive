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
| **`gc573_user_edid.[ch]`** | Hardware EDID profile generator (4K60, 1440p60/120 SDR & HDR, 1080p240) and I2C programmer. |
| **`gc573_user_hdr.[ch]`** | Real-time HDR10 (PQ) & HLG to SDR Hable filmic tonemapper and BT.2020-to-BT.709 chroma matrix. |
| **`gc573_user_ipc.[ch]`** | Lock-free JSON-RPC 2.0 Unix control socket (`everlive.sock`) for zero-interrupt runtime mutations. |
| **`gc573_user_audio_tap.[ch]`** | In-process PipeWire virtual audio tap (`EverLive Capture Audio`) for isolated OBS recording. |
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
  --ipc-socket PATH    Control socket for the Control Hub (default:
                       $XDG_RUNTIME_DIR/everlive.sock)
  --no-ipc             Do not open the control socket
  --audio-tap [NAME]   Mirror the mixed audio into a PipeWire sink
                       (default: everlive-capture-audio)
  --no-hdr             Disable HDR10/HLG -> SDR tonemapping (bypass)
  --hdr-exposure EV    Exposure bias for the tonemapper (-2.0 .. +2.0)
```

Game-window hotkeys: <kbd>f</kbd>/<kbd>F11</kbd> fullscreen, <kbd>h</kbd> HDR tonemapper, <kbd>a</kbd> aspect ratio,
<kbd>p</kbd> OSD position, <kbd>o</kbd>/<kbd>i</kbd>/<kbd>Tab</kbd> stats overlay,
<kbd>s</kbd>/<kbd>F10</kbd> overlay + cursor, <kbd>Esc</kbd>/<kbd>q</kbd> quit.

## Remote Control Socket

The engine owns the VFIO container and BAR0 mapping exclusively, so a second
process can never open the card. Instead of failing with "an external instance
already owns the device", the streamer publishes its state over a Unix domain
socket and the Control Hub attaches to it as a read-only **Remote Session**.

- Path: `$XDG_RUNTIME_DIR/everlive.sock` (fallback `/run/user/<uid>`, then
  `/tmp/everlive-<uid>`), mode `0600`.
- Auth: `SO_PEERCRED` — only clients with the streamer's own UID are accepted.
- Ownership: an `flock` on `everlive.sock.lock` is authoritative. A socket left
  behind by a crash is detected and removed on the next start.
- Transport: newline-delimited JSON-RPC 2.0, max 64 KiB per line.

Methods: `hello`, `ping`, `status.get`, and the write path
`led.set`, `led.mode.set`, `audio.volume.set`, `audio.mute.set`, `osd.set`,
`osd.position.set`, `aspect.set`, `mode.set`, `v4l2.set`, `hdr.set`.
Mutations are queued into a lock-free ring and applied by the render thread, so
they never touch the DMA path; the audio gain uses a 20 ms linear ramp instead
of pausing the device. The snapshot is published once per second from the
render loop through a seqlock, so remote queries never touch the DMA path.

```console
$ printf '{"jsonrpc":"2.0","id":1,"method":"status.get"}\n' \
    | socat - UNIX-CONNECT:"$XDG_RUNTIME_DIR/everlive.sock"
{"jsonrpc":"2.0","id":1,"result":{"proto":1,"version":"1.3.0",...}}
```

## HDR10 / HLG to SDR

The IT6805 DRM InfoFrame (packet `0x87`, bank 2, `0x24..0x42`) is decoded to
`hdr_present`, `eotf` (0 SDR, 2 PQ, 3 HLG), `MaxCLL` and `MaxFALL`. It is read
on every full signal probe and once per second afterwards, so an HDR toggle at
an unchanged resolution is still noticed.

When PQ or HLG is detected the frame is tonemapped **in place** before it is
handed to SDL and to the V4L2 sink, so the gaming window and OBS show the same
picture:

- Luma: one 256-entry table per transfer function, built on demand. It folds
  PQ/HLG EOTF, a John Hable filmic curve calibrated to ITU-R BT.2408 diffuse
  white (`203 cd/m²`, `white = 11.2`), the BT.709 OETF and limited-range
  scaling (`16..235`) into a single byte lookup.
- Chroma: a constant 2x2 matrix derived from BT.2020 -> BT.709, applied to the
  centred Cb/Cr values (`HDR_SATURATION = 1.15`).

SDR signals stay on the zero-copy path, so the pass costs nothing when it is
not needed. Hotkey <kbd>h</kbd> toggles the tonemapper, `--no-hdr` disables it,
`--hdr-exposure EV` biases the curve.

Measured throughput on the development machine: **~4.2 ms per 1440p frame**
(7.2 MB YUYV, ~1.7 GB/s). The pass is memory-bandwidth bound, not compute
bound; 4K scales to roughly 2.5x. The <1 ms target is not reachable for a
full-frame CPU pass at 4K - that would need a GPU render pass plus a readback
for the V4L2 feed, which is more expensive overall.

## Live Virtual Camera

`v4l2.set {"enabled": true, "device": "/dev/video10"}` opens or closes the
v4l2loopback writer on a running engine:

```console
$ printf '{"jsonrpc":"2.0","id":1,"method":"v4l2.set","params":{"enabled":true}}\n' \
    | socat - UNIX-CONNECT:"$XDG_RUNTIME_DIR/everlive.sock"
{"jsonrpc":"2.0","id":1,"result":{"ok":true}}
```

Only the loopback fd is opened or closed. The DMA ring, the gaming window, the
audio tap and the VFIO container keep running. When a signal is already locked,
the sink is negotiated immediately so the next frame reaches OBS without
waiting for the writer's retry interval. `device` is optional and must be an
absolute `/dev/` path; it defaults to `/dev/video10`.

## Live Mode Switch

`mode.set {"mode": "gaming"|"headless"}` swaps the presentation layer of a
running engine:

```console
$ printf '{"jsonrpc":"2.0","id":1,"method":"mode.set","params":{"mode":"gaming"}}\n' \
    | socat - UNIX-CONNECT:"$XDG_RUNTIME_DIR/everlive.sock"
{"jsonrpc":"2.0","id":1,"result":{"ok":true}}
```

Only the SDL window/renderer/texture are created or destroyed. The VFIO
container, the DMA ring, the V4L2 loopback writer and the audio path keep
running, so an OBS recording survives the switch. The SDL video subsystem is
initialised on demand (important for a process started with `--headless`) and
is never torn down once it is up.

The command is acknowledged as soon as it is queued, before the switch is
applied - the render loop performs it at the top of its next iteration.

## Virtual Audio Track (OBS)

`--audio-tap [NAME]` mirrors exactly what the streamer plays (L-PCM after the
volume ramp, or the decoded AC-3/DTS stereo downmix) into a dedicated PipeWire
sink, so OBS can record it as its own track while the desktop mix stays
untouched.

```console
$ ./gc573_user_streamer --audio-tap --headless --v4l2 /dev/video10
```

In OBS add **Audio Input Capture (PipeWire) → Monitor of EverLive Capture
Audio**.

The sink is normally created by a config drop-in that `install.sh` writes to
`/etc/pipewire/pipewire.conf.d/50-everlive-capture.conf`. If the node is
missing, the streamer asks the daemon for it at runtime via `pw-cli
load-module` (no shell involved) and logs a hint when that fails.

Implementation notes:

- A lock-free SPSC ring (64 KiB) connects the audio worker to the PipeWire RT
  thread; the audio path never blocks on the graph.
- The tap always publishes **S16_LE stereo**. 24-bit HDMI carriers arrive as
  left-aligned S32LSB and are re-quantised; the playback path is untouched.
- The stream is connected only once the HDMI sample rate is known, so a
  44.1 kHz source is never played at 48 kHz pitch.
- Muting or lowering the volume affects the tap identically - OBS records
  what the player hears, never raw compressed bytes.

---

## Custom EDID Requirements & Audio Unification

Custom EDIDs must be exactly **256 bytes** (base block + one CTA-861 extension) and pass the header/checksum check in `edid_validate()`.

By default EverLive **unifies the audio block** of every custom EDID: the CTA-861 Audio Data Block is rewritten to advertise L-PCM (8 ch), AC-3 5.1 and DTS 5.1. If the file has no audio block, a complete one is appended. Replacing an L-PCM-only block needs 6 spare bytes, appending one needs 10; the optional Video Capability Data Block is dropped first if that is the only way to retain all detailed timings.

If the extension block has no room at all, the unmodified file is used and the streamer logs:
`[EDID] Using raw audio block from '<path>' (unification bypassed: no space for SADs)`

Pass `--raw-edid` (alias `--no-unify-edid`) to load custom files **bit-exact** without any modification.
