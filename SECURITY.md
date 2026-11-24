# Security Policy

## Supported Versions

| Version / Branch | Supported |
|:---|:---|
| `v1.4.x` | :white_check_mark: |
| `v1.3.x` | :white_check_mark: |
| `main` | :white_check_mark: |
| `v1.2.x` and older | :x: |
| Legacy (`open_gc573.ko` kernel module) | :x: *(Deprecated & Superseded by Userspace VFIO)* |

---

## Security Model (Userspace VFIO & Hardware IOMMU Isolation)

EverLive operates **100% in unprivileged userspace (Ring 3)** via the upstream Linux Virtual Function I/O framework (`vfio-pci`):

* **IOMMU Memory Safety:** DMA transfers are strictly governed and bounded by the system's hardware IOMMU (Intel VT-d / AMD-Vi). The hardware capture card cannot access or corrupt arbitrary kernel memory or other processes' memory spaces.
* **Non-Root Execution:** EverLive does not require root privileges or setuid binaries to run. Access is granted per card: only the matched card's `/dev/vfio/<iommu_group>` node is set to `root:video 0660`, and `/dev/vfio/vfio` is restricted by a single container-node udev rule. `cap_ipc_lock=+ep` on the engine binaries provides DMA page pinning; a bounded 1 GiB `memlock` fallback is installed only when `setcap` is unavailable. No global `vfio-pci ids=` or `new_id` binding is used.
* **Kernel Integrity:** EverLive does not load out-of-tree kernel modules, does not taint the kernel (`sys_init_module`), and is fully compatible with strict UEFI Secure Boot policies.
* **Local IPC Isolation:** Control sockets and the shared-memory frame store live in private `0700` runtime directories owned by the engine UID. Foreign or permissive fallback directories abort startup; lock files are opened `O_NOFOLLOW`; Unix-socket peers are checked with `SO_PEERCRED`; and frame-store objects must be `0600` and owned by the engine UID. Raw diagnostic commands (`reinit`, `action`) require `EVERLIVE_DEBUG_CONTROL=1` in the daemon environment.

---

## Reporting a Vulnerability

If you discover a security vulnerability (such as an IOMMU mapping boundary flaw, buffer overflow in the bitstream audio unpacker, or privilege escalation vector), please do not disclose it publicly in an open issue.

Instead, please report it via:
* **GitHub Private Vulnerability Reporting:** Navigate to the **Security** tab of the [EverLive repository](https://github.com/Everlite/EverLive/security) and click **Report a vulnerability**.
* Or contact the project maintainer directly through GitHub.

We will review security reports promptly and coordinate a patch and advisory as quickly as possible.
