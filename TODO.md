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

## 🚀 Geplante Features für v1.1+ (Detaillierte Roadmap in `docs/ROADMAP_v1.1.md`)

Eine vollständige technische Spezifikation aller Module, Datenfluss-Diagramme und Implementierungs-Details ist in [docs/ROADMAP_v1.1.md](docs/ROADMAP_v1.1.md) dokumentiert.

### 1. Zero-DKMS Userspace V4L2-Kompatibilität (`libeverlive-v4l2.so` via `LD_PRELOAD`)
- **Konzept:** Inspiriert von @PolygonPrincess (`libgc570d-v4l2.so`).
- **Ziel:** Abfangen von V4L2-Systemaufrufen (`open`, `ioctl`, `mmap`) im Userspace, um Bilddaten direkt aus dem Shared Memory von `everlive_userd` zu lesen.
- **Vorteil:** Ermöglicht native V4L2-Nutzung in Discord, Chromium und OBS ohne Kernel-Module und ohne `v4l2loopback`.

### 2. RetroTINK 4K Pro & Custom Modelines
- **Anamorphes 2x Vertical-Scaling:** Automatische 2x Vertikal-Entzerrung bei `3840x1080` RetroTINK 120Hz/BFI-Signalen.
- **Sync Lock OSD Toast:** Dezent einblendbare Statusanzeige bei Signalerkennung und Mode-Switch.
- **Audio-Steuerung:** Software-Mute (<kbd>m</kbd>) und Gain-Regler direkt im C-Streamer.

### 3. GUI & Multi-Card Architektur (`libeverlive-core`)
- **RGB Color Wheel:** Freie Hex-Farbwahl via `QColorDialog` in `everlive_viewer.py`.
- **Modulare Hardware-Abstraktion:** Saubere Trennung von PCIe-DMA-Engine und karten-spezifischen Register-Tabellen (Vorbereitung für GC570D Live Gamer DUO / GC575 2.1).
