# GC575 on Linux — Findings from the AVerMedia EXEs and an EverLive Integration Plan

Date: 2026-11-14. Sources: `GC575FwUpdateTool_1.1.7.1.exe`,
`RECentral_4_v4.7.114.1_24121801_x64.exe`, the cloned `GC575-Research`
repository, and the firmware/DLL artifacts extracted from those installers.
No hardware was operated; every statement below is static analysis or derived
from the captures shipped with the research export.

## 1. Executive summary

1. **The GC575 is physically a PCIe card, but its capture function is a USB
   UVC device, `07ca:0575`.** Linux binds `uvcvideo` and `snd_usb_audio`;
   there is no native PCIe video function to hand to VFIO. EverLive therefore
   needs a dedicated UVC/V4L2 branch — **not** the GC573 VFIO engine.
2. **1080p240 is already available out of the box** through NV12 and MJPEG.
   The YUY2-1080p240 descriptor burn documented in the research is **not**
   required for 240 fps.
3. **4K144/4K120 capture is already present in firmware profile 1** and is
   activated by AVerMedia Streaming Center through profile selection. On Linux
   it can be selected with `AT b0` (`Set_USB_decription_Index`) without
   flashing firmware.
4. **The USB link must be 10 Gbps (USB 3.2 Gen 2).** A 5 Gbps link lacks the
   uncompressed 1080p240 and 1440p144 modes; USB 2.0 exposes only low
   resolutions.
5. **The firmware extracted from the public updater is byte-identical to the
   research baseline** (`SHA-256 c51aab…`). All 25 firmware sections and the
   six USB descriptor templates are therefore reproducible.
6. **MJPEG quality is disabled in the vendor SDK's GC575 path.** Use NV12 for
   quality; use MJPEG for 4K144 or when bandwidth is tight.

## 2. Analysis of the two EXEs

### 2.1 `GC575FwUpdateTool_1.1.7.1.exe`

| Property | Value |
|---|---|
| Type | NSIS 3 self-extracting installer (Deflate) |
| SHA-256 | `49c261853ad8b0ede02ff8a8f4d9bbdd57c8bbc2c9c9ed2e6acba2749f6c5e21` |
| Contains | `Live Gamer 4K 2.1 FwUpdateTool.exe`, `libavtfwupdater.dll`, `RTICE_SDK_x86.dll`, `RTK_IO_x86.dll`, Qt5 runtime, `7z.exe` |
| Firmware | `FW/GC575_FW_1.1.7.1_20260604.img` (2,498,016 bytes) |
| MCU | `FW/GC575_MCU_24042616_cc61.bin` |
| ENE | `FW/6K77xx_AverMedia_GC575_20240627v17_0x00232DB3_V0x17.bin` |
| Rescue | `FW/install_rescue_7119594_20230824.img` |
| Settings | `FW/ToolSetting.ini`: FW 1.1.7.1, ENE 80.22.02.23, MCU 24042616, RESCUE 118592916 |

**Hash comparison against the research:**

```text
GC575_FW_1.1.7.1_20260604.img
  c51aab22540afc7e7d1fd053c4668509a4e3d161d2de3011f13c20d7fd0267e9  ← exact research baseline

RTK_IO_x86.dll
  ff92321af82a24a107bc2f31fd05a1a455c02b9477cedab1f66b37d38c50c382  ← exact research updater DLL

USB_DT_SSP.bin (section 13)
  746f0c6ebf5a1dce6acaf41b82169e1332cafb487ec0c4af4d09bef11cc26031
USB_DT_SSP_01.bin (section 22)
  fdabb0300f01b52be069ef2df4245126b30379ab5a0073d835cceafd981a16a0
```

The `DTUPIMG` firmware container has 25 entries; **all** payload hashes match
the directory hashes. Important sections:

| # | Name | Size | Meaning |
|---|---|---:|---|
| 0 | `fw` | 2,252,800 | Main firmware (high entropy, no verified ISA/load map) |
| 1 | `SW_CFG` | 4,096 | Configuration |
| 2 | `USER_DEFAULT` | 4,096 | Defaults |
| 13 | `USB_DT_SSP` | 4,096 | USB 3.2 Gen 2 descriptor, profile 0 |
| 14 | `USB_DT_SS` | 4,096 | USB 3.0 5 Gbps descriptor, profile 0 |
| 15 | `USB_DT_HS` | 4,096 | USB 2.0 descriptor, profile 0 |
| 17 | `CUSTOMER_VER` | 128 | ASCII `1.1.7.1` |
| 18 | `USB_INFO` | 4,096 | USB string descriptors (`Live Gamer 4K 2.1-Video`, `…-Audio`) |
| 22 | `USB_DT_SSP_01` | 4,096 | USB 3.2 Gen 2 descriptor, profile 1 (4K144) |
| 23 | `USB_DT_SS_01` | 4,096 | USB 3.0 5 Gbps descriptor, profile 1 |
| 24 | `USB_DT_HS_01` | 4,096 | USB 2.0 descriptor, profile 1 |

The updater itself uses `libavtfwupdater.dll` (SHA-256
`c87d1087dac258c55bcdfb1cdfaa40007e0bfd4250f41ebe4a8e9b3f9c3f9303`) and
`RTK_IO_x86.dll`. Notably, the PDB path is
`D:\source code\Vender\GC575\GC553G2_FW\win\GC553G2FwUpdateSdk\…`: the GC575
updater comes from the same SDK lineage as the GC553G2 updater.

`RTK_IO_x86.dll` exports include:

```text
rtk_burnUSBDesription
rtk_burnMultiFiles
rtk_burnToFlash
rtk_sendATCommand
rtk_setUVCExtension
```

The descriptor burn is therefore a Windows-side RTK SDK operation. It is not
required for normal Linux capture.

### 2.2 `RECentral_4_v4.7.114.1_24121801_x64.exe`

| Property | Value |
|---|---|
| Type | NSIS 3.09 self-extracting installer (Deflate) |
| SHA-256 | `26396a9f79b9bb5f623c7b68140b1a7267f98b291fc908f472e41a11ff786288` |
| Contents | `Setup.exe` (224 MB, WiX Burn bootstrapper) |
| Inside | CAB0 (UX) + CAB1 (payload `a0`, MSI) + .NET installers |
| MSI | 1,869 files, ~523 MB extracted, RECentral 4.7.114.1 |

Relevant components:

| File in MSI | SHA-256 | Meaning |
|---|---|---|
| `REC30RTK_IO_x64_dll_dll_32` | `b9697cc6307b9d8af974dbe857ec779d3a7a3dbf87d3f5a3f3b2b6eba3fbdab2` | newer `RTK_IO_x64.dll` than the research build `b5c600…` |
| `REC30UVCDevice_dll_dll_34` | `2a7a223bca0e7d42ec855b97eaf4a4a9c1a2f6e2d7719561d248f946486a0d79` | UVC device class; contains `pid_0575` |
| `REC30AVerRtkControlSdk_dll_dll_11` | `d6cf8ea56419cda6a67530de1b999bf5d24c6532e6aa40e10caa6c571ca1031e` | vendor SDK; exports `AT_Set_USB_decription_Index`, `rtk_burnUSBDesription` |

`pid_0575` in `REC30UVCDevice` independently confirms that **the GC575 capture
function is USB `07ca:0575`**, not a PCIe video device. The newer `RTK_IO_x64`
build shows that the vendor SDK changes between releases, but the USB/UVC
transport layer stayed the same.

## 3. What the firmware descriptors actually expose

The 4096-byte templates contain the UVC configuration tree with placeholders
(`wTotalLength = 0xffff`, `bNumInterfaces = 8`) that are patched at runtime.
The **live configuration** from the research capture is 2,185 bytes long and
has 5 interfaces (UVC + 3 audio). It contains:

| Format | 1080p | 1440p | 4K |
|---|---|---|---|
| YUY2 | max 120 fps | 60 fps | 50 fps |
| NV12 | **240 fps** | **144 fps** | 60 fps |
| MJPEG | **240 fps** | **144 fps** | **144 fps** (live config) |
| RGB24 | 120 fps | 60 fps | — |
| P010 | 120 fps | 60 fps | 30 fps |

