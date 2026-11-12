<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Adding a New Capture Card to EverLive

This guide documents the contributor workflow for adding support for new capture hardware to EverLive. It covers the complete architecture from hardware reverse-engineering and core library registration down to userspace DMA streaming, JSON-RPC 2.0 control, packaging, and unit testing.

> **Contribution Path for Hardware Explorers**:
> Hardware findings without a full engine implementation are warmly welcomed! If you have captured diagnostic dumps (`lspci`, `USBPcap`, IOMMU topology) or reverse-engineered register mappings for an unlisted capture card, please share them via a GitHub Discussion or Discord. New hardware can land immediately as a **Community Preview** descriptor in `libeverlive-core` and advance through the support tiers as community testing progresses.

---

## 1. Architectural Foundations & Transport Branching

EverLive separates hardware abstraction, streaming daemons, and client applications:

- **Core Hardware Abstraction Layer (`tools/libeverlive-core`)**: Provides the static card registry, hardware capability flags, polymorphic `everlive_card_ops` vtable, allocation-free JSON-RPC 2.0 helpers, high-performance YUY2/RGB frame conversion, the triple-buffered shared frame store (`everlive_framestore`), and the reusable V4L2 preload engine (`libeverlive-core/v4l2`).
- **Engine Daemon (`tools/opengc*-userd`)**: Runs the hardware-specific driver logic, manages video/audio streaming, publishes post-processing frames to the shared store, and hosts the Unix domain socket server (`everlive.sock`).
- **Clients & Packaging**: The PySide6 Console Hub (`tools/everlive/everlive_viewer.py`), the `everlive-ctl` CLI, system udev rules, modprobe configs, optional per-card V4L2 preload DSOs, and Arch/AUR packaging (`packaging/arch/`).

```
+--------------------------------------------------------------------------+
|                        EverLive Architecture                             |
+--------------------------------------------------------------------------+
|                                                                          |
|  [ User Interface Layer ]                                                |
|     +-------------------------+      +-------------------------------+   |
|     |  PySide6 Console Hub    |      |   everlive-ctl CLI / play.sh  |   |
|     +-------------------------+      +-------------------------------+   |
|                  \                               /                       |
|                   \  JSON-RPC 2.0 (everlive.sock)                        |
|                    \                           /                         |
|  [ Engine Daemon Layer (tools/opengc*-userd) ]                           |
|     +----------------------------------------------------------------+   |
|     |  Capture Engine Daemon                                         |   |
|     |  - Branch A: Native PCIe FPGA DMA via VFIO (Implemented)       |   |
|     |  - Branch B: UVC / USB-Bridge via V4L2 (Planned Blueprint)     |   |
|     |  - PipeWire Audio & SDL2 / V4L2 Loopback Presenter             |   |
|     |  - Publishes post-processed frames to shared SHM store         |   |
|     +----------------------------------------------------------------+   |
|                  |                               |                       |
|                  | Links                         | Registers Ops (Opt)   |
|                  v                               v                       |
|  [ Core Abstraction Layer (tools/libeverlive-core) ]                    |
|     +----------------------------------------------------------------+   |
|     |  libeverlive-core                                              |   |
|     |  - Static Card Registry & Matcher (vendor/device/subsystem)    |   |
|     |  - Operations Vtable Dispatch (struct everlive_card_ops)       |   |
|     |  - JSON-RPC 2.0 Helpers (jsmn) & Socket Path Resolver (IPC)    |   |
|     |  - SIMD/Scalar Frame & Color Space Converters                  |   |
|     |  - Triple-buffered Frame Store (everlive_framestore)           |   |
|     |  - Shared V4L2 Preload Engine (card descriptors + interposer)  |   |
|     +----------------------------------------------------------------+   |
|                                                                          |
|  [ Kernel & Hardware Layer ]                                             |
|     /dev/vfio/<iommu_group>  OR  /dev/video* (uvcvideo, planned)         |
|     /dev/shm/<slug>_v4l2_shm -> virtual /dev/videoN (LD_PRELOAD only)    |
|                                                                          |
+--------------------------------------------------------------------------+
```

Both streaming branches can publish their final frames into the shared store.
The optional module-free V4L2 path then exposes them as a process-local
`/dev/videoN` through a per-card `LD_PRELOAD` DSO; it is independent of
`v4l2loopback` and of the JSON-RPC control plane.

### Transport Decision Tree: Native PCIe DMA vs. UVC Bridge

Before implementing a driver, verify the physical architecture of the hardware:

1. **Branch A: Native PCIe MMIO + DMA Engine (Implemented in EverLive: GC573, GC570D; GC555 Preview)**
   - The card exposes native PCIe BARs directly to a custom FPGA or ASIC DMA controller.
   - Requires kernel binding to `vfio-pci`, PCIe Bus Master activation, MMIO register mapping, pinned host DMA memory (`VFIO_IOMMU_MAP_DMA`), and MSI/MSI-X interrupts.
2. **Branch B: UVC / USB Host Bridge Architecture (Architectural Blueprint)**
   - External USB 3.x capture devices (e.g. Live Gamer ULTRA GC553) or PCIe cards carrying an onboard USB host controller behind a bridge expose standard USB Video Class interfaces.
   - Video streaming operates via the standard Linux `uvcvideo` driver (`/dev/video*`).
   - Hardware controls (RGB LEDs, EDID switching, custom modes) operate via UVC Extension Units (`UVCIOC_CTRL_QUERY`) rather than raw PCIe MMIO.
   - **Crucial**: Do not bind Branch B devices to `vfio-pci`. Doing so detaches the USB controller from the kernel, destroying `/dev/video*`.

---

## 2. Hardware Research & Reverse-Engineering

When exploring new hardware, gather diagnostic dumps before writing implementation code:

### 2.1 PCIe Configuration Space & Subsystems
```bash
# Detailed PCIe properties and full configuration space dump
lspci -nn -vvv -xxxx -d 1461:
```
Key fields to document:
- **Vendor ID** (`0x1461` for AVerMedia).
- **Device ID** (e.g. `0x0054`, `0x0055`, `0x0057`).
- **Subsystem Vendor ID & Subsystem Device ID**: Required to distinguish models sharing base device IDs (e.g. GC573 vs. GC570D both use `1461:0054`).
- **BARs & Sizes**: Inspect 32-bit vs. 64-bit non-prefetchable memory regions.
- **Interrupt Capabilities**: Verify MSI and MSI-X vectors.

### 2.2 IOMMU Grouping (Branch A only)
```bash
for d in /sys/kernel/iommu_groups/*/devices/*; do
    echo -n "Group $(basename $(dirname $(dirname $d))): "
    lspci -nns $(basename $d)
done | grep -i '1461:'
```
A card destined for native VFIO userspace operation requires an isolated IOMMU group (or isolation shared only with upstream PCIe bridge root ports).

### 2.3 Hardware Tracing Strategies
- **PCIe MMIO (Branch A)**: When running the vendor driver in a Windows VM with VFIO passthrough, native KVM EPT/NPT page tables map BARs directly into the guest, bypassing standard QEMU tracepoints. To inspect MMIO accesses, pass `-device vfio-pci,host=...,x-no-mmap=on` to QEMU and trace VFIO region reads/writes (`-trace vfio_region_read* -trace vfio_region_write*`), or capture kernel events via `trace-cmd record -e vfio:vfio_pci_read -e vfio:vfio_pci_write`.
- **USB / UVC Extension Units (Branch B)**: Capture vendor control requests on Windows using Wireshark and USBPcap. Filter for bulk/control traffic (`usb.transfer_type == 0x02` or `usb.control`), inspect `bRequest`, `wValue`, and `wIndex`, and extract the Extension Unit GUID from the UVC descriptor.
- **I2C EEPROM**: Inspect onboard I2C EEPROMs using `i2cdetect` / `i2cdump` if an I2C controller is exposed.

