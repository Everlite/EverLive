# EverLive - AVerMedia GC573 Linux Gaming Suite & Open Driver

[![EverLive](https://img.shields.io/badge/project-EverLive-8a2be2.svg)](#)
[![Discord](https://img.shields.io/badge/Discord-Join%20Community-5865F2?logo=discord&logoColor=white)](https://discord.com/invite/D2XFhkPMmc)
[![Status](https://img.shields.io/badge/status-low%20latency%20(experimental)-brightgreen.svg)](#highlights--verified-capabilities)
[![open_gc573](https://img.shields.io/badge/driver-open__gc573%20(100%25%20blob--free)-2e7d32.svg)](#hardware-and-kernel-compatibility)
[![RGB Control](https://img.shields.io/badge/RGB%20Control-On--the--Fly%20Hardware%20MMIO-blueviolet.svg)](#-hardware-rgb-led-lighting-on-the-fly-control)
[![Gaming Suite](https://img.shields.io/badge/suite-console%20hub%20%2B%20direct%20display-blue.svg)](#-everlive-console-hub--game-window)
[![Kernel](https://img.shields.io/badge/kernel-tested%206.19%20--%207.x%20(CachyOS%2FArch)-2e7d32.svg)](#hardware-and-kernel-compatibility)
[![AI Assisted](https://img.shields.io/badge/code-heavily%20AI%20assisted-ff69b4.svg)](#-transparency--ai-assistance)
[![License](https://img.shields.io/badge/license-GPL--2.0--only-blue.svg)](#legal-and-licensing)

**EverLive** is an all-in-one Linux console gaming suite and 100% blob-free open-source driver for the **AVerMedia Live Gamer 4K (GC573)** (PCI `1461:0054`, Subsystem `1461:5730`) and **Live Gamer BOLT (GC555)** (`1461:5550`).

With **EverLive**, you can play PlayStation 5, Xbox Series X, Nintendo Switch, or PCs directly on your Linux desktop (Wayland & X11) at **1440p @ 120 FPS**. HDMI capture at 1440p and 120.35 Hz is verified on a PlayStation 5. How smooth that looks on the monitor still depends on the panel clock. That part is experimental; see [Known Issues](#️-known-issues--work-in-progress). EverLive also provides synchronized multi-channel audio, live volume control, hardware RGB control, and start-menu integration.

---

## 🚀 Quickstart: 1-Click Installation (Beginner-Friendly)

> **Is installation easy for Linux beginners?**  
> **Yes.** You don't need prior experience with kernel headers, udev permissions, or DKMS. The installer handles everything in a single step.

### 1-Command Installation

Open a terminal and run:

```bash
git clone https://github.com/Everlite/EverLive.git
cd EverLive
sudo ./install.sh
```

### What the installer does:
1. **Detects your distribution and dependencies:** Detects your package manager (`pacman` on Arch/CachyOS/Manjaro, `apt` on Ubuntu/Debian/Mint/Pop!_OS, `dnf` on Fedora/Nobara), lists missing packages (kernel headers, DKMS, build tools, SDL2, PipeWire/ALSA tools, `v4l-utils`, mpv, Qt XCB cursor libs) and offers to install them.
2. **Sets up Python (PySide6):** If PySide6 is not available system-wide (e.g. Debian, Ubuntu, Linux Mint), it creates a local virtual environment (`tools/everlive/.venv`) so the hub runs without touching system Python.
3. **Compiles the driver and display engine:** Builds the `open_gc573` kernel module and the native C display engine (`everlive_display`).
4. **DKMS support & auto-recompile on kernel updates:** Registers the module with DKMS for automatic rebuilds on kernel updates. Both `play.sh` and `open_gc573/load.sh` also feature automatic on-the-fly re-compilation whenever a newly updated kernel is booted.
5. **Loads the driver at boot:** Adds `open_gc573` to `/etc/modules-load.d/everlive.conf`.
6. **Sets permissions so you can play without root:** Installs `/etc/udev/rules.d/70-everlive.rules` (`TAG+="uaccess"` for video/audio, group `video` for the RGB and HDMI re-sync sysfs files) and adds your user to the `video` and `audio` groups. *Log out and back in once after installing so the group change takes effect.*
7. **Adds a start menu entry:** Installs the **EverLive** desktop launcher (and removes the old `gc573-ps5.desktop` launcher if present).

After installation, launch **EverLive** from your application menu, or run:
```bash
./play.sh
```
*If the driver isn't loaded yet, `play.sh` loads it for you (it asks for your password once via `pkexec`).*

---

## Highlights & Verified Capabilities

- **2560×1440 @ 120 FPS verified on PlayStation 5:** 120.35 Hz HDMI 2.0 capture verified on real PS5 hardware (*The Finals*, *Fortnite 120Hz Mode*, PS5 Dashboard) with live HDR switching, low-latency Direct Rendering, and PipeWire multichannel audio.
- **Retro scalers & verified custom modelines (RetroTINK 4K Pro):** Extensively tested on physical hardware by contributor [@haxman](https://github.com/haxman). A measured rate within ±2 Hz of a mode in the driver's table is accepted. The Console Hub and game window HUD print refresh rates to two decimal places (e.g. `59.94 FPS (16.68ms)`) with `p` (progressive) and `i` (interlaced) indicators. Physically verified capabilities include:
  - **240p Retro Modes:** Supports CEA-861 pixel-repeated 240p (`1440x240` / `2880x240 @ 60Hz`) as well as **1920x240 CRT Super Resolution** (~36 MHz clock) with clean 4:3 integer rendering.
  - **480i & 576i Interlaced (CEA-861 HDMI Pixel Repetition):** Decodes 2x pixel-repeated HDMI transport (`1440x480i` / `1440x576i`) with automatic 4:3 aspect-ratio letterboxing.
  - **PAL Modes (50 Hz & 100 Hz):** Verified handshake and capture for `4Kp50`, `1440p50`, `1080p50`, as well as high-refresh `1440p100` and `1080p100`.
  - **Cinema BFI & Ultrawide:** `1080p @ 95.94Hz` (4x 23.976Hz film BFI), `3840x1080` (RetroTINK 4K 120Hz BFI mode with 2x vertical restoration), `1920x1080 @ 180Hz / 240Hz`, and `1920x1200` WUXGA.
- **Graceful Standby & Auto-Connect:** If launched before the console or video source is powered on or connected, the game window opens immediately displaying a clean Standby screen (*"Waiting for HDMI signal..."*) instead of exiting, and seamlessly transitions into active video playback once the HDMI handshake completes.
- **Hot Resolution Switching:** If the console or scaler changes output resolution during active playback (e.g. 4K Dashboard $\leftrightarrow$ 1440p 120Hz $\leftrightarrow$ 1080p $\leftrightarrow$ 480i), the game window catches kernel `V4L2_EVENT_SOURCE_CHANGE` events on the fly, re-allocates V4L2 buffers and GPU textures gracefully, and resumes playback without black-screening or requiring a window restart.
- **Low-latency direct rendering (experimental):** `everlive_display` asks for a 3-buffer V4L2 queue. While playback stays under the threshold it shows frames in order. If the time since the DMA timestamp exceeds about 18 ms at 120 Hz, or about 32 ms at 60 Hz, it skips one queued frame. It does not empty the whole queue in one step.
- **Separate console hub + game window (no more X11 embedding):**
  - **Why the hub no longer embeds the video:** Earlier versions passed the Qt widget to `everlive_display -w` (`SDL_CreateWindowFrom`) and forced the X11 video driver. On Wayland that path runs through Xwayland. The nested window is composited with the hub, and a size check in the renderer called `SDL_SetWindowSize` on that foreign window. The Console Hub does not pass `-w` anymore. The flag still exists on the renderer.
  - **How it works now:** The **EverLive Console Hub** (Qt, runs natively on Wayland) handles setup: signal status, format preset, RGB LED, volume, HDMI re-sync and launch options. The video stream runs in its **own top-level window** (`everlive_display`, SDL2) that talks to the compositor directly.
  - **Native Wayland window:** If your SDL2 comes from **sdl2-compat / SDL3** (e.g. Arch, CachyOS), the game window is a native Wayland client. With classic SDL2 builds (e.g. older Ubuntu/Mint releases), SDL uses X11 by default, so on Wayland it runs through Xwayland. On X11 sessions it always runs natively.
  - **Non-blocking presentation:** VSync is off by default. Frames are presented as soon as they arrive and never wait for vblank. In fullscreen, compositors such as KWin can use direct scanout automatically. VSync can be turned on in the hub if you prefer.
  - **No HDMI polling while you play:** While the game window is open, the hub does not query DV timings. That avoids a once-per-second stall on the HDMI receiver's I2C bus. It still reads the cached `hdr_mode` control once a second, so the label can follow an HDR change.
- **Aspect-ratio-preserving scaling (letterbox / pillarbox):** Keeps the source aspect ratio on 16:10, 21:9 and 4:3 setups with black borders and no stretching, in both windowed and fullscreen mode.
- **Live HDR switching:** HDR10 (PQ) and HLG come from the HDMI DRM InfoFrame. At the same resolution the driver updates the color matrix only and leaves DMA running, so the game window stays up. The Console Hub label follows that metadata (`SDR`, `HDR10`, or `HLG`). Switching HDR on a PlayStation 5 during capture was verified. The pixels stay in the selected capture format (usually YUYV); PQ and HLG are not tone-mapped to SDR. P010 is the 10-bit capture format.
- **Direct render pipeline:** `everlive_display` maps the V4L2 DMA buffers and uploads each frame to an SDL2 OpenGL texture (`SDL_UpdateTexture`). The HUD latency number is taken before that upload and before `SDL_RenderPresent`. It is not the time until the panel shows the frame.
- **Synchronized low-latency audio (LPCM stereo & surround):** Bridges console LPCM audio to PipeWire via an adaptive loopback pipeline (`everlive-audio` with real-time clock drift compensation and a 60 ms jitter-safe buffer) as the stream **EverLive Game Audio**. PipeWire automatically routes and mixes it down to your stereo headset or surround setup.
- **Hardware RGB LED control (reverse-engineered):**
  - **Factory blinking fixed:** Reverse-engineered the Artix-7 FPGA lighting engine (BAR 0 `0x0800`–`0x086c`, script RAM `0x2008`). The card no longer blinks red endlessly on Linux, and you don't need Windows software.
  - **Standard Linux Multicolor LED subsystem:** Implements the kernel multicolor LED classdev at `/sys/class/leds/gc573:rgb:status` using official Profile 241 (Constant Color 24-bit PWM).
  - **No dropped frames, no password prompts:** Writable by the `video` group (`0660`). After installing, you can change colors from the hub or the CLI without `sudo`. Changes are applied via direct register writes, with no stream restart and no dropped frames.
  - **8 hardware profiles:** Off (Dark), Blue, Green, Red, Cyan, Magenta, Yellow and White.
- **In-game HUD & live volume control:**
  - Change the volume (0–150%) with the **mouse wheel**, the keyboard, the on-screen HUD slider, or the hub slider. All of them work while you play.
  - Stats overlay (FPS, latency, jitter), **on by default**, top right of the game window. <kbd>Tab</kbd> or <kbd>i</kbd> hides it. The same line is printed to the terminal once per second. The latency figure is the time from DMA completion to dequeue, before the texture upload and the present.
- **100% blob-free:** `open_gc573` and the whole repository are open source under **GPL-2.0-only**. The repository contains no proprietary binary blobs.

---

## 🎮 EverLive Console Hub & Game Window

![EverLive on Linux](docs/everlive_hub_preview.png)

*Console Hub on the desktop, capture in its own window. The hub does not embed the video.*

**Console Hub (`everlive_viewer.py`):**
- Live HDMI signal status capsule (e.g. `● 2560x1440p @ 120 FPS • SDR` or `● 720x480i @ 59.94 FPS • SDR`)
- Format preset dropdown (Auto / 4K / 1440p / 1080p / 720p / 480p / 576p / 240p / Ultrawide)
- Hardware RGB LED dropdown with live color previews, volume slider + mute, **Re-sync Signal** button
- **Start Gaming Screen** with options: *start in fullscreen* (checked by default), *open automatically on launch*, *enable VSync*, *Streamer Mode (Clean Feed)*

**Command-line options:**
```bash
./play.sh -f                 # start the game window immediately (fullscreen)
./play.sh -s                 # start in Streamer Mode (clean feed without overlays)
./play.sh --cli -f           # no GUI, game window only
./play.sh --vsync            # enable VSync
./play.sh --led off          # set RGB LED and exit
./play.sh -d /dev/video0 -a hw:CARD=A4K,DEV=0   # override auto-detected devices
./play.sh --mpv              # use mpv instead of everlive_display (fallback)
```

You can also start the renderer directly, without the hub: `./tools/everlive/everlive_display -f`

---

## Video Demonstration (PS5 Gameplay & Latency)

Live gameplay demonstration of **THE FINALS** on PlayStation 5 played through EverLive on the Linux desktop:

https://github.com/Everlite/EverLive/raw/main/docs/thefinals_demo.mp4

[![THE FINALS Latency Demonstration](docs/thefinals_demo_poster.jpg)](https://github.com/Everlite/EverLive/raw/main/docs/thefinals_demo.mp4)

*(Click the player or preview image above to watch the demonstration)*

---

## Architecture

```mermaid
flowchart TD
  subgraph Hardware["Console & Hardware Layer"]
    ps5["PS5 / Console (1440p @ 120 FPS)"] -->|HDMI 2.0| gc573["AVerMedia GC573 (Live Gamer 4K)"]
    gc573 -->|"PCIe SG-DMA Video (3 mmap buffers)"| v4l2["V4L2 Video Device (/dev/video*)"]
    gc573 -->|"PCIe SG-DMA Audio (8ch LPCM)"| alsa["ALSA PCM Capture (hw:CARD=A4K)"]
    gc573 -.->|"Linux LED classdev / sysfs"| sysfs["/sys/class/leds/gc573:rgb:status<br/>sysfs: hpd_pulse"]
  end

  subgraph EverLive["EverLive Linux Suite"]
    hub["EverLive Console Hub (Qt)<br/>Presets, RGB, Volume, Re-Sync<br/>DV timings paused while playing"]
    hub -.->|Launch| game["everlive_display (C / SDL2)<br/>Separate top-level window<br/>Wayland or X11"]
    hub -.-> sysfs

    v4l2 -->|"mmap, then SDL texture upload"| game
    game -->|"VSync off by default"| screen["120 Hz Monitor"]

    alsa -->|"everlive-audio (adaptive loopback)"| pw["PipeWire (60 ms safe buffer)"]
    pw -->|Channel mixdown| speakers["Headset / Speakers"]
  end
```

---

## Repository Structure

| Directory / File | Description |
|:---|:---|
| **`install.sh`** | One-click installer (dependencies, driver build, DKMS, udev permissions, boot autoload, desktop launcher). |
| **`uninstall.sh`** | Uninstaller that removes the driver, DKMS entry, udev rules, autoload config and launcher. |
| **`play.sh`** | Launcher used by the start menu entry; loads the driver if needed, then starts the Console Hub. |
| **`open_gc573/`** | **Kernel driver:** native, blob-free Linux driver (GPL-2.0-only), plus `load.sh` / `unload.sh`. |
| **`tools/everlive/`** | **EverLive suite:** Console Hub (`everlive_viewer.py`), native display engine (`everlive_display.c`), desktop entry. |

---

## Controls & Hotkeys During Gameplay

These controls work inside the game window:

| Key / Action | Function |
|:---|:---|
| **Mouse Wheel Up / Down** | Volume up / down in 5% steps (with an on-screen notice) |
| **HUD Volume Slider** | Drag the slider in the bar at the bottom center of the window |
| <kbd>s</kbd> or <kbd>F10</kbd> | Toggle **Streamer Mode (Clean Feed)** — locks overlays, hides cursor & suppresses HUD for OBS capture |
| <kbd>m</kbd> | Mute / unmute audio |
| <kbd>F11</kbd> or <kbd>f</kbd> (or double-click) | Toggle borderless fullscreen |
| <kbd>Tab</kbd> or <kbd>i</kbd> | Show / hide the stats overlay (FPS, latency, jitter) |
| <kbd>h</kbd> or <kbd>r</kbd> | Force HDMI handshake & re-sync video timings |
| <kbd>+</kbd> / <kbd>-</kbd>, <kbd>↑</kbd> / <kbd>↓</kbd> or <kbd>0</kbd> / <kbd>9</kbd> | Volume up / down in 5% steps (0–150%) |
| <kbd>Esc</kbd> or <kbd>q</kbd> | Close the game window |

*Note: The mouse cursor and HUD fade out after 2.5 seconds without input so nothing blocks your view. In Streamer Mode (<kbd>s</kbd>), all overlays and cursor popups are hard-disabled for clean capture.*

---

## 💡 Hardware RGB LED Lighting (On-the-Fly Control)

On Windows, the GC573 needs AVerMedia's background software to stop its unconfigured factory state (endless red blinking). On Linux, this used to leave the card blinking inside the PC case permanently.

Through reverse-engineering of the official firmware and the FPGA register space, **EverLive has full native control over the card's RGB lighting** via the standard Linux Multicolor LED subsystem (`/sys/class/leds/gc573:rgb:status`):

### Key Advantages
* **No gameplay interruption:** Changing colors is a direct register write. It doesn't renegotiate HDMI, reset DMA, or drop frames. Tested live on PS5 during 1440p @ 120 FPS gameplay.
* **Standard Linux Kernel Subsystem:** Exposed via standard `led_classdev_mc` (`multi_intensity` and `brightness`), writable by the `video` group (`0660`, set by udev rules).
* **24-bit True Hardware Color PWM:** Implements official Profile 241 with dedicated hardware PWM slots for Red, Green, and Blue.
* **Hardware profiles:**
  - `0`: 🌑 **Off (Dark)** (driver default at load time)
  - `1`: 🔵 **Solid Blue**
  - `2`: 🟢 **Solid Green**
  - `3`: 🔴 **Solid Red**
  - `4`: 🔷 **Cyan / Turquoise**
  - `5`: 🟣 **Magenta / Purple**
  - `6`: 🟡 **Yellow**
  - `7`: ⚪ **White**
* **Live switching in the GUI:** Pick a color at any time from the LED dropdown in the Console Hub.
* **Command line:**
  ```bash
  ./play.sh --led off
  ./play.sh --led blue
  ./play.sh --led cyan
  ```

---

## ⚠️ Known Issues & Work in Progress

> [!NOTE]
> **EverLive is a work in progress.** 1440p @ 120.35 Hz capture is verified on a PlayStation 5. Pacing on the monitor is still **experimental**:
>
> - **HDMI mode switches (resolution & refresh rate changes):** When a game or console changes resolution or refresh rate (e.g. from 60 Hz PS5 home menu to 120 Hz in *The Finals*), the game window automatically detects the new timings and re-allocates video buffers.
>   - **Hardware HPD re-sync:** If the HDMI receiver or console stalls while switching clock rates, press <kbd>h</kbd> or <kbd>r</kbd> in the game window (or click **🔄 HDMI Re-Sync** in the Console Hub). This sends a hardware HPD (Hot-Plug Detect) pulse via sysfs, forcing an immediate, clean handshake without touching physical cables.
> - **Refresh-rate mismatch (120.35 Hz vs. 120.00 Hz):** The PS5 outputs ~120.35 Hz. On a fixed 120.00 Hz monitor with **VSync enabled**, the extra 0.35 frames per second build up, and one frame is dropped about every 3 seconds. With VSync off (default), presentation doesn't wait for vblank, so this doesn't happen.
> - **SDL backend:** The game window runs natively on Wayland only with sdl2-compat / SDL3. With classic SDL2 it runs through Xwayland (see *Highlights*). You can force a backend with `SDL_VIDEODRIVER=wayland` or `SDL_VIDEODRIVER=x11`.
> - **Console Audio Settings (Stereo / 2-Channel LPCM Recommended):** On PlayStation 5, Xbox Series X/S, Nintendo Switch, and PC sources, configure audio output to **Linear PCM 2.0 (Stereo)**.
>   - Multi-channel surround modes (5.1 / 7.1 LPCM) stream uncompressed 8-channel audio, which can cause micro-stutters and buffer resync clicks on desktop USB audio devices (such as wireless headsets or USB DACs) due to asynchronous clock drift between PCIe DMA and USB audio clocks.
>   - Setting the console to 2-channel Stereo guarantees completely smooth, dropout-free playback and allows console binaural engines (like PS5 Tempest 3D Audio) to deliver an optimal spatial mix for headphones.
> - **Active development:** DMA queue tuning, buffer recycling and HDMI audio compatibility across more sources are still being improved.
>
> **Tips for the best experience:**
> 1. **Set Console Audio to 2-channel Linear PCM (Stereo)** to prevent audio dropouts, pops, and USB DAC resyncs.
> 2. **Keep VSync off** (default) unless you see tearing that bothers you.
> 3. **Try VRR / Adaptive Sync:** Depending on your GPU driver, compositor and monitor (e.g. KDE Plasma *System Settings → Display → Adaptive Sync → Always*), VRR can smooth out pacing by matching the refresh rate to the incoming frames. *How well this works depends on your compositor.*
> 4. **Set your monitor to ≥ 120 Hz:** A 120 FPS stream on a 60 Hz desktop will always drop frames.
> 5. **Use fullscreen:** Fullscreen gives the compositor the best chance to use direct scanout.

---

## Hardware and Kernel Compatibility

| Device | Interface | PCI Subsystem | Support Status | Notes |
|:---|:---|:---:|:---:|:---|
| **AVerMedia Live Gamer 4K (GC573)** | PCIe Gen2 x4 | `1461:5730` | **Verified on Hardware** | Physically verified on PlayStation 5 (1440p120 HDR, 1080p, 4K60) by Everlite and RetroTINK 4K Pro (59.94 Hz NTSC, 50 Hz / 100 Hz PAL, 480i, 1440x240p, 1920x240p CRT super-res, 1080p96) by @haxman. |
| **AVerMedia Live Gamer BOLT (GC555)** | Thunderbolt 3 | `1461:5550` | **Should work (untested)** | Code inherited from ostrich's clean-room driver; not yet tested on real hardware by Everlite. Testers welcome! |

- **Linux distributions tested:** CachyOS, Arch Linux, Linux Mint 22.3 (Cinnamon), Ubuntu, Debian.
- **Linux kernels:** Linux 6.19 through 7.x (tested from 7.0 generic up to CachyOS 7.2.11).
- **Desktop environments:** Main test setup is KDE Plasma 6 on Wayland (CachyOS, sdl2-compat → native Wayland game window). Also used on X11 (Cinnamon, XFCE). Other Wayland compositors (GNOME, Hyprland, Sway) should work but haven't been tested yet.

---

## Troubleshooting & FAQ

#### 1. Black screen or "No Signal"
- **Disable HDCP:** On PlayStation 5, go to **Settings → System → HDMI → Enable HDCP** and turn it **OFF**. Capture cards can't record HDCP-encrypted video.
- **Cable connection:** Connect the console to the card's **HDMI IN** port (the one closest to the motherboard).
- **Check lock status:** Find the device with `v4l2-ctl --list-devices`, then check whether the card has locked onto the signal: `v4l2-ctl -d /dev/videoX --query-dv-timings`.

#### 2. No sound output
- Make sure audio isn't muted in the game window (press <kbd>m</kbd> or scroll the mouse wheel up).
- In your KDE/GNOME sound settings under "Applications", make sure the stream **`EverLive Game Audio`** (or `everlive-audio`) is active and not muted.

#### 3. Signal lost after launching or closing a game (mode / refresh rate switch)
- **Symptom:** The screen goes black or loses signal when switching between the 60 Hz console menu and a 120 Hz game (e.g. *The Finals*), or when the source changes video mode.
- **Fix (software re-sync):** Press <kbd>h</kbd> or <kbd>r</kbd> in the game window, or click **🔄 HDMI Re-Sync** in the Console Hub. EverLive renegotiates HDMI and picks up the new timings right away. You usually don't need to unplug the cable.
- **Last resort:** Only if the HDMI receiver stops responding completely, briefly unplug and replug the HDMI cable at the capture card.

#### 4. RGB / Re-Sync does nothing ("Permission denied")
- Log out and back in after installing so your new `video` group membership takes effect. Check with `groups`.

#### 5. Stutter or lag: how do I measure it?
- Start the renderer from a terminal: `./tools/everlive/everlive_display -f`. Once per second it prints the measured FPS, latency and jitter. Press <kbd>Tab</kbd> to see the same values in the game window. Include this output when you open an issue.

#### 6. Secure Boot: "Loading of unsigned module is rejected"
- **Symptom:** With UEFI Secure Boot enabled (e.g. Ubuntu, Linux Mint, Fedora), the kernel may refuse to load out-of-tree drivers with `Loading of unsigned module is rejected` or `module verification failed`.
- **Solution:**
  1. **Option A (simplest):** Disable **Secure Boot** in your UEFI/BIOS settings.
  2. **Option B (keep Secure Boot on):** Sign `open_gc573.ko` with your Machine Owner Key (MOK):
     ```bash
     # Generate a MOK key pair (if you don't have one already):
     openssl req -new -x509 -newkey rsa:2048 -keyout MOK.priv -outform DER -out MOK.der -nodes -days 36500 -subj "/CN=EverLive MOK/"
     
     # Import the key into your system MOK list (prompts for a one-time password):
     sudo mokutil --import MOK.der
     # (Reboot your PC and follow the blue MOK management screen to enroll the key)

     # Sign the installed driver module (sign-file path shown for Debian/Ubuntu):
     sudo /usr/src/linux-headers-$(uname -r)/scripts/sign-file sha256 MOK.priv MOK.der $(modinfo -n open_gc573)
     ```

#### 7. How do I uninstall EverLive?
Run the uninstaller in the repository folder:
```bash
sudo ./uninstall.sh
```

---

## 💬 Community & Discord

Have questions, want to share your hardware setup, or discuss compatibility across different consoles, monitors, and Linux distributions?

Join our official community on Discord:  
👉 **[Join the EverLive Discord Server](https://discord.com/invite/D2XFhkPMmc)**

* Share logs, `dmesg` outputs, and hardware configs
* Get help setting up PS5, Xbox Series X, Switch, or RetroTINK
* Chat about upcoming driver milestones and latency tuning

---

## Legal and Licensing

| Component | Path | License | Notes |
|:---|:---|:---:|:---|
| **Kernel Driver** | `open_gc573/` | **GPL-2.0-only** | Based on clean-room reverse-engineering by ostrich ([gc555](https://github.com/ostrich/gc555)), extended by Everlite. |
| **EverLive Gaming Suite** | `tools/everlive/`, `play.sh`, `install.sh`, `uninstall.sh` | **GPL-2.0-only** | Copyright © 2026 Everlite. Native C display engine and Qt Console Hub. See [`LICENSE.md`](LICENSE.md). |

### Disclaimers & Trademark Notice

- **Trademarks:** AVerMedia, Live Gamer 4K (GC573) and Live Gamer BOLT (GC555) are trademarks or registered trademarks of AVerMedia Technologies, Inc. PlayStation, PS4 and PS5 are registered trademarks of Sony Interactive Entertainment Inc. All product names and trademarks belong to their respective owners and are used only to identify hardware and compatibility. This independent open-source project is not affiliated with, authorized, maintained, sponsored, or endorsed by AVerMedia Technologies, Inc., Sony, or any of their affiliates.
- **Warranty:** This software and documentation are provided "AS IS", without warranty of any kind, express or implied, including but not limited to the warranties of merchantability, fitness for a particular purpose, and non-infringement. In no event shall the authors or contributors be liable for any claim, damages, or other liability arising from the use of this software. Register tuning, EDID flashing, and kernel driver interactions are performed at your own risk.

---

## Acknowledgements

- **[Everlite](https://github.com/Everlite)**: Project initiator; EverLive gaming suite, PS5 1440p120 hardware tuning, low-latency direct renderer and PipeWire audio.
- **[Haxman](https://github.com/haxman)**: Hardware verification on RetroTINK 4K Pro, modelines analysis, and extensive testing of PAL (50Hz / 100Hz), cinema BFI, 480i, 240p / 1920x240 CRT super-res modes, and on-the-fly resolution switching.
- **[ostrich](https://github.com/ostrich)**: Author of the [gc555](https://github.com/ostrich/gc555) driver. His clean-room reverse-engineering of the Artix-7 registers and the FPGA DMA ring is the foundation of `open_gc573`.
- **[Lou Perret](https://github.com/lou-perret)**: Capture stability fixes, ALSA audio constraints and 4K VM measurements.
- **[derrod](https://github.com/derrod)**: Early Linux reverse-engineering work on AVerMedia capture devices.

### 🤖 Transparency & AI Assistance

In the spirit of open-source transparency: **this project is heavily AI-assisted**.
The project owner (Everlite) directed the project, tested on physical hardware (PlayStation 5, monitor timings, audio sync), analyzed hardware behavior and validated performance. The code, refactoring, scripts and debugging were written and iterated with generative AI / LLM coding agents.
