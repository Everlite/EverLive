# EverLive & open_gc573 - Future Roadmap & Ideas (TODO)

Dieses Dokument erfasst geplante Features, architektonische Ideen und Untersuchungspunkte für zukünftige Versionen von EverLive und dem `open_gc573` Treiber.

---

## 🚀 Geplante Features & Verbesserungen

### 1. Anamorphes 2x Vertical-Scaling für MPV Studio (Issue #42)
- **Kontext:** Bei super-ultrawide Signalen wie `3840x1080` (oft von RetroTINK 4K bei 120Hz/BFI genutzt) streckt `everlive_display` das Bild bereits automatisch um den Faktor 2x vertikal auf das beabsichtigte 16:9 Format (3840x2160).
- **Ziel:** In `tools/everlive/everlive_viewer.py` bei Erkennung von `3840x1080` auch für den MPV Studio Renderer automatisch die passende Video-Filter-Skalierung (z. B. `--vf-add=scale=3840:2160` oder `--video-aspect-override=16:9`) setzen.

### 2. Natives HDR10-Testing an PS5 / PC (Hardware-Verifikation)
- **Kontext:** Haxman beobachtet bei RetroTINK 4K HDR-Signalen in dunklen Szenen ein leichtes Pixel-Grieseln / Dithering bei YUV 4:2:2.
- **Ziel:** Gegentest an einer physischen PlayStation 5 oder einem HDR-fähigen Gaming-PC durchführen, um zu verifizieren, ob das Rauschen spezifisch für den RetroTINK 4K Upscaler/Dither ist oder die FPGA-Quantisierung betrifft.

---

## 🔮 Zukunfts-Architektur & Forschung (Research)

### 3. Userspace-Treiber via VFIO & PipeWire (Secure Boot / EAC v2 Kompatibilität)
- **Idee eingereicht von:** @PolygonPrincess (Discord Community)
- **Hintergrund:** Neue Anti-Cheat-Anforderungen (z. B. Epic Games EAC v2 für Linux) fordern zunehmend striktes Secure Boot ohne Custom MOK-Keys. Out-of-Tree Kernel-Module können in solchen Szenarien durch den Kernel Lockdown Mode blockiert werden.
- **Konzept:**
  - Anstelle eines Out-of-Tree Kernelmoduls (`open_gc573.ko`) wird die PCIe-Karte an den Standard-Mainline-Treiber `vfio-pci` gebunden.
  - Ein Userspace-Daemon (`everlive-daemon`) steuert die Karte über `/dev/vfio/*` (MMIO per `mmap`, Interrupts via `eventfd`, DMA über IOMMU).
  - Die Videoausgabe wird direkt in **PipeWire** (als virtuelle Kamera / SPA Video-Source) oder über Shared Memory an OBS und EverLive übergeben.
- **Vorteile:**
  - 100 % Secure-Boot-kompatibel ohne Kernel-Signing oder MOK.
  - Überlebt Kernel-Updates ohne Neukompilierung.
  - Kein Risiko von Kernel-Panics im Ring 0.
- **Herausforderungen:**
  - Hohe Komplexität: 4K60 Scatter-Gather DMA (bis zu 600 MB/s), IT6805/IT6664 I2C-Subsystem und HDMI-Audio müssen im Userspace reimplementiert werden.
  - Standard-V4L2 (`/dev/video*`) im Userspace erfordert entweder PipeWire-V4L2 Emulation oder CUSE/FUSE.