---

## 3. Core Registry Integration (`tools/libeverlive-core`)

All card models must be declared in `libeverlive-core`. The core library is allocation-free; registration of ops tables is a single-threaded initialization step, after which ops lookup freezes the dispatch table (`kEverliveOpsFrozen`).

### 3.1 Declare Capabilities in `everlive_card.h`

Review the `EVERLIVE_CAP_*` bitflags in `tools/libeverlive-core/everlive_card.h`:

```c
#define EVERLIVE_CAP_VIDEO_CAPTURE       (1U << 0)
#define EVERLIVE_CAP_AUDIO_CAPTURE       (1U << 1)
#define EVERLIVE_CAP_PASSTHROUGH         (1U << 2)
#define EVERLIVE_CAP_1080P60             (1U << 3)
#define EVERLIVE_CAP_4K60                (1U << 4)
#define EVERLIVE_CAP_1440P120            (1U << 5)
#define EVERLIVE_CAP_1080P240            (1U << 6)
#define EVERLIVE_CAP_HDR10               (1U << 7)
#define EVERLIVE_CAP_DUAL_INPUT          (1U << 8)
#define EVERLIVE_CAP_RGB_LED             (1U << 9)
#define EVERLIVE_CAP_BITSTREAM_AUDIO     (1U << 10)
#define EVERLIVE_CAP_FRAMEGEN            (1U << 11)
#define EVERLIVE_CAP_HARDWARE_SCALER     (1U << 12)
#define EVERLIVE_CAP_HARDWARE_SPLITTER   (1U << 13)
#define EVERLIVE_CAP_EDID_PASSTHROUGH    (1U << 14)
#define EVERLIVE_CAP_4K120               (1U << 15)
```

Add bitflags if the new hardware exposes capabilities not yet represented (e.g. HDMI 2.1 4K144, Variable Refresh Rate, eARC).

### 3.2 Add or Update the Descriptor in `kEverliveCardRegistry[]`

In `tools/libeverlive-core/everlive_card.c`, update the existing placeholder entry (or append a new descriptor for unlisted hardware):

```c
static const struct everlive_device_desc kEverliveCardRegistry[] = {
    /* ... existing entries (e.g. GC570D, GC573) ... */
    {
        /* Example: Live Gamer 4K 2.1 (GC575 placeholder) */
        .vendor_id       = 0x1461,
        .device_id       = 0x0057,
        .subsystem_id    = 0, /* Subsystem ID; 0 for wildcard until hardware confirmed */
        .model_slug      = "gc575",
        .name            = "AVerMedia Live Gamer 4K 2.1 (GC575)",
        .capabilities    = EVERLIVE_CAP_VIDEO_CAPTURE |
                           EVERLIVE_CAP_AUDIO_CAPTURE |
                           EVERLIVE_CAP_PASSTHROUGH |
                           EVERLIVE_CAP_1080P60 |
                           EVERLIVE_CAP_4K60 |
                           EVERLIVE_CAP_1440P120 |
                           EVERLIVE_CAP_HDR10 |
                           EVERLIVE_CAP_RGB_LED |
                           EVERLIVE_CAP_BITSTREAM_AUDIO |
                           EVERLIVE_CAP_EDID_PASSTHROUGH,
        .num_input_ports = 1,
        .ops             = &kEverliveDefaultOps,
        .fallback_match  = false,
    },
};
```

#### Precedence Rules in `everlive_match_card()`:
1. **Exact match**: `vendor_id == vendor && device_id == device && subsystem_id == subsystem`
2. **Wildcard match**: `vendor_id == vendor && device_id == device && subsystem_id == 0`
3. **Fallback entry**: `vendor_id == vendor && device_id == device && fallback_match == true` (used by GC573 for arbitrary subsystems).

> **Fail-Safe Policy in `everlive_match_card_ex()`**:
> If sysfs cannot read `subsystem_device`, exact matches and the GC573 fallback match are suppressed to prevent misidentification when device IDs are shared. Wildcard entries (`subsystem_id == 0`) remain matchable.

### 3.3 The Polymorphic Vtable (`struct everlive_card_ops`)

`libeverlive-core` provides `struct everlive_card_ops` for runtime dispatch. New capture engines can register their vtable during single-threaded startup via `everlive_register_card_ops()`:

```c
struct everlive_card_ops {
    int  (*probe)(struct everlive_device *dev);
    int  (*init_hardware)(struct everlive_device *dev);
    int  (*standby)(struct everlive_device *dev);
    void (*release)(struct everlive_device *dev);
    int  (*set_input_port)(struct everlive_device *dev, int port);
    int  (*start_capture)(struct everlive_device *dev);
    int  (*stop_capture)(struct everlive_device *dev);
    int  (*set_edid_profile)(struct everlive_device *dev, int port,
                             const char *profile_name,
                             const uint8_t *custom_bin, size_t bin_len);
    int  (*enable_passthrough)(struct everlive_device *dev, bool enable);
    int  (*set_led)(struct everlive_device *dev, enum everlive_led_mode mode,
                    uint8_t r, uint8_t g, uint8_t b);
    int  (*get_telemetry)(struct everlive_device *dev,
                          struct everlive_telemetry *out_telemetry);
};
```

#### Callback Contract & NULL-ability
- Function pointers in `struct everlive_card_ops` **must not be left NULL**.
- If a card does not support an optional capability (e.g. RGB LEDs on an industrial board, or multi-input port switching on a single-port card), point that callback to a stub returning `-ENOTSUP` (following the pattern of `kEverliveDefaultOps` in `everlive_card.c`).
- `enable_passthrough` is part of the hardware C abstraction. For cards where passthrough is permanently hardwired at the HDMI receiver, `enable_passthrough` returns 0 if requested `enable == true` and `-ENOTSUP` if disabled.

Register during single-threaded engine initialization:
```c
int ret = everlive_register_card_ops("gc575", &my_engine_ops);
if (ret < 0) {
    /* Handle error (-EINVAL, -ENOENT, -EBUSY, -ENOSPC) */
}
```

> **Registry Freeze Guard**: The first call to `everlive_card_ops_for()` freezes the ops binding table (`kEverliveOpsFrozen = true`). Subsequent registrations return `-EBUSY`.

### 3.4 Optional V4L2 Preload Descriptor

The module-free V4L2 preload path is optional. If the new engine supports it,
add a descriptor under the engine directory:

```text
tools/opengc<slug>-userd/src/<slug>-v4l2.c
```

This descriptor is separate from `struct everlive_device_desc`: the core
registry identifies hardware, while the preload descriptor tells the shared
`libeverlive-core/v4l2` engine how to expose the card's frames. Do **not**
copy `everlive_v4l2_state.c` or `everlive_v4l2_preload.c`; write only the
descriptor and let the common engine do the work. The descriptor file defines
exactly one `everlive_v4l2_card_descriptor()`.

Required fields include:

| Field | Purpose |
|---|---|
| `shm_name` | POSIX SHM store, e.g. `/gc573_v4l2_shm` |
| `slug`, `driver`, `card_name` | Identity reported by `VIDIOC_QUERYCAP` |
| `port_names[]`, `bus_suffixes[]` | Per-port card names and `bus_info` suffixes |
| `fallback_bdf` | Bus info BDF when no publisher/store is available |
| `node_base`, `port_count` | Virtual node range (`node_base + port_count - 1`) |
| `max_width`, `max_height` | Largest source frame the descriptor accepts |
| `default_width`, `default_height` | Initial `VIDIOC_G_FMT` geometry |
| `mmap_base`, `mmap_stride` | Virtual buffer offset layout |
| `sizes[]`, `fps_denominators[]` | `VIDIOC_ENUM_FRAMESIZES` / intervals |
| `clamps[]` | `VIDIOC_TRY_FMT` / `S_FMT` request-to-output buckets |

