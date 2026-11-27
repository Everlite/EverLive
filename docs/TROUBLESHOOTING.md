# Troubleshooting & Known Limitations

## IOMMU & VFIO Error `-22`

**Symptom:** The capture card blinks red and `dmesg | grep -i -E 'vfio|iommu'` shows:
`vfio-pci ...: probe with driver vfio-pci failed with error -22`

1. Verify IOMMU is enabled in UEFI/BIOS (`Intel VT-d` or `AMD-Vi / IOMMU = Enabled`, not just `Auto`) **and** on the kernel command line (`intel_iommu=on iommu=pt` or `amd_iommu=on iommu=pt`).
2. Check `/sys/kernel/iommu_groups/` (see [`docs/INSTALL.md`](INSTALL.md#3-verify-iommu-grouping)).

### AMD AM5 Motherboards (B650 / X670 / B850 / X870)

- **Shared chipset IOMMU groups:** Lower PCIe slots routed through the AM5 chipset often share an IOMMU group with onboard USB, SATA, NVMe, or Ethernet controllers. If `vfio-pci` cannot isolate the card, move the capture card to a **CPU-wired PCIe slot** (check your motherboard manual's PCIe lane diagram) or check BIOS AGESA/ACS options.
- **`IOMMU = Auto`:** Explicitly change `IOMMU` from `Auto` to `Enabled` in AMD CBS / NBIO Common Options and ensure `amd_iommu=on iommu=pt` is in your bootloader config.

---

## V4L2 Applications Hang or Freeze on Startup

**Symptom:** `v4l2-ctl -d /dev/video60 --all` (or OBS, Discord, a browser camera
preview) never returns while `LD_PRELOAD=.../libgc570d-v4l2.so` is active. A
stack trace shows the process blocked in `fstat()` → `pthread_mutex_lock`, and
`/dev/shm/gc570d_v4l2_shm` or `/dev/shm/gc573_v4l2_shm` exists.

**Cause:** the shared frame store called the shim's interposed `fstat()`,
`mmap()` or `close()` while the shim already held its global `g_v4l2_lock`.
The non-recursive mutex then deadlocked, which blocked `VIDIOC_QUERYCAP` and
every later ioctl.

**Fix:** frame-store metadata now uses raw syscalls, `VIDIOC_DQBUF` releases the
lock around its blocking `poll()`, and `open(..., O_NONBLOCK)` is honoured.
Upgrade to the current build. If you are on an older release, remove the stale
store before retrying so the old shim starts from a clean state:

```bash
rm -f /dev/shm/gc573_v4l2_shm /dev/shm/gc570d_v4l2_shm \
      /dev/shm/hwsuhd_v4l2_shm /dev/shm/everlive_cam_v4l2_shm
```

The regression test `make -C tests v4l2-preload` creates a live store, runs
`VIDIOC_QUERYCAP`, checks non-blocking `VIDIOC_DQBUF`, and reads a full frame,
with a `SIGALRM`/`timeout` guard so CI fails instead of wedging.

---

## Known Architecture Limitations

1. **Stale `/dev/shm/*_v4l2_shm` after `kill -9` or crash across users:**
   Frame stores in `/dev/shm/` are created with mode `0600`. If a process is killed ungracefully (`kill -9`), another local user cannot unlink the file due to the `/dev/shm` sticky bit. Remove it as the owning user or root:
   ```bash
   sudo rm -f /dev/shm/gc573_v4l2_shm /dev/shm/gc570d_v4l2_shm /dev/shm/hwsuhd_v4l2_shm /dev/shm/everlive_cam_v4l2_shm
   ```
2. **`LD_PRELOAD` client re-attachment after cross-user handoff:**
   When a card is handed off to another user and later resumed, a new frame store instance is created. An already-open OBS/Discord instance using `LD_PRELOAD` may show a black frame until its video source is refreshed or the app is restarted.
3. **Two identical-model cards in Multiview / `--all`:**
   Running two cards of the exact same model simultaneously is not supported yet because they share the same `/dev/shm` frame store name.

---

## HDMI 2.0 Pixel Speckles / Audio Crackle (`>340 MHz` TMDS)

GC573 runs automatic per-lane DFE/RS equalization on the IT6664/IT6805 receivers (`[HDMI] TMDS errors B/G/R` in logs) and caches clean settings in `~/.local/state/everlive/link-eq`. If you are debugging a marginal cable or splitter, you can override auto-EQ with `EVERLIVE_NO_AUTO_EQ=1` or `EVERLIVE_EQ_INPUT_DFE=0`.

---

## OBS Shows Black Screen on Startup

- If OBS was launched before EverLive, toggle the V4L2 source visibility (eye icon) or open its Properties once EverLive is running.
- If the capture stays on the standby card even though the console is on, check **HDCP** on the console (for PS5: *Settings → System → HDMI → Enable HDCP = Off*).
