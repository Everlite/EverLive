# EverLive - Future Roadmap & Ideas (TODO)

Dieses Dokument erfasst geplante Features, architektonische Ideen und Untersuchungspunkte für zukünftige Versionen von EverLive.

---

## 🎯 Abgeschlossen in v1.0.0 ("The Userspace Era")

### ✅ 100% Blob-Freier Userspace-VFIO-Treiber (Ring 3)
- **Initiale Inspiration:** @PolygonPrincess (OpenGC570D)
- **Status:** **Erfolgreich implementiert & verifiziert in v1.0.0!**
- Keine DKMS-Abhängigkeiten und keine Kernel-Kompilierung mehr bei Kernel-Updates.
- 100% kompatibel mit UEFI Secure Boot (verwendet upstream `vfio-pci`).
- Hardware DMA für Video (1440p120 & 4K60) und L-PCM Audio (SDL2 / PipeWire).
- Hardware-EDID-Profile & FPGA-RGB-Steuerung.
- Standby-Daemon (`everlive_userd`) ohne PCIe FLR-Reset.

---

## 🚀 Geplante Features für v1.1+

### 1. Virtuelle Kamera / Loopback für Headless-Streaming (`v4l2loopback` / PipeWire SPA)
- **Ziel:** Optionales Einspeisen des Userspace-Video-Streams in ein virtuelles `/dev/video*`-Device via `v4l2loopback` oder PipeWire SPA Video Node, damit auch native OBS V4L2-Quellen ohne GUI-Fenster genutzt werden können.

### 2. Anamorphes 2x Vertical-Scaling für MPV Studio
- **Kontext:** Bei super-ultrawide Signalen wie `3840x1080` (oft von RetroTINK 4K bei 120Hz/BFI genutzt) streckt die Anzeige das Bild um den Faktor 2x vertikal auf das 16:9-Format (3840x2160).
- **Ziel:** Auch für den MPV-Fallback-Pfad bei Erkennung von `3840x1080` automatisch die passende Video-Filter-Skalierung (`--vf-add=scale=3840:2160`) hinterlegen.

### 3. Hardware-Fund: Analyse & Portierung für AVerMedia Live Gamer 4K 2.1 (GC575)
- **Ziel:** Sobald die Karte über den GitHub Sponsors Hardware Fund beschafft ist: Register-Dumps der HDMI 2.1 PHY und I2C-Matrix anfertigen, um HDMI 2.1 (4K120/4K144 VRR) auf Linux zu unterstützen.