Current examples:

| Card | Store | Nodes | Ports | Max | Default | mmap stride |
|---|---|---|---|---|---|---|
| GC570D | `/gc570d_v4l2_shm` | 60, 61 | 2 | 1920×1080 | 1920×1080 | `0x00800000` |
| GC573 | `/gc573_v4l2_shm` | 63 | 1 | 3840×2160 | 1920×1080 | `0x02000000` |

**Node allocation:** reserve `/dev/video60–69` for EverLive preload nodes.
GC570D uses 60/61 and GC573 uses 63; `/dev/video62` is currently unused and the
next recommended allocation is 64. A new card must use a `node_base` range that
does not overlap another preload descriptor or a real system node.

**Store geometry:** use three slots per port. The card-specific `frame_stride`
must hold the largest source YUY2 frame (`3840×2160×2 = 16,588,800` bytes for
4K). The `mmap_stride` must exceed the largest **converted** output frame so
virtual buffer offsets cannot overlap; 4K RGB32 is `33,177,600` bytes, so
GC573's `0x02000000` (33,554,432) is the current minimum-safe example. A 4K
single-port card therefore uses roughly 50 MB in `/dev/shm`.

The shared engine enumerates seven output formats and accepts the existing
YUY2/UYVY/NV12/YUV420/RGB24/BGR24/XBGR32/BGR32 conversion set. If the new card
needs a source or output format outside that set, extend the shared engine
instead of adding a private conversion layer. Branch B engines that receive
MJPEG or NV12 must decode/convert to YUY2 before publishing.

---

## 4. Hardware Engine Implementation

### 4.1 Branch A: Native PCIe via VFIO

For cards with native PCIe FPGA/ASIC DMA engines, follow strict resource management standards:

#### Robust VFIO Initialization with Full Error Checking
```c
#include <linux/vfio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

int ret = 0;
int container_fd = -1;
int group_fd = -1;
int device_fd = -1;
void *bar0 = MAP_FAILED;
struct vfio_region_info bar0_info = { 0 };

container_fd = open("/dev/vfio/vfio", O_RDWR);
if (container_fd < 0) {
    ret = -errno;
    goto fail;
}

if (ioctl(container_fd, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
    ret = -EPROTONOSUPPORT;
    goto fail;
}

if (ioctl(container_fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) <= 0) {
    ret = -ENODEV;
    goto fail;
}

char group_path[64];
snprintf(group_path, sizeof(group_path), "/dev/vfio/%d", iommu_group);
group_fd = open(group_path, O_RDWR);
if (group_fd < 0) {
    ret = -errno;
    goto fail;
}

/* Verify the IOMMU group is viable (all devices are bound to VFIO or unbound from host drivers) */
struct vfio_group_status group_status = { .argsz = sizeof(group_status) };
if (ioctl(group_fd, VFIO_GROUP_GET_STATUS, &group_status) < 0) {
    ret = -errno;
    goto fail;
}
if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
    fprintf(stderr, "IOMMU group %d is not viable (devices bound to conflicting host drivers)\n", iommu_group);
    ret = -EPERM;
    goto fail;
}

if (ioctl(group_fd, VFIO_GROUP_SET_CONTAINER, &container_fd) < 0) {
    ret = -errno;
    goto fail;
}

if (ioctl(container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0) {
    ret = -errno;
    goto fail;
}

device_fd = ioctl(group_fd, VFIO_GROUP_GET_DEVICE_FD, pci_bdf);
if (device_fd < 0) {
    ret = -errno;
    goto fail;
}

/* Enable PCI Bus Master and Memory Space in config space (required for FPGA DMA) */
struct vfio_region_info cfg = {
    .argsz = sizeof(cfg),
    .index = VFIO_PCI_CONFIG_REGION_INDEX,
};
uint16_t cmd = 0;
if (ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &cfg) == 0 &&
    pread(device_fd, &cmd, sizeof(cmd), cfg.offset + 0x04) == sizeof(cmd) &&
    !(cmd & 0x0004)) {
    cmd |= 0x0006; /* Bit 1: Memory Space, Bit 2: Bus Master */
    if (pwrite(device_fd, &cmd, sizeof(cmd), cfg.offset + 0x04) != sizeof(cmd)) {
        ret = -errno;
        goto fail;
    }
}

/* Map MMIO registers (BAR0) */
bar0_info.argsz = sizeof(bar0_info);
bar0_info.index = VFIO_PCI_BAR0_REGION_INDEX;
if (ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &bar0_info) < 0) {
    ret = -errno;
    goto fail;
}

/* Validate that BAR0 is non-empty and supports memory mapping */
if (bar0_info.size == 0 || !(bar0_info.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
    fprintf(stderr, "BAR0 region is invalid or does not support MMAP\n");
    ret = -ENODEV;
    goto fail;
}

bar0 = mmap(NULL, bar0_info.size, PROT_READ | PROT_WRITE,
            MAP_SHARED, device_fd, bar0_info.offset);
if (bar0 == MAP_FAILED) {
    ret = -errno;
    goto fail;
}

return 0;

fail:
    if (bar0 != MAP_FAILED) munmap(bar0, bar0_info.size);
    if (device_fd >= 0) close(device_fd);
    if (group_fd >= 0) close(group_fd);
    if (container_fd >= 0) close(container_fd);
    return ret;
```

#### Memory Pinning, RLIMIT_MEMLOCK, and `cap_ipc_lock`
Uncompressed 4K60 / 1440p120 video frames require substantial DMA buffer pools:
- One 4K frame in YUY2 format: `3840 * 2160 * 2 bytes = 16,588,800 bytes (15.82 MiB)`.
- 4 ring-buffer frames: `4 * 15.82 MiB = 63.28 MiB (66.36 MB)`.

> **Understanding the Memlock Architecture**:
> - `mmap(MAP_LOCKED)` requests the kernel to lock pages into RAM, but it does not bypass `RLIMIT_MEMLOCK`. If the process memory limit is exceeded, `mmap()` fails with `ENOMEM`.
> - During `VFIO_IOMMU_MAP_DMA`, the kernel IOMMU driver pins physical memory pages. If the process exceeds its locked memory allowance, `ioctl(VFIO_IOMMU_MAP_DMA)` fails with `ENOMEM`.
> - Setting file capability `cap_ipc_lock=+ep` on the engine binary allows unprivileged users to lock arbitrary memory ranges. In EverLive, this is paired with `ulimit -l unlimited` in `play.sh` and `/etc/security/limits.d/99-everlive-memlock.conf`.

Explicitly map host virtual memory into the IOMMU virtual address space (IOVA):
```c
struct vfio_iommu_type1_dma_map dma_map = {
    .argsz = sizeof(dma_map),
    .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
    .vaddr = (uint64_t)dma_virtual_addr,
    .iova  = dma_iova,
    .size  = dma_total_size,
};
if (ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
    fprintf(stderr, "VFIO_IOMMU_MAP_DMA failed: %s (check ulimit -l and cap_ipc_lock)\n",
            strerror(errno));
    return -errno;
}
```

#### Link Training & TMDS / FRL Equalization
High-bandwidth HDMI capture (>340 MHz TMDS clock for 4K60, or Fixed Rate Link for HDMI 2.1) is sensitive to cable quality and high-frequency attenuation:
- Calibrate the hardware receiver's Decision Feedback Equalization (DFE) and Adaptive Link Equalizer registers upon signal detection.
- Implement debounce hysteresis when polling clock detector status registers to prevent rapid HDMI disconnect loops during resolution transitions.

#### Teardown, Standby Handoff, and Reset Semantics
Orderly resource teardown requires distinguishing between normal shutdown and fatal fault recovery:

