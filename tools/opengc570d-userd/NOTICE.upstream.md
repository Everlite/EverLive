# Notice

This is an unofficial community project. It is not affiliated with, sponsored
by, approved by, or endorsed by AVerMedia Technologies, Inc. AVerMedia, Live
Gamer DUO, and GC570D are used only to identify the compatible hardware. All
trademarks belong to their respective owners.

## Upstream derivation

The GC570D hardware stack in this directory is a derivative of
[`neoyagami/OpenGC570D`](https://github.com/neoyagami/OpenGC570D), including its
userspace VFIO-PCI + PipeWire adaptation by PolygonPrincess. It remains
licensed under **GPL-2.0-only**. The EverLive-specific SDL2 frontend,
v4l2loopback writers, input routing and Console Hub integration are additional
GPL-2.0-only work by Everlite.

| Reference | Commit | Purpose |
| --- | --- | --- |
| Upstream validated audio baseline | `4673bb9` | validated 2.0/5.1 LPCM HDMI passthrough |
| Splitter audio pull request | `484aa8d` | PR #6 splitter LPCM output implementation |
| Userspace derivative import | `afbb06e` | VFIO-PCI + PipeWire userspace port based on `5cef916` / v1.0.5 |

Upstream copyright and authorship remain with the OpenGC570D contributors.

The repository contains an independently written Linux kernel driver
(`gc570d.ko`), userspace VFIO-PCI + PipeWire daemon (`opengc570d-userd`), V4L2
preload compatibility library (`libgc570d-v4l2.so`), CLI control utility
(`gc570d-ctl`), build and installation scripts, `udev`/`systemd`/WirePlumber
integration files, usage documentation, and the functional interoperability
information needed to operate the device: PCI and I2C identifiers, register
operations, timing and format tables, EDID data, and a generated tone-mapping
lookup table. See [`docs/REVERSE_ENGINEERING.md`](docs/REVERSE_ENGINEERING.md)
and [`docs/USERSPACE.md`](docs/USERSPACE.md) for an honest description of how
that information was obtained and how AI agents (including Gemini Argon) were
used.

This software is provided without warranty. It controls PCI, VFIO-PCI IOMMU DMA,
HDMI, and I2C hardware directly and may fail, hang a stream, or require a
reboot. Do not use it for protected content or where capture failure could cause
harm.
