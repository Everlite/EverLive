<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Embedded data provenance

This inventory exists so reviewers and distribution packagers can distinguish
independently authored project assets from functional interoperability data
observed in the official Windows software. It is not a legal conclusion.

| Data | Location | Origin and purpose | Packaging status |
| --- | --- | --- | --- |
| PCI, MMIO and I2C constants; register sequences; masks and delays | `src/` | Functional hardware behavior recovered through static analysis and bounded physical experiments, then expressed in Linux-native code | Required for operation; reviewable source |
| HDMI IN 1 and HDMI IN 2 EDID blocks | `src/gc570d-receiver.c` | Byte-identical functional EDID payloads distributed with the official software and required by the validated negotiation paths | Requires explicit distributor/legal review; replacement with independently generated equivalent EDIDs is an open packaging task |
| Default 3000-nit HDR-to-SDR LUT | `data/gc570d_hdr_lut_3000.inc` | Functional 512-word output produced by the official driver's default 3000-nit path and written to the FPGA preprocessing registers | Requires explicit distributor/legal review; an independently specified generator would improve provenance |
| Disconnected-input artwork | `assets/no-signal.png` | Original project artwork distributed under GPL-2.0-only | Redistributable project asset |
| YUYV disconnected-input frame | `data/gc570d_no_signal_640x360.inc` | Deterministically generated from `assets/no-signal.png` by `scripts/generate-no-signal-asset.sh` | Redistributable generated project data |

The repository contains no vendor firmware image and the driver does not call
`request_firmware()`. References to receiver or splitter firmware describe
firmware already present on the capture card; the Linux module checks or
initializes that hardware but does not redistribute or upload its firmware.

The two EDID blocks and the HDR LUT must not be described as independently
authored. They are retained openly because they are functional inputs needed
for interoperability and because changing them can alter HDMI negotiation or
HDR capture. They should not be replaced without preserving the current data
for comparison and physically retesting passthrough, capture and hotplug.