1. **Normal Session Teardown & Standby Daemon Handoff**:
   - Stop the HDMI receiver DMA engine and mask capture interrupts in device registers.
   - Nullify active device handles in the JSON-RPC dispatch thread before memory release.
   - **Do NOT trigger a PCIe Function Level Reset (FLR)** during normal teardown or standby daemon handoff. Preserving the PCIe device state keeps the HDMI PHY receiver locked and preserves DFE calibration, avoiding a 4-to-5 second link retraining cycle on the next viewer launch.
   - Unmap IOMMU DMA buffers via `VFIO_IOMMU_UNMAP_DMA`.
   - `munmap()` the BAR regions.
   - Close `device_fd`, `group_fd`, and `container_fd`.

2. **Fault Recovery / Full Uninstall Reset**:
   - If the FPGA or DMA controller enters an unrecoverable hung state (e.g. fatal DMA timeout), inspect reset capability via `ioctl(device_fd, VFIO_DEVICE_GET_INFO, &dev_info)` (ensuring `dev_info.argsz = sizeof(dev_info)` is set).
   - If `dev_info.flags & VFIO_DEVICE_FLAGS_RESET` is asserted, issue `ioctl(device_fd, VFIO_DEVICE_RESET)` to reset the PCIe function to its quiescent state.
   - If `ioctl(device_fd, VFIO_DEVICE_RESET)` returns `< 0` with `errno == ENOTTY` or `errno == EINVAL` (or if the reset flag is missing), hardware reset is not supported by the endpoint; fall back to the hardware-specific register soft-reset sequence.

---

### 4.2 Branch B: UVC / USB-Bridge Architecture (V4L2 + Extension Units)

When dealing with a UVC device behind a USB controller:

#### Basic V4L2 Streaming Lifecycle Skeleton
```c
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>

struct v4l2_buf_state {
    void  *start;
    size_t length;
};
struct v4l2_buf_state *buffers = NULL;
enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
bool streaming = false;
int ret = 0;

/* 1. Verify capability (single-planar video capture with streaming support) */
struct v4l2_capability cap;
if (ioctl(v4l2_fd, VIDIOC_QUERYCAP, &cap) < 0) {
    return -errno;
}
uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) {
    fprintf(stderr, "V4L2 node does not support single-planar video capture\n");
    return -ENODEV;
}
if (!(caps & V4L2_CAP_STREAMING)) {
    fprintf(stderr, "V4L2 node does not support streaming I/O\n");
    return -ENODEV;
}

/* 2. Negotiate format (MJPEG, YUYV, or NV12) */
struct v4l2_format fmt = {
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .fmt.pix.width       = 3840,
    .fmt.pix.height      = 2160,
    .fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG,
    .fmt.pix.field       = V4L2_FIELD_NONE,
};
if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
    return -errno;
}
if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG ||
    fmt.fmt.pix.width != 3840 || fmt.fmt.pix.height != 2160) {
    /* Driver clamped geometry or rejected requested compression format */
    fprintf(stderr, "Driver renegotiated unsupported format: 0x%08x (%ux%u, sizeimage %u)\n",
            fmt.fmt.pix.pixelformat, fmt.fmt.pix.width, fmt.fmt.pix.height,
            fmt.fmt.pix.sizeimage);
    return -EOPNOTSUPP;
}

/* 3. Request MMAP buffers (driver may adjust or return a higher count) */
struct v4l2_requestbuffers req = {
    .count  = 4,
    .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .memory = V4L2_MEMORY_MMAP,
};
if (ioctl(v4l2_fd, VIDIOC_REQBUFS, &req) < 0) {
    return -errno;
}
if (req.count < 2) {
    req.count = 0;
    ioctl(v4l2_fd, VIDIOC_REQBUFS, &req);
    return -ENOMEM;
}

/* Dynamically allocate tracking array matching driver's returned count */
buffers = calloc(req.count, sizeof(*buffers));
if (!buffers) {
    /* Release kernel buffer allocations if host tracking allocation fails */
    req.count = 0;
    ioctl(v4l2_fd, VIDIOC_REQBUFS, &req);
    return -ENOMEM;
}

/* 4. Query, mmap, and queue buffers */
for (unsigned int i = 0; i < req.count; i++) {
    struct v4l2_buffer buf = {
        .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
        .index  = i,
    };
    if (ioctl(v4l2_fd, VIDIOC_QUERYBUF, &buf) < 0) {
        ret = -errno;
        goto cleanup_buffers;
    }

    buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                            MAP_SHARED, v4l2_fd, buf.m.offset);
    if (buffers[i].start == MAP_FAILED) {
        ret = -errno;
        goto cleanup_buffers;
    }
    buffers[i].length = buf.length;

    if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
        ret = -errno;
        goto cleanup_buffers;
    }
}

/* 5. Start streaming */
if (ioctl(v4l2_fd, VIDIOC_STREAMON, &type) < 0) {
    ret = -errno;
    goto cleanup_buffers;
}
streaming = true;

return 0;

/* 6. Active capture loop (run in capture worker thread):
 *    - Dequeue: ioctl(v4l2_fd, VIDIOC_DQBUF, &buf)
 *    - Process / decode frame from buffers[buf.index].start (up to buf.bytesused bytes)
 *    - Requeue: ioctl(v4l2_fd, VIDIOC_QBUF, &buf)
 *    - On shutdown: ioctl(v4l2_fd, VIDIOC_STREAMOFF, &type)
 */

cleanup_buffers:
    if (streaming) {
        ioctl(v4l2_fd, VIDIOC_STREAMOFF, &type);
        streaming = false;
    }
    if (buffers) {
        for (unsigned int i = 0; i < req.count; i++) {
            if (buffers[i].start && buffers[i].start != MAP_FAILED) {
                munmap(buffers[i].start, buffers[i].length);
            }
        }
        free(buffers);
        buffers = NULL;
    }
    /* Release kernel-allocated DMA streaming buffers */
    req.count = 0;
    ioctl(v4l2_fd, VIDIOC_REQBUFS, &req);
    return ret;
```

> **Scoping & Thread State**: In an actual capture daemon, store `buffers`, `req.count`, and negotiated geometry (`sizeimage`, `width`, `height`) inside the engine's device state structure (`struct my_engine_context`) rather than local function scope so the background capture worker thread can access them.
>
> **Single-Planar vs. Multi-Planar**: The example above utilizes single-planar capture (`V4L2_BUF_TYPE_VIDEO_CAPTURE`). Multi-planar devices (such as certain camera ISP SoC nodes) use `V4L2_CAP_VIDEO_CAPTURE_MPLANE` and `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE` with `struct v4l2_plane`. Most USB/UVC capture dongles and HDMI-to-USB bridges expose standard single-planar nodes.
>
> **Production Implementation Note**: During graceful daemon shutdown, issue `ioctl(v4l2_fd, VIDIOC_STREAMOFF, ...)`, `munmap()` all active buffers, free the heap tracking array, and call `VIDIOC_REQBUFS` with `count = 0` to release allocated kernel DMA buffers.

#### USB 3.2 Bus Bandwidth & Format Constraints
USB 3.2 Gen 2 specifies 10 Gbps raw signaling. Accounting for 128b/132b line coding, the theoretical physical layer throughput is ~9.70 Gbps. However, protocol packet headers, link management, host xHCI controller scheduling, and USB bridge chipset characteristics dictate sustained application throughput:
- **1080p240 YUY2**: `1920 * 1080 * 240 * 16 bits = 7.96 Gbps`. On typical real-world USB host controllers and bridge chipsets, sustained uncompressed throughput is frequently bounded around ~7.3 to 8.0 Gbps (~220 FPS). Achieving jitter-free 240 FPS across varied host chipsets generally requires switching to compressed streams (e.g. NV12 or hardware MJPEG).
- **4K60 Uncompressed (YUY2/NV12)**: Uncompressed 4K60 YUY2 requires 7.96 Gbps, while uncompressed NV12 requires 5.97 Gbps. USB 3.2 Gen 1 (5 Gbps raw, ~3.2 Gbps effective) cannot carry either uncompressed stream; Branch B devices on 5 Gbps links must use compressed MJPEG.
- Consider offloading MJPEG / compressed decoding to GPU hardware decoders (VA-API `libva` or GPUJPEG) to prevent high CPU latency.

