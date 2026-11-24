# Installation, Build & IOMMU Setup

## Quick Install

```bash
git clone https://github.com/Everlite/EverLive.git
cd EverLive
sudo ./install.sh
```

### What `install.sh` Configures
- **Dependencies:** SDL2, SDL2_ttf, FFmpeg dev libs, `libcap`, PipeWire/WirePlumber, `xcb-util-cursor`, PySide6, and build tools.
- **Permissions & VFIO:** subsystem-qualified udev rules (`/etc/udev/rules.d/70-everlive.rules`, `99-gc570d-vfio.rules`), per-BDF `vfio-pci` binding through `everlive-vfio-bind` (`driver_override`, no global `ids=`/`new_id`), a single `/dev/vfio/vfio` container rule (`70-everlive-vfio.rules`), per-card `/dev/vfio/<group>` ownership (`root:video 0660`), `cap_ipc_lock=+ep` on every installed engine, and blacklisting of the legacy `open_gc573` module. (HWS UHD cards bind via `hws-vfio-bind.sh` / `99-hwsuhd-vfio.rules`.) A bounded 1 GiB `memlock` fallback is installed only when `setcap` is unavailable.
- **Binaries & Service:** Builds `libeverlive-core`, `opengc573-userd`, `opengc570d-userd`, and `everlive-cam`, and installs `libeverlive-core`, `opengc573-userd`, `opengc570d-userd`, `everlive-ctl`, `everlive-service`, and `everlive.service`. (`tools/openhws-userd` is installed via `sudo make hwsuhd install`, or launched directly from the repo by `play.sh`.)
- **Console Hub & Launcher:** installs the current Hub, fonts, icons, and webcam bridge into `/usr/local/lib/everlive`, creates `/usr/local/bin/everlive`, and refreshes the desktop entry. Re-running the installer replaces a copy left behind by an older `make install`, so the menu and terminal always start the current UI.
- **Audio & Video Sinks:** Isolated PipeWire audio sink (`EverLive Capture Audio`), optional `v4l2loopback` nodes (`/dev/video10`, `/dev/video11`), and desktop launcher.

### Installer Options (`sudo ./install.sh --help`)

| Flag | Behavior |
|:---|:---|
| `--no-loopback` / `--skip-loopback` | Skip `v4l2loopback` (keeps the system free of DKMS/out-of-tree modules; use `LD_PRELOAD` or PipeWire window capture for OBS) |
| `--with-framegen` | Install optional `lsfg-vk` Vulkan layer without prompting (interactive runs prompt `[y/N]`; non-interactive runs skip unless `--with-framegen` is given) |
| `--no-framegen` / `--skip-framegen` | Skip `lsfg-vk` without prompting |
| `--with-preload` / `--preload-apps` | Create reversible `<App> (EverLive Camera)` launchers for native Discord, OBS, Chrome, Chromium, Brave, Firefox, and Zoom (interactive runs prompt `[y/N]`) |
| `--no-preload` / `--skip-preload` | Skip `<App> (EverLive Camera)` launchers without prompting |

To remove EverLive and revert its system configuration:

```bash
sudo ./uninstall.sh
```

---

## Building, Testing & Packaging

```bash
make -j"$(nproc)"                                       # core + GC573 + GC570D + VK pacer + cam
make hwsuhd                                             # optional: HWS UHD (ACASIS VS2583 / VC12-4K) engine
make test                                               # 10 C unit-test suites (7 core, 2 GC570D, 1 GC573)
python3 -m unittest discover -s tools/everlive/tests -v # Python Console Hub & GC553G2 unit tests
make asan                                               # AddressSanitizer + UBSan build & test
sudo make install                                       # optional system-wide install (PREFIX=/usr/local)
```

`./build.sh` wraps the top-level `make`. Arch Linux `PKGBUILD` files (`everlive` and `everlive-git`) live in [`packaging/arch/`](../packaging/arch/).

### Updating Pinned Release Checksums

Whenever bumping a pinned version, always update the version and its SHA-256 checksum together:

- **Optional `lsfg-vk` fallback in `install.sh`:** update `LSFG_VK_VERSION` and `LSFG_VK_SHA256` together after verifying the archive checksum:
  ```bash
  curl --proto '=https' --tlsv1.2 --fail --location -fsSL "https://builds.lsfg-vk.dev/lsfg-vk-<VERSION>.tar.xz" | sha256sum
  ```
- **Arch release package (`packaging/arch/everlive/PKGBUILD`):** bump `pkgver`, push/tag `v<pkgver>`, and regenerate `packaging/arch/everlive/SHA256SUMS` + `sha256sums` together:
  ```bash
  ./packaging/arch/everlive/update-checksums.sh
  ```

---

## IOMMU & PCIe Group Setup

PCIe VFIO engines require hardware IOMMU enabled in both UEFI/BIOS and the Linux kernel.

### 1. Enable IOMMU in UEFI/BIOS
- **Intel:** Enable **VT-d**.
- **AMD:** Enable **AMD-Vi** / **IOMMU** (and **SVM** if needed on your board).

### 2. Enable IOMMU on the Kernel Command Line
Add to `GRUB_CMDLINE_LINUX_DEFAULT` in `/etc/default/grub` (or your bootloader's kernel parameters):
- **Intel:** `intel_iommu=on iommu=pt`
- **AMD:** `amd_iommu=on iommu=pt`

Update GRUB and reboot:

```bash
# Ubuntu / Debian / Mint
sudo update-grub && sudo reboot

# Arch / Fedora / CachyOS
sudo grub-mkconfig -o /boot/grub/grub.cfg && sudo reboot
```

### 3. Verify IOMMU Grouping

```bash
for d in /sys/kernel/iommu_groups/*/devices/*; do
    echo -n "Group $(basename $(dirname $(dirname "$d"))): "
    lspci -nns "$(basename "$d")"
done | grep -i -E '1461:|8888:85|1f33:8581'
```

- **Clean isolation:** The capture card is alone in its IOMMU group (aside from its PCIe root port/bridge).
- **Shared group:** All non-bridge devices in the same IOMMU group must be bound to `vfio-pci` (or not held by incompatible host drivers).

### AMD AM5 Motherboard Notes (B650 / X670 / B850 / X870)

AM5 boards have a few common PCIe/IOMMU pitfalls:
1. **Chipset slots vs. CPU-wired slots:** Secondary PCIe x4/x1 slots routed through the AM5 Promontory21 chipset often share a single IOMMU group with onboard USB/SATA/NVMe/Ethernet controllers. If `vfio-pci` fails or refuses to bind because other chipset devices are in the same group, **move the capture card to a CPU-wired PCIe slot** (check your motherboard manual's block diagram) or verify your BIOS **AGESA / ACS** settings.
2. **IOMMU set to `Auto` in BIOS:** Some AM5 BIOSes expose `IOMMU` as `Auto` (leaving it partially disabled or without full PCIe passthrough). Explicitly set `IOMMU = Enabled` in AMD CBS / NBIO Common Options, and pass `amd_iommu=on iommu=pt` on the kernel command line.
3. **Resets & probe error `-22`:** If the card blinks red and `dmesg | grep -i -E 'vfio|iommu'` shows `probe with driver vfio-pci failed with error -22`, check whether the card actually has an IOMMU group (`/sys/kernel/iommu_groups/`) and whether it is sharing a group with active host devices. See [`docs/TROUBLESHOOTING.md`](TROUBLESHOOTING.md#iommu--vfio-error--22).
