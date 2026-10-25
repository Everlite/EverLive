# Security Policy

## Supported Versions

| Version / Branch | Supported |
|:---|:---|
| `v1.1.x` | :white_check_mark: |
| `main` | :white_check_mark: |
| `v1.0.x` | :white_check_mark: |
| Legacy (`open_gc573.ko` kernel module) | :x: *(Deprecated & Superseded by Userspace VFIO)* |

---

## Security Model (Userspace VFIO & Hardware IOMMU Isolation)

EverLive operates **100% in unprivileged userspace (Ring 3)** via the upstream Linux Virtual Function I/O framework (`vfio-pci`):

* **IOMMU Memory Safety:** DMA transfers are strictly governed and bounded by the system's hardware IOMMU (Intel VT-d / AMD-Vi). The hardware capture card cannot access or corrupt arbitrary kernel memory or other processes' memory spaces.
* **Non-Root Execution:** EverLive does not require root privileges or setuid binaries to run. Access to the card is granted via standard Linux group permissions (`/etc/udev/rules.d/70-everlive.rules`) and bounded memory-locking limits (`CAP_IPC_LOCK` / `RLIMIT_MEMLOCK`).
* **Kernel Integrity:** EverLive does not load out-of-tree kernel modules, does not taint the kernel (`sys_init_module`), and is fully compatible with strict UEFI Secure Boot policies.

---

## Reporting a Vulnerability

If you discover a security vulnerability (such as an IOMMU mapping boundary flaw, buffer overflow in the bitstream audio unpacker, or privilege escalation vector), please do not disclose it publicly in an open issue.

Instead, please report it via:
* **GitHub Private Vulnerability Reporting:** Navigate to the **Security** tab of the [EverLive repository](https://github.com/Everlite/EverLive/security) and click **Report a vulnerability**.
* Or contact the project maintainer directly through GitHub.

We will review security reports promptly and coordinate a patch and advisory as quickly as possible.
