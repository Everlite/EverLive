# EverLive — Linux Console Gaming Suite & Userspace Driver

[![Version](https://img.shields.io/badge/version-v2.0.1-blue.svg)](https://github.com/Everlite/EverLive/releases)
[![Status](https://img.shields.io/badge/GC573-verified%20(1440p120%20%2B%204K60)-brightgreen.svg)](docs/CARDS.md)
[![Driver](https://img.shields.io/badge/driver-Userspace%20VFIO%20(Blob--Free)-2e7d32.svg)](docs/ARCHITECTURE.md)
[![Kernel Modules](https://img.shields.io/badge/DKMS-None%20Required-success.svg)](docs/INSTALL.md)
[![Hardware Fund](https://img.shields.io/badge/Hardware%20Fund-GitHub%20Sponsors-ea4aaa.svg?logo=github)](https://github.com/sponsors/Everlite)
[![Discord](https://img.shields.io/badge/Discord-Join%20Community-5865F2?logo=discord&logoColor=white)](https://discord.com/invite/D2XFhkPMmc)
[![License](https://img.shields.io/badge/license-GPL--2.0--only-blue.svg)](#legal-and-licensing)
[![AI Assisted](https://img.shields.io/badge/transparency-heavily%20AI%20assisted-ff69b4.svg)](#transparency--ai-assistance)

**EverLive** is an open-source Linux console gaming suite and userspace capture driver (Wayland & X11). PCIe capture cards run in Ring 3 via the in-tree Linux `vfio-pci` subsystem (no out-of-tree kernel modules or DKMS required), while supported USB cards use standard `uvcvideo` plus USB/HID controls.

> **Quick Navigation:** [Quickstart](#quickstart) · [Supported Hardware](#supported-hardware) · [Install & IOMMU](docs/INSTALL.md) · [Controls & Hotkeys](docs/CONTROLS.md) · [OBS & Webcam](docs/OBS.md) · [Frame Gen (LSFG-VK)](docs/FRAMEGEN.md) · [Architecture](docs/ARCHITECTURE.md) · [Troubleshooting](docs/TROUBLESHOOTING.md)

---

## Quickstart

VFIO capture requires IOMMU (`Intel VT-d` or `AMD-Vi / IOMMU`) enabled in UEFI/BIOS and on the kernel command line. See [`docs/INSTALL.md`](docs/INSTALL.md#iommu--pcie-group-setup) if the card is not detected.

```bash
git clone https://github.com/Everlite/EverLive.git
cd EverLive
sudo ./install.sh
```

Launch the Console Hub (or start **EverLive** from your desktop application menu):

```bash
everlive           # installed Console Hub
./play.sh          # run directly from this checkout
./play.sh --tray   # start minimized in system tray
./play.sh --all    # run all installed PCIe cards simultaneously
```

Update the installed Hub (`install.sh` replaces the system-wide copy) or install strictly module-free without `v4l2loopback`:

```bash
git pull && sudo ./install.sh   # refresh the system-wide Hub and engines
./play.sh                       # or run directly from this checkout
sudo ./install.sh --no-loopback --with-preload
```

---

## Supported Hardware

| Card | Interface / ID | Engine | Status | Verified By |
|:---|:---|:---|:---|:---|
| **AVerMedia Live Gamer 4K (GC573)** | PCIe `1461:0054` (`1461:5730`) | VFIO (`opengc573-userd`) | **Stable** | Maintainer (PS5, 1440p120 & 4K60 SDR/HDR) |
| **AVerMedia Live Gamer DUO (GC570D)** | PCIe `1461:0054` (`1461:5700`) | VFIO (`opengc570d-userd`) | **Stable** | Contributor (`@PolygonPrincess`, PR #52) |
| **HWS UHD (ACASIS VS2583 / AVMatrix VC12-4K)** | PCIe `8888:8581` / `1f33:8581` (`0007`) | VFIO (`openhws-userd`) | **Beta** | Contributor (`@Scyllinice`, PR #70) |
| **AVerMedia Live Gamer ULTRA 2.1 (GC553G2)** | USB `07ca:2553` | USB/UVC (`gc553g2_control.py`) | **Experimental** | Protocol tested (`@accola88`); Hub UI pending |

### Features per Card

| Feature | GC573 | GC570D (DUO) | HWS UHD | GC553G2 |
|:---|:---:|:---:|:---:|:---:|
| **Max Capture** | 4K60 / 1440p120 *(pass-through)* | Dual 1080p60 | 4K60 verified *(up to 1080p240 in code)* | 4K144 / 1440p120 / 1080p240 *(protocol tested)* |
| **HDR → SDR Tonemapping** | Yes (Hable) | Yes (HDMI 1 LUT) | No | Hardware AT toggle *(protocol tested)* |
| **Audio** | LPCM 2.0/5.1/7.1, AC-3/DTS | Stereo LPCM (dual) | Stereo LPCM | USB Audio / PipeWire |
| **Virtual Camera (`v4l2loopback` / `LD_PRELOAD`)** | Yes (`/dev/video10`, `/dev/video63`) | Yes (`/dev/video10-11`, `/dev/video60-61` *(in validation)*) | Yes (`/dev/video10`, `/dev/video64`) | Native `/dev/videoX` (`uvcvideo`) |
| **Custom EDID** | Yes (with SAD unification) | Internal / Downstream policy | No | Yes (`merge` / `copy` / `internal`) |
| **RGB LED Control** | Yes | Yes | No | Yes (HID LampArray) |
| **Frame Generation (LSFG-VK)** | Yes (2×/3×/4×) | No | No | No |
| **Multiview & Multi-Card (`--all`)** | Yes | Yes | Yes | No |

Details and per-card notes: [`docs/CARDS.md`](docs/CARDS.md).

---

## Documentation

| Guide | Contents |
|:---|:---|
| [`docs/INSTALL.md`](docs/INSTALL.md) | Installer options, module-free setup, build/test commands, Arch packaging, IOMMU & AM5 setup, uninstall |
| [`docs/CARDS.md`](docs/CARDS.md) | Per-card capabilities, verification status, custom EDID rules, multi-card & `play.sh --all` |
| [`docs/CONTROLS.md`](docs/CONTROLS.md) | Hub & engine hotkeys, Quick Navigation bar, Performance Stats HUD |
| [`docs/OBS.md`](docs/OBS.md) | 4 OBS capture workflows, `LD_PRELOAD` shims, PipeWire audio tap, Webcam Picture-in-Picture |
| [`docs/FRAMEGEN.md`](docs/FRAMEGEN.md) | Optional LSFG-VK frame generation (2×/3×/4×), 30 FPS dedup, known throughput limits |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Userspace VFIO design, architecture diagram, `libeverlive-core`, handoff, S3 suspend/resume |
| [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) | Known limitations, AM5/IOMMU error `-22`, stale `/dev/shm` cleanup, HDMI 2.0 auto-EQ |
| [`docs/ADDING_A_NEW_CARD.md`](docs/ADDING_A_NEW_CARD.md) | Contributor guide for adding new capture card engines |
| [`CHANGELOG.md`](CHANGELOG.md) | Release history and recent changes |

---

## Video Demonstration (PS5 1440p @ 120 FPS)

Launching from the desktop menu into the PS5 dashboard and *The Finals* at **1440p @ 120 FPS** *(compressed for GitHub's 10 MB embed limit; full-bitrate clip in [Releases](https://github.com/Everlite/EverLive/releases))*:

https://github.com/user-attachments/assets/19932c79-5364-4f16-81cf-0187f741129c

---

## Contributing & Hardware Fund

Contributions, register dumps, and new card ports are welcome — see [`CONTRIBUTING.md`](CONTRIBUTING.md), [`docs/ADDING_A_NEW_CARD.md`](docs/ADDING_A_NEW_CARD.md), and the [Discord community](https://discord.com/invite/D2XFhkPMmc).

**EverLive** is an independent, non-commercial open-source project. All [GitHub Sponsors](https://github.com/sponsors/Everlite) funds are used exclusively to acquire second-hand capture cards for clean-room Linux driver development (current targets: **AVerMedia GC575** and **Elgato 4K60 Pro / Cam Link Pro**).

---

## Legal and Licensing

| Component | Path | License | Notes |
|:---|:---|:---:|:---|
| **GC573 Engine & VK Pacer** | `tools/opengc573-userd/` | **GPL-2.0-only** | Userspace VFIO driver & `VK_LAYER_EVERLIVE_pacer` |
| **Shared Core Library** | `tools/libeverlive-core/` | **GPL-2.0-only** | Card registry, `everlive_framestore`, preload engine, handoff, `everlive-ctl` |
| **GC570D (DUO) Engine** | `tools/opengc570d-userd/` | **GPL-2.0-only** | Derived from [`neoyagami/OpenGC570D`](https://github.com/neoyagami/OpenGC570D) |
| **HWS UHD Engine** | `tools/openhws-userd/` | **GPL-2.0-only** | Userspace VFIO engine for ACASIS VS2583 / AVMatrix VC12-4K |
| **GC553G2 USB/UVC Control** | `tools/everlive/gc553g2_control.py` | **GPL-2.0-only** | Adapted from [`accola88/GC553G2-Linux-Utility`](https://github.com/accola88/GC553G2-Linux-Utility) (MIT) |
| **Console Hub & Scripts** | `tools/everlive/`, `play.sh`, `install.sh` | **GPL-2.0-only** | Python/Qt Hub and launcher scripts |
| **Bundled Fonts** | `tools/everlive/fonts/` | **CC BY 4.0 / SIL OFL 1.1 / Apache 2.0** | Font Awesome Free 6 & Noto Sans |
| **Legacy Kernel Module** | `legacy/open_gc573-kernel/` | **GPL-2.0-only** | Historical reference |

*All product names and trademarks belong to their respective owners and are used strictly for compatibility identification. This project is not affiliated with or endorsed by AVerMedia Technologies, Inc., Sony, ACASIS, or AVMatrix. Register configuration and EDID operations are performed at your own risk.*

---

## Acknowledgements

- **[@PolygonPrincess](https://github.com/PolygonPrincess)**: Live Gamer DUO (`GC570D`) userspace VFIO port (PR #52), `libeverlive-core` & unified control plane (PR #61, PR #63), reusable V4L2 preload engine, S3 suspend/resume, background service & app shortcuts (PR #64, PR #67), Quick Navigation / Hub OSD polish (PR #72, PR #73).
- **[@Scyllinice](https://github.com/Scyllinice)**: HWS UHD (`ACASIS VS2583 / AVMatrix VC12-4K`) userspace VFIO engine & multi-card Hub selector (PR #70), HDMI 2.0 dynamic auto-EQ (PR #57), cross-user handoff (PR #65), Arch Linux packaging (PR #58), fast DDR startup & logging (PR #60), viewer lifecycle fix (PR #59).
- **[@accola88](https://github.com/accola88)**: GC553G2 USB/UVC AT protocol, high-FPS descriptors, and HID LampArray RGB ([Discussion #68](https://github.com/Everlite/EverLive/discussions/68)).
- **[@neoyagami](https://github.com/neoyagami)**: [OpenGC570D](https://github.com/neoyagami/OpenGC570D) dual-input audio and splitter reverse-engineering.
- **[@haxman](https://github.com/haxman)**: RetroTINK 4K Pro modelines, 240p/480i/576i, and high-refresh verification.
- **[@ostrich](https://github.com/ostrich)**, **[@lou-perret](https://github.com/lou-perret)**, **[@derrod](https://github.com/derrod)**: Foundational Artix-7 register reverse-engineering, early capture/DMA breakthroughs (PR #7, PR #8), and `CAP_IPC_LOCK` instant 4K capture (PR #53).

### Transparency & AI Assistance

This project is heavily AI-assisted: the maintainer (Everlite) directs architecture, tests on physical hardware, analyzes hardware behavior, and validates performance, while implementation, refactoring, and debugging are iterated with AI coding agents.
