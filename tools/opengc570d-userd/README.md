<!-- SPDX-License-Identifier: GPL-2.0-only -->

# EverLive GC570D (Live Gamer DUO) userspace engine

`opengc570d-userd` is EverLive's userspace VFIO-PCI engine for the AVerMedia
Live Gamer DUO (GC570D, PCI `1461:0054`, subsystem `1461:5700`). It owns BAR0,
the IT68051/IT6802 HDMI receivers, the VideoSplitter, the Xilinx HDR-to-SDR
preprocessor, both DMA video rings and both audio rings. No out-of-tree kernel
module is required beyond the distribution's `vfio-pci`.

The same process also runs the EverLive SDL2 presentation window and the
optional dual v4l2loopback writers. Frames travel from the capture threads to
the presentation layer through the POSIX shared-memory store
`/dev/shm/gc570d_v4l2_shm`, so OBS never blocks the DMA ring.

## Build

```sh
make -C tools/opengc570d-userd -j"$(nproc)"
```

Dependencies: a C11 compiler, `pkg-config`, SDL2 and PipeWire development
headers. Without SDL2 the daemon still builds as a headless capture engine;
without PipeWire the capture rings and control socket remain available.

## Permissions and interrupts

VFIO pins every DMA buffer, so the daemon needs `CAP_IPC_LOCK` on the binary
(`cap_ipc_lock=+ep`). The installers apply it with
`packaging/libexec/everlive-setcap`; no unlimited memlock limit is shipped.
If `setcap` is unavailable, the installer adds a bounded 1 GiB `@video`
fallback plus a bounded `LimitMEMLOCK=1G` user-unit drop-in. No root
privileges are needed once the capability is in place. Rebuilding with `make`
replaces the binary and clears its file capability, so re-apply
`cap_ipc_lock=+ep`.

VFIO interrupts are allocated MSI-first. INTx is only a fallback because its
mask/unmask path can stall the bridge I2C engine on some hosts. When INTx or
no VFIO eventfd is available, the IRQ thread polls the MMIO status register
every 2 ms.

## Health monitoring

The daemon reinitializes the capture hardware after a system sleep/resume or a
PCIe power reset. It also watches the per-input DMA completion heartbeats:
HDMI IN 1 uses the continuous DMA0 ring, while HDMI IN 2 is only checked while
VB2 has buffers queued. Five seconds without progress triggers the same
in-place reinitialization path. Recovery is capped at three consecutive
attempts, and the budget is restored only after ten minutes without a stall,
so a persistent fault cannot cause a reinit loop. Audio silence is never
treated as a fault.

Set `GC570D_DEBUG=1` to enable the kernel-parity `dev_dbg()` diagnostics
(I2C transactions, splitter negotiation details and DMA snapshots).

## Run

```sh
# Interactive window, HDMI IN 1, virtual camera on /dev/video10
./tools/opengc570d-userd/opengc570d-userd --sdl --v4l2 /dev/video10

# Headless Studio mode, both HDMI inputs published to OBS
./tools/opengc570d-userd/opengc570d-userd --headless \
    --v4l2 /dev/video10 --v4l2-2 /dev/video11

# Start on HDMI IN 2 with picture-in-picture of HDMI IN 1
./tools/opengc570d-userd/opengc570d-userd --sdl --port 2 --pip
```

The BDF may be passed positionally or with `--pci`. `--copy-edid` copies the
downstream display EDID; without it the internal 4K60 bridge EDID is used.

| Hotkey | Action |
| --- | --- |
| `1` / `2` | Select HDMI IN 1 / HDMI IN 2 |
| `P` | Toggle picture-in-picture of the other input |
| `L` | Toggle scaling mode (`fit` bilinear ↔ `integer` nearest-neighbor) |
| `A` | Cycle aspect ratio (`auto` → `16:9` → `4:3` → `stretch`) |
| `↑` / `↓` (or `+` / `-`) | Volume up / down (in 5% steps) |
| `M` | Toggle audio mute |
| `Tab` | Toggle the performance stats overlay |
| `Alt` + `Z` | Toggle the Quick Navigation bar |
| `C` | Toggle OSD top/bottom |
| `F` / `F11` | Toggle fullscreen |
| `Esc` / `Q` | Quit |

## Virtual cameras

The daemon's `--v4l2`/`--v4l2-2` writers target v4l2loopback nodes:
`/dev/video10` carries the selected HDMI input (HDMI 1 by default) and
`/dev/video11` carries HDMI IN 2. `play.sh` and `install.sh` configure
v4l2loopback with both nodes:

```sh
sudo modprobe v4l2loopback video_nr=10,11 \
    card_label="EverLive Capture,EverLive Capture HDMI 2" exclusive_caps=0
```

`libgc570d-v4l2.so` is a separate, optional `LD_PRELOAD` bridge that exposes
the same two feeds as `/dev/video60` (HDMI 1) and `/dev/video61` (HDMI 2) to
applications that do not use the loopback nodes. It reads the shared-memory
frame store directly and does not replace the v4l2loopback path.