#### UVC Extension Units (XU) Control
```c
#include <linux/uvcvideo.h>
#include <linux/usb/video.h>

struct uvc_xu_control_query q = {
    .unit     = XU_VENDOR_UNIT_ID,
    .selector = XU_CMD_LED_CONTROL,
    .query    = UVC_SET_CUR,
    .size     = payload_len,
    .data     = payload_buf,
};
if (ioctl(v4l2_fd, UVCIOC_CTRL_QUERY, &q) < 0) {
    /* Handle extension unit error */
}
```

### 4.3 Publishing Frames to the Shared V4L2 Store

Any engine can optionally publish its final video frames into the shared
frame store. The store is the contract between the capture engine and the
preload DSO; the V4L2 state machine never touches BAR0, DMA rings or audio.

A small per-card module (for example `gc573_camera.[ch]`) should wrap the core
API:

```c
struct everlive_framestore_config cfg = {
    .name = "/gc<slug>_v4l2_shm",
    .pci_bdf = pci_bdf,
    .port_count = <ports>,
    .slot_count = 3,
    .frame_stride = <max_yuy2_frame_bytes>,
};
struct everlive_framestore *fs = everlive_framestore_create(&cfg);
```

Publisher contract:

- Create the store only after fatal hardware initialization succeeds, so early
  error paths do not need camera cleanup.
- Publish the **post-processing** frame: after HDR tonemapping, deinterlacing,
  scaling or any other operation the display and loopback consumers see. Do not
  publish raw HDR DMA frames; the preload feed would look washed out while the
  window looks correct.
- Publish the generated standby card with `no_signal = 1` at roughly 30 fps
  while capture is idle.
- Call `everlive_framestore_mark_no_signal()` or
  `everlive_framestore_reset()` when capture stops. A reset clears `seq` and
  sets `no_signal = 1`; a subsequent standby publish makes the outage visible.
- Keep the store mapped across S3 suspend/resume; reset it and republish after
  the hardware reinitialises. Do not unlink and recreate it during recovery.
- Do **not** add a background heartbeat thread. Liveness comes only from
  successful publishes; a dead or stalled publisher must eventually produce the
  black-frame timeout in the preload reader.
- Use `everlive_framestore_read_begin()` + `read_validate()` for zero-copy
  consumers such as the V4L2 shim. Use `read_snapshot()` when the consumer wants
  a private stable copy.
- Keep the raw `mmap`/`munmap`/`close` syscalls inside `everlive_framestore.c`.
  Replacing them with libc calls reintroduces the deadlock that occurs when the
  preload shim attaches a store while holding its own internal mutex.
- One publisher per card model is supported. Two simultaneous instances of the
  same model share one store name and are not currently isolated.

The store payload is YUY2 by design. Branch B engines that capture MJPEG/NV12
must decode/convert to YUY2 before calling `everlive_framestore_publish()`.

---

## 5. Control Plane & Unified JSON-RPC 2.0 Protocol

EverLive requires all capture engines to expose an identical newline-delimited JSON-RPC 2.0 interface over the local Unix domain socket.

### 5.1 Socket Path Resolution & Security
Always resolve paths using `everlive_ipc_prepare_socket_path()` (for servers) or `everlive_ipc_resolve_socket_path()` (for clients):

```c
#include "everlive_ipc.h"

char sock_path[EVERLIVE_IPC_PATH_MAX];
if (everlive_ipc_prepare_socket_path(sock_path, sizeof(sock_path), NULL) == 0) {
    /* Binds to /run/user/<uid>/everlive.sock or $XDG_RUNTIME_DIR/everlive.sock */
    bind_unix_socket(sock_path);
}
```

The resolver evaluates locations in strict precedence:
1. Explicit caller override path parameter.
2. `$EVERLIVE_SOCKET` environment variable (primary override used by `everlive_client.py`).
3. `$EVERLIVE_CONTROL_SOCKET` environment variable (legacy override).
4. `$XDG_RUNTIME_DIR/everlive.sock` (if set and writable).
5. `/run/user/<uid>/everlive.sock` (if writable).
6. `/tmp/everlive-<uid>/everlive.sock`

**Security & Single-Instance Guards**:
- Acquire an advisory lock (`flock(lock_fd, LOCK_EX | LOCK_NB)`) on a lockfile to prevent multiple conflicting daemons from binding the same socket.
- Verify connecting clients using `SO_PEERCRED` to ensure only the exact same UID as the daemon process is accepted (`cred.uid == getuid()`). Requests from differing UIDs (including root when the daemon runs as an unprivileged user) are rejected.

### 5.2 Method Dispatch Contract

EverLive partitions JSON-RPC 2.0 methods into two categories:

#### Core Methods (Mandatory for ALL Engines)
These methods must be implemented by every capture engine without exception:

| Method | Parameters | Return / Description |
|---|---|---|
| `hello` | None | Handshake & capability discovery: returns `{"proto": 1, "app": "everlive", "version": "...", "pid": N, "socket": "...", "methods": ["hello", "ping", "status.get", ...]}` |
| `ping` | None | Keep-alive: returns `"pong"` |
| `status.get` | None | Full telemetry object (must include all 18 canonical keys) |

> **Capability Discovery via `hello.methods[]`**:
> The `methods` array returned by `hello` is the primary dynamic discovery mechanism of the protocol. Clients inspect this list on connection to determine which features the active engine supports.

#### Feature Methods (Hardware-Dependent)
These methods are implemented when supported by the hardware, and must return error `-32601` ("Method not found") when unsupported:

| Method | Parameters | Supported Values / Notes | Engine Availability |
|---|---|---|---|
| `led.set` | `{"r": N, "g": N, "b": N}` | RGB color (0–255 per channel) | GC573, GC570D |
| `led.mode.set` | `{"mode": "..."}` | GC573: `static\|wave\|rainbow\|off`<br>GC570D: `off\|static\|breathing\|colorful` | GC573, GC570D |
| `audio.volume.set` | `{"gain": 1.0}` | Capture volume gain (`0.0` to `1.5`, clamped) | GC573, GC570D |
| `audio.mute.set` | `{"muted": true}` | Audio mute toggle (`true`/`false`) | GC573, GC570D |
| `audio.channels.set`| `{"mode": "auto"}` | Layout: `auto\|stereo\|5.1\|7.1` (GC573); `auto\|stereo` (GC570D) | GC573, GC570D |
| `osd.set` | `{"enabled": true}` | On-screen display toggle | GC573, GC570D |
| `osd.position.set` | `{"pos": "tr"}` | OSD placement: `tr`, `br`, `bl`, `tl` (or `top-right`, etc.) | GC573, GC570D |
| `aspect.set` | `{"mode": "auto"}` | Aspect ratio: `auto`, `16:9`, `4:3`, `stretch` | GC573, GC570D |
| `scale.set` | `{"mode": "fit"}` | Scaling: `fit`, `integer` | GC573, GC570D |
| `mode.set` | `{"mode": "gaming"}` | Modes: `gaming`, `headless` | GC573 only (GC570D -> `-32601`) |
| `v4l2.set` | `{"enabled": true}` | Toggle V4L2 loopback presentation stream (optional `device`) | GC573 only (GC570D -> `-32601`) |
| `hdr.set` | `{"mode": "auto"}` | Modes: `auto`, `on`, `off`, `passthrough` | GC573 only (GC570D -> `-32601`) |
| `hdr.passthrough` | `{"passthrough": true}` | Control direct HDR passthrough | GC573 only (GC570D -> `-32601`) |
| `hdr.exposure` | `{"ev": 0.0}` | Tone mapping exposure compensation | GC573 only (GC570D -> `-32601`) |
| `framegen.set` | `{"mode": "60to120", "flow_scale": 1.0}` | LSFG-VK modes: `off`, `60to120`, `30to60` (`flow_scale`: 0.5–1.0) | GC573 only (GC570D -> `-32601`) |
| `toast.show` | `{"message": "..."}` | Render transient OSD notification banner | GC573 only (GC570D -> `-32601`) |
| `input.set` | `{"port": 1}` | Input selection: `1` (Main) or `2` (Sub) | GC570D only (GC573 -> `-32601`) |
| `pip.set` | `{"enabled": true}` | Picture-in-picture mode toggle | GC570D only (GC573 -> `-32601`) |
| `edid.set` | `{"profile": "default"}` | EDID configuration profile | GC573, GC570D |
| `hotplug` | `{"pulse": true}` | GC573: `{"pulse": bool}` or `{"action": "on\|off"}`<br>GC570D: `{"port": 1\|2}` | GC573, GC570D |

