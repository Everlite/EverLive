# Contributing to EverLive

Contributions, bug reports, and hardware testing feedback are warmly welcomed! Whether you want to report an issue with a scaler modeline, submit an optimization for frame pacing, or add support for other distributions, here is how you can help.

---

## 🛠️ How to Report Bugs

If you encounter an issue, please open an issue using our [Bug Report Template](.github/ISSUE_TEMPLATE/bug_report.md).
Please provide:
1. Your distribution, desktop environment (Wayland / X11), and kernel version (`uname -r`).
2. PCI device detection (`lspci -nnk -d 1461:`).
3. The userspace streamer terminal log (`./play.sh --cli`).
4. Connected console / scaler (e.g. PS5, Switch, RetroTINK-4K, OSSC Pro) and monitor model.

---

## 💡 Feature Requests & Hardware Proposals

For feature proposals, requests for new capture cards (such as GC570D, GC555, etc.), or questions about setup and audio routing, please open a thread in [GitHub Discussions](https://github.com/Everlite/EverLive/discussions)!
This keeps GitHub Issues clean and focused on verified bugs and active implementation tasks.

---

## 💻 Development & Submitting Pull Requests

1. **Clean-Room & Blob-Free Policy:**
   - Under no circumstances should proprietary binaries, firmware dumps, decompiler outputs, or proprietary SDK headers be committed to the repository.
   - All contributions must be original source code under the **GPL-2.0-only** license (see [`LICENSE.md`](LICENSE.md)).
   - EverLive is 100% userspace and blob-free (Ring 3 via `vfio-pci`).

2. **Code Standards & Compilation:**
   - The userspace engine in `tools/opengc573-userd/` is written in standard C (C11 / GNU C) with POSIX threads and SDL2.
   - All code must compile cleanly with **0 warnings** on both GCC and Clang:
     ```bash
     # Test GCC build
     make -C tools/opengc573-userd clean
     make -C tools/opengc573-userd -j"$(nproc)"

     # Test Clang build
     make -C tools/opengc573-userd clean
     make -C tools/opengc573-userd CC=clang -j"$(nproc)"
     ```
   - Python code in `tools/everlive/` must follow PEP 8 and remain compatible with Python 3.10+.

3. **AI-Assisted Contributions:**
   - In accordance with our [transparency policy](README.md#transparency--ai-assistance), AI-assisted code is welcomed as long as you have personally validated it on real hardware or through tests, and you understand the logic being submitted.

---

## 🎮 Hardware Testing

We are especially interested in test reports for:
- **Scalers & Retro Line-Doublers:** RetroTINK-4K, OSSC Pro, Framemeister, GBS-Control (at 240p, 480p, 1080p, and custom 120Hz BFI modelines).
- **Consoles:** PlayStation 5 (1440p120, HDR, Bitstream audio), Nintendo Switch (720p/1080p), Xbox Series X/S, Steam Deck dock.
- **Hardware variants:** AVerMedia Live Gamer BOLT (GC555) over Thunderbolt.
- **Compositors & Environments:** KDE Plasma 6 (Wayland), GNOME 47/48 (Wayland), Hyprland, Sway, X11.
