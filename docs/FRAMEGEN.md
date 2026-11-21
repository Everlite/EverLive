# Frame Generation (LSFG-VK · Experimental)

EverLive supports optional **2×, 3×, and 4×** GPU frame generation on **GC573** via **LSFG-VK** (Vulkan implicit layer) with a phase-locked 30-in-60 cadence deduplicator and live multiplier cycling via the Console Hub or hotkey <kbd>G</kbd>.

*(Note: AFMF is not included on `dev`; experimental AFMF work is kept separately on branch `exp/afmf`.)*

---

## Requirements

1. Install `lsfg-vk` (either via `sudo ./install.sh --with-framegen` or your distribution package).
2. Own **Lossless Scaling** on Steam and switch it to the `lsfg-vk` beta:
   `Steam Library → Right Click "Lossless Scaling" → Properties → Betas → lsfg-vk`.
3. EverLive automatically discovers the Steam library and Vulkan layer.

---

## Features & Controls

- **Modes:** `60to120` (e.g. `60 → 120 / 180 / 240 FPS`) and `30to60` (e.g. `30 → 60 / 90 / 120 FPS` with 30 FPS deduplication).
- **Phase-Locked Cadence Deduplicator:** Real-time MAD-based detection locks onto `30-in-60` duplicate HDMI frames (`A, A, B, B`) even under dither/grain/water ripples and unlocks cleanly when sustained 60p motion resumes.
- **Options:** `Low-Latency Swapchain` (`PRESERVE_SWAPCHAIN_IMAGE_COUNT=1`), `Performance Mode`, and `Flow Scale`. Display pacing is handled via FIFO presentation and `VK_LAYER_EVERLIVE_pacer` (`libeverlive-vk-pacer.so`).

---

## Observed Throughput Limits (SDL2 Vulkan `SDL_Renderer` on `dev`)

- **`1080p60` / `1440p60`:** `2× / 3× / 4×` (`60 → 120/180/240 FPS`) and `30 → 60/90/120 FPS` run at full rate (verified on RX 9070 XT).
- **`1080p120`:** Reaches `~100 → 200+ FPS` (verified on RX 9070 XT).
- **`1440p120`:** Currently limited to **~48–60 base FPS (~95–120 output FPS)** on the SDL2 Vulkan `SDL_Renderer` + LSFG-VK path (RX 9070 XT). Pass-through OpenGL capture at `1440p120` without frame generation runs at full `120 FPS`.