> For a new capture card, see
> [`docs/ADDING_A_NEW_CARD.md`](../../docs/ADDING_A_NEW_CARD.md) §3.4, §4.3 and
> §6.5 for the shared frame-store and preload-descriptor contract.

## Control socket

The daemon serves the unified newline-delimited JSON-RPC 2.0 protocol on
`$XDG_RUNTIME_DIR/everlive.sock` (used by `everlive-ctl` and the Console Hub)
as well as the legacy text protocol on `$XDG_RUNTIME_DIR/gc570d/control.sock`
used by `gc570d-ctl`. When `XDG_RUNTIME_DIR` is unset, the same private
`/run/user/<uid>/` or `/tmp/gc570d-<uid>/` fallback rules apply to both sides.

```sh
gc570d-ctl status
gc570d-ctl input 2                 # switch the window to HDMI IN 2
gc570d-ctl osd 0                  # hide the overlay
gc570d-ctl pip 1                  # enable picture-in-picture
gc570d-ctl led colorful            # colorful | breathing | static | off
```

Both sockets only accept clients with the daemon's own UID. The runtime
directory must be a real `0700` directory owned by that UID; a foreign or
permissive path aborts daemon startup instead of being reused.

The raw diagnostic commands `reinit` and `action <node>` are debug-gated. They
require `EVERLIVE_DEBUG_CONTROL=1` in the **daemon** environment:

```sh
systemctl --user set-environment EVERLIVE_DEBUG_CONTROL=1
systemctl --user restart opengc570d-userd.service

gc570d-ctl reinit
gc570d-ctl action splitter_windows_bridge_edid 1
gc570d-ctl action it68051_hpd 1   # pulse HDMI IN 1 hot-plug
```

Normal controls stay available without the debug flag. The Hub uses the
JSON-RPC feature methods (`hotplug`, `edid.set`, `led.mode.set`, and so on) for
its DUO controls. On resume, `gc570d-sleep.sh` restarts the user unit rather
than issuing the gated `reinit` command.

The unified socket also accepts `mode.set`, which swaps only the presentation
layer.  The Console Hub uses it to dock a headless session into its viewport
without releasing VFIO, interrupting the DMA rings or pausing OBS audio/video:

```sh
everlive-ctl runtime mode headless
everlive-ctl runtime mode gaming "$WINDOW_ID"   # 0 = standalone window
```

## Installation

`make install` installs the daemon, `gc570d-ctl`, the optional preload bridge,
the subsystem-qualified udev rule, the shared VFIO bind helper and a per-user
systemd unit. `PREFIX` defaults to `/usr/local`; the udev rule and sleep hook
are rewritten to match the chosen prefix. No unlimited memlock file is
installed; `cap_ipc_lock=+ep` is applied to the daemon.

```sh
sudo make -C tools/opengc570d-userd install
sudo udevadm control --reload-rules
sudo udevadm trigger
sudo usermod -aG video "$USER"   # log out and back in afterwards
systemctl --user daemon-reload
systemctl --user enable --now opengc570d-userd.service
```

The unit starts the engine headless and publishes `/dev/video10`. For the
optional `LD_PRELOAD` bridge, run the consuming application with:

```sh
LD_PRELOAD=/usr/local/lib/libgc570d-v4l2.so obs
```

## Audio

HDMI IN 1 accepts stereo and 5.1/7.1 LPCM sources. The HDMI OUT passthrough
keeps the source layout, but the capture side of both inputs is intentionally
stereo: ALSA and PipeWire expose two channels (FL/FR) only. HDMI IN 2 is stereo
as well. Multichannel content is therefore passed through to the HDMI output,
not captured as more than two channels.

The engine publishes each input as a PipeWire source node and the SDL2 frontend
plays the selected input through the default sink. For OBS, add the monitor of
the `EverLive Capture Audio` sink or capture the exported source nodes directly.

## Upstream attribution

The hardware stack is derived from
[`neoyagami/OpenGC570D`](https://github.com/neoyagami/OpenGC570D) and the
userspace VFIO-PCI + PipeWire port by PolygonPrincess. It remains licensed
under **GPL-2.0-only**; see `COPYING`, `NOTICE.upstream.md`, `docs/LEGAL.md`,
`docs/REVERSE_ENGINEERING.md` and `docs/DATA_PROVENANCE.md`.

Upstream base and integration points:

| Reference | Commit | Purpose |
| --- | --- | --- |
| GC570D audio splitter validation | `4673bb9` | validated 2.0/5.1 LPCM passthrough baseline |
| Splitter audio pull request | `484aa8d` | PR #6 LPCM output implementation |
| Userspace derivative import | `afbb06e` | VFIO-PCI + PipeWire userspace port (based on `5cef916` / v1.0.5) |

EverLive-specific additions (SDL2 frontend, dual v4l2loopback writers, OSD,
Console Hub integration, dual-input routing and the systemd/udev packaging)
are Copyright © 2026 Everlite and GPL-2.0-only.
