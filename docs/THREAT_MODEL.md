# EverLive Threat Model

This document defines the security assets, threat actors, trust boundaries, and mitigations for **EverLive (2.0.x)**.

## 1. Protected Assets

| Asset | Description |
|:---|:---|
| **Console Video & Audio** | Live HDMI video frames, audio streams, shared-memory frame store (`/dev/shm/*`), and virtual camera nodes (`/dev/video*`) |
| **User Session** | Unprivileged desktop user (`everlive` UI, PipeWire session, `~/.config/everlive/`, XDG runtime socket) |
| **Root Helpers** | Privileged bind/capability scripts (`everlive-vfio-bind`, `everlive-setcap`, per-card bind wrappers) invoked via `pkexec` or `sudo` |
| **DMA & PCIe Hardware** | VFIO IOMMU group mappings, BAR0 MMIO regions, and pinned DMA ring buffers for capture hardware |

---

## 2. Threat Actors

1. **Local Unprivileged User:** Another non-root account on the same Linux host attempting to read capture streams, inject commands, or escalate privileges via `pkexec`/udev/sysfs helpers.
2. **Malicious Local App:** An untrusted desktop/browser/Flatpak process attempting to hijack `everlive.sock`, `/dev/shm/*`, or `LD_PRELOAD` / PipeWire routing.
3. **Malicious PipeWire Node Name:** A crafted PipeWire sink/source or node description containing shell metacharacters, format specifiers, or control characters.
4. **Malicious HDMI / EDID Source:** A hostile or malformed HDMI device/cable injecting invalid EDID blocks, unexpected timings, or malformed AC-3/DTS audio bitstreams.
5. **Compromised Mirror / Supply Chain:** A tampered upstream archive, compromised package mirror, or floating GitHub Action tag during install or CI builds.

---

## 3. Trust Boundaries & Mitigations

### B1. Hardware / HDMI Source -> Userspace Capture Engine (Ring 3)
- **Boundary:** Untrusted HDMI/EDID bytes and DMA payloads enter the userspace engine (`gc573_user_streamer`, `opengc570d-userd`, `hws-userd`).
- **Mitigations:**
  - Hardware IOMMU (`intel_iommu=on` / `amd_iommu=on`) confines card DMA strictly to explicitly mapped IOVA buffers.
  - No out-of-tree kernel module (`sys_init_module` is never required; Secure Boot remains enforced).
  - EDID blocks are validated (256-byte size and checksum) and bounded; audio/video parsers run unprivileged in Ring 3.

### B2. Unprivileged User Session -> Root Helpers (`pkexec` / `sudo` / `udev`)
- **Boundary:** The Console Hub (`everlive_viewer.py`) or installer requests `vfio-pci` binding or `cap_ipc_lock=+ep`.
- **Mitigations:**
  - `everlive_viewer.py` enforces `_trusted_helper(path)`: only root-owned, non-symlink, non-group/world-writable binaries under `/usr/` are passed to `pkexec`. Checkout-relative paths are never elevated.
  - `everlive-vfio-bind` validates PCI BDFs against `^[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]$` and verifies vendor/device/subsystem IDs in sysfs before binding via `driver_override` (never global `new_id` or `vfio-pci ids=`).
  - Only the matched card's `/dev/vfio/<iommu_group>` node is set to `root:video 0660` (with ACLs stripped via `setfacl -b`); `/dev/vfio/vfio` uses a single `0660 root:video` rule; `TAG+="uaccess"` and `0666` permissions are forbidden.
  - `cap_ipc_lock=+ep` replaces unlimited memlock; a bounded 1 GiB fallback (`LimitMEMLOCK=1G`) is only used when `setcap` is unavailable.

### B3. Console Hub / Config -> Background Service & External Tools
- **Boundary:** UI settings, `service.env`, PipeWire node names, and V4L2 device paths flow to `everlive-service.sh` and `ffmpeg`/`pactl` subprocesses.
- **Mitigations:**
  - `service_manager.py` validates `--v4l2` paths against `^/dev/video[0-9]{1,3}$`, checks `realpath`, verifies `stat.S_ISCHR`, and quotes `EVERLIVE_SERVICE_ARGS`.
  - Subprocesses use argument arrays (`execve`-style, no `sh -c` with user-controlled strings).
  - IPC socket (`/run/user/<uid>/everlive.sock`) and shared memory reside under user-scoped runtime permissions.

### B4. Upstream Mirrors / CI -> Installer & Packages
- **Boundary:** External downloads (`lsfg-vk` fallback, `PKGBUILD` source tarballs, GitHub Actions).
- **Mitigations:**
  - `install.sh` pins exact `lsfg-vk` version + SHA-256 checksum, downloads over TLS 1.2+ (`curl --proto '=https' --tlsv1.2 --fail --location`), verifies checksums and `tar -tJf` paths (fail-closed) in a private `0700` temp directory before extraction.
  - Release `PKGBUILD` pins tagged tarballs with generated `SHA256SUMS` (never `SKIP`).
  - CI workflows pin GitHub Actions to full commit SHAs, enforce `permissions: contents: read`, `persist-credentials: false`, `actionlint`, `dependency-review`, and `check_tree_hygiene.py`.

---

## 4. Related Documents
- [Security Policy](../SECURITY.md)
- [Contributing & Security Invariants](../CONTRIBUTING.md)
- [2026-11 Security Hardening Tracker (`plan/hardening`)](https://github.com/Everlite/EverLive/issues?q=label%3Aplan%2Fhardening)