The live config has MJPEG 4K144 and 3440×1440 144. That matches **profile 1**
(`USB_DT_SSP_01`), not the profile-0 template (`USB_DT_SSP`, which has 4K60).
The live config was therefore captured with profile 1 active. This is an
**inference from comparing the templates**, not a new hardware measurement.

### Profile 0 vs. profile 1 (USB 3.2 Gen 2, 10 Gbps)

| Mode | Profile 0 (`USB_DT_SSP`) | Profile 1 (`USB_DT_SSP_01`) |
|---|---|---|
| NV12 1080p240 | ✅ | ✅ |
| MJPEG 1080p240 | ✅ | ✅ |
| NV12 1440p144 | ✅ | ✅ |
| MJPEG 1440p144 | ✅ | ✅ |
| MJPEG 3440×1440 | 60 fps | 144 fps |
| MJPEG 4K | 60 fps | **144 fps** |
| YUY2 1080p240 | ❌ (120 fps) | ❌ (120 fps; 240 only after Windows burn) |

### USB speed dependency

- `USB_DT_SSP`/`_01` = SuperSpeedPlus, 10 Gbps: 1080p240, 1440p144, 4K144.
- `USB_DT_SS`/`_01` = SuperSpeed, 5 Gbps: NV12/YUY2 max 1080p120 or 4K30;
  MJPEG 1080p240 is advertised, but uncompressed 240 is missing.
- `USB_DT_HS`/`_01` = High Speed, 480 Mbps: only 640×480/720p.

**Diagnostic for the friend:** if `v4l2-ctl --list-formats-ext` shows only small
modes, the card is on USB 2.0. If 1080p240 is missing but 1080p120 is present,
it is a 5 Gbps link. 4K144 and uncompressed 1080p240 require a 10 Gbps port and
a 10 Gbps cable.

### Bandwidth sanity check

| Mode | Payload | Bitrate |
|---|---:|---:|
| NV12 1080p240 | 3,110,400 B/frame | ~5.97 Gbps |
| YUY2 1080p240 | 4,147,200 B/frame | ~7.96 Gbps |
| NV12 1440p144 | 5,529,600 B/frame | ~6.37 Gbps |
| NV12 4K60 | 12,441,600 B/frame | ~5.97 Gbps |
| MJPEG 4K144 | compressed | content-dependent |

The descriptors set `dwMaxBitRate` for these modes to the 120/60 fps value
(the 32-bit limit). Some applications filter on it and hide 240 fps even
though the interval entry exists. `v4l2-ctl` and `ffmpeg` show the intervals
directly.

## 4. The private control channel

| Layer | Value |
|---|---|
| USB ID | `07ca:0575` |
| VideoControl interface | 0 |
| XU entity | 4 |
| XU GUID | `961073c7-49f7-44f2-ab42-e940405940c2` |
| Selector 2 | 2-byte LE: length of the pending request/response |
| Selector 1 | AT request/response bytes |

AT23 example (`Get_System_Info`):

```text
SET_CUR selector 2: 21 01 00 02 00 04 02 00  data: 09 00
SET_CUR selector 1: 21 01 00 01 00 04 09 00  data: a1 06 00 00 23 00 00 00 36
GET_CUR selector 2: a1 81 00 02 00 04 02 00  → 11 00
GET_CUR selector 1: a1 81 00 01 00 04 11 00  → a1 0e 00 00 44 42 32 30 35 39 23 31 36 30 35 00 0c
```

Result: `DB2059#1605`.

AT framing (service 1, marker `0xa1`):

- Body = LE32(command ID) + payload, `L = 4 + payload_len`.
- Short form (`L < 126`): `(L+2)&0x7f, 00, 00`.
- Long form: `0x80 | ((L+1)>>8), (L+1)&0xff, 00`.
- Checksum = `-sum(all preceding bytes) & 0xff`.

Important commands:

| AT | Name | Payload | Use |
|---|---|---|---|
| `0x23` | `Get_System_Info` | empty | device/firmware info |
| `0x37` | `HdmiRX_GetRawTimingInfo` | empty | 40-byte HDMI timing |
| `0x38` | `HdmiRX_Get_VideoStable` | empty | signal lock |
| `0x44` | `UVC_Get_Output_Timing` | empty | 12-byte output timing |
| `0x45` | `UVC_Get_VideoFormat` | empty | format byte |
| `0x86` | `HdmiRX_Video_Vfreq` | empty | vertical frequency |
| `0xb0` | `Set_USB_decription_Index` | `01 00 00 00` + LE32(index) | select profile 0/1 |
| `0xb1` | `Get_USB_decription_Index` | empty | read profile |

**Profile selection is not a flash.** `AT b0` sets the active descriptor index
at runtime. The historical experiment burned a descriptor first and then
called `AT_Set_USB_decription_Index(1)`. For 4K144, profile selection alone is
sufficient because profile 1 already contains 4K144 MJPEG.

## 5. Linux support

### Drivers

- `uvcvideo` binds automatically (USB class `0e/01/00`). There is **no**
  GC575-specific quirk in `uvc_driver.c`; the card runs as a generic UVC
  device.
- `snd_usb_audio` binds the three audio interfaces.
- No vendor driver, no VFIO, no IOMMU required.

### Check commands

```sh
lsusb -d 07ca:0575
lsusb -t                      # 10000M? 5000M? 480M?
v4l2-ctl --list-devices
v4l2-ctl -d /dev/videoN --list-formats-ext
arecord -l
```

### Capture

```sh
# 1080p240 NV12
v4l2-ctl -d /dev/videoN \
  --set-fmt-video=width=1920,height=1080,pixelformat=NV12 \
  --set-parm=240

ffmpeg -f v4l2 -input_format nv12 -video_size 1920x1080 -framerate 240 \
  -i /dev/videoN -c:v libx264 -preset ultrafast -f null -
```

```sh
# 4K144 MJPEG (profile 1)
gc575ctl -d /dev/videoN set-index 1
ffmpeg -f v4l2 -input_format mjpeg -video_size 3840x2160 -framerate 144 \
  -i /dev/videoN -c:v copy /tmp/gc575-4k144.mkv
```

### `gc575ctl`

Lives in `EverLive-dev/tools/opengc575-userd/`:

```sh
make
./gc575ctl -d /dev/video0 info
./gc575ctl -d /dev/video0 get-index
./gc575ctl -d /dev/video0 set-index 1
./gc575ctl -D /dev/bus/usb/001/013 info    # usbfs fallback
```

The UVC backend uses `UVCIOC_CTRL_QUERY` (no root required, video group). The
`-D` backend uses exact `usbfs` control transfers and needs access to the USB
device node.

### Audio

- LPCM stereo works through `snd_usb_audio` without vendor commands.
- 5.1/non-PCM requires the vendor AT94/AT95 path. The official
  OBS-MultichannelAudio helper can call `setNonPcmOnOff(1)` during init. This
  is not implemented in `gc575ctl` yet.
- For EverLive: select the GC575's ALSA/PipeWire device. No VFIO audio taps
  like the GC573.

### HDR / P010

- P010 is present as a UVC format. `uvcvideo` maps it to
  `V4L2_PIX_FMT_P010`.
- HDR10 metadata and tonemapping are not part of the standard UVC stream.
  Vendor AT1d/1e/1f exist but are not validated.

### MJPEG quality

- The vendor setter uses generic property `0x0b`, but it is disabled for GC575
  by two gates. No Realtek fallback was found.
- `wCompQuality` in UVC Probe/Commit is a possible alternative but is not
  verified.
- Practical choice: NV12 for quality, MJPEG for bandwidth/4K144.

## 6. EverLive integration

### 6.1 What has already been changed

In the cloned `EverLive-dev` (branch `dev`):

- `tools/libeverlive-core/everlive_card.h`
  - `EVERLIVE_CAP_4K144`
  - `enum everlive_bus { PCI, USB }`
  - `bus` in `everlive_device_desc` and `everlive_device`
  - `usb_path` in `everlive_device`
  - `everlive_match_card_bus()`
  - `everlive_detect_installed_usb_cards()`