### 5.3 Canonical `status.get` Response Schema

To ensure compatibility with the PySide6 Console Hub, `status.get` must return all **18 canonical top-level keys**:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "proto": 1,
    "version": "1.5.0",
    "pid": 21080,
    "headless": false,
    "streaming": true,
    "signal": {
      "locked": true,
      "width": 3840,
      "height": 2160,
      "interlaced": false,
      "rate_hz": 59.94,
      "pixel_clock_khz": 594000
    },
    "video": {
      "fps": 59.94,
      "frames": 1200,
      "ring_buffers": 4,
      "ring_channels": 3,
      "output_width": 3840,
      "output_height": 2160
    },
    "audio": {
      "state": "lpcm",
      "label": "L-PCM 48kHz 7.1",
      "rate_hz": 48000,
      "channels": 8,
      "out_channels": 8,
      "layout": "auto",
      "bits": 16,
      "gain": 1.000,
      "muted": false
    },
    "v4l2": {
      "path": "/dev/video10",
      "configured": true,
      "width": 3840,
      "height": 2160,
      "frames": 1200,
      "dropped": 0
    },
    "osd": {
      "enabled": true,
      "position": "top-right"
    },
    "aspect": "auto",
    "scale": "fit",
    "vsync": false,
    "fullscreen": false,
    "framegen": {
      "mode": "off",
      "present_fps": 0.0,
      "output_fps": 0.0,
      "unique_fps": 0.0,
      "raw_fps": 0.0,
      "flow_scale": 1.0,
      "perf_mode": false,
      "low_latency": true,
      "layer_found": true,
      "dll_found": true,
      "dll_path": "",
      "vulkan_active": false,
      "deduplicator_active": false
    },
    "led": {
      "r": 168,
      "g": 85,
      "b": 247
    },
    "clients": 1,
    "age_ms": 12
  }
}
```

### 5.4 Capability Probing & Error -32601
If a feature method is not supported by your hardware (e.g. HDR on an SDR card, or PIP on a single-input card), do not invent dummy replies. Return standard JSON-RPC 2.0 error `-32601` ("Method not found"):

```json
{"jsonrpc": "2.0", "id": 1, "error": {"code": -32601, "message": "Method not supported on this card"}}
```
Clients and utilities (such as `everlive-ctl`) inspect the declared `methods[]` array from `hello` and treat `-32601` as the protocol signal that a given feature is unsupported on the active engine, gracefully disabling or skipping corresponding controls.

### 5.5 V4L2 Preload vs. `v4l2loopback`

These are two separate output paths:

- `v4l2.set` controls the `/dev/video10`/`/dev/video11` **v4l2loopback**
  sink. It is a runtime JSON-RPC mutation.
- The preload DSO is selected at process launch with `LD_PRELOAD`; it is not a
  JSON-RPC method and does not create a system-wide node.
- The virtual `/dev/videoN` from a preload DSO exists only inside the process
  that loaded it, and only when the per-process opt-in allows it:
  `EVERLIVE_V4L2_TARGET` may contain a comma-separated list of executable
  basenames (or `*`).
- No udev rule, kernel module, VFIO binding or modprobe change is needed for the
  preload node. It is injected through the shared interposer.
- Load only one card's preload DSO in a given process. The DSOs export the same
  interposition symbols.

---

## 6. System Configuration & Packaging

Packaging rules differ depending on the card's transport branch:

### 6.1 Udev Rules (`packaging/70-everlive.rules`)

#### For Branch A (Native PCIe / VFIO):
Match by vendor, device, and subsystem IDs to prevent misbinding cards that share device IDs:
```udev
# Example: Match specific Branch A card by subsystem
ACTION=="add", SUBSYSTEM=="pci", ATTR{vendor}=="0x1461", ATTR{device}=="0x0054", ATTR{subsystem_vendor}=="0x1461", ATTR{subsystem_device}=="0x5730", ATTR{driver_override}="vfio-pci"

