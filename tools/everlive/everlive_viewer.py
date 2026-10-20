#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
EverLive - Live Gamer 4K (GC573) Control Center & Video Viewer
Desktop Studio with low-latency direct-DMA preview for Linux gaming.
"""

import sys
import os
import signal
import subprocess
import time
import argparse
import re
import json
from typing import Optional, Tuple, Dict

__version__ = "0.4.0"

CONFIG_DIR = os.path.expanduser("~/.config/everlive")
CONFIG_FILE = os.path.join(CONFIG_DIR, "settings.json")


def load_settings() -> dict:
    if os.path.isfile(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, "r") as f:
                return json.load(f)
        except Exception:
            pass
    return {}


def save_settings(data: dict):
    try:
        os.makedirs(CONFIG_DIR, exist_ok=True)
        cur = load_settings()
        cur.update(data)
        with open(CONFIG_FILE, "w") as f:
            json.dump(cur, f, indent=2)
    except Exception:
        pass

# PySide6 is optional when run with --cli
try:
    from PySide6.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QGridLayout, QLabel, QPushButton, QComboBox, QCheckBox, QSlider, QFrame,
        QGroupBox, QMessageBox, QStackedLayout, QSizePolicy
    )
    from PySide6.QtCore import Qt, QTimer, QThread, Signal, QEvent, QUrl, QSize
    from PySide6.QtGui import QGuiApplication, QFont, QIcon, QColor, QPalette, QKeySequence, QShortcut, QDesktopServices, QPixmap, QPainter
    HAS_QT = True
except ImportError:
    HAS_QT = False


def find_gc573_vdev() -> str:
    """Auto-detects the V4L2 device node for open_gc573."""
    import glob
    # 1. Check open_gc573 driver binding in sysfs
    for cand in sorted(glob.glob("/sys/bus/pci/drivers/open_gc573/*/video4linux/video*")):
        vdev_name = os.path.basename(cand)
        dev_path = f"/dev/{vdev_name}"
        if os.path.exists(dev_path):
            return dev_path

    # 2. Check /sys/class/video4linux/*/device/driver symlink
    for d in sorted(glob.glob("/sys/class/video4linux/video*")):
        try:
            driver_link = os.path.realpath(os.path.join(d, "device", "driver"))
            if os.path.basename(driver_link) == "open_gc573":
                dev_path = f"/dev/{os.path.basename(d)}"
                if os.path.exists(dev_path):
                    return dev_path
        except Exception:
            pass

    # 3. Check /sys/class/video4linux/*/name
    for name_file in sorted(glob.glob("/sys/class/video4linux/video*/name")):
        try:
            with open(name_file, "r") as f:
                name = f.read().lower()
                if "gc573" in name or "live gamer 4k" in name or "open_gc573" in name:
                    dev_path = f"/dev/{os.path.basename(os.path.dirname(name_file))}"
                    if os.path.exists(dev_path):
                        return dev_path
        except Exception:
            pass

    # Default fallback if card has not yet registered
    return "/dev/video0"


def find_gc573_adev() -> str:
    """Auto-detects ALSA capture card for GC573."""
    try:
        with open("/proc/asound/cards", "r") as f:
            for line in f:
                if "GC573" in line or "Live Gamer 4K" in line or "A4K" in line:
                    m = re.search(r"^\s*(\d+)\s+\[([^\]]+)\]", line)
                    if m:
                        card_id = m.group(2).strip()
                        return f"hw:CARD={card_id},DEV=0"
    except Exception:
        pass
    return "hw:CARD=A4K,DEV=0"


def find_pipewire_source_node() -> Optional[str]:
    """Finds the native PipeWire Audio/Source node.name for GC573 HDMI Capture."""
    try:
        out = subprocess.check_output(["pw-dump", "Node"], stderr=subprocess.DEVNULL)
        for n in json.loads(out):
            p = n.get("info", {}).get("props", {})
            if p.get("media.class") == "Audio/Source":
                driver = p.get("alsa.driver_name", "")
                card_id = p.get("alsa.id", "")
                card_name = p.get("alsa.card_name", "")
                node_name = p.get("node.name", "")
                node_desc = p.get("node.description", "")
                alsa_name = p.get("alsa.name", "")

                is_gc573 = (
                    driver == "open_gc573"
                    or card_id in ("A4K", "GC573")
                    or "live gamer 4k" in card_name.lower()
                    or "live gamer 4k" in node_desc.lower()
                    or "gc573" in node_name.lower()
                    or "gc573" in node_desc.lower()
                    or "gc573" in alsa_name.lower()
                )
                is_line = (
                    "line" in node_desc.lower()
                    or "line" in alsa_name.lower()
                    or "input-1" in node_name
                    or "pro-input-1" in node_name
                    or p.get("alsa.device") == 1
                )
                if is_gc573 and not is_line:
                    return node_name
    except Exception:
        pass

    # Secondary check via wpctl status if pw-dump didn't match directly
    try:
        import re
        out = subprocess.check_output(["wpctl", "status"], stderr=subprocess.DEVNULL).decode("utf-8", errors="ignore")
        in_sources = False
        for line in out.splitlines():
            if "Sources:" in line:
                in_sources = True
                continue
            elif in_sources and ("Sinks:" in line or "Filters:" in line or "Streams:" in line):
                break
            if in_sources and ("live gamer 4k" in line.lower() or "gc573" in line.lower() or "a4k" in line.lower()) and "line" not in line.lower():
                m = re.search(r"^\s*[│\s*]*(\d+)\.", line)
                if m:
                    wp_id = m.group(1)
                    # Resolve numeric wpctl ID to exact PipeWire node.name
                    try:
                        insp = subprocess.check_output(["wpctl", "inspect", wp_id], text=True, stderr=subprocess.DEVNULL)
                        for insp_line in insp.splitlines():
                            if "node.name" in insp_line and "=" in insp_line:
                                actual_name = insp_line.split("=")[-1].strip().strip('"')
                                if actual_name:
                                    return actual_name
                    except Exception:
                        pass
                    return wp_id
    except Exception:
        pass

    return None


DEFAULT_VDEV = find_gc573_vdev()
DEFAULT_ADEV = find_gc573_adev()

PRESETS = {
    "Auto (HDMI Signal)": None,
    "3840x2160 @ 60 FPS":  {"width": 3840, "height": 2160, "fps": 60,  "fmt": "YUYV"},
    "3840x2160 @ 30 FPS":  {"width": 3840, "height": 2160, "fps": 30,  "fmt": "YUYV"},
    "3840x2160 @ 24 FPS":  {"width": 3840, "height": 2160, "fps": 24,  "fmt": "YUYV"},
    "2880x2160 @ 60 FPS (4:3 4K)": {"width": 2880, "height": 2160, "fps": 60, "fmt": "YUYV"},
    "3840x1080 @ 120 FPS": {"width": 3840, "height": 1080, "fps": 120, "fmt": "YUYV"},
    "3840x1080 @ 60 FPS":  {"width": 3840, "height": 1080, "fps": 60,  "fmt": "YUYV"},
    "2560x1440 @ 144 FPS": {"width": 2560, "height": 1440, "fps": 144, "fmt": "YUYV"},
    "2560x1440 @ 120 FPS": {"width": 2560, "height": 1440, "fps": 120, "fmt": "YUYV"},
    "2560x1440 @ 100 FPS": {"width": 2560, "height": 1440, "fps": 100, "fmt": "YUYV"},
    "2560x1440 @ 60 FPS":  {"width": 2560, "height": 1440, "fps": 60,  "fmt": "YUYV"},
    "2560x1080 @ 144 FPS": {"width": 2560, "height": 1080, "fps": 144, "fmt": "YUYV"},
    "2560x1080 @ 60 FPS":  {"width": 2560, "height": 1080, "fps": 60,  "fmt": "YUYV"},
    "1920x1080 @ 240 FPS": {"width": 1920, "height": 1080, "fps": 240, "fmt": "YUYV"},
    "1920x1080 @ 180 FPS": {"width": 1920, "height": 1080, "fps": 180, "fmt": "YUYV"},
    "1920x1080 @ 144 FPS": {"width": 1920, "height": 1080, "fps": 144, "fmt": "YUYV"},
    "1920x1080 @ 120 FPS": {"width": 1920, "height": 1080, "fps": 120, "fmt": "YUYV"},
    "1920x1080 @ 100 FPS": {"width": 1920, "height": 1080, "fps": 100, "fmt": "YUYV"},
    "1920x1080 @ 60 FPS":  {"width": 1920, "height": 1080, "fps": 60,  "fmt": "YUYV"},
    "1920x1080 @ 50 FPS":  {"width": 1920, "height": 1080, "fps": 50,  "fmt": "YUYV"},
    "1280x720 @ 60 FPS":   {"width": 1280, "height": 720,  "fps": 60,  "fmt": "YUYV"},
    "720x480 @ 60 FPS (480p)": {"width": 720, "height": 480, "fps": 60, "fmt": "YUYV"},
    "720x576 @ 50 FPS (576p)": {"width": 720, "height": 576, "fps": 50, "fmt": "YUYV"},
    "1920x240 @ 60 FPS (240p CRT)": {"width": 1920, "height": 240, "fps": 60, "fmt": "YUYV"},
    "1440x240 @ 60 FPS (240p)": {"width": 1440, "height": 240, "fps": 60, "fmt": "YUYV"},
    "2880x240 @ 60 FPS (240p RT4K)": {"width": 2880, "height": 240, "fps": 60, "fmt": "YUYV"},
}

LED_MODES = {
    "Off (Dark)": 0,
    "Blue (Default)": 1,
    "Green": 2,
    "Red": 3,
    "Cyan": 4,
    "Magenta": 5,
    "Yellow": 6,
    "White": 7,
}

LED_NAME_MAP = {
    "0": 0, "off": 0, "aus": 0, "dark": 0,
    "1": 1, "blue": 1, "blau": 1,
    "2": 2, "green": 2, "gruen": 2, "grün": 2,
    "3": 3, "red": 3, "rot": 3,
    "4": 4, "cyan": 4, "tuerkis": 4, "türkis": 4,
    "5": 5, "magenta": 5, "lila": 5, "purple": 5,
    "6": 6, "yellow": 6, "gelb": 6,
    "7": 7, "white": 7, "weiss": 7, "weiß": 7,
}

LED_COLORS = {
    0: "#334155",  # Aus (Dunkel)
    1: "#3b82f6",  # Blau (Standard)
    2: "#10b981",  # Grün
    3: "#ef4444",  # Rot
    4: "#06b6d4",  # Cyan / Türkis
    5: "#d946ef",  # Magenta / Lila
    6: "#eab308",  # Gelb
    7: "#f8fafc",  # Weiß
}


def create_led_icon(mode: int, size: int = 14):
    """Creates a smooth anti-aliased circular color swatch for the LED mode dropdown."""
    if not HAS_QT:
        return None
    pix = QPixmap(size, size)
    pix.fill(Qt.transparent)
    painter = QPainter(pix)
    painter.setRenderHint(QPainter.Antialiasing)
    color_hex = LED_COLORS.get(mode, "#94a3b8")
    painter.setBrush(QColor(color_hex))
    if mode == 0:
        painter.setPen(QColor("#64748b"))
    elif mode == 7:
        painter.setPen(QColor("#94a3b8"))
    else:
        painter.setPen(Qt.NoPen)
    painter.drawEllipse(1, 1, size - 2, size - 2)
    painter.end()
    return QIcon(pix)


def find_led_sysfs_path() -> Optional[str]:
    """Locates the open_gc573 standard multicolor LED class or legacy sysfs led_mode attribute."""
    import glob
    led_class = "/sys/class/leds/gc573:rgb:status"
    if os.path.exists(led_class):
        return led_class
    candidates = glob.glob("/sys/bus/pci/drivers/open_gc573/*/led_mode")
    if candidates and os.path.exists(candidates[0]):
        return candidates[0]
    return None


def read_led_mode() -> int:
    """Reads current LED mode from kernel driver."""
    path = find_led_sysfs_path()
    if not path:
        return -1
    try:
        if os.path.isdir(path):
            # Official Linux Multicolor LED classdev
            with open(os.path.join(path, "brightness"), "r") as f:
                brightness = int(f.read().strip())
            if brightness == 0:
                return 0
            with open(os.path.join(path, "multi_intensity"), "r") as f:
                vals = [int(x) > 0 for x in f.read().strip().split()]
            if len(vals) >= 3:
                r, g, b = vals[0], vals[1], vals[2]
                modes = {
                    (False, False, False): 0,
                    (False, False, True): 1,
                    (False, True, False): 2,
                    (True, False, False): 3,
                    (False, True, True): 4,
                    (True, False, True): 5,
                    (True, True, False): 6,
                    (True, True, True): 7,
                }
                return modes.get((r, g, b), 1)
            return 1
        else:
            with open(path, "r") as f:
                return int(f.read().strip())
    except Exception:
        pass
    return -1


def write_led_mode(mode: int) -> bool:
    """Applies new LED mode to hardware in real-time and persists it."""
    path = find_led_sysfs_path()
    if not path:
        return False
    success = False
    try:
        if os.path.isdir(path):
            # Official Linux Multicolor LED classdev
            colors = {
                0: ((0, 0, 0), 0),
                1: ((0, 0, 255), 255),
                2: ((0, 255, 0), 255),
                3: ((255, 0, 0), 255),
                4: ((0, 255, 255), 255),
                5: ((255, 0, 255), 255),
                6: ((255, 255, 0), 255),
                7: ((255, 255, 255), 255),
            }
            rgb, brightness = colors.get(mode, ((0, 0, 0), 0))
            with open(os.path.join(path, "multi_intensity"), "w") as f:
                f.write(f"{rgb[0]} {rgb[1]} {rgb[2]}\n")
            with open(os.path.join(path, "brightness"), "w") as f:
                f.write(f"{brightness}\n")
            success = True
        else:
            with open(path, "w") as f:
                f.write(f"{mode}\n")
            success = True
    except Exception:
        success = False

    if success:
        save_settings({"led_mode": mode})
    return success


def find_hpd_pulse_sysfs_path() -> Optional[str]:
    """Locates the open_gc573 sysfs hpd_pulse attribute."""
    import glob
    candidates = glob.glob("/sys/bus/pci/drivers/open_gc573/*/hpd_pulse")
    if candidates and os.path.exists(candidates[0]):
        return candidates[0]
    return None


def trigger_hpd_pulse() -> bool:
    """Pulses the hardware HDMI Hot-Plug Detect line to trigger a physical HDMI re-sync."""
    path = find_hpd_pulse_sysfs_path()
    if not path:
        return False
    try:
        with open(path, "w") as f:
            f.write("1\n")
        return True
    except Exception:
        return False


def set_audio_volume(vol_pct: int):
    """Sets stream volume for EverLive via wpctl / pactl (0-150%)."""
    vol_float = vol_pct / 100.0
    targets = set()
    try:
        out = subprocess.check_output(["wpctl", "status"], text=True, stderr=subprocess.DEVNULL)
        in_audio = False
        for line in out.splitlines():
            line_s = line.strip()
            if line_s.startswith("Audio"):
                in_audio = True
            elif in_audio and (line_s.startswith("Video") or line_s.startswith("Settings")):
                in_audio = False
            if in_audio:
                # Target both the AVerMedia hardware capture source and active EverLive playback stream
                if (("AVerMedia Live Gamer 4K Pro" in line and "Pro 1" not in line) or
                    (any(s in line for s in ["EverLive", "everlive-audio", "gc573-audio"]) and "input." not in line)):
                    for part in line_s.split():
                        clean = part.strip(".│*")
                        if clean.isdigit():
                            targets.add(clean)
                            break
        for target in targets:
            subprocess.run(["wpctl", "set-volume", target, f"{vol_float:.2f}"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass

    try:
        out = subprocess.check_output(["pactl", "list", "sink-inputs"], text=True, stderr=subprocess.DEVNULL)
        curr_id = None
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("Sink-Eingabe #") or line.startswith("Sink Input #"):
                curr_id = line.split("#")[-1].strip()
            elif any(s in line for s in ["EverLive", "everlive-audio", "gc573-audio"]) and curr_id:
                subprocess.run(["pactl", "set-sink-input-volume", curr_id, f"{vol_pct}%"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass


def set_audio_mute(mute: bool):
    """Mutes or unmutes stream for EverLive via wpctl / pactl."""
    mute_arg = "1" if mute else "0"
    targets = set()
    try:
        out = subprocess.check_output(["wpctl", "status"], text=True, stderr=subprocess.DEVNULL)
        in_audio = False
        for line in out.splitlines():
            line_s = line.strip()
            if line_s.startswith("Audio"):
                in_audio = True
            elif in_audio and (line_s.startswith("Video") or line_s.startswith("Settings")):
                in_audio = False
            if in_audio:
                if (("AVerMedia Live Gamer 4K Pro" in line and "Pro 1" not in line) or
                    (any(s in line for s in ["EverLive", "everlive-audio", "gc573-audio"]) and "input." not in line)):
                    for part in line_s.split():
                        clean = part.strip(".│*")
                        if clean.isdigit():
                            targets.add(clean)
                            break
        for target in targets:
            subprocess.run(["wpctl", "set-mute", target, mute_arg],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass

    try:
        out = subprocess.check_output(["pactl", "list", "sink-inputs"], text=True, stderr=subprocess.DEVNULL)
        curr_id = None
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("Sink-Eingabe #") or line.startswith("Sink Input #"):
                curr_id = line.split("#")[-1].strip()
            elif any(s in line for s in ["EverLive", "everlive-audio", "gc573-audio"]) and curr_id:
                subprocess.run(["pactl", "set-sink-input-mute", curr_id, mute_arg],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass


class GameStreamer:
    """Manages low-latency video and audio child processes."""
    def __init__(self, vdev: Optional[str] = None, adev: Optional[str] = None, force_mpv: bool = False):
        self.vdev = vdev if vdev is not None else find_gc573_vdev()
        self.adev = adev if adev is not None else find_gc573_adev()
        self.pw_node = find_pipewire_source_node()
        self.force_mpv = force_mpv
        self.mpv_proc: Optional[subprocess.Popen] = None
        self.arecord_proc: Optional[subprocess.Popen] = None
        self.pwplay_proc: Optional[subprocess.Popen] = None
        self.is_running = False

    def get_signal_info(self) -> Dict[str, str]:
        """Queries current signal status via v4l2-ctl."""
        info = {
            "locked": False,
            "resolution": "No Signal",
            "framerate": "",
            "pixelformat": "",
            "audio": "8 Channels @ 48 kHz"
        }
        if not os.path.exists(self.vdev):
            info["resolution"] = f"{self.vdev} not found"
            return info

        try:
            out = subprocess.check_output(
                ["v4l2-ctl", "-d", self.vdev, "--query-dv-timings"],
                stderr=subprocess.DEVNULL, timeout=1.0
            ).decode()
            
            w_m = re.search(r"Active width:\s*(\d+)", out)
            h_m = re.search(r"Active height:\s*(\d+)", out)
            clk_m = re.search(r"Pixelclock:\s*(\d+)", out)
            tot_w = re.search(r"Total width:\s*(\d+)", out)
            tot_h = re.search(r"Total height:\s*(\d+)", out)
            il_m = re.search(r"interlaced", out, re.IGNORECASE)
            scan = "i" if il_m else "p"

            if w_m and h_m:
                w, h = int(w_m.group(1)), int(h_m.group(1))
                fps_str = "60 FPS"
                if clk_m and tot_w and tot_h:
                    clk = int(clk_m.group(1))
                    tw = int(tot_w.group(1))
                    th = int(tot_h.group(1))
                    if tw > 0 and th > 0:
                        fps_calc = clk / (tw * th)
                        if scan == "i":
                            fps_calc *= 2.0
                        if abs(fps_calc - round(fps_calc)) < 0.01:
                            fps_str = f"{int(round(fps_calc))} FPS"
                        else:
                            fps_str = f"{fps_calc:.2f} FPS"
                disp_w = 720 if (w == 1440 and (h == 480 or h == 576)) else w
                info["locked"] = True
                info["resolution"] = f"{disp_w}x{h}{scan}"
                info["framerate"] = fps_str
                info["chroma"] = self.chroma_label()
                info["hdr"] = self.hdr_label() or "SDR"
        except Exception:
            info["locked"] = False
            info["resolution"] = "No Signal"
            info["framerate"] = ""
            info["chroma"] = ""
            info["hdr"] = ""

        return info

    def chroma_label(self) -> str:
        """Reads active driver pixel format and returns friendly chroma description."""
        if not self.vdev or not os.path.exists(self.vdev):
            return ""
        try:
            out = subprocess.check_output(
                ["v4l2-ctl", "-d", self.vdev, "--get-fmt-video"],
                stderr=subprocess.DEVNULL, timeout=0.8
            ).decode()
            if "BGR3" in out or "RGB4" in out:
                return "RGB 4:4:4"
            if "NV12" in out:
                return "YUV 4:2:0"
            if "P010" in out:
                return "P010 4:2:0"
            if "YUYV" in out:
                return "YUV 4:2:2"
        except Exception:
            pass
        return "YUV 4:2:2"

    def hdr_label(self, fallback: bool = True) -> str:
        """SDR, HDR10 or HLG from the driver's cached metadata. No HDMI I2C."""
        if not self.vdev or not os.path.exists(self.vdev):
            return ""
        try:
            out = subprocess.check_output(
                ["v4l2-ctl", "-d", self.vdev, "-C", "hdr_mode"],
                stderr=subprocess.DEVNULL, timeout=0.8
            ).decode()
            for line in out.splitlines():
                if "hdr_mode" in line and ":" in line:
                    val = int(line.split(":")[-1].strip())
                    return {0: "SDR", 1: "HDR10", 2: "HDR10", 3: "HLG"}.get(val, "SDR")
        except Exception:
            pass
        if not fallback:
            return ""
        try:
            fmt_out = subprocess.check_output(
                ["v4l2-ctl", "-d", self.vdev, "--get-fmt-video"],
                stderr=subprocess.DEVNULL, timeout=0.8
            ).decode()
            if "SMPTE 2084" in fmt_out or "P010" in fmt_out:
                return "HDR10"
        except Exception:
            pass
        return ""

    def get_active_format(self) -> Tuple[Optional[str], Optional[str]]:
        """Reads active driver streaming resolution and framerate without touching HDMI I2C."""
        if not self.vdev or not os.path.exists(self.vdev):
            return None, None
        try:
            out = subprocess.check_output(
                ["v4l2-ctl", "-d", self.vdev, "--get-dv-timings"],
                stderr=subprocess.DEVNULL, timeout=0.5
            ).decode()
            w_m = re.search(r"Active width:\s*(\d+)", out)
            h_m = re.search(r"Active height:\s*(\d+)", out)
            clk_m = re.search(r"Pixelclock:\s*(\d+)", out)
            tot_w = re.search(r"Total width:\s*(\d+)", out)
            tot_h = re.search(r"Total height:\s*(\d+)", out)
            il_m = re.search(r"interlaced", out, re.IGNORECASE)
            scan = "i" if il_m else "p"
            if w_m and h_m:
                w, h = int(w_m.group(1)), int(h_m.group(1))
                fps_str = None
                if clk_m and tot_w and tot_h:
                    clk = int(clk_m.group(1))
                    tw = int(tot_w.group(1))
                    th = int(tot_h.group(1))
                    if tw > 0 and th > 0:
                        fps_calc = clk / (tw * th)
                        if scan == "i":
                            fps_calc *= 2.0
                        if abs(fps_calc - round(fps_calc)) < 0.01:
                            fps_str = f"{int(round(fps_calc))} FPS"
                        else:
                            fps_str = f"{fps_calc:.2f} FPS"
                disp_w = 720 if (w == 1440 and (h == 480 or h == 576)) else w
                return f"{disp_w}x{h}{scan}", fps_str
        except Exception:
            pass

        try:
            out = subprocess.check_output(
                ["v4l2-ctl", "-d", self.vdev, "--get-fmt-video"],
                stderr=subprocess.DEVNULL, timeout=0.5
            ).decode()
            m = re.search(r"Width/Height\s*:\s*(\d+)/(\d+)", out)
            il_m = re.search(r"Field\s*:\s*(Interlaced|Alternate|Top|Bottom)", out, re.IGNORECASE)
            scan = "i" if il_m else "p"
            if m:
                w, h = int(m.group(1)), int(m.group(2))
                disp_w = 720 if (w == 1440 and (h == 480 or h == 576)) else w
                if w > 0 and h > 0:
                    return f"{disp_w}x{h}{scan}", None
        except Exception:
            pass
        return None, None

    def get_active_resolution(self) -> Optional[str]:
        res, _ = self.get_active_format()
        return res

    def resync(self) -> bool:
        """Forces hardware HDMI HPD re-sync and updates timings via v4l2-ctl query."""
        trigger_hpd_pulse()
        time.sleep(0.35)
        if not os.path.exists(self.vdev):
            return False
        try:
            subprocess.run(
                ["v4l2-ctl", "-d", self.vdev, "--set-dv-bt-timings", "query"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=2.0
            )
            return True
        except Exception:
            return False

    def start(self, preset_name: str = "Auto (HDMI Signal)", fullscreen: bool = False, volume: float = 1.0, vsync: bool = False, streamer_mode: bool = False) -> bool:
        if self.is_running:
            return True

        self.vsync = vsync
        if not os.path.exists(self.vdev):
            print(f"[!] Error: Video device {self.vdev} does not exist.")
            return False

        preset = PRESETS.get(preset_name)
        width, height, fps = None, None, None

        if preset:
            width = preset["width"]
            height = preset["height"]
            fps = preset["fps"]
            fmt = preset["fmt"]
            # Apply fixed preset to V4L2 device
            try:
                subprocess.run(
                    ["v4l2-ctl", "-d", self.vdev, f"--set-fmt-video=width={width},height={height},pixelformat={fmt}"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1.0
                )
            except Exception as e:
                print(f"[*] Note while setting format: {e}")
        else:
            # Auto (HDMI Signal): Synchronous v4l2 query only needed for mpv fallback.
            # Native everlive_display manages DV-Timings and Standby asynchronously without blocking GUI.
            script_dir = os.path.dirname(os.path.abspath(__file__))
            native_bin = os.path.join(script_dir, "everlive_display")
            if not os.path.isfile(native_bin):
                native_bin = os.path.join(script_dir, "gc573_display")
            if self.force_mpv or not (os.path.isfile(native_bin) and os.access(native_bin, os.X_OK)):
                try:
                    subprocess.run(
                        ["v4l2-ctl", "-d", self.vdev, "--set-dv-bt-timings", "query"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1.0
                    )
                except Exception:
                    pass

        # Prefer native high-performance everlive_display binary (< 2 ms display latency)
        script_dir = os.path.dirname(os.path.abspath(__file__))
        native_bin = os.path.join(script_dir, "everlive_display")
        if not os.path.isfile(native_bin):
            native_bin = os.path.join(script_dir, "gc573_display")

        if not self.force_mpv and os.path.isfile(native_bin) and os.access(native_bin, os.X_OK):
            cmd = [native_bin, "-d", self.vdev, "-a", self.adev]
            if self.pw_node:
                cmd.extend(["--pw-node", self.pw_node])
            if fullscreen:
                cmd.append("-f")
            if vsync:
                cmd.append("--vsync")
            if streamer_mode:
                cmd.append("--streamer-mode")
            cmd.extend(["-v", f"{volume:.2f}"])
            print(f"[*] Starting native EverLive Direct-Renderer (Pure Native Wayland): {' '.join(cmd)}")
            try:
                self.mpv_proc = subprocess.Popen(cmd, start_new_session=True)
                self.is_running = True
                return True
            except Exception as e:
                print(f"[!] Error launching everlive_display ({e}), falling back to mpv...")

        # Fallback: optimized mpv command with zero-delay low-latency profile
        mpv_cmd = [
            "mpv",
            f"av://v4l2:{self.vdev}",
            "--title=EverLive - Live Gamer 4K",
            "--profile=low-latency",
            "--untimed",
            "--no-cache",
            "--no-correct-pts",
            "--demuxer-lavf-probe-info=no",
            "--demuxer-lavf-analyzeduration=0",
            "--demuxer-readahead-secs=0",
            "--demuxer-lavf-o=fflags=nobuffer+fastseek",
            "--demuxer-lavf-o-add=flags=low_delay",
            "--demuxer-lavf-o-add=probesize=32",
            "--video-latency-hacks=yes",
            "--interpolation=no",
            "--framedrop=vo",
            "--video-sync=display-desync",
            "--opengl-glfinish=no",
            "--vd-lavc-threads=1",
            "--vd-lavc-fast=yes",
            "--vo=gpu",
            "--cursor-autohide=1000",
            "--no-terminal",
            "--input-default-bindings=yes",
        ]

        if fullscreen:
            mpv_cmd.append("--fullscreen")
        if streamer_mode:
            mpv_cmd.extend(["--no-osc", "--osd-level=0", "--cursor-autohide=always"])

        try:
            mpv_env = os.environ.copy()
            self.mpv_proc = subprocess.Popen(mpv_cmd, env=mpv_env, start_new_session=True)
            self.is_running = True
            if self.adev and self.adev != "none":
                try:
                    mpv_pid = self.mpv_proc.pid if self.mpv_proc else os.getpid()
                    props = (
                        "{ application.name = \"EverLive\" application.icon-name = \"everlive\" "
                        "application.id = \"everlive\" application.process.binary = \"everlive\" "
                        f"application.process.id = {mpv_pid} "
                        "node.name = \"everlive-audio\" node.description = \"EverLive Game Audio\" "
                        "media.name = \"HDMI Game Audio\" media.role = \"Game\" }"
                    )
                    client_env = (
                        "PIPEWIRE_PROPS='application.name=\"EverLive\" application.icon-name=\"everlive\" application.id=\"everlive\"' "
                    )
                    if self.pw_node:
                        # Full 8-channel downmixing matrix to ensure 5.1 and 7.1 LPCM (Center dialogue, Surround)
                        # are cleanly folded into stereo without dropped channels or volume loss
                        cmd = (
                            f"{client_env}pw-loopback -i '{{ target.object = \"{self.pw_node}\" audio.position = [ AUX0, AUX1, AUX2, AUX3, AUX4, AUX5, AUX6, AUX7 ] resample.disable = false }}' -l 40 "
                            f"-o '{{ audio.position = [ FL, FR ] channelmix.matrix = [ [ 1.0, 0.0, 0.707, 0.5, 0.707, 0.0, 0.5, 0.0 ], [ 0.0, 1.0, 0.707, 0.5, 0.0, 0.707, 0.0, 0.5 ] ] resample.disable = false resample.quality = 4 {props[1:-1]} }}' 2>/dev/null"
                        )
                    else:
                        plug_adev = f"plughw:{self.adev[3:]}" if self.adev.startswith("hw:") else self.adev
                        cmd = (
                            f"{{ arecord -D '{self.adev}' -t wav -f S16_LE -c 2 -r 48000 --period-time=20000 --buffer-time=80000 -q 2>/dev/null || "
                            f"   arecord -D '{plug_adev}' -t wav -f S16_LE -c 2 -r 48000 --period-time=20000 --buffer-time=80000 -q 2>/dev/null; }} | "
                            f"{client_env}pw-play --latency=40ms -P '{props}' - 2>/dev/null"
                        )
                    self.pwplay_proc = subprocess.Popen(cmd, shell=True, start_new_session=True)
                except Exception as ae:
                    print(f"[*] Note while starting audio stream: {ae}")
            return True
        except Exception as e:
            print(f"[!] Failed to launch mpv: {e}")
            self.stop()
            return False

    def stop(self):
        self.is_running = False
        if self.mpv_proc:
            try:
                pgid = os.getpgid(self.mpv_proc.pid)
                os.killpg(pgid, signal.SIGTERM)
            except Exception:
                try:
                    self.mpv_proc.terminate()
                except Exception:
                    pass
            try:
                self.mpv_proc.wait(timeout=1.0)
            except Exception:
                try:
                    pgid = os.getpgid(self.mpv_proc.pid)
                    os.killpg(pgid, signal.SIGKILL)
                except Exception:
                    try:
                        self.mpv_proc.kill()
                    except Exception:
                        pass
            self.mpv_proc = None

        if self.pwplay_proc:
            try:
                pgid = os.getpgid(self.pwplay_proc.pid)
                os.killpg(pgid, signal.SIGTERM)
            except Exception:
                try:
                    self.pwplay_proc.terminate()
                except Exception:
                    pass
            try:
                self.pwplay_proc.wait(timeout=0.5)
            except Exception:
                try:
                    pgid = os.getpgid(self.pwplay_proc.pid)
                    os.killpg(pgid, signal.SIGKILL)
                except Exception:
                    try:
                        self.pwplay_proc.kill()
                    except Exception:
                        pass
            self.pwplay_proc = None

        if self.arecord_proc:
            try:
                pgid = os.getpgid(self.arecord_proc.pid)
                os.killpg(pgid, signal.SIGTERM)
            except Exception:
                try:
                    self.arecord_proc.terminate()
                except Exception:
                    pass
            try:
                self.arecord_proc.wait(timeout=0.5)
            except Exception:
                try:
                    pgid = os.getpgid(self.arecord_proc.pid)
                    os.killpg(pgid, signal.SIGKILL)
                except Exception:
                    try:
                        self.arecord_proc.kill()
                    except Exception:
                        pass
            self.arecord_proc = None

    def poll(self) -> bool:
        """Returns True if mpv is still alive."""
        if self.mpv_proc:
            if self.mpv_proc.poll() is not None:
                self.stop()
                return False
            return True
        return False


if HAS_QT:
    class MainWindow(QMainWindow):
        def __init__(self, streamer: GameStreamer, vsync: bool = False, streamer_mode: bool = False):
            super().__init__()
            self.streamer = streamer
            self.enable_vsync = vsync
            self.muted = False
            self.saved_cfg = load_settings()
            if streamer_mode:
                self.saved_cfg["streamer_mode"] = True

            self.init_ui()

            # Signal monitor timer (1s polling)
            self.timer = QTimer(self)
            self.timer.timeout.connect(self.update_status)
            self.timer.start(1000)

            # Check if autostart is requested
            if self.saved_cfg.get("autostart", False) or getattr(streamer, "_cli_autostart", False):
                QTimer.singleShot(250, self.start_gaming_screen)

        def init_ui(self):
            self.setWindowTitle("EverLive - Live Gamer 4K Console Hub")
            self.setFixedSize(690, 520)

            # Application & Window Icon
            icon_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "everlive.png")
            if os.path.isfile(icon_path):
                self.setWindowIcon(QIcon(icon_path))

            # EverLive Clean Dark Studio Theme
            self.setStyleSheet("""
                QMainWindow {
                    background-color: #0b0d13;
                }
                QFrame#cardFrame {
                    background-color: #12151f;
                    border: 1px solid #1f2537;
                    border-radius: 8px;
                    padding: 14px;
                }
                QFrame#bottomBar {
                    background-color: #080a0e;
                    border-top: 1px solid #171b26;
                    padding: 5px 12px;
                }
                QLabel {
                    color: #e2e8f0;
                    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
                }
                QLabel#logoTitle {
                    color: #ffffff;
                    font-size: 15px;
                    font-weight: 700;
                    letter-spacing: 0.5px;
                }
                QLabel#logoVersion {
                    color: #64748b;
                    font-size: 11px;
                    font-weight: 500;
                }
                QLabel#badgeModel {
                    color: #94a3b8;
                    background-color: #1a202c;
                    border: 1px solid #2d3748;
                    border-radius: 4px;
                    padding: 2px 7px;
                    font-size: 10px;
                    font-weight: 600;
                    letter-spacing: 0.5px;
                }
                QLabel#pillLive {
                    border-radius: 12px;
                    padding: 4px 12px;
                    font-size: 11px;
                    font-weight: 600;
                }
                QLabel#sectionTitle {
                    color: #64748b;
                    font-size: 11px;
                    font-weight: 700;
                    letter-spacing: 0.8px;
                }
                QLabel#heroSignal {
                    color: #ffffff;
                    font-size: 18px;
                    font-weight: 700;
                }
                QLabel#heroMeta {
                    color: #94a3b8;
                    font-size: 12px;
                }
                QLabel#engineBadge {
                    background-color: #0d2318;
                    color: #34d399;
                    border: 1px solid #164e32;
                    border-radius: 4px;
                    padding: 3px 8px;
                    font-size: 11px;
                    font-weight: 600;
                }
                QComboBox {
                    background-color: #171b26;
                    color: #f1f5f9;
                    border: 1px solid #2b3348;
                    border-radius: 6px;
                    padding: 6px 10px;
                    font-size: 12px;
                    min-height: 22px;
                }
                QComboBox:hover {
                    border-color: #3b82f6;
                }
                QComboBox::drop-down {
                    border: none;
                    width: 20px;
                }
                QCheckBox {
                    color: #94a3b8;
                    font-size: 12px;
                    spacing: 8px;
                }
                QCheckBox:hover {
                    color: #f1f5f9;
                }
                QCheckBox::indicator {
                    width: 15px;
                    height: 15px;
                    border-radius: 3px;
                    border: 1px solid #2d3748;
                    background-color: #171b26;
                }
                QCheckBox::indicator:hover {
                    border-color: #3b82f6;
                }
                QCheckBox::indicator:checked {
                    background-color: #2563eb;
                    border-color: #3b82f6;
                }
                QPushButton {
                    background-color: #1a202c;
                    color: #e2e8f0;
                    border: 1px solid #2d3748;
                    border-radius: 6px;
                    padding: 6px 14px;
                    font-size: 12px;
                    font-weight: 500;
                }
                QPushButton:hover {
                    background-color: #252d3d;
                    border-color: #3b82f6;
                }
                QPushButton:pressed {
                    background-color: #141822;
                }
                QPushButton#btnDiscord {
                    background-color: #5865F2;
                    color: #ffffff;
                    border: none;
                    border-radius: 4px;
                    padding: 4px 10px;
                    font-size: 11px;
                    font-weight: 600;
                }
                QPushButton#btnDiscord:hover {
                    background-color: #4752c4;
                }
                QPushButton#btnMute {
                    background-color: #1a202c;
                    color: #e2e8f0;
                    border: 1px solid #2d3748;
                    border-radius: 5px;
                    padding: 4px 6px;
                    font-size: 11px;
                    font-weight: 600;
                }
                QPushButton#btnMute:hover {
                    background-color: #252d3d;
                    border-color: #3b82f6;
                }
                QPushButton#btnLaunchStart {
                    background-color: #10b981;
                    color: #ffffff;
                    border: none;
                    font-size: 14px;
                    font-weight: 600;
                    border-radius: 6px;
                    padding: 11px 18px;
                }
                QPushButton#btnLaunchStart:hover {
                    background-color: #059669;
                }
                QPushButton#btnLaunchStop {
                    background-color: #ef4444;
                    color: #ffffff;
                    border: none;
                    font-size: 14px;
                    font-weight: 600;
                    border-radius: 6px;
                    padding: 11px 18px;
                }
                QPushButton#btnLaunchStop:hover {
                    background-color: #dc2626;
                }
                QSlider::groove:horizontal {
                    height: 4px;
                    background: #1e2537;
                    border-radius: 2px;
                }
                QSlider::sub-page:horizontal {
                    background: #3b82f6;
                    border-radius: 2px;
                }
                QSlider::handle:horizontal {
                    background: #e2e8f0;
                    width: 14px;
                    margin-top: -5px;
                    margin-bottom: -5px;
                    border-radius: 7px;
                }
                QSlider::handle:horizontal:hover {
                    background: #ffffff;
                }
            """)

            central_widget = QWidget()
            self.setCentralWidget(central_widget)
            root_layout = QVBoxLayout(central_widget)
            root_layout.setContentsMargins(18, 14, 18, 12)
            root_layout.setSpacing(12)

            # -------------------------------------------------------------
            # Top Bar: Logo, Title, Badge, Discord, Signal Pill
            # -------------------------------------------------------------
            top_bar = QHBoxLayout()
            top_bar.setSpacing(10)

            # Application branding icon
            if os.path.isfile(icon_path):
                lbl_icon = QLabel()
                pix = QPixmap(icon_path).scaled(22, 22, Qt.KeepAspectRatio, Qt.SmoothTransformation)
                lbl_icon.setPixmap(pix)
                lbl_icon.setFixedSize(22, 22)
                top_bar.addWidget(lbl_icon)

            lbl_logo = QLabel("EVERLIVE")
            lbl_logo.setObjectName("logoTitle")
            top_bar.addWidget(lbl_logo)

            lbl_badge = QLabel("LIVE GAMER 4K")
            lbl_badge.setObjectName("badgeModel")
            top_bar.addWidget(lbl_badge)

            lbl_ver = QLabel(f"v{__version__}")
            lbl_ver.setObjectName("logoVersion")
            top_bar.addWidget(lbl_ver)

            top_bar.addStretch(1)

            btn_discord = QPushButton("Discord")
            btn_discord.setObjectName("btnDiscord")
            btn_discord.setCursor(Qt.PointingHandCursor)
            btn_discord.setToolTip("Join the EverLive Discord community (Feedback & Hardware Support)")
            btn_discord.clicked.connect(lambda: QDesktopServices.openUrl(QUrl("https://discord.com/invite/D2XFhkPMmc")))
            top_bar.addWidget(btn_discord)

            self.lbl_pill = QLabel("○ Standby")
            self.lbl_pill.setObjectName("pillLive")
            self.lbl_pill.setStyleSheet("background-color: #1a202c; color: #94a3b8; border: 1px solid #2d3748;")
            top_bar.addWidget(self.lbl_pill)

            root_layout.addLayout(top_bar)

            # -------------------------------------------------------------
            # Card 1: Signal & Launch Controls
            # -------------------------------------------------------------
            card_hero = QFrame()
            card_hero.setObjectName("cardFrame")
            v_hero = QVBoxLayout(card_hero)
            v_hero.setSpacing(12)
            v_hero.setContentsMargins(14, 12, 14, 14)

            h_sig_info = QHBoxLayout()
            v_sig_text = QVBoxLayout()
            v_sig_text.setSpacing(3)

            self.lbl_hero_signal = QLabel("Waiting for HDMI signal...")
            self.lbl_hero_signal.setObjectName("heroSignal")
            v_sig_text.addWidget(self.lbl_hero_signal)

            self.lbl_hero_meta = QLabel("Checking input source (Console / PC)...")
            self.lbl_hero_meta.setObjectName("heroMeta")
            v_sig_text.addWidget(self.lbl_hero_meta)

            h_sig_info.addLayout(v_sig_text, 1)

            lbl_engine_badge = QLabel("Direct Wayland (< 2ms)")
            lbl_engine_badge.setObjectName("engineBadge")
            h_sig_info.addWidget(lbl_engine_badge, 0, Qt.AlignVCenter)

            v_hero.addLayout(h_sig_info)

            # Confident Action Button
            self.btn_launch = QPushButton("Start Gaming Screen")
            self.btn_launch.setObjectName("btnLaunchStart")
            self.btn_launch.setCursor(Qt.PointingHandCursor)
            self.btn_launch.clicked.connect(self.toggle_stream)
            v_hero.addWidget(self.btn_launch)

            # Launch Options (2x2 Grid)
            grid_opts = QGridLayout()
            grid_opts.setHorizontalSpacing(24)
            grid_opts.setVerticalSpacing(8)

            self.chk_fullscreen = QCheckBox("Start in Fullscreen")
            self.chk_fullscreen.setChecked(self.saved_cfg.get("fullscreen", True))
            self.chk_fullscreen.toggled.connect(lambda val: save_settings({"fullscreen": val}))
            grid_opts.addWidget(self.chk_fullscreen, 0, 0)

            self.chk_vsync = QCheckBox("Enable VSync (120 Hz)")
            self.chk_vsync.setChecked(self.saved_cfg.get("vsync", self.enable_vsync))
            self.chk_vsync.toggled.connect(lambda val: save_settings({"vsync": val}))
            grid_opts.addWidget(self.chk_vsync, 0, 1)

            self.chk_autostart = QCheckBox("Auto-start on launch")
            self.chk_autostart.setChecked(self.saved_cfg.get("autostart", False))
            self.chk_autostart.toggled.connect(lambda val: save_settings({"autostart": val}))
            grid_opts.addWidget(self.chk_autostart, 1, 0)

            self.chk_streamer = QCheckBox("Streamer Mode (Clean Feed)")
            self.chk_streamer.setToolTip("Hides mouse control bar, HUD, and overlays for clean recordings (Hotkey: 'S')")
            self.chk_streamer.setChecked(self.saved_cfg.get("streamer_mode", False))
            self.chk_streamer.toggled.connect(lambda val: save_settings({"streamer_mode": val}))
            grid_opts.addWidget(self.chk_streamer, 1, 1)

            v_hero.addLayout(grid_opts)
            root_layout.addWidget(card_hero)

            # -------------------------------------------------------------
            # Card 2: Hardware & Capture Settings
            # -------------------------------------------------------------
            card_settings = QFrame()
            card_settings.setObjectName("cardFrame")
            v_settings = QVBoxLayout(card_settings)
            v_settings.setSpacing(10)
            v_settings.setContentsMargins(14, 12, 14, 14)

            lbl_sec_settings = QLabel("HARDWARE & CAPTURE")
            lbl_sec_settings.setObjectName("sectionTitle")
            v_settings.addWidget(lbl_sec_settings)

            grid = QGridLayout()
            grid.setHorizontalSpacing(14)
            grid.setVerticalSpacing(10)

            # Row 0: Preset & LED
            lbl_preset = QLabel("Format / Preset:")
            lbl_preset.setStyleSheet("font-weight: 500; color: #94a3b8;")
            grid.addWidget(lbl_preset, 0, 0)

            self.combo_presets = QComboBox()
            self.combo_presets.addItems(list(PRESETS.keys()))
            if "preset" in self.saved_cfg and self.saved_cfg["preset"] in PRESETS:
                self.combo_presets.setCurrentText(self.saved_cfg["preset"])
            self.combo_presets.currentIndexChanged.connect(self.on_preset_changed)
            grid.addWidget(self.combo_presets, 0, 1)

            lbl_led = QLabel("Card LED:")
            lbl_led.setStyleSheet("font-weight: 500; color: #94a3b8;")
            grid.addWidget(lbl_led, 0, 2)

            self.combo_led = QComboBox()
            self.combo_led.setIconSize(QSize(14, 14))
            for name, val in LED_MODES.items():
                icon = create_led_icon(val)
                if icon:
                    self.combo_led.addItem(icon, name, val)
                else:
                    self.combo_led.addItem(name, val)

            saved_led = self.saved_cfg.get("led_mode", 1)
            if saved_led is not None and isinstance(saved_led, int) and 0 <= saved_led <= 7:
                write_led_mode(saved_led)
                idx = self.combo_led.findData(saved_led)
                if idx >= 0:
                    self.combo_led.setCurrentIndex(idx)
            else:
                curr_led = read_led_mode()
                if curr_led >= 0:
                    idx = self.combo_led.findData(curr_led)
                    if idx >= 0:
                        self.combo_led.setCurrentIndex(idx)
                else:
                    self.combo_led.setEnabled(False)
            self.combo_led.currentIndexChanged.connect(self.on_led_changed)
            grid.addWidget(self.combo_led, 0, 3)

            # Row 1: Volume & Re-Sync
            lbl_audio = QLabel("Volume:")
            lbl_audio.setStyleSheet("font-weight: 500; color: #94a3b8;")
            grid.addWidget(lbl_audio, 1, 0)

            h_vol = QHBoxLayout()
            h_vol.setSpacing(8)
            self.btn_mute = QPushButton("Mute")
            self.btn_mute.setObjectName("btnMute")
            self.btn_mute.setFixedWidth(66)
            self.btn_mute.setCursor(Qt.PointingHandCursor)
            self.btn_mute.clicked.connect(self.toggle_mute)
            h_vol.addWidget(self.btn_mute)

            self.slider_vol = QSlider(Qt.Horizontal)
            self.slider_vol.setRange(0, 150)
            initial_vol = int(self.saved_cfg.get("volume", 1.0) * 100)
            self.slider_vol.setValue(initial_vol)
            self.slider_vol.valueChanged.connect(self.on_volume_changed)
            h_vol.addWidget(self.slider_vol, 1)

            self.lbl_vol = QLabel(f"{initial_vol}%")
            self.lbl_vol.setFixedWidth(34)
            self.lbl_vol.setStyleSheet("color: #94a3b8; font-size: 11px;")
            h_vol.addWidget(self.lbl_vol)
            grid.addLayout(h_vol, 1, 1)

            lbl_sync = QLabel("HDMI Sync:")
            lbl_sync.setStyleSheet("font-weight: 500; color: #94a3b8;")
            grid.addWidget(lbl_sync, 1, 2)

            self.btn_resync = QPushButton("Re-sync Signal")
            self.btn_resync.setCursor(Qt.PointingHandCursor)
            self.btn_resync.setToolTip("Trigger hardware HDMI handshake (HPD pulse) - also key 'H' in window")
            self.btn_resync.clicked.connect(self.force_resync)
            grid.addWidget(self.btn_resync, 1, 3)

            v_settings.addLayout(grid)
            root_layout.addWidget(card_settings)

            # -------------------------------------------------------------
            # Bottom Bar: Device Info & Shortcuts
            # -------------------------------------------------------------
            bottom_box = QFrame()
            bottom_box.setObjectName("bottomBar")
            b_layout = QHBoxLayout(bottom_box)
            b_layout.setContentsMargins(4, 2, 4, 2)

            if self.streamer.pw_node:
                audio_str = f"PipeWire ({self.streamer.pw_node})"
            else:
                audio_str = f"ALSA ({self.streamer.adev})"
            lbl_vdev = QLabel(f"V4L2: {self.streamer.vdev} • Audio: {audio_str}")
            lbl_vdev.setStyleSheet("color: #64748b; font-size: 11px;")
            b_layout.addWidget(lbl_vdev)

            b_layout.addStretch(1)

            lbl_hints = QLabel("F Fullscreen • Tab Stats • S Streamer • M Mute • H Re-Sync • Esc Exit")
            lbl_hints.setStyleSheet("color: #64748b; font-size: 11px;")
            b_layout.addWidget(lbl_hints)

            root_layout.addWidget(bottom_box)

            # Initial status poll
            self.update_status()

        def toggle_stream(self):
            if self.streamer.is_running:
                self.streamer.stop()
                self.btn_launch.setText("Start Gaming Screen")
                self.btn_launch.setObjectName("btnLaunchStart")
                self.btn_launch.setStyle(self.btn_launch.style())
            else:
                self.start_gaming_screen()

        def start_gaming_screen(self):
            if self.streamer.is_running:
                return
            preset = self.combo_presets.currentText()
            vol = self.slider_vol.value() / 100.0
            is_fs = self.chk_fullscreen.isChecked()
            is_vsync = self.chk_vsync.isChecked()
            is_streamer = self.chk_streamer.isChecked()
            save_settings({
                "preset": preset,
                "volume": vol,
                "fullscreen": is_fs,
                "vsync": is_vsync,
                "streamer_mode": is_streamer,
            })
            success = self.streamer.start(preset_name=preset, fullscreen=is_fs, volume=vol, vsync=is_vsync, streamer_mode=is_streamer)
            if success:
                self.btn_launch.setText("Stop Gaming Screen")
                self.btn_launch.setObjectName("btnLaunchStop")
                self.btn_launch.setStyle(self.btn_launch.style())
                set_audio_volume(self.slider_vol.value())
                QTimer.singleShot(600, lambda: set_audio_volume(self.slider_vol.value()))

        def on_preset_changed(self):
            preset = self.combo_presets.currentText()
            save_settings({"preset": preset})
            if self.streamer.is_running:
                self.streamer.stop()
                QTimer.singleShot(150, self.start_gaming_screen)

        def on_led_changed(self, index: int):
            mode = self.combo_led.currentData()
            if mode is not None:
                write_led_mode(mode)

        def toggle_mute(self):
            self.muted = not self.muted
            if self.muted:
                self.btn_mute.setText("Unmute")
                self.btn_mute.setStyleSheet("background-color: #36171d; color: #f87171; border: 1px solid #7f1d1d;")
            else:
                self.btn_mute.setText("Mute")
                self.btn_mute.setStyleSheet("")
            set_audio_mute(self.muted)

        def on_volume_changed(self, val: int):
            self.lbl_vol.setText(f"{val}%")
            save_settings({"volume": val / 100.0})
            if not self.muted:
                set_audio_volume(val)

        def force_resync(self, *args):
            """Forces HDMI handshake renegotiation and restarts stream if active."""
            if hasattr(self, "_resyncing") and self._resyncing:
                return
            self._resyncing = True
            if hasattr(self, "btn_resync"):
                self.btn_resync.setEnabled(False)
                self.btn_resync.setText("Synchronizing...")
            try:
                was_running = self.streamer.is_running
                if was_running:
                    self.streamer.stop()
                    self.btn_launch.setText("Start Gaming Screen")
                    self.btn_launch.setObjectName("btnLaunchStart")
                    self.btn_launch.setStyle(self.btn_launch.style())
                self.streamer.resync()
                self.update_status()
                if was_running:
                    QTimer.singleShot(600, lambda: self._wait_and_restart_after_resync(12))
            except Exception as e:
                print(f"[!] Error during HDMI re-sync: {e}")
            finally:
                QTimer.singleShot(2500, lambda: (
                    setattr(self, "_resyncing", False),
                    hasattr(self, "btn_resync") and self.btn_resync.setEnabled(True),
                    hasattr(self, "btn_resync") and self.btn_resync.setText("Re-sync Signal")
                ))

        def _wait_and_restart_after_resync(self, attempts=12):
            if self.streamer.is_running:
                return
            sig = self.streamer.get_signal_info()
            if sig.get("active") or attempts <= 1:
                self.start_gaming_screen()
            else:
                QTimer.singleShot(250, lambda: self._wait_and_restart_after_resync(attempts - 1))

        def _show_live(self, info):
            hdr_str = f" • {info['hdr']}" if info.get("hdr") else ""
            chroma_str = f" • {info['chroma']}" if info.get("chroma") else ""
            self.lbl_pill.setText(f"● {info['resolution']} @ {info['framerate']}{chroma_str}{hdr_str}")
            self.lbl_pill.setStyleSheet("background-color: #0d2318; color: #34d399; border: 1px solid #164e32;")
            res_clean = re.sub(r"(\d+)x(\d+)([pi]?)", r"\1 × \2\3", info['resolution'])
            self.lbl_hero_signal.setText(f"{res_clean} @ {info['framerate']}")
            hdr_val = info.get('hdr', 'SDR')
            chroma_val = info.get('chroma', 'YUV 4:2:2')
            self.lbl_hero_meta.setText(f"Signal locked • Chroma: {chroma_val} • Color: {hdr_val} • 8ch LPCM (PipeWire)")

        def update_status(self):
            # While the gaming screen is open, only read the cached driver format and HDR control.
            # query-dv-timings hits the HDMI receiver and stalls the stream.
            if self.streamer.is_running:
                if not self.streamer.poll():
                    # Window was closed by user
                    self.btn_launch.setText("Start Gaming Screen")
                    self.btn_launch.setObjectName("btnLaunchStart")
                    self.btn_launch.setStyle(self.btn_launch.style())
                    return
                live = getattr(self, "_live", None)
                active_res, active_fps = self.streamer.get_active_format()
                hdr = self.streamer.hdr_label(fallback=False)
                chroma = self.streamer.chroma_label()
                if live and live.get("locked"):
                    updated = False
                    live = dict(live)
                    if active_res and active_res != live.get("resolution"):
                        live["resolution"] = active_res
                        updated = True
                    if active_fps and active_fps != live.get("framerate"):
                        live["framerate"] = active_fps
                        updated = True
                    if hdr and hdr != live.get("hdr"):
                        live["hdr"] = hdr
                        updated = True
                    if chroma and chroma != live.get("chroma"):
                        live["chroma"] = chroma
                        updated = True
                    if updated:
                        self._live = live
                        self._show_live(live)
                return

            info = self.streamer.get_signal_info()
            self._live = info
            if info["locked"]:
                self._show_live(info)
            else:
                self.lbl_pill.setText("○ Standby")
                self.lbl_pill.setStyleSheet("background-color: #1a202c; color: #94a3b8; border: 1px solid #2d3748;")
                self.lbl_hero_signal.setText("No HDMI Signal")
                self.lbl_hero_meta.setText("Waiting for input signal (e.g. power on console or PC)...")

        def closeEvent(self, event):
            self.streamer.stop()
            event.accept()


def main():
    parser = argparse.ArgumentParser(description="EverLive - Live Gamer 4K Console Gaming Screen")
    parser.add_argument("-f", "--fullscreen", action="store_true", help="Launch directly in fullscreen")
    parser.add_argument("-p", "--preset", default="Auto (HDMI Signal)", choices=list(PRESETS.keys()), help="Resolution preset")
    parser.add_argument("--vsync", action="store_true", help="Enable VSync for smooth tearing-free presentation")
    parser.add_argument("-s", "--streamer-mode", "--clean", action="store_true", help="Streamer Mode: Clean feed without HUD or controls")
    parser.add_argument("--cli", action="store_true", help="Launch directly in CLI mode without Qt GUI")
    parser.add_argument("--mpv", action="store_true", help="Force mpv backend instead of everlive_display")
    parser.add_argument("--led", type=str, default=None,
                        help="Change GC573 LED lighting immediately (0-7 or off/blue/green/red/cyan/magenta/yellow/white)")
    parser.add_argument("-v", "--volume", type=float, default=1.0, help="Audio volume (0.0 to 1.5)")
    parser.add_argument("-d", "--device", default=None, help="V4L2 video device (e.g. /dev/video0, default: auto-detect)")
    parser.add_argument("-a", "--audio", default=None, help="ALSA audio device (e.g. hw:CARD=A4K,DEV=0, default: auto-detect)")
    args = parser.parse_args()

    if args.led is not None:
        key = args.led.lower().strip()
        val = LED_NAME_MAP.get(key)
        if val is None:
            try:
                val = int(key)
            except ValueError:
                pass
        if val is not None and 0 <= val <= 7:
            if write_led_mode(val):
                print(f"[+] GC573 LED lighting set to mode {val}.")
            else:
                print(f"[!] Failed to set GC573 LED to mode {val}.")
        else:
            print(f"[!] Invalid LED mode '{args.led}'. Valid: {list(LED_NAME_MAP.keys())}")
        if not args.cli and not any(arg in sys.argv for arg in ["-f", "--fullscreen", "-p", "--preset"]):
            sys.exit(0)

    streamer = GameStreamer(vdev=args.device, adev=args.audio, force_mpv=args.mpv)
    if args.fullscreen or args.streamer_mode:
        streamer._cli_autostart = True

    print(f"[*] GC573 Hardware: Video={streamer.vdev}, Audio={streamer.adev}")

    # Graceful shutdown on Ctrl+C
    def sig_handler(sig, frame):
        streamer.stop()
        sys.exit(0)
    signal.signal(signal.SIGINT, sig_handler)
    signal.signal(signal.SIGTERM, sig_handler)

    if args.cli or not HAS_QT:
        print("[*] Launching Gaming Screen in low-latency mode...")
        info = streamer.get_signal_info()
        chroma_disp = f" [{info['chroma']}]" if info.get('chroma') else ""
        print(f"[*] Signal: {info['resolution']} {info['framerate']}{chroma_disp}".strip())
        if not info["locked"]:
            print("[!] Warning: No active HDMI signal detected on GC573.")
            print("    Please check if the video source is powered on and sending video.")
        if not streamer.start(preset_name=args.preset, fullscreen=args.fullscreen, volume=args.volume, vsync=args.vsync, streamer_mode=args.streamer_mode):
            sys.exit(1)
        print("[+] Window opened. Press 'q' in window or Ctrl+C to exit.")
        while streamer.poll():
            time.sleep(0.5)
        streamer.stop()
    else:
        QGuiApplication.setDesktopFileName("everlive")
        app = QApplication(sys.argv)
        app.setApplicationName("EverLive")
        icon_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "everlive.png")
        if os.path.isfile(icon_path):
            app.setWindowIcon(QIcon(icon_path))
        window = MainWindow(streamer, vsync=args.vsync, streamer_mode=args.streamer_mode)
        window.show()
        sys.exit(app.exec())


if __name__ == "__main__":
    main()
