# Controls, Hotkeys & OSD

## Keyboard Shortcuts

| Key / Action | Scope | Function |
|:---|:---|:---|
| <kbd>F</kbd> / <kbd>F11</kbd> / Double-click | All | Toggle borderless / Cinema fullscreen |
| <kbd>Esc</kbd> / <kbd>Q</kbd> *(pop-out)* | All / Engine | Exit Cinema fullscreen (embedded: <kbd>Esc</kbd>) or close pop-out window (<kbd>Esc</kbd>/<kbd>Q</kbd>) |
| <kbd>M</kbd> | All | Mute / unmute audio |
| <kbd>↑</kbd> / <kbd>↓</kbd> *(pop-out)* or <kbd>+</kbd> / <kbd>-</kbd> | All | Volume up / down (5% steps) |
| <kbd>A</kbd> | All | Cycle aspect ratio (`auto` → `16:9` → `4:3` → `stretch`) |
| <kbd>L</kbd> / <kbd>1</kbd> *(GC573/HWS)* · <kbd>L</kbd> *(GC570D)* | All | Toggle scaling (`fit` bilinear ↔ `integer` nearest-neighbor) |
| <kbd>Tab</kbd> / <kbd>I</kbd> | All | Toggle the Performance Stats HUD |
| <kbd>Alt</kbd> + <kbd>Z</kbd> | All | Toggle the Quick Navigation bar on/off (mouse wake is disabled while toggled off) |
| <kbd>P</kbd> *(GC573/HWS)* · <kbd>C</kbd> *(GC570D)* | All | Toggle OSD position (Top / Bottom) |
| <kbd>F1</kbd> *(Hub/GC573)* · <kbd>?</kbd> *(GC573 pop-out)* | Hub / GC573 | Open keyboard shortcuts dialog (Hub) or on-screen help overlay (pop-out) |
| <kbd>H</kbd> | GC573 / Hub | Toggle HDR10 / HLG → SDR tonemapping |
| <kbd>G</kbd> | GC573 / Hub | Cycle Frame Generation (`Off` → `2×` → `3×` → `4×` → `Off`) |
| <kbd>1</kbd> / <kbd>2</kbd> | GC570D | Select HDMI IN 1 / HDMI IN 2 |
| <kbd>P</kbd> | GC570D | Toggle Picture-in-Picture of secondary HDMI input |
| <kbd>Ctrl</kbd> + <kbd>R</kbd> | Hub | Start / stop recording |
| <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>A</kbd> | Hub | Toggle Simple / Advanced UI view |
| <kbd>T</kbd> | Hub | Dock remote video |
| <kbd>S</kbd> / <kbd>F10</kbd> | GC573 (pop-out) | Streamer Mode (hide OSD + mouse cursor; `--streamer-mode` at launch) |

*VSync is a launch option (`--vsync` / `--no-vsync` / Hub checkbox) for OpenGL pass-through and cannot be toggled while streaming; the Vulkan / LSFG-VK frame-generation path always uses FIFO VSync presentation.*

---

## Quick Navigation Bar & Performance Stats HUD

The engine OSD consists of two **independent** overlays:

1. **Quick Navigation Bar:** A clickable strip (live dot + signal tooltip on hover, HDMI 1/2 switcher on DUO, Aspect dropdown, Scale toggle, Record, PiP, volume/mute, fullscreen, stats toggle, and hide button). When enabled in the Hub, it wakes on mouse movement and auto-hides after 3 seconds; toggling it off with <kbd>Alt</kbd> + <kbd>Z</kbd> or the Hide button keeps it hidden until <kbd>Alt</kbd> + <kbd>Z</kbd> is pressed again.
2. **Performance Stats HUD:** A two-row corner readout showing live signal/audio status and engine diagnostics (with a `×` close button). Showing or hiding either overlay does not affect the other.

Use `everlive-ctl runtime osd-mode <auto|compact|card>` to switch OSD policy (`compact` suppresses automatic Quick Navigation while keeping the stats HUD), or set `EVERLIVE_OSD_DEBUG=1` to trace bar geometry and pointer hitboxes.
