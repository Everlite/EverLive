# OpenGC573 Userspace Driver (VFIO & PipeWire Architecture)

> [!WARNING]
> **Branch `intern-test` — Highly Experimental (Not recommended for daily gaming yet)**  
> This userspace driver implementation is under active development. For daily stable gaming on PlayStation 5 / PC, please remain on the `main` branch (`git checkout main`).

---

## 💡 Overview & Architecture

Modern Linux gaming and competitive titles increasingly mandate strict UEFI Secure Boot (e.g. Anti-Cheat EAC v2 / Vanguard). In kernel lockdown mode, unsigned or custom out-of-tree kernel modules (`open_gc573.ko`) can be blocked.

The **OpenGC573 Userspace Driver** runs 100% in userspace (Ring 3) without requiring any custom kernel module:
1. **PCIe Access via Mainline `vfio-pci`:** The capture card (`0000:0a:00.0`) is attached to Linux's standard in-tree VFIO driver.
2. **Userspace MMIO via `mmap()`:** The Artix-7 FPGA BAR0 register space is directly mapped into process virtual memory.
3. **Userspace I2C Subsystem:** The IT6805 (HDMI 2.0 Receiver), IT6664 (Transmitter/Passthrough), and RGB LED microcontroller are driven directly via register transfers on FPGA I2C master blocks.
4. **Zero-Copy DMA via IOMMU:** Hardware DMA ring descriptors point to userspace buffers registered via `VFIO_IOMMU_MAP_DMA`.
5. **Event-Driven Interrupts:** Hardware MSI interrupts are forwarded directly to userspace via `eventfd` and Linux `poll()`.

---

## 🛠️ Components & Tools

| Tool / File | Purpose |
|:---|:---|
| **`vfio_pci_bind.sh`** | Safe switcher script to toggle between kernel driver (`open_gc573.ko`) and userspace mode (`vfio-pci`). |
| **`gc573_vfio.c` / `.h`** | VFIO container, group, device, and IOMMU DMA mapping layer. |
| **`gc573_user_i2c.c` / `.h`** | Userspace I2C bitbang/controller implementation for IT6805, IT6664, and RGB MCU. |
| **`gc573_user_probe.c`** | Hardware probe utility validating MMIO, I2C discovery, IOMMU DMA allocation, and MSI interrupts. |

---

## 🚀 Quickstart & Testing

### 1. Build the userspace probe tool:
```bash
make -C tools/opengc573-userd
```

### 2. Switch card to VFIO mode:
```bash
sudo ./tools/opengc573-userd/vfio_pci_bind.sh --bind
```

### 3. Run hardware probe (without root):
```bash
./tools/opengc573-userd/gc573_user_probe
```

### 4. Switch back to stable kernel driver for gaming:
```bash
sudo ./tools/opengc573-userd/vfio_pci_bind.sh --restore-kernel
```
