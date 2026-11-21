<!-- SPDX-License-Identifier: GPL-2.0-only -->

# EverLive HWS UHD userspace engine (`hwsuhd`)

Userspace VFIO support for the HWS UHD capture family:

| Card | PCI ID | Subsystem |
|---|---|---|
| ACASIS VS2583 4K60 | `8888:8581` | `8888:0007` |
| AVMatrix VC12-4K | `8888:8581` | `8888:0007` |
| Siblings | `8888:85A1`, `8888:8591`, `1F33:8581` | `0007` |

One HDMI input, up to 4096×2160 at 60 Hz or 1920×1080 at 240 Hz, YUYV 4:2:2,
48 kHz stereo L-PCM audio, hardwired HDMI loop-out. The host has no access to
EDID, InfoFrames, HDR metadata or LEDs on these cards.

The engine is self-contained: it shares only `libeverlive-core` (frame store,
JSON-RPC helpers, socket path, V4L2 preload engine) with the AVerMedia
engines, and EverLive selects it only with `EVERLIVE_CARD=hwsuhd` or when no
AVerMedia card is installed. GC573/GC570D defaults are unchanged.

| Binary | Purpose |
|---|---|
| `hws_user_streamer` | the engine: SDL window or headless, v4l2loopback, frame store, PipeWire audio tap, control socket |
| `libhwsuhd-v4l2.so` | `LD_PRELOAD` camera `/dev/video64` for OBS, Discord, browsers |
| `hws_user_probe` | bring-up and diagnostics (frame/WAV dumps, `--regs`) |
| `hws-vfio-bind.sh` | runtime bind/restore of this card only |
| `99-hwsuhd-vfio.rules` | optional: bind at boot (`make install-system`) |

## Hardware model

- Artix-7 FPGA behind a Xilinx AXI-PCIe bridge (Gen2 x4), 64 KiB BAR0, MSI.
- A frame arrives as four *quarters*. Quarter *q* is written through
  card-side window *q*, which bridge translation entry *q* maps onto a
  512 MiB-aligned host range. Under VFIO all four entries point at one IOVA
  window (`0x20000000`), and the quarters of a frame are laid out back to
  back, so a completed frame is one contiguous YUYV image (zero copy).
- Quarter address registers are latched per frame. The engine rotates frame
  slots by repointing quarters 0/1 after the top-half interrupt and 2/3 after
  the bottom-half one. A sentinel word at the end of each quarter detects a
  quarter the card did not write.
- HDMI audio: two 4 KiB packets (1024 stereo frames, 21.3 ms) after the frame
  slots; the toggle register names the packet being filled.
- The input size/rate registers are only live while the capture core runs, so
  the core is started at open, and stopping capture only disables the video
  and audio engines.

## Power and reset (important)

Opening the card must not reset or power-cycle it, or the HDMI path restarts:
the signal drops for about 3 s and the source renegotiates its mode (an
upstream scaler may come back at a different resolution).

- `vfio-pci` runtime-suspends an idle device, and this card then enters
  **D3cold**: power is cut and the FPGA reloads on the next open. Keep it in
  D0 with `power/control=on`.
- Its only PCI reset method is a secondary bus reset, which `vfio-pci` would
  issue on open/close. Disable it with an empty `reset_method`.

`hws-vfio-bind.sh --bind` applies both; `--restore` and a reboot undo them.

## Build and run

```sh
make hwsuhd                       # from the repo root (core + this engine)

sudo SUDO_USER=$USER tools/openhws-userd/hws-vfio-bind.sh --bind

EVERLIVE_CARD=hwsuhd ./play.sh    # Console Hub on this card
EVERLIVE_CARD=hwsuhd ./play.sh --cli --v4l2 /dev/video10   # engine directly
tools/openhws-userd/hws_user_streamer --headless --v4l2 /dev/video10

sudo tools/openhws-userd/hws-vfio-bind.sh --restore    # back to the previous driver
```

Without an AVerMedia card in the machine `EVERLIVE_CARD` is not needed. With
more than one supported card, the Console Hub's card selector switches between
them and remembers the choice; `EVERLIVE_CARD_PCI=0000:84:00.0` pins a card by
address. `everlive-service.sh` follows the same rules.

To bind at every boot, install the engine and the udev rule:

```sh
sudo make -C tools/openhws-userd install   # /usr/local/{bin,lib,libexec} + rule
```

The rule matches subsystem `0x0007` only, sets `driver_override=vfio-pci`
before the kernel driver loads, keeps the card in D0, disables its bus reset
and gives `/dev/vfio/<group>` to group `video`.

### Engine

`hws_user_streamer` accepts the GC573 engine's options, so the Hub launches it
the same way. Options for hardware this card does not have (`--led`, `--edid`,
`--framegen`, `--fg-*`, `--no-hdr`, `--hdr-exposure`, ...) are accepted and
ignored. `--help` lists the rest.

- Capture thread: services MSI, rotates five zero-copy frame slots and follows
  the input every 100 ms (mode change or a 1 s stall restarts video; signal
  loss stops it and shows the standby card).
- The window and the output thread take the newest frame by reference; a slot
  a consumer still holds is never handed back to the card.
- v4l2loopback: native size by default; if a reader such as OBS already holds
  a format, the engine adopts it and scales.
- Control socket (`$XDG_RUNTIME_DIR/everlive.sock`, `--ipc-socket`):
  `hello`, `ping`, `status.get`, `audio.volume.set`, `audio.mute.set`,
  `audio.channels.set` (stereo only), `osd.set`, `osd.position.set`,
  `osd.mode.set` (auto/compact/card), `record.state`, `aspect.set`,
  `scale.set`, `mode.set` (gaming/headless, `parent_winid`), `v4l2.set`,
  `toast.show`. Other methods return `-32601`. One EverLive engine
  can own the default socket at a time; use `--ipc-socket` to run this one
  next to another card's engine.
- Hotkeys: `f` fullscreen, `a` aspect, `l` scaling, Up/Down volume, `m` mute,
  `i`/`Tab` stats, `Alt`+`Z` Quick Navigation, `p` OSD top/bottom, `s` streamer mode, `F1` help, `q` quit.

### Probe

```sh
tools/openhws-userd/hws_user_probe --seconds 15 --frame /tmp/frame --wav /tmp/audio.wav
```

`hws_user_probe --regs` prints identity, PCIe link and input registers
without starting capture. The probe needs `cap_ipc_lock` or a memlock limit of
about 70 MiB for three frame slots; `--bind` sets the capability on the
binary.

## Measured (FPGA 122, PCIe 5 GT/s x4)

| Input | Result |
|---|---|
| 3840×2160p60 (PS3 via upscaler) | 60.00 fps, 0 lost/torn over 15 s; capture starts instantly with D0 kept |
| HDMI audio | 48 kHz stereo, continuous across packet boundaries |
| Interrupts | MSI via VFIO eventfd, ~0 spurious |
| Engine, headless + v4l2loopback | 3840×2160 at 60.00 fps, 0 dropped; V4L2 node 3840×2160 |
| Engine, SDL window | 60.00 fps, 0 dropped, IPC aspect/toast live |
| `/dev/video64` preload | streams via `v4l2-ctl --stream-mmap` |

Not measured yet: 1080p240, 1440p, signal loss/mode change on live hardware,
Console Hub embedding (`--parent-winid`).
