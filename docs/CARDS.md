# Supported Capture Cards & Multi-Card Mode

EverLive detects installed PCIe cards via PCI `subsystem_device` and routes each card to its matching engine.

---

## AVerMedia Live Gamer 4K (GC573) — `Stable`

- **PCI ID:** `1461:0054`, Subsystem `1461:5730`
- **Engine:** `tools/opengc573-userd/` (`gc573_user_streamer`, `everlive_userd`)
- **Verified hardware setup:** Maintainer-verified with PlayStation 5 at `1440p60`, `1440p120`, and `4K60` (SDR & HDR10/HLG), and RetroTINK 4K Pro (`240p`, `480i/576i`, `95.94 Hz`) by `@haxman`.
- **Key features:**
  - Hable HDR10/HLG → SDR filmic tonemapping (BT.2408 203-nit diffuse white, BT.2020 → BT.709, live `-2.0..+2.0 EV` bias, bypass on SDR).
  - Audio DMA: Linear PCM `2.0 / 5.1 / 7.1` passthrough and Dolby Digital (AC-3 5.1) / DTS (5.1) decoding via `libavcodec` with ITU-R BS.775 stereo downmix.
  - Dynamic HDMI 2.0 receiver equalization (IT6664 + IT6805 DFE/RS auto-EQ) with link-EQ state caching (`~/.local/state/everlive/link-eq`).
  - Live EDID profile injection (`Default`, `Force 1440p120`, `Force 1080p240`, custom `.bin`), RGB LED control, standby daemon handoff (`everlive_userd`), cross-user card handoff, S3 suspend/resume recovery, and optional LSFG-VK Frame Generation.

### Custom EDID Requirements & Audio Unification (GC573)
Custom EDIDs (`--edid /path/to/custom.bin` or Hub *Load Custom EDID*) must be **256 bytes** (1 base block + 1 CTA-861 extension block) with valid header and checksums. Unless `--raw-edid` (`--no-unify-edid`) is passed, EverLive unifies the CTA-861 Audio Data Block to advertise **L-PCM (8 ch), AC-3 5.1, and DTS 5.1** (needs 6 spare bytes to replace an L-PCM block, or 10 bytes to append; falls back to raw if full).

---

## AVerMedia Live Gamer DUO (GC570D) — `Stable`

- **PCI ID:** `1461:0054`, Subsystem `1461:5700`
- **Engine:** `tools/opengc570d-userd/` (`opengc570d-userd`, `gc570d-ctl`) — contributed by `@PolygonPrincess` (PR #52).
- **Verification:** Dual-input capture, HDR LUT, and stereo LPCM verified on physical hardware by `@PolygonPrincess` (`libgc570d-v4l2.so` preload shim in community validation).
- **Key features:**
  - Simultaneous HDMI 1 + HDMI 2 capture (`/dev/video10` & `/dev/video11` loopback, or `/dev/video60` & `/dev/video61` via `libgc570d-v4l2.so`).
  - HDMI 1 3000-nit BT.2020 HDR-to-SDR LUT and native downscaler (`640×480` up to `1920×1080`).
  - Stereo LPCM capture from both inputs (multichannel passthrough preserved on HDMI OUT).
  - Live PiP (`p`), input selection (`1`/`2`), EDID policy, HPD pulses, and RGB effects.

See [`tools/opengc570d-userd/README.md`](../tools/opengc570d-userd/README.md) for engine-specific CLI details.

---

## HWS UHD (ACASIS VS2583 / AVMatrix VC12-4K) — `Beta`

- **PCI IDs:** `8888:8581`, `8888:85A1`, `8888:8591`, `1F33:8581`, Subsystem `0007`
- **Engine:** `tools/openhws-userd/` (`hws_user_streamer`, `libhwsuhd-v4l2.so`) — contributed by `@Scyllinice` (PR #70).
- **Key features:**
  - Quarter-DMA Artix-7 FPGA engine supporting up to `4096×2160 @ 60` (`4K60`, verified) or `1920×1080 @ 240` YUYV (`1080p240` / `1440p` not yet measured on hardware).
  - 48 kHz stereo L-PCM audio, PipeWire audio tap (`EverLive Capture Audio`), `v4l2loopback`, and `LD_PRELOAD` virtual camera (`/dev/video64`).
  - `hws-vfio-bind.sh` keeps the card powered with bus reset disabled to avoid HDMI sync drops.

See [`tools/openhws-userd/README.md`](../tools/openhws-userd/README.md) for engine-specific CLI details.

---

## AVerMedia Live Gamer ULTRA 2.1 (GC553G2) — `Experimental`

- **USB ID:** `07ca:2553` (HID LampArray `07ca:d553`)
- **Control:** `tools/everlive/gc553g2_control.py` (protocol findings by `@accola88`, Discussion #68).
- **Verification:** USB/UVC AT protocol and descriptor switching tested on hardware (FW `1.0.10.2`); end-to-end Hub UI validation pending.
- **Key features:** Uses standard kernel `uvcvideo` + USB Audio (no VFIO, IOMMU, or loopback needed). Supports `60 Hz` YUYV and `4K120/144`, `1440p120`, `1080p240` MJPEG (USB 3.2 Gen 2 10 Gbps recommended), HDMI presets, EDID modes, hardware HDR toggle, and HID LampArray RGB:
  ```bash
  python3 tools/everlive/gc553g2_control.py status
  python3 tools/everlive/gc553g2_control.py high-fps
  python3 tools/everlive/gc553g2_control.py edid merge
  python3 tools/everlive/gc553g2_control.py rgb '#38BDF8'
  ```

---

## Multi-Card & Multiview

- **Console Hub:** When more than one supported card is installed, the Hub header shows a card selector and a **Multiview** toggle (starts up to 3 extra cards in muted side tiles with per-tile listen/volume controls and synchronized multi-file **Record**).
- **CLI (`play.sh --all`):** Runs one engine per PCIe card with isolated sockets (`everlive-<model>-<pci>.sock`) and PipeWire sinks (`everlive-<model>-<pci>-audio`):
  ```bash
  ./play.sh --all
  ./play.sh --all --record --headless --duration 600
  ./play.sh --all --cards 84:00.0,85:00.0
  python3 tools/everlive/everlive_multi.py --list
  ```
  *Note: Two cards of the exact same model cannot run simultaneously yet because they share a frame store name.*
