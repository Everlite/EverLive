# Reverse-engineering and development method

This document describes the research, implementation and validation process
used to build the Linux driver and its userspace adaptation.

## Research input

The official Windows software distributed for the capture card was lawfully
obtained and used locally to study how the hardware communicates. The research
focused on the functional behavior required for Linux interoperability.

Ghidra provided the static-analysis environment: disassembly, decompilation,
cross-references, call graphs, data references and comparisons between
initialization paths. The resulting observations were organized into hardware
operations, state transitions and testable hypotheses.

## Development process

Development proceeded in small iterations:

1. Locate a relevant routine or data structure in the Windows driver.
2. Trace its callers, state changes, register operations and timing behavior.
3. Express the observed behavior through Linux PCI, V4L2/videobuf2, ALSA or
   LED-class interfaces (and, in `OpenGC570D-Userspace`, through `vfio-pci`,
   IOMMU DMA, and PipeWire).
4. Build a bounded test for the specific behavior under investigation.
5. Compare hardware status, logs, DMA or frame evidence and physical output
   with the expected result.
6. Keep, revise or discard the hypothesis before moving to the next part.

This process produced a Linux-native implementation with lifecycle and data
structures designed for Linux interfaces. Functional device details needed for
interoperability were retained, including identifiers, register sequences,
masks, delays, format and timing values, EDID contents, and a generated
HDR-to-SDR lookup table.

## AI-assisted workflow (Upstream & Gemini Argon Userspace Port)

In the upstream [`OpenGC570D`](https://github.com/neoyagami/OpenGC570D)
repository, AI agents were used throughout the iterative reverse-engineering
process to:

- navigate and compare Ghidra exports and cross-references;
- map Windows routines and state transitions to hardware operations;
- develop and review Linux kernel-driver changes;
- prepare one-test-at-a-time hardware scripts;
- analyze logs, register snapshots, DMA/frame evidence and regressions;
- integrate V4L2, ALSA, passthrough and RGB support; and
- draft and check documentation, installation scripts and packaging.

In this fork (`OpenGC570D-Userspace`), **Gemini Argon** was used to:

- design and implement the pure userspace `vfio-pci` runtime (`src/gc570d-userspace.c`) and kernel compatibility shim (`src/gc570d-user-compat.h`);
- implement native PipeWire video streaming and lock-free SPSC ring-buffer audio streaming;
- build the `LD_PRELOAD` POSIX shared-memory V4L2 compatibility bridge (`src/gc570d-v4l2-preload.c` and the shared `libeverlive-core/everlive_framestore.h`) with `libv4l2` interception and Electron/Discord child-process filtering;
- diagnose and implement standalone `HDMI IN 1` capture (without an `HDMI OUT` monitor) and S3 sleep/resume (`D3hot -> D0`) hardware recovery;
- merge upstream hardware updates and restore original code comments across all shared source files; and
- author the unified userspace installer scripts and project documentation.

Research priorities and test boundaries were established under human
direction. Hardware experiments were run locally, and their physical video,
audio, passthrough and RGB results were observed directly. Those observations,
together with the recorded technical evidence, guided each subsequent
iteration.

## Validation

A feature is documented as physically validated only after its real output or
capture behavior has been observed on the project hardware. Depending on the
feature, the supporting evidence also includes register state, daemon logs,
frame counts and hashes, DMA progress, timing or audio levels.

Validation was performed on physical hardware across real HDMI sources and
displays. The tested matrix and remaining gaps are listed in
[STATUS.md](https://www.google.com/search?q=STATUS.md).

## Repository output

The resulting public tree contains:

- the shared GC570D hardware initialization, receiver, splitter, DMA, video, audio, and LED modules;
- the userspace VFIO-PCI + PipeWire daemon (`opengc570d-userd`), V4L2 preload library (`libgc570d-v4l2.so`), and CLI tool (`gc570d-ctl`);
- the constants, tables and register sequences used by the driver;
- unified build, installation, uninstallation, VFIO binding, and sleep/resume scripts;
- `udev`, `limits.d`, and `systemd` service configurations; and
- documentation of supported operation, userspace architecture, validation status and research method.

The repository documents enough of the process and results for further testing
and independent verification. Any additional research should use lawfully
obtained inputs and follow the laws and license terms applicable to it.