- `tools/libeverlive-core/everlive_card.c`
  - GC575 entry corrected to **USB `07ca:0575`**, bus `USB`, capabilities
    including 1080p240, 4K120, 4K144.
  - USB sysfs scan (`/sys/bus/usb/devices`, `idVendor`/`idProduct`).
  - PCI matching remains backward-compatible (`everlive_match_card` = PCI).
- `tools/libeverlive-core/everlive_ctl.c`
  - `detect` scans PCI **and** USB, shows bus, USB path and driver.
  - `list-models` shows USB/PCI ID and the 4K144 capability.
- `tests/test_card_registry.c`
  - GC575 USB matching, rejection of the old PCI placeholder, USB sysfs mock.
- `tools/opengc575-userd/`
  - `gc575ctl` (XU/AT diagnostics and profile selection)
  - `opengc575-userd` (UVC capture daemon)
  - `src/gc575_xu.[ch]` (AT transport)
  - `src/gc575_v4l2.[ch]` (device discovery, format negotiation, MMAP capture,
    NV12→YUY2, optional MJPEG decode)
  - `src/gc575_camera.[ch]` (framestore publisher)
  - `src/gc575_userd.c` (daemon entry point)
  - `src/gc575-v4l2.c` + `libgc575-v4l2.so` (`/dev/video64` preload)
  - `play-gc575.sh` (reference end-to-end launcher)
  - `Makefile`, `README.md`
- `tools/everlive/everlive_display.c`
  - UVC fallback: when `VIDIOC_QUERY_DV_TIMINGS` is unsupported, negotiate a
    normal V4L2 format and optional frame interval instead of aborting.
  - New `-S/--size WxH` and `--fps N` options.
- Top-level `Makefile`: `gc575` target, build/clean/install/uninstall.
- `README.md` and `docs/ADDING_A_NEW_CARD.md`: GC575 documented as a USB UVC
  path.

Build and tests:

```text
make -j4             → successful
make test            → all EverLive tests pass
./tools/opengc575-userd/opengc575-userd --help → OK
everlive-ctl detect  → detects GC573 (PCIe) and lists GC575 (USB) in the model registry
gcc everlive_display.c → UVC fallback compiles
```

End-to-end reference path (no hardware validation):

```sh
PROFILE=0 MODE=nv12 SIZE=1920x1080 FPS=240 \
  tools/opengc575-userd/play-gc575.sh
```

### 6.2 The correct EverLive branch for the GC575

The GC573/GC570D engines are VFIO-based and require exclusive PCIe access. For
the GC575:

```text
PCIe card
  └─ integrated USB controller (PCIe function)
       └─ USB UVC device 07ca:0575
            ├─ interface 0: VideoControl + private XU (entity 4)
            ├─ interface 1: VideoStreaming, bulk IN 0x82
            └─ interfaces 2–4: UAC audio
```

The kernel already owns the device through `uvcvideo`. EverLive must **not**
try to bind the PCIe function or claim the USB interfaces. Instead:

1. **Discovery**: USB sysfs match (already implemented).
2. **Capture**: open `/dev/videoN` directly; select NV12/MJPEG with
   `VIDIOC_S_FMT`; set fps with `VIDIOC_S_PARM`.
3. **Display**: the native renderer now has a UVC fallback. When
   `VIDIOC_QUERY_DV_TIMINGS` is unsupported, it uses `G_FMT`/`S_FMT` and an
   optional `S_PARM` interval instead of aborting. Launch it with
   `-F nv12 --size 1920x1080 --fps 240 -d /dev/videoN`.
4. **Framestore/preload (optional)**: `opengc575-userd` converts NV12→YUY2 or
   decodes MJPEG (when built with libavcodec), publishes to
   `/gc575_v4l2_shm`, and `libgc575-v4l2.so` exposes it as `/dev/video64`.
   The framestore contract is YUY2; raw MJPEG is never published.