# Global VFIO device permissions
SUBSYSTEM=="vfio", MODE="0660", TAG+="uaccess", GROUP="video"
```

> **Repository Rules vs. Subsystem Discrimination**: The currently shipped `packaging/70-everlive.rules` binds `0x1461:0x0054` and `0x1461:0x0055` at the vendor+device level because the hardware targets configured in this repository (the verified GC573 and GC570D, plus the unverified Live Gamer BOLT (GC555) preview template) are native PCIe/Thunderbolt endpoints requiring `vfio-pci`. Distinction between specific models sharing IDs occurs in userspace (`play.sh` and `everlive-ctl`) via sysfs subsystem inspection. When introducing a Branch B device sharing a device ID with Branch A hardware, subsystem-qualified udev rules or explicit userspace binding via `vfio_pci_bind.sh` must be used to avoid inadvertently capturing the USB host controller into VFIO.
>
> **Driver Override vs. Active Binding**: Writing to `driver_override` registers the preferred driver for subsequent kernel probe events. If the device was already claimed by another driver or booted without an automatic probe, `tools/opengc*-userd/vfio_pci_bind.sh` unbinds it from `/sys/bus/pci/devices/<BDF>/driver/unbind` and writes to `/sys/bus/pci/drivers/vfio-pci/bind` to execute the active binding at runtime.

#### For Branch B (UVC / USB Bridge):
**Do not set driver_override="vfio-pci"**. Grant permissions on the video nodes instead:
```udev
# Verify USB idVendor and idProduct via 'lsusb' (not necessarily the PCIe vendor ID)
KERNEL=="video[0-9]*", SUBSYSTEM=="video4linux", ATTRS{idVendor}=="07ca", MODE="0660", TAG+="uaccess", GROUP="video"
```

### 6.2 Early Kernel Module Binding (`packaging/everlive-vfio.modprobe.conf`)
Add Device IDs for **Branch A devices only**:
```conf
options vfio-pci ids=1461:0054,1461:0055
```
> **Shared Device ID Precaution**: Note that `options vfio-pci ids=` matches globally on Vendor:Device without subsystem awareness. The shipped configuration binds `1461:0054` (GC573, GC570D) and `1461:0055` (Live Gamer BOLT GC555 preview). If two physical models share the same Device ID (such as `1461:0054`), `ids=` binds both indiscriminately. If one of those devices must remain bound to a different driver or requires selective handling, omit it from `everlive-vfio.modprobe.conf` and rely exclusively on subsystem-specific udev rules and `vfio_pci_bind.sh`. Never add Branch B devices here; the kernel must bind them to `xhci_hcd` and `uvcvideo`.

### 6.3 Launcher & Service Integration

When adding a new capture engine, update the launcher, service scripts, and UI whitelists across these files:

1. **`play.sh` Launcher**:
   - `detect_card_sysfs()`: Add the new PCIe Device ID to the device loop and map the subsystem ID to the model slug.
   - Model Whitelist (around line 58): Update `case "${CARD_MODEL}" in gc573|gc570d|<new_model>) ;;`.
   - Routing Dispatch: Configure engine directory, binary name, binding script, and loopback labels:
     ```bash
     # Native PCIe / VFIO engine (e.g. GC570D):
     if [ "$CARD_MODEL" = "gc570d" ]; then
         USERD_DIR="${GC570D_DIR}"
         STREAMER_BIN="${USERD_DIR}/opengc570d-userd"
         BIND_SCRIPT="${USERD_DIR}/gc570d-vfio-bind.sh"
         LOOPBACK_LABEL="EverLive Capture"
         LOOPBACK_LABEL_2="EverLive Capture HDMI 2"
     fi

     # Future UVC / USB-Bridge engine (e.g. external USB capture):
     if [ "$CARD_MODEL" = "gc553" ]; then
         USERD_DIR="${GC553_DIR}"
         STREAMER_BIN="${USERD_DIR}/gc553_user_streamer"
         BIND_SCRIPT=""   # UVC devices operate via V4L2; no VFIO binding script
         LOOPBACK_LABEL="EverLive Capture"
         LOOPBACK_LABEL_2=""
     fi
     ```
2. **`tools/libeverlive-core/everlive-service.sh`**:
   - Updates `detect_card_sysfs()` and the `case "${CARD_MODEL}" in ...` whitelist to allow systemd background daemon launches.
3. **`tools/everlive/everlive_viewer.py`**:
   - The GUI hub queries `GC573_DEVICE_IDS` for model-specific UI controls. Add new model mappings to enable tailored settings panels.
4. **Standalone Installation (`install.sh` / `uninstall.sh`)**:
   - Ensure `install.sh` and `uninstall.sh` copy the new engine binary and setup required permissions.

*(Repository Reference: In the EverLive source tree, the GC573 engine uses `tools/opengc573-userd/vfio_pci_bind.sh`, while the GC570D uses `tools/opengc570d-userd/gc570d-vfio-bind.sh`.)*

### 6.4 Root `Makefile`
Wire the new engine into the root build orchestration:
```makefile
GC575_DIR := tools/opengc575-userd

all: core gc573 gc570d gc575

gc575: core
	+$(MAKE) -C $(GC575_DIR)

install: all
	+$(MAKE) -C $(GC575_DIR) install-bin PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)
```

> **Makefile Convention**: New capture engines should expose an `install-bin` target (the modern GC570D convention). The GC573 `install` target is historical. When a preload DSO exists, `install-bin` must also install it to `$(PREFIX)/lib`.

### 6.5 Preload DSO Build and Packaging

A per-card preload DSO is built from four inputs:

1. the card descriptor (`src/<slug>-v4l2.c`),
2. `tools/libeverlive-core/v4l2/everlive_v4l2_state.c`,
3. `tools/libeverlive-core/v4l2/everlive_v4l2_preload.c`,
4. the core frame-store object (`libeverlive_core.a`; the linker pulls only
   the required frame-store object).

```make
lib<slug>-v4l2.so: src/<slug>-v4l2.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_state.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_preload.c \
		$(CORE_LIB)
	$(CC) -O2 -g -Wall -Wextra -Wno-unused-parameter \
		-std=gnu11 -D_GNU_SOURCE -shared -fPIC \
		-Isrc -I$(CORE_DIR) -I$(CORE_V4L2_DIR) \
		-o $@ src/<slug>-v4l2.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_state.c \
		$(CORE_V4L2_DIR)/everlive_v4l2_preload.c \
		$(CORE_LIB) -ldl -lrt -pthread
	@chmod 0644 $@
```

Packaging rules:

- Install the DSO to `$(PREFIX)/lib`, not a private subdirectory. With
  `PREFIX=/usr`, both current DSOs live at `/usr/lib/libgc570d-v4l2.so` and
  `/usr/lib/libgc573-v4l2.so`. Do not keep a second copy under
  `$(PREFIX)/lib/everlive`.
- Add the DSO to the engine's `install`/`install-bin` and `uninstall` targets.
- Add the build output to `.gitignore`.
- Add `/dev/shm/<slug>_v4l2_shm` removal to `uninstall.sh`.
- Keep the top-level `Makefile` install/uninstall path in sync so packaging
  (`make DESTDIR=... PREFIX=/usr install`) carries the DSO automatically.
- `play.sh` does not load the preload DSO; the user selects it when launching
  the consuming application (OBS, Discord, Chromium, etc.).
- Do not put the interposition object into `libeverlive_core.so`; it would
  hijack `open`/`ioctl`/`mmap` for every daemon and CLI tool.
- No udev or modprobe files are involved. The node is process-local.

Staged verification:

```bash
stage="$(mktemp -d)"
make DESTDIR="$stage" PREFIX=/usr install
ls -l "$stage/usr/lib/libgc570d-v4l2.so" "$stage/usr/lib/libgc573-v4l2.so"
make DESTDIR="$stage" PREFIX=/usr uninstall
```

---

## 7. Unit Testing & Verification Workflow

EverLive enforces rigorous test-driven validation.

### 7.1 Unit Tests in `tests/test_card_registry.c`

Add tests asserting descriptor matching and sysfs detection:

```c
/* Test matching logic against template/placeholder */
const struct everlive_device_desc *card =
    everlive_match_card(0x1461, 0x0057, 0x0000);
assert(card != NULL);
assert(strcmp(card->model_slug, "gc575") == 0);
assert(card->capabilities & EVERLIVE_CAP_1440P120);
TEST_PASS("Identified GC575 device ID 0x0057 with 1440p120 template");

/* Test mock sysfs autodiscovery (using mkdtemp test harness) */
char tmp_root[] = "/tmp/everlive_sysfs_test_XXXXXX";
char *res = mkdtemp(tmp_root);
assert(res != NULL);

char dev_dir[512], p[PATH_MAX];
snprintf(dev_dir, sizeof(dev_dir), "%s/0000:03:00.0", tmp_root);
assert(mkdir(dev_dir, 0755) == 0);

snprintf(p, sizeof(p), "%s/vendor", dev_dir);
create_file(p, "0x1461\n");
snprintf(p, sizeof(p), "%s/device", dev_dir);
create_file(p, "0x0057\n");
snprintf(p, sizeof(p), "%s/subsystem_device", dev_dir);
create_file(p, "0x0000\n");

/* Clean up temporary mock sysfs directory */
nftw(tmp_root, remove_entry, 64, FTW_DEPTH | FTW_PHYS);
```

### 7.2 Verification Commands

```bash
# 1. Compile everything and run the 9-suite unit test matrix
make test

# 2. Run AddressSanitizer and UndefinedBehaviorSanitizer
make asan

# 3. Test staged installation and clean uninstallation
make install DESTDIR=/tmp/test-stage PREFIX=/usr
make uninstall DESTDIR=/tmp/test-stage PREFIX=/usr

