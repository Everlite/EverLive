---
name: Bug report
about: Create a report to help us improve EverLive
title: '[BUG] '
labels: bug
assignees: ''
---

**Describe the bug**
A clear and concise description of what the bug is.

**System Information:**
- **OS / Distribution:** (e.g. CachyOS, Arch Linux, Ubuntu 24.04, Fedora 41)
- **Kernel Version:** (run `uname -r`)
- **Desktop Environment & Session:** (e.g. KDE Plasma 6 Wayland, GNOME 47 X11)
- **Capture Card:** (AVerMedia Live Gamer 4K GC573 or GC555)
- **Source Device:** (e.g. PlayStation 5, Nintendo Switch, Xbox Series X, PC)
- **HDMI Resolution & Refresh Rate:** (e.g. 1440p120, 1080p60, 4K60)

**Diagnostic Output:**
Please run the following commands and paste the output:

1. **Hardware detection & Driver Binding:**
```bash
lspci -nnk -d 1461:
```

2. **VFIO Device Permissions:**
```bash
ls -la /dev/vfio/
```

3. **Streamer CLI Output:**
Run the streamer directly from the terminal and paste the logs:
```bash
./play.sh --cli
```

**Steps to Reproduce:**
1. Step 1...
2. Step 2...
3. See error...

**Expected Behavior:**
A clear and concise description of what you expected to happen.