5. **Audio**: use ALSA/PipeWire directly; no VFIO audio tap.
6. **Vendor features**: profile selection (`gc575ctl`), telemetry
   (AT37/38/44/45), later EDID/HDR/color. LED is not verified.

### 6.3 Recommended order

| Phase | Content | State |
|---|---|---|
| 1 | `gc575ctl` + USB discovery | implemented, compile-tested |
| 2 | UVC fallback in the native renderer (`-F nv12 --fps 240 -d /dev/videoN`) | implemented, compile-tested |
| 3 | `opengc575-userd`: V4L2 capture, NV12→YUY2, framestore `/dev/video64` | implemented, compile-tested |
| 4 | MJPEG decode (4K144) via libavcodec | implemented when libavcodec is present, not hardware-tested |
| 5 | 5.1/non-PCM audio over AT94/95 | not implemented |
| 6 | EDID/HDR/color over XU | not implemented |

### 6.4 Important design decisions

- **No VFIO for the GC575.** Keep the card on the kernel (`uvcvideo`);
  otherwise the only working capture path is lost.
- **NV12 as the preferred format** for 240 fps: 5.97 Gbps instead of 7.96 Gbps
  with YUY2, and it is already in the stock descriptor.
- **MJPEG only for 4K144** or when the host/link lacks bandwidth. MJPEG decode
  must then run on CPU/GPU; 4K144 is demanding without hardware decode.
- **Profile 1 via AT b0**, not a firmware burn. Keep profile 0 as fallback.
- **No descriptor burns from Linux.** The burn is a Windows RTK SDK operation;
  its only gain (YUY2 1080p240) is unnecessary for the Linux path.
- **Do not parse the UVC templates as a contiguous config descriptor.** The
  templates contain placeholders and `0xff` padding; the live configuration is
  the truth.
- **Keep arbitrary AT commands behind an explicit unsafe switch.** The normal
  capture path is read-only; `gc575ctl raw` requires `-U`, and `set-index` is
  restricted to 0/1. Unknown AT commands could change EDID, MCU state or other
  persistent settings even though they are not a full firmware flash.

## 7. Open points / limits

- No hardware validation; `gc575ctl`, `opengc575-userd` and the
  `everlive_display` UVC fallback are compile-tested but not hardware-tested.
- Whether `UVCIOC_CTRL_QUERY` accepts the full 512-byte control for this XU must
  be checked on the device; the `-D` usbfs backend is the exact fallback.
- Whether `AT b0` triggers re-enumeration and whether the profile selection is
  persistent is not verified. After switching, rebind `uvcvideo` if necessary.
- Whether the live config is really profile 1 is inferred from the 4K144 MJPEG
  comparison; the research file is named `configuration-0`, not `profile-0`.
- The card's PCIe function (if it appears as its own PCIe controller) was not
  identified. It is irrelevant for capture; it may matter later for
  passthrough/EDID.
- 5.1/non-PCM audio, EDID profiles, HDR metadata and LED are not implemented.
- The RECentral builds contain newer vendor DLLs; the transport analysis is
  based on the research build. The USB/UVC layer is build-stable, but feature
  gates may change.

## 8. References

- `GC575-Research/01-VERIFIED-TRANSPORT.md` — XU transport, AT23.
- `GC575-Research/07-PROTOCOL-BOUNDARY.md` — AT framing, descriptor provenance.
- `GC575-Research/08-DESCRIPTOR-EXPERIMENTS.md` — YUY2 1080p240 burn.
- `GC575-Research/03-FORMATS-ROI-LATENCY.md` — formats/bandwidth.
- `GC575-Research/02-MJPEG-QUALITY.md` — MJPEG gates.
- `GC575-Research/05-AUDIO-CONTROLS.md` — audio AT94/95.
- `GC575-Research/10-VENDOR-DLL-RE.md` — RTK/UVC/KS implementation.
- `GC575-Research/evidence/descriptor-experiments/live-before-configuration-0.bin`
  — live UVC config.
- `GC575-Research/evidence/descriptor-experiments/live-final-configuration-0.bin`
  — live config after the YUY2 burn.