# 4. Verify live hardware detection
./tools/libeverlive-core/everlive-ctl detect
./tools/libeverlive-core/everlive-ctl status
```

### 7.3 Preload and Frame-Store Tests

The shared suite already provides:

- `tests/test_framestore.c` — publish/snapshot, slot rotation, invalid
  mappings, heartbeat, flags, reset and a threaded torn-frame stress test.
- `tests/test_v4l2_shim.c` — the V4L2 state machine without libc
  interposition: QUERYCAP, formats, S_FMT clamping, REQBUFS/QUERYBUF/QBUF/
  DQBUF, STREAMON/OFF, mmap resolution, read path, duplicate and close.

A new descriptor should add assertions for its identity and layout. The
simplest pattern is a dedicated test target that links the new
`src/<slug>-v4l2.c` with `everlive_v4l2_state.c` and asserts:

- node base and port count,
- driver/card names and bus-info fallback/BDF suffix,
- maximum and default geometry,
- `sizes[]` and `fps_denominators[]` enumeration,
- clamp buckets,
- mmap base/stride and buffer offsets.

Synthetic preload smoke test:

```bash
# 1. Create /dev/shm/<slug>_v4l2_shm with the core API and publish a frame.
# 2. Then exercise the card DSO:
LD_PRELOAD=$PWD/tools/opengc<slug>-userd/lib<slug>-v4l2.so \
    v4l2-ctl --list-devices
LD_PRELOAD=$PWD/tools/opengc<slug>-userd/lib<slug>-v4l2.so \
    v4l2-ctl -d /dev/videoN --all
LD_PRELOAD=$PWD/tools/opengc<slug>-userd/lib<slug>-v4l2.so \
    v4l2-ctl -d /dev/videoN --list-formats-ext
LD_PRELOAD=$PWD/tools/opengc<slug>-userd/lib<slug>-v4l2.so \
    v4l2-ctl -d /dev/videoN \
        --set-fmt-video=width=1920,height=1080,pixelformat=YUYV \
        --stream-mmap --stream-count=10
```

`v4l2-ctl --all` depends on the shared `/sys/dev/char/<major>:<minor>/uevent`
and `fopen` interposition; do not reimplement those in a card descriptor.

Also verify the failure paths: no store/publisher (black frame), stale
heartbeat after `kill -9`, no-signal standby publication, and process-local
node visibility (the node must not appear in a process without `LD_PRELOAD`).

---

## 8. Hardware Validation & Evidence Standards

Before a Pull Request is merged, new capture hardware must clear this evidence standard:

| Capability | Verification Requirement |
|---|---|
| **Hardware Detection** | `everlive-ctl detect` outputs accurate BDF, model slug, and IOMMU group (PCI hardware). |
| **Video Ingestion** | Continuous streaming at 60 FPS baseline AND at the card's maximum claimed resolution/refresh rate (e.g. 1080p240 or 4K120) without dropped frames or tearing for >= 30 minutes. |
| **Module-free V4L2 preload** | Under `LD_PRELOAD`, the virtual node appears with the expected number/name/bus info; 4K/1440p/1080p negotiation works; YUYV plus at least one converted format streams; standby frames appear with no signal; `kill -9` of the publisher produces black after the heartbeat timeout; OBS/Discord/Chromium can open the node. The node must not exist in processes without the preload. |
| **Suspend / resume** | Suspend and resume with a live preload consumer. The publisher reinitialises, `/dev/shm/<slug>_v4l2_shm` resumes publishing, and the consumer recovers without restarting the engine. |
| **High-Bandwidth Link (EQ / TMDS / FRL)** | - **HDMI 2.0**: 4K60 RGB or 1440p120 (>340 MHz TMDS clock) capture verified for >= 30 minutes with zero Character Error Detection (CED) errors, no visual sparkles, and no audio crackle.<br>- **HDMI 2.1**: Verified Fixed Rate Link (FRL) link training (e.g. 3L/3G up to 4L/12G), error status register monitoring, and sustained capture at maximum advertised FRL modes (e.g. 4K120 / 4K144 HDR) without link retraining. |
| **Audio Pipeline** | Clean L-PCM / bitstream audio capture routed into PipeWire without buffer underruns, with A/V sync drift < 10 ms over a 30-minute run. |
| **Passthrough** | HDMI pass-through signal and low latency verified on an external display. |
| **EDID Handling** | Valid EDID handshake with source consoles (PS5, Xbox Series X, Switch, PC). |
| **Thermal / Stability** | No memory leaks or DMA buffer stalls under continuous 4K capture. |
| **Clean Shutdown** | Terminating `play.sh` releases all resources (VFIO groups on Branch A; V4L2 streaming buffers on Branch B) and allows clean restart without reboot. |

Support tiers in documentation:
- **Verified**: Confirmed on real hardware across all verification requirements.
- **Partially Verified**: Core capture functioning; optional features (e.g. HDR, EDID bridge) undergoing testing.
- **Untested / Community Preview**: Declarative descriptor added in `libeverlive-core`; hardware testing in progress.

---

## 9. Contributor Checklist

Ensure all items are verified before submitting a pull request:

- [ ] Hardware architecture evaluated (Branch A Native PCIe DMA vs. Branch B UVC Bridge).
- [ ] Device descriptor added to `kEverliveCardRegistry[]` in `tools/libeverlive-core/everlive_card.c`.
- [ ] Subsystem ID differentiation confirmed if Device ID is shared with another model.
- [ ] Ops vtable (`struct everlive_card_ops`) registered before thread startup without NULL callbacks.
- [ ] Engine daemon implements the unified JSON-RPC 2.0 contract (`hello`, `ping`, `status.get`, etc.).
- [ ] Canonical `status.get` schema matches all 18 top-level keys.
- [ ] Unsupported commands return error code `-32601`.
- [ ] Memory locking limits (`cap_ipc_lock` / `RLIMIT_MEMLOCK`) configured (Branch A).
- [ ] Udev rules match subsystems accurately (and avoid `vfio-pci` on Branch B devices).
- [ ] `packaging/everlive-vfio.modprobe.conf` contains PCI IDs for Branch A devices only.
- [ ] Top-level `play.sh` routes the model slug to the engine binary.
- [ ] Root `Makefile` supports `all`, `install`, `uninstall`, and `clean` with `DESTDIR`/`PREFIX`.
- [ ] Unit tests added to `tests/test_card_registry.c`.
- [ ] `make test` and `make asan` pass 100% with zero memory leaks.
- [ ] Hardware capabilities documented in `README.md` with model slug and support tier.
- [ ] `CHANGELOG.md` updated under `[Unreleased]`.

### Optional Module-Free V4L2 Preload Checklist

Only needed when the engine opts into the preload path:

- [ ] Per-card `<slug>-v4l2.c` descriptor added; shared engine sources reused, not copied.
- [ ] `shm_name`, `slug`, `driver`, `card_name`, port names and bus-info fallback/suffix configured.
- [ ] `node_base`/`port_count` use the reserved 60–69 range without collisions.
- [ ] `max_width/height`, default geometry, sizes, fps and clamp buckets match the hardware.
- [ ] Store uses three slots and a `frame_stride` covering the largest YUY2 frame.
- [ ] `mmap_stride` covers the largest converted output (4K RGB32 needs >= 33,177,600 bytes).
- [ ] Publisher creates the store after fatal init, publishes post-tonemap frames and standby no-signal frames, and resets/marks no-signal on capture stop.
- [ ] Publisher keeps the store mapped across S3 resume and does not add a background heartbeat thread.
- [ ] DSO built from the shared `libeverlive-core/v4l2` engine and installed to `$(PREFIX)/lib`.
- [ ] `.gitignore`, `install.sh`/`uninstall.sh`, top-level `Makefile`, and `/dev/shm/<slug>_v4l2_shm` cleanup updated.
- [ ] Descriptor/state tests and synthetic `LD_PRELOAD` (`v4l2-ctl`) checks pass.
- [ ] `EVERLIVE_V4L2_TARGET` opt-in behavior verified for child processes.
- [ ] The preload node is verified to be process-local and absent without `LD_PRELOAD`.
