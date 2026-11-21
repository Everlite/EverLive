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
import dataclasses
import json
import threading
import shutil
from pathlib import Path
from typing import Optional, Tuple, Dict, List

if os.environ.get("DISPLAY") and "--cli" not in sys.argv:
    os.environ.setdefault("QT_QPA_PLATFORM", "xcb")

try:
    import app_integrations
    import everlive_cam
    import everlive_multi
    import everlive_recorder
    import fa_icons
    import service_manager
    import ui_theme
    import ui_widgets
    from card_profile import detect_card_profile, identify_pci_card, CAP_EDID
    from ui_widgets import (IconButton, SegmentedControl, StatusChip,
                            ToggleRow)
    from everlive_client import (EverLiveIpcClient, probe as ipc_probe,
                                 paused_state as handoff_paused_state)
except ImportError:  # imported as a module from another working directory
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import app_integrations
    import everlive_cam
    import everlive_multi
    import everlive_recorder
    import fa_icons
    import service_manager
    import ui_theme
    import ui_widgets
    from card_profile import detect_card_profile, identify_pci_card, CAP_EDID
    from ui_widgets import (IconButton, SegmentedControl, StatusChip,
                            ToggleRow)
    from everlive_client import (EverLiveIpcClient, probe as ipc_probe,
                                 paused_state as handoff_paused_state)

__version__ = "2.0.0"

CONFIG_DIR = os.path.expanduser("~/.config/everlive")
CONFIG_FILE = os.path.join(CONFIG_DIR, "settings.json")

GC573_VENDOR_ID = "0x1461"
GC573_DEVICE_IDS = {"0x0054", "0x0055"}
GC570D_SUBSYSTEM_ID = "0x5700"
GC573_SUBSYSTEM_ID = "0x5730"
MODEL_GC573 = "gc573"
MODEL_GC570D = "gc570d"
MODEL_HWSUHD = "hwsuhd"
MODEL_GC553G2 = "gc553g2"
MODEL_NAMES = {
    MODEL_GC573: "GC573 • LIVE GAMER 4K",
    MODEL_GC570D: "GC570D • LIVE GAMER DUO",
    MODEL_HWSUHD: "HWS UHD • 4K60 CAPTURE",
    MODEL_GC553G2: "GC553G2 • LIVE GAMER ULTRA 2.1",
}
# HWS UHD family (ACASIS VS2583): its own engine in tools/openhws-userd.
HWSUHD_PCI_IDS = {("0x8888", "0x8581"), ("0x8888", "0x85a1"),
                  ("0x8888", "0x8591"), ("0x1f33", "0x8581")}
HWSUHD_SUBSYSTEM_ID = "0x0007"

try:
    from . import gc553g2_control as _gc553g2_control
except Exception:
    try:
        import gc553g2_control as _gc553g2_control
    except Exception:
        _gc553g2_control = None
ENGINE_VFIO_TEXT = "● Userspace VFIO"
V4L2_DEFAULT_DEVICE = "/dev/video10"
# One breakpoint for the compact header label and compact chrome.
HEADER_COMPACT_WIDTH = 1340
# The EDID readout is the first telemetry detail to run out of room.
DECK_EDID_WIDTH = 1440
V4L2_DUO_SECOND_DEVICE = "/dev/video11"
AUDIO_TAP_NODE = "everlive-capture-audio"

ASPECT_OPTIONS = [
    ("Auto (Native HDMI)", "auto"),
    ("16:9 Anamorphic (RetroTINK / OSSC)", "16:9"),
    ("4:3 Retro", "4:3"),
    ("Stretch to Fill", "stretch"),
]
ASPECT_KEYS = {key for _, key in ASPECT_OPTIONS}

OSD_POSITIONS = ("top", "bottom")
OSD_POSITION_OPTIONS = [
    ("Top", "top"),
    ("Bottom", "bottom"),
]
OSD_POSITION_ALIASES = {
    "tr": "top", "tl": "top", "top-right": "top", "top-left": "top",
    "br": "bottom", "bl": "bottom", "bottom-right": "bottom",
    "bottom-left": "bottom",
}


def normalize_osd_position(pos: str) -> str:
    pos = OSD_POSITION_ALIASES.get(pos, pos)
    return pos if pos in OSD_POSITIONS else "top"

SCALE_OPTIONS = [
    ("Aspect Fit (Bilinear)", "fit"),
    ("Integer Scale (Nearest)", "integer"),
]
SCALE_MODES = {key for _, key in SCALE_OPTIONS}

# lsfg-vk frame generation (gc573_user_streamer --framegen).
FRAMEGEN_OPTIONS = [
    ("Off — Native Ultra-Low Latency", "off"),
    ("2× Frame Gen", "2x"),
    ("3× Frame Gen", "3x"),
    ("4× Frame Gen", "4x"),
]
FRAMEGEN_KEYS = {key for _, key in FRAMEGEN_OPTIONS}

FRAMEGEN_BACKEND_OPTIONS = [
    ("LSFG-VK (External, Beta)", "lsfg"),
]

# Older settings files / scripts used 60to120/30to60 or shader-pipeline names.
# Map them onto the 2x/3x/4x multiplier + dedup flag.
FRAMEGEN_ALIASES = {
    "60to120": "2x",
    "30to60": "2x",
    "30to90": "3x",
    "30to120": "4x",
    "interp": "2x",
    "interpolation": "2x",
    "60": "2x",
    "120": "2x",
    "adaptive": "2x",
    "max": "2x",
    "maxhz": "2x",
    "30": "2x",
    "ps5": "2x",
    "none": "off",
    "native": "off",
}

DEFAULT_SETTINGS = {
    "v4l2_loopback": False,
    "aspect_ratio": "auto",
    "osd_enabled": True,
    "osd_position": "top",
    "quicknav_enabled": True,
    "quicknav_fs_only": False,
    "audio_tap": False,
    "hdr_tonemap": True,
    "hdr_exposure": 0.0,
    "scale_mode": "fit",
    "audio_channels": "auto",
    "framegen_mode": "off",
    "fg_multiplier": 2,
    "fg_backend": "lsfg",
    "fg_dedup": False,
    "fg_flow_scale": 1.0,
    "fg_perf_mode": False,
    "fg_low_latency": True,
    "minimize_to_tray": True,
    "duo_input": 0,
    "duo_edid_bridge": True,
    "popout_window": False,
    "ui_mode": "simple",
    "rec_fps": "auto",
    "rec_source": "auto",
    "rec_encoder": "auto",
    "rec_quality": "high",
    "rec_format": "mkv",
    "rec_audio": "everlive",
    "rec_dir": "",
}


def load_settings() -> dict:
    data = {}
    if os.path.isfile(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, "r") as f:
                loaded = json.load(f)
            if isinstance(loaded, dict):
                data = loaded
        except Exception:
            pass

    settings = dict(DEFAULT_SETTINGS)
    settings.update(data)
    if not isinstance(settings.get("v4l2_loopback"), bool):
        settings["v4l2_loopback"] = False
    if not isinstance(settings.get("minimize_to_tray"), bool):
        settings["minimize_to_tray"] = True
    if settings.get("aspect_ratio") not in ASPECT_KEYS:
        settings["aspect_ratio"] = "auto"
    if not isinstance(settings.get("osd_enabled"), bool):
        settings["osd_enabled"] = True
    settings["osd_position"] = normalize_osd_position(
        settings.get("osd_position"))
    if not isinstance(settings.get("audio_tap"), bool):
        settings["audio_tap"] = False
    if not isinstance(settings.get("hdr_tonemap"), bool):
        settings["hdr_tonemap"] = True
    if not isinstance(settings.get("hdr_exposure"), (int, float)):
        settings["hdr_exposure"] = 0.0
    settings["hdr_exposure"] = max(-2.0, min(2.0, float(settings["hdr_exposure"])))
    if settings.get("scale_mode") not in SCALE_MODES:
        settings["scale_mode"] = "fit"
    if settings.get("audio_channels") not in AUDIO_CHANNEL_KEYS:
        settings["audio_channels"] = "auto"
    # lsfg-vk frame generation.  Migrate legacy 60to120/30to60 & shader-era keys.
    raw_fg = str(settings.get("framegen_mode", "off")).lower()
    if "fg_dedup" not in data and raw_fg in ("30to60", "30to90", "30to120", "30", "ps5", "adaptive", "max", "maxhz"):
        settings["fg_dedup"] = True
    fg_mode = FRAMEGEN_ALIASES.get(raw_fg, raw_fg)
    if fg_mode not in FRAMEGEN_KEYS:
        fg_mode = "off"
    settings["framegen_mode"] = fg_mode
    if fg_mode in ("2x", "3x", "4x"):
        settings["fg_multiplier"] = int(fg_mode[0])
    else:
        try:
            mult_val = int(settings.get("fg_multiplier", 2))
            settings["fg_multiplier"] = max(2, min(4, mult_val))
        except (TypeError, ValueError):
            settings["fg_multiplier"] = 2
    if not isinstance(settings.get("fg_dedup"), bool):
        settings["fg_dedup"] = False
    try:
        settings["fg_flow_scale"] = max(
            0.5, min(1.0, float(settings.get("fg_flow_scale", 1.0))))
    except (TypeError, ValueError):
        settings["fg_flow_scale"] = 1.0
    if not isinstance(settings.get("fg_perf_mode"), bool):
        settings["fg_perf_mode"] = False
    if not isinstance(settings.get("fg_low_latency"), bool):
        settings["fg_low_latency"] = True
    # The OpenGL shader pipeline (HUD protection, CAS) no longer exists.
    for stale in ("framegen_hud_protect", "cas_enabled", "cas_strength"):
        settings.pop(stale, None)
    if settings.get("duo_input") not in (0, 1):
        settings["duo_input"] = 0
    if not isinstance(settings.get("duo_edid_bridge"), bool):
        settings["duo_edid_bridge"] = True
    if not isinstance(settings.get("popout_window"), bool):
        settings["popout_window"] = False
    if settings.get("ui_mode") not in ("simple", "advanced"):
        settings["ui_mode"] = "simple"
    if settings.get("rec_fps") not in everlive_recorder.REC_FPS_KEYS:
        settings["rec_fps"] = "auto"
    if settings.get("rec_source") not in everlive_recorder.REC_SOURCE_KEYS:
        settings["rec_source"] = "auto"
    if settings.get("rec_encoder") not in everlive_recorder.REC_ENCODER_KEYS:
        settings["rec_encoder"] = "auto"
    if settings.get("rec_quality") not in everlive_recorder.REC_QUALITY_KEYS:
        settings["rec_quality"] = "high"
    if settings.get("rec_format") not in everlive_recorder.REC_FORMAT_KEYS:
        settings["rec_format"] = "mkv"
    if settings.get("rec_audio") not in everlive_recorder.REC_AUDIO_KEYS:
        settings["rec_audio"] = "everlive"
    if not isinstance(settings.get("rec_dir"), str):
        settings["rec_dir"] = ""
    # "bitstream" used to be a separate EDID profile.  Every built-in profile
    # now advertises AC-3/DTS, so old settings migrate to the unified default.
    if settings.get("edid_profile") == "bitstream":
        settings["edid_profile"] = "default"
    return settings


def save_settings(data: dict):
    try:
        os.makedirs(CONFIG_DIR, exist_ok=True)
        cur = load_settings()
        cur.update(data)
        with open(CONFIG_FILE, "w") as f:
            json.dump(cur, f, indent=2)
    except Exception:
        pass


def _resolve_lsfg_dll(candidate: str) -> str:
    """Resolve a candidate shader DLL path for lsfg-vk 2.x.

    If a stale config or env var points at Windows ``Lossless.dll``, prefer a
    sibling ``lsfg-vk.dll`` (from Steam's ``lsfg-vk`` beta branch) and reject
    bare ``Lossless.dll`` which lacks the precompiled Vulkan SPIR-V shaders.
    """
    if not candidate:
        return ""
    if os.path.basename(candidate).lower() == "lossless.dll":
        sibling = os.path.join(os.path.dirname(candidate), "lsfg-vk.dll")
        if os.path.isfile(sibling) and os.access(sibling, os.R_OK):
            return sibling
        return ""
    if os.path.isfile(candidate) and os.access(candidate, os.R_OK):
        return candidate
    return ""


def _lsfg_dll_from_conf(path: str) -> str:
    """Return the ``dll = "..."`` value from lsfg-vk's conf.toml, if usable."""
    try:
        with open(path, "r", errors="ignore") as f:
            for raw in f:
                line = raw.lstrip()
                if not line.lower().startswith("dll"):
                    continue
                first = line.find('"')
                if first < 0:
                    continue
                last = line.find('"', first + 1)
                if last < 0:
                    continue
                resolved = _resolve_lsfg_dll(line[first + 1:last])
                if resolved:
                    return resolved
    except OSError:
        pass
    return ""


def _lsfg_dll_from_libraryfolders(path: str) -> Tuple[str, bool]:
    """Scan a Steam libraryfolders.vdf for a Lossless Scaling install across all library paths.

    Returns ``(lsfg_vk_dll_path, legacy_lossless_dll_found)``.
    """
    try:
        with open(path, "r", errors="ignore") as f:
            body = f.read()
    except OSError:
        return "", False

    legacy_found = False
    lowered = body.lower()
    pos = 0
    found_candidate = ""
    while True:
        pos = lowered.find('"path"', pos)
        if pos < 0:
            break
        pos += len('"path"')
        first = body.find('"', pos)
        if first < 0:
            break
        last = body.find('"', first + 1)
        if last < 0:
            break
        library = body[first + 1:last].replace("\\", "/")
        game_dir = os.path.join(
            library, "steamapps", "common", "Lossless Scaling")
        candidate = os.path.join(game_dir, "lsfg-vk.dll")
        if os.path.isfile(candidate) and os.access(candidate, os.R_OK):
            return candidate, True
        legacy = os.path.join(game_dir, "Lossless.dll")
        if os.path.isfile(legacy) and os.access(legacy, os.R_OK):
            legacy_found = True
        pos = last + 1

    return found_candidate, legacy_found


def probe_lsfg_vk() -> Dict[str, object]:
    """Locate the lsfg-vk Vulkan layer and lsfg-vk.dll without running tools.

    Mirrors ``gc573_fg_probe_system()`` in ``gc573_user_framegen.c`` so the Hub
    can show readiness before a stream is even started, and after the user
    installs the layer or enables the Steam beta while the Hub stays open.
    """
    layer_dir = ""
    home = os.path.expanduser("~")
    xdg_data = os.environ.get("XDG_DATA_HOME") or os.path.join(
        home, ".local", "share")
    search = [
        "/usr/share/vulkan/implicit_layer.d",
        "/etc/vulkan/implicit_layer.d",
        os.path.join(xdg_data, "vulkan", "implicit_layer.d"),
    ]
    for directory in search:
        try:
            for name in os.listdir(directory):
                lower = name.lower()
                if not lower.endswith(".json"):
                    continue
                hit = "lsfg" in lower
                if not hit:
                    try:
                        with open(os.path.join(directory, name), "r",
                                  errors="ignore") as f:
                            body = f.read().lower()
                        hit = "lsfg" in body or "lossless" in body
                    except OSError:
                        pass
                if hit:
                    layer_dir = directory
                    break
        except OSError:
            continue
        if layer_dir:
            break

    dll_path = _resolve_lsfg_dll(os.environ.get("LSFGVK_DLL_PATH") or "")
    legacy_dll_found = False
    if not dll_path:
        xdg_cfg = os.environ.get("XDG_CONFIG_HOME") or os.path.join(
            home, ".config")
        dll_path = _lsfg_dll_from_conf(
            os.path.join(xdg_cfg, "lsfg-vk", "conf.toml"))
    if not dll_path:
        for root in ("~/.local/share/Steam", "~/.steam/steam"):
            base = os.path.expanduser(root)
            game_dir = os.path.join(
                base, "steamapps", "common", "Lossless Scaling")
            candidate = os.path.join(game_dir, "lsfg-vk.dll")
            if os.path.isfile(candidate) and os.access(candidate, os.R_OK):
                dll_path = candidate
                break
            legacy = os.path.join(game_dir, "Lossless.dll")
            if os.path.isfile(legacy) and os.access(legacy, os.R_OK):
                legacy_dll_found = True
            dll_path, vdf_legacy = _lsfg_dll_from_libraryfolders(
                os.path.join(base, "steamapps", "libraryfolders.vdf"))
            if vdf_legacy:
                legacy_dll_found = True
            if dll_path:
                break

    return {
        "layer_found": bool(layer_dir),
        "dll_found": bool(dll_path),
        "legacy_dll_found": legacy_dll_found,
        "layer_dir": layer_dir,
        "dll_path": dll_path,
    }


def _read_sysfs(path: str) -> str:
    try:
        with open(path, "r") as f:
            return f.read().strip()
    except OSError:
        return ""


def _identify_card(dev: str) -> Optional[str]:
    """Model slug for a supported capture card at sysfs path ``dev``."""
    return identify_pci_card(dev)


def detect_capture_cards() -> List[Dict[str, Optional[str]]]:
    """Every supported capture card, in PCI address order.

    GC573 (Live Gamer 4K) and GC570D (Live Gamer DUO) both use PCI device
    1461:0054.  Only ``subsystem_device`` separates them: 0x5730 is the GC573
    and 0x5700 is the GC570D.  The BOLT (1461:0055) is always a GC573-class
    device.  HWS UHD cards have no kernel-mode path in EverLive: they are
    either on vfio-pci ("vfio") or still need binding ("unknown").
    In VFIO mode there is intentionally no /dev/video* node and no ALSA card;
    the userspace engine owns the device instead.
    """
    import glob

    cards: List[Dict[str, Optional[str]]] = []
    for dev in sorted(glob.glob("/sys/bus/pci/devices/*")):
        model = _identify_card(dev)
        if model is None:
            continue
        driver = None
        mode = "unknown"
        driver_link = os.path.join(dev, "driver")
        try:
            if os.path.islink(driver_link):
                driver = os.path.basename(os.path.realpath(driver_link))
                if driver == "vfio-pci":
                    mode = "vfio"
                elif model != MODEL_HWSUHD and driver in ("open_gc573", "gc570d"):
                    mode = "kernel"
        except OSError:
            pass
        subsystem = _read_sysfs(os.path.join(dev, "subsystem_device")).lower()
        cards.append({
            "mode": mode,
            "driver": driver,
            "pci": os.path.basename(dev),
            "subsystem": subsystem or None,
            "model": model,
        })
    return cards


def card_display_name(card: Dict[str, Optional[str]]) -> str:
    """Short selector label, e.g. "GC573 · 85:00.0"."""
    short = {MODEL_GC573: "GC573", MODEL_GC570D: "GC570D",
             MODEL_HWSUHD: "HWS UHD"}.get(card.get("model") or "", "Card")
    pci = card.get("pci") or ""
    if pci.startswith("0000:"):
        pci = pci[5:]
    return f"{short} · {pci}"


def select_capture_card(cards: List[Dict[str, Optional[str]]]
                        ) -> Optional[Dict[str, Optional[str]]]:
    """Pick the card this session uses.

    1. ``EVERLIVE_CARD_PCI`` (play.sh exports the card it prepared),
    2. ``EVERLIVE_CARD`` model slug (gc573, gc570d, hwsuhd),
    3. the card chosen in the Hub's card selector (``card_pci`` setting),
    4. otherwise the first AVerMedia card, then an HWS UHD card.
    A choice that names a card which is not installed is ignored.
    """
    if not cards:
        return None
    by_pci = {c["pci"]: c for c in cards}
    pci = os.environ.get("EVERLIVE_CARD_PCI", "").strip()
    if pci in by_pci:
        return by_pci[pci]
    want = os.environ.get("EVERLIVE_CARD", "").strip().lower()
    for card in cards:
        if want and card["model"] == want:
            return card
    saved = load_settings().get("card_pci")
    if isinstance(saved, str) and saved in by_pci:
        return by_pci[saved]
    for card in cards:
        if card["model"] != MODEL_HWSUHD:
            return card
    return cards[0]


def detect_gc573_driver() -> Dict[str, Optional[str]]:
    """Return the selected card's binding and model: vfio, kernel, or unknown."""
    result: Dict[str, Optional[str]] = {
        "mode": "unknown",
        "driver": None,
        "pci": None,
        "subsystem": None,
        "model": MODEL_GC573,
    }
    card = select_capture_card(detect_capture_cards())
    if card is not None:
        result.update(card)
    elif _gc553g2_control is not None:
        usb_devs = _gc553g2_control.find_gc553g2_devices()
        if usb_devs:
            result["mode"] = "uvc"
            result["driver"] = "uvcvideo"
            result["model"] = MODEL_GC553G2
            result["usb_port"] = usb_devs[0].name
    return result


# Engines the Hub started for its multiview tiles.  They own other cards,
# so the single-engine checks below ("a streamer is already running",
# stopping a stale DUO daemon) must not see them.
_MULTIVIEW_PIDS: set = set()


def _proc_running(name_prefix: str, any_user: bool = False) -> bool:
    """Match /proc/<pid>/comm (15-char safe) without relying on pgrep.

    Only this user's processes count by default: an engine that belongs to
    another user is not ours to stop or wait for.  It releases the capture
    card on request instead (see everlive_handoff.h), so it must not block a
    launch from here.  Multiview engines (``_MULTIVIEW_PIDS``) are ignored.
    """
    try:
        pids = os.listdir("/proc")
    except OSError:
        return False

    uid = os.getuid()
    for pid in pids:
        if not pid.isdigit() or int(pid) in _MULTIVIEW_PIDS:
            continue
        try:
            if not any_user and os.stat(f"/proc/{pid}").st_uid != uid:
                continue
            with open(f"/proc/{pid}/comm", "r") as f:
                if f.read().strip().startswith(name_prefix):
                    return True
        except OSError:
            pass
    return False


def userd_dir(model: str = MODEL_GC573) -> str:
    folder = ("opengc570d-userd" if model == MODEL_GC570D else
              "openhws-userd" if model == MODEL_HWSUHD else "opengc573-userd")
    return os.path.normpath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "..", folder))


def userd_bin(name: str, model: str = MODEL_GC573) -> str:
    return os.path.join(userd_dir(model), name)


def _stop_vfio_daemon(wait_s: float = 0.8):
    """Release BAR0/VFIO before the userspace streamer starts."""
    for cmd in (["systemctl", "--user", "stop", "everlive-userd"],
                ["pkill", "-x", "everlive_userd"],
                ["pkill", "-x", "opengc570d-userd"]):
        try:
            subprocess.run(cmd, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        except Exception:
            pass
    deadline = time.time() + wait_s
    while time.time() < deadline:
        if not (_proc_running("everlive_userd") or
                _proc_running("opengc570d-user")):
            return
        time.sleep(0.05)


def _format_rec_dir_display(path: str) -> str:
    """Shorten a recording directory path for display using '~' for $HOME."""
    raw = os.path.normpath(str(path or ""))
    home = os.path.normpath(os.path.expanduser("~"))
    if raw == home:
        return "~"
    if raw.startswith(home + os.sep):
        return "~" + raw[len(home):]
    return raw


def _vfio_streamer_running(any_user: bool = False) -> bool:
    # Both binaries are longer than 15 chars, so /proc/<pid>/comm is
    # truncated to "gc573_user_stre" or "opengc570d-user".
    return (_proc_running("gc573_user_stre", any_user) or
            _proc_running("opengc570d-user", any_user))


def _duo_service_active() -> bool:
    """True when the installed GC570D headless user unit is running."""
    try:
        out = subprocess.check_output(
            ["systemctl", "--user", "is-active", "opengc570d-userd"],
            text=True, stderr=subprocess.DEVNULL, timeout=2.0)
    except Exception:
        return False
    return out.strip() == "active"


def _stop_duo_service() -> bool:
    """Stop the headless GC570D unit; True when it had been active."""
    if not _duo_service_active():
        return False
    try:
        subprocess.run(["systemctl", "--user", "stop", "opengc570d-userd"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=10.0)
    except Exception:
        return False
    return True


def _start_duo_service():
    try:
        subprocess.run(["systemctl", "--user", "start", "opengc570d-userd"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=10.0)
    except Exception:
        pass


def _raise_memlock_limit():
    """Native 4K frames need ~17 MB of pinned VFIO memory.

    The GUI launches the streamer directly (not via play-intern.sh), so it
    must raise its own soft limit before fork/exec.  This is best-effort;
    the streamer reports a clean error if the hard limit is still too small.
    """
    try:
        import resource
        soft, hard = resource.getrlimit(resource.RLIMIT_MEMLOCK)
        try:
            resource.setrlimit(resource.RLIMIT_MEMLOCK,
                               (resource.RLIM_INFINITY,
                                resource.RLIM_INFINITY))
            return
        except (ValueError, OSError):
            pass
        if soft != hard:
            try:
                resource.setrlimit(resource.RLIMIT_MEMLOCK, (hard, hard))
            except (ValueError, OSError):
                pass
    except Exception:
        pass

# PySide6 is optional when run with --cli
try:
    from PySide6.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QGridLayout, QLabel, QPushButton, QComboBox, QCheckBox, QSlider, QFrame,
        QGroupBox, QMessageBox, QStackedLayout, QSizePolicy, QFileDialog,
        QSystemTrayIcon, QMenu, QDoubleSpinBox, QAbstractSpinBox,
        QTabWidget, QStackedWidget, QScrollArea, QDialog, QTextEdit,
        QGraphicsOpacityEffect
    )
    from PySide6.QtCore import Qt, QTimer, QThread, Signal, QEvent, QUrl, QSize, QRectF, QPointF, QProcess
    from PySide6.QtGui import (
        QGuiApplication, QFont, QIcon, QColor, QPalette, QKeySequence, QShortcut,
        QDesktopServices, QPixmap, QPainter, QAction, QPen, QBrush,
        QLinearGradient, QPainterPath, QImage, QRegion
    )
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

    # 4. Check USB UVC GC553G2 (07ca:2553) if no PCIe V4L2 node was found
    if _gc553g2_control is not None:
        usb_devs = _gc553g2_control.find_gc553g2_devices()
        if usb_devs:
            uvc_node = _gc553g2_control.find_gc553g2_video_node(usb_devs[0])
            if uvc_node and os.path.exists(uvc_node):
                return uvc_node

    # Default fallback if card has not yet registered
    return "/dev/video0"


def find_gc573_adev() -> str:
    """Auto-detects ALSA capture card for GC573 / GC553G2."""
    try:
        with open("/proc/asound/cards", "r") as f:
            for line in f:
                low = line.lower()
                if any(tok in low for tok in ("gc573", "live gamer 4k", "a4k", "gc553g2", "live gamer ultra")):
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
                    or card_id in ("A4K", "GC573", "GC553G2")
                    or "live gamer 4k" in card_name.lower()
                    or "live gamer 4k" in node_desc.lower()
                    or "live gamer ultra" in card_name.lower()
                    or "live gamer ultra" in node_desc.lower()
                    or "gc573" in node_name.lower()
                    or "gc573" in node_desc.lower()
                    or "gc573" in alsa_name.lower()
                    or "gc553g2" in node_name.lower()
                    or "gc553g2" in node_desc.lower()
                    or "gc553g2" in alsa_name.lower()
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


PRESETS = {
    "Auto (HDMI Signal)": None,
    "3840x2160 @ 144 FPS": {"width": 3840, "height": 2160, "fps": 144, "fmt": "MJPG"},
    "3840x2160 @ 120 FPS": {"width": 3840, "height": 2160, "fps": 120, "fmt": "MJPG"},
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
    "Blue": 1,
    "Green": 2,
    "Red": 3,
    "Cyan": 4,
    "Magenta": 5,
    "Yellow": 6,
    "White": 7,
    "Rainbow Wave (Multi-Zone)": 8,
    "Spectrum Cycle (Breathing)": 9,
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
    "8": 8, "wave": 8, "welle": 8, "dream": 8, "softdream": 8,
    "9": 9, "rainbow": 9, "cycle": 9, "regenbogen": 9,
}

# Hardware EDID profiles understood by gc573_user_streamer --edid.
# The display text is what the user sees; the key is the CLI value.
# Every built-in profile advertises L-PCM, Dolby Digital (AC-3) and DTS 5.1,
# so compressed audio is detected and decoded without a profile switch.
EDID_PROFILES = [
    ("Default (4K60 / 1440p120 HDR)", "default"),
    ("Force 1440p @ 120 Hz", "1440p120"),
    ("Force 1080p @ 240 Hz", "1080p240"),
    ("Load Custom EDID (.bin)...", "custom"),
]
EDID_PROFILE_KEYS = {key for _, key in EDID_PROFILES if key != "custom"}

# Playback layouts understood by gc573_user_streamer --audio-channels.
# "auto" follows the HDMI source; "stereo" applies the ITU-R BS.775 downmix
# for 5.1/7.1 sources instead of dropping everything but FL/FR.
AUDIO_CHANNEL_MODES = [
    ("Auto (HDMI Source)", "auto"),
    ("Stereo (BS.775 Downmix)", "stereo"),
    ("5.1 Surround", "5.1"),
    ("7.1 Surround", "7.1"),
]
AUDIO_CHANNEL_KEYS = {key for _, key in AUDIO_CHANNEL_MODES}

LED_COLORS = {
    0: "#334155",  # Aus (Dunkel)
    1: "#3b82f6",  # Blau (Standard)
    2: "#10b981",  # Grün
    3: "#ef4444",  # Rot
    4: "#06b6d4",  # Cyan / Türkis
    5: "#d946ef",  # Magenta / Lila
    6: "#eab308",  # Gelb
    7: "#f8fafc",  # Weiß
    8: "#a855f7",  # Rainbow Wave (Multi-Zone)
    9: "#06b6d4",  # Spectrum Cycle
}

# RGB values understood by everlive_userd --color.  Cyan matches the verified
# streamer/daemon colour (0, 204, 255) rather than the kernel classdev value.
LED_RGB_VALUES = {
    0: (0, 0, 0),
    1: (0, 0, 255),
    2: (0, 255, 0),
    3: (255, 0, 0),
    4: (0, 204, 255),
    5: (255, 0, 255),
    6: (255, 255, 0),
    7: (255, 255, 255),
    8: (168, 85, 247),
    9: (0, 204, 255),
}


def led_hex_for_mode(mode: int) -> str:
    """LED mode index -> "#rrggbb" understood by gc573_user_streamer."""
    red, green, blue = LED_RGB_VALUES.get(int(mode), (0, 204, 255))
    return f"#{red:02x}{green:02x}{blue:02x}"


def send_gc570d_ctl(*args: str) -> Optional[str]:
    """Send one command to a running GC570D daemon."""
    ctl = userd_bin("gc570d-ctl", MODEL_GC570D)
    if not os.access(ctl, os.X_OK):
        return None
    try:
        return subprocess.check_output(
            [ctl, *args], text=True, stderr=subprocess.DEVNULL, timeout=2.0)
    except Exception:
        return None


def set_gc570d_led(mode: int) -> bool:
    """Apply a GC570D LED effect through gc570d-ctl."""
    mode = int(mode)
    if mode == 0:
        effect = "off"
    elif mode == 8:
        effect = "colorful"
    elif mode == 9:
        effect = "breathing"
    else:
        effect = "static"
    if send_gc570d_ctl("led", effect) is None:
        return False
    save_settings({"led_mode": mode})
    return True


def set_vfio_led(mode: int) -> bool:
    """Apply an LED colour through everlive_userd (VFIO mode).

    everlive_userd owns BAR0 in standby.  A colour change therefore restarts
    the daemon with --color.  While the userspace streamer is running it owns
    BAR0 and drives the LED itself; in that case the choice is only persisted
    and restored after the streamer exits.
    """
    mode = int(mode)
    resolved = userd_bin("everlive_userd")
    if not (os.path.isfile(resolved) and os.access(resolved, os.X_OK)):
        return False

    # A streamer owns the card and drives the LED itself, whoever runs it.
    if _vfio_streamer_running(any_user=True):
        save_settings({"led_mode": mode})
        return True

    _stop_vfio_daemon()
    args = [resolved, "--daemon"]
    if mode == 8:
        args.append("--wave")
    elif mode == 9:
        args.append("--rainbow")
    elif mode == 0:
        args.append("--off")
    else:
        red, green, blue = LED_RGB_VALUES.get(mode, (0, 0, 0))
        args.extend(["--color", str(red), str(green), str(blue)])

    try:
        subprocess.Popen(args, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True)
    except Exception:
        return False

    time.sleep(0.35)
    if _proc_running("everlive_userd"):
        save_settings({"led_mode": mode})
        return True
    return False


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
    dot = 8
    offset = max(0, (size - dot) // 2)
    painter.drawEllipse(offset, offset, dot, dot)
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


def _is_everlive_audio_line(line: str) -> bool:
    """Match EverLive audio streams in wpctl/pactl output.

    The kernel path names its nodes after the capture source; the userspace
    VFIO engine plays through SDL2, so its sink-input is identified by the
    process binary instead.
    """
    low = line.lower()
    if ("gc573_user_streamer" in low or "opengc570d-user" in low or
            "hws_user_streamer" in low or
            "sdl application" in low):
        return True
    if "avermedia live gamer 4k pro" in low and "pro 1" not in low:
        return True
    return (any(s in low for s in ("everlive", "everlive-audio", "gc573-audio"))
            and "input." not in low)


def _multiview_stream_ids() -> set:
    """Sink-input ids owned by multiview engines.

    Their playback is muted / levelled per tile, so the Hub's own volume
    and mute helpers must leave them alone (a GC573 tile also calls its
    stream "EverLive").  pipewire-pulse uses the PipeWire object id as the
    sink-input index, so the ids also match wpctl's.
    """
    if not _MULTIVIEW_PIDS:
        return set()
    return {st["id"] for st in everlive_multi.list_sink_inputs()
            if st["pid"] in _MULTIVIEW_PIDS}


def _wpctl_everlive_stream_ids() -> list:
    """PipeWire stream ids for EverLive playback, via wpctl."""
    ids = []
    try:
        out = subprocess.check_output(["wpctl", "status"], text=True,
                                      stderr=subprocess.DEVNULL)
    except Exception:
        return ids

    in_audio = False
    for line in out.splitlines():
        line_s = line.strip()
        if line_s.startswith("Audio"):
            in_audio = True
        elif in_audio and (line_s.startswith("Video") or line_s.startswith("Settings")):
            in_audio = False
        if in_audio and _is_everlive_audio_line(line_s):
            for part in line_s.split():
                clean = part.strip(".│*")
                if clean.isdigit():
                    ids.append(clean)
                    break
    skip = _multiview_stream_ids()
    return [i for i in ids if i not in skip]


def _pactl_everlive_sink_ids() -> list:
    """PulseAudio/PipeWire-pulse sink-input ids for EverLive playback."""
    ids = []
    try:
        out = subprocess.check_output(["pactl", "list", "sink-inputs"],
                                      text=True, stderr=subprocess.DEVNULL)
    except Exception:
        return ids

    current = None
    matches = False

    def flush():
        if current is not None and matches:
            ids.append(current)

    for line in out.splitlines():
        line_s = line.strip()
        if line_s.startswith("Sink-Eingabe #") or line_s.startswith("Sink Input #"):
            flush()
            current = line_s.split("#")[-1].strip()
            matches = False
        elif current is not None and _is_everlive_audio_line(line_s):
            matches = True
    flush()
    skip = _multiview_stream_ids()
    return [i for i in ids if i not in skip]


def set_audio_stream_unity():
    """Sets the userspace SDL2 sink-input in PipeWire to 100% (unity gain).

    Userspace VFIO engines (gc573_user_streamer and gc570d-user) perform
    software volume scaling internally with a clickless ramp and hotkey
    controls. The host PipeWire stream must remain at unity (100%) so that
    volume is not double-attenuated.
    """
    try:
        for target in _wpctl_everlive_stream_ids():
            subprocess.run(["wpctl", "set-volume", target, "1.00"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass

    try:
        for target in _pactl_everlive_sink_ids():
            subprocess.run(["pactl", "set-sink-input-volume", target, "100%"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass


def set_audio_volume(vol_pct: int):
    """Sets EverLive stream volume via wpctl / pactl (0-150%).

    Covers both the kernel PipeWire stream and the SDL2 sink-input created by
    gc573_user_streamer in VFIO mode.
    """
    vol_float = vol_pct / 100.0
    try:
        for target in _wpctl_everlive_stream_ids():
            subprocess.run(["wpctl", "set-volume", target, f"{vol_float:.2f}"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass

    try:
        for target in _pactl_everlive_sink_ids():
            subprocess.run(["pactl", "set-sink-input-volume", target, f"{vol_pct}%"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass


def set_audio_mute(mute: bool):
    """Mutes or unmutes EverLive playback via wpctl / pactl."""
    mute_arg = "1" if mute else "0"
    try:
        for target in _wpctl_everlive_stream_ids():
            subprocess.run(["wpctl", "set-mute", target, mute_arg],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass

    try:
        for target in _pactl_everlive_sink_ids():
            subprocess.run(["pactl", "set-sink-input-mute", target, mute_arg],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass


def default_sink_issue():
    """Return a hint when the default output is muted or at 0%, else None."""
    try:
        out = subprocess.check_output(
            ["wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"],
            text=True, stderr=subprocess.DEVNULL).strip()
        vol_m = re.search(r"Volume:\s*([0-9.]+)", out)
        if vol_m:
            if "MUTED" in out.upper():
                return "default audio sink is muted"
            if float(vol_m.group(1)) <= 0.0:
                return "default audio sink is at 0%"
            return None
    except Exception:
        pass

    try:
        mute = subprocess.check_output(
            ["pactl", "get-sink-mute", "@DEFAULT_SINK@"],
            text=True, stderr=subprocess.DEVNULL)
        if "yes" in mute.lower():
            return "default audio sink is muted"
        vol = subprocess.check_output(
            ["pactl", "get-sink-volume", "@DEFAULT_SINK@"],
            text=True, stderr=subprocess.DEVNULL)
        vol_m = re.search(r"(\d+)%", vol)
        if vol_m and int(vol_m.group(1)) == 0:
            return "default audio sink is at 0%"
    except Exception:
        pass
    return None


class GameStreamer:
    """Manages low-latency video and audio child processes."""
    def __init__(self, vdev: Optional[str] = None, adev: Optional[str] = None, force_mpv: bool = False,
                 card: Optional[Dict[str, Optional[str]]] = None):
        # ``card`` comes from detect_capture_cards() when the Hub's card
        # selector switches cards; otherwise the usual selection applies.
        self.hw_info = dict(card) if card else detect_gc573_driver()
        self.mode = self.hw_info.get("mode", "unknown")
        self.model = self.hw_info.get("model", MODEL_GC573)
        self.duo_mode = self.model == MODEL_GC570D
        self.model_label = MODEL_NAMES.get(self.model, MODEL_NAMES[MODEL_GC573])
        self.vfio_mode = self.mode == "vfio"
        self.kernel_mode = self.mode == "kernel"
        self.uvc_mode = self.mode == "uvc"

        if self.vfio_mode:
            # VFIO intentionally has no V4L2 node and no ALSA card.  Never
            # probe them, or the GUI would report misleading warnings.
            self.vdev = vdev or ""
            self.adev = adev or ""
            self.pw_node = None
        else:
            self.vdev = vdev if vdev is not None else find_gc573_vdev()
            self.adev = adev if adev is not None else find_gc573_adev()
            self.pw_node = find_pipewire_source_node()

        self.force_mpv = bool(force_mpv) and not self.vfio_mode
        self.mpv_proc: Optional[subprocess.Popen] = None
        self.arecord_proc: Optional[subprocess.Popen] = None
        self.pwplay_proc: Optional[subprocess.Popen] = None
        self.is_running = False
        self.last_error: str = ""

        # Userspace VFIO engine status cache.  The streamer writes a
        # line-buffered log which is parsed below; the last signal is also
        # persisted so standby can show it without touching the hardware.
        if self.duo_mode:
            self.streamer_bin = userd_bin("opengc570d-userd", self.model)
            self.userd_bin = self.streamer_bin
            self.probe_bin = ""
            self.ctl_bin = userd_bin("gc570d-ctl", self.model)
        elif self.model == MODEL_HWSUHD:
            self.streamer_bin = userd_bin("hws_user_streamer", self.model)
            self.userd_bin = self.streamer_bin
            self.probe_bin = ""
            self.ctl_bin = ""
        else:
            self.streamer_bin = userd_bin("gc573_user_streamer", self.model)
            self.userd_bin = userd_bin("everlive_userd", self.model)
            self.probe_bin = userd_bin("gc573_user_probe", self.model)
            self.ctl_bin = ""
        self._vfio_log_path = os.path.join(CONFIG_DIR, "streamer.log")
        self._vfio_log_pos = 0
        self._vfio_live = False
        self._vfio_saw_output = False
        self._duo_no_signal = False
        self._stopped_duo_service = False
        self._pending_hub_actions = []
        # Last audio state parsed from the streamer log; shown in the capsule.
        self._vfio_audio = {"state": "unknown", "detail": "", "kind": ""}
        self._vfio_metrics = {
            "fps": 0.0,
            "frames": 0,
            "drops": 0,
            "latency_ms": 0.0,
            "out_w": 0,
            "out_h": 0,
            "pclk_khz": 0,
            "audio_underruns": 0,
            "audio_sdl_bytes": 0,
        }
        saved_aspect = load_settings().get("aspect_ratio", "auto")
        self.aspect = saved_aspect if saved_aspect in ASPECT_KEYS else "auto"
        self.headless = False
        self.v4l2_device: Optional[str] = None
        self.v4l2_device_2: Optional[str] = None
        self.duo_input = 0
        saved_signal = load_settings().get("last_signal")
        self._vfio_signal = saved_signal if isinstance(saved_signal, dict) else None

    def _update_vfio_signal(self, width: int, height: int, scan: str,
                            rate_hz: int, rate_frac: int,
                            chroma: Optional[str] = None):
        resolution = f"{width}x{height}{scan}"
        framerate = f"{rate_hz}.{rate_frac:02d} FPS"
        previous = self._vfio_signal or {}
        if chroma is None:
            chroma = previous.get("chroma", "")

        info = {
            "locked": True,
            "resolution": resolution,
            "framerate": framerate,
            "chroma": chroma,
            "hdr": previous.get("hdr", ""),
        }
        self._vfio_signal = info
        self._vfio_live = not getattr(self, "_duo_no_signal", False)

        if (previous.get("resolution") != resolution or
                previous.get("framerate") != framerate or
                previous.get("chroma") != chroma):
            save_settings({"last_signal": {
                "resolution": resolution,
                "framerate": framerate,
                "chroma": chroma,
                "hdr": info["hdr"],
            }})

    @staticmethod
    def _friendly_chroma(raw: str) -> str:
        raw = (raw or "").strip()
        return {
            "RGB": "RGB 4:4:4",
            "YCbCr 4:2:2": "YUV 4:2:2",
            "YCbCr 4:4:4": "YUV 4:4:4",
            "YCbCr 4:2:0": "YUV 4:2:0",
        }.get(raw, raw)

    def _parse_streamer_line(self, line: str):
        if not line:
            return
        self._vfio_saw_output = True

        if "[HUB] TOGGLE_FULLSCREEN" in line:
            self._pending_hub_actions.append("toggle_fullscreen")
            return
        if "[HUB] TOGGLE_RECORD" in line:
            self._pending_hub_actions.append("toggle_record")
            return
        if "[HUB] EXIT_FULLSCREEN" in line:
            self._pending_hub_actions.append("exit_fullscreen")
            return
        m_hub_fg = re.search(r"\[HUB\] FG_MULT (\d+)", line)
        if m_hub_fg:
            self._pending_hub_actions.append(("fg_mult", m_hub_fg.group(1)))
            return

        # [VIDEO] Capturing 2560x1440p @ 120.00 Hz (VIP 120 Hz, RGB, pclk 483000 kHz) -> ...
        m = re.search(
            r"\[VIDEO\] Capturing (\d+)x(\d+)([ip]) @ (\d+)\.(\d+) Hz "
            r"\([^,]+, ([^,]+), pclk\s*(\d+)?", line)
        if m:
            if m.group(7):
                self._vfio_metrics["pclk_khz"] = int(m.group(7))
            self._update_vfio_signal(
                int(m.group(1)), int(m.group(2)), m.group(3),
                int(m.group(4)), int(m.group(5)),
                self._friendly_chroma(m.group(6)))
            return

        # [STREAM] 2560x1440p @ 120.00 Hz -> 2560x1440 | FPS: 120.0 | Frames: ...
        m = re.search(
            r"\[STREAM\] (\d+)x(\d+)([ip]) @ (\d+)\.(\d+) Hz -> "
            r"(\d+)x(\d+) \| FPS: ([0-9.]+)", line)
        if m:
            fps = float(m.group(8))
            if fps > 0.5:
                self._duo_no_signal = False
            self._vfio_metrics["out_w"] = int(m.group(6))
            self._vfio_metrics["out_h"] = int(m.group(7))
            self._vfio_metrics["fps"] = fps
            m_fr = re.search(r"Frames:\s*(\d+)", line)
            if m_fr:
                self._vfio_metrics["frames"] = int(m_fr.group(1))
            m_dr = re.search(r"preview drops:\s*(\d+)", line)
            if m_dr:
                self._vfio_metrics["drops"] = int(m_dr.group(1))
            m_age = re.search(r"age\s*([0-9.]+)\s*ms", line)
            if m_age:
                self._vfio_metrics["latency_ms"] = float(m_age.group(1))
            self._update_vfio_signal(
                int(m.group(1)), int(m.group(2)), m.group(3),
                int(m.group(4)), int(m.group(5)))
            return

        # Audio diagnostics; keep the last state for the status capsule and
        # the Audio Format label.
        m = re.search(r"\[AUDIO\] Decoding (.+?) -> Stereo", line)
        if m:
            self._vfio_audio = {"state": "on",
                                "detail": f"{m.group(1).strip()} -> Stereo",
                                "kind": "bitstream"}
            return
        m = re.search(r"\[AUDIO\] Bitstream detected: (.+)", line)
        if m:
            # Streamer appends ", muted)" while it cannot decode the burst
            # (Phase 1 guard) and omits it once decoding is active.
            detail = m.group(1).strip()
            state = "detecting"
            if detail.endswith(", muted)"):
                detail = detail[:-len(", muted)")] + ") (Muted)"
                state = "muted"
            self._vfio_audio = {"state": state,
                                "detail": detail,
                                "kind": "bitstream"}
            return
        m = re.search(r"\[AUDIO\] (.+?) \(muted\)", line)
        if m:
            self._vfio_audio = {"state": "muted",
                                "detail": f"{m.group(1).strip()} (Muted)",
                                "kind": "bitstream"}
            return
        m = re.search(r"SDL audio: opened (\d+) Hz, (\d+) ch.*?, (\d+)-bit", line)
        if m:
            self._vfio_audio = {"state": "on",
                                "detail": f"{m.group(1)} Hz {m.group(2)} ch",
                                "kind": "lpcm"}
            return
        m = re.search(r"SDL audio: playing (\d+) Hz, 2 ch, (\d+)-bit via (\S+)", line)
        if m:
            self._vfio_audio = {"state": "on",
                                "detail": f"{m.group(1)} Hz 2 ch",
                                "kind": "lpcm"}
            return
        m = re.search(r"\[AUDIO\] (\d+) Hz (\d+) ch (\d+)-bit", line)
        if m:
            self._vfio_audio = {"state": "on",
                                "detail": f"{m.group(1)} Hz {m.group(2)} ch",
                                "kind": "lpcm"}
            self._vfio_metrics["audio_rate"] = int(m.group(1))
            self._vfio_metrics["audio_ch"] = int(m.group(2))
            self._vfio_metrics["audio_bits"] = int(m.group(3))
            m_ur = re.search(r"underruns\s*(\d+)", line)
            if m_ur:
                self._vfio_metrics["audio_underruns"] = int(m_ur.group(1))
            m_sdl = re.search(r"SDL\s*(\d+)\s*B", line)
            if m_sdl:
                self._vfio_metrics["audio_sdl_bytes"] = int(m_sdl.group(1))
            return
        if "SDL audio unavailable" in line or "dummy driver" in line:
            self._vfio_audio = {"state": "unavailable", "detail": "", "kind": ""}
            return
        if "receiver reports no playable stream" in line:
            self._vfio_audio = {"state": "absent", "detail": "", "kind": ""}
            return
        if "format not playable" in line:
            self._vfio_audio = {"state": "unsupported", "detail": "", "kind": ""}
            return
        if "HDMI audio DMA start failed" in line:
            self._vfio_audio = {"state": "error", "detail": "", "kind": ""}
            return

        if ("HDMI signal lost" in line or
                "Signal vanished" in line or
                "Waiting for HDMI signal" in line):
            self._vfio_live = False
            return
        if "EVERLIVE DUO" in line and "no signal" in line.lower():
            self._duo_no_signal = True
            self._vfio_live = False

    def _read_vfio_log(self):
        if not self._vfio_log_path:
            return
        try:
            with open(self._vfio_log_path, "r", errors="ignore") as f:
                f.seek(0, os.SEEK_END)
                size = f.tell()
                if self._vfio_log_pos > size:
                    self._vfio_log_pos = 0
                f.seek(self._vfio_log_pos)
                for line in f:
                    self._parse_streamer_line(line.strip())
                self._vfio_log_pos = f.tell()
        except OSError:
            pass

    def get_vfio_signal_info(self) -> Dict[str, object]:
        """Signal snapshot for the userspace engine, without V4L2/ALSA."""
        self._read_vfio_log()
        sig = self._vfio_signal or {}
        live = bool(self.is_running and self._vfio_live and sig.get("resolution"))
        has_cache = bool(sig.get("resolution") and
                         sig.get("resolution") != "No Signal")

        return {
            "locked": live,
            "resolution": sig.get("resolution", "No Signal"),
            "framerate": sig.get("framerate", ""),
            "chroma": sig.get("chroma", ""),
            "hdr": sig.get("hdr", ""),
            "cached": (not live) and has_cache,
            "starting": bool(self.is_running and not live and
                             not self._vfio_saw_output),
            "engine": "vfio",
        }

    def audio_state(self) -> Dict[str, str]:
        """Audio state parsed from the userspace streamer log."""
        return dict(self._vfio_audio)

    def audio_format_label(self) -> str:
        """Human-readable HDMI audio format for the hub status row."""
        state = self._vfio_audio.get("state", "unknown")
        detail = (self._vfio_audio.get("detail") or "").strip()
        kind = self._vfio_audio.get("kind", "")

        if state == "detecting" and detail:
            return f"{detail} (detecting)"
        if state in ("unknown", "absent", "unavailable", "unsupported", "error"):
            return "No Audio"
        if kind == "bitstream" or "-> Stereo" in detail or "Muted" in detail:
            return detail or "Bitstream"

        m = re.search(r"(\d+) Hz (\d+) ch", detail)
        if m:
            khz = int(m.group(1)) / 1000.0
            channels = int(m.group(2))
            channel_label = {
                1: "Mono",
                2: "Stereo",
                6: "5.1",
                8: "7.1",
            }.get(channels, f"{channels} ch")
            return f"L-PCM {khz:g}kHz ({channel_label})"
        return "No Audio"

    def get_signal_info(self) -> Dict[str, object]:
        """Queries current signal status via v4l2-ctl (kernel mode) or cache."""
        if self.vfio_mode:
            return self.get_vfio_signal_info()

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
        if self.vfio_mode:
            return (self._vfio_signal or {}).get("chroma", "")
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
        if self.vfio_mode:
            return (self._vfio_signal or {}).get("hdr", "")
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
        if self.vfio_mode:
            info = self.get_vfio_signal_info()
            if not info.get("locked"):
                return None, None
            return info.get("resolution"), info.get("framerate")
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
        """Forces hardware HDMI HPD re-sync (kernel mode only)."""
        if self.vfio_mode:
            # The userspace engine handles hot-switching automatically.
            return False
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

    def ctl(self, *args: str) -> Optional[str]:
        """Run gc570d-ctl and return its output (None when unavailable)."""
        if not self.ctl_bin or not os.access(self.ctl_bin, os.X_OK):
            return None
        try:
            return subprocess.check_output(
                [self.ctl_bin, *args], text=True,
                stderr=subprocess.DEVNULL, timeout=2.0)
        except Exception:
            return None

    def _duo_ipc(self):
        """Return the attached control client, or None when unavailable."""
        ipc = getattr(self, "ipc", None)
        if ipc is not None and getattr(ipc, "connected", False):
            return ipc
        return None

    def set_duo_input(self, port: int) -> bool:
        """Select HDMI 1 (0) or HDMI 2 (1) on the Live Gamer DUO."""
        port = 1 if int(port) else 0
        self.duo_input = port
        save_settings({"duo_input": port})
        if not self.duo_mode:
            return False
        if not self.is_running:
            return True
        ipc = self._duo_ipc()
        if ipc is not None:
            return ipc.request("input.set", {"port": port + 1})
        return self.ctl("input", str(port + 1)) is not None

    def apply_duo_led(self, mode: int) -> bool:
        """Map an EverLive LED mode to the GC570D effect interface."""
        mode = int(mode)
        if mode == 0:
            effect = "off"
        elif mode == 8:
            effect = "colorful"
        elif mode == 9:
            effect = "breathing"
        else:
            effect = "static"
        ipc = self._duo_ipc()
        if ipc is not None:
            return ipc.request("led.mode.set", {"mode": effect})
        return self.ctl("led", effect) is not None

    def duo_hotplug(self, port: int = 0) -> bool:
        """Pulse HPD on IT68051 (HDMI 1) or IT6802 (HDMI 2)."""
        node = "it68051_hpd" if int(port) == 0 else "it6802_hpd"
        ipc = self._duo_ipc()
        if ipc is not None:
            return ipc.request("hotplug", {"port": 1 if int(port) == 0 else 2})
        return self.ctl("action", node, "1") is not None

    def duo_edid_policy(self, bridge: bool) -> bool:
        """Select internal bridge EDID (True) or downstream copy (False)."""
        node = ("splitter_windows_bridge_edid" if bridge
                else "splitter_windows_edid_copy")
        ipc = self._duo_ipc()
        if ipc is not None:
            return ipc.request("edid.set",
                               {"profile": "default" if bridge else "copy"})
        return self.ctl("action", node, "1") is not None

    def _start_vfio_duo(self, fullscreen: bool, vsync: bool,
                        streamer_mode: bool, v4l2_device: Optional[str],
                        headless: bool, aspect: Optional[str], osd: bool,
                        osd_pos: str, scale: str,
                        volume: float,
                        parent_winid: Optional[int] = None) -> bool:
        """Launch the GC570D (Live Gamer DUO) userspace engine."""
        # The installed headless unit can outlive a previous session; stop it
        # before the ownership guard so the Hub can claim the card.
        if _stop_duo_service():
            self._stopped_duo_service = True
            print("[*] Stopped opengc570d-userd.service to start the Hub session.")

        if _vfio_streamer_running():
            print("[!] A userspace streamer is already running; refusing to start a second instance.")
            return False

        if not (os.path.isfile(self.streamer_bin) and
                os.access(self.streamer_bin, os.X_OK)):
            print(f"[!] Userspace streamer not found: {self.streamer_bin}")
            print("    Build it with: make -C tools/opengc570d-userd")
            return False

        _stop_vfio_daemon()
        _raise_memlock_limit()

        cmd = [self.streamer_bin]
        if self.hw_info.get("pci"):
            cmd.append(self.hw_info["pci"])

        embed_wid = int(parent_winid) if (parent_winid and not headless) else 0

        if headless:
            cmd.append("--headless")
            # Studio/streamer mode publishes a clean OBS feed without OSD.
            cmd.append("--no-osd" if streamer_mode or not osd else "--osd")
        else:
            cmd.append("--sdl")
            if fullscreen and not embed_wid:
                cmd.append("--fullscreen")
            cmd.append("--vsync" if vsync else "--no-vsync")
            if streamer_mode:
                cmd.append("--streamer-mode")
            else:
                cmd.append("--osd" if osd else "--no-osd")
        osd_pos = normalize_osd_position(osd_pos)
        cmd.extend(["--osd-pos", osd_pos])

        if aspect is None:
            aspect = getattr(self, "aspect", "auto")
        if aspect not in ASPECT_KEYS:
            aspect = "auto"
        self.aspect = aspect
        if aspect != "auto":
            cmd.extend(["--aspect", aspect])

        if scale not in SCALE_MODES:
            scale = "fit"
        cmd.extend(["--scale", scale])
        cmd.extend(["--volume", f"{volume:.2f}"])

        duo_input = load_settings().get("duo_input", 0)
        self.duo_input = 1 if duo_input == 1 else 0
        cmd.extend(["--port", str(self.duo_input + 1)])

        if not load_settings().get("duo_edid_bridge", True):
            cmd.append("--copy-edid")

        self.v4l2_device = v4l2_device
        self.v4l2_device_2 = None
        if v4l2_device:
            cmd.extend(["--v4l2", v4l2_device])
            if os.path.exists(V4L2_DUO_SECOND_DEVICE):
                self.v4l2_device_2 = V4L2_DUO_SECOND_DEVICE
                cmd.extend(["--v4l2-2", V4L2_DUO_SECOND_DEVICE])

        self.headless = bool(headless)
        try:
            os.makedirs(CONFIG_DIR, exist_ok=True)
            log = open(self._vfio_log_path, "wb")
        except OSError:
            log = subprocess.DEVNULL

        stdbuf = shutil.which("stdbuf")
        if stdbuf:
            cmd = [stdbuf, "-oL", "-eL"] + cmd

        env = os.environ.copy()
        if embed_wid > 0:
            env["EVERLIVE_PARENT_WINID"] = str(embed_wid)
            env["SDL_VIDEODRIVER"] = "x11"
        else:
            env.pop("EVERLIVE_PARENT_WINID", None)
            env.pop("SDL_VIDEODRIVER", None)
            env.pop("SDL_VIDEODRIVER", None)

        try:
            self.mpv_proc = subprocess.Popen(
                cmd, stdout=log, stderr=subprocess.STDOUT,
                env=env, start_new_session=True)
        except Exception as e:
            print(f"[!] Failed to launch userspace streamer: {e}")
            self.headless = False
            self.v4l2_device = None
            self.v4l2_device_2 = None
            if log is not subprocess.DEVNULL:
                log.close()
            return False
        finally:
            if log is not subprocess.DEVNULL and not log.closed:
                log.close()

        self._vfio_log_pos = 0
        self._vfio_live = False
        self._vfio_saw_output = False
        self._duo_no_signal = False
        self._pending_hub_actions = []
        self._vfio_audio = {"state": "unknown", "detail": "", "kind": ""}
        self._vfio_metrics = {
            "fps": 0.0,
            "frames": 0,
            "drops": 0,
            "latency_ms": 0.0,
            "out_w": 0,
            "out_h": 0,
            "pclk_khz": 0,
            "audio_underruns": 0,
            "audio_sdl_bytes": 0,
        }
        self.is_running = True
        print(f"[*] Starting Userspace VFIO engine (DUO): {' '.join(cmd)}")

        time.sleep(0.25)
        if self.mpv_proc.poll() is not None:
            self._read_vfio_log()
            self.is_running = False
            self.mpv_proc = None
            self.headless = False
            self.v4l2_device = None
            self.v4l2_device_2 = None
            print(f"[!] Userspace streamer exited at startup; see {self._vfio_log_path}")
            return False

        # Apply the saved LED effect once the daemon owns BAR0.
        mode = load_settings().get("led_mode", 4)
        if isinstance(mode, int):
            self.apply_duo_led(mode)
        return True

    def _start_vfio(self, fullscreen: bool = False, vsync: bool = False,
                    streamer_mode: bool = False,
                    v4l2_device: Optional[str] = None,
                    headless: bool = False,
                    aspect: Optional[str] = None,
                    osd: bool = True,
                    osd_pos: str = "top",
                    audio_tap: Optional[str] = None,
                    hdr: bool = True,
                    hdr_exposure: float = 0.0,
                    scale: str = "fit",
                    volume: float = 1.0,
                    framegen: str = "off",
                    fg_multiplier: int = 2,
                    fg_backend: str = "lsfg",
                    fg_dedup: bool = False,
                    fg_flow_scale: float = 1.0,
                    fg_perf_mode: bool = False,
                    fg_low_latency: bool = True,
                    parent_winid: Optional[int] = None) -> bool:
        """Launch the blob-free userspace VFIO streamer directly."""
        if self.duo_mode:
            return self._start_vfio_duo(
                fullscreen=fullscreen, vsync=vsync,
                streamer_mode=streamer_mode,
                v4l2_device=v4l2_device, headless=headless,
                aspect=aspect, osd=osd, osd_pos=osd_pos,
                scale=scale, volume=volume,
                parent_winid=parent_winid)

        if _vfio_streamer_running() or (
                self.model == MODEL_HWSUHD and _proc_running("hws_user_stream")):
            print("[!] A userspace streamer is already running; refusing to start a second instance.")
            return False

        if not (os.path.isfile(self.streamer_bin) and
                os.access(self.streamer_bin, os.X_OK)):
            print(f"[!] Userspace streamer not found: {self.streamer_bin}")
            print("    Build it with: make -C tools/"
                  + os.path.basename(userd_dir(self.model)))
            return False

        # everlive_userd holds the GC573's BAR0/VFIO in standby; release it
        # first.  The HWS UHD card has no standby daemon, and another card's
        # daemon is left alone.
        if self.model != MODEL_HWSUHD:
            _stop_vfio_daemon()
        _raise_memlock_limit()

        cmd = [self.streamer_bin]
        if self.hw_info.get("pci"):
            cmd.append(self.hw_info["pci"])

        # Keep the colour selected in the GUI while gaming instead of the
        # streamer's built-in cyan default.
        led_mode = load_settings().get("led_mode", 4)
        if led_mode == 8:
            cmd.extend(["--led", "wave"])
        elif led_mode == 9:
            cmd.extend(["--led", "rainbow"])
        elif isinstance(led_mode, int) and 0 <= led_mode <= 7:
            cmd.extend(["--led", led_hex_for_mode(led_mode)])
        else:
            cmd.extend(["--led", "#00ccff"])

        # Hardware EDID profile advertised to the console.
        edid_profile = load_settings().get("edid_profile", "default")
        edid_custom = load_settings().get("edid_custom_path", "")
        if edid_profile == "custom":
            if edid_custom and os.path.isfile(edid_custom):
                cmd.extend(["--edid", edid_custom])
            else:
                print("[!] Custom EDID file is missing; using the default profile.")
                cmd.extend(["--edid", "default"])
        elif edid_profile in EDID_PROFILE_KEYS:
            cmd.extend(["--edid", edid_profile])
        else:
            cmd.extend(["--edid", "default"])

        embed_wid = int(parent_winid) if (parent_winid and not headless) else 0

        # Presentation options only matter for the interactive SDL2 window.
        if not headless:
            if embed_wid > 0:
                cmd.extend(["--parent-winid", str(embed_wid)])
            elif fullscreen:
                cmd.append("--fullscreen")
            cmd.append("--vsync" if vsync else "--no-vsync")
            if streamer_mode:
                cmd.append("--streamer-mode")
            else:
                cmd.append("--osd" if osd else "--no-osd")
        osd_pos = normalize_osd_position(osd_pos)
        cmd.extend(["--osd-pos", osd_pos])

        if aspect is None:
            aspect = getattr(self, "aspect", "auto")
        if aspect not in ASPECT_KEYS:
            aspect = "auto"
        self.aspect = aspect
        if aspect != "auto":
            cmd.extend(["--aspect", aspect])

        if scale not in SCALE_MODES:
            scale = "fit"
        cmd.extend(["--scale", scale])
        cmd.extend(["--volume", f"{volume:.2f}"])

        # Playback channel layout.  "auto" follows the HDMI source; the other
        # modes force a layout and fall back to the BS.775 stereo downmix when
        # the sink refuses it.  Live changes go through audio.channels.set.
        audio_channels = load_settings().get("audio_channels", "auto")
        if audio_channels not in AUDIO_CHANNEL_KEYS:
            audio_channels = "auto"
        cmd.extend(["--audio-channels", audio_channels])

        # lsfg-vk frame generation.  Passed in headless mode too: the engine
        # keeps LSFG idle while the window is closed and activates Vulkan +
        # LSFG as soon as mode.set("gaming") opens it, so a Studio -> Gaming
        # switch needs no restart.  The V4L2 loopback keeps receiving the 
        # raw capture path independently.  The GC570D engine has no 
        # LSFG path; those flags stay on this GC573 command line only.
        if framegen not in FRAMEGEN_KEYS:
            framegen = FRAMEGEN_ALIASES.get(str(framegen).lower(), "off")
        if framegen in FRAMEGEN_KEYS and framegen != "off":
            mult = max(2, min(4, int(framegen[0]))) if framegen[0].isdigit() else max(2, min(4, int(fg_multiplier)))
            cli_fg = "30to60" if fg_dedup else "60to120"
            cmd.extend(["--framegen", cli_fg, "--fg-mult", str(mult)])
            if fg_dedup:
                cmd.append("--fg-dedup")
        # Flow/perf/latency/backend are passed even in mode OFF so a later live toggle
        # (hotkey or IPC) starts from the Hub's saved values, not defaults.
        cmd.extend(["--fg-backend",
                    "native" if str(fg_backend).lower() == "native" else "lsfg"])
        cmd.extend(["--fg-flow-scale",
                    f"{max(0.5, min(1.0, float(fg_flow_scale))):.2f}"])
        cmd.append("--fg-perf" if fg_perf_mode else "--no-fg-perf")
        cmd.append("--fg-low-latency" if fg_low_latency else "--no-fg-low-latency")

        if v4l2_device:
            cmd.extend(["--v4l2", v4l2_device])
        if audio_tap:
            cmd.extend(["--audio-tap", audio_tap])
        if not hdr:
            cmd.append("--no-hdr")
        elif abs(hdr_exposure) >= 0.05:
            cmd.extend(["--hdr-exposure", f"{hdr_exposure:.1f}"])
        if headless:
            cmd.append("--headless")

        self.headless = bool(headless)
        self.v4l2_device = v4l2_device
        self.v4l2_device_2 = None

        try:
            os.makedirs(CONFIG_DIR, exist_ok=True)
            log = open(self._vfio_log_path, "wb")
        except OSError:
            log = subprocess.DEVNULL

        # The streamer's stdout carries the current signal; keep it line
        # buffered so standby/live status can be parsed without V4L2.
        stdbuf = shutil.which("stdbuf")
        if stdbuf:
            cmd = [stdbuf, "-oL", "-eL"] + cmd

        env = os.environ.copy()
        if embed_wid > 0:
            env["EVERLIVE_PARENT_WINID"] = str(embed_wid)
            env["SDL_VIDEODRIVER"] = "x11"
        else:
            env.pop("EVERLIVE_PARENT_WINID", None)
            env.pop("SDL_VIDEODRIVER", None)
            env.pop("SDL_VIDEODRIVER", None)

        try:
            self.mpv_proc = subprocess.Popen(
                cmd, stdout=log, stderr=subprocess.STDOUT,
                env=env, start_new_session=True)
        except Exception as e:
            print(f"[!] Failed to launch userspace streamer: {e}")
            self.headless = False
            self.v4l2_device = None
            if log is not subprocess.DEVNULL:
                log.close()
            return False
        finally:
            if log is not subprocess.DEVNULL and not log.closed:
                log.close()

        self._vfio_log_pos = 0
        self._vfio_live = False
        self._vfio_saw_output = False
        self._duo_no_signal = False
        self._pending_hub_actions = []
        self._vfio_audio = {"state": "unknown", "detail": "", "kind": ""}
        self._vfio_metrics = {
            "fps": 0.0,
            "frames": 0,
            "drops": 0,
            "latency_ms": 0.0,
            "out_w": 0,
            "out_h": 0,
            "pclk_khz": 0,
            "audio_underruns": 0,
            "audio_sdl_bytes": 0,
        }
        self.is_running = True
        print(f"[*] Starting Userspace VFIO engine: {' '.join(cmd)}")

        # Catch immediate failures (missing VFIO permission, unbound card).
        time.sleep(0.25)
        if self.mpv_proc.poll() is not None:
            self._read_vfio_log()
            self.is_running = False
            self.mpv_proc = None
            self.headless = False
            self.v4l2_device = None
            print(f"[!] Userspace streamer exited at startup; see {self._vfio_log_path}")
            return False
        return True

    def start(self, preset_name: str = "Auto (HDMI Signal)", fullscreen: bool = False, volume: float = 1.0, vsync: bool = False, streamer_mode: bool = False, v4l2_device: Optional[str] = None, headless: bool = False, aspect: Optional[str] = None, osd: bool = True, osd_pos: str = "top", audio_tap: Optional[str] = None, hdr: bool = True, hdr_exposure: float = 0.0, scale: str = "fit", framegen: str = "off", fg_multiplier: int = 2, fg_backend: str = "lsfg", fg_dedup: bool = False, fg_flow_scale: float = 1.0, fg_perf_mode: bool = False, fg_low_latency: bool = True, parent_winid: Optional[int] = None) -> bool:
        if self.is_running:
            return True

        if self.vfio_mode:
            if preset_name != "Auto (HDMI Signal)":
                print("[*] VFIO mode: the input mode comes from the HDMI "
                      "source and the selected EDID profile.")
            return self._start_vfio(fullscreen=fullscreen, vsync=vsync,
                                    streamer_mode=streamer_mode,
                                    v4l2_device=v4l2_device,
                                    headless=headless,
                                    aspect=aspect,
                                    osd=osd,
                                    osd_pos=osd_pos,
                                    audio_tap=audio_tap,
                                    hdr=hdr,
                                    hdr_exposure=hdr_exposure,
                                    scale=scale,
                                    volume=volume,
                                    framegen=framegen,
                                    fg_multiplier=fg_multiplier,
                                    fg_backend=fg_backend,
                                    fg_dedup=fg_dedup,
                                    fg_flow_scale=fg_flow_scale,
                                    fg_perf_mode=fg_perf_mode,
                                    fg_low_latency=fg_low_latency,
                                    parent_winid=parent_winid)

        if headless:
            print("[!] Headless/Studio mode requires the userspace VFIO engine.")
            return False
        if v4l2_device:
            print("[*] Note: --v4l2 is only used by the userspace VFIO engine.")

        self.last_error = ""
        self.vsync = vsync
        if not self.vdev or not os.path.exists(self.vdev):
            self.last_error = f"Video device {self.vdev or '(none)'} not found"
            print(f"[!] Error: Video device {self.vdev} does not exist.")
            return False

        preset = PRESETS.get(preset_name)
        width, height, fps = None, None, None

        if preset:
            width = preset["width"]
            height = preset["height"]
            fps = preset["fps"]
            fmt = "MJPG" if (self.uvc_mode and fps >= 100) else preset["fmt"]
            # Apply fixed preset to V4L2 device
            v4l2_args = [
                "v4l2-ctl", "-d", self.vdev,
                f"--set-fmt-video=width={width},height={height},pixelformat={fmt}",
            ]
            if self.uvc_mode and fps:
                v4l2_args.append(f"--set-parm={fps}")
            try:
                subprocess.run(
                    v4l2_args,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1.0
                )
            except Exception as e:
                print(f"[*] Note while setting format: {e}")
        else:
            # Auto (HDMI Signal): Synchronous v4l2 query only needed for mpv fallback
            # on legacy kernel/PCIe devices (UVC devices have no DV timings).
            if not self.uvc_mode:
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
        # on PCIe devices; UVC/GC553G2 always uses mpv so MJPEG 120/144 FPS decodes.
        script_dir = os.path.dirname(os.path.abspath(__file__))
        native_bin = os.path.join(script_dir, "everlive_display")
        if not os.path.isfile(native_bin):
            native_bin = os.path.join(script_dir, "gc573_display")

        if (not self.uvc_mode and not self.force_mpv
                and os.path.isfile(native_bin) and os.access(native_bin, os.X_OK)):
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
        mpv_title = (
            "EverLive - Live Gamer ULTRA 2.1"
            if self.uvc_mode
            else "EverLive - Live Gamer 4K"
        )
        mpv_cmd = [
            "mpv",
            f"av://v4l2:{self.vdev}",
            f"--title={mpv_title}",
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
            "--video-sync=display-resync" if vsync else "--video-sync=display-desync",
            "--opengl-glfinish=no",
            "--vd-lavc-threads=1",
            "--vd-lavc-fast=yes",
            "--vo=gpu",
            "--cursor-autohide=1000",
            "--no-terminal",
            "--input-default-bindings=yes",
        ]
        if self.uvc_mode and preset:
            uvc_fmt = "mjpeg" if fps and fps >= 100 else "yuyv422"
            mpv_cmd.append(f"--demuxer-lavf-o-add=input_format={uvc_fmt}")
            mpv_cmd.append(f"--demuxer-lavf-o-add=video_size={width}x{height}")
            mpv_cmd.append(f"--demuxer-lavf-o-add=framerate={fps}")

        if fullscreen:
            mpv_cmd.append("--fullscreen")
        if streamer_mode:
            mpv_cmd.extend(["--no-osc", "--osd-level=0", "--cursor-autohide=always"])

        # Automatic HDR10 / HLG metadata bridging for mpv:
        # Since FFmpeg's libavdevice/v4l2 demuxer does not propagate V4L2 colorimetry,
        # detect HDR directly from V4L2 metadata and inject the exact tone-mapping pipeline.
        hdr_type = self.hdr_label()
        if hdr_type == "HDR10":
            mpv_cmd.append("--vf=setparams=range=limited:color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc")
            print("[*] HDR10 (PQ BT.2020) signal detected: Auto-configured mpv GPU tone-mapping (zero CPU delay)")
        elif hdr_type == "HLG":
            mpv_cmd.append("--vf=setparams=range=limited:color_primaries=bt2020:color_trc=arib-std-b67:colorspace=bt2020nc")
            print("[*] HLG (BT.2020) signal detected: Auto-configured mpv HLG GPU tone-mapping (zero CPU delay)")

        # Anamorphic 3840x1080 scaling:
        # 3840x1080 is an anamorphic half-vertical 4K120 signal (e.g. RetroTINK 4K Pro)
        # that requires 2x vertical stretching to restore the intended 16:9 aspect ratio (3840x2160).
        active_fmt, _ = self.get_active_format()
        if active_fmt and "3840x1080" in active_fmt:
            mpv_cmd.append("--video-aspect-override=16:9")
            print("[*] Anamorphic 3840x1080 signal detected: Auto-configured 16:9 aspect ratio (2x vertical stretch) in mpv")

        if self.uvc_mode and not shutil.which("mpv"):
            self.last_error = (
                "mpv is required for GC553G2 USB/UVC playback — "
                "install mpv via your package manager."
            )
            print(f"[!] {self.last_error}")
            return False

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
                    audio_env = dict(os.environ)
                    audio_env["PIPEWIRE_PROPS"] = (
                        'application.name="EverLive" application.icon-name="everlive" application.id="everlive"'
                    )
                    if self.pw_node:
                        # Native PipeWire loopback with 60ms safe buffer matching everlive_display
                        self.pwplay_proc = subprocess.Popen(
                            [
                                "pw-loopback",
                                "-i", f'{{ target.object = "{self.pw_node}" resample.disable = false }}',
                                "-l", "60",
                                "-o", f"{{ node.virtual = false resample.disable = false resample.quality = 4 {props[1:-1]} }}",
                            ],
                            env=audio_env,
                            stderr=subprocess.DEVNULL,
                            start_new_session=True,
                        )
                    else:
                        plug_adev = f"plughw:{self.adev[3:]}" if self.adev.startswith("hw:") else self.adev
                        arecord_cmd = [
                            "arecord", "-D", self.adev, "-t", "wav", "-f", "S16_LE",
                            "-c", "2", "-r", "48000", "--period-time=20000",
                            "--buffer-time=80000", "-q",
                        ]
                        arecord_proc = subprocess.Popen(
                            arecord_cmd,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL,
                            start_new_session=True,
                        )
                        if arecord_proc.poll() is not None:
                            arecord_cmd[2] = plug_adev
                            arecord_proc = subprocess.Popen(
                                arecord_cmd,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL,
                                start_new_session=True,
                            )
                        self.pwplay_proc = subprocess.Popen(
                            ["pw-play", "--latency=40ms", "-P", props, "-"],
                            stdin=arecord_proc.stdout,
                            env=audio_env,
                            stderr=subprocess.DEVNULL,
                            start_new_session=True,
                        )
                        if arecord_proc.stdout:
                            arecord_proc.stdout.close()
                        self.arecord_proc = arecord_proc
                except Exception as ae:
                    print(f"[*] Note while starting audio stream: {ae}")
            return True
        except Exception as e:
            self.last_error = f"Failed to launch mpv: {e}"
            print(f"[!] {self.last_error}")
            self.stop()
            return False

    def stop(self):
        self.is_running = False
        if self.vfio_mode:
            self._read_vfio_log()
            self._vfio_live = False
            self._duo_no_signal = False
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
                # The userspace streamer performs the LED handoff and DMA
                # teardown on SIGTERM; give it a little more time.
                self.mpv_proc.wait(timeout=5.0 if self.vfio_mode else 1.0)
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

        self.headless = False
        self.v4l2_device = None
        self.v4l2_device_2 = None

    def poll(self) -> bool:
        """Returns True while the stream process is still alive."""
        if self.mpv_proc:
            if self.vfio_mode:
                self._read_vfio_log()
            if self.mpv_proc.poll() is not None:
                if self.vfio_mode:
                    self._read_vfio_log()
                self.stop()
                return False
            return True
        return False


if HAS_QT:
    class VeyraPerfOrb(QWidget):
        """Veyra-style circular ring gauge (ProPerfOrb) with neon glow."""
        def __init__(self, label: str, size: int = 50,
                     custom_color: Optional[str] = None, parent=None):
            super().__init__(parent)
            self.orb_label = label
            self.orb_size = size
            self.custom_color = custom_color
            self.fraction = 0.0
            self.known = False
            self.value_text = "—"
            self.setFixedSize(max(size + 6, 56), size + 18)

        def set_state(self, fraction: float, value_text: str,
                      known: bool = True, tip: str = ""):
            self.fraction = max(0.0, min(1.0, float(fraction)))
            self.value_text = value_text
            self.known = bool(known)
            if tip:
                self.setToolTip(tip)
            self.update()

        def _tone_color(self) -> QColor:
            if not self.known:
                return QColor(255, 255, 255, 55)
            if self.custom_color and self.fraction < 0.72:
                return QColor(self.custom_color)
            if self.fraction < 0.70:
                return QColor("#5ED4B2")
            if self.fraction < 0.92:
                return QColor("#F5C84B")
            return QColor("#FF5D5D")

        def paintEvent(self, _event):
            p = QPainter(self)
            p.setRenderHint(QPainter.Antialiasing, True)
            s = float(self.orb_size)
            cx = self.width() / 2.0
            rx = cx - s / 2.0 + 4.0
            ry = 3.0
            rw = s - 8.0
            rect = QRectF(rx, ry, rw, rw)

            # Background track
            track_pen = QPen(QColor(255, 255, 255, 22), 4.2)
            track_pen.setCapStyle(Qt.RoundCap)
            p.setPen(track_pen)
            p.drawEllipse(rect)

            tone = self._tone_color()
            if self.known and self.fraction > 0.008:
                span_deg = -int(round(self.fraction * 360.0 * 16.0))
                # Soft neon glow halo
                glow_col = QColor(tone)
                glow_col.setAlpha(48)
                glow_pen = QPen(glow_col, 8.5)
                glow_pen.setCapStyle(Qt.RoundCap)
                p.setPen(glow_pen)
                p.drawArc(rect, 90 * 16, span_deg)

                # Crisp main arc
                arc_pen = QPen(tone, 4.2)
                arc_pen.setCapStyle(Qt.RoundCap)
                p.setPen(arc_pen)
                p.drawArc(rect, 90 * 16, span_deg)

            # Center value
            val_font = QFont(ui_theme.MONO_FAMILY, 8)
            val_font.setPixelSize(12 if len(self.value_text) > 4 else 13)
            val_font.setBold(True)
            p.setFont(val_font)
            p.setPen(QColor("#F7F4FC") if self.known else QColor("#6E6882"))
            p.drawText(rect, Qt.AlignCenter, self.value_text)

            # Label underneath
            lbl_font = QFont(self.font())
            lbl_font.setPixelSize(12)
            lbl_font.setWeight(QFont.Medium)
            p.setFont(lbl_font)
            p.setPen(QColor("#9E97B0"))
            lbl_rect = QRectF(0, s + 1.0, self.width(), 16.0)
            p.drawText(lbl_rect, Qt.AlignHCenter | Qt.AlignTop, self.orb_label)


    class VeyraMetricBar(QWidget):
        """Veyra-style stage/resource row with gradient pill bar and monospace value."""
        def __init__(self, label: str, c_start: str, c_end: str, parent=None):
            super().__init__(parent)
            self.label_text = label
            self.c_start = QColor(c_start)
            self.c_end = QColor(c_end)
            self.fraction = 0.0
            self.value_text = "—"
            self.known = False
            self.setFixedHeight(24)

        def set_metric(self, fraction: float, value_text: str,
                       known: bool = True, tip: str = ""):
            self.fraction = max(0.0, min(1.0, float(fraction)))
            self.value_text = value_text
            self.known = bool(known)
            if tip:
                self.setToolTip(tip)
            self.update()

        def paintEvent(self, _event):
            p = QPainter(self)
            p.setRenderHint(QPainter.Antialiasing, True)
            w = float(self.width())
            h = float(self.height())
            lbl_w = 80.0
            val_w = 132.0

            # Left label
            lbl_font = QFont(self.font())
            lbl_font.setPixelSize(13)
            p.setFont(lbl_font)
            p.setPen(QColor("#ACA5C0"))
            p.drawText(QRectF(0, 0, lbl_w, h),
                       Qt.AlignLeft | Qt.AlignVCenter, self.label_text)

            # Middle progress bar
            bar_x = lbl_w + 6.0
            bar_w = max(24.0, w - bar_x - val_w - 6.0)
            bar_h = 6.0
            bar_y = (h - bar_h) / 2.0
            track_rect = QRectF(bar_x, bar_y, bar_w, bar_h)

            p.setPen(Qt.NoPen)
            p.setBrush(QColor(255, 255, 255, 20))
            p.drawRoundedRect(track_rect, 3.0, 3.0)

            if self.known and self.fraction > 0.005:
                fill_w = max(5.0, bar_w * self.fraction)
                fill_rect = QRectF(bar_x, bar_y, fill_w, bar_h)
                grad = QLinearGradient(bar_x, 0, bar_x + bar_w, 0)
                if self.fraction >= 0.92:
                    grad.setColorAt(0.0, QColor("#F5C84B"))
                    grad.setColorAt(1.0, QColor("#FF5D5D"))
                else:
                    grad.setColorAt(0.0, self.c_start)
                    grad.setColorAt(1.0, self.c_end)
                p.setBrush(grad)
                p.drawRoundedRect(fill_rect, 3.0, 3.0)

            # Right monospace value
            val_font = QFont(ui_theme.MONO_FAMILY, 8)
            val_font.setPixelSize(13)
            val_font.setBold(True)
            p.setFont(val_font)
            p.setPen(QColor("#F7F4FC") if self.known else QColor("#6E6882"))
            p.drawText(QRectF(w - val_w, 0, val_w, h),
                       Qt.AlignRight | Qt.AlignVCenter, self.value_text)


    class VeyraSparkline(QWidget):
        """Veyra-style live FPS/cadence sparkline with gradient fill under curve."""
        def __init__(self, parent=None):
            super().__init__(parent)
            self.points = []
            self.target_fps = 60.0
            self.accent_hex = "#5ED4B2"
            self.setFixedHeight(28)

        def push_sample(self, val: float, target: float = 60.0,
                        accent_hex: str = "#5ED4B2"):
            self.target_fps = max(1.0, float(target))
            self.accent_hex = accent_hex
            self.points.append(max(0.0, float(val)))
            if len(self.points) > 48:
                self.points = self.points[-48:]
            self.update()

        def clear_samples(self):
            if self.points:
                self.points.clear()
                self.update()

        def paintEvent(self, _event):
            p = QPainter(self)
            p.setRenderHint(QPainter.Antialiasing, True)
            w = float(self.width())
            h = float(self.height())
            if w < 10 or h < 6:
                return

            # Baseline dashed line
            dash_pen = QPen(QColor(255, 255, 255, 28), 1.0, Qt.DashLine)
            p.setPen(dash_pen)
            p.drawLine(QPointF(0, h * 0.35), QPointF(w, h * 0.35))

            pts = self.points
            if len(pts) < 2:
                return

            lo = min(min(pts), self.target_fps * 0.8)
            hi = max(max(pts), self.target_fps * 1.08)
            span = max(2.0, hi - lo)
            lo -= span * 0.12
            hi += span * 0.12

            def _xy(idx: int, val: float) -> QPointF:
                x = (idx / 47.0) * w + ((48 - len(pts)) / 47.0) * w
                y = h - 2.0 - ((val - lo) / (hi - lo)) * (h - 5.0)
                return QPointF(x, max(2.0, min(h - 2.0, y)))

            line_path = QPainterPath()
            first_pt = _xy(0, pts[0])
            line_path.moveTo(first_pt)
            for i in range(1, len(pts)):
                line_path.lineTo(_xy(i, pts[i]))

            fill_path = QPainterPath(line_path)
            last_pt = _xy(len(pts) - 1, pts[-1])
            fill_path.lineTo(QPointF(last_pt.x(), h))
            fill_path.lineTo(QPointF(first_pt.x(), h))
            fill_path.closeSubpath()

            col = QColor(self.accent_hex)
            grad = QLinearGradient(0, 0, 0, h)
            c_top = QColor(col)
            c_top.setAlpha(68)
            c_bot = QColor(col)
            c_bot.setAlpha(0)
            grad.setColorAt(0.0, c_top)
            grad.setColorAt(1.0, c_bot)
            p.fillPath(fill_path, QBrush(grad))

            curve_pen = QPen(col, 1.6)
            curve_pen.setCapStyle(Qt.RoundCap)
            p.setPen(curve_pen)
            p.drawPath(line_path)


    class VeyraStatusLight(QWidget):
        """Veyra ProStatusLight: glowing colored bar + right-aligned status summary."""
        def __init__(self, parent=None):
            super().__init__(parent)
            self.level = "idle"
            self.text = "Standby · Ready"
            self.setFixedHeight(22)

        def set_status(self, level: str, text: str):
            if self.level != level or self.text != text:
                self.level = level
                self.text = text
                self.setToolTip(text)
                self.update()

        def paintEvent(self, _event):
            p = QPainter(self)
            p.setRenderHint(QPainter.Antialiasing, True)
            w = float(self.width())
            h = float(self.height())
            if self.level == "ok":
                tone = QColor("#5ED4B2")
            elif self.level == "accent":
                tone = QColor("#F287A7")
            elif self.level == "warn":
                tone = QColor("#F5C84B")
            elif self.level == "err":
                tone = QColor("#FF5D5D")
            else:
                tone = QColor(255, 255, 255, 75)

            font = QFont(self.font())
            font.setPixelSize(13)
            font.setWeight(QFont.Medium)
            p.setFont(font)
            fm = p.fontMetrics()
            max_txt_w = int(w * 0.74)
            elided = fm.elidedText(self.text, Qt.ElideRight, max_txt_w)
            txt_w = fm.horizontalAdvance(elided)

            bar_w = max(24.0, w - txt_w - 12.0)
            bar_y = (h - 4.0) / 2.0
            bar_rect = QRectF(0, bar_y, bar_w, 4.0)

            p.setPen(Qt.NoPen)
            if self.level != "idle":
                glow = QColor(tone)
                glow.setAlpha(45)
                p.setBrush(glow)
                p.drawRoundedRect(QRectF(-1.5, bar_y - 2.0, bar_w + 3.0, 8.0), 4.0, 4.0)

            bar_col = QColor(tone)
            if self.level == "idle":
                bar_col.setAlpha(90)
            p.setBrush(bar_col)
            p.drawRoundedRect(bar_rect, 2.0, 2.0)

            p.setPen(tone if self.level != "idle" else QColor("#9E97B0"))
            p.drawText(QRectF(bar_w + 8.0, 0, w - bar_w - 8.0, h),
                       Qt.AlignRight | Qt.AlignVCenter, elided)


    class NoSignalCanvas(QWidget):
        """Full-stage replica of EverLive's gc573_user_nosignal.c standby screen."""
        def __init__(self, parent=None):
            super().__init__(parent)
            self._title = "NO HDMI SIGNAL"
            self._subtitle = "Waiting for console / input..."
            self._footer = "EVERLIVE  |  USERSPACE VFIO ENGINE"
            self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)

        def set_messages(self, title: str, subtitle: str,
                         footer: str = "EVERLIVE  |  USERSPACE VFIO ENGINE"):
            if (self._title, self._subtitle, self._footer) != (title, subtitle, footer):
                self._title = title
                self._subtitle = subtitle
                self._footer = footer
                self.update()

        def paintEvent(self, event):
            p = QPainter(self)
            p.setRenderHint(QPainter.Antialiasing)
            w, h = float(self.width()), float(self.height())
            if w <= 2 or h <= 2:
                return
            s = min(w / 1280.0, h / 720.0)
            if s < 0.35:
                s = 0.35
            cx = w * 0.5

            # Background + subtle radial cyan glow + vignette (matches gc573_user_nosignal.c)
            p.fillRect(self.rect(), QColor(8, 11, 18))
            from PySide6.QtGui import QRadialGradient
            grad = QRadialGradient(cx, h * 0.48, max(w, h) * 0.52)
            grad.setColorAt(0.0, QColor(30, 120, 150, 26))
            grad.setColorAt(0.55, QColor(14, 24, 38, 12))
            grad.setColorAt(1.0, QColor(4, 6, 10, 140))
            p.fillRect(self.rect(), grad)

            # Subtle grid
            step = max(24, int(round(80.0 * (h / 1080.0))))
            row = 0
            y = 0
            while y < int(h):
                a = 19 if (row % 3 == 0) else 9
                p.setPen(QPen(QColor(148, 163, 184, a), 1.0))
                p.drawLine(0, y, int(w), y)
                y += step
                row += 1
            col = 0
            x = 0
            while x < int(w):
                a = 19 if (col % 3 == 0) else 9
                p.setPen(QPen(QColor(148, 163, 184, a), 1.0))
                p.drawLine(x, 0, x, int(h))
                x += step
                col += 1

            # Top wordmark: EVERLIVE + cyan bar
            mark_y = 0.16 * h
            mark_font = QFont(ui_theme.MONO_FAMILY, max(9, int(round(16 * s))))
            mark_font.setBold(True)
            mark_font.setLetterSpacing(QFont.AbsoluteSpacing, max(2.0, 4.0 * s))
            p.setFont(mark_font)
            p.setPen(QColor(34, 211, 238, 235))
            mark_rect = QRectF(0, mark_y, w, 28 * s)
            p.drawText(mark_rect, Qt.AlignHCenter | Qt.AlignTop, "EVERLIVE")

            bar_w = 56.0 * s
            bar_h = max(2.0, 2.5 * s)
            bar_y = mark_y + 26.0 * s
            p.fillRect(QRectF(cx - bar_w * 0.5, bar_y, bar_w, bar_h),
                       QColor(34, 211, 238, 140))

            # Center monitor icon with stand + red diagonal slash
            icon_w = min(0.42 * w, 340.0 * s)
            icon_h = icon_w * 0.58
            icon_cy = bar_y + bar_h + 26.0 * s + icon_h * 0.5
            radius = max(6.0, 14.0 * s)
            stroke = max(2.2, 3.8 * s)
            mon_rect = QRectF(cx - icon_w * 0.5, icon_cy - icon_h * 0.5, icon_w, icon_h)

            p.setBrush(QColor(10, 17, 28, 155))
            p.setPen(QPen(QColor(56, 189, 248, 218), stroke))
            p.drawRoundedRect(mon_rect, radius, radius)

            stand_top = icon_cy + icon_h * 0.5
            stand_h = 18.0 * s
            p.setPen(QPen(QColor(56, 189, 248, 192), stroke * 0.9, Qt.SolidLine, Qt.RoundCap))
            p.drawLine(QPointF(cx, stand_top), QPointF(cx, stand_top + stand_h))
            p.drawLine(QPointF(cx - icon_w * 0.16, stand_top + stand_h),
                       QPointF(cx + icon_w * 0.16, stand_top + stand_h))

            p1 = QPointF(cx - icon_w * 0.36, icon_cy - icon_h * 0.32)
            p2 = QPointF(cx + icon_w * 0.36, icon_cy + icon_h * 0.32)
            p.setPen(QPen(QColor(8, 11, 18, 185), stroke * 2.2, Qt.SolidLine, Qt.RoundCap))
            p.drawLine(p1, p2)
            p.setPen(QPen(QColor(248, 113, 113, 242), stroke * 1.25, Qt.SolidLine, Qt.RoundCap))
            p.drawLine(p1, p2)

            # Headline: NO HDMI SIGNAL
            title_y = stand_top + stand_h + 40.0 * s
            title_font = QFont(ui_theme.MONO_FAMILY, max(14, int(round(34 * s))))
            title_font.setBold(True)
            title_font.setLetterSpacing(QFont.AbsoluteSpacing, max(2.0, 4.0 * s))
            p.setFont(title_font)
            p.setPen(QColor(226, 232, 240, 255))
            p.drawText(QRectF(0, title_y, w, 52 * s), Qt.AlignHCenter | Qt.AlignTop, self._title)

            # Subtitle: Waiting for console / input...
            sub_y = title_y + 60.0 * s
            sub_font = QFont(ui_theme.MONO_FAMILY, max(9, int(round(13 * s))))
            p.setFont(sub_font)
            p.setPen(QColor(100, 116, 139, 255))
            p.drawText(QRectF(0, sub_y, w, 26 * s), Qt.AlignHCenter | Qt.AlignTop, self._subtitle)

            # Footer: EVERLIVE  |  USERSPACE VFIO ENGINE
            foot_font = QFont(ui_theme.MONO_FAMILY, max(8, int(round(10 * s))))
            foot_font.setLetterSpacing(QFont.AbsoluteSpacing, max(1.0, 2.0 * s))
            p.setFont(foot_font)
            p.setPen(QColor(51, 65, 85, 230))
            p.drawText(QRectF(0, h - 38.0 * s, w, 22 * s), Qt.AlignHCenter | Qt.AlignTop, self._footer)

    class CameraLauncherDialog(QDialog):
        """Create reversible per-app desktop shortcuts with LD_PRELOAD."""

        DIALOG_QSS = ui_theme.build_stylesheet()

        def __init__(self, profile, parent=None):
            super().__init__(parent)
            self.profile = profile
            self.library = app_integrations.find_preload_library(profile)
            self.setWindowTitle("EverLive App Camera Shortcuts")
            self.setMinimumSize(680, 460)
            self.setStyleSheet(self.DIALOG_QSS)

            root = QVBoxLayout(self)
            root.setContentsMargins(18, 16, 18, 16)
            root.setSpacing(10)

            title = QLabel("Launch apps with the EverLive camera")
            title.setStyleSheet(
                f"color: {ui_theme.TEXT}; font-size: 16px; font-weight: 700;")
            root.addWidget(title)

            nodes = ", ".join(profile.preload_nodes) or "no nodes"
            desc = QLabel(
                "Creates separate application-menu shortcuts that set "
                f"LD_PRELOAD for one app. Exposed nodes: {nodes}. "
                "Session-wide LD_PRELOAD is never set.")
            desc.setWordWrap(True)
            desc.setObjectName("fieldLabel")
            root.addWidget(desc)

            lib_label = QLabel(
                f"Library: {self.library}" if self.library
                else "Preload library not found. Install EverLive first.")
            lib_label.setObjectName("fieldLabel")
            lib_label.setWordWrap(True)
            root.addWidget(lib_label)

            self.scroll = QScrollArea()
            self.scroll.setWidgetResizable(True)
            self.scroll.setFrameShape(QFrame.NoFrame)
            self.scroll.viewport().setStyleSheet("background: transparent;")
            self.body = QWidget()
            self.body.setStyleSheet("background: transparent;")
            self.body_layout = QVBoxLayout(self.body)
            self.body_layout.setContentsMargins(0, 0, 0, 0)
            self.body_layout.setSpacing(8)
            self.scroll.setWidget(self.body)
            root.addWidget(self.scroll, 1)

            self.lbl_status = QLabel("")
            self.lbl_status.setObjectName("hintText")
            self.lbl_status.setWordWrap(True)
            root.addWidget(self.lbl_status)

            footer = QHBoxLayout()
            btn_refresh = QPushButton("Refresh")
            btn_refresh.clicked.connect(self._populate)
            footer.addWidget(btn_refresh)

            self.btn_create_all = QPushButton("Create All Shortcuts")
            self.btn_create_all.setToolTip(
                "Create an EverLive camera launcher for every supported "
                "native app. Existing shortcuts are left untouched.")
            self.btn_create_all.clicked.connect(self._create_all)
            self.btn_create_all.setEnabled(bool(self.library))
            footer.addWidget(self.btn_create_all)
            footer.addStretch(1)
            btn_close = QPushButton("Close")
            btn_close.clicked.connect(self.accept)
            footer.addWidget(btn_close)
            root.addLayout(footer)

            self._populate()

        def _clear(self):
            while self.body_layout.count():
                item = self.body_layout.takeAt(0)
                widget = item.widget()
                if widget is not None:
                    widget.deleteLater()

        def _populate(self):
            self._clear()
            apps = app_integrations.find_camera_apps()
            if not apps:
                empty = QLabel(
                    "No supported apps detected. Install OBS Studio, Discord, "
                    "Chrome/Chromium/Brave, Firefox or Zoom, then refresh.")
                empty.setWordWrap(True)
                empty.setObjectName("fieldLabel")
                self.body_layout.addWidget(empty)
                self.body_layout.addStretch(1)
                return

            for app in apps:
                row = QFrame()
                row.setObjectName("cardFrame")
                lay = QHBoxLayout(row)
                lay.setContentsMargins(10, 8, 10, 8)
                lay.setSpacing(8)

                text = QVBoxLayout()
                name = QLabel(f"{app.name}  ·  {app.badge}")
                name.setStyleSheet(
                    f"color: {ui_theme.TEXT}; font-weight: 600;")
                text.addWidget(name)
                state = QLabel(
                    "Shortcut installed" if app_integrations.launcher_exists(app)
                    else ("Sandboxed — use v4l2loopback" if app.sandboxed
                          else "No EverLive shortcut"))
                state.setObjectName("fieldLabel")
                text.addWidget(state)
                if ("chrome" in app.desktop_id.lower() or
                        "chromium" in app.desktop_id.lower() or
                        "brave" in app.desktop_id.lower()):
                    warn = QLabel(
                        "Experimental: Chromium's renderer sandbox may block "
                        "the virtual camera.")
                    warn.setObjectName("fieldLabel")
                    warn.setStyleSheet(f"color: {ui_theme.WARN};")
                    warn.setWordWrap(True)
                    text.addWidget(warn)
                lay.addLayout(text, 1)

                if not app.sandboxed:
                    btn_create = QPushButton(
                        "Create" if not app_integrations.launcher_exists(app)
                        else "Recreate")
                    btn_create.clicked.connect(
                        lambda _=False, a=app: self._create(a))
                    lay.addWidget(btn_create)

                    btn_remove = QPushButton("Remove")
                    btn_remove.setEnabled(app_integrations.launcher_exists(app))
                    btn_remove.clicked.connect(
                        lambda _=False, a=app: self._remove(a))
                    lay.addWidget(btn_remove)

                    btn_copy = QPushButton("Copy Command")
                    btn_copy.clicked.connect(
                        lambda _=False, a=app: self._copy(a))
                    lay.addWidget(btn_copy)
                else:
                    warn = QLabel("LD_PRELOAD cannot cross this sandbox.")
                    warn.setObjectName("fieldLabel")
                    warn.setStyleSheet(f"color: {ui_theme.WARN};")
                    lay.addWidget(warn)

                self.body_layout.addWidget(row)

            test_row = QHBoxLayout()
            test_row.addStretch(1)
            btn_test = QPushButton("Test Camera")
            btn_test.setEnabled(bool(self.library and self.profile.preload_nodes))
            btn_test.clicked.connect(self._test_camera)
            test_row.addWidget(btn_test)
            self.body_layout.addLayout(test_row)
            self.body_layout.addStretch(1)

        def _create(self, app):
            if not self.library:
                self._notify("Preload library not found.")
                return
            _ok, message = app_integrations.create_launcher(app, self.library)
            self._notify(message)
            self._populate()

        def _create_all(self):
            if not self.library:
                self._notify("Preload library not found.")
                return
            results = app_integrations.install_launchers(self.library)
            created = sum(
                1 for _app, ok, message in results
                if ok and message.startswith("Created"))
            failed = sum(1 for _app, ok, _message in results if not ok)
            if created:
                message = f"Created {created} camera shortcut(s)."
                if failed:
                    message += f" {failed} app(s) could not be configured."
                self._notify(message)
            elif failed:
                self._notify(
                    f"No shortcuts created; {failed} app(s) could not be "
                    "configured.")
            else:
                self._notify("All supported apps already have shortcuts.")
            self._populate()

        def _remove(self, app):
            _ok, message = app_integrations.remove_launcher(app)
            self._notify(message)
            self._populate()

        def _copy(self, app):
            if not self.library:
                self._notify("Preload library not found.")
                return
            QApplication.clipboard().setText(
                app_integrations.launch_command(app, self.library))
            self._notify("Launch command copied to the clipboard.")

        def _test_camera(self):
            if not self.library or not self.profile.preload_nodes:
                return
            node = self.profile.preload_nodes[0]
            ok, output = app_integrations.test_camera(self.library, node)
            box = QMessageBox(self)
            box.setWindowTitle("EverLive Camera Test")
            box.setIcon(QMessageBox.Information if ok else QMessageBox.Warning)
            box.setText(
                f"{node}: {'opened successfully' if ok else 'could not be opened'}")
            box.setDetailedText(output[:4000])
            box.exec()

        def _notify(self, message: str):
            self.lbl_status.setText(message)
            parent = self.parent()
            if parent is not None and hasattr(parent, "_set_vfio_notice"):
                try:
                    parent._set_vfio_notice(
                        "App Camera", message, seconds=5.0)
                except Exception:
                    pass

    class PipPreview(QWidget):
        """Webcam picture-in-picture preview painted by the Hub.

        A native child (so it can sit above the engine's embedded window)
        that paints RGB24 frames read from the bridge's preview pipe on a
        background thread.  Only the newest frame is kept to ensure zero latency.
        """

        frameReady = Signal()

        def __init__(self, parent=None):
            super().__init__(parent)
            self.setAttribute(Qt.WA_NativeWindow, True)
            self.setAttribute(Qt.WA_DontCreateNativeAncestors, True)
            self.setAttribute(Qt.WA_TransparentForMouseEvents, True)
            self.setAttribute(Qt.WA_OpaquePaintEvent, True)
            self._image: Optional[QImage] = None
            self._thread: Optional[threading.Thread] = None
            self._stream = None
            self._lock = threading.Lock()
            self._pending_frame = None
            self.frames = 0
            self.frameReady.connect(self._on_frame)

        def attach(self, stream, width: int, height: int):
            """Read ``width`` x ``height`` RGB24 frames from ``stream``."""
            self.detach()
            self._stream = stream
            self._thread = threading.Thread(
                target=self._reader, args=(stream, width, height),
                name="pip-preview", daemon=True)
            self._thread.start()

        def detach(self):
            # The reader ends at EOF, when the preview process is stopped.
            self._stream = None
            self._thread = None
            with self._lock:
                self._pending_frame = None
            self._image = None

        def _reader(self, stream, width: int, height: int):
            size = width * height * 3
            while self._stream is stream:
                buf = bytearray(size)
                view = memoryview(buf)
                got = 0
                try:
                    while got < size:
                        n = stream.readinto(view[got:])
                        if not n:
                            return
                        got += n
                except (OSError, ValueError):
                    return
                if self._stream is stream:
                    with self._lock:
                        self._pending_frame = (bytes(buf), width, height)
                    self.frameReady.emit()

        def _on_frame(self):
            with self._lock:
                payload = self._pending_frame
                self._pending_frame = None
            if payload is None:
                return
            data, width, height = payload
            self._image = QImage(data, width, height, width * 3,
                                 QImage.Format_RGB888)
            self.frames += 1
            self.update()

        def paintEvent(self, event):  # noqa: N802 (Qt casing)
            painter = QPainter(self)
            if self._image is None:
                painter.fillRect(self.rect(), QColor("#000000"))
            else:
                painter.setRenderHint(QPainter.SmoothPixmapTransform, True)
                painter.drawImage(self.rect(), self._image)
            painter.end()

    class MultiviewTile(QFrame):
        """One extra card in the multiview stage.

        The card's engine embeds its SDL window into ``host`` (a native
        child window); the slim bar on top names the card and carries its
        listen toggle and recording state.
        """

        listenToggled = Signal(str, bool)
        volumeChanged = Signal(str, int)

        def __init__(self, card: Dict[str, Optional[str]], parent=None,
                     volume: int = 100):
            super().__init__(parent)
            self.pci = str(card.get("pci") or "")
            self.setObjectName("multiviewTile")
            self.setStyleSheet(
                "QFrame#multiviewTile {"
                f" background-color: {ui_theme.VIDEO_BG};"
                f" border: 1px solid {ui_theme.BORDER};"
                f" border-radius: {ui_theme.SEGMENT_RADIUS_CSS}; }}")
            lay = QVBoxLayout(self)
            lay.setContentsMargins(1, 1, 1, 1)
            lay.setSpacing(0)

            bar = QWidget(self)
            bar.setFixedHeight(30)
            bar_lay = QHBoxLayout(bar)
            bar_lay.setContentsMargins(10, 0, 4, 0)
            bar_lay.setSpacing(8)
            self.lbl_name = QLabel(card_display_name(card), bar)
            self.lbl_name.setStyleSheet(
                f"color: {ui_theme.TEXT}; font-size: 11.5px; font-weight: 700;"
                " letter-spacing: 0.4px;")
            bar_lay.addWidget(self.lbl_name)
            self.lbl_state = QLabel("Starting…", bar)
            self.lbl_state.setStyleSheet(
                f"color: {ui_theme.TEXT_MUTED}; font-size: 11px;")
            bar_lay.addWidget(self.lbl_state, 1)
            self.lbl_rec = QLabel("", bar)
            self.lbl_rec.setStyleSheet(
                f"color: {ui_theme.DANGER}; font-size: 11px; font-weight: 700;")
            bar_lay.addWidget(self.lbl_rec)
            self.btn_listen = IconButton(
                "mute", "Listen to this card (its recording always has sound)",
                bar, checkable=True, size=26, icon_size=13)
            self.btn_listen.toggled.connect(self._on_listen)
            bar_lay.addWidget(self.btn_listen)
            # Local listening level only; the recording keeps the card's
            # own level (it reads the audio tap).
            self.slider_vol = QSlider(Qt.Horizontal, bar)
            self.slider_vol.setRange(0, 150)
            self.slider_vol.setFixedWidth(96)
            self.slider_vol.setValue(max(0, min(150, int(volume))))
            self.slider_vol.setToolTip(
                "Listening volume for this card (recordings are not affected)")
            self.slider_vol.valueChanged.connect(self._on_volume)
            bar_lay.addWidget(self.slider_vol)
            self.lbl_vol = QLabel(f"{self.slider_vol.value()}%", bar)
            self.lbl_vol.setFixedWidth(38)
            self.lbl_vol.setStyleSheet(
                f"color: {ui_theme.TEXT_MUTED}; font-size: 11px;")
            bar_lay.addWidget(self.lbl_vol)
            lay.addWidget(bar)

            self.host = QWidget(self)
            self.host.setAttribute(Qt.WA_NativeWindow, True)
            self.host.setAttribute(Qt.WA_DontCreateNativeAncestors, True)
            self.host.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
            self.host.setStyleSheet(
                f"background-color: {ui_theme.VIDEO_BG};")
            lay.addWidget(self.host, 1)
            _ = self.host.winId()

        def host_winid(self) -> int:
            try:
                return int(self.host.winId())
            except Exception:
                return 0

        def _on_listen(self, checked: bool):
            self.btn_listen.set_icon("volume" if checked else "mute")
            self.listenToggled.emit(self.pci, bool(checked))

        def _on_volume(self, value: int):
            self.lbl_vol.setText(f"{value}%")
            self.volumeChanged.emit(self.pci, int(value))
            # Turning a muted card up means "let me hear it".
            if value > 0 and not self.btn_listen.isChecked():
                self.btn_listen.setChecked(True)

        def set_state(self, text: str, rec: str = ""):
            if self.lbl_state.text() != text:
                self.lbl_state.setText(text)
            if self.lbl_rec.text() != rec:
                self.lbl_rec.setText(rec)

    class MainWindow(QMainWindow):
        def __init__(self, streamer: GameStreamer, vsync: bool = True, streamer_mode: bool = False,
                     headless_start: bool = False, v4l2_device: Optional[str] = None):
            super().__init__()
            self.setFocusPolicy(Qt.StrongFocus)
            self.streamer = streamer
            self.vfio_mode = streamer.vfio_mode
            self.uvc_mode = getattr(streamer, "uvc_mode", False)
            self.duo_mode = getattr(streamer, "duo_mode", False)
            self.model = getattr(streamer, "model", MODEL_GC573)
            self.card_profile = detect_card_profile(self.model)
            self.enable_vsync = vsync
            self.muted = False
            self.saved_cfg = load_settings()
            self._headless_start = headless_start
            self._v4l2_device = v4l2_device
            # Remote control: set once we attach to a streamer we did not
            # start ourselves (it owns the card, we only observe it).
            self._remote = False
            self._remote_status: Dict = {}
            self._ipc = None
            self._ipc_timer = None
            self._ipc_shutdown = False
            # A remote engine docked into the Hub viewport over mode.set.
            self._remote_docked = False
            self._dock_pending_until = 0.0
            # The headless DUO unit was running and we stopped it; restart it
            # when the Hub exits.
            self._service_was_active = False
            # EverLive universal background service (separate from the legacy
            # DUO unit above).
            self._background_service_active = False
            self._background_service_status = None
            self._takeover_pending = False
            self._takeover_tries = 0
            self._vfio_notice_until = 0.0
            self._vfio_volume_applies = 0
            self._launch_mode: Optional[str] = None
            self._last_launch_mode = "gaming"
            # Set while one of our engines has handed the card to another user.
            self._handoff_paused: Optional[Dict] = None
            # Target mode of an in-flight gaming <-> studio handover.
            self._switch_pending: Optional[str] = None
            self._handoff_tries = 0
            self.recorder = everlive_recorder.EverLiveRecorder()
            self._rec_was_active = False
            if streamer_mode:
                self.saved_cfg["streamer_mode"] = True
            if not self.vfio_mode and self.saved_cfg.get("engine", "native") == "mpv":
                self.streamer.force_mpv = True

            self.init_ui()
            self._apply_card_profile()
            self._refresh_service_config()
            self._refresh_service_status()

            if v4l2_device:
                self.chk_v4l2.setChecked(True)
                if isinstance(v4l2_device, str) and v4l2_device != V4L2_DEFAULT_DEVICE:
                    # The compact layout has no path field; surface a custom
                    # node through the tooltip instead of a phantom widget.
                    self.chk_v4l2.setToolTip(
                        f"Virtual camera output to {v4l2_device} "
                        "(custom path from the command line).")

            # Multiview (extra cards in stage tiles).
            self._mv_runner = None
            self._mv_tiles: Dict[str, "MultiviewTile"] = {}
            self._mv_timer = QTimer(self)
            self._mv_timer.setInterval(1000)
            self._mv_timer.timeout.connect(self._multiview_poll)

            # Webcam bridge + picture-in-picture preview (the bridge object
            # is created by init_ui, which needs it for the settings card).
            self._ensure_cam()
            self._cam_timer = QTimer(self)
            self._cam_timer.setInterval(2000)
            self._cam_timer.timeout.connect(self._webcam_tick)

            # Signal monitor timer (1s polling)
            self.timer = QTimer(self)
            self.timer.timeout.connect(self.update_status)
            self.timer.start(1000)

            # Background capture service state (10s polling).
            self._service_timer = QTimer(self)
            self._service_timer.setInterval(10000)
            self._service_timer.timeout.connect(self._refresh_service_status)
            self._service_timer.start()

            # Fast 100 ms poll for embedded SDL2 hotkey forwarders ([HUB] ...)
            self._hub_action_timer = QTimer(self)
            self._hub_action_timer.setInterval(100)
            self._hub_action_timer.timeout.connect(self._poll_hub_actions)
            self._hub_action_timer.start()

            # Remote channel to a streamer that already owns the device.
            # QSocketNotifier keeps this on the GUI thread: no worker threads
            # and no cross-thread signals.
            self._ipc = EverLiveIpcClient(self)
            if self.streamer is not None:
                # Let the engine wrapper prefer IPC for DUO input/HPD/EDID
                # when the hub is attached; gc570d-ctl stays as the fallback.
                self.streamer.ipc = self._ipc
            self._ipc.connectedChanged.connect(self._on_remote_connection)
            self._ipc.statusReceived.connect(self._on_remote_status)
            self._ipc.errorRaised.connect(lambda msg: print(f"[IPC] {msg}"))
            self._ipc_timer = QTimer(self)
            self._ipc_timer.setInterval(1000)
            self._ipc_timer.timeout.connect(self._poll_remote_status)

            # Check if autostart or headless start is requested
            if headless_start:
                QTimer.singleShot(250, self.start_studio)
            elif self.saved_cfg.get("autostart", False) or getattr(streamer, "_cli_autostart", False):
                QTimer.singleShot(250, self.start_gaming_screen)

            QTimer.singleShot(300, self, self._ipc_try_attach)

        def init_ui(self):
            self.setWindowTitle("EverLive - Console Hub")
            self._cinema_fullscreen = False
            self.ui_mode = (
                "advanced"
                if str(self.saved_cfg.get("ui_mode", "simple")) == "advanced"
                else "simple")
            self.resize(1440, 860)
            self.setMinimumSize(1180, 720)

            # Application & Window Icon
            icon_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "everlive.png")
            if os.path.isfile(icon_path):
                self.setWindowIcon(QIcon(icon_path))

            # Palette, spacing and control styling live in ui_theme.py so
            # the shell, inspector and dialogs share one source of truth.
            ui_theme.ensure_fonts()
            self.setStyleSheet(ui_theme.build_stylesheet())

            central_widget = QWidget()
            self.setCentralWidget(central_widget)
            self.root_layout = QVBoxLayout(central_widget)
            # The header and bottom bar are full-bleed; only the workspace
            # content between them is inset.
            self.root_layout.setContentsMargins(0, 0, 0, 0)
            self.root_layout.setSpacing(0)

            # =============================================================
            # 1. Top Studio Header Bar
            #    Brand · capture source · state · actions · mode · links
            # =============================================================
            self.header_widget = QFrame()
            self.header_widget.setObjectName("headerBar")
            self.header_widget.setFixedHeight(ui_theme.HEADER_HEIGHT)
            top_bar = QHBoxLayout(self.header_widget)
            top_bar.setContentsMargins(16, 14, 16, 14)
            top_bar.setSpacing(8)
            self.top_bar = top_bar

            if os.path.isfile(icon_path):
                lbl_icon = QLabel()
                pix = QPixmap(icon_path).scaled(
                    26, 26, Qt.KeepAspectRatio, Qt.SmoothTransformation)
                lbl_icon.setPixmap(pix)
                lbl_icon.setFixedSize(26, 26)
                top_bar.addWidget(lbl_icon, 0, Qt.AlignVCenter)

            lbl_logo = QLabel("EVERLIVE")
            lbl_logo.setObjectName("logoTitle")
            top_bar.addWidget(lbl_logo, 0, Qt.AlignVCenter)

            lbl_ver = QLabel(f"v{__version__}")
            lbl_ver.setObjectName("logoVersion")
            top_bar.addWidget(lbl_ver, 0, Qt.AlignVCenter)

            top_bar.addSpacing(12)

            # --- Capture source: card name and DUO input share one group ---
            card_detected = (
                getattr(self.streamer, "mode", "unknown") != "unknown"
                or bool(getattr(self.streamer, "hw_info", {}).get("pci"))
                or bool(getattr(self.streamer, "hw_info", {}).get("usb_port"))
            )
            if card_detected:
                self._card_full_label = MODEL_NAMES.get(
                    self.model, MODEL_NAMES[MODEL_GC573])
                if self.model == MODEL_GC570D:
                    self._card_short_label = "GC570D • DUO"
                elif self.model == MODEL_HWSUHD:
                    self._card_short_label = "HWS • 4K"
                elif self.model == MODEL_GC553G2:
                    self._card_short_label = "GC553G2 • ULTRA 2.1"
                else:
                    self._card_short_label = "GC573 • 4K"
            else:
                self._card_full_label = "NO CAPTURE CARD"
                self._card_short_label = "NO CAPTURE CARD"

            self.source_group = QFrame()
            self.source_group.setObjectName("sourceGroup")
            self.source_group.setFixedHeight(ui_theme.CONTROL_H)
            source_layout = QHBoxLayout(self.source_group)
            source_layout.setContentsMargins(12, 0, 0, 0)
            source_layout.setSpacing(10)

            self.lbl_card_title = QLabel(self._card_full_label)
            self.lbl_card_title.setObjectName("sourceName")
            self.lbl_card_title.setAlignment(Qt.AlignVCenter | Qt.AlignLeft)
            self.lbl_card_title.setFixedHeight(ui_theme.CONTROL_H)
            self.lbl_card_title.setToolTip("Status: Standby")
            if not card_detected:
                self.lbl_card_title.setStyleSheet(
                    "color: #FF6B7A; font-size: 12.5px; font-weight: 800; "
                    "letter-spacing: 0.6px;")
            source_layout.addWidget(self.lbl_card_title, 0, Qt.AlignVCenter)

            # Card selector: only when more than one supported card exists.
            self._capture_cards = detect_capture_cards()
            if len(self._capture_cards) > 1:
                current_pci = getattr(self.streamer, "hw_info", {}).get("pci")
                self.combo_card = QComboBox()
                self.combo_card.setObjectName("sourceInputCombo")
                for card in self._capture_cards:
                    self.combo_card.addItem(card_display_name(card), card["pci"])
                    if card["pci"] == current_pci:
                        self.combo_card.setCurrentIndex(
                            self.combo_card.count() - 1)
                self.combo_card.setFixedHeight(ui_theme.CONTROL_H)
                self.combo_card.setMinimumWidth(150)
                self.combo_card.setToolTip(
                    "Capture card used by the Console Hub. Switching restarts "
                    "the Hub on the selected card and remembers the choice.")
                self.combo_card.currentIndexChanged.connect(
                    self.on_card_changed)
                source_layout.addWidget(self.combo_card, 0, Qt.AlignVCenter)

                # Multiview: every other card in its own tile on the stage.
                self.btn_multiview = IconButton(
                    "table-cells-large",
                    "Multiview: show the other capture cards beside this one. "
                    "Record then records every card.",
                    checkable=True, size=ui_theme.CONTROL_H, icon_size=15)
                self.btn_multiview.setChecked(
                    bool(load_settings().get("multiview", False)))
                self.btn_multiview.toggled.connect(self.on_multiview_toggled)
                source_layout.addWidget(self.btn_multiview, 0, Qt.AlignVCenter)

            # Built for every card; _apply_card_profile() shows it on the DUO.
            saved_input = load_settings().get("duo_input", 0)
            self.combo_duo_input = QComboBox()
            self.combo_duo_input.setObjectName("sourceInputCombo")
            self.combo_duo_input.addItem("HDMI 1", 0)
            self.combo_duo_input.addItem("HDMI 2", 1)
            self.combo_duo_input.setCurrentIndex(
                1 if saved_input == 1 else 0)
            self.combo_duo_input.setFixedHeight(ui_theme.CONTROL_H)
            self.combo_duo_input.setMinimumWidth(108)
            self.combo_duo_input.setToolTip(
                "Select which Live Gamer DUO HDMI input is shown in the "
                "gaming window and primary capture stream.")
            self.combo_duo_input.currentIndexChanged.connect(
                self.on_duo_input_changed)
            source_layout.addWidget(
                self.combo_duo_input, 0, Qt.AlignVCenter)

            top_bar.addWidget(self.source_group, 0, Qt.AlignVCenter)

            top_bar.addStretch(1)

            # Hidden compatibility carrier; existing update paths still write
            # the compact status text here and to the card-chip tooltip.
            self.lbl_pill = QLabel("○ Standby", self.header_widget)
            self.lbl_pill.setVisible(False)

            # Fused primary capture controls.
            self.action_group = QWidget()
            self.action_group.setObjectName("actionGroup")
            self.action_group.setAttribute(Qt.WA_StyledBackground, True)
            self.action_group.setFixedHeight(ui_theme.CONTROL_H)
            action_layout = QHBoxLayout(self.action_group)
            action_layout.setContentsMargins(0, 0, 0, 0)
            action_layout.setSpacing(0)

            self.btn_launch = IconButton(
                "play", "Start Gaming", size=ui_theme.CONTROL_H,
                icon_size=16, color="#FFFFFF", active_color="#FFFFFF")
            self.btn_launch.setObjectName("actionStart")
            self.btn_launch.clicked.connect(self.toggle_stream)
            action_layout.addWidget(self.btn_launch)

            self.btn_studio = IconButton(
                "tower-broadcast",
                "Start Studio / Headless (OBS)",
                size=ui_theme.CONTROL_H, icon_size=16)
            self.btn_studio.setObjectName("actionButton")
            self.btn_studio.setToolTip(
                "Runs the userspace engine headless and publishes native video & audio "
                "to PipeWire, Userspace SHM (libgc570d-v4l2.so), and optional V4L2 for OBS/Discord.")
            self.btn_studio.clicked.connect(self.toggle_studio)
            if not self.vfio_mode:
                self.btn_studio.setEnabled(False)
                self.btn_studio.setToolTip(
                    "Studio / headless mode requires the userspace VFIO engine.")
            action_layout.addWidget(self.btn_studio)

            # Picture-in-picture overlay over the main card's picture.
            self.btn_pip = IconButton(
                "window-restore",
                "Picture-in-picture overlay: shows a webcam or 2nd capture source in a corner "
                "of the picture and adds it to recordings. Settings: Capture "
                "→ Picture-in-Picture.",
                checkable=True, size=ui_theme.CONTROL_H, icon_size=15)
            self.btn_pip.setObjectName("actionButton")
            self.btn_pip.setChecked(
                bool(everlive_cam.normalize_settings(load_settings())["pip_enabled"]))
            self.btn_pip.toggled.connect(self.on_pip_toggled)
            action_layout.addWidget(self.btn_pip)

            self.btn_record = IconButton(
                "record", "Start Video Recording (Ctrl+R)",
                size=ui_theme.CONTROL_H, icon_size=15,
                color=ui_theme.DANGER, active_color=ui_theme.ON_ACCENT)
            self.btn_record.setObjectName("actionRecord")
            self.btn_record.setProperty("recording", False)
            self.btn_record.clicked.connect(self.toggle_recording)
            action_layout.addWidget(self.btn_record)

            self.btn_resync = IconButton(
                "rotate",
                "Restart the userspace VFIO engine. HDMI hot-switching is automatic."
                if self.vfio_mode else
                "Trigger hardware HDMI handshake (HPD pulse) - also key 'H' in window",
                size=ui_theme.CONTROL_H, icon_size=16)
            self.btn_resync.setObjectName("actionButton")
            self.btn_resync.clicked.connect(self.force_resync)
            action_layout.addWidget(self.btn_resync)

            top_bar.addWidget(self.action_group, 0, Qt.AlignVCenter)
            self._update_action_group_positions()

            top_bar.addSpacing(6)

            self.combo_ui_mode = QComboBox()
            self.combo_ui_mode.addItem("Simple", "simple")
            self.combo_ui_mode.addItem("Advanced", "advanced")
            saved_mode = ("advanced"
                          if str(self.saved_cfg.get("ui_mode", "simple"))
                          == "advanced" else "simple")
            self.combo_ui_mode.setCurrentIndex(
                self.combo_ui_mode.findData(saved_mode))
            self.combo_ui_mode.setFixedHeight(ui_theme.CONTROL_H)
            self.combo_ui_mode.setMinimumWidth(124)
            self.combo_ui_mode.setToolTip(
                "Simple hides the inspector and telemetry deck. Advanced "
                "exposes the full control surface.")
            self.combo_ui_mode.currentIndexChanged.connect(
                self.on_ui_mode_changed)
            top_bar.addWidget(self.combo_ui_mode, 0, Qt.AlignVCenter)

            top_bar.addSpacing(4)

            self.btn_discord = IconButton(
                "discord", "Join the EverLive Discord community",
                size=ui_theme.CONTROL_H, icon_size=18)
            self.btn_discord.clicked.connect(
                lambda: QDesktopServices.openUrl(
                    QUrl("https://discord.com/invite/D2XFhkPMmc")))
            top_bar.addWidget(self.btn_discord, 0, Qt.AlignVCenter)

            self.btn_github = IconButton(
                "github", "EverLive on GitHub",
                size=ui_theme.CONTROL_H, icon_size=18)
            self.btn_github.clicked.connect(
                lambda: QDesktopServices.openUrl(
                    QUrl("https://github.com/everlite/everlive")))
            top_bar.addWidget(self.btn_github, 0, Qt.AlignVCenter)

            self.btn_help = IconButton(
                "help", "Help and keyboard shortcuts",
                size=ui_theme.CONTROL_H, icon_size=18)
            self.btn_help.clicked.connect(self.on_help_clicked)
            top_bar.addWidget(self.btn_help, 0, Qt.AlignVCenter)

            self.root_layout.addWidget(self.header_widget)

            self.content_widget = QWidget()
            self.content_layout = QVBoxLayout(self.content_widget)
            self.content_layout.setContentsMargins(14, 10, 14, 10)
            self.content_layout.setSpacing(10)
            self.root_layout.addWidget(self.content_widget, 1)

            # =============================================================
            # 2. Main Workspace Split: Left Monitor Stage + Right Inspector
            # =============================================================
            self.workspace_widget = QWidget()
            workspace_layout = QHBoxLayout(self.workspace_widget)
            workspace_layout.setContentsMargins(0, 0, 0, 0)
            workspace_layout.setSpacing(10)

            # -------------------------------------------------------------
            # 2A. Left/Center Column: Unified Video+Transport Card + 3 Veyra Meters
            # -------------------------------------------------------------
            self.center_stage_widget = QWidget()
            stage_col = QVBoxLayout(self.center_stage_widget)
            stage_col.setContentsMargins(0, 0, 0, 0)
            stage_col.setSpacing(10)

            # --- Unified Video + Transport Card (Veyra vwrap style) ---
            self.video_stage_frame = QFrame()
            self.video_stage_frame.setObjectName("videoStageFrame")
            self.video_stage_frame.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
            stage_frame_layout = QVBoxLayout(self.video_stage_frame)
            stage_frame_layout.setContentsMargins(1, 1, 1, 1)
            stage_frame_layout.setSpacing(0)

            # Grid for the multiview stage: the main video host sits at
            # (0, 0); extra cards' tiles are added beside it when Multiview
            # is on.
            self.stage_grid_host = QWidget(self.video_stage_frame)
            self.stage_grid = QGridLayout(self.stage_grid_host)
            self.stage_grid.setContentsMargins(0, 0, 0, 0)
            self.stage_grid.setSpacing(0)

            self.video_host = QWidget(self.stage_grid_host)
            self.video_host.setFocusPolicy(Qt.StrongFocus)
            self.video_host.setAttribute(Qt.WA_NativeWindow, True)
            self.video_host.setAttribute(Qt.WA_DontCreateNativeAncestors, True)
            self.video_host.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
            self.video_host.setStyleSheet(
                f"background-color: {ui_theme.VIDEO_BG};")
            self.video_host.installEventFilter(self)

            # Rounded-corner mask for the native video surface.  The engine's
            # SDL window and its native host are rectangular, so this raised
            # sibling covers the corner cut-outs instead of relying on the
            # frame's QSS radius to clip a child window.
            self.video_corner_overlay = QWidget(self.stage_grid_host)
            self.video_corner_overlay.setObjectName("videoCornerOverlay")
            self.video_corner_overlay.setAttribute(Qt.WA_NativeWindow, True)
            self.video_corner_overlay.setAttribute(
                Qt.WA_DontCreateNativeAncestors, True)
            self.video_corner_overlay.setAttribute(
                Qt.WA_TransparentForMouseEvents, True)
            self.video_corner_overlay.setStyleSheet(
                f"background-color: {ui_theme.SURFACE};")
            self.video_corner_overlay.hide()

            # Webcam preview: a native sibling raised above video_host (and
            # the engine window inside it).
            self.pip_host = PipPreview(self.stage_grid_host)
            # The preview is display-only; never let it steal OSD clicks.
            self.pip_host.setAttribute(Qt.WA_TransparentForMouseEvents, True)
            self.pip_host.hide()

            host_inner_layout = QVBoxLayout(self.video_host)
            host_inner_layout.setContentsMargins(24, 24, 24, 24)
            host_inner_layout.setSpacing(0)

            # Standby / Status Overlay inside the video stage (hidden when embedded SDL is active)
            self.video_standby_overlay = NoSignalCanvas(self.video_host)
            host_inner_layout.addWidget(self.video_standby_overlay)

            # Detached remote-session card, overlaid on the stage.
            self.remote_card = QFrame(self.video_host)
            self.remote_card.setObjectName("remoteCard")
            self.remote_card.setFixedSize(440, 196)
            self.remote_card.setVisible(False)
            remote_lay = QVBoxLayout(self.remote_card)
            remote_lay.setContentsMargins(24, 22, 24, 20)
            remote_lay.setSpacing(10)
            remote_title = QLabel("REMOTE SESSION ATTACHED")
            remote_title.setObjectName("remoteTitle")
            remote_title.setAlignment(Qt.AlignCenter)
            remote_lay.addWidget(remote_title)
            remote_sub = QLabel(
                "Click below or press 'T' to dock video here.")
            remote_sub.setObjectName("hintText")
            remote_sub.setWordWrap(True)
            remote_sub.setAlignment(Qt.AlignCenter)
            remote_lay.addWidget(remote_sub)
            remote_lay.addStretch(1)
            self.btn_remote_takeback = QPushButton("Take Back Capture")
            self.btn_remote_takeback.setObjectName("btnLaunchStart")
            self.btn_remote_takeback.setFixedHeight(ui_theme.BUTTON_H)
            self.btn_remote_takeback.setCursor(Qt.PointingHandCursor)
            self.btn_remote_takeback.clicked.connect(self._remote_dock)
            remote_lay.addWidget(self.btn_remote_takeback)

            self.stage_grid.addWidget(self.video_host, 0, 0)
            stage_frame_layout.addWidget(self.stage_grid_host, 1)

            # Force creation of the native X11 window handle early
            _ = self.video_host.winId()


            # The audio transport moved into the full-width bottom bar, so the
            # video card is now a clean, uninterrupted stage.
            stage_col.addWidget(self.video_stage_frame, 1)

            # =============================================================
            # 2A-Bottom. Compact Unified EverLive Telemetry Strip
            # =============================================================
            self.telemetry_strip = QFrame()
            self.telemetry_strip.setObjectName("telemetryDeck")
            self.telemetry_strip.setFixedHeight(140)
            deck_layout = QVBoxLayout(self.telemetry_strip)
            deck_layout.setContentsMargins(16, 12, 16, 12)
            deck_layout.setSpacing(6)
            deck_title = QLabel("TELEMETRY DECK")
            deck_title.setObjectName("sectionTitle")
            deck_layout.addWidget(deck_title)
            tele_row = QHBoxLayout()
            tele_row.setContentsMargins(0, 0, 0, 0)
            tele_row.setSpacing(16)

            def _vsep():
                v = QFrame()
                v.setFixedWidth(1)
                v.setStyleSheet("background-color: #2A2D3E;")
                return v

            # --- Status ---------------------------------------------------
            seg1 = QVBoxLayout()
            seg1.setContentsMargins(0, 0, 0, 0)
            seg1.setSpacing(4)

            s1_title = QLabel("STATUS")
            s1_title.setObjectName("sectionTitle")
            seg1.addWidget(s1_title)

            s1_top = QHBoxLayout()
            s1_top.setContentsMargins(0, 0, 0, 0)
            s1_top.setSpacing(8)
            self.lbl_sig_tag = QLabel("● STANDBY")
            self.lbl_sig_tag.setStyleSheet(
                "color: #9AA0B0; background: transparent; border: none; "
                "padding: 0px; font-size: 11px; font-weight: 700;")
            s1_top.addWidget(self.lbl_sig_tag, 0, Qt.AlignVCenter)
            self.lbl_hero_signal = QLabel("Ready for Gaming")
            self.lbl_hero_signal.setObjectName("heroSignal")
            s1_top.addWidget(self.lbl_hero_signal, 1)
            seg1.addLayout(s1_top)

            # Granular status sub-column: format/tonemap, EDID and route.
            s1_sub = QGridLayout()
            s1_sub.setContentsMargins(0, 0, 0, 0)
            s1_sub.setHorizontalSpacing(16)
            s1_sub.setVerticalSpacing(2)
            self.lbl_kv_color = QLabel("YUV 4:2:2 · SDR")
            self.lbl_kv_color.setObjectName("kvKey")
            self.lbl_kv_pclk = QLabel(
                f"EDID: {self._edid_profile_label()}")
            self.lbl_kv_pclk.setObjectName("kvKey")
            self.lbl_kv_route = QLabel("SHM (/dev/video63)")
            self.lbl_kv_route.setObjectName("kvKey")
            s1_sub.addWidget(self.lbl_kv_color, 0, 0)
            s1_sub.addWidget(self.lbl_kv_pclk, 0, 1)
            s1_sub.addWidget(self.lbl_kv_route, 1, 0, 1, 2)
            seg1.addLayout(s1_sub)

            self.lbl_hero_meta = QLabel("", self.telemetry_strip)
            self.lbl_hero_meta.setObjectName("heroMeta")
            self.lbl_hero_meta.setVisible(False)
            self.lbl_kv_audio = QLabel(
                "No HDMI Audio", self.telemetry_strip)
            self.lbl_kv_audio.setObjectName("kvKey")
            self.lbl_kv_audio.setVisible(False)

            # Retained for the status-update path; not shown in the deck.
            self.status_light = VeyraStatusLight(self.telemetry_strip)
            self.status_light.setVisible(False)
            tele_row.addLayout(seg1, 10)
            tele_row.addWidget(_vsep())

            # --- Frame rate and cadence ----------------------------------
            seg2 = QVBoxLayout()
            seg2.setContentsMargins(0, 0, 0, 0)
            seg2.setSpacing(4)

            fps_row = QHBoxLayout()
            fps_row.setContentsMargins(0, 0, 0, 0)
            fps_row.setSpacing(6)
            self.lbl_fps_hero = QLabel("0.0")
            self.lbl_fps_hero.setObjectName("fpsHero")
            fps_row.addWidget(self.lbl_fps_hero, 0, Qt.AlignBaseline)
            self.lbl_fps_unit = QLabel("fps")
            self.lbl_fps_unit.setObjectName("fpsUnit")
            fps_row.addWidget(self.lbl_fps_unit, 0, Qt.AlignBaseline)
            self.lbl_drops_counter = QLabel("0 drops · 0 underrun")
            self.lbl_drops_counter.setStyleSheet(
                f"color: #ACA5C0; font-family: {ui_theme.MONO_STACK}; "
                "font-size: 13px;")
            fps_row.addWidget(
                self.lbl_drops_counter, 1,
                Qt.AlignRight | Qt.AlignBaseline)
            seg2.addLayout(fps_row)

            self.fps_sparkline = VeyraSparkline()
            seg2.addWidget(self.fps_sparkline)
            tele_row.addLayout(seg2, 12)
            tele_row.addWidget(_vsep())

            # --- Pipeline bars (mockup: PCIe DMA + Latency) --------------
            seg3 = QVBoxLayout()
            seg3.setContentsMargins(0, 0, 0, 0)
            seg3.setSpacing(5)
            self.bar_dma = VeyraMetricBar("PCIe DMA", "#38BDF8", "#BB96F3")
            self.bar_latency = VeyraMetricBar("Latency", "#5ED4B2", "#38BDF8")
            self.bar_vram = VeyraMetricBar(
                "VRAM", "#BB96F3", "#F287A7", self.telemetry_strip)
            self.bar_vram.setVisible(False)
            self.bar_cpu = VeyraMetricBar(
                "CPU", "#F0B17B", "#EF6FA3", self.telemetry_strip)
            self.bar_cpu.setVisible(False)
            seg3.addWidget(self.bar_dma)
            seg3.addWidget(self.bar_latency)

            self.lbl_budget_note = QLabel("Budget 16.7 ms / frame")
            self.lbl_budget_note.setStyleSheet(
                f"color: #94A3B8; font-family: {ui_theme.MONO_STACK}; "
                "font-size: 13px;")
            seg3.addWidget(self.lbl_budget_note)

            self.lbl_fg_status = QLabel("VRAM: — / — GB")
            self.lbl_fg_status.setObjectName("telemetryMeta")
            seg3.addWidget(self.lbl_fg_status)

            self.lbl_tele_pipeline = QLabel(
                "— ms Latency · 0.0 FPS", self.telemetry_strip)
            self.lbl_tele_pipeline.setVisible(False)
            self.lbl_tele_routing = QLabel(
                "DMA: 0 MB/s · Drops: 0 · Underruns: 0",
                self.telemetry_strip)
            self.lbl_tele_routing.setVisible(False)
            tele_row.addLayout(seg3, 9)
            tele_row.addWidget(_vsep())

            # --- DMA / GPU / CPU orbs ------------------------------------
            orbs_row = QHBoxLayout()
            orbs_row.setContentsMargins(0, 0, 0, 0)
            orbs_row.setSpacing(8)
            self.orb_budget = VeyraPerfOrb("DMA", size=52)
            self.orb_gpu = VeyraPerfOrb("GPU", size=52)
            self.orb_cpu = VeyraPerfOrb("CPU", size=52)
            orbs_row.addWidget(self.orb_budget)
            orbs_row.addWidget(self.orb_gpu)
            orbs_row.addWidget(self.orb_cpu)
            tele_row.addLayout(orbs_row, 0)

            self.lbl_tele_cadence = QLabel(
                "CPU: —% · GPU: —%", self.telemetry_strip)
            self.lbl_tele_cadence.setVisible(False)
            deck_layout.addLayout(tele_row)
            stage_col.addWidget(self.telemetry_strip, 0)
            workspace_layout.addWidget(self.center_stage_widget, 1)

            # -------------------------------------------------------------
            # 2B. Right Inspector Sidebar: Tabbed EverLive Studio Controls
            # -------------------------------------------------------------
            self.right_dock = QWidget()
            self.right_dock.setFixedWidth(ui_theme.INSPECTOR_WIDTH)
            dock_layout = QVBoxLayout(self.right_dock)
            dock_layout.setContentsMargins(0, 0, 0, 0)
            dock_layout.setSpacing(12)

            def _section(title: str, dot_color: str = "#F287A7", right_widget: Optional[QWidget] = None):
                frame = QFrame()
                frame.setObjectName("cardFrame")
                lay = QVBoxLayout(frame)
                lay.setContentsMargins(12, 12, 12, 12)
                lay.setSpacing(10)
                h_head = QHBoxLayout()
                h_head.setContentsMargins(0, 0, 0, 0)
                h_head.setSpacing(6)
                head = QLabel(title)
                head.setObjectName("sectionTitle")
                h_head.addWidget(head, 0, Qt.AlignVCenter)
                h_head.addStretch(1)
                if right_widget is not None:
                    h_head.addWidget(right_widget, 0, Qt.AlignVCenter)
                lay.addLayout(h_head)
                grid = QGridLayout()
                grid.setHorizontalSpacing(10)
                grid.setVerticalSpacing(12)
                lay.addLayout(grid)
                return frame, grid

            def _field(text, tip=""):
                lbl = QLabel(text)
                lbl.setObjectName("fieldLabel")
                if tip:
                    lbl.setToolTip(tip)
                return lbl

            def _hint(text):
                lbl = QLabel(text)
                lbl.setObjectName("hintText")
                lbl.setWordWrap(True)
                return lbl

            def _stacked_field(text, control, tip=""):
                """Label above its input, per the mockup form layout."""
                box = QWidget()
                lay = QVBoxLayout(box)
                lay.setContentsMargins(0, 0, 0, 0)
                lay.setSpacing(8)
                lay.addWidget(_field(text, tip))
                lay.addWidget(control)
                return box

            def _shrinkable(combo):
                """Let a combo follow the inspector width instead of its
                longest item, which otherwise overflows the card."""
                combo.setSizeAdjustPolicy(
                    QComboBox.AdjustToMinimumContentsLengthWithIcon)
                combo.setMinimumContentsLength(8)
                combo.setSizePolicy(
                    QSizePolicy.Ignored, QSizePolicy.Fixed)
                combo.setMinimumWidth(0)
                combo.setFixedHeight(ui_theme.INPUT_H)
                return combo

            def _make_tab_page():
                scroll = QScrollArea()
                scroll.setObjectName("inspectorScroll")
                scroll.setWidgetResizable(True)
                scroll.setFrameShape(QFrame.NoFrame)
                scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
                page = QWidget()
                page_lay = QVBoxLayout(page)
                # Leave room for the 8 px scrollbar so card borders are
                # never painted underneath it.
                page_lay.setContentsMargins(12, 12, 12, 12)
                page_lay.setSpacing(12)
                scroll.setWidget(page)
                return scroll, page_lay

            self._toggle_rows = {}

            def _toggle(checkbox, text: str = "", hint: str = ""):
                """Wrap a QCheckBox in a ToggleRow without changing behavior."""
                # Own the hidden checkbox so it is never a stray top-level
                # window; it must stay enabled-parented for state mirroring.
                checkbox.setParent(self)
                checkbox.hide()
                row = ToggleRow(
                    text or checkbox.text(), hint, checkbox=checkbox)
                self._toggle_rows[checkbox] = row
                return row

            self.inspector_tabbar = SegmentedControl(
                [("Video && Audio", 0), ("Capture", 1), ("System", 2)],
                button_height=36,
                object_name="inspectorTabbar")
            self.inspector_tabbar.setFixedHeight(ui_theme.CONTROL_H)
            dock_layout.addWidget(self.inspector_tabbar)

            self.inspector_tabs = QStackedWidget()
            self.inspector_tabs.setObjectName("inspectorStack")
            dock_layout.addWidget(self.inspector_tabs, 1)

            tab_video, lay_video = _make_tab_page()
            tab_rec, lay_rec = _make_tab_page()
            tab_app, lay_app = _make_tab_page()

            self.inspector_tabs.addWidget(tab_video)
            self.inspector_tabs.addWidget(tab_rec)
            self.inspector_tabs.addWidget(tab_app)
            self.inspector_tabbar.currentIndexChanged.connect(
                self.inspector_tabs.setCurrentIndex)
            # --- Pinned EDID & Handshake footer (always visible) ---
            card_edid, grid_edid = _section("EDID & HANDSHAKE", "#38BDF8")
            card_edid.setObjectName("edidCard")
            self._card_edid = card_edid
            self._grid_edid = grid_edid
            grid_edid.setColumnStretch(0, 1)
            grid_edid.setColumnStretch(1, 1)

            if self.duo_mode:
                edid_mode_text = "Downstream Copy"
            else:
                edid_mode_text = self._edid_profile_label()
            self.lbl_edid_mode = QLabel()
            self.lbl_edid_mode.setObjectName("hintText")
            self.lbl_edid_mode.setTextFormat(Qt.RichText)
            self._set_edid_mode_text(edid_mode_text)
            self.lbl_edid_mode.setWordWrap(True)
            grid_edid.addWidget(self.lbl_edid_mode, 0, 0, 1, 2)

            # DUO bridge/copy buttons: built for every card, shown on the DUO.
            btn_bridge = QPushButton("Bridge EDID")
            btn_bridge.setObjectName("btnGhost")
            btn_bridge.setCursor(Qt.PointingHandCursor)
            btn_bridge.setFixedHeight(ui_theme.BUTTON_H)
            btn_bridge.setToolTip(
                "Advertise the internal 4K60 bridge EDID to HDMI IN 1.")
            btn_bridge.clicked.connect(
                lambda: self._duo_edid_action(True))
            grid_edid.addWidget(btn_bridge, 1, 0)
            self.btn_edid_bridge = btn_bridge

            btn_copy = QPushButton("Copy Downstream EDID")
            btn_copy.setObjectName("btnGhost")
            btn_copy.setCursor(Qt.PointingHandCursor)
            btn_copy.setFixedHeight(ui_theme.BUTTON_H)
            btn_copy.setToolTip(
                "Copy the EDID of the display/AVR connected to HDMI OUT.")
            btn_copy.clicked.connect(
                lambda: self._duo_edid_action(False))
            grid_edid.addWidget(btn_copy, 1, 1)
            self.btn_edid_copy = btn_copy

            btn_hpd = QPushButton("Pulse HPD")
            btn_hpd.setObjectName("btnGhost")
            btn_hpd.setCursor(Qt.PointingHandCursor)
            btn_hpd.setFixedHeight(ui_theme.BUTTON_H)
            # Tooltip, action and grid row follow the card
            # (_apply_card_profile / _on_hpd_clicked).
            btn_hpd.clicked.connect(self._on_hpd_clicked)
            grid_edid.addWidget(btn_hpd, 1, 0, 1, 2)
            self.btn_edid_hpd = btn_hpd

            dock_layout.addWidget(card_edid)

            # =============================================================
            # TAB 1: VIDEO & AUDIO
            # =============================================================
            card_video, grid_video = _section("PICTURE & HDR", "#F287A7")
            grid_video.setColumnStretch(0, 1)

            self.combo_presets = _shrinkable(QComboBox())
            if self.vfio_mode:
                for text, key in EDID_PROFILES:
                    display = ("Default (4K60 / 1440p120)"
                               if key == "default" else text)
                    self.combo_presets.addItem(display, key)
                saved_profile = self.saved_cfg.get("edid_profile", "default")
                custom_path = self.saved_cfg.get("edid_custom_path", "")
                if saved_profile == "custom":
                    if custom_path and os.path.isfile(custom_path):
                        idx = self.combo_presets.findData("custom")
                        if idx >= 0:
                            self.combo_presets.setItemText(
                                idx, f"Custom: {os.path.basename(custom_path)}")
                    else:
                        saved_profile = "default"
                idx = self.combo_presets.findData(saved_profile)
                if idx >= 0:
                    self.combo_presets.setCurrentIndex(idx)
                self._presets_tooltip = (
                    "256-byte EDID advertised to the console. Every profile "
                    "supports L-PCM, Dolby Digital (AC-3) and DTS 5.1; "
                    "compressed audio is decoded automatically. "
                    "Applies live over IPC when the engine is running.")
                self.combo_presets.setToolTip(self._presets_tooltip)
            else:
                self.combo_presets.addItems(list(PRESETS.keys()))
                if "preset" in self.saved_cfg and self.saved_cfg["preset"] in PRESETS:
                    self.combo_presets.setCurrentText(self.saved_cfg["preset"])
            self.combo_presets.activated.connect(self.on_preset_changed)
            self._presets_field = _stacked_field(
                "EDID Profile" if self.vfio_mode else "Format / Preset",
                self.combo_presets)
            grid_video.addWidget(self._presets_field, 0, 0, 1, 2)
            self._update_edid_port_label()

            self.combo_aspect = _shrinkable(QComboBox())
            for text, key in ASPECT_OPTIONS:
                self.combo_aspect.addItem(text, key)
            saved_aspect = self.saved_cfg.get("aspect_ratio", "auto")
            idx = self.combo_aspect.findData(saved_aspect)
            if idx >= 0:
                self.combo_aspect.setCurrentIndex(idx)
            self.combo_aspect.setToolTip(
                "Forwarded as --aspect to gc573_user_streamer unless "
                "'Auto (Native HDMI)' is selected.")
            self.combo_aspect.currentIndexChanged.connect(self.on_aspect_changed)
            grid_video.addWidget(_stacked_field(
                "Aspect Ratio", self.combo_aspect,
                "Overrides SDL2 presentation. Auto follows the native HDMI "
                "timing; 16:9 fixes anamorphic RetroTINK/OSSC modes."),
                1, 0, 1, 2)

            self.combo_scale = _shrinkable(QComboBox())
            for text, key in SCALE_OPTIONS:
                self.combo_scale.addItem(text, key)
            saved_scale = self.saved_cfg.get("scale_mode", "fit")
            scale_idx = self.combo_scale.findData(saved_scale)
            if scale_idx >= 0:
                self.combo_scale.setCurrentIndex(scale_idx)
            self.combo_scale.setToolTip(
                "Forwarded as --scale. Applies live over IPC while the "
                "engine runs. Hotkey 'L' in the gaming window cycles it.")
            self.combo_scale.currentIndexChanged.connect(self.on_scale_changed)
            grid_video.addWidget(_stacked_field(
                "Scaling Mode", self.combo_scale,
                "Integer Scale picks the largest whole multiple of the "
                "source (crisp pixels for retro/720p). Falls back to Aspect "
                "Fit when the monitor is smaller or the content is anamorphic."),
                2, 0, 1, 2)

            self.chk_hdr = QCheckBox("HDR → SDR Tonemapping")
            self.chk_hdr.setChecked(bool(self.saved_cfg.get("hdr_tonemap", True)))
            self.chk_hdr.setToolTip(
                "Maps BT.2020 PQ/HLG signals to BT.709 SDR for both the "
                "gaming window and OBS (Hotkey: H).")
            self.chk_hdr.toggled.connect(self.on_hdr_toggled)
            grid_video.addWidget(_toggle(self.chk_hdr), 3, 0, 1, 2)

            # Hidden state holder; the IPC and settings paths keep using it.
            self.spin_hdr_exposure = QDoubleSpinBox(card_video)
            self.spin_hdr_exposure.setVisible(False)
            self.spin_hdr_exposure.setButtonSymbols(QAbstractSpinBox.NoButtons)
            self.spin_hdr_exposure.setRange(-2.0, 2.0)
            self.spin_hdr_exposure.setSingleStep(0.1)
            self.spin_hdr_exposure.setDecimals(1)
            self.spin_hdr_exposure.setValue(
                float(self.saved_cfg.get("hdr_exposure", 0.0)))
            self.spin_hdr_exposure.setEnabled(self.chk_hdr.isChecked())
            self.spin_hdr_exposure.valueChanged.connect(
                self.on_hdr_exposure_changed)

            self.slider_hdr_exposure = QSlider(Qt.Horizontal)
            self.slider_hdr_exposure.setRange(-20, 20)
            self.slider_hdr_exposure.setValue(
                int(round(self.spin_hdr_exposure.value() * 10)))
            self.slider_hdr_exposure.setEnabled(self.chk_hdr.isChecked())
            self.slider_hdr_exposure.setToolTip(
                "Linear exposure bias for the tonemapper (-2.0 .. +2.0 EV).")

            self.lbl_hdr_exposure = QLabel(
                f"{self.spin_hdr_exposure.value():+.1f} EV")
            self.lbl_hdr_exposure.setObjectName("numericValue")
            self.lbl_hdr_exposure.setFixedWidth(54)
            self.lbl_hdr_exposure.setAlignment(Qt.AlignCenter)

            self.btn_hdr_exp_reset = QPushButton("Reset")
            self.btn_hdr_exp_reset.setObjectName("btnGhost")
            self.btn_hdr_exp_reset.setFixedHeight(ui_theme.BUTTON_H)
            self.btn_hdr_exp_reset.setEnabled(self.chk_hdr.isChecked())
            self.btn_hdr_exp_reset.setToolTip("Reset exposure bias to 0.0 EV")
            self.btn_hdr_exp_reset.clicked.connect(
                lambda: self.spin_hdr_exposure.setValue(0.0))

            exposure_box = QWidget()
            exposure_lay = QVBoxLayout(exposure_box)
            exposure_lay.setContentsMargins(0, 0, 0, 2)
            exposure_lay.setSpacing(6)
            exposure_head = QHBoxLayout()
            exposure_head.setContentsMargins(0, 0, 0, 0)
            exposure_head.setSpacing(8)
            exposure_head.addWidget(_field(
                "Exposure Bias",
                "Linear exposure bias for the tonemapper (-2.0 .. +2.0 EV)."))
            exposure_head.addStretch(1)
            exposure_head.addWidget(self.lbl_hdr_exposure)
            exposure_lay.addLayout(exposure_head)
            exposure_lay.addWidget(self.slider_hdr_exposure)
            self.btn_hdr_exp_reset.setSizePolicy(
                QSizePolicy.Expanding, QSizePolicy.Fixed)
            exposure_lay.addWidget(self.btn_hdr_exp_reset)

            def _on_exposure_slider(value: int):
                self.spin_hdr_exposure.setValue(value / 10.0)

            def _on_exposure_value(value: float):
                if not self.slider_hdr_exposure.isSliderDown():
                    self.slider_hdr_exposure.blockSignals(True)
                    self.slider_hdr_exposure.setValue(int(round(value * 10)))
                    self.slider_hdr_exposure.blockSignals(False)
                self.lbl_hdr_exposure.setText(f"{value:+.1f} EV")

            self.slider_hdr_exposure.valueChanged.connect(_on_exposure_slider)
            self.spin_hdr_exposure.valueChanged.connect(_on_exposure_value)
            grid_video.addWidget(exposure_box, 4, 0, 1, 2)
            self._hdr_exposure_box = exposure_box
            lay_video.addWidget(card_video)

            # --- Frame Generation Card ---
            fg_head_right = QWidget()
            fg_hr_lay = QHBoxLayout(fg_head_right)
            fg_hr_lay.setContentsMargins(0, 0, 0, 0)
            fg_hr_lay.setSpacing(6)
            self.lbl_fg_ready = QLabel("Checking…")
            self.lbl_fg_ready.setObjectName("fieldLabel")
            fg_hr_lay.addWidget(self.lbl_fg_ready, 0, Qt.AlignVCenter)
            self.btn_fg_recheck = IconButton(
                "rotate",
                "Re-scan for the lsfg-vk Vulkan layer and lsfg-vk.dll",
                size=24, icon_size=12)
            self.btn_fg_recheck.setStyleSheet(
                "QPushButton#iconButton {"
                " min-height: 22px; min-width: 22px;"
                " border: 1px solid #3F455C; border-radius: 6px;"
                " background-color: transparent; padding: 0px;"
                "}"
                "QPushButton#iconButton:hover {"
                " background-color: #262A3C; border-color: #3A3E54;"
                "}")
            self.btn_fg_recheck.clicked.connect(self.on_fg_recheck)
            fg_hr_lay.addWidget(self.btn_fg_recheck, 0, Qt.AlignVCenter)

            card_fg, grid_fg = _section(
                "FRAME GENERATION", "#BB96F3", fg_head_right)
            grid_fg.setColumnStretch(0, 1)

            lbl_fg_exp = QLabel("EXPERIMENTAL")
            lbl_fg_exp.setObjectName("fieldLabel")
            lbl_fg_exp.setStyleSheet(
                "color: #BB96F3; font-size: 11px; font-weight: 700;"
                " letter-spacing: 0.6px; margin-top: 0px; margin-bottom: 4px;")
            card_fg.layout().insertWidget(1, lbl_fg_exp)

            self.combo_fg_backend = _shrinkable(QComboBox())
            for text, key in FRAMEGEN_BACKEND_OPTIONS:
                self.combo_fg_backend.addItem(text, key)
            be_idx = self.combo_fg_backend.findData(
                self.saved_cfg.get("fg_backend", "lsfg"))
            if be_idx >= 0:
                self.combo_fg_backend.setCurrentIndex(be_idx)
            self.combo_fg_backend.currentIndexChanged.connect(
                self.on_fg_backend_changed)
            grid_fg.addWidget(_stacked_field(
                "Engine", self.combo_fg_backend,
                "LSFG-VK uses Lossless Scaling (Vulkan); EverLive Native is "
                "a 100% open-source built-in prototype with Static HUD Lock "
                "requiring no Steam or external DLLs."),
                0, 0, 1, 2)

            self._fg_ready_ok = False
            self._refresh_fg_readiness()

            self.combo_framegen = _shrinkable(QComboBox())
            for text, key in FRAMEGEN_OPTIONS:
                self.combo_framegen.addItem(text, key)
            fg_idx = self.combo_framegen.findData(
                self.saved_cfg.get("framegen_mode", "off"))
            if fg_idx >= 0:
                self.combo_framegen.setCurrentIndex(fg_idx)
            self.combo_framegen.setToolTip(
                "Forwarded as --framegen / --fg-mult. Applies live over IPC while the "
                "engine runs (hotkey 'g' cycles it).")
            self.combo_framegen.currentIndexChanged.connect(
                self.on_framegen_changed)
            grid_fg.addWidget(_stacked_field(
                "Multiplier", self.combo_framegen,
                "Frame generation multiplier (Off / 2× / 3× / 4×). Enable '30 FPS Dedup' "
                "below when a console outputs a 30 FPS game in a 60 Hz HDMI container."),
                1, 0, 1, 2)

            h_flow = QHBoxLayout()
            h_flow.setContentsMargins(0, 0, 0, 0)
            h_flow.setSpacing(8)
            self.slider_flow = QSlider(Qt.Horizontal)
            self.slider_flow.setMinimumHeight(20)
            self.slider_flow.setRange(50, 100)
            self.slider_flow.setValue(int(round(
                float(self.saved_cfg.get("fg_flow_scale", 1.0)) * 100)))
            self.slider_flow.setToolTip("LSFGVK_FLOW_SCALE: 50% to 100%.")
            self.slider_flow.valueChanged.connect(self.on_flow_scale_changed)
            self.slider_flow.sliderReleased.connect(
                lambda: save_settings(self._framegen_config()))
            h_flow.addWidget(self.slider_flow, 1)

            self.lbl_flow = QLabel(f"{self.slider_flow.value()}%")
            self.lbl_flow.setObjectName("numericValue")
            self.lbl_flow.setFixedWidth(36)
            h_flow.addWidget(self.lbl_flow, 0, Qt.AlignVCenter)

            flow_box = QWidget()
            flow_lay = QVBoxLayout(flow_box)
            flow_lay.setContentsMargins(0, 0, 0, 0)
            flow_lay.setSpacing(6)
            flow_lay.addWidget(_field(
                "Flow Scale",
                "Optical-flow resolution used by LSFG. 100% is full quality; "
                "50-70% is recommended for 4K captures."))
            flow_lay.addLayout(h_flow)
            grid_fg.addWidget(flow_box, 2, 0, 1, 2)

            self.chk_fg_dedup = QCheckBox("30 FPS Dedup (PS5 / PS4)")
            self.chk_fg_dedup.setChecked(
                bool(self.saved_cfg.get("fg_dedup", False)))
            self.chk_fg_dedup.setToolTip(
                "Phase-locked 2:1 HDMI deduplicator for 30 FPS console games "
                "wrapped in a 60 Hz signal (e.g. 30→60/90/120 FPS).")
            self.chk_fg_dedup.toggled.connect(self.on_fg_dedup_toggled)
            grid_fg.addWidget(_toggle(self.chk_fg_dedup), 3, 0, 1, 2)

            self.chk_fg_perf = QCheckBox("Performance Mode (LSFG)")
            self.chk_fg_perf.setChecked(
                bool(self.saved_cfg.get("fg_perf_mode", False)))
            self.chk_fg_perf.setToolTip(
                "LSFGVK_PERFORMANCE_MODE: less GPU load, at the cost of "
                "interpolation quality (disable for cleaner 3×/4×).")
            self.chk_fg_perf.toggled.connect(self.on_fg_perf_toggled)
            grid_fg.addWidget(_toggle(self.chk_fg_perf), 4, 0, 1, 2)

            self.chk_fg_ll = QCheckBox("Low-Latency Swapchain (2×)")
            self.chk_fg_ll.setChecked(
                bool(self.saved_cfg.get("fg_low_latency", True)))
            self.chk_fg_ll.setToolTip(
                "Sets LSFGVK_PRESERVE_SWAPCHAIN_IMAGE_COUNT=1 at 2× so lsfg-vk "
                "keeps minimal queue depth. Automatically relaxed at 3×/4×.")
            self.chk_fg_ll.toggled.connect(self.on_fg_low_latency_toggled)
            grid_fg.addWidget(_toggle(self.chk_fg_ll), 5, 0, 1, 2)
            self._sync_fg_backend_controls()

            self._refresh_idle_fg_status()
            self._card_fg = card_fg
            if not self.card_profile.supports_framegen:
                card_fg.setVisible(False)
            lay_video.addWidget(card_fg)

            # =============================================================
            # VIDEO & AUDIO TAB: SOUND ROUTING
            # =============================================================
            card_audio, grid_audio = _section("AUDIO", "#38BDF8")
            grid_audio.setColumnStretch(0, 1)

            self.combo_audio_channels = _shrinkable(QComboBox())
            for label, key in AUDIO_CHANNEL_MODES:
                self.combo_audio_channels.addItem(label, key)
            saved_layout = str(self.saved_cfg.get("audio_channels", "auto"))
            idx = self.combo_audio_channels.findData(saved_layout)
            self.combo_audio_channels.setCurrentIndex(idx if idx >= 0 else 0)
            self.combo_audio_channels.currentIndexChanged.connect(
                self.on_audio_channels_changed)
            if self.duo_mode:
                self.combo_audio_channels.setEnabled(False)
                self.combo_audio_channels.setToolTip(
                    "The Live Gamer DUO engine publishes stereo only.")
            elif self.uvc_mode:
                self.combo_audio_channels.setEnabled(False)
                self.combo_audio_channels.setToolTip(
                    "USB/UVC audio routing follows the system PipeWire/ALSA device.")
            grid_audio.addWidget(_stacked_field(
                "Audio Layout", self.combo_audio_channels,
                "Playback channel layout. Auto follows the HDMI source; "
                "5.1/7.1 fall back to an ITU-R BS.775 stereo downmix when "
                "the audio sink cannot open them."), 0, 0, 1, 2)
            lay_video.addWidget(card_audio)

            # --- On-Screen Overlay Card (grouped cleanly!) ---
            card_osd, grid_osd = _section("ON-SCREEN OVERLAY", "#F287A7")
            grid_osd.setColumnStretch(0, 1)

            self.chk_osd = QCheckBox("Show On-Screen Stats")
            self.chk_osd.setToolTip(
                "Live resolution, FPS and audio format drawn by the engine "
                "itself (Hotkeys: Tab / O toggle, P cycles corner).")
            self.chk_osd.setChecked(bool(self.saved_cfg.get("osd_enabled", True)))
            self.chk_osd.toggled.connect(self.on_osd_changed)
            grid_osd.addWidget(_toggle(self.chk_osd), 0, 0, 1, 2)

            self.combo_osd_pos = _shrinkable(QComboBox())
            for text, key in OSD_POSITION_OPTIONS:
                self.combo_osd_pos.addItem(text, key)
            saved_pos = self.saved_cfg.get("osd_position", "top")
            pos_idx = self.combo_osd_pos.findData(saved_pos)
            if pos_idx >= 0:
                self.combo_osd_pos.setCurrentIndex(pos_idx)
            self.combo_osd_pos.setToolTip(
                "Top or bottom edge for the stats HUD and Quick Navigation "
                "bar. Forwarded as --osd-pos.")
            self.combo_osd_pos.setEnabled(self.chk_osd.isChecked())
            self.combo_osd_pos.currentIndexChanged.connect(self.on_osd_pos_changed)
            self.osd_pos_field = _stacked_field(
                "OSD Position", self.combo_osd_pos,
                "Top or bottom screen edge for the OSD bar (Hotkey: P).")
            self.osd_pos_effect = QGraphicsOpacityEffect(self.osd_pos_field)
            self.osd_pos_effect.setOpacity(1.0)
            self.osd_pos_field.setGraphicsEffect(self.osd_pos_effect)
            grid_osd.addWidget(self.osd_pos_field, 1, 0, 1, 2)

            if self.uvc_mode:
                self.chk_osd.blockSignals(True)
                self.chk_osd.setChecked(False)
                self.chk_osd.blockSignals(False)
                self.chk_osd.setEnabled(False)
                self.chk_osd.setToolTip(
                    "Built-in engine stats OSD is available on VFIO PCIe cards.")
                self.combo_osd_pos.setEnabled(False)
                self.combo_osd_pos.setToolTip(
                    "Built-in engine stats OSD is available on VFIO PCIe cards.")

            self.chk_quicknav = QCheckBox("Quick Navigation Bar (Auto-Hide)")
            self.chk_quicknav.setToolTip(
                "Auto-wakes the interactive control bar (HDMI, aspect, "
                "scaling, volume, mute) on mouse movement; it auto-hides "
                "after 3s. Alt+Z toggles it, and a hidden bar stays hidden "
                "until Alt+Z.")
            self.chk_quicknav.setChecked(
                bool(self.saved_cfg.get("quicknav_enabled", True)))
            self.chk_quicknav.toggled.connect(self.on_quicknav_changed)
            grid_osd.addWidget(_toggle(self.chk_quicknav), 2, 0, 1, 2)

            self.chk_quicknav_fs = QCheckBox("Quick Navigation: Fullscreen Only")
            self.chk_quicknav_fs.setToolTip(
                "Only auto-wake the Quick Navigation bar in fullscreen mode. "
                "Alt+Z still forces it in windowed mode.")
            self.chk_quicknav_fs.setChecked(
                bool(self.saved_cfg.get("quicknav_fs_only", False)))
            self.chk_quicknav_fs.setEnabled(self.chk_quicknav.isChecked())
            self.chk_quicknav_fs.toggled.connect(self.on_quicknav_fs_changed)
            grid_osd.addWidget(_toggle(self.chk_quicknav_fs), 3, 0, 1, 2)

            self.chk_streamer = QCheckBox("Clean Feed (Hide All Overlays)")
            self.chk_streamer.setToolTip(
                "Hides mouse cursor, HUD and overlays for clean recordings.")
            self.chk_streamer.setChecked(self.saved_cfg.get("streamer_mode", False))
            self.chk_streamer.toggled.connect(
                lambda val: save_settings({"streamer_mode": val}))
            self.chk_streamer.toggled.connect(
                lambda _val: self._update_osd_controls())
            grid_osd.addWidget(_toggle(self.chk_streamer), 4, 0, 1, 2)
            lay_video.addWidget(card_osd)
            lay_video.addStretch(1)
            self._update_osd_controls()

            # --- Video Recording Card ---
            self.lbl_rec_status = QLabel("Standby")
            self.lbl_rec_status.setObjectName("fieldLabel")
            card_rec, grid_rec = _section(
                "VIDEO RECORDING", "#FF6B7A", self.lbl_rec_status)
            grid_rec.setColumnStretch(0, 2)
            grid_rec.setColumnStretch(1, 1)

            self.combo_rec_fps = _shrinkable(QComboBox())
            for text, key in everlive_recorder.REC_FPS_OPTIONS:
                self.combo_rec_fps.addItem(text, key)
            rfps_idx = self.combo_rec_fps.findData(
                self.saved_cfg.get("rec_fps", "auto"))
            if rfps_idx >= 0:
                self.combo_rec_fps.setCurrentIndex(rfps_idx)
            self.combo_rec_fps.setToolTip(
                "Target output frame rate. 'Auto' matches capture FPS and "
                "scales with active Frame Generation (up to 240 FPS).")
            self.combo_rec_fps.currentIndexChanged.connect(
                lambda *_: save_settings(
                    {"rec_fps": self.combo_rec_fps.currentData() or "auto"}))
            grid_rec.addWidget(_stacked_field(
                "Frame Rate", self.combo_rec_fps), 0, 0)

            self.combo_rec_format = _shrinkable(QComboBox())
            for text, key in everlive_recorder.REC_FORMAT_OPTIONS:
                self.combo_rec_format.addItem(text, key)
            rfmt_idx = self.combo_rec_format.findData(
                self.saved_cfg.get("rec_format", "mkv"))
            if rfmt_idx >= 0:
                self.combo_rec_format.setCurrentIndex(rfmt_idx)
            self.combo_rec_format.setToolTip(
                "MKV survives unexpected power loss; MP4 enables faststart.")
            self.combo_rec_format.currentIndexChanged.connect(
                lambda *_: save_settings(
                    {"rec_format": self.combo_rec_format.currentData() or "mkv"}))
            grid_rec.addWidget(_stacked_field(
                "Format", self.combo_rec_format), 0, 1)

            self.combo_rec_source = _shrinkable(QComboBox())
            for text, key in everlive_recorder.REC_SOURCE_OPTIONS:
                self.combo_rec_source.addItem(text, key)
            rsrc_idx = self.combo_rec_source.findData(
                self.saved_cfg.get("rec_source", "auto"))
            if rsrc_idx >= 0:
                self.combo_rec_source.setCurrentIndex(rsrc_idx)
            self.combo_rec_source.setToolTip(
                "Direct Hardware (SHM) records clean HDMI without OSD; "
                "Gaming Viewport records rendered post-FrameGen output (120-240 FPS).")
            self.combo_rec_source.currentIndexChanged.connect(
                lambda *_: save_settings(
                    {"rec_source": self.combo_rec_source.currentData() or "auto"}))
            grid_rec.addWidget(_stacked_field(
                "Capture Source", self.combo_rec_source), 1, 0, 1, 2)

            self.combo_rec_encoder = _shrinkable(QComboBox())
            for text, key in everlive_recorder.REC_ENCODER_OPTIONS:
                self.combo_rec_encoder.addItem(text, key)
            renc_idx = self.combo_rec_encoder.findData(
                self.saved_cfg.get("rec_encoder", "auto"))
            if renc_idx >= 0:
                self.combo_rec_encoder.setCurrentIndex(renc_idx)
            self.combo_rec_encoder.setToolTip(
                "Video encoder backend. Auto prefers GPU hardware (NVENC / VAAPI) "
                "with CPU fallback.")
            self.combo_rec_encoder.currentIndexChanged.connect(
                lambda *_: save_settings(
                    {"rec_encoder": self.combo_rec_encoder.currentData() or "auto"}))
            grid_rec.addWidget(_stacked_field(
                "Encoder", self.combo_rec_encoder), 2, 0, 1, 2)

            self.combo_rec_audio = _shrinkable(QComboBox())
            for text, key in everlive_recorder.REC_AUDIO_OPTIONS:
                self.combo_rec_audio.addItem(text, key)
            raud_idx = self.combo_rec_audio.findData(
                self.saved_cfg.get("rec_audio", "everlive"))
            if raud_idx >= 0:
                self.combo_rec_audio.setCurrentIndex(raud_idx)
            self.combo_rec_audio.setToolTip(
                "Choose whether recordings capture only isolated EverLive HDMI "
                "audio (no Discord/desktop sounds), full desktop audio, or mute audio.")
            self.combo_rec_audio.currentIndexChanged.connect(
                lambda *_: save_settings(
                    {"rec_audio": self.combo_rec_audio.currentData() or "everlive"}))
            grid_rec.addWidget(_stacked_field(
                "Audio Track", self.combo_rec_audio), 3, 0, 1, 2)

            self.combo_rec_quality = _shrinkable(QComboBox())
            for text, key in everlive_recorder.REC_QUALITY_OPTIONS:
                self.combo_rec_quality.addItem(text, key)
            rqual_idx = self.combo_rec_quality.findData(
                self.saved_cfg.get("rec_quality", "high"))
            if rqual_idx >= 0:
                self.combo_rec_quality.setCurrentIndex(rqual_idx)
            self.combo_rec_quality.setToolTip(
                "Constant quantizer / CRF quality target.")
            self.combo_rec_quality.currentIndexChanged.connect(
                lambda *_: save_settings(
                    {"rec_quality": self.combo_rec_quality.currentData() or "high"}))
            grid_rec.addWidget(_stacked_field(
                "Quality", self.combo_rec_quality), 4, 0, 1, 2)

            rec_dir_box = QWidget()
            rec_dir_lay = QHBoxLayout(rec_dir_box)
            rec_dir_lay.setContentsMargins(0, 0, 0, 0)
            rec_dir_lay.setSpacing(6)
            rec_dir_frame = QFrame()
            rec_dir_frame.setObjectName("sourceGroup")
            rec_dir_frame.setFixedHeight(ui_theme.INPUT_H)
            rec_dir_frame_lay = QHBoxLayout(rec_dir_frame)
            rec_dir_frame_lay.setContentsMargins(10, 0, 10, 0)
            rec_dir_val = (
                self.saved_cfg.get("rec_dir", "").strip()
                or everlive_recorder.default_recording_dir())
            self.edit_rec_dir = QLabel(_format_rec_dir_display(rec_dir_val))
            self.edit_rec_dir.setObjectName("fieldLabel")
            self.edit_rec_dir.setToolTip(rec_dir_val)
            self.edit_rec_dir.setSizePolicy(
                QSizePolicy.Ignored, QSizePolicy.Preferred)
            rec_dir_frame_lay.addWidget(self.edit_rec_dir, 1)
            rec_dir_lay.addWidget(rec_dir_frame, 1)

            btn_rec_browse = QPushButton("Browse…")
            btn_rec_browse.setObjectName("btnGhost")
            btn_rec_browse.setFixedHeight(ui_theme.BUTTON_H)
            btn_rec_browse.setCursor(Qt.PointingHandCursor)
            btn_rec_browse.setToolTip("Choose recording output folder")
            btn_rec_browse.clicked.connect(self._browse_recording_dir)
            rec_dir_lay.addWidget(btn_rec_browse, 0)

            btn_rec_open = IconButton(
                "folder", "Open recordings folder",
                size=ui_theme.BUTTON_H, icon_size=15)
            btn_rec_open.clicked.connect(self._open_recording_dir)
            rec_dir_lay.addWidget(btn_rec_open, 0)

            grid_rec.addWidget(_stacked_field(
                "Output Folder", rec_dir_box), 5, 0, 1, 2)

            lay_rec.addWidget(card_rec)

            # --- Picture-in-Picture (PiP) Overlay Card ---
            pip_cfg = everlive_cam.normalize_settings(self.saved_cfg)
            self._ensure_cam()
            card_pip, grid_pip = _section("PICTURE-IN-PICTURE (PiP)", "#7AB8FF")
            grid_pip.setColumnStretch(0, 1)
            grid_pip.setColumnStretch(1, 1)

            self.chk_pip = QCheckBox("Enable Overlay (PiP)")
            self.chk_pip.setToolTip(
                "Shows a webcam or 2nd capture source in a corner of the main card's picture "
                "and adds it to recordings. OBS can use it too: "
                f"LD_PRELOAD={everlive_cam.CAM_PRELOAD}, {everlive_cam.CAM_NODE}.")
            self.chk_pip.setChecked(bool(pip_cfg["pip_enabled"]))
            self.chk_pip.toggled.connect(self.on_pip_toggled)
            grid_pip.addWidget(_toggle(self.chk_pip), 0, 0, 1, 2)

            self.combo_pip_device = _shrinkable(QComboBox())
            self.combo_pip_device.addItem("First available capture device", "")
            for path, name in everlive_cam.list_webcams(self._cam.camd):
                self.combo_pip_device.addItem(f"{name} ({path})", path)
            dev_idx = self.combo_pip_device.findData(pip_cfg["pip_device"])
            if dev_idx < 0 and pip_cfg["pip_device"]:
                self.combo_pip_device.addItem(
                    pip_cfg["pip_device"], pip_cfg["pip_device"])
                dev_idx = self.combo_pip_device.count() - 1
            self.combo_pip_device.setCurrentIndex(max(0, dev_idx))
            self.combo_pip_device.currentIndexChanged.connect(
                lambda *_: self._pip_setting_changed(
                    {"pip_device": self.combo_pip_device.currentData() or ""},
                    restart=True))
            grid_pip.addWidget(_stacked_field(
                "Overlay Source", self.combo_pip_device), 1, 0, 1, 2)

            self.combo_pip_mode = _shrinkable(QComboBox())
            for text, key in everlive_cam.PIP_MODE_OPTIONS:
                self.combo_pip_mode.addItem(text, key)
            mode_idx = self.combo_pip_mode.findData(pip_cfg["pip_mode"])
            self.combo_pip_mode.setCurrentIndex(max(0, mode_idx))
            self.combo_pip_mode.setToolTip(
                "Overlay capture mode. 720p is plenty for a corner overlay; "
                "60 FPS halves the capture frame delay.")
            self.combo_pip_mode.currentIndexChanged.connect(
                lambda *_: self._pip_setting_changed(
                    {"pip_mode": self.combo_pip_mode.currentData()},
                    restart=True))
            grid_pip.addWidget(_stacked_field(
                "Mode", self.combo_pip_mode), 2, 0, 1, 2)

            self.combo_pip_corner = _shrinkable(QComboBox())
            for text, key in everlive_cam.PIP_CORNER_OPTIONS:
                self.combo_pip_corner.addItem(text, key)
            self.combo_pip_corner.setCurrentIndex(
                max(0, self.combo_pip_corner.findData(pip_cfg["pip_corner"])))
            self.combo_pip_corner.currentIndexChanged.connect(
                lambda *_: self._pip_setting_changed(
                    {"pip_corner": self.combo_pip_corner.currentData()}))
            grid_pip.addWidget(_stacked_field(
                "Corner", self.combo_pip_corner), 3, 0)

            self.combo_pip_size = _shrinkable(QComboBox())
            for text, key in everlive_cam.PIP_SIZE_OPTIONS:
                self.combo_pip_size.addItem(text, key)
            size_idx = self.combo_pip_size.findData(pip_cfg["pip_size"])
            self.combo_pip_size.setCurrentIndex(
                size_idx if size_idx >= 0 else 1)
            self.combo_pip_size.currentIndexChanged.connect(
                lambda *_: self._pip_setting_changed(
                    {"pip_size": self.combo_pip_size.currentData()}))
            grid_pip.addWidget(_stacked_field(
                "Size", self.combo_pip_size), 3, 1)

            if not self._cam.available:
                for w in (self.chk_pip, self.combo_pip_device,
                          self.combo_pip_mode, self.btn_pip):
                    w.setEnabled(False)
                self.chk_pip.setToolTip(
                    "PiP overlay bridge not built: run 'make cam'.")
                self.btn_pip.setToolTip(
                    "PiP overlay bridge not built: run 'make cam'.")

            lay_rec.addWidget(card_pip)

            # --- Device Hardware Controls Card ---
            self.lbl_cam_ctrl_status = QLabel("")
            self.lbl_cam_ctrl_status.setObjectName("fieldLabel")
            card_cam_ctrl, grid_cam_ctrl = _section(
                "DEVICE CONTROLS", "#F472B6", self.lbl_cam_ctrl_status)
            grid_cam_ctrl.setColumnStretch(0, 1)
            grid_cam_ctrl.setColumnStretch(1, 1)
            self._card_cam_ctrl = card_cam_ctrl
            self._grid_cam_ctrl = grid_cam_ctrl

            cam_ctrl_bar = QHBoxLayout()
            cam_ctrl_bar.setContentsMargins(0, 0, 0, 4)
            self.btn_cam_reset = QPushButton("Reset Defaults")
            self.btn_cam_reset.setObjectName("btnGhost")
            self.btn_cam_reset.setFixedHeight(ui_theme.BUTTON_H)
            self.btn_cam_reset.setCursor(Qt.PointingHandCursor)
            self.btn_cam_reset.setToolTip(
                "Reset all hardware settings to factory defaults")
            self.btn_cam_reset.clicked.connect(self._reset_camera_controls)
            cam_ctrl_bar.addWidget(self.btn_cam_reset, 1)

            self.btn_cam_refresh = IconButton(
                "rotate", "Reload hardware controls from device",
                size=ui_theme.BUTTON_H, icon_size=15)
            self.btn_cam_refresh.clicked.connect(self._refresh_camera_controls)
            cam_ctrl_bar.addWidget(self.btn_cam_refresh, 0)
            grid_cam_ctrl.addLayout(cam_ctrl_bar, 0, 0, 1, 2)

            self._cam_ctrl_widgets = {}
            self._cam_ctrl_container = QWidget()
            self._cam_ctrl_layout = QGridLayout(self._cam_ctrl_container)
            self._cam_ctrl_layout.setContentsMargins(0, 0, 0, 0)
            self._cam_ctrl_layout.setSpacing(6)
            self._cam_ctrl_layout.setColumnStretch(0, 1)
            self._cam_ctrl_layout.setColumnStretch(1, 1)
            grid_cam_ctrl.addWidget(self._cam_ctrl_container, 1, 0, 1, 2)

            lay_rec.addWidget(card_cam_ctrl)
            self._refresh_camera_controls()

            # --- OBS & Capture Routing Card ---
            card_obs, grid_obs = _section("OBS & CAPTURE ROUTING", "#5ED4B2")
            grid_obs.setColumnStretch(0, 1)

            self.chk_audio_tap = QCheckBox("Virtual Audio Track (PipeWire)")
            self.chk_audio_tap.setToolTip(
                "Publishes the mixed HDMI audio (L-PCM and decoded AC-3/DTS) "
                f"to the PipeWire sink '{AUDIO_TAP_NODE}'. In OBS add "
                "'Monitor of EverLive Capture Audio' as its own audio source.")
            self.chk_audio_tap.setChecked(bool(self.saved_cfg.get("audio_tap", False)))
            self.chk_audio_tap.toggled.connect(
                lambda val: save_settings({"audio_tap": val}))
            if not self.vfio_mode:
                self.chk_audio_tap.setChecked(False)
                self.chk_audio_tap.setEnabled(False)
            grid_obs.addWidget(_toggle(self.chk_audio_tap), 0, 0)

            has_loopback_node = os.path.exists(V4L2_DEFAULT_DEVICE)
            self.chk_v4l2 = QCheckBox("Legacy V4L2 Loopback")
            self.chk_v4l2.setChecked(bool(self.saved_cfg.get("v4l2_loopback", False) and has_loopback_node))
            self.chk_v4l2.toggled.connect(self.on_v4l2_toggled)
            if self.vfio_mode:
                if has_loopback_node:
                    self.chk_v4l2.setToolTip(
                        "Optional system-wide v4l2loopback node (/dev/video10) for OBS/Discord/browsers. "
                        "Module-free PipeWire & SHM (libgc573-v4l2.so) are always active.")
                else:
                    self.chk_v4l2.setEnabled(False)
                    self.chk_v4l2.setToolTip(
                        f"{V4L2_DEFAULT_DEVICE} is not loaded (module-free mode). "
                        "Use PipeWire capture or LD_PRELOAD=libgc573-v4l2.so for OBS/Discord.")
            else:
                self.chk_v4l2.setChecked(False)
                self.chk_v4l2.setEnabled(False)
                self.chk_v4l2.setToolTip(
                    "Virtual camera output requires the userspace VFIO engine.")
            row_v4l2 = _toggle(self.chk_v4l2)
            v4l2_row = QHBoxLayout()
            v4l2_row.setContentsMargins(0, 0, 0, 0)
            v4l2_row.setSpacing(6)
            v4l2_row.addWidget(row_v4l2, 1)
            lbl_v4l2_obsolete = QLabel("Legacy")
            lbl_v4l2_obsolete.setObjectName("legacyBadge")
            lbl_v4l2_obsolete.setFixedHeight(18)
            lbl_v4l2_obsolete.setToolTip(
                "PipeWire and the LD_PRELOAD shim cover OBS, Discord and "
                "browsers without a kernel module.")
            v4l2_row.addWidget(lbl_v4l2_obsolete, 0, Qt.AlignVCenter)
            grid_obs.addLayout(v4l2_row, 1, 0)
            note = QLabel()
            note.setObjectName("hintText")
            note.setWordWrap(True)
            note.setText(
                "GC553G2 exposes a native USB UVC camera — add its "
                "/dev/videoX device directly in OBS / Discord (no "
                "v4l2loopback or LD_PRELOAD needed)."
                if self.uvc_mode else
                "PipeWire video & DMA-BUF Zero-Copy are always active "
                "without kernel modules.")
            grid_obs.addWidget(note, 2, 0)
            lay_rec.addWidget(card_obs)
            lay_rec.addStretch(1)

            # =============================================================
            # SYSTEM TAB: HARDWARE, APP BEHAVIOR & KEYBOARD SHORTCUTS
            # =============================================================
            card_hw, grid_hw = _section("HARDWARE RGB LED", "#F287A7")
            grid_hw.setColumnStretch(0, 1)
            self._card_led = card_hw

            self.combo_led = _shrinkable(QComboBox())
            self.combo_led.setIconSize(QSize(14, 14))
            for name, val in LED_MODES.items():
                icon = create_led_icon(val)
                if icon:
                    self.combo_led.addItem(icon, name, val)
                else:
                    self.combo_led.addItem(name, val)

            saved_led = self.saved_cfg.get("led_mode", None)
            if self.vfio_mode:
                led_mode = saved_led if isinstance(saved_led, int) and 0 <= saved_led <= 9 else 4
                idx = self.combo_led.findData(led_mode)
                if idx >= 0:
                    self.combo_led.setCurrentIndex(idx)
                will_autostart = self._headless_start or self.saved_cfg.get("autostart", False) or getattr(self.streamer, "_cli_autostart", False)
                if isinstance(saved_led, int) and 0 <= saved_led <= 9 and not will_autostart:
                    if self.duo_mode:
                        if _vfio_streamer_running():
                            set_gc570d_led(saved_led)
                    elif self.model == MODEL_GC573:
                        # everlive_userd is the GC573's standby daemon; other
                        # cards must not start it.
                        set_vfio_led(saved_led)
            else:
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
            grid_hw.addWidget(_stacked_field(
                "Card RGB LED", self.combo_led), 0, 0, 1, 2)
            if self.uvc_mode:
                card_hw.setVisible(False)
            lay_app.addWidget(card_hw)

            if self.uvc_mode and _gc553g2_control is not None:
                card_uvc, grid_uvc = _section("GC553G2 · USB / HDMI 2.1", "#38BDF8")
                grid_uvc.setColumnStretch(0, 1)

                self.lbl_gc553_status = QLabel("USB UVC (07ca:2553) · Click Refresh for AT/HDMI status")
                self.lbl_gc553_status.setObjectName("hintText")
                self.lbl_gc553_status.setWordWrap(True)
                grid_uvc.addWidget(self.lbl_gc553_status, 0, 0, 1, 2)

                btn_uvc_row = QHBoxLayout()
                btn_uvc_row.setContentsMargins(0, 0, 0, 0)
                btn_uvc_row.setSpacing(6)
                self.btn_gc553_refresh = QPushButton("Refresh Status")
                self.btn_gc553_refresh.setObjectName("btnGhost")
                self.btn_gc553_refresh.setFixedHeight(ui_theme.BUTTON_H)
                self.btn_gc553_refresh.clicked.connect(self._refresh_gc553_status)
                btn_uvc_row.addWidget(self.btn_gc553_refresh)

                self.btn_gc553_144 = QPushButton("Enable 120/144 Hz MJPEG")
                self.btn_gc553_144.setObjectName("btnGhost")
                self.btn_gc553_144.setFixedHeight(ui_theme.BUTTON_H)
                self.btn_gc553_144.setToolTip(
                    "Switches GC553G2 USB descriptor index to expose 4K120/144 MJPEG modes.")
                self.btn_gc553_144.clicked.connect(self._on_gc553_enable_high_fps)
                btn_uvc_row.addWidget(self.btn_gc553_144)

                self.btn_gc553_60 = QPushButton("Restore 60 Hz")
                self.btn_gc553_60.setObjectName("btnGhost")
                self.btn_gc553_60.setFixedHeight(ui_theme.BUTTON_H)
                self.btn_gc553_60.setToolTip(
                    "Restores GC553G2 default USB descriptor index 0 (60 Hz).")
                self.btn_gc553_60.clicked.connect(self._on_gc553_restore_60hz)
                btn_uvc_row.addWidget(self.btn_gc553_60)
                grid_uvc.addLayout(btn_uvc_row, 1, 0, 1, 2)

                self.combo_gc553_preset = _shrinkable(QComboBox())
                for label, key in [("HDMI 2.1 (4K144)", "2.1-144"), ("HDMI 2.1 (4K120)", "2.1-120"), ("HDMI 2.0", "2.0")]:
                    self.combo_gc553_preset.addItem(label, key)
                self.combo_gc553_preset.activated.connect(self._on_gc553_hdmi_preset)
                grid_uvc.addWidget(_stacked_field(
                    "HDMI Preset", self.combo_gc553_preset), 2, 0, 1, 2)

                self.combo_gc553_edid = _shrinkable(QComboBox())
                for label, key in [("Merge", "merge"), ("Copy", "copy"), ("Internal", "internal")]:
                    self.combo_gc553_edid.addItem(label, key)
                self.combo_gc553_edid.activated.connect(self._on_gc553_edid_mode)
                grid_uvc.addWidget(_stacked_field(
                    "EDID Mode", self.combo_gc553_edid), 3, 0, 1, 2)

                self.chk_gc553_hdcp = QCheckBox("HDCP Detection")
                self.chk_gc553_hdcp.toggled.connect(self._on_gc553_hdcp)
                grid_uvc.addWidget(_toggle(self.chk_gc553_hdcp), 4, 0, 1, 2)

                self.chk_gc553_hdr_sdr = QCheckBox("HW HDR-to-SDR")
                self.chk_gc553_hdr_sdr.toggled.connect(self._on_gc553_hdr_to_sdr)
                grid_uvc.addWidget(_toggle(self.chk_gc553_hdr_sdr), 5, 0, 1, 2)

                self.combo_gc553_rgb = _shrinkable(QComboBox())
                for label, key in [
                    ("Autonomous", "autonomous"),
                    ("EverLive Pink", "#F287A7"),
                    ("Cyber Cyan", "#38BDF8"),
                    ("Neon Violet", "#BB96F3"),
                    ("Off", "#000000"),
                ]:
                    self.combo_gc553_rgb.addItem(label, key)
                self.combo_gc553_rgb.activated.connect(self._on_gc553_rgb)
                grid_uvc.addWidget(_stacked_field(
                    "LampArray RGB", self.combo_gc553_rgb), 6, 0, 1, 2)

                lay_app.addWidget(card_uvc)
                QTimer.singleShot(250, lambda: self._refresh_gc553_status(include_at=False))

            card_app, grid_app = _section("WINDOW & STARTUP", "#38BDF8")
            grid_app.setColumnStretch(0, 1)

            self.chk_vsync = QCheckBox("VSync (Display Pacing)")
            self.chk_vsync.setChecked(self.saved_cfg.get("vsync", self.enable_vsync))
            self.chk_vsync.toggled.connect(lambda val: save_settings({"vsync": val}))
            grid_app.addWidget(_toggle(self.chk_vsync), 0, 0)

            self.chk_fullscreen = QCheckBox("Fullscreen on Launch")
            self.chk_fullscreen.setChecked(self.saved_cfg.get("fullscreen", False))
            self.chk_fullscreen.toggled.connect(lambda val: save_settings({"fullscreen": val}))
            grid_app.addWidget(_toggle(self.chk_fullscreen), 1, 0)

            self.chk_autostart = QCheckBox("Auto-start Stream on Launch")
            self.chk_autostart.setChecked(self.saved_cfg.get("autostart", False))
            self.chk_autostart.toggled.connect(lambda val: save_settings({"autostart": val}))
            grid_app.addWidget(_toggle(self.chk_autostart), 2, 0)

            self.chk_tray = QCheckBox("Close to System Tray")
            self.chk_tray.setToolTip(
                "Closing with [X] hides the Console Hub window into the system "
                "tray so it does not clutter Alt-Tab.")
            self.chk_tray.setChecked(bool(self.saved_cfg.get("minimize_to_tray", True)))
            self.chk_tray.toggled.connect(lambda val: save_settings({"minimize_to_tray": val}))
            grid_app.addWidget(_toggle(self.chk_tray), 3, 0)

            if self.vfio_mode:
                self.chk_fullscreen.setToolTip(
                    "Starts in Cinema Fullscreen (embedded) or SDL Fullscreen (pop-out). "
                    "Press F / F11 or double-click the video stage to toggle at runtime.")
                self.chk_vsync.setToolTip(
                    "Paces SDL2 presentation to the display refresh rate.")
                self.chk_streamer.setToolTip(
                    "Clean feed: hides the on-screen stats overlay and the "
                    "mouse cursor in the game window (Tab/'o' toggles at runtime).")
            lay_app.addWidget(card_app)

            # --- Reversible per-app LD_PRELOAD camera shortcuts -----------
            card_camera, grid_camera = _section(
                "APP CAMERA SHORTCUTS", "#5ED4B2")
            grid_camera.setColumnStretch(0, 1)

            self.app_camera_note = _hint(
                "Create separate per-app launchers for OBS, Discord and "
                "browsers, or run the installer with --with-preload. Each "
                "shortcut sets LD_PRELOAD for one process; session-wide "
                "LD_PRELOAD is never used.")
            grid_camera.addWidget(self.app_camera_note, 0, 0)

            self.btn_app_shortcuts = QPushButton("App Camera Shortcuts…")
            self.btn_app_shortcuts.setObjectName("btnGhost")
            self.btn_app_shortcuts.setCursor(Qt.PointingHandCursor)
            self.btn_app_shortcuts.setFixedHeight(ui_theme.BUTTON_H)
            self.btn_app_shortcuts.setSizePolicy(
                QSizePolicy.Expanding, QSizePolicy.Fixed)
            self.btn_app_shortcuts.setToolTip(
                "Create reversible OBS/Discord/browser launchers that use the "
                "module-free virtual camera.")
            self.btn_app_shortcuts.clicked.connect(
                self._open_app_shortcuts)
            grid_camera.addWidget(self.btn_app_shortcuts, 1, 0)
            if self.uvc_mode:
                card_camera.setVisible(False)
            lay_app.addWidget(card_camera)

            # --- Background capture service ---------------------------------
            card_integrations, grid_int = _section(
                "BACKGROUND SERVICE", "#5ED4B2")
            grid_int.setColumnStretch(0, 1)

            self.lbl_background_service = QLabel(
                "Checking background service…")
            self.lbl_background_service.setObjectName("hintText")
            self.lbl_background_service.setWordWrap(True)
            grid_int.addWidget(self.lbl_background_service, 0, 0, 1, 2)

            service_grid = QGridLayout()
            service_grid.setHorizontalSpacing(6)
            service_grid.setVerticalSpacing(6)

            def _service_button(text):
                button = QPushButton(text)
                button.setObjectName("btnGhost")
                button.setCursor(Qt.PointingHandCursor)
                button.setFixedHeight(ui_theme.BUTTON_H)
                button.setSizePolicy(
                    QSizePolicy.Expanding, QSizePolicy.Fixed)
                return button

            self.btn_service_install = _service_button("Install")
            self.btn_service_install.setToolTip(
                "Install a per-user systemd unit when the package did not "
                "provide one.")
            self.btn_service_install.clicked.connect(self._service_install)
            service_grid.addWidget(self.btn_service_install, 0, 0)

            self.btn_service_enable = _service_button("Enable at Login")
            self.btn_service_enable.setToolTip(
                "Enable the background capture service and start it now.")
            self.btn_service_enable.clicked.connect(self._service_enable)
            service_grid.addWidget(self.btn_service_enable, 0, 1)

            self.btn_service_start = _service_button("Start")
            self.btn_service_start.clicked.connect(self._service_start)
            service_grid.addWidget(self.btn_service_start, 1, 0)

            self.btn_service_stop = _service_button("Stop")
            self.btn_service_stop.clicked.connect(self._service_stop)
            service_grid.addWidget(self.btn_service_stop, 1, 1)

            self.btn_service_disable = _service_button("Disable")
            self.btn_service_disable.clicked.connect(self._service_disable)
            service_grid.addWidget(self.btn_service_disable, 2, 0)

            self.btn_service_uninstall = _service_button("Uninstall")
            self.btn_service_uninstall.setToolTip(
                "Remove a user-installed unit. Package-owned units are left "
                "to the package manager.")
            self.btn_service_uninstall.clicked.connect(
                self._service_uninstall)
            service_grid.addWidget(self.btn_service_uninstall, 2, 1)

            self.btn_service_logs = _service_button("View Service Logs")
            self.btn_service_logs.clicked.connect(self._service_logs)
            service_grid.addWidget(self.btn_service_logs, 3, 0, 1, 2)
            grid_int.addLayout(service_grid, 1, 0, 1, 2)

            self.chk_service_v4l2 = QCheckBox(
                f"Service: publish to {V4L2_DEFAULT_DEVICE}")
            self.chk_service_v4l2.setToolTip(
                "Pass --v4l2 to the background engine when the v4l2loopback "
                "node is available.")
            self._service_v4l2_row = _toggle(self.chk_service_v4l2)
            grid_int.addWidget(self._service_v4l2_row, 2, 0, 1, 2)

            self.chk_service_audio_tap = QCheckBox(
                "Service: virtual audio track")
            self.chk_service_audio_tap.setToolTip(
                f"Publish clean HDMI audio to the '{AUDIO_TAP_NODE}' "
                "PipeWire source for OBS.")
            grid_int.addWidget(_toggle(self.chk_service_audio_tap), 3, 0, 1, 2)
            # Shown for the GC573 only (_apply_card_profile).

            self.chk_service_v4l2.toggled.connect(
                lambda _checked: self._service_config_changed())
            self.chk_service_audio_tap.toggled.connect(
                lambda _checked: self._service_config_changed())

            if self.uvc_mode:
                card_integrations.setVisible(False)
            lay_app.addWidget(card_integrations)

            card_diag, grid_diag = _section("DIAGNOSTICS & HELP", "#BB96F3")
            grid_diag.setColumnStretch(0, 1)

            self.btn_shortcuts = QPushButton("Keyboard Shortcuts")
            self.btn_shortcuts.setObjectName("btnGhost")
            self.btn_shortcuts.setFixedHeight(ui_theme.BUTTON_H)
            self.btn_shortcuts.setCursor(Qt.PointingHandCursor)
            self.btn_shortcuts.clicked.connect(self._open_shortcuts_dialog)
            grid_diag.addWidget(self.btn_shortcuts, 0, 0)

            self.btn_diag_report = QPushButton("Copy Diagnostic Report")
            self.btn_diag_report.setObjectName("btnGhost")
            self.btn_diag_report.setFixedHeight(ui_theme.BUTTON_H)
            self.btn_diag_report.setCursor(Qt.PointingHandCursor)
            self.btn_diag_report.setToolTip(
                "Copy engine, signal, routing and service details to the "
                "clipboard for a bug report.")
            self.btn_diag_report.clicked.connect(self._copy_diagnostic_report)
            grid_diag.addWidget(self.btn_diag_report, 1, 0)
            lay_app.addWidget(card_diag)
            lay_app.addStretch(1)

            workspace_layout.addWidget(self.right_dock, 0)
            self.content_layout.addWidget(self.workspace_widget, 1)

            # =============================================================
            # 3. Bottom Audio & Capture Bar
            # =============================================================
            self.bottom_box = QFrame()
            self.bottom_box.setObjectName("bottomBar")
            self.bottom_box.setFixedHeight(ui_theme.BOTTOM_BAR_HEIGHT)
            b_layout = QHBoxLayout(self.bottom_box)
            b_layout.setContentsMargins(16, 10, 16, 10)
            b_layout.setSpacing(10)

            initial_vol = int(self.saved_cfg.get("volume", 1.0) * 100)

            self.btn_mute = IconButton(
                "volume", "Mute HDMI audio", size=ui_theme.BUTTON_H,
                icon_size=16)
            self.btn_mute.clicked.connect(self.toggle_mute)
            b_layout.addWidget(self.btn_mute, 0, Qt.AlignVCenter)

            self.slider_vol = QSlider(Qt.Horizontal)
            self.slider_vol.setRange(0, 150)
            self.slider_vol.setFixedWidth(140)
            self.slider_vol.setToolTip(
                "Volume 0-150%. Note: Set console audio to 2-channel Linear "
                "PCM (Stereo) to avoid buffer underruns.")
            self.slider_vol.setValue(initial_vol)
            self.slider_vol.valueChanged.connect(self.on_volume_changed)
            b_layout.addWidget(self.slider_vol, 0, Qt.AlignVCenter)

            self.lbl_vol = QLabel(f"{initial_vol}%")
            self.lbl_vol.setFixedWidth(42)
            self.lbl_vol.setAlignment(Qt.AlignCenter)
            self.lbl_vol.setObjectName("numericValue")
            b_layout.addWidget(self.lbl_vol, 0, Qt.AlignVCenter)

            if self.vfio_mode:
                self.btn_mute.setToolTip(
                    "Mutes the SDL2 HDMI audio stream via PipeWire/PulseAudio.")
                self.slider_vol.setToolTip(
                    "Sets the PipeWire volume of the userspace SDL2 HDMI audio stream.")

            self.lbl_audio_fmt = StatusChip("No Audio", "idle")
            self.lbl_audio_fmt.setMinimumWidth(124)
            self.lbl_audio_fmt.setToolTip(
                "Audio format detected by the userspace engine from the "
                "IT6805 / SDL2 stream log.")
            b_layout.addWidget(self.lbl_audio_fmt, 0, Qt.AlignVCenter)

            self.chip_bar_capture = StatusChip("Standby • No Signal", "idle")
            self.chip_bar_capture.setVisible(True)
            b_layout.addWidget(self.chip_bar_capture, 0, Qt.AlignVCenter)

            self.chip_bar_record = StatusChip("Recording", "danger")
            self.chip_bar_record.setVisible(False)
            b_layout.addWidget(self.chip_bar_record, 0, Qt.AlignVCenter)
            self._sync_recording_ui()

            # Device route stays available through the bar tooltip; the mockup
            # keeps the bar free of long diagnostic strings.
            if self.vfio_mode:
                self.lbl_vdev = QLabel("", self.bottom_box)
                self._update_vdev_label()
                hints_text = "F / F11 Fullscreen · Tab OSD · H HDR · P Corner · M Mute"
            else:
                if self.streamer.pw_node:
                    audio_str = f"PipeWire ({self.streamer.pw_node})"
                else:
                    audio_str = f"ALSA ({self.streamer.adev})"
                self.lbl_vdev = QLabel(
                    f"V4L2 {self.streamer.vdev} · Audio {audio_str}",
                    self.bottom_box)
                hints_text = "F / F11 Fullscreen · Tab OSD · M Mute · H Re-sync"
            self.lbl_vdev.setObjectName("footerText")
            self.lbl_vdev.setVisible(False)

            self.lbl_hints = QLabel(hints_text, self.bottom_box)
            self.lbl_hints.setObjectName("footerText")
            self.lbl_hints.setVisible(False)

            b_layout.addStretch(1)

            self.chk_popout = IconButton(
                "external", "Pop-out video window", checkable=True,
                size=ui_theme.BUTTON_H, icon_size=15)
            self.chk_popout.setToolTip(
                "Pop the video out into a standalone SDL window. "
                "Unchecked keeps the zero-latency embedded surface.")
            self.chk_popout.setChecked(
                bool(self.saved_cfg.get("popout_window", False)))
            self.chk_popout.toggled.connect(self.on_popout_toggled)
            b_layout.addWidget(self.chk_popout, 0, Qt.AlignVCenter)

            self.btn_cinema_fs = IconButton(
                "maximize", "Cinema Fullscreen (F / F11)",
                size=ui_theme.BUTTON_H, icon_size=15)
            self.btn_cinema_fs.setToolTip(
                "Expand the embedded video stage to fullscreen "
                "(F / F11 / double-click · Esc exits)")
            self.btn_cinema_fs.clicked.connect(self.toggle_cinema_fullscreen)
            b_layout.addWidget(self.btn_cinema_fs, 0, Qt.AlignVCenter)

            if hasattr(self, "lbl_vdev"):
                self.bottom_box.setToolTip(self.lbl_vdev.text())
            self.root_layout.addWidget(self.bottom_box, 0)

            # Hotkey shortcuts inside the Hub window
            self._init_shortcuts()

            # Apply the persisted shell mode (Simple by default).
            self._apply_ui_mode(self.ui_mode, persist=False)

            # Initial status poll
            self.update_status()

            # System Tray Integration
            self._init_tray()

        def _init_shortcuts(self):
            win_ctx = Qt.ShortcutContext.ApplicationShortcut
            for seq in ("F11", "F"):
                sc = QShortcut(QKeySequence(seq), self)
                sc.setContext(win_ctx)
                sc.activated.connect(self.toggle_cinema_fullscreen)
            sc_esc = QShortcut(QKeySequence("Escape"), self)
            sc_esc.setContext(win_ctx)
            sc_esc.activated.connect(self._exit_cinema_fullscreen)
            sc_mute = QShortcut(QKeySequence("M"), self)
            sc_mute.setContext(win_ctx)
            sc_mute.activated.connect(self.toggle_mute)
            sc_osd = QShortcut(QKeySequence("Tab"), self)
            sc_osd.setContext(win_ctx)
            sc_osd.activated.connect(
                lambda: self.chk_osd.setChecked(not self.chk_osd.isChecked()))
            sc_hdr = QShortcut(QKeySequence("H"), self)
            sc_hdr.setContext(win_ctx)
            sc_hdr.activated.connect(
                lambda: self.chk_hdr.setChecked(not self.chk_hdr.isChecked()))
            sc_fg = QShortcut(QKeySequence("G"), self)
            sc_fg.setContext(win_ctx)
            sc_fg.activated.connect(self._cycle_framegen)
            sc_mode = QShortcut(QKeySequence("Ctrl+Shift+A"), self)
            sc_mode.setContext(win_ctx)
            sc_mode.activated.connect(self._toggle_ui_mode)
            sc_dock = QShortcut(QKeySequence("T"), self)
            sc_dock.setContext(win_ctx)
            sc_dock.activated.connect(self._remote_dock)
            sc_rec = QShortcut(QKeySequence("Ctrl+R"), self)
            sc_rec.setContext(win_ctx)
            sc_rec.activated.connect(self.toggle_recording)
            sc_osd_pos = QShortcut(QKeySequence("P"), self)
            sc_osd_pos.setContext(win_ctx)
            sc_osd_pos.activated.connect(self._cycle_osd_position)
            sc_quicknav = QShortcut(QKeySequence("Alt+Z"), self)
            sc_quicknav.setContext(win_ctx)
            sc_quicknav.activated.connect(self._toggle_quicknav)
            sc_aspect = QShortcut(QKeySequence("A"), self)
            sc_aspect.setContext(win_ctx)
            sc_aspect.activated.connect(self._cycle_aspect)
            for seq in ("L", "1"):
                sc_scale = QShortcut(QKeySequence(seq), self)
                sc_scale.setContext(win_ctx)
                sc_scale.activated.connect(self._cycle_scale)
            sc_stats = QShortcut(QKeySequence("I"), self)
            sc_stats.setContext(win_ctx)
            sc_stats.activated.connect(
                lambda: self.chk_osd.setChecked(not self.chk_osd.isChecked()))
            for seq in ("+", "="):
                sc_vup = QShortcut(QKeySequence(seq), self)
                sc_vup.setContext(win_ctx)
                sc_vup.activated.connect(lambda: self._step_volume(5))
            sc_vdown = QShortcut(QKeySequence("-"), self)
            sc_vdown.setContext(win_ctx)
            sc_vdown.activated.connect(lambda: self._step_volume(-5))

        def _cycle_osd_position(self):
            if not hasattr(self, "combo_osd_pos"):
                return
            count = self.combo_osd_pos.count()
            if count <= 1:
                return
            self.combo_osd_pos.setCurrentIndex(
                (self.combo_osd_pos.currentIndex() + 1) % count)

        def _toggle_quicknav(self):
            if self._ipc is not None and self._ipc.connected:
                self._ipc.request("osd.quicknav.toggle", {})

        def _cycle_aspect(self):
            if not hasattr(self, "combo_aspect"):
                return
            count = self.combo_aspect.count()
            if count <= 1:
                return
            self.combo_aspect.setCurrentIndex(
                (self.combo_aspect.currentIndex() + 1) % count)

        def _cycle_scale(self):
            if not hasattr(self, "combo_scale"):
                return
            count = self.combo_scale.count()
            if count <= 1:
                return
            self.combo_scale.setCurrentIndex(
                (self.combo_scale.currentIndex() + 1) % count)

        def _step_volume(self, delta: int):
            if not hasattr(self, "slider_vol"):
                return
            self.slider_vol.setValue(
                max(0, min(150, self.slider_vol.value() + int(delta))))

        def _cycle_framegen(self):
            if not hasattr(self, "combo_framegen"):
                return
            count = self.combo_framegen.count()
            if count <= 1:
                return
            nxt = (self.combo_framegen.currentIndex() + 1) % count
            self.combo_framegen.setCurrentIndex(nxt)

        def _poll_hub_actions(self):
            if not (self.vfio_mode and self.streamer is not None and
                    self.streamer.is_running):
                return
            self.streamer._read_vfio_log()
            actions = getattr(self.streamer, "_pending_hub_actions", None)
            if not actions:
                return
            pending = list(actions)
            actions.clear()
            mult_to_key = {"0": "off", "2": "2x", "3": "3x", "4": "4x"}
            for act in pending:
                if act == "toggle_fullscreen":
                    self.toggle_cinema_fullscreen()
                elif act == "toggle_record":
                    self.toggle_recording()
                elif act == "exit_fullscreen":
                    self._exit_cinema_fullscreen()
                elif isinstance(act, tuple) and act[0] == "fg_mult":
                    ui_key = mult_to_key.get(act[1])
                    if ui_key and hasattr(self, "combo_framegen"):
                        idx = self.combo_framegen.findData(ui_key)
                        if idx >= 0 and idx != self.combo_framegen.currentIndex():
                            self.combo_framegen.blockSignals(True)
                            self.combo_framegen.setCurrentIndex(idx)
                            self.combo_framegen.blockSignals(False)
                            self.saved_cfg["framegen_mode"] = ui_key
                            save_settings({"framegen_mode": ui_key})

        def eventFilter(self, obj, event):
            if obj is getattr(self, "video_host", None):
                if event.type() in (QEvent.Type.Resize, QEvent.Type.Move):
                    self._position_pip()
                    self._update_video_corner_overlay()
                if event.type() == QEvent.Type.MouseButtonPress:
                    self.video_host.setFocus()
                elif event.type() == QEvent.Type.MouseButtonDblClick:
                    self.toggle_cinema_fullscreen()
                    return True
            return super().eventFilter(obj, event)

        def keyPressEvent(self, event):
            if (not event.isAutoRepeat() and
                    not isinstance(QApplication.focusWidget(), QAbstractSpinBox)):
                key = event.key()
                if key == Qt.Key_Escape:
                    if getattr(self, "_cinema_fullscreen", False):
                        self._exit_cinema_fullscreen()
                        event.accept()
                        return
                elif key in (Qt.Key_F, Qt.Key_F11):
                    self.toggle_cinema_fullscreen()
                    event.accept()
                    return
            super().keyPressEvent(event)

        # -----------------------------------------------------------------
        # Simple / Advanced shell modes
        # -----------------------------------------------------------------
        def on_ui_mode_changed(self, index_or_mode):
            mode = index_or_mode
            if (hasattr(self, "combo_ui_mode")
                    and isinstance(index_or_mode, int)):
                mode = (self.combo_ui_mode.itemData(index_or_mode)
                        or "simple")
            self._apply_ui_mode(str(mode))

        def _toggle_ui_mode(self):
            mode = ("advanced"
                    if getattr(self, "ui_mode", "simple") == "simple"
                    else "simple")
            if hasattr(self, "combo_ui_mode"):
                index = self.combo_ui_mode.findData(mode)
                if index >= 0:
                    self.combo_ui_mode.setCurrentIndex(index)
                    return
            self._apply_ui_mode(mode)

        def on_help_clicked(self):
            self._open_shortcuts_dialog()

        def _apply_ui_mode(self, mode: str, persist: bool = True):
            mode = "advanced" if str(mode).lower() == "advanced" else "simple"
            self.ui_mode = mode
            advanced = mode == "advanced"
            if hasattr(self, "combo_ui_mode"):
                index = self.combo_ui_mode.findData(mode)
                if index >= 0 and index != self.combo_ui_mode.currentIndex():
                    self.combo_ui_mode.blockSignals(True)
                    self.combo_ui_mode.setCurrentIndex(index)
                    self.combo_ui_mode.blockSignals(False)
            if hasattr(self, "right_dock"):
                self.right_dock.setVisible(
                    advanced and not self._cinema_fullscreen)
            if hasattr(self, "telemetry_strip"):
                self.telemetry_strip.setVisible(
                    advanced and not self._cinema_fullscreen)
            if hasattr(self, "btn_resync"):
                self.btn_resync.setVisible(advanced)
            self._apply_responsive_chrome()
            self._apply_capture_controls()
            if persist:
                save_settings({"ui_mode": mode})

        def _apply_responsive_chrome(self):
            """Swap in compact header content before it starts clipping."""
            narrow = self.width() < HEADER_COMPACT_WIDTH
            if hasattr(self, "lbl_card_title"):
                label = (getattr(self, "_card_short_label", "")
                         if narrow
                         else getattr(self, "_card_full_label", ""))
                if label:
                    self.lbl_card_title.setText(label)
            if hasattr(self, "top_bar"):
                self.top_bar.setSpacing(6 if narrow else 8)
                self.top_bar.setContentsMargins(
                    12 if narrow else 16, 14,
                    12 if narrow else 16, 14)
            if hasattr(self, "lbl_hints"):
                self.lbl_hints.setVisible(False)
            # The deck drops secondary readouts before they start clipping.
            for attr in ("lbl_hero_signal", "lbl_drops_counter",
                         "lbl_fg_status"):
                widget = getattr(self, attr, None)
                if widget is not None:
                    widget.setVisible(not narrow)
            if hasattr(self, "lbl_kv_pclk"):
                self.lbl_kv_pclk.setVisible(
                    self.width() >= DECK_EDID_WIDTH)
            self._refresh_header_labels()
            self._apply_capture_controls()

        def resizeEvent(self, event):  # noqa: N802 (Qt casing)
            super().resizeEvent(event)
            self._apply_responsive_chrome()
            self._position_remote_card()
            self._update_video_corner_overlay()

        def _update_action_group_positions(self):
            buttons = [
                button for button in (
                    getattr(self, "btn_launch", None),
                    getattr(self, "btn_studio", None),
                    getattr(self, "btn_pip", None),
                    getattr(self, "btn_record", None),
                    getattr(self, "btn_resync", None),
                )
                if button is not None and not button.isHidden()
            ]
            for index, button in enumerate(buttons):
                if len(buttons) == 1:
                    pos = "only"
                elif index == 0:
                    pos = "first"
                elif index == len(buttons) - 1:
                    pos = "last"
                else:
                    pos = "middle"
                if button.property("actionPos") != pos:
                    button.setProperty("actionPos", pos)
                    button.style().unpolish(button)
                    button.style().polish(button)

        def _refresh_header_labels(self):
            """Keep the fused action group positions and icons in sync."""
            self._update_action_group_positions()
            if hasattr(self, "btn_resync"):
                self.btn_resync.set_icon("rotate")
                self.btn_resync.setToolTip(
                    "Restart the userspace VFIO engine. HDMI hot-switching is automatic."
                    if self.vfio_mode else
                    "Trigger hardware HDMI handshake (HPD pulse) - also key 'H' in window")

        def _compact_signal_text(self):
            """Compact bottom-bar badge: state or resolution + audio format."""
            live = getattr(self, "_live", None) or {}
            audio = (self.lbl_audio_fmt.text()
                     if hasattr(self, "lbl_audio_fmt") else "No Audio")
            if not live.get("locked"):
                if getattr(self, "_remote", False):
                    return "Remote • No Signal", "accent"
                if getattr(self, "_launch_mode", None):
                    return "CAPTURE ACTIVE • No Signal", "warn"
                return "Standby • No Signal", "idle"

            resolution = str(live.get("resolution") or "—")
            rate = str(live.get("framerate") or "")
            text = f"CAPTURE ACTIVE • {resolution}"
            if rate:
                text += f" @ {rate}"
            return text, "active"

        def _set_audio_status(self, text):
            """Update the bottom-bar audio pill with the right semantic tone."""
            if not hasattr(self, "lbl_audio_fmt"):
                return
            text = str(text or "No Audio")
            tone = "idle" if text.strip().lower() == "no audio" else "active"
            chip = self.lbl_audio_fmt
            if chip.state_text() != text or chip.tone() != tone:
                chip.set_state(text, tone)

        def _refresh_signal_badge(self):
            if not hasattr(self, "chip_bar_capture"):
                return
            text, tone = self._compact_signal_text()
            if self.chip_bar_capture.state_text() != text:
                self.chip_bar_capture.set_state(text, tone)
            width = (self.chip_bar_capture.fontMetrics().horizontalAdvance(text)
                     + 34)
            self.chip_bar_capture.setMinimumWidth(width)
            self.chip_bar_capture.setSizePolicy(
                QSizePolicy.Minimum, QSizePolicy.Fixed)
            self.chip_bar_capture.setVisible(True)

        def _apply_capture_controls(self):
            """Sync the passive capture chip and the idle Start button."""
            self._refresh_header_labels()
            mode = getattr(self, "_launch_mode", None)
            remote = bool(getattr(self, "_remote", False))
            active = bool(mode) or remote
            if mode == "studio":
                label = "Studio Capture Active"
            elif mode == "gaming":
                label = "Local Capture Active"
            elif remote:
                label = "Remote Session Active"
            else:
                label = "Standby"

            ui_mode = getattr(self, "ui_mode", "simple")
            # The primary button doubles as the capture state control, so it
            # stays visible in both Simple and Advanced.
            show_launch = True
            if hasattr(self, "lbl_card_title"):
                short = self.width() < HEADER_COMPACT_WIDTH
                card_label = (getattr(self, "_card_short_label", "")
                              if short
                              else getattr(self, "_card_full_label", ""))
                if card_label:
                    self.lbl_card_title.setText(card_label)
            state = (label, active, show_launch, ui_mode, remote)
            if getattr(self, "_capture_ui_state", None) == state:
                return
            self._capture_ui_state = state

            self._refresh_signal_badge()
            if hasattr(self, "btn_launch"):
                self.btn_launch.setVisible(show_launch)

        def toggle_cinema_fullscreen(self):
            self._set_cinema_fullscreen(not getattr(self, "_cinema_fullscreen", False))

        def _exit_cinema_fullscreen(self):
            if getattr(self, "_cinema_fullscreen", False):
                self._set_cinema_fullscreen(False)

        def _sync_osd_mode(self):
            """Sync OSD mode (card, compact, or auto) and recording state."""
            if self._ipc is None or not self._ipc.connected:
                return
            mode = "card"
            if hasattr(self, "chk_quicknav") and not self.chk_quicknav.isChecked():
                mode = "compact"
            elif hasattr(self, "chk_quicknav_fs") and self.chk_quicknav_fs.isChecked():
                mode = "card" if getattr(self, "_cinema_fullscreen", False) else "compact"
            self._ipc.set_osd_mode(mode)
            active = bool(
                getattr(self, "recorder", None) and self.recorder.is_recording)
            self._ipc.set_recording(active)
            self._osd_recording_state = active

        def _set_cinema_fullscreen(self, enabled: bool):
            self._cinema_fullscreen = bool(enabled)
            self._sync_osd_mode()
            if hasattr(self, "btn_cinema_fs"):
                self.btn_cinema_fs.set_icon(
                    "minimize" if self._cinema_fullscreen else "maximize")
                self.btn_cinema_fs.setToolTip(
                    "Exit Cinema Fullscreen" if self._cinema_fullscreen
                    else "Cinema Fullscreen (F / F11)")
            if self._cinema_fullscreen:
                self.header_widget.setVisible(False)
                self.telemetry_strip.setVisible(False)
                self.right_dock.setVisible(False)
                self.bottom_box.setVisible(False)
                self.content_layout.setContentsMargins(0, 0, 0, 0)
                self.content_layout.setSpacing(0)
                self.video_stage_frame.setStyleSheet(
                    "QFrame#videoStageFrame { background-color: #000000; border: none; border-radius: 0px; }")
                self.video_stage_frame.layout().setContentsMargins(0, 0, 0, 0)
                if hasattr(self, "video_corner_overlay"):
                    self.video_corner_overlay.hide()
                self.showFullScreen()
                self.activateWindow()
                self.video_host.setFocus()
            else:
                self.showNormal()
                self.content_layout.setContentsMargins(14, 10, 14, 10)
                self.content_layout.setSpacing(10)
                self.video_stage_frame.setStyleSheet("")
                self.video_stage_frame.layout().setContentsMargins(1, 1, 1, 1)
                self.header_widget.setVisible(True)
                self.bottom_box.setVisible(True)
                self._apply_ui_mode(
                    getattr(self, "ui_mode", "simple"), persist=False)
                self._update_video_corner_overlay()
                self.video_host.setFocus()

        def _get_video_host_winid(self) -> int:
            if not hasattr(self, "video_host") or self.video_host is None:
                return 0
            try:
                return int(self.video_host.winId())
            except Exception:
                return 0

        def _target_parent_winid(self) -> int:
            if hasattr(self, "chk_popout") and self.chk_popout.isChecked():
                return 0
            return self._get_video_host_winid()

        def _is_embedded_gaming_active(self) -> bool:
            return (
                self._launch_mode == "gaming"
                and (self.streamer.is_running or self._remote_docked)
                and not (hasattr(self, "chk_popout") and self.chk_popout.isChecked())
                and self._get_video_host_winid() > 0
            )

        def _update_video_corner_overlay(self):
            """Mask the native video surface to the stage frame's radius."""
            overlay = getattr(self, "video_corner_overlay", None)
            host = getattr(self, "video_host", None)
            if overlay is None or host is None:
                return
            if getattr(self, "_cinema_fullscreen", False):
                overlay.hide()
                return
            width = host.width()
            height = host.height()
            if width <= 0 or height <= 0:
                overlay.hide()
                return

            radius = max(0, int(ui_theme.RADIUS) - 1)
            overlay.setGeometry(host.geometry())
            outer = QRegion(0, 0, width, height)
            if radius:
                path = QPainterPath()
                path.addRoundedRect(
                    QRectF(0, 0, width, height), radius, radius)
                inner = QRegion(path.toFillPolygon().toPolygon())
                overlay.setMask(outer.subtracted(inner))
            else:
                overlay.setMask(outer)
            overlay.show()
            overlay.raise_()
            if getattr(self, "pip_host", None) is not None and \
                    self.pip_host.isVisible():
                self.pip_host.raise_()

        def _position_remote_card(self):
            if not hasattr(self, "remote_card"):
                return
            host = self.video_host.size()
            self.remote_card.move(
                max(0, (host.width() - self.remote_card.width()) // 2),
                max(0, (host.height() - self.remote_card.height()) // 2))

        def _refresh_stage_overlay(self):
            if not hasattr(self, "video_standby_overlay"):
                return
            remote_detached = (
                bool(getattr(self, "_remote", False))
                and not bool(getattr(self, "_remote_docked", False)))
            if hasattr(self, "remote_card"):
                self.remote_card.setVisible(
                    remote_detached and not self._is_embedded_gaming_active())
                if self.remote_card.isVisible():
                    self._position_remote_card()
                    self.remote_card.raise_()

            if self._is_embedded_gaming_active():
                self.video_standby_overlay.setVisible(False)
                return
            if remote_detached:
                self.video_standby_overlay.setVisible(False)
                return

            self.video_standby_overlay.setVisible(True)
            if self._launch_mode == "studio":
                self.video_standby_overlay.set_messages(
                    "STUDIO HEADLESS ACTIVE",
                    "Publishing zero-copy video & audio to OBS / PipeWire")
            elif self._launch_mode == "gaming" and hasattr(self, "chk_popout") and self.chk_popout.isChecked():
                self.video_standby_overlay.set_messages(
                    "EXTERNAL POP-OUT ACTIVE",
                    "Uncheck 'Pop-out Window' below to dock the stream back here")
            else:
                self.video_standby_overlay.set_messages(
                    "NO HDMI SIGNAL",
                    "Waiting for console / input...")

        def on_popout_toggled(self, checked: bool):
            save_settings({"popout_window": bool(checked)})
            self.saved_cfg["popout_window"] = bool(checked)
            if checked and getattr(self, "_cinema_fullscreen", False):
                self._set_cinema_fullscreen(False)
            if self._launch_mode == "gaming" and self._ipc is not None and self._ipc.connected:
                target_wid = 0 if checked else self._get_video_host_winid()
                if self._ipc.set_mode("gaming", parent_winid=target_wid):
                    print(f"[*] Live window dock switch -> {'pop-out' if checked else f'embedded (0x{target_wid:x})'}")
            self._refresh_stage_overlay()

        def _set_launch_running(self, running: bool, mode: Optional[str] = None):
            if running:
                self._launch_mode = mode or "gaming"
                self._last_launch_mode = self._launch_mode
                if (getattr(self, "btn_multiview", None) is not None
                        and self.btn_multiview.isChecked()
                        and self._mv_runner is None):
                    QTimer.singleShot(400, self._multiview_start)
                QTimer.singleShot(600, self._webcam_start)
            else:
                self._launch_mode = None
                self._webcam_stop()
                if getattr(self, "_cinema_fullscreen", False):
                    self._set_cinema_fullscreen(False)

            if self._launch_mode == "studio" and getattr(self, "_cinema_fullscreen", False):
                self._set_cinema_fullscreen(False)

            studio_idle_tip = (
                "Runs the userspace engine headless and publishes native video & audio "
                "to PipeWire, Userspace SHM (libgc570d-v4l2.so), and optional V4L2 for OBS/Discord."
                if self.vfio_mode else
                "Studio / headless mode requires the userspace VFIO engine.")

            if self._launch_mode == "studio":
                self.btn_launch.set_icon("tower-broadcast")
                self.btn_launch.setEnabled(True)
                self.btn_launch.setToolTip(
                    "Studio Capture Active — click to open the gaming window "
                    "without interrupting OBS")
                self.btn_studio.set_icon("stop")
                self.btn_studio.setEnabled(True)
                self.btn_studio.setToolTip(
                    "Stop the headless streamer and release the capture card.")
            elif self._launch_mode == "gaming":
                self.btn_launch.set_icon("stop")
                self.btn_launch.setEnabled(True)
                self.btn_launch.setToolTip(
                    "Local Capture Active — click to stop the gaming session")
                self.btn_studio.set_icon("tower-broadcast")
                self.btn_studio.setEnabled(self.vfio_mode)
                self.btn_studio.setToolTip(
                    "Switch to Studio (Headless)"
                    if self.vfio_mode else studio_idle_tip)
            else:
                self.btn_launch.set_icon("play")
                self.btn_launch.setEnabled(True)
                self.btn_launch.setToolTip("Start Gaming")
                self.btn_studio.set_icon("tower-broadcast")
                self.btn_studio.setEnabled(self.vfio_mode)
                self.btn_studio.setToolTip(studio_idle_tip)

            self._update_action_group_positions()

            if hasattr(self, "lbl_pill"):
                if self._launch_mode == "studio":
                    self.lbl_pill.setText("● Live · Studio")
                    self.lbl_pill.setStyleSheet(
                        "background-color: #122B26; color: #5ED4B2; border: 1px solid #24584B;")
                elif self._launch_mode == "gaming":
                    self.lbl_pill.setText("● Live · Gaming")
                    self.lbl_pill.setStyleSheet(
                        "background-color: #122B26; color: #5ED4B2; border: 1px solid #24584B;")
                elif not self._remote:
                    self.lbl_pill.setText("○ Standby")
                    self.lbl_pill.setStyleSheet(
                        "background-color: #1C192C; color: #ACA5C0; border: 1px solid #2A2540;")
            self._update_vdev_label()
            self._apply_capture_controls()
            self._update_tray_menu()
            self._update_telemetry_cards(getattr(self, "_remote_status", None))
            self._refresh_stage_overlay()
            if self._handoff_paused:
                self._handoff_apply_buttons()

        # -----------------------------------------------------------------
        # Cross-user handoff: our engine paused for another user
        # -----------------------------------------------------------------
        def _handoff_apply_buttons(self):
            self.btn_launch.set_icon("play")
            self.btn_launch.setEnabled(True)
            self.btn_launch.setToolTip(
                "Take Back Capture — ask the other session to release the card")
            self.btn_studio.set_icon("stop")
            self.btn_studio.setEnabled(True)
            self.btn_studio.setToolTip(
                "Quit the paused engine instead of resuming it when the "
                "other user finishes.")
            self._refresh_header_labels()
            if hasattr(self, "lbl_pill"):
                self.lbl_pill.setText("○ Paused")
                self.lbl_pill.setStyleSheet(
                    "background-color: #2B2212; color: #F5C84B; border: 1px solid #58431C;")

        def _handoff_update(self) -> bool:
            """Track the paused marker; True while the paused view is shown."""
            state = handoff_paused_state()
            if state is None:
                if self._handoff_paused is not None:
                    self._handoff_resumed()
                return False

            if self._handoff_paused is None:
                print(f"[*] Capture paused: card in use by {state['holder']}")
                # Forget the pre-pause signal so it is not shown as live.
                self.streamer._vfio_live = False
                self._handoff_paused = state
                self._handoff_apply_buttons()
            else:
                self._handoff_paused = state

            if time.time() >= self._vfio_notice_until:
                self.lbl_hero_signal.setText("Capture Paused")
                self.lbl_hero_meta.setText(
                    f"Capture card is in use by {state['holder']} • "
                    "resumes automatically when they finish")
            if hasattr(self, "lbl_audio_fmt"):
                self._set_audio_status("No Audio")
            return True

        def _handoff_resumed(self):
            self._handoff_paused = None
            if self.streamer.is_running:
                # Our own engine restarted in place; its control socket is new.
                self._set_launch_running(
                    True, self._launch_mode or self._last_launch_mode)
                QTimer.singleShot(400, self._ipc_try_attach)
                self._set_vfio_notice(
                    "Capture Resumed",
                    "The capture card is back with this session",
                    seconds=5.0)
            else:
                self._set_launch_running(False)

        def _handoff_take_back(self):
            state = handoff_paused_state()
            if state is None:
                return
            try:
                os.kill(state["pid"], signal.SIGUSR1)
            except OSError:
                return
            self.btn_launch.setEnabled(False)
            self._set_vfio_notice(
                "Taking Back Capture",
                f"Asking {state['holder']} to release the capture card",
                seconds=8.0)

        def _handoff_stop_paused(self):
            state = self._handoff_paused
            self._handoff_paused = None
            if self.streamer.is_running:
                self._stop_stream()
                return
            if state:
                try:
                    os.kill(state["pid"], signal.SIGTERM)
                except OSError:
                    pass
            self._set_launch_running(False)

        def _retry_audio_props(self):
            # SDL creates its sink-input only once the first HDMI audio
            # format arrives, so apply volume/mute twice.
            for delay in (1200, 3000):
                QTimer.singleShot(
                    delay, lambda: set_audio_volume(self.slider_vol.value()))
                QTimer.singleShot(delay, lambda: set_audio_mute(self.muted))

        def _check_audio_sink(self):
            hint = default_sink_issue()
            if hint:
                print(f"[!] Audio: {hint}; the SDL2 stream will be silent.")
                self._set_vfio_notice(
                    "Audio Muted",
                    hint.capitalize() + " — unmute the system output",
                    seconds=4.0)

        # -----------------------------------------------------------------
        # Background capture service & app camera shortcuts
        # -----------------------------------------------------------------
        def _refresh_service_config(self):
            if self.uvc_mode:
                return
            args = service_manager.read_service_args()
            has_node = os.path.exists(V4L2_DEFAULT_DEVICE)
            env_exists = os.path.exists(service_manager.service_env_path())
            v4l2 = ("--v4l2" in args) if env_exists else has_node
            tap = "--audio-tap" in args
            self.chk_service_v4l2.blockSignals(True)
            self.chk_service_audio_tap.blockSignals(True)
            self.chk_service_v4l2.setChecked(v4l2 and has_node)
            self.chk_service_audio_tap.setChecked(tap)
            self.chk_service_v4l2.blockSignals(False)
            self.chk_service_audio_tap.blockSignals(False)
            self.chk_service_v4l2.setEnabled(has_node)
            if not has_node:
                self.chk_service_v4l2.setToolTip(
                    f"{V4L2_DEFAULT_DEVICE} is not present. Use the "
                    "module-free App Camera shortcuts instead.")

        def _service_config_changed(self):
            v4l2 = (V4L2_DEFAULT_DEVICE
                    if self.chk_service_v4l2.isChecked() else "")
            tap = bool(self.chk_service_audio_tap.isChecked() and
                       self.card_profile.model == MODEL_GC573)
            args = service_manager.write_service_args(v4l2, tap)
            message = ("Service engine arguments updated"
                       + (f": {args}" if args else ": defaults"))
            if self._background_service_active:
                message += " · restart the service to apply"
            self._set_vfio_notice(
                "Background Service", message, seconds=6.0)

        def _refresh_service_status(self):
            if self.uvc_mode:
                return
            status = service_manager.status()
            self._background_service_status = status
            self._background_service_active = status.active
            text = status.label
            if status.detail:
                text += f"\n{status.detail}"
            self.lbl_background_service.setText(text)
            if status.active:
                self.lbl_background_service.setStyleSheet(
                    "color: #5ED4B2; font-size: 11px;")
                self.lbl_card_title.setToolTip(
                    f"Status: {status.label}")
            elif status.installed:
                self.lbl_background_service.setStyleSheet(
                    "color: #A8A1BC; font-size: 11px;")
                if self.lbl_card_title.toolTip().startswith(
                        "Status: Background service"):
                    self.lbl_card_title.setToolTip("Status: Standby")
            else:
                self.lbl_background_service.setStyleSheet(
                    f"color: {ui_theme.WARN}; font-size: 11px;")
                if self.lbl_card_title.toolTip().startswith(
                        "Status: Background service"):
                    self.lbl_card_title.setToolTip("Status: Standby")

            self.btn_service_install.setEnabled(
                bool(not status.installed and
                     service_manager.systemd_available()))
            self.btn_service_enable.setEnabled(
                bool(status.installed and not status.enabled))
            self.btn_service_disable.setEnabled(
                bool(status.installed and status.enabled))
            self.btn_service_start.setEnabled(
                bool(status.installed and not status.active))
            self.btn_service_stop.setEnabled(bool(status.active))
            user_unit = bool(
                status.unit_path and
                Path(status.unit_path).parent ==
                Path(os.path.expanduser("~/.config/systemd/user")))
            self.btn_service_uninstall.setEnabled(user_unit)
            self.btn_service_uninstall.setToolTip(
                "Remove the user-installed unit." if user_unit else
                "This unit is owned by the package; use Disable or your "
                "package manager.")

        def _apply_service_action(self, title: str, action):
            ok, message = action()
            self._set_vfio_notice(title, message, seconds=6.0)
            self._refresh_service_status()
            return ok

        def _service_install(self):
            self._apply_service_action(
                "Background Service", service_manager.install_unit)

        def _service_enable(self):
            if self.streamer.is_running and not self._background_service_active:
                self._set_vfio_notice(
                    "Stop the Local Engine First",
                    "The background service needs exclusive access to the card.")
                return
            if self._apply_service_action(
                    "Background Service", service_manager.enable):
                QTimer.singleShot(1200, self._ipc_try_attach)

        def _service_start(self):
            if self.streamer.is_running and not self._background_service_active:
                self._set_vfio_notice(
                    "Stop the Local Engine First",
                    "The background service needs exclusive access to the card.")
                return
            if self._apply_service_action(
                    "Background Service", service_manager.start):
                QTimer.singleShot(1200, self._ipc_try_attach)

        def _service_stop(self):
            if self._apply_service_action(
                    "Background Service", service_manager.stop):
                self._background_service_active = False
                self._refresh_service_status()

        def _service_disable(self):
            if self._apply_service_action(
                    "Background Service", service_manager.disable):
                self._background_service_active = False
                self._refresh_service_status()

        def _service_uninstall(self):
            answer = QMessageBox.question(
                self,
                "Remove Background Service",
                "Remove the user-installed EverLive service unit?\n"
                "The capture engine will no longer start at login.",
                QMessageBox.Yes | QMessageBox.No,
                QMessageBox.No)
            if answer != QMessageBox.Yes:
                return
            self._background_service_active = False
            self._apply_service_action(
                "Background Service", service_manager.uninstall_unit)

        def _service_logs(self):
            dialog = QDialog(self)
            dialog.setWindowTitle("EverLive Background Service Logs")
            dialog.resize(760, 520)
            dialog.setStyleSheet(CameraLauncherDialog.DIALOG_QSS)
            lay = QVBoxLayout(dialog)
            text = QTextEdit()
            text.setReadOnly(True)
            text.setPlainText(service_manager.logs())
            lay.addWidget(text)
            close = QPushButton("Close")
            close.clicked.connect(dialog.accept)
            lay.addWidget(close, 0, Qt.AlignRight)
            dialog.exec()

        def _open_app_shortcuts(self):
            dialog = CameraLauncherDialog(self.card_profile, self)
            dialog.exec()

        SHORTCUT_ROWS = (
            ("F / F11 / Double-click", "Toggle Cinema Fullscreen"),
            ("Esc", "Exit Fullscreen"),
            ("G", "Cycle Frame Generation"),
            ("Tab", "Toggle On-Screen Stats"),
            ("H", "Toggle HDR Tonemapping"),
            ("M", "Mute Or Unmute Audio"),
            ("Ctrl+Shift+A", "Toggle Simple / Advanced View"),
            ("Ctrl+R", "Start / Stop Video Recording"),
            ("T", "Dock Remote Video"),
        )

        ENGINE_SHORTCUT_ROWS = (
            ("I", "Toggle Performance HUD"),
            ("L", "Cycle Scaling Mode"),
            ("Alt + Z", "Toggle Quick Navigation Bar"),
            ("P", "Toggle OSD Position (Top / Bottom)"),
        )

        def _open_shortcuts_dialog(self):
            dialog = QDialog(self)
            dialog.setWindowTitle("EverLive Keyboard Shortcuts")
            dialog.resize(560, 610)
            dialog.setStyleSheet(CameraLauncherDialog.DIALOG_QSS)

            lay = QVBoxLayout(dialog)
            lay.setContentsMargins(18, 16, 18, 16)
            lay.setSpacing(10)

            title = QLabel("Keyboard Shortcuts")
            title.setStyleSheet(
                f"color: {ui_theme.TEXT}; font-size: 15px; font-weight: 700;")
            lay.addWidget(title)

            subtitle = QLabel(
                "Hub shortcuts work while EverLive has focus. Engine-window "
                "keys work when the video window has focus.")
            subtitle.setObjectName("hintText")
            subtitle.setWordWrap(True)
            lay.addWidget(subtitle)

            def _shortcut_grid(rows):
                grid = QGridLayout()
                grid.setColumnStretch(0, 1)
                grid.setColumnStretch(1, 0)
                grid.setHorizontalSpacing(16)
                grid.setVerticalSpacing(9)
                for row, (keys, description) in enumerate(rows):
                    desc_label = QLabel(description)
                    desc_label.setObjectName("fieldLabel")
                    desc_label.setAlignment(Qt.AlignLeft | Qt.AlignVCenter)
                    grid.addWidget(desc_label, row, 0)

                    key_box = QHBoxLayout()
                    key_box.setContentsMargins(0, 0, 0, 0)
                    key_box.setSpacing(4)
                    for part in keys.split(" / "):
                        key_label = QLabel(part)
                        key_label.setObjectName("kbd")
                        key_label.setAlignment(Qt.AlignCenter)
                        key_box.addWidget(key_label)
                    grid.addLayout(key_box, row, 1, Qt.AlignRight)
                return grid

            hub_heading = QLabel("HUB")
            hub_heading.setObjectName("sectionTitle")
            lay.addWidget(hub_heading)
            lay.addLayout(_shortcut_grid(self.SHORTCUT_ROWS))

            engine_heading = QLabel("ENGINE WINDOW")
            engine_heading.setObjectName("sectionTitle")
            lay.addWidget(engine_heading)
            lay.addLayout(_shortcut_grid(self.ENGINE_SHORTCUT_ROWS))
            lay.addStretch(1)

            close = QPushButton("Close")
            close.clicked.connect(dialog.accept)
            lay.addWidget(close, 0, Qt.AlignRight)
            dialog.exec()

        def _build_diagnostic_report(self) -> str:
            """Plain-text snapshot for bug reports; safe with partial state."""
            def _value(value, default="—"):
                if value in (None, "", {}):
                    return default
                return str(value)

            live = getattr(self, "_live", None) or {}
            service = getattr(self, "_background_service_status", None)
            signal = "locked" if live.get("locked") else "no signal"
            resolution = live.get("resolution") or "—"
            framerate = live.get("framerate") or "—"
            hdr = live.get("hdr") or "SDR"
            chroma = live.get("chroma") or "—"
            audio = live.get("audio") or "No Audio"
            ipc_state = (
                "connected"
                if self._ipc is not None and self._ipc.connected
                else "disconnected")
            service_state = getattr(service, "label", "unknown")
            unit_path = getattr(service, "unit_path", "") or "—"

            lines = [
                "EverLive Diagnostic Report",
                f"Version: {__version__}",
                f"Card: {getattr(self.streamer, 'model_label', self.model)}",
                f"Engine: {'userspace VFIO' if self.vfio_mode else 'kernel driver'}",
                f"Launch mode: {self._launch_mode or 'idle'}",
                f"Attachment: {'remote session' if self._remote else 'local'}",
                f"IPC: {ipc_state}",
                f"Signal: {signal}",
                f"Format: {resolution} @ {framerate}",
                f"Color: {chroma} · HDR: {hdr}",
                f"Audio: {audio}",
                f"Frame generation: {_value(self.saved_cfg.get('framegen_mode'), 'off')}",
                f"Scaling: {_value(self.saved_cfg.get('scale_mode'), 'fit')}",
                f"HDR tonemap: {'on' if self.chk_hdr.isChecked() else 'off'}",
                f"Exposure: {self.spin_hdr_exposure.value():+.1f} EV",
                f"OSD: {'on' if self.chk_osd.isChecked() else 'off'}"
                f" ({_value(self.saved_cfg.get('osd_position'), 'top')})",
                f"Background service: {service_state}",
                f"Service unit: {unit_path}",
                f"Video node: {_value(getattr(self.streamer, 'vdev', None))}",
                f"PipeWire node: {_value(getattr(self.streamer, 'pw_node', None))}",
                f"V4L2 loopback: {V4L2_DEFAULT_DEVICE}",
                f"Virtual audio: {AUDIO_TAP_NODE}",
            ]
            return "\n".join(lines)

        def _copy_diagnostic_report(self):
            report = self._build_diagnostic_report()
            QApplication.clipboard().setText(report)
            self._set_vfio_notice(
                "Diagnostic Report",
                "Copied to the clipboard. Paste it into your bug report.",
                seconds=4.0)
            return report

        def _background_service_blocks_local_start(self) -> bool:
            if not self._background_service_active:
                return False
            if self._remote or (self._ipc is not None and self._ipc.connected):
                return False
            if ipc_probe() is not None:
                return False
            self._set_vfio_notice(
                "Background Service Active",
                "Stop or disable the service in the System tab before "
                "starting a local engine.",
                seconds=8.0)
            return True

        def _vfio_preflight(self) -> bool:
            if not self.vfio_mode:
                self._set_vfio_notice(
                    "Userspace Engine Required",
                    "Studio / virtual camera output needs the VFIO userspace engine")
                return False
            if not (os.path.isfile(self.streamer.streamer_bin) and
                    os.access(self.streamer.streamer_bin, os.X_OK)):
                folder = os.path.basename(userd_dir(self.streamer.model))
                self._set_vfio_notice(
                    "Userspace Engine Missing",
                    f"Build it with: make -C tools/{folder}")
                return False
            if self._takeover_pending:
                return True
            if self._remote or ipc_probe() is not None:
                self._set_vfio_notice(
                    "Remote Session",
                    "Another streamer owns the capture card. Use 'Dock Video "
                    "Here' to attach its preview, or take over capture.",
                    seconds=8.0)
                return False
            if _vfio_streamer_running():
                engine = os.path.basename(self.streamer.streamer_bin)
                self._set_vfio_notice(
                    "Userspace Engine Active",
                    f"An external {engine} instance already owns the device")
                return False
            return True

        # -----------------------------------------------------------------
        # Remote control channel
        # -----------------------------------------------------------------
        def _ipc_try_attach(self):
            """Attach to a running streamer (local or external)."""
            if self._ipc is None or self._ipc_shutdown:
                return
            if self._ipc.connected:
                return
            if self._ipc.try_connect():
                return
            # If a local streamer is running, or if idle watching for external streamers, retry
            if self.streamer.is_running or getattr(self, "_launch_mode", None):
                QTimer.singleShot(400, self._ipc_try_attach)
            else:
                QTimer.singleShot(2500, self, self._ipc_try_attach)

        def _on_remote_connection(self, connected: bool):
            was_remote = self._remote
            is_local = self.streamer.is_running or bool(getattr(self, "_launch_mode", None))
            self._remote = bool(connected and not is_local)
            if connected:
                self._ipc_timer.start()
                print(f"[IPC] Attached to running streamer "
                      f"v{self._ipc.version or '?'}")
                if self._remote:
                    self._set_remote_ui(True)
                self._sync_osd_mode()
                self._poll_remote_status()
            else:
                self._ipc_timer.stop()
                self._remote_status = {}
                if was_remote:
                    self._set_remote_ui(False)
                self._remote = False
                # The streamer may come back; keep watching quietly.
                if not self._ipc_shutdown and not self.streamer.is_running:
                    QTimer.singleShot(2500, self, self._ipc_try_attach)

        def _set_remote_ui(self, remote: bool):
            """Mirror the hub between local control and remote observation."""
            if remote:
                if hasattr(self, "lbl_pill"):
                    self.lbl_pill.setText("● Remote Session")
                    self.lbl_pill.setStyleSheet(
                        "background-color: #122B26; color: #5ED4B2; border: 1px solid #24584B;")
                self.btn_launch.set_icon("tower-broadcast")
                self.btn_launch.setEnabled(True)
                self.btn_launch.setToolTip(
                    "Remote Session Active — use Take Back Capture in the "
                    "viewport to dock it here.")
                self.btn_studio.set_icon("tower-broadcast")
                self.btn_studio.setEnabled(False)
                self.btn_studio.setToolTip(
                    "The external engine already owns the capture card.")
                self._update_action_group_positions()
                # The LED, volume and aspect controls stay live: they now talk
                # to the running engine over the control socket.
                if hasattr(self, "combo_led"):
                    self.combo_led.setEnabled(True)
                self._set_vfio_notice(
                    "Remote Session",
                    "Attached to the userspace streamer that owns the card — "
                    "click 'Dock Video Here' for the preview",
                    seconds=6.0)
                self._apply_capture_controls()
                self._refresh_stage_overlay()
            else:
                self._remote_docked = False
                self._dock_pending_until = 0.0
                self._set_launch_running(False)

        def _remote_dock(self):
            """Attach the external engine's SDL window to the Hub viewport."""
            if (not self._remote or self._remote_docked or
                    self._ipc is None or not self._ipc.connected):
                return
            parent_wid = self._target_parent_winid()
            if parent_wid < 0:
                parent_wid = 0
            self._dock_pending_until = time.time() + 3.0
            if not self._ipc.set_mode_async("gaming", parent_wid,
                                            self._on_dock_reply):
                self._dock_pending_until = 0.0
                self._remote_takeover("Control socket closed while docking")
                return
            self._set_vfio_notice(
                "Docking Video",
                "Attaching the running capture engine to the Hub viewport…",
                seconds=3.0)

        def _on_dock_reply(self, message: Dict):
            if not isinstance(message, dict):
                return
            error = message.get("error")
            result = message.get("result") or {}
            ok = isinstance(result, dict) and bool(result.get("ok"))
            if isinstance(error, dict) or not ok:
                reason = (error.get("message") if isinstance(error, dict)
                          else "engine rejected the docking request")
                print(f"[!] mode.set failed: {reason}")
                self._dock_pending_until = 0.0
                self._remote_takeover(reason)
                return
            self._dock_pending_until = 0.0
            self._remote_docked = True
            self._launch_mode = "gaming"
            self._last_launch_mode = "gaming"
            if hasattr(self, "video_standby_overlay"):
                self.video_standby_overlay.setVisible(False)
            if hasattr(self, "btn_launch"):
                self.btn_launch.set_icon("compress")
                self.btn_launch.setToolTip(
                    "Close Preview — detach the video and keep the engine "
                    "capturing headless.")
            self._set_vfio_notice(
                "Video Docked",
                "Capture kept running — DMA, OBS and audio are uninterrupted",
                seconds=5.0)
            print("[*] Remote engine docked into the Hub viewport")

        def _remote_undock(self):
            """Return a docked external engine to headless capture."""
            if self._ipc is not None and self._ipc.connected:
                self._ipc.set_mode("headless")
            self._remote_docked = False
            self._launch_mode = "studio"
            self._last_launch_mode = "studio"
            if hasattr(self, "btn_launch"):
                self.btn_launch.set_icon("display")
                self.btn_launch.setToolTip(
                    "Dock Video Here — attach the running capture engine to "
                    "the Hub viewport without interrupting DMA, OBS or audio.")
            self._refresh_stage_overlay()
            self._set_vfio_notice(
                "Preview Closed",
                "The external engine keeps capturing headless for OBS",
                seconds=5.0)

        def _remote_takeover(self, reason: str = ""):
            """Stop the external engine and restart capture inside the Hub."""
            if self._takeover_pending:
                return
            self._takeover_pending = True
            self._takeover_tries = 0
            self._dock_pending_until = 0.0
            self._set_vfio_notice(
                "Taking Over Capture",
                ((reason + ". ") if reason else "") +
                "Restarting the engine in the Hub…",
                seconds=6.0)
            print(f"[*] Taking over capture: {reason or 'dock unavailable'}")
            QTimer.singleShot(50, self._stop_remote_engine)

        def _stop_remote_engine(self):
            if self.duo_mode and _duo_service_active():
                self._service_was_active = True
                _stop_duo_service()
            self._takeover_tries = 0
            QTimer.singleShot(200, self._finish_takeover)

        def _finish_takeover(self):
            # Wait for systemd/VFIO teardown without blocking the GUI thread.
            if _vfio_streamer_running() and self._takeover_tries < 40:
                self._takeover_tries += 1
                if self._takeover_tries == 8:
                    # A manually-started engine: ask it to release the card.
                    _stop_vfio_daemon(wait_s=0.0)
                QTimer.singleShot(125, self._finish_takeover)
                return

            self._takeover_tries = 0
            self._takeover_pending = False
            self._remote_docked = False
            self._launch_mode = None
            if self._ipc is not None:
                self._ipc.close()
            if _vfio_streamer_running():
                self._set_vfio_notice(
                    "Handover Failed",
                    "The external engine still owns the VFIO device",
                    seconds=8.0)
                return
            self.start_gaming_screen()

        def _poll_remote_status(self):
            if self._ipc is not None and self._ipc.connected:
                self._ipc.request_status()

        def _on_remote_status(self, status: Dict):
            """Render a snapshot published by the remote streamer."""
            self._remote_status = status
            remote_headless = bool(status.get("headless"))
            if self._remote and not self.streamer.is_running:
                if (self._dock_pending_until and
                        time.time() > self._dock_pending_until):
                    self._dock_pending_until = 0.0
                    if remote_headless:
                        self._remote_takeover(
                            "Engine could not open a preview window")
                        return
                if self._remote_docked and remote_headless:
                    self._remote_docked = False
                    self._launch_mode = "studio"
                    if hasattr(self, "btn_launch"):
                        self.btn_launch.set_icon("display")
                        self.btn_launch.setToolTip(
                            "Dock Video Here — attach the running capture "
                            "engine to the Hub viewport without interrupting "
                            "DMA, OBS or audio.")
                    self._refresh_stage_overlay()
            self._update_framegen_status(status.get("framegen") or {},
                                         remote_headless)
            signal_info = status.get("signal") or {}
            video = status.get("video") or {}
            audio = status.get("audio") or {}
            v4l2 = status.get("v4l2") or {}

            # Always sync engine controls (volume, mute, aspect, scale) into the GUI
            self._sync_remote_controls(status, audio)
            self._update_telemetry_cards(status)

            if not self._remote or self.streamer.is_running:
                return

            if hasattr(self, "lbl_audio_fmt"):
                self._set_audio_status(audio.get("label") or "No Audio")

            if signal_info.get("locked"):
                interlace = "i" if signal_info.get("interlaced") else "p"
                self.lbl_hero_signal.setText(
                    f"{signal_info.get('width', 0)} × "
                    f"{signal_info.get('height', 0)}{interlace} @ "
                    f"{signal_info.get('rate_hz', 0.0):.2f} Hz")
                parts = [
                    f"{video.get('fps', 0.0):.1f} FPS",
                    f"ring {video.get('ring_buffers', 0)}/"
                    f"{video.get('ring_channels', 0)}ch",
                ]
                if audio.get("label"):
                    parts.append(audio["label"])
                if v4l2.get("configured"):
                    parts.append(
                        f"V4L2 {v4l2.get('width', 0)}×{v4l2.get('height', 0)}")
                self.lbl_hero_meta.setText(" • ".join(parts))
                if hasattr(self, "lbl_kv_color"):
                    hdr_state = self._kv_hdr_text("HDR Tonemap", "SDR / Bypass")
                    self.lbl_kv_color.setText(f"YUV 4:2:2 · {hdr_state}")
                if hasattr(self, "lbl_kv_pclk"):
                    self.lbl_kv_pclk.setText(self._kv_link_text(
                        f"Ring {video.get('ring_buffers', 0)}/{video.get('ring_channels', 0)}ch"))
                if hasattr(self, "lbl_kv_audio"):
                    self.lbl_kv_audio.setText(audio.get("label") or "HDMI LPCM")
            else:
                self.lbl_hero_signal.setText("No HDMI Signal")
                self.lbl_hero_meta.setText(
                    "Remote streamer running • waiting for input")
            self._update_telemetry_cards(status)
            self._refresh_stage_overlay()

        def _sync_remote_controls(self, status: Dict, audio: Dict):
            """Mirror the engine's settings into the local controls.

            Signals are blocked so the 1 Hz refresh cannot bounce the value
            straight back to the streamer.
            """
            gain = audio.get("gain")
            if isinstance(gain, (int, float)) and not self.slider_vol.isSliderDown():
                pct = max(0, min(150, int(round(float(gain) * 100))))
                if pct != self.slider_vol.value():
                    self.slider_vol.blockSignals(True)
                    self.slider_vol.setValue(pct)
                    self.slider_vol.blockSignals(False)
                self.lbl_vol.setText(f"{pct}%")

            muted = bool(audio.get("muted"))
            if (muted != self.muted and
                    time.monotonic() >= getattr(self, "_mute_pending_until", 0.0)):
                self._set_mute_visual(muted)

            aspect = status.get("aspect")
            if aspect in ASPECT_KEYS:
                idx = self.combo_aspect.findData(aspect)
                if idx >= 0 and idx != self.combo_aspect.currentIndex():
                    self.combo_aspect.blockSignals(True)
                    self.combo_aspect.setCurrentIndex(idx)
                    self.combo_aspect.blockSignals(False)

            scale = status.get("scale")
            if scale in SCALE_MODES:
                idx = self.combo_scale.findData(scale)
                if idx >= 0 and idx != self.combo_scale.currentIndex():
                    self.combo_scale.blockSignals(True)
                    self.combo_scale.setCurrentIndex(idx)
                    self.combo_scale.blockSignals(False)

            layout = (audio or {}).get("layout")
            if (hasattr(self, "combo_audio_channels") and
                    layout in AUDIO_CHANNEL_KEYS):
                idx = self.combo_audio_channels.findData(layout)
                if (idx >= 0 and
                        idx != self.combo_audio_channels.currentIndex()):
                    self.combo_audio_channels.blockSignals(True)
                    self.combo_audio_channels.setCurrentIndex(idx)
                    self.combo_audio_channels.blockSignals(False)

            osd = status.get("osd") or {}
            if (isinstance(osd.get("enabled"), bool) and
                    osd["enabled"] != self.chk_osd.isChecked()):
                self.chk_osd.blockSignals(True)
                self.chk_osd.setChecked(osd["enabled"])
                self.chk_osd.blockSignals(False)

            pos = osd.get("position")
            pos = OSD_POSITION_ALIASES.get(pos, pos)
            if pos in OSD_POSITIONS:
                idx = self.combo_osd_pos.findData(pos)
                if idx >= 0 and idx != self.combo_osd_pos.currentIndex():
                    self.combo_osd_pos.blockSignals(True)
                    self.combo_osd_pos.setCurrentIndex(idx)
                    self.combo_osd_pos.blockSignals(False)

            # lsfg-vk: mirror the engine's live state.
            fg = status.get("framegen") or {}
            headless = bool(status.get("headless"))
            if hasattr(self, "combo_framegen"):
                raw_mode = str(fg.get("mode", "")).lower()
                mult = int(fg.get("multiplier") or 2)
                mult = max(2, min(4, mult))
                ui_mode = "off" if raw_mode == "off" else f"{mult}x"
                # A headless engine can legitimately report "off" (old build or
                # started without --framegen).  Never let that silently
                # downgrade the user's saved selection in the Hub.
                if raw_mode and ui_mode in FRAMEGEN_KEYS and not (headless and ui_mode == "off"):
                    idx = self.combo_framegen.findData(ui_mode)
                    if idx >= 0 and idx != self.combo_framegen.currentIndex():
                        self.combo_framegen.blockSignals(True)
                        self.combo_framegen.setCurrentIndex(idx)
                        self.combo_framegen.blockSignals(False)

                if hasattr(self, "chk_fg_dedup") and raw_mode in ("30to60", "60to120"):
                    dedup_val = (raw_mode == "30to60")
                    if dedup_val != self.chk_fg_dedup.isChecked():
                        self.chk_fg_dedup.blockSignals(True)
                        self.chk_fg_dedup.setChecked(dedup_val)
                        self.chk_fg_dedup.blockSignals(False)

                if hasattr(self, "chk_fg_perf"):
                    value = fg.get("perf_mode")
                    if isinstance(value, bool) and value != self.chk_fg_perf.isChecked():
                        self.chk_fg_perf.blockSignals(True)
                        self.chk_fg_perf.setChecked(value)
                        self.chk_fg_perf.blockSignals(False)

                if hasattr(self, "chk_fg_ll"):
                    value = fg.get("low_latency")
                    if isinstance(value, bool) and value != self.chk_fg_ll.isChecked():
                        self.chk_fg_ll.blockSignals(True)
                        self.chk_fg_ll.setChecked(value)
                        self.chk_fg_ll.blockSignals(False)

                if hasattr(self, "slider_flow") and not self.slider_flow.isSliderDown():
                    flow = fg.get("flow_scale")
                    if isinstance(flow, (int, float)):
                        flow_pct = max(50, min(100, int(round(float(flow) * 100))))
                        if flow_pct != self.slider_flow.value():
                            self.slider_flow.blockSignals(True)
                            self.slider_flow.setValue(flow_pct)
                            self.slider_flow.blockSignals(False)
                            self.lbl_flow.setText(f"{flow_pct}%")

        def _restore_led_after_stream(self):
            """The streamer hands off; restore the hub's saved colour."""
            if not self.vfio_mode:
                return
            if self.duo_mode or self.model != MODEL_GC573:
                # The DUO engine keeps the last LED effect in hardware; there
                # is no separate standby daemon to hand the colour back to.
                # Cards without LEDs have nothing to restore.
                return
            mode = load_settings().get("led_mode",
                                       self.saved_cfg.get("led_mode"))
            if not isinstance(mode, int) or not 0 <= mode <= 9:
                mode = 4
            # If the streamer failed before its handoff there may be no
            # standby daemon at all; start one even for the cyan default.
            if _proc_running("everlive_userd") and mode == 4:
                return
            set_vfio_led(mode)

        def apply_led(self, mode):
            if mode is None:
                return False
            mode = int(mode)
            self.saved_cfg["led_mode"] = mode
            save_settings({"led_mode": mode})
            if not self.card_profile.supports_rgb_led:
                return True
            if self._ipc is not None and self._ipc.connected:
                return self._send_ipc_led(mode)
            if self.vfio_mode:
                if self.duo_mode:
                    if self.streamer.is_running:
                        return self.streamer.apply_duo_led(mode)
                    print("[*] LED mode saved; it is applied when the DUO engine starts.")
                    return True
                ok = set_vfio_led(mode)
                if self.streamer.is_running:
                    print("[*] LED colour saved; the active userspace stream keeps cyan until it exits.")
                return ok
            return write_led_mode(mode)

        def _send_ipc_led(self, mode: int) -> bool:
            """Forward a LED mode to the running streamer via IPC.

            Modes 8/9 are animations the engine renders itself; 0 turns the
            LEDs off; 1-7 are static colours sent as an explicit RGB triple.
            """
            if self._ipc is None or not self._ipc.connected:
                return False
            if self.duo_mode:
                # The DUO's animations are named differently from the GC573
                # renderer's: mode 8 is its colorful cycle, mode 9 breathing.
                if mode == 8:
                    return self._ipc.set_led_mode("colorful")
                if mode == 9:
                    return self._ipc.set_led_mode("breathing")
            if mode == 8:
                return self._ipc.set_led_mode("wave")
            if mode == 9:
                return self._ipc.set_led_mode("rainbow")
            if mode == 0:
                return self._ipc.set_led_mode("off")

            color = LED_COLORS.get(mode)
            if not color or len(color) != 7:
                return False
            return self._ipc.set_led(int(color[1:3], 16),
                                     int(color[3:5], 16),
                                     int(color[5:7], 16))

        def toggle_stream(self):
            if self._switch_pending:
                return
            if self._handoff_paused:
                self._handoff_take_back()
                return
            if self._remote and self._ipc is not None and self._ipc.connected:
                if self._remote_docked or self._launch_mode == "gaming":
                    self._remote_undock()
                else:
                    self._remote_dock()
                return
            if self._launch_mode == "studio":
                # Studio is capturing: hand the card over to the SDL window.
                self._switch_mode("gaming")
            elif self._launch_mode == "gaming" or self.streamer.is_running:
                self._stop_stream()
            else:
                self.start_gaming_screen()

        def toggle_studio(self):
            if self._switch_pending:
                return
            if self._handoff_paused:
                self._handoff_stop_paused()
                return
            if self._launch_mode == "gaming":
                # Gaming window is open: keep capturing without it.
                self._switch_mode("studio")
            elif self._launch_mode == "studio" or self.streamer.is_running:
                self._stop_stream()
            else:
                self.start_studio()

        def _switch_mode(self, target: str):
            """Restart the streamer in the other mode without a manual stop.

            ``GameStreamer.stop()`` blocks until the child has exited (SIGTERM,
            LED handoff and DMA teardown), so the VFIO group is free again
            before the next instance claims it.  The restart is deferred so Qt
            can repaint the button row in between.
            """
            if self._launch_mode == target or self._switch_pending:
                return
            if target == "studio" and not self.vfio_mode:
                return

            # A remote engine owns the card: swap only its presentation layer.
            if (self._remote and self._ipc is not None and
                    self._ipc.connected):
                if target == "gaming":
                    self._remote_dock()
                else:
                    self._remote_undock()
                return

            # Fast path: the engine swaps its presentation layer in place, so
            # VFIO stays bound and the DMA ring / OBS feed never stop.
            if (self._ipc is not None and self._ipc.connected and
                    self.streamer.is_running):
                mode = "gaming" if target == "gaming" else "headless"
                parent_wid = self._target_parent_winid() if target == "gaming" else None
                if self._ipc.set_mode(mode, parent_winid=parent_wid):
                    self.streamer.headless = target == "studio"
                    self._set_launch_running(True, target)
                    self._vfio_notice_until = 0.0
                    if target == "studio":
                        self._vfio_volume_applies = 0
                        self._set_vfio_notice(
                            "Studio Mode Active",
                            "Output switched live - DMA ring and OBS feed "
                            "uninterrupted", seconds=6.0)
                    else:
                        self._set_vfio_notice(
                            "Gaming Screen Opened",
                            "Output switched live - capture kept running",
                            seconds=6.0)
                    print(f"[*] Live mode switch to {target} (no restart)")
                    return

            self._switch_pending = target
            self._handoff_tries = 0
            if self._ipc is not None:
                self._ipc.close()
            self.streamer.stop()
            self._set_launch_running(False)

            # Freeze the row during the handover so the transient idle labels
            # cannot be clicked twice.
            self.btn_launch.setEnabled(False)
            self.btn_studio.setEnabled(False)
            if target == "studio":
                self.btn_studio.set_icon("rotate")
                self.btn_studio.setToolTip("Switching to Studio…")
                self._set_vfio_notice(
                    "Switching to Studio",
                    "Closing the gaming window; the OBS feed keeps running",
                    seconds=5.0)
            else:
                self.btn_launch.set_icon("rotate")
                self.btn_launch.setToolTip("Switching to Gaming…")
                self._set_vfio_notice(
                    "Switching to Gaming Screen",
                    "Opening the interactive SDL2 window",
                    seconds=5.0)
            QTimer.singleShot(250, lambda: self._handoff_launch(target))

        def _handoff_launch(self, target: str):
            """Start the target mode once the old streamer released the card."""
            if self._switch_pending != target:
                return  # superseded by an explicit stop/start

            if _vfio_streamer_running() and self._handoff_tries < 8:
                # The previous instance is still tearing down; retry shortly.
                self._handoff_tries += 1
                QTimer.singleShot(150, lambda: self._handoff_launch(target))
                return

            timed_out = _vfio_streamer_running()
            self._switch_pending = None
            self._handoff_tries = 0

            # Restore an enabled, idle button row: start_* re-labels it on
            # success and leaves it usable if its preflight refuses.
            self._set_launch_running(False)

            if timed_out:
                print("[!] Mode switch timed out; the previous streamer still owns the card.")
                self._set_vfio_notice(
                    "Handover Failed",
                    "Previous streamer still owns the VFIO device",
                    seconds=8.0)
                return

            if target == "studio":
                self.start_studio()
            else:
                self.start_gaming_screen()

        def _browse_recording_dir(self):
            current = (
                self.saved_cfg.get("rec_dir", "").strip()
                or everlive_recorder.default_recording_dir())
            os.makedirs(current, exist_ok=True)
            chosen = QFileDialog.getExistingDirectory(
                self, "Select Recording Output Folder", current)
            if not chosen:
                return
            save_settings({"rec_dir": chosen})
            self.saved_cfg["rec_dir"] = chosen
            if hasattr(self, "edit_rec_dir"):
                self.edit_rec_dir.setText(_format_rec_dir_display(chosen))
                self.edit_rec_dir.setToolTip(chosen)

        def _open_recording_dir(self):
            folder = (
                self.saved_cfg.get("rec_dir", "").strip()
                or everlive_recorder.default_recording_dir())
            os.makedirs(folder, exist_ok=True)
            QDesktopServices.openUrl(QUrl.fromLocalFile(folder))

        def _recording_geometry_and_fps(self) -> Tuple[int, int, float]:
            w, h, src_fps = 1920, 1080, 60.0
            status = (
                self._remote_status
                if isinstance(getattr(self, "_remote_status", None), dict)
                else {})
            signal_d = (
                status.get("signal")
                if isinstance(status.get("signal"), dict) else {})
            video_d = (
                status.get("video")
                if isinstance(status.get("video"), dict) else {})
            m = getattr(self.streamer, "_vfio_metrics", {}) or {}
            live = getattr(self, "_live", None)
            if not isinstance(live, dict):
                live = {}

            if (int(video_d.get("output_width") or 0) >= 320 and
                    int(video_d.get("output_height") or 0) >= 240):
                w = int(video_d["output_width"])
                h = int(video_d["output_height"])
            elif int(m.get("out_w") or 0) >= 320 and int(m.get("out_h") or 0) >= 240:
                w = int(m["out_w"])
                h = int(m["out_h"])
            elif (int(signal_d.get("width") or 0) >= 320 and
                  int(signal_d.get("height") or 0) >= 240):
                w = int(signal_d["width"])
                h = int(signal_d["height"])
            else:
                res_m = re.match(r"(\d+)x(\d+)", str(live.get("resolution") or ""))
                if res_m:
                    w, h = int(res_m.group(1)), int(res_m.group(2))

            for cand in (signal_d.get("rate_hz"), video_d.get("fps"), m.get("fps")):
                try:
                    val = float(cand or 0.0)
                    if val >= 20.0:
                        src_fps = val
                        break
                except (TypeError, ValueError):
                    pass
            else:
                fps_m = re.search(
                    r"([0-9]+(?:\.[0-9]+)?)", str(live.get("framerate") or ""))
                if fps_m:
                    val = float(fps_m.group(1))
                    if val >= 20.0:
                        src_fps = val

            return w, h, src_fps

        # -- multiview -------------------------------------------------------

        def _multiview_cards(self) -> List[Dict[str, Optional[str]]]:
            """Supported cards other than the Hub's own, in PCI order."""
            own = getattr(self.streamer, "hw_info", {}).get("pci")
            return [c for c in detect_capture_cards() if c["pci"] != own]

        def on_multiview_toggled(self, checked: bool):
            save_settings({"multiview": bool(checked)})
            if checked:
                self._multiview_start()
            else:
                self._multiview_stop()

        def _multiview_layout(self):
            """Main video at (0, 0); tiles fill a 1×2 or 2×2 grid."""
            tiles = list(self._mv_tiles.values())
            cols = 2 if tiles else 1
            for idx, tile in enumerate(tiles, start=1):
                self.stage_grid.addWidget(tile, idx // cols, idx % cols)
            rows = (len(tiles) + 1 + cols - 1) // cols
            for r in range(max(rows, 2)):
                self.stage_grid.setRowStretch(r, 1 if r < rows else 0)
            for c in range(2):
                self.stage_grid.setColumnStretch(c, 1 if c < cols else 0)
            self.stage_grid.setSpacing(6 if tiles else 0)

        def _multiview_start(self):
            if self._mv_runner is not None:
                return
            cards = self._multiview_cards()
            ready = [c for c in cards if c["mode"] == "vfio"][:3]
            skipped = [card_display_name(c) for c in cards if c["mode"] != "vfio"]
            if not ready:
                detail = ("No other capture card is installed." if not cards else
                          f"{', '.join(skipped)} not bound to vfio-pci; "
                          "run ./play.sh --all once.")
                self._set_vfio_notice("Multiview", detail, seconds=8.0)
                return
            if not os.environ.get("DISPLAY"):
                self._set_vfio_notice(
                    "Multiview", "Embedding needs X11 or XWayland.", seconds=8.0)
                return

            volumes = load_settings().get("multiview_volumes", {})
            if not isinstance(volumes, dict):
                volumes = {}
            for card in ready:
                try:
                    vol = int(volumes.get(card["pci"], 100))
                except (TypeError, ValueError):
                    vol = 100
                tile = MultiviewTile(card, self.stage_grid_host, volume=vol)
                tile.listenToggled.connect(self._multiview_listen)
                tile.volumeChanged.connect(self._multiview_volume)
                self._mv_tiles[card["pci"]] = tile
            self._multiview_layout()
            for tile in self._mv_tiles.values():
                tile.show()
            QApplication.processEvents()

            runner = everlive_multi.MultiCardRunner(
                ready,
                parent_winids={pci: t.host_winid()
                               for pci, t in self._mv_tiles.items()},
                silent_playback=True)
            for pci, tile in self._mv_tiles.items():
                runner.set_volume(pci, tile.slider_vol.value())
            started = runner.start_engines()
            _MULTIVIEW_PIDS.update(runner.engine_pids())
            if started == 0:
                self._multiview_clear_tiles()
                self._set_vfio_notice(
                    "Multiview Failed",
                    "No engine started; see ~/.config/everlive/multi/.",
                    seconds=8.0)
                return
            self._mv_runner = runner
            self._mv_timer.start()
            # The streams appear within the first second; mute them early.
            for delay in (300, 700):
                QTimer.singleShot(delay, self._multiview_mute_now)
            note = f"{started} more card{'s' if started != 1 else ''} running"
            if skipped:
                note += f"; {', '.join(skipped)} not bound to vfio-pci"
            self._set_vfio_notice("Multiview", note, seconds=6.0)

        def _multiview_clear_tiles(self):
            for tile in self._mv_tiles.values():
                self.stage_grid.removeWidget(tile)
                tile.hide()
                tile.deleteLater()
            self._mv_tiles.clear()
            self._multiview_layout()

        # -----------------------------------------------------------------
        # Webcam picture-in-picture
        # -----------------------------------------------------------------
        def _ensure_cam(self):
            if getattr(self, "_cam", None) is None:
                self._cam = everlive_cam.CameraBridge(
                    log_path=os.path.join(CONFIG_DIR, "webcam.log"))

        def _pip_settings(self) -> dict:
            """PiP settings as the Hub shows them.

            The controls are the source of truth while the Hub runs;
            settings.json only fills in values before the UI exists (it can
            be replaced behind the Hub's back, and Record must still match
            what the camera button shows).
            """
            cfg = dict(load_settings())
            btn = getattr(self, "btn_pip", None)
            if btn is not None:
                cfg["pip_enabled"] = btn.isChecked()
            for key, name in (("pip_device", "combo_pip_device"),
                              ("pip_mode", "combo_pip_mode"),
                              ("pip_corner", "combo_pip_corner"),
                              ("pip_size", "combo_pip_size")):
                combo = getattr(self, name, None)
                if combo is not None and combo.currentIndex() >= 0:
                    cfg[key] = combo.currentData()
            return everlive_cam.normalize_settings(cfg)

        def on_pip_toggled(self, enabled: bool):
            enabled = bool(enabled)
            save_settings({"pip_enabled": enabled})
            for control in (getattr(self, "btn_pip", None),
                            getattr(self, "chk_pip", None)):
                if control is not None and control.isChecked() != enabled:
                    control.blockSignals(True)
                    control.setChecked(enabled)
                    control.blockSignals(False)
            if enabled:
                if self._launch_mode is not None:
                    self._webcam_start()
            else:
                self._webcam_stop()

        def _pip_setting_changed(self, values: dict, restart: bool = False):
            save_settings(values)
            if "pip_device" in values:
                self._refresh_camera_controls()
            if restart and self._cam.running:
                self._cam.stop()
                self._webcam_start()
            else:
                self._position_pip()

        def _webcam_start(self):
            cfg = self._pip_settings()
            if not cfg["pip_enabled"] or self._launch_mode is None:
                return
            if not self._cam.start(str(cfg["pip_device"]), str(cfg["pip_mode"])):
                self._set_vfio_notice(
                    "Webcam Unavailable", self._cam.last_error, seconds=6.0)
                return
            self._cam_timer.start()
            self._webcam_update_preview()

        def _webcam_stop(self):
            cam = getattr(self, "_cam", None)
            if cam is None:
                return
            self._cam_timer.stop()
            if getattr(self, "pip_host", None) is not None:
                self.pip_host.detach()
                self.pip_host.hide()
            cam.stop()

        def _webcam_tick(self):
            if not self._cam.running:
                # The camera was unplugged or the bridge failed.
                self._webcam_stop()
                self._set_vfio_notice(
                    "Webcam Stopped",
                    f"The webcam bridge exited; see {self._cam.log_path}.",
                    seconds=6.0)
                return
            self._webcam_update_preview()

        def _webcam_update_preview(self):
            host = getattr(self, "pip_host", None)
            if host is None:
                return
            if not (self._cam.running and self._is_embedded_gaming_active()):
                host.detach()
                self._cam.stop_preview()
                host.hide()
                return
            self._position_pip()
            host.show()
            host.raise_()
            if not self._cam.preview_running:
                pw, ph = everlive_cam.PREVIEW_SIZE
                cfg = self._pip_settings()
                _mw, _mh, mfps = everlive_cam.parse_mode(str(cfg.get("pip_mode", "")))
                if self._cam.start_preview(pw, ph, fps=mfps):
                    host.attach(self._cam.preview.stdout, pw, ph)
                else:
                    host.hide()
                    if self._cam.last_error:
                        self._set_vfio_notice(
                            "Webcam Preview", self._cam.last_error, seconds=6.0)

        def _position_pip(self):
            host = getattr(self, "pip_host", None)
            if host is None or not hasattr(self, "video_host"):
                return
            area = self.video_host.geometry()
            if area.width() < 32 or area.height() < 32:
                return
            # The engine letterboxes the picture to 16:9 inside video_host.
            pic_w = min(area.width(), int(area.height() * 16 / 9))
            pic_h = min(area.height(), int(pic_w * 9 / 16))
            pic_x = area.x() + (area.width() - pic_w) // 2
            pic_y = area.y() + (area.height() - pic_h) // 2
            cfg = self._pip_settings()
            x, y, w, h = everlive_recorder.pip_geometry(
                everlive_recorder.PipOverlay(
                    corner=str(cfg["pip_corner"]), size_pct=int(cfg["pip_size"])),
                pic_w, pic_h)
            host.setGeometry(pic_x + x, pic_y + y, w, h)
            if host.isVisible():
                host.raise_()

        def _webcam_recording_overlay(self, plan):
            """The PiP for the main recording, or None when it is off."""
            if not (self._cam.running and self._pip_settings()["pip_enabled"]):
                return None
            pip = everlive_cam.pip_overlay(
                self._pip_settings(), self._cam.preload_library)
            if plan.source == "window" and self.pip_host.isVisible():
                # The window grab already holds the (smaller, scaled)
                # preview; draw the full-quality camera exactly over it.
                ratio = self.video_host.devicePixelRatioF() or 1.0
                local = self.pip_host.geometry().translated(
                    -self.video_host.x(), -self.video_host.y())
                pip = dataclasses.replace(pip, rect=(
                    int(local.x() * ratio), int(local.y() * ratio),
                    int(local.width() * ratio) & ~1,
                    int(local.height() * ratio) & ~1))
            return pip

        def _on_camera_control_changed(self, ctrl_name: str, value: int):
            dev = self.combo_pip_device.currentData() or ""
            everlive_cam.set_camera_control(dev, ctrl_name, value)
            widgets = getattr(self, "_cam_ctrl_widgets", {})
            if ctrl_name == "auto_exposure" and "exposure_time_absolute" in widgets:
                widgets["exposure_time_absolute"].setEnabled(value == 1)
            elif ctrl_name == "white_balance_automatic" and "white_balance_temperature" in widgets:
                widgets["white_balance_temperature"].setEnabled(not bool(value))
            elif ctrl_name in ("focus_automatic_continuous", "focus_auto") and "focus_absolute" in widgets:
                widgets["focus_absolute"].setEnabled(not bool(value))

        def _reset_camera_controls(self):
            dev = self.combo_pip_device.currentData() or ""
            if everlive_cam.reset_camera_controls(dev):
                self._set_vfio_notice("Camera Controls", "Reset camera hardware controls to defaults.", seconds=4.0)
            self._refresh_camera_controls()

        def _refresh_camera_controls(self):
            if not hasattr(self, "_cam_ctrl_layout"):
                return
            while self._cam_ctrl_layout.count():
                item = self._cam_ctrl_layout.takeAt(0)
                w = item.widget()
                if w:
                    w.deleteLater()
            self._cam_ctrl_widgets.clear()

            dev = self.combo_pip_device.currentData() or ""
            ctrls = everlive_cam.query_camera_controls(dev)
            if not ctrls:
                lbl = QLabel("No hardware V4L2 controls available for this camera (or camera disconnected).")
                lbl.setObjectName("hintText")
                lbl.setWordWrap(True)
                self._cam_ctrl_layout.addWidget(lbl, 0, 0, 1, 2)
                self.btn_cam_reset.setEnabled(False)
                return

            self.btn_cam_reset.setEnabled(True)

            def _shrink(cb):
                cb.setSizeAdjustPolicy(QComboBox.AdjustToMinimumContentsLengthWithIcon)
                cb.setMinimumContentsLength(8)
                cb.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Fixed)
                cb.setMinimumWidth(0)
                return cb

            def _slider_row(name: str, title: str, info: dict):
                w = QWidget()
                lay = QVBoxLayout(w)
                lay.setContentsMargins(0, 0, 0, 0)
                lay.setSpacing(2)
                h = QHBoxLayout()
                h.setContentsMargins(0, 0, 0, 0)
                lbl_title = QLabel(title)
                lbl_title.setObjectName("fieldLabel")
                lbl_val = QLabel(str(info.get("value", 0)))
                lbl_val.setObjectName("numericValue")
                h.addWidget(lbl_title, 1)
                h.addWidget(lbl_val, 0)
                lay.addLayout(h)

                slider = QSlider(Qt.Horizontal)
                slider.setRange(int(info.get("min", 0)), int(info.get("max", 100)))
                slider.setSingleStep(max(1, int(info.get("step", 1))))
                slider.setValue(int(info.get("value", 0)))
                slider.valueChanged.connect(lambda v, lv=lbl_val: lv.setText(str(v)))
                slider.sliderReleased.connect(lambda n=name, s=slider: self._on_camera_control_changed(n, s.value()))
                lay.addWidget(slider)
                self._cam_ctrl_widgets[name] = slider
                return w

            def _field_box(text: str, control: QWidget):
                box = QWidget()
                lay = QVBoxLayout(box)
                lay.setContentsMargins(0, 0, 0, 0)
                lay.setSpacing(4)
                lbl = QLabel(text)
                lbl.setObjectName("fieldLabel")
                lay.addWidget(lbl)
                lay.addWidget(control)
                return box

            def _camera_toggle(text: str, checked: bool, on_toggle,
                               tooltip: str = ""):
                """Wrap a dynamic camera checkbox in a Hub ToggleRow."""
                row = ToggleRow(text, "", self._cam_ctrl_container)
                chk = QCheckBox(row)
                chk.setChecked(bool(checked))
                if tooltip:
                    chk.setToolTip(tooltip)
                chk.toggled.connect(on_toggle)
                chk.hide()
                row.bind(chk)
                return row

            row = 0

            # 1. Exposure & Lighting
            exp_group = QGroupBox("EXPOSURE && LIGHTING")
            exp_group.setObjectName("cameraGroup")
            exp_lay = QGridLayout(exp_group)
            exp_lay.setContentsMargins(8, 10, 8, 8)
            exp_lay.setSpacing(8)
            exp_row = 0

            if "auto_exposure" in ctrls:
                info = ctrls["auto_exposure"]
                cb = _shrink(QComboBox())
                menu = info.get("menu", {})
                for k, v in menu.items():
                    cb.addItem(v, k)
                if not menu:
                    cb.addItem("Manual", 1)
                    cb.addItem("Auto", 3)
                idx = cb.findData(info.get("value", 3))
                if idx >= 0:
                    cb.setCurrentIndex(idx)
                cb.currentIndexChanged.connect(
                    lambda *_, c=cb: self._on_camera_control_changed("auto_exposure", c.currentData()))
                exp_lay.addWidget(_field_box("Exposure Mode", cb), exp_row, 0, 1, 2)
                exp_row += 1

            if "exposure_time_absolute" in ctrls:
                exp_lay.addWidget(_slider_row("exposure_time_absolute", "Shutter Speed / Exposure", ctrls["exposure_time_absolute"]), exp_row, 0, 1, 2)
                auto_val = ctrls.get("auto_exposure", {}).get("value", 3)
                if "exposure_time_absolute" in self._cam_ctrl_widgets:
                    self._cam_ctrl_widgets["exposure_time_absolute"].setEnabled(auto_val == 1)
                exp_row += 1

            if "exposure_dynamic_framerate" in ctrls:
                info = ctrls["exposure_dynamic_framerate"]
                chk_dyn = _camera_toggle(
                    "Dynamic Framerate in Low Light",
                    bool(info.get("value", 1)),
                    lambda checked: self._on_camera_control_changed(
                        "exposure_dynamic_framerate", int(checked)),
                    "When disabled, camera maintains full target FPS even in "
                    "dimmer light (lower latency)")
                exp_lay.addWidget(chk_dyn, exp_row, 0, 1, 2)
                exp_row += 1

            if "gain" in ctrls:
                exp_lay.addWidget(_slider_row("gain", "Sensor Gain", ctrls["gain"]), exp_row, 0)
            if "power_line_frequency" in ctrls:
                info = ctrls["power_line_frequency"]
                cb_flk = _shrink(QComboBox())
                menu = info.get("menu", {})
                for k, v in menu.items():
                    cb_flk.addItem(v, k)
                if not menu:
                    cb_flk.addItem("Disabled", 0)
                    cb_flk.addItem("50 Hz", 1)
                    cb_flk.addItem("60 Hz", 2)
                idx = cb_flk.findData(info.get("value", 2))
                if idx >= 0:
                    cb_flk.setCurrentIndex(idx)
                cb_flk.currentIndexChanged.connect(
                    lambda *_, c=cb_flk: self._on_camera_control_changed("power_line_frequency", c.currentData()))
                exp_lay.addWidget(_field_box("Anti-Flicker", cb_flk), exp_row, 1)
            exp_row += 1

            self._cam_ctrl_layout.addWidget(exp_group, row, 0, 1, 2)
            row += 1

            # 2. Focus & Framing Group
            if any(k in ctrls for k in ("focus_automatic_continuous", "focus_auto", "focus_absolute", "zoom_absolute", "pan_absolute", "tilt_absolute")):
                foc_group = QGroupBox("FOCUS && FRAMING")
                foc_group.setObjectName("cameraGroup")
                foc_lay = QGridLayout(foc_group)
                foc_lay.setContentsMargins(8, 10, 8, 8)
                foc_lay.setSpacing(8)
                foc_row = 0

                af_key = "focus_automatic_continuous" if "focus_automatic_continuous" in ctrls else "focus_auto" if "focus_auto" in ctrls else None
                if af_key:
                    chk_af = _camera_toggle(
                        "Continuous Auto Focus",
                        bool(ctrls[af_key].get("value", 1)),
                        lambda checked, k=af_key:
                            self._on_camera_control_changed(k, int(checked)))
                    foc_lay.addWidget(chk_af, foc_row, 0, 1, 2)
                    foc_row += 1

                if "focus_absolute" in ctrls:
                    foc_lay.addWidget(_slider_row("focus_absolute", "Manual Focus", ctrls["focus_absolute"]), foc_row, 0, 1, 2)
                    if af_key and "focus_absolute" in self._cam_ctrl_widgets:
                        self._cam_ctrl_widgets["focus_absolute"].setEnabled(not bool(ctrls[af_key].get("value", 1)))
                    foc_row += 1

                if "zoom_absolute" in ctrls:
                    foc_lay.addWidget(_slider_row("zoom_absolute", "Digital Zoom", ctrls["zoom_absolute"]), foc_row, 0, 1, 2)
                    foc_row += 1

                if "pan_absolute" in ctrls and "tilt_absolute" in ctrls:
                    foc_lay.addWidget(_slider_row("pan_absolute", "Pan", ctrls["pan_absolute"]), foc_row, 0)
                    foc_lay.addWidget(_slider_row("tilt_absolute", "Tilt", ctrls["tilt_absolute"]), foc_row, 1)
                    foc_row += 1

                self._cam_ctrl_layout.addWidget(foc_group, row, 0, 1, 2)
                row += 1

            # 3. Image & Color Quality
            col_group = QGroupBox("COLOR && IMAGE QUALITY")
            col_group.setObjectName("cameraGroup")
            col_lay = QGridLayout(col_group)
            col_lay.setContentsMargins(8, 10, 8, 8)
            col_lay.setSpacing(8)
            col_row = 0

            if "white_balance_automatic" in ctrls:
                chk_wb = _camera_toggle(
                    "Auto White Balance",
                    bool(ctrls["white_balance_automatic"].get("value", 1)),
                    lambda checked: self._on_camera_control_changed(
                        "white_balance_automatic", int(checked)))
                col_lay.addWidget(chk_wb, col_row, 0, 1, 2)
                col_row += 1

            if "white_balance_temperature" in ctrls:
                col_lay.addWidget(_slider_row("white_balance_temperature", "Color Temperature (Kelvin)", ctrls["white_balance_temperature"]), col_row, 0, 1, 2)
                if "white_balance_automatic" in ctrls and "white_balance_temperature" in self._cam_ctrl_widgets:
                    self._cam_ctrl_widgets["white_balance_temperature"].setEnabled(not bool(ctrls["white_balance_automatic"].get("value", 1)))
                col_row += 1

            if "brightness" in ctrls:
                col_lay.addWidget(_slider_row("brightness", "Brightness", ctrls["brightness"]), col_row, 0)
            if "contrast" in ctrls:
                col_lay.addWidget(_slider_row("contrast", "Contrast", ctrls["contrast"]), col_row, 1)
            col_row += 1

            if "saturation" in ctrls:
                col_lay.addWidget(_slider_row("saturation", "Saturation", ctrls["saturation"]), col_row, 0)
            if "sharpness" in ctrls:
                col_lay.addWidget(_slider_row("sharpness", "Sharpness", ctrls["sharpness"]), col_row, 1)
            col_row += 1

            if "backlight_compensation" in ctrls:
                col_lay.addWidget(_slider_row("backlight_compensation", "Backlight Compensation", ctrls["backlight_compensation"]), col_row, 0, 1, 2)
                col_row += 1

            self._cam_ctrl_layout.addWidget(col_group, row, 0, 1, 2)
            row += 1

        def _multiview_stop(self):
            runner = getattr(self, "_mv_runner", None)
            if runner is None:
                return
            self._mv_runner = None
            self._mv_timer.stop()
            runner.stop_recording()
            # Engines first: their windows live inside the tiles.
            runner.stop_engines()
            _MULTIVIEW_PIDS.clear()
            self._multiview_clear_tiles()

        def _multiview_listen(self, pci: str, enabled: bool):
            if self._mv_runner is not None:
                self._mv_runner.set_listen(pci, enabled)

        def _multiview_mute_now(self):
            if self._mv_runner is not None:
                self._mv_runner.apply_playback_mute()

        def _multiview_volume(self, pci: str, percent: int):
            if self._mv_runner is not None:
                self._mv_runner.set_volume(pci, percent)
            volumes = load_settings().get("multiview_volumes", {})
            if not isinstance(volumes, dict):
                volumes = {}
            volumes[pci] = int(percent)
            save_settings({"multiview_volumes": volumes})

        def _multiview_poll(self):
            runner = self._mv_runner
            if runner is None:
                return
            alive = runner.poll()
            for sess in runner.sessions:
                tile = self._mv_tiles.get(sess.card["pci"])
                if tile is None:
                    continue
                if not sess.alive:
                    tile.set_state("Engine stopped (see ~/.config/everlive/multi/)")
                    continue
                w, h, fps, locked = everlive_multi.signal_geometry(
                    sess.status(timeout=0.2))
                state = f"{w}×{h} @ {fps:.0f}" if locked else "No signal"
                rec = ""
                for src in sess.sources:
                    if src.active:
                        rec = f"● REC {src.recorder.format_elapsed()}"
                tile.set_state(state, rec)
            if not alive:
                self._set_vfio_notice(
                    "Multiview", "The other cards' engines have exited.",
                    seconds=6.0)
                self._multiview_stop()
                if getattr(self, "btn_multiview", None) is not None:
                    self.btn_multiview.blockSignals(True)
                    self.btn_multiview.setChecked(False)
                    self.btn_multiview.blockSignals(False)

        def _multiview_recording_settings(self):
            def _combo(name, default):
                combo = getattr(self, name, None)
                return (combo.currentData() if combo is not None else None) or default
            self._mv_runner.configure_recording(
                rec_dir=(self.saved_cfg.get("rec_dir", "").strip()
                         or everlive_recorder.default_recording_dir()),
                encoder=_combo("combo_rec_encoder", "auto"),
                quality=_combo("combo_rec_quality", "high"),
                container=_combo("combo_rec_format", "mkv"))

        def _sync_recording_ui(self):
            active = bool(getattr(self, "recorder", None) and self.recorder.is_recording)
            plan = self.recorder.current_plan if active else None

            if getattr(self, "_osd_recording_state", None) != active:
                self._osd_recording_state = active
                ipc = getattr(self, "_ipc", None)
                if ipc is not None and ipc.connected:
                    ipc.set_recording(active)

            if hasattr(self, "btn_record"):
                self.btn_record.set_icon(
                    "stop" if active else "record",
                    color=ui_theme.ON_ACCENT if active else ui_theme.DANGER)
                self.btn_record.setToolTip(
                    "Stop Video Recording (Ctrl+R)" if active
                    else "Start Video Recording (Ctrl+R)")
                if bool(self.btn_record.property("recording")) != active:
                    self.btn_record.setProperty("recording", active)
                    self.btn_record.style().unpolish(self.btn_record)
                    self.btn_record.style().polish(self.btn_record)

            if hasattr(self, "chip_bar_record"):
                self.chip_bar_record.setVisible(active)

            if hasattr(self, "lbl_rec_status"):
                if active and plan is not None:
                    self.lbl_rec_status.setText(
                        f"● {self.recorder.format_elapsed()} · {plan.fps} fps")
                    self.lbl_rec_status.setStyleSheet(
                        "color: #FF6B7A; font-weight: 700;")
                else:
                    self.lbl_rec_status.setText("Standby")
                    self.lbl_rec_status.setStyleSheet("")

            for attr in (
                "combo_rec_fps", "combo_rec_source", "combo_rec_encoder",
                "combo_rec_audio", "combo_rec_format", "combo_rec_quality",
            ):
                w = getattr(self, attr, None)
                if w is not None:
                    w.setEnabled(not active)

        def _poll_recording_state(self):
            if not hasattr(self, "recorder"):
                return
            if self.recorder.is_recording or self._rec_was_active:
                alive = self.recorder.poll()
                if not alive and self._rec_was_active:
                    self._rec_was_active = False
                    self._sync_recording_ui()
                    if self.recorder.last_error:
                        self._set_vfio_notice(
                            "Recording Error",
                            self.recorder.last_error,
                            seconds=8.0)
                    else:
                        self._set_vfio_notice(
                            "Recording Stopped",
                            "Capture writer exited cleanly.",
                            seconds=5.0)
                else:
                    self._sync_recording_ui()

        def toggle_recording(self):
            if self.recorder.is_recording:
                self.stop_recording()
            else:
                self.start_recording()

        def start_recording(self) -> bool:
            if self.recorder.is_recording:
                return True
            shm_alive = any(
                os.path.exists(f"/dev/shm/{m}_v4l2_shm")
                for m in (MODEL_GC573, MODEL_GC570D, MODEL_HWSUHD))
            if self.vfio_mode and not (
                self.streamer.is_running
                or self._remote
                or self._background_service_active
                or shm_alive
            ):
                self._set_vfio_notice(
                    "Capture Inactive",
                    "Start Gaming or Studio first before recording.",
                    seconds=5.0)
                return False

            w, h, src_fps = self._recording_geometry_and_fps()
            fg_cfg = self._framegen_config()
            win_id = (
                self._get_video_host_winid()
                if self._is_embedded_gaming_active() else None)
            preload_lib = (
                app_integrations.find_preload_library(self.card_profile)
                if self.vfio_mode else None)
            duo_inp = (
                self.combo_duo_input.currentData()
                if hasattr(self, "combo_duo_input")
                else self.saved_cfg.get("duo_input", 0))
            rec_audio = (
                self.combo_rec_audio.currentData()
                if hasattr(self, "combo_rec_audio")
                else self.saved_cfg.get("rec_audio", "everlive"))
            audio_tap_active = bool(
                self.vfio_mode and hasattr(self, "chk_audio_tap")
                and self.chk_audio_tap.isChecked())
            audio_source = everlive_recorder.resolve_pulse_monitor(
                audio_tap_active=audio_tap_active,
                audio_tap_node=AUDIO_TAP_NODE,
                audio_mode=rec_audio or "everlive",
                model=self.model,
                duo_input=int(duo_inp or 0))
            out_dir = (
                self.saved_cfg.get("rec_dir", "").strip()
                or everlive_recorder.default_recording_dir())
            v4l2_dev = (
                None if self.vfio_mode
                else getattr(self.streamer, "vdev", None))

            plan_args = dict(
                output_dir=out_dir,
                model=self.model,
                duo_input=int(duo_inp or 0),
                vfio_mode=self.vfio_mode,
                v4l2_device=v4l2_dev,
                preload_library=preload_lib,
                width=w,
                height=h,
                source_fps=src_fps,
                fps_setting=(
                    self.combo_rec_fps.currentData()
                    if hasattr(self, "combo_rec_fps") else "auto"),
                source_setting=(
                    self.combo_rec_source.currentData()
                    if hasattr(self, "combo_rec_source") else "auto"),
                framegen_mode=fg_cfg.get("framegen_mode", "off"),
                fg_multiplier=int(fg_cfg.get("fg_multiplier", 2)),
                encoder_setting=(
                    self.combo_rec_encoder.currentData()
                    if hasattr(self, "combo_rec_encoder") else "auto"),
                quality_setting=(
                    self.combo_rec_quality.currentData()
                    if hasattr(self, "combo_rec_quality") else "high"),
                container_format=(
                    self.combo_rec_format.currentData()
                    if hasattr(self, "combo_rec_format") else "mkv"),
                window_id=win_id,
                audio_source=audio_source,
            )
            plan = everlive_recorder.build_recording_plan(**plan_args)
            pip = self._webcam_recording_overlay(plan)
            if pip is not None:
                plan_args["pip"] = pip
                plan = everlive_recorder.build_recording_plan(**plan_args)
            mv = self._mv_runner
            stamp = everlive_multi.take_stamp()
            if mv is not None:
                # One take: every card's file carries the same timestamp.
                plan = everlive_recorder.build_recording_plan(
                    **plan_args,
                    filename=everlive_multi.take_filename_for(
                        self.model,
                        getattr(self.streamer, "hw_info", {}).get("pci") or "card",
                        int(duo_inp or 0) if self.duo_mode else 0,
                        stamp, plan.fps, plan_args["container_format"]))

            ok, msg = self.recorder.start(plan)
            self._rec_was_active = ok
            extra = 0
            if mv is not None and ok:
                self._multiview_recording_settings()
                extra = mv.start_recording(stamp)
            self._sync_recording_ui()
            if ok:
                more = f" + {extra} more card{'s' if extra != 1 else ''}" if extra else ""
                if plan.pip is not None:
                    more += " · webcam overlay"
                self._set_vfio_notice(
                    "Recording Started",
                    f"{plan.width}×{plan.height} @ {plan.fps} FPS ({plan.encoder}) → {os.path.basename(plan.output_path)}{more}",
                    seconds=6.0)
            else:
                self._set_vfio_notice("Recording Failed", msg, seconds=8.0)
            return ok

        def stop_recording(self):
            if self._mv_runner is not None:
                self._mv_runner.stop_recording()
            if not self.recorder.is_recording and not self._rec_was_active:
                return
            ok, msg = self.recorder.stop()
            self._rec_was_active = False
            self._sync_recording_ui()
            self._set_vfio_notice(
                "Recording Saved" if ok else "Recording Stopped",
                msg, seconds=6.0)

        def _stop_stream(self):
            self._multiview_stop()
            if getattr(self, "recorder", None) and self.recorder.is_recording:
                plan = self.recorder.current_plan
                if not (self._background_service_active and plan and plan.source == "shm"):
                    self.stop_recording()
            # Cancel any in-flight mode handover so its timer cannot relaunch.
            self._switch_pending = None
            self._handoff_tries = 0
            if self._ipc is not None:
                self._ipc.close()
            self.streamer.stop()
            self._set_launch_running(False)
            if self.vfio_mode:
                QTimer.singleShot(900, self._restore_led_after_stream)

        def _set_vfio_notice(self, title: str, detail: str, seconds: float = 8.0):
            self.lbl_hero_signal.setText(title)
            self.lbl_hero_meta.setText(detail)
            self._vfio_notice_until = time.time() + seconds

        def start_gaming_screen(self):
            if self.streamer.is_running:
                return
            if self._background_service_blocks_local_start():
                return
            if self._remote and not self._takeover_pending:
                self._remote_dock()
                return

            if self.vfio_mode:
                if not self._vfio_preflight():
                    return

                want_fullscreen = self.chk_fullscreen.isChecked()
                want_vsync = self.chk_vsync.isChecked()
                want_streamer = self.chk_streamer.isChecked()
                want_v4l2 = self.chk_v4l2.isChecked()
                v4l2_device = V4L2_DEFAULT_DEVICE if want_v4l2 else None
                aspect = self.combo_aspect.currentData() or "auto"
                if aspect not in ASPECT_KEYS:
                    aspect = "auto"
                want_osd = self.chk_osd.isChecked()
                osd_pos = normalize_osd_position(
                    self.combo_osd_pos.currentData() or "top")
                want_isolated_rec_tap = (
                    not self.duo_mode
                    and hasattr(self, "combo_rec_audio")
                    and self.combo_rec_audio.currentData() == "everlive")
                audio_tap = (
                    AUDIO_TAP_NODE
                    if (self.chk_audio_tap.isChecked() or want_isolated_rec_tap)
                    else None)
                want_hdr = self.chk_hdr.isChecked()
                hdr_exposure = float(self.spin_hdr_exposure.value())
                scale_mode = self.combo_scale.currentData() or "fit"
                if scale_mode not in SCALE_MODES:
                    scale_mode = "fit"
                vol = self.slider_vol.value() / 100.0
                fg_cfg = self._framegen_config()

                if v4l2_device and not os.path.exists(v4l2_device):
                    v4l2_device = None
                    self._set_vfio_notice(
                        "Module-Free Mode",
                        f"{V4L2_DEFAULT_DEVICE} absent — running pure userspace "
                        "(SDL2 + PipeWire / Userspace SHM)",
                        seconds=4.0)

                save_settings({
                    "fullscreen": want_fullscreen,
                    "vsync": want_vsync,
                    "streamer_mode": want_streamer,
                    "v4l2_loopback": want_v4l2,
                    "aspect_ratio": aspect,
                    "osd_enabled": want_osd,
                    "osd_position": osd_pos,
                    "audio_tap": self.chk_audio_tap.isChecked(),
                    "hdr_tonemap": want_hdr,
                    "hdr_exposure": hdr_exposure,
                    "scale_mode": scale_mode,
                    **fg_cfg,
                })
                self.streamer.aspect = aspect
                parent_wid = self._target_parent_winid()
                if parent_wid > 0 and hasattr(self, "video_standby_overlay"):
                    self.video_standby_overlay.setVisible(False)
                    QApplication.processEvents()
                success = self.streamer.start(
                    fullscreen=want_fullscreen,
                    vsync=want_vsync,
                    streamer_mode=want_streamer,
                    v4l2_device=v4l2_device,
                    headless=False,
                    aspect=aspect,
                    osd=want_osd,
                    osd_pos=osd_pos,
                    audio_tap=audio_tap,
                    hdr=want_hdr,
                    hdr_exposure=hdr_exposure,
                    scale=scale_mode,
                    volume=vol,
                    framegen=fg_cfg["framegen_mode"],
                    fg_multiplier=fg_cfg["fg_multiplier"],
                    fg_backend=fg_cfg["fg_backend"],
                    fg_dedup=fg_cfg["fg_dedup"],
                    fg_flow_scale=fg_cfg["fg_flow_scale"],
                    fg_perf_mode=fg_cfg["fg_perf_mode"],
                    fg_low_latency=fg_cfg["fg_low_latency"],
                    parent_winid=parent_wid)
                if success:
                    self._set_launch_running(True, "gaming")
                    if getattr(self.streamer, "_stopped_duo_service", False):
                        self._service_was_active = True
                        self.streamer._stopped_duo_service = False
                    if want_fullscreen and parent_wid > 0:
                        self._set_cinema_fullscreen(True)
                    self._vfio_notice_until = 0.0
                    self._vfio_volume_applies = 0
                    self._retry_audio_props()
                    self._check_audio_sink()
                    QTimer.singleShot(350, self._ipc_try_attach)
                else:
                    self._refresh_stage_overlay()
                    self._set_vfio_notice(
                        "Userspace Engine Failed",
                        "See ~/.config/everlive/streamer.log for the hardware error")
                    self._restore_service_after_failed_start()
                    QTimer.singleShot(300, self._restore_led_after_stream)
                return

            preset = self.combo_presets.currentText()
            vol = self.slider_vol.value() / 100.0
            is_fs = self.chk_fullscreen.isChecked()
            is_vsync = self.chk_vsync.isChecked()
            is_streamer = self.chk_streamer.isChecked()
            engine = "mpv" if self.streamer.force_mpv else "native"
            save_settings({
                "preset": preset,
                "volume": vol,
                "fullscreen": is_fs,
                "vsync": is_vsync,
                "streamer_mode": is_streamer,
                "engine": engine,
            })
            success = self.streamer.start(
                preset_name=preset, fullscreen=is_fs, volume=vol,
                vsync=is_vsync, streamer_mode=is_streamer)
            if success:
                self._set_launch_running(True, "gaming")
                set_audio_volume(self.slider_vol.value())
                QTimer.singleShot(600, lambda: set_audio_volume(self.slider_vol.value()))
            elif getattr(self.streamer, "last_error", ""):
                err = self.streamer.last_error
                self._set_vfio_notice("Playback Error", err, seconds=6.0)
                if self.uvc_mode and hasattr(self, "lbl_gc553_status"):
                    self.lbl_gc553_status.setText(err)

        def start_studio(self):
            if self.streamer.is_running:
                return
            if self._background_service_blocks_local_start():
                return
            if not self._vfio_preflight():
                return

            want_v4l2 = self.chk_v4l2.isChecked()
            v4l2_device = V4L2_DEFAULT_DEVICE if (want_v4l2 and os.path.exists(V4L2_DEFAULT_DEVICE)) else None
            if want_v4l2 and not v4l2_device:
                self._set_vfio_notice(
                    "Module-Free Studio",
                    f"{V4L2_DEFAULT_DEVICE} absent — publishing to PipeWire & Userspace SHM",
                    seconds=4.0)

            aspect = self.combo_aspect.currentData() or "auto"
            if aspect not in ASPECT_KEYS:
                aspect = "auto"
            want_isolated_rec_tap = (
                not self.duo_mode
                and hasattr(self, "combo_rec_audio")
                and self.combo_rec_audio.currentData() == "everlive")
            audio_tap = (
                AUDIO_TAP_NODE
                if (self.chk_audio_tap.isChecked() or want_isolated_rec_tap)
                else None)
            want_hdr = self.chk_hdr.isChecked()
            hdr_exposure = float(self.spin_hdr_exposure.value())
            scale_mode = self.combo_scale.currentData() or "fit"
            if scale_mode not in SCALE_MODES:
                scale_mode = "fit"
            vol = self.slider_vol.value() / 100.0
            fg_cfg = self._framegen_config()
            self.streamer.aspect = aspect
            save_settings({
                "v4l2_loopback": want_v4l2,
                "aspect_ratio": aspect,
                "audio_tap": self.chk_audio_tap.isChecked(),
                "hdr_tonemap": want_hdr,
                "hdr_exposure": hdr_exposure,
                "scale_mode": scale_mode,
                **fg_cfg,
            })
            success = self.streamer.start(
                fullscreen=False,
                vsync=False,
                streamer_mode=True,
                v4l2_device=v4l2_device,
                headless=True,
                aspect=aspect,
                audio_tap=audio_tap,
                hdr=want_hdr,
                hdr_exposure=hdr_exposure,
                scale=scale_mode,
                volume=vol,
                framegen=fg_cfg["framegen_mode"],
                fg_multiplier=fg_cfg["fg_multiplier"],
                fg_dedup=fg_cfg["fg_dedup"],
                fg_flow_scale=fg_cfg["fg_flow_scale"],
                fg_perf_mode=fg_cfg["fg_perf_mode"],
                fg_low_latency=fg_cfg["fg_low_latency"])
            if success:
                self._set_launch_running(True, "studio")
                if getattr(self.streamer, "_stopped_duo_service", False):
                    self._service_was_active = True
                    self.streamer._stopped_duo_service = False
                self._vfio_notice_until = 0.0
                self._vfio_volume_applies = 0
                self._retry_audio_props()
                QTimer.singleShot(350, self._ipc_try_attach)
                if v4l2_device:
                    self._set_vfio_notice(
                        "Studio Mode Active",
                        f"Headless capture → {V4L2_DEFAULT_DEVICE} "
                        "('EverLive Capture')",
                        seconds=6.0)
                else:
                    self._set_vfio_notice(
                        "Headless Active",
                        "Pure Userspace: PipeWire & SHM active in background",
                        seconds=6.0)
                self._check_audio_sink()
            else:
                self._set_vfio_notice(
                    "Studio Engine Failed",
                    "See ~/.config/everlive/streamer.log for the hardware error")
                self._restore_service_after_failed_start()
                QTimer.singleShot(300, self._restore_led_after_stream)

        def _set_edid_mode_text(self, mode_value: str):
            if not hasattr(self, "lbl_edid_mode"):
                return
            self.lbl_edid_mode.setText(
                "<span style=\"color:#94A3B8;\">Mode:</span> "
                f"<span style=\"color:#CBD5E1;\">{mode_value}</span>")

        def _duo_edid_action(self, bridge: bool):
            if not self.duo_mode:
                return
            save_settings({"duo_edid_bridge": bool(bridge)})
            self._set_edid_mode_text(
                "Built-in Bridge" if bridge else "Downstream Copy")
            self._update_vdev_label()
            if not self.streamer.is_running:
                self._set_vfio_notice(
                    "EDID Policy Saved",
                    "Start the engine to apply it", seconds=4.0)
                return
            ok = self.streamer.duo_edid_policy(bridge)
            self._set_vfio_notice(
                "EDID Policy Applied" if ok else "EDID Policy Failed",
                "Internal bridge" if bridge else "Downstream copy",
                seconds=5.0)

        def _duo_hpd_action(self):
            if not self.duo_mode or not self.streamer.is_running:
                self._set_vfio_notice(
                    "HPD Unavailable",
                    "Start the engine before pulsing hot-plug", seconds=4.0)
                return
            port = 0
            if hasattr(self, "combo_duo_input"):
                port = 1 if self.combo_duo_input.currentData() == 1 else 0
            ok = self.streamer.duo_hotplug(port)
            self._set_vfio_notice(
                "HPD Pulse Sent" if ok else "HPD Pulse Failed",
                "HDMI 1" if port == 0 else "HDMI 2", seconds=4.0)

        def _update_edid_port_label(self):
            """Reflect the active DUO input in the EDID profile label."""
            if not hasattr(self, "combo_presets"):
                return
            index = self.combo_presets.findData("default")
            if index < 0:
                return
            port = 0
            if self.duo_mode and hasattr(self, "combo_duo_input"):
                port = 1 if self.combo_duo_input.currentData() == 1 else 0
            label = ("Default (1080p60)" if port == 1
                     else "Default (4K60 / 1440p120)")
            self.combo_presets.setItemText(index, label)

        def _apply_card_profile(self):
            """Show, hide and rewire the card-specific controls.

            Runs once after init_ui() and again whenever the card selector
            switches cards in place.  Every card-specific widget is built
            for all cards; this decides what the current card shows.  The
            GC573 and GC570D keep exactly the controls they always had.
            """
            duo = self.duo_mode
            gc573 = self.model == MODEL_GC573
            avermedia = self.model in (MODEL_GC573, MODEL_GC570D)
            profile = self.card_profile

            # Header: card name and the DUO input selector.
            hw = getattr(self.streamer, "hw_info", {}) or {}
            if getattr(self.streamer, "mode", "unknown") != "unknown" or hw.get("pci"):
                self._card_full_label = MODEL_NAMES.get(
                    self.model, MODEL_NAMES[MODEL_GC573])
                self._card_short_label = (
                    "GC570D • DUO" if duo
                    else "HWS • 4K" if self.model == MODEL_HWSUHD
                    else "GC573 • 4K")
                self.lbl_card_title.setStyleSheet("")
            self.combo_duo_input.setVisible(duo)
            if hasattr(self, "combo_card"):
                self.combo_card.blockSignals(True)
                for i in range(self.combo_card.count()):
                    if self.combo_card.itemData(i) == hw.get("pci"):
                        self.combo_card.setCurrentIndex(i)
                self.combo_card.blockSignals(False)

            # EDID & handshake (the HWS UHD card has no host-side EDID).
            self._card_edid.setVisible(profile.supports(CAP_EDID) or avermedia)
            self._set_edid_mode_text(
                "Downstream Copy" if duo else self._edid_profile_label())
            self.btn_edid_bridge.setVisible(duo)
            self.btn_edid_copy.setVisible(duo)
            self._grid_edid.removeWidget(self.btn_edid_hpd)
            self._grid_edid.addWidget(self.btn_edid_hpd, 2 if duo else 1, 0, 1, 2)
            self.btn_edid_hpd.setToolTip(
                "Re-trigger the HDMI hot-plug handshake on the selected input."
                if duo else "Trigger a hardware HDMI handshake (HPD pulse).")

            # EDID profile / preset.
            self._presets_field.setVisible(
                not self.vfio_mode or profile.supports(CAP_EDID) or avermedia)
            self.combo_presets.setEnabled(not duo)
            if duo:
                self.combo_presets.setToolTip(
                    "The Live Gamer DUO selects the EDID policy with the "
                    "EDID: Bridge / Copy buttons in the transport bar.")
            elif self.vfio_mode:
                self.combo_presets.setToolTip(
                    getattr(self, "_presets_tooltip", ""))
            self._update_edid_port_label()

            # HDR tonemapping (the DUO keeps the controls it always showed).
            show_hdr = profile.supports_hdr or avermedia
            self._toggle_rows[self.chk_hdr].setVisible(show_hdr)
            self._hdr_exposure_box.setVisible(show_hdr)

            # Frame generation: GC573 only, as before.
            self._card_fg.setVisible(not duo and profile.supports_framegen)

            # Audio layout: both other cards publish stereo only.
            if duo:
                self.combo_audio_channels.setEnabled(False)
                self.combo_audio_channels.setToolTip(
                    "The Live Gamer DUO engine publishes stereo only.")
            elif self.model == MODEL_HWSUHD:
                self.combo_audio_channels.setEnabled(False)
                self.combo_audio_channels.setToolTip(
                    "This card delivers 48 kHz stereo only.")
            else:
                self.combo_audio_channels.setEnabled(True)
                self.combo_audio_channels.setToolTip("")

            # RGB LED and the GC573-only service audio tap.
            self._card_led.setVisible(profile.supports_rgb_led or avermedia)
            self._toggle_rows[self.chk_service_audio_tap].setVisible(gc573)

            self._update_vdev_label()
            self._apply_responsive_chrome()

        def _card_has_hdr(self) -> bool:
            # The DUO keeps the HDR wording it always showed.
            return (self.card_profile.supports_hdr
                    or self.model in (MODEL_GC573, MODEL_GC570D))

        def _card_has_edid(self) -> bool:
            return (self.card_profile.supports(CAP_EDID)
                    or self.model in (MODEL_GC573, MODEL_GC570D))

        def _kv_hdr_text(self, tonemap_on: str, tonemap_off: str) -> str:
            if not self._card_has_hdr():
                return "SDR"
            return tonemap_on if self.chk_hdr.isChecked() else tonemap_off

        def _kv_link_text(self, first: str) -> str:
            if not self._card_has_edid():
                return first
            return f"{first} · EDID: {self._edid_profile_label()}"

        def _preload_node(self) -> str:
            nodes = getattr(self.card_profile, "preload_nodes", ()) or ()
            return nodes[0] if nodes else "/dev/video63"

        def _on_hpd_clicked(self, *_):
            if self.duo_mode:
                self._duo_hpd_action()
            else:
                self.force_resync()

        def _select_card_combo(self, pci: Optional[str]):
            if not hasattr(self, "combo_card"):
                return
            self.combo_card.blockSignals(True)
            for i in range(self.combo_card.count()):
                if self.combo_card.itemData(i) == pci:
                    self.combo_card.setCurrentIndex(i)
            self.combo_card.blockSignals(False)

        def on_card_changed(self, index: int):
            pci = self.combo_card.itemData(index)
            label = self.combo_card.itemText(index)
            current = getattr(self.streamer, "hw_info", {}).get("pci")
            if not pci or pci == current or getattr(self, "_card_switching", False):
                return
            if self._remote:
                self._select_card_combo(current)
                self._set_vfio_notice(
                    "Remote Session",
                    "Take back capture before switching cards.", seconds=6.0)
                return
            busy = self.streamer.is_running or bool(
                getattr(self, "recorder", None) and self.recorder.is_recording)
            if busy:
                answer = QMessageBox.question(
                    self, "Switch Capture Card",
                    f"Switch to {label}?\n\n"
                    "The current capture and any recording stop.")
                if answer != QMessageBox.Yes:
                    self._select_card_combo(current)
                    return
            if not self.vfio_mode:
                # A kernel-driver session builds a different UI; restart.
                save_settings({"card_pci": pci})
                self._card_restart = pci
                self.close()
                return
            self._switch_card(pci, label)

        def _switch_card(self, pci: str, label: str):
            """Move the Hub to another capture card without restarting."""
            self._card_switching = True
            self._switch_pending = None
            # The new card may be one of the multiview tiles.
            self._multiview_stop()
            if getattr(self, "recorder", None) and self.recorder.is_recording:
                self.recorder.stop()
                self._sync_recording_ui()
            was_running = self.streamer.is_running
            if was_running:
                self.streamer.stop()
                self._set_launch_running(False)
                self._restore_led_after_stream()
            self._restore_headless_service()

            card = next((c for c in detect_capture_cards() if c["pci"] == pci), None)
            if card is None:
                self._card_switch_failed(f"{label} is no longer present.")
                return
            if card["mode"] == "vfio":
                self._finish_card_switch(card, label)
                return
            self._bind_card(card, label)

        def _bind_card(self, card: Dict[str, Optional[str]], label: str):
            """Hand ``card`` to vfio-pci with its bind script (pkexec)."""
            scripts = {
                MODEL_GC573: ("opengc573-userd", "vfio_pci_bind.sh"),
                MODEL_GC570D: ("opengc570d-userd", "gc570d-vfio-bind.sh"),
                MODEL_HWSUHD: ("openhws-userd", "hws-vfio-bind.sh"),
            }
            folder, name = scripts.get(card["model"], ("", ""))
            here = os.path.dirname(os.path.abspath(__file__))
            candidates = [os.path.join(here, "..", folder, name),
                          f"/usr/local/libexec/{name}", f"/usr/libexec/{name}"]
            script = next((os.path.normpath(c) for c in candidates
                           if folder and os.access(c, os.X_OK)), None)
            if script is None or not shutil.which("pkexec"):
                self._card_switch_failed(
                    f"{label} is not bound to vfio-pci; run ./play.sh once.")
                return
            args = [script, "--bind"]
            if card["model"] == MODEL_HWSUHD:
                args.append(card["pci"])
            self._set_vfio_notice(
                "Preparing Capture Card",
                f"Binding {label} to vfio-pci…", seconds=30.0)
            proc = QProcess(self)
            proc.setProcessChannelMode(QProcess.MergedChannels)

            def _done(*_):
                out = bytes(proc.readAll()).decode(errors="replace").strip()
                if out:
                    print(out)
                fresh = next((c for c in detect_capture_cards()
                              if c["pci"] == card["pci"]), None)
                if fresh is not None and fresh["mode"] == "vfio":
                    self._finish_card_switch(fresh, label)
                else:
                    self._card_switch_failed(
                        f"Could not bind {label} to vfio-pci.")
                proc.deleteLater()

            proc.finished.connect(_done)
            proc.start("pkexec", args)

        def _finish_card_switch(self, card: Dict[str, Optional[str]], label: str):
            old = self.streamer
            new = GameStreamer(card=card)
            new.aspect = getattr(old, "aspect", "auto")
            new.ipc = self._ipc
            self.streamer = new
            self.vfio_mode = new.vfio_mode
            self.duo_mode = new.duo_mode
            self.model = new.model
            self.card_profile = detect_card_profile(self.model)
            save_settings({"card_pci": card["pci"]})
            self._apply_card_profile()
            self._card_switching = False
            print(f"[*] Console Hub switched to {label} ({new.model_label}).")
            self._set_vfio_notice("Capture Card", f"Switched to {label}", seconds=5.0)
            self.update_status()

        def _card_switch_failed(self, detail: str):
            self._card_switching = False
            self._select_card_combo(getattr(self.streamer, "hw_info", {}).get("pci"))
            self._set_vfio_notice("Card Switch Failed", detail, seconds=8.0)

        def on_duo_input_changed(self, index: int):
            if not self.duo_mode:
                return
            port = 1 if self.combo_duo_input.itemData(index) == 1 else 0
            self.streamer.set_duo_input(port)
            self._update_edid_port_label()
            label = "HDMI 1" if port == 0 else "HDMI 2"
            if self.streamer.is_running:
                self._set_vfio_notice(
                    f"DUO Input: {label}",
                    "Switch applied to the running engine", seconds=4.0)
            else:
                self._set_vfio_notice(
                    f"DUO Input: {label}",
                    "The next launch will use this input", seconds=4.0)

        def on_aspect_changed(self, *_):
            aspect = self.combo_aspect.currentData() or "auto"
            if aspect not in ASPECT_KEYS:
                aspect = "auto"
            self.saved_cfg["aspect_ratio"] = aspect
            self.streamer.aspect = aspect
            save_settings({"aspect_ratio": aspect})
            if self._ipc is not None and self._ipc.connected:
                self._ipc.set_aspect(aspect)
            elif self.duo_mode and self.streamer.is_running:
                self.streamer.ctl("aspect", aspect)
            elif self.streamer.is_running:
                print("[*] Aspect ratio saved; restart the stream to apply it.")

        def on_scale_changed(self, *_):
            mode = self.combo_scale.currentData() or "fit"
            if mode not in SCALE_MODES:
                mode = "fit"
            self.saved_cfg["scale_mode"] = mode
            self.streamer.scale_mode = mode
            save_settings({"scale_mode": mode})
            if self._ipc is not None and self._ipc.connected:
                self._ipc.set_scale(mode)
            elif self.duo_mode and self.streamer.is_running:
                self.streamer.ctl("scale", mode)
            elif self.streamer.is_running:
                print("[*] Scaling mode saved; restart the stream to apply it.")

        def on_audio_channels_changed(self, *_):
            mode = self.combo_audio_channels.currentData() or "auto"
            if mode not in AUDIO_CHANNEL_KEYS:
                mode = "auto"
            self.saved_cfg["audio_channels"] = mode
            save_settings({"audio_channels": mode})
            if self._ipc is not None and self._ipc.connected:
                if self._ipc.set_audio_channels(mode):
                    print(f"[*] Audio layout -> {mode}")
            elif self.streamer.is_running:
                print("[*] Audio layout saved; restart the stream to apply it.")

        def _update_osd_controls(self):
            if not hasattr(self, "combo_osd_pos"):
                return
            clean_feed = self.chk_streamer.isChecked()
            quicknav_on = bool(
                getattr(self, "chk_quicknav", None)
                and self.chk_quicknav.isChecked())
            allowed = ((self.chk_osd.isChecked() or quicknav_on)
                       and not clean_feed
                       and not bool(getattr(self, "uvc_mode", False)))
            self.combo_osd_pos.setEnabled(allowed)
            if hasattr(self, "osd_pos_effect"):
                self.osd_pos_effect.setOpacity(1.0 if allowed else 0.45)
            if hasattr(self, "chk_quicknav"):
                self.chk_quicknav.setEnabled(not clean_feed)
            if hasattr(self, "chk_quicknav_fs"):
                qn_active = bool(getattr(self, "chk_quicknav", None) and self.chk_quicknav.isChecked())
                self.chk_quicknav_fs.setEnabled(qn_active and not clean_feed)

        def on_quicknav_changed(self, enabled: bool):
            self.saved_cfg["quicknav_enabled"] = bool(enabled)
            save_settings({"quicknav_enabled": bool(enabled)})
            self._update_osd_controls()
            self._sync_osd_mode()

        def on_quicknav_fs_changed(self, enabled: bool):
            self.saved_cfg["quicknav_fs_only"] = bool(enabled)
            save_settings({"quicknav_fs_only": bool(enabled)})
            self._sync_osd_mode()

        def on_osd_changed(self, enabled: bool):
            self.saved_cfg["osd_enabled"] = bool(enabled)
            save_settings({"osd_enabled": bool(enabled)})
            self._update_osd_controls()
            if self._ipc is not None and self._ipc.connected:
                self._ipc.set_osd(bool(enabled))
            elif self.duo_mode and self.streamer.is_running:
                self.streamer.ctl("osd", "1" if enabled else "0")
            elif self.streamer.is_running:
                print("[*] OSD setting saved; use Tab/I for stats and O for "
                      "Quick Navigation in the gaming window.")

        def on_osd_pos_changed(self, *_):
            pos = normalize_osd_position(
                self.combo_osd_pos.currentData() or "top")
            self.saved_cfg["osd_position"] = pos
            save_settings({"osd_position": pos})
            if self._ipc is not None and self._ipc.connected:
                self._ipc.set_osd_position(pos)
            elif self.duo_mode and self.streamer.is_running:
                self.streamer.ctl("osd_pos", pos)

        def on_v4l2_toggled(self, enabled: bool):
            """Persist the preference and apply it live when an engine runs."""
            save_settings({"v4l2_loopback": bool(enabled)})
            self.saved_cfg["v4l2_loopback"] = bool(enabled)

            if self._ipc is None or not self._ipc.connected:
                return  # applied by the next start

            if enabled and not os.path.exists(V4L2_DEFAULT_DEVICE):
                self._set_vfio_notice(
                    "Loopback Node Absent",
                    f"{V4L2_DEFAULT_DEVICE} not found. PipeWire and Userspace SHM (libgc570d-v4l2.so) are already active module-free.",
                    seconds=6.0)
                self.chk_v4l2.blockSignals(True)
                self.chk_v4l2.setChecked(False)
                self.chk_v4l2.blockSignals(False)
                save_settings({"v4l2_loopback": False})
                self.saved_cfg["v4l2_loopback"] = False
                return

            if self._ipc.set_v4l2(bool(enabled), V4L2_DEFAULT_DEVICE):
                self._set_vfio_notice(
                    "Virtual Camera",
                    (f"Dynamically attached to {V4L2_DEFAULT_DEVICE}"
                     if enabled else "Dynamically detached"),
                    seconds=5.0)
                print("[*] Virtual Camera "
                      f"{'attached' if enabled else 'detached'} live")
            else:
                self._set_vfio_notice(
                    "Virtual Camera",
                    "Live switch refused; it applies on the next start",
                    seconds=8.0)

        def on_hdr_toggled(self, enabled: bool):
            save_settings({"hdr_tonemap": bool(enabled)})
            self.saved_cfg["hdr_tonemap"] = bool(enabled)
            if hasattr(self, "spin_hdr_exposure"):
                self.spin_hdr_exposure.setEnabled(bool(enabled))
            if hasattr(self, "slider_hdr_exposure"):
                self.slider_hdr_exposure.setEnabled(bool(enabled))
            if hasattr(self, "btn_hdr_exp_reset"):
                self.btn_hdr_exp_reset.setEnabled(bool(enabled))
            if self._ipc is not None and self._ipc.connected:
                self._ipc.set_hdr(bool(enabled),
                                  float(self.spin_hdr_exposure.value()))
            elif self.streamer.is_running:
                print("[*] HDR setting saved; applies on the next start.")

        def on_hdr_exposure_changed(self, value: float):
            save_settings({"hdr_exposure": float(value)})
            self.saved_cfg["hdr_exposure"] = float(value)
            if self._ipc is not None and self._ipc.connected:
                self._ipc.set_hdr(self.chk_hdr.isChecked(), float(value))

        # -----------------------------------------------------------------
        # Frame generation & CAS
        # -----------------------------------------------------------------
        def _framegen_config(self) -> Dict:
            mode = self.combo_framegen.currentData() or "off"
            if mode not in FRAMEGEN_KEYS:
                mode = "off"
            mult = int(mode[0]) if mode in ("2x", "3x", "4x") else int(self.saved_cfg.get("fg_multiplier", 2))
            backend = self.combo_fg_backend.currentData() if hasattr(self, "combo_fg_backend") else self.saved_cfg.get("fg_backend", "lsfg")
            if backend not in ("lsfg", "native"):
                backend = "lsfg"
            dedup = self.chk_fg_dedup.isChecked() if hasattr(self, "chk_fg_dedup") else False
            return {
                "framegen_mode": mode,
                "fg_multiplier": max(2, min(4, mult)),
                "fg_backend": backend,
                "fg_dedup": dedup,
                "fg_flow_scale": max(0.5, min(1.0,
                                             self.slider_flow.value() / 100.0)),
                "fg_perf_mode": self.chk_fg_perf.isChecked(),
                "fg_low_latency": self.chk_fg_ll.isChecked(),
            }

        def _apply_framegen(self, persist: bool = True):
            if not hasattr(self, "combo_framegen"):
                return
            cfg = self._framegen_config()
            self.saved_cfg.update(cfg)
            if persist:
                save_settings(cfg)
            if self._ipc is not None and self._ipc.connected:
                wire_mode = "off" if cfg["framegen_mode"] == "off" else (
                    "30to60" if cfg["fg_dedup"] else "60to120")
                self._ipc.set_framegen(
                    wire_mode, cfg["fg_flow_scale"],
                    cfg["fg_perf_mode"], cfg["fg_low_latency"],
                    multiplier=cfg["fg_multiplier"],
                    dedup=cfg["fg_dedup"],
                    backend=cfg["fg_backend"])
            else:
                self._refresh_idle_fg_status()
                if self.streamer.is_running:
                    print("[*] Frame generation saved; applies on the next start.")

        def on_framegen_changed(self, *_):
            self._apply_framegen()

        def _sync_fg_backend_controls(self):
            for w_attr in ("slider_flow", "lbl_flow", "chk_fg_perf", "chk_fg_ll"):
                w = getattr(self, w_attr, None)
                if w is not None:
                    w.setEnabled(True)

        def on_fg_backend_changed(self, *_):
            self._sync_fg_backend_controls()
            self._refresh_fg_readiness()
            self._apply_framegen()

        def on_fg_dedup_toggled(self, _enabled: bool):
            self._apply_framegen()

        def on_fg_perf_toggled(self, _enabled: bool):
            self._apply_framegen()

        def on_fg_low_latency_toggled(self, _enabled: bool):
            self._apply_framegen()

        def on_flow_scale_changed(self, value: int):
            self.lbl_flow.setText(f"{value}%")
            # Live while dragging, persisted once the handle is released.
            self._apply_framegen(persist=not self.slider_flow.isSliderDown())

        def _set_fg_readiness(self, layer_found: bool, dll_found: bool,
                              dll_path: str = "",
                              legacy_dll_found: Optional[bool] = None):
            """Colour the readiness badge green/amber with the exact fix."""
            backend = (self.combo_fg_backend.currentData()
                       if hasattr(self, "combo_fg_backend")
                       else self.saved_cfg.get("fg_backend", "lsfg"))
            self._fg_ready_ok = bool(layer_found and dll_found)
            if not hasattr(self, "lbl_fg_ready"):
                return
            if layer_found and dll_found:
                name = os.path.basename(dll_path) if dll_path else "lsfg-vk.dll"
                self.lbl_fg_ready.setText("✓ Ready")
                self.lbl_fg_ready.setStyleSheet(
                    "color: #5ED4B2; font-size: 11px; font-weight: 700;")
                self.lbl_fg_ready.setToolTip(f"LSFG-VK Ready · {name} found\n{dll_path}")
                return

            if not layer_found:
                message = ("Missing lsfg-vk package "
                           "(sudo pacman -S lsfg-vk)")
                short_msg = "⚠ No Layer"
            else:
                if legacy_dll_found is None:
                    try:
                        legacy_dll_found = bool(
                            probe_lsfg_vk().get("legacy_dll_found"))
                    except Exception:
                        legacy_dll_found = False
                if legacy_dll_found:
                    message = ("Select 'lsfg-vk' Beta in Steam "
                               "(Lossless Scaling -> Properties -> Betas)")
                    short_msg = "⚠ Beta Req."
                else:
                    message = ("Missing Lossless Scaling in Steam "
                               "(enable 'lsfg-vk' Beta)")
                    short_msg = "⚠ No DLL"
            self.lbl_fg_ready.setText(short_msg)
            self.lbl_fg_ready.setStyleSheet(
                "color: #F5C84B; font-size: 11px; font-weight: 700;")
            self.lbl_fg_ready.setToolTip(
                f"{message}\n"
                "Install the lsfg-vk Vulkan layer and opt Lossless Scaling "
                "into the 'lsfg-vk' Beta in Steam, then press Re-check.")

        def _refresh_fg_readiness(self):
            """Offline probe so readiness is visible before any stream starts."""
            try:
                probe = probe_lsfg_vk()
            except Exception:
                probe = {"layer_found": False, "dll_found": False,
                         "legacy_dll_found": False, "dll_path": ""}
            self._set_fg_readiness(
                bool(probe.get("layer_found")), bool(probe.get("dll_found")),
                str(probe.get("dll_path") or ""),
                bool(probe.get("legacy_dll_found")))

        def on_fg_recheck(self):
            self._refresh_fg_readiness()
            if not (self._ipc is not None and self._ipc.connected):
                self._refresh_idle_fg_status()

        def _refresh_idle_fg_status(self):
            """Reflect the selected Frame Generation preset while idle."""
            cfg = self._framegen_config()
            mode = cfg["framegen_mode"]
            mult = int(cfg.get("fg_multiplier") or 2)
            dedup = bool(cfg.get("fg_dedup"))
            if mode == "off":
                self._fg_summary = ""
                self._fg_fps_badge = ""
            elif not getattr(self, "_fg_ready_ok", False):
                self._fg_summary = "LSFG-VK Missing"
                self._fg_fps_badge = ""
            else:
                dedup_tag = " (Dedup)" if dedup else ""
                self._fg_summary = f"LSFG {mult}x{dedup_tag}: Standby"
                self._fg_fps_badge = ""
            self._fg_mult = mult
            self._fg_present_fps = 0.0
            self._update_telemetry_cards(getattr(self, "_remote_status", None))

        def _update_framegen_status(self, fg: Dict, headless: bool = False):
            """Render the engine's live cadence into telemetry summaries."""
            if not isinstance(fg, dict):
                return
            raw_mode = str(fg.get("mode", "off")).lower()
            mult = max(2, min(4, int(fg.get("multiplier") or 2)))
            present = float(fg.get("present_fps") or 0.0)
            output = float(fg.get("output_fps") or 0.0)
            dedup_live = bool(fg.get("deduplicator_active")) or raw_mode == "30to60"
            self._fg_mult = mult

            if raw_mode == "off":
                self._fg_summary = ""
                self._fg_fps_badge = ""
                self._fg_present_fps = 0.0
                self._fg_output_fps = 0.0
                return

            is_native = fg.get("backend") == "native"
            if not is_native and not (fg.get("layer_found") and fg.get("dll_found")):
                self._fg_summary = "LSFG-VK Missing"
                self._fg_fps_badge = ""
                self._fg_present_fps = 0.0
                self._fg_output_fps = 0.0
                return

            dedup_tag = " (Dedup)" if dedup_live else ""
            if headless:
                tag = "Native" if is_native else "LSFG"
                self._fg_summary = f"{tag} {mult}x{dedup_tag}: Armed"
                self._fg_fps_badge = ""
                self._fg_present_fps = 0.0
                self._fg_output_fps = 0.0
                return

            if not is_native and fg.get("vulkan_active") is False:
                self._fg_summary = "OpenGL Fallback (No FG)"
                self._fg_fps_badge = ""
                self._fg_present_fps = 0.0
                self._fg_output_fps = 0.0
                return

            tag = "Native" if is_native else "LSFG"
            self._fg_summary = f"{tag} {mult}x{dedup_tag}: {present:.0f}→{output:.0f} FPS"
            self._fg_fps_badge = f"{present:.0f}→{output:.0f} FPS ({mult}x)"
            self._fg_present_fps = present
            self._fg_output_fps = output

        def _find_drm_gpu_device(self) -> Optional[str]:
            cached = getattr(self, "_drm_device_dir", None)
            if cached is not None:
                return cached or None
            best_dir = ""
            best_vram = -1
            try:
                for entry in sorted(os.listdir("/sys/class/drm")):
                    if not entry.startswith("card") or "-" in entry:
                        continue
                    dev_dir = os.path.join("/sys/class/drm", entry, "device")
                    busy_path = os.path.join(dev_dir, "gpu_busy_percent")
                    if not os.path.isfile(busy_path):
                        continue
                    vram_total = 0
                    vram_path = os.path.join(dev_dir, "mem_info_vram_total")
                    if os.path.isfile(vram_path):
                        try:
                            with open(vram_path, "r", encoding="utf-8") as vf:
                                vram_total = int(vf.read().strip())
                        except Exception:
                            vram_total = 0
                    if vram_total > best_vram:
                        best_vram = vram_total
                        best_dir = dev_dir
            except Exception:
                pass
            self._drm_device_dir = best_dir
            return best_dir or None

        def _sample_host_metrics(self) -> Dict[str, object]:
            """Sample lightweight sysfs/procfs CPU, engine process, and GPU metrics."""
            now = time.monotonic()
            last_sample_ts = getattr(self, "_host_metrics_ts", 0.0)
            cached = getattr(self, "_host_metrics_cache", None)
            if cached is not None and (now - last_sample_ts) < 0.45:
                return cached

            cpu_pct = getattr(self, "_last_cpu_pct", 0.0)
            try:
                with open("/proc/stat", "r", encoding="utf-8") as f:
                    first = f.readline()
                if first.startswith("cpu "):
                    parts = [int(x) for x in first.split()[1:9]]
                    total = sum(parts)
                    idle = parts[3] + (parts[4] if len(parts) > 4 else 0)
                    prev = getattr(self, "_prev_cpu_stat", None)
                    self._prev_cpu_stat = (total, idle)
                    if prev is not None:
                        dt = total - prev[0]
                        di = idle - prev[1]
                        if dt > 0:
                            cpu_pct = max(0.0, min(100.0, 100.0 * (dt - di) / dt))
                            self._last_cpu_pct = cpu_pct
            except Exception:
                pass

            eng_pid = 0
            if (self.streamer.mpv_proc is not None and
                    self.streamer.mpv_proc.poll() is None):
                eng_pid = int(self.streamer.mpv_proc.pid or 0)
            elif isinstance(getattr(self, "_remote_status", None), dict):
                eng_pid = int(self._remote_status.get("pid") or 0)

            eng_cpu_pct: Optional[float] = None
            if eng_pid > 0:
                try:
                    with open(f"/proc/{eng_pid}/stat", "r", encoding="utf-8") as pf:
                        raw = pf.read()
                    rpar = raw.rfind(")")
                    if rpar > 0:
                        fields = raw[rpar + 2:].split()
                        proc_ticks = int(fields[11]) + int(fields[12])
                        prev_proc = getattr(self, "_prev_proc_stat", None)
                        self._prev_proc_stat = (eng_pid, proc_ticks, now)
                        if prev_proc is not None and prev_proc[0] == eng_pid:
                            dt_s = now - prev_proc[2]
                            clk = float(os.sysconf("SC_CLK_TCK") or 100)
                            if dt_s > 0.05 and clk > 0:
                                eng_cpu_pct = max(
                                    0.0,
                                    100.0 * (proc_ticks - prev_proc[1]) / (clk * dt_s))
                                self._last_eng_cpu_pct = eng_cpu_pct
                            else:
                                eng_cpu_pct = getattr(self, "_last_eng_cpu_pct", 0.0)
                except Exception:
                    self._prev_proc_stat = None
            else:
                self._prev_proc_stat = None

            gpu_pct: Optional[int] = None
            vram_used_gb: Optional[float] = None
            vram_total_gb: Optional[float] = None
            dev_dir = self._find_drm_gpu_device()
            if dev_dir:
                try:
                    with open(os.path.join(dev_dir, "gpu_busy_percent"),
                              "r", encoding="utf-8") as gf:
                        gpu_pct = int(gf.read().strip())
                except Exception:
                    gpu_pct = None
                try:
                    with open(os.path.join(dev_dir, "mem_info_vram_used"),
                              "r", encoding="utf-8") as uf, \
                         open(os.path.join(dev_dir, "mem_info_vram_total"),
                              "r", encoding="utf-8") as tf:
                        u_bytes = int(uf.read().strip())
                        t_bytes = int(tf.read().strip())
                        if t_bytes > 0:
                            vram_used_gb = u_bytes / (1024.0 ** 3)
                            vram_total_gb = t_bytes / (1024.0 ** 3)
                except Exception:
                    pass

            result = {
                "cpu_pct": cpu_pct,
                "eng_cpu_pct": eng_cpu_pct,
                "gpu_pct": gpu_pct,
                "vram_used_gb": vram_used_gb,
                "vram_total_gb": vram_total_gb,
            }
            self._host_metrics_ts = now
            self._host_metrics_cache = result
            return result

        def _update_telemetry_cards(self, status: Optional[Dict] = None):
            """Keep the bottom visual telemetry cards and top badge updated with live metrics."""
            if not hasattr(self, "lbl_tele_pipeline") or not hasattr(self, "lbl_tele_routing"):
                return

            if status is None and isinstance(getattr(self, "_remote_status", None), dict):
                status = self._remote_status

            host = self._sample_host_metrics()
            cpu_pct = float(host.get("cpu_pct") or 0.0)
            eng_cpu_pct = host.get("eng_cpu_pct")
            gpu_pct = host.get("gpu_pct")
            vram_used = host.get("vram_used_gb")
            vram_total = host.get("vram_total_gb")

            gpu_str = f"{gpu_pct}%" if isinstance(gpu_pct, int) else "—"
            eng_str = (f" (Engine {eng_cpu_pct:.1f}%)"
                       if isinstance(eng_cpu_pct, (int, float)) else "")

            m = getattr(self.streamer, "_vfio_metrics", {}) or {}
            video = (status or {}).get("video") if isinstance(status, dict) else None
            signal_d = (status or {}).get("signal") if isinstance(status, dict) else None
            headless = bool((status or {}).get("headless")) if isinstance(status, dict) else (self._launch_mode == "studio")

            active_stream = bool(self.streamer.is_running or self._remote)
            fps = 0.0
            lat_ms = 0.0
            drops = 0
            out_w = 0
            out_h = 0
            underruns = int(m.get("audio_underruns") or 0)
            sdl_bytes = int(m.get("audio_sdl_bytes") or 0)

            if active_stream:
                if isinstance(video, dict):
                    fps = float(video.get("fps") or 0.0)
                    lat_ms = float(video.get("latency_ms") or 0.0)
                    if "dropped" in video:
                        drops = int(video.get("dropped") or 0)
                    else:
                        drops = int(m.get("drops") or 0)
                    out_w = int(video.get("output_width") or 0)
                    out_h = int(video.get("output_height") or 0)
                if fps <= 0.0:
                    fps = float(m.get("fps") or 0.0)
                if lat_ms <= 0.0:
                    lat_ms = float(m.get("latency_ms") or 0.0)
                if out_w <= 0 or out_h <= 0:
                    out_w = int(m.get("out_w") or 0)
                    out_h = int(m.get("out_h") or 0)
                if (out_w <= 0 or out_h <= 0) and isinstance(signal_d, dict):
                    out_w = int(signal_d.get("width") or 0)
                    out_h = int(signal_d.get("height") or 0)
                if (out_w <= 0 or out_h <= 0) and isinstance(getattr(self, "_live", None), dict):
                    res_m = re.match(r"(\d+)x(\d+)", str(self._live.get("resolution") or ""))
                    if res_m:
                        out_w = int(res_m.group(1))
                        out_h = int(res_m.group(2))

            dma_mb_s = (out_w * out_h * 2.0 * fps) / (1024.0 * 1024.0) if (out_w > 0 and out_h > 0 and fps > 0.0) else 0.0
            dma_gbps = (dma_mb_s * 8.0) / 1000.0
            fg_fps_badge = getattr(self, "_fg_fps_badge", "") or ""
            fg_output_fps = float(getattr(self, "_fg_output_fps", 0.0) or 0.0)
            fps_disp = fg_fps_badge if fg_fps_badge else f"{fps:.1f} FPS"

            target_fps = 60.0
            if fps > 80.0:
                target_fps = 120.0
            elif 20.0 <= fps <= 36.0:
                target_fps = 30.0
            budget_ms = 1000.0 / max(fps if fps > 10.0 else 60.0, 24.0)
            if hasattr(self, "lbl_budget_note"):
                self.lbl_budget_note.setText(f"Budget {budget_ms:.1f} ms / frame")

            # --- Meter 1: SIGNAL & HDMI LINK ---
            is_locked = bool(
                (isinstance(signal_d, dict) and signal_d.get("locked"))
                or (isinstance(getattr(self, "_live", None), dict) and self._live.get("locked"))
            )
            if hasattr(self, "lbl_sig_tag"):
                if active_stream and is_locked:
                    self.lbl_sig_tag.setText("● LOCKED")
                    self.lbl_sig_tag.setStyleSheet(
                        "color: #7FD8C0; background: transparent; "
                        "border: none; padding: 0px; "
                        "font-size: 13px; font-weight: 800;")
                elif active_stream:
                    self.lbl_sig_tag.setText("● NO SIGNAL")
                    self.lbl_sig_tag.setStyleSheet(
                        "color: #E8B36B; background: transparent; "
                        "border: none; padding: 0px; "
                        "font-size: 13px; font-weight: 800;")
                else:
                    self.lbl_sig_tag.setText("● STANDBY")
                    self.lbl_sig_tag.setStyleSheet(
                        "color: #9AA0B0; background: transparent; "
                        "border: none; padding: 0px; "
                        "font-size: 13px; font-weight: 800;")
            if hasattr(self, "lbl_kv_route"):
                want_v4l2 = hasattr(self, "chk_v4l2") and self.chk_v4l2.isChecked()
                want_tap = hasattr(self, "chk_audio_tap") and self.chk_audio_tap.isChecked()
                route_txt = (f"V4L2 ({V4L2_DEFAULT_DEVICE})" if want_v4l2
                             else f"SHM ({self._preload_node()})")
                if want_tap:
                    route_txt += " · Tap"
                self.lbl_kv_route.setText(route_txt)

            # --- Meter 2: PIPELINE & HARDWARE LOAD (VeyraMetricBars + compat labels) ---
            if active_stream and fps > 0.2:
                if headless:
                    self.lbl_tele_pipeline.setText(f"Headless DMA · {fps:.1f} FPS")
                elif lat_ms > 0.0:
                    self.lbl_tele_pipeline.setText(f"{lat_ms:.2f} ms Latency · {fps_disp}")
                else:
                    frame_time_ms = 1000.0 / max(fps, 1.0)
                    self.lbl_tele_pipeline.setText(f"~{frame_time_ms:.1f} ms Frame · {fps_disp}")
                self.lbl_tele_routing.setText(
                    f"DMA: {dma_mb_s:.0f} MB/s ({dma_gbps:.2f} Gbps) · "
                    f"Drops: {drops} · Underruns: {underruns}"
                )
                if hasattr(self, "bar_latency"):
                    eff_lat = lat_ms if lat_ms > 0.0 else (1000.0 / max(fps, 1.0))
                    lat_txt = (f"{lat_ms:.2f} ms" if lat_ms > 0.0
                               else ("Headless DMA" if headless else f"~{eff_lat:.1f} ms"))
                    self.bar_latency.set_metric(
                        eff_lat / budget_ms, lat_txt, known=True,
                        tip=f"Capture-to-present latency vs {budget_ms:.1f} ms frame budget"
                    )
                if hasattr(self, "bar_dma"):
                    self.bar_dma.set_metric(
                        dma_mb_s / 1000.0,
                        f"{dma_mb_s:.0f} MB/s ({dma_gbps:.2f}G)",
                        known=True,
                        tip=f"PCIe Gen2 x4 Ring-3 DMA throughput: {dma_mb_s:.1f} MB/s ({dma_gbps:.2f} Gbps)"
                    )
            else:
                self.lbl_tele_pipeline.setText("— ms Latency · 0.0 FPS")
                self.lbl_tele_routing.setText(
                    f"DMA: 0 MB/s · Drops: {drops} · Audio Underruns: {underruns}"
                )
                if hasattr(self, "bar_latency"):
                    self.bar_latency.set_metric(0.0, "Idle", known=False)
                if hasattr(self, "bar_dma"):
                    self.bar_dma.set_metric(0.0, "0 MB/s", known=False)

            if hasattr(self, "bar_cpu"):
                cpu_bar_txt = (f"{cpu_pct:.1f}% (Eng {eng_cpu_pct:.1f}%)"
                               if isinstance(eng_cpu_pct, (int, float))
                               else f"{cpu_pct:.1f}%")
                self.bar_cpu.set_metric(
                    cpu_pct / 100.0, cpu_bar_txt, known=True,
                    tip="Host CPU utilization (and EverLive capture engine process CPU %)"
                )

            if hasattr(self, "bar_vram"):
                if isinstance(vram_used, float) and isinstance(vram_total, float) and vram_total > 0:
                    self.bar_vram.set_metric(
                        vram_used / vram_total,
                        f"{vram_used:.1f} / {vram_total:.1f} GB",
                        known=True,
                        tip=f"GPU VRAM usage: {vram_used:.2f} GiB of {vram_total:.2f} GiB"
                    )
                else:
                    self.bar_vram.set_metric(0.0, "N/A", known=False)

            # --- Meter 3: FRAME RATE & CADENCE (Hero FPS + Sparkline + 3 Orbs + StatusLight) ---
            if hasattr(self, "lbl_tele_cadence"):
                self.lbl_tele_cadence.setText(f"CPU: {cpu_pct:.1f}%{eng_str} · GPU: {gpu_str}")

            a_rate = int(m.get("audio_rate") or 48000)
            a_ch = int(m.get("audio_ch") or 2)
            bytes_per_ms = max(192.0, (a_rate * a_ch * 2.0) / 1000.0)

            if hasattr(self, "lbl_fg_status"):
                sub_parts = []
                if isinstance(vram_used, float) and isinstance(vram_total, float):
                    sub_parts.append(f"VRAM: {vram_used:.1f} / {vram_total:.1f} GB")
                if active_stream and sdl_bytes > 0:
                    audio_ms = sdl_bytes / bytes_per_ms
                    sub_parts.append(f"Audio Buf: {audio_ms:.1f} ms")
                fg_summary = getattr(self, "_fg_summary", "") or ""
                if fg_summary:
                    sub_parts.append(fg_summary)
                self.lbl_fg_status.setText(" · ".join(sub_parts) if sub_parts else "VRAM: —")

            if hasattr(self, "lbl_card_title") and hasattr(self, "lbl_pill"):
                pill_txt = (self.lbl_pill.text() or "Standby").lstrip("●○ ").strip()
                pci = getattr(self.streamer, "hw_info", {}).get("pci") or ""
                tip = f"Status: {pill_txt}"
                if pci:
                    tip += f" · PCI {pci}"
                self.lbl_card_title.setToolTip(tip)

            if hasattr(self, "lbl_fps_hero") and hasattr(self, "fps_sparkline"):
                if active_stream and fps > 0.2:
                    if fg_fps_badge and fg_output_fps > 0.0:
                        fg_mult = int(getattr(self, "_fg_mult", 2) or 2)
                        base_fps = float(getattr(self, "_fg_present_fps", 0.0) or 0.0) or fps
                        self.lbl_fps_hero.setText(f"{fg_output_fps:.0f}")
                        self.lbl_fps_hero.setStyleSheet(
                            f"color: #FF9EBC; font-family: {ui_theme.MONO_STACK}; "
                            "font-size: 24px; font-weight: 800; background: transparent; border: none;"
                        )
                        if hasattr(self, "lbl_fps_unit"):
                            self.lbl_fps_unit.setText(f"fps ({fg_mult}x from {base_fps:.0f})")
                        self.fps_sparkline.push_sample(
                            fg_output_fps, target=base_fps * fg_mult, accent_hex="#FF9EBC")
                    else:
                        self.lbl_fps_hero.setText(f"{fps:.1f}")
                        self.lbl_fps_hero.setStyleSheet(
                            f"color: #7EE8C8; font-family: {ui_theme.MONO_STACK}; "
                            "font-size: 24px; font-weight: 800; background: transparent; border: none;"
                        )
                        if hasattr(self, "lbl_fps_unit"):
                            self.lbl_fps_unit.setText("fps")
                        self.fps_sparkline.push_sample(
                            fps, target=target_fps, accent_hex="#7EE8C8")
                else:
                    self.lbl_fps_hero.setText("0.0")
                    self.lbl_fps_hero.setStyleSheet(
                        f"color: #FFF6FB; font-family: {ui_theme.MONO_STACK}; "
                        "font-size: 24px; font-weight: 800; background: transparent; border: none;"
                    )
                    if hasattr(self, "lbl_fps_unit"):
                        self.lbl_fps_unit.setText("fps")
                    self.fps_sparkline.clear_samples()

            if hasattr(self, "lbl_drops_counter"):
                self.lbl_drops_counter.setText(f"{drops} drops · {underruns} underrun")
                if drops > 0 or underruns > 0:
                    self.lbl_drops_counter.setStyleSheet(
                        f"color: #F5C84B; font-family: {ui_theme.MONO_STACK}; "
                        "font-size: 11px; font-weight: 700; background: transparent; border: none;"
                    )
                else:
                    self.lbl_drops_counter.setStyleSheet(
                        f"color: #ACA5C0; font-family: {ui_theme.MONO_STACK}; "
                        "font-size: 11px; font-weight: 500; background: transparent; border: none;"
                    )

            if hasattr(self, "orb_budget"):
                if active_stream and fps > 0.2 and lat_ms > 0.0:
                    self.orb_budget.set_state(
                        lat_ms / budget_ms, f"{lat_ms:.1f}ms", known=True,
                        tip=f"Capture-to-present latency ({lat_ms:.2f} ms) vs {budget_ms:.1f} ms frame budget"
                    )
                elif active_stream and fps > 0.2:
                    self.orb_budget.set_state(
                        0.08, "DMA", known=True,
                        tip="Zero-copy Ring-3 DMA active"
                    )
                else:
                    self.orb_budget.set_state(0.0, "—", known=False)

            if hasattr(self, "orb_gpu"):
                if isinstance(gpu_pct, int):
                    self.orb_gpu.set_state(
                        gpu_pct / 100.0, f"{gpu_pct}%", known=True,
                        tip=f"GPU engine load: {gpu_pct}%"
                    )
                else:
                    self.orb_gpu.set_state(0.0, "—", known=False)

            if hasattr(self, "orb_cpu"):
                self.orb_cpu.set_state(
                    cpu_pct / 100.0, f"{cpu_pct:.0f}%", known=True,
                    tip=f"System CPU load: {cpu_pct:.1f}%{eng_str}"
                )

            if hasattr(self, "status_light"):
                fg_summary = getattr(self, "_fg_summary", "") or ""
                if active_stream and fps > 0.2:
                    light_parts = []
                    if sdl_bytes > 0:
                        audio_ms = sdl_bytes / bytes_per_ms
                        light_parts.append(f"Audio Buf {audio_ms:.1f} ms")
                    else:
                        light_parts.append(f"DMA {dma_gbps:.2f} Gbps")
                    if fg_summary:
                        light_parts.append(fg_summary)
                    else:
                        light_parts.append("Zero-Copy Active")
                    lvl = "warn" if (drops > 0 or underruns > 0) else ("accent" if fg_fps_badge else "ok")
                    self.status_light.set_status(lvl, " · ".join(light_parts))
                else:
                    idle_txt = f"Standby · {fg_summary}" if fg_summary else "Standby · Waiting for HDMI input"
                    self.status_light.set_status("idle", idle_txt)

            self._refresh_signal_badge()

        def on_preset_changed(self, *_):
            if self.vfio_mode:
                self._on_edid_profile_changed()
                return
            preset = self.combo_presets.currentText()
            save_settings({"preset": preset})
            if self.streamer.is_running:
                self.streamer.stop()
                QTimer.singleShot(150, self.start_gaming_screen)

        def _edid_profile_label(self) -> str:
            profile = self.saved_cfg.get("edid_profile", "default")
            if profile == "custom":
                path = self.saved_cfg.get("edid_custom_path", "")
                return os.path.basename(path) if path else "default"
            return profile

        def _update_vdev_label(self):
            if not self.vfio_mode or not hasattr(self, "lbl_vdev"):
                return
            pci = self.streamer.hw_info.get("pci") or "auto"
            if self.duo_mode:
                if self.streamer.v4l2_device:
                    out = (f"{self.streamer.v4l2_device}"
                           f" + {self.streamer.v4l2_device_2 or V4L2_DUO_SECOND_DEVICE}")
                else:
                    out = "PipeWire & SHM (/dev/video63 + /dev/video62)"
                edid = ("internal bridge"
                        if load_settings().get("duo_edid_bridge", True)
                        else "downstream copy")
                self.lbl_vdev.setText(
                    f"PCI: {pci} • EDID: {edid} • Out: {out}")
                return
            loop = (f" • Out: {self.streamer.v4l2_device}"
                    if getattr(self.streamer, "v4l2_device", None)
                    else f" • Out: SHM ({self._preload_node()}) & PipeWire")
            self.lbl_vdev.setText(self._kv_link_text(f"PCI: {pci}").replace(" · ", " • ") + loop)
            if hasattr(self, "bottom_box"):
                self.bottom_box.setToolTip(self.lbl_vdev.text())

        def _restore_edid_selection(self):
            idx = self.combo_presets.findData(
                self.saved_cfg.get("edid_profile", "default"))
            if idx < 0:
                idx = self.combo_presets.findData("default")
            if idx >= 0:
                self.combo_presets.blockSignals(True)
                self.combo_presets.setCurrentIndex(idx)
                self.combo_presets.blockSignals(False)

        def _commit_edid_selection(self, profile: str, custom_path: str = ""):
            self.saved_cfg["edid_profile"] = profile
            settings = {"edid_profile": profile}
            if custom_path:
                self.saved_cfg["edid_custom_path"] = custom_path
                settings["edid_custom_path"] = custom_path
            save_settings(settings)

            if profile == "custom":
                idx = self.combo_presets.findData("custom")
                if idx >= 0 and custom_path:
                    self.combo_presets.blockSignals(True)
                    self.combo_presets.setItemText(
                        idx, f"Custom: {os.path.basename(custom_path)}")
                    self.combo_presets.setCurrentIndex(idx)
                    self.combo_presets.blockSignals(False)

            self._update_vdev_label()
            if self.streamer.is_running:
                target_edid = custom_path if profile == "custom" else profile
                if (self._ipc is not None and self._ipc.connected and
                        target_edid and
                        self._ipc.request("edid.set", {"profile": target_edid})):
                    self._set_vfio_notice(
                        "EDID Applied Live",
                        f"Reprogrammed IT6805/IT6664 DDC RAM to '{self._edid_profile_label()}'",
                        seconds=4.0)
                else:
                    print("[*] EDID profile saved; restart the Gaming Screen to apply it.")

        def _on_edid_profile_changed(self):
            key = self.combo_presets.currentData()
            if key != "custom":
                self._commit_edid_selection(key or "default")
                return

            path, _ = QFileDialog.getOpenFileName(
                self, "Load custom EDID",
                self.saved_cfg.get("edid_custom_path", ""),
                "EDID files (*.bin *.edid);;All files (*)")
            if not path:
                self._restore_edid_selection()
                return
            try:
                size = os.path.getsize(path)
            except OSError:
                size = 0
            if size < 256:
                QMessageBox.warning(
                    self, "Invalid EDID",
                    "The selected file must contain at least 256 bytes "
                    "(one base block plus one CEA-861 extension block).")
                self._restore_edid_selection()
                return
            self._commit_edid_selection("custom", path)

        def on_led_changed(self, index: int):
            mode = self.combo_led.currentData()
            if mode is not None:
                self.apply_led(mode)

        def _gc553_usb_port(self) -> Optional[str]:
            return getattr(self.streamer, "hw_info", {}).get("usb_port")

        def _refresh_gc553_status(self, include_at: bool = True):
            if not (self.uvc_mode and _gc553g2_control is not None and hasattr(self, "lbl_gc553_status")):
                return
            try:
                st = _gc553g2_control.query_device_status(
                    port=self._gc553_usb_port(), include_at=include_at
                )
            except Exception as exc:
                self.lbl_gc553_status.setText(f"GC553G2 query error: {exc}")
                return

            speed = st.get("speed_mbps")
            speed_txt = f"{speed} Mbps" if speed else "USB ?"
            if not st.get("superspeed_10g"):
                speed_txt += " (10Gbps USB 3.2 Gen 2 recommended)"
            rates = st.get("mjpeg_4k_rates") or []
            rates_txt = f"4K MJPEG {rates}fps" if rates else "V4L2 ready"
            parts = [f"{st.get('video_node') or 'no video'}", speed_txt, rates_txt]

            if st.get("firmware"):
                parts.append(f"FW {st['firmware']}")
            if st.get("hdmi_timing"):
                t = st["hdmi_timing"]
                parts.append(f"HDMI {t['width']}x{t['height']}@{t['refresh_hz']:.0f}Hz")
            elif include_at and not st.get("at_error"):
                parts.append("HDMI standby")
            if st.get("at_error"):
                at_err = str(st["at_error"])
                if "LIBUSB_ERROR_ACCESS" in at_err or "Permission" in at_err:
                    parts.append(
                        "USB/HID permission denied — run 'sudo ./install.sh' to "
                        "install udev rules (07ca:2553 / 07ca:d553)"
                    )
                else:
                    parts.append(f"AT: {at_err}")
            self.lbl_gc553_status.setText(" · ".join(parts))

            for combo, key in (
                (getattr(self, "combo_gc553_preset", None), st.get("hdmi_preset")),
                (getattr(self, "combo_gc553_edid", None), st.get("edid_mode")),
            ):
                if combo is not None and key:
                    idx = combo.findData(key)
                    if idx >= 0:
                        combo.blockSignals(True)
                        combo.setCurrentIndex(idx)
                        combo.blockSignals(False)

            for chk, key in (
                (getattr(self, "chk_gc553_hdcp", None), "hdcp_detection"),
                (getattr(self, "chk_gc553_hdr_sdr", None), "hdr_to_sdr"),
            ):
                if chk is not None and key in st:
                    chk.blockSignals(True)
                    chk.setChecked(bool(st[key]))
                    chk.blockSignals(False)

        def _run_gc553_op(self, fn, *args):
            if not (self.uvc_mode and _gc553g2_control is not None):
                return
            if self.streamer.is_running:
                self.lbl_gc553_status.setText("Stop the stream before changing USB/AT settings.")
                return
            try:
                res = fn(*args, port=self._gc553_usb_port())
                self._refresh_gc553_status(include_at=False)
                return res
            except PermissionError:
                self.lbl_gc553_status.setText(
                    "USB/HID permission denied. Run 'sudo ./install.sh' to install udev rules for 07ca:2553 / 07ca:d553."
                )
            except Exception as exc:
                msg = str(exc)
                if "LIBUSB_ERROR_ACCESS" in msg:
                    self.lbl_gc553_status.setText(
                        "USB permission denied (LIBUSB_ERROR_ACCESS). Run 'sudo ./install.sh' for 07ca:2553 udev rules."
                    )
                else:
                    self.lbl_gc553_status.setText(f"GC553G2 error: {msg}")
            return None

        def _apply_gc553_descriptor_result(self, index: int, res: Optional[Dict]):
            if not res:
                return
            if res.get("video_node"):
                self.streamer.vdev = res["video_node"]
            if res.get("reconnect_required"):
                label = "120/144 Hz" if index == 1 else "60 Hz default"
                self.lbl_gc553_status.setText(
                    f"{label} descriptor applied — reconnect USB if formats did not refresh."
                )

        def _on_gc553_enable_high_fps(self):
            res = self._run_gc553_op(_gc553g2_control.set_descriptor_index, 1)
            self._apply_gc553_descriptor_result(1, res)

        def _on_gc553_restore_60hz(self):
            res = self._run_gc553_op(_gc553g2_control.set_descriptor_index, 0)
            self._apply_gc553_descriptor_result(0, res)

        def _on_gc553_hdmi_preset(self):
            preset = self.combo_gc553_preset.currentData()
            if preset:
                self._run_gc553_op(_gc553g2_control.set_hdmi_preset, preset)

        def _on_gc553_edid_mode(self):
            mode = self.combo_gc553_edid.currentData()
            if mode:
                self._run_gc553_op(_gc553g2_control.set_edid_mode, mode)

        def _on_gc553_hdcp(self, checked: bool):
            self._run_gc553_op(_gc553g2_control.set_hdcp_detection, bool(checked))

        def _on_gc553_hdr_to_sdr(self, checked: bool):
            self._run_gc553_op(_gc553g2_control.set_hdr_to_sdr, bool(checked))

        def _on_gc553_rgb(self):
            mode = self.combo_gc553_rgb.currentData()
            if mode:
                self._run_gc553_op(_gc553g2_control.set_rgb_mode, mode)

        def toggle_mute(self):
            self._set_mute_visual(not self.muted)
            self._mute_pending_until = time.monotonic() + 0.6
            if self._ipc is not None and self._ipc.connected:
                self._ipc.set_mute(self.muted)
                return
            if self.duo_mode and self.streamer.is_running:
                self.streamer.ctl("mute", "1" if self.muted else "0")
            set_audio_mute(self.muted)
            if self.vfio_mode:
                # Re-arm the retry window if the sink-input is not there yet.
                self._vfio_volume_applies = 0

        def _set_mute_visual(self, muted: bool):
            self.muted = bool(muted)
            if not hasattr(self, "btn_mute"):
                return
            icon_name = "mute" if self.muted else "volume"
            self.btn_mute.set_icon(
                icon_name,
                color=ui_theme.ON_ACCENT if self.muted else ui_theme.TEXT_MUTED)
            self.btn_mute.setToolTip(
                "Unmute HDMI audio" if self.muted else "Mute HDMI audio")
            self.btn_mute.setStyleSheet(
                "QPushButton#iconButton {"
                f" background-color: {ui_theme.DANGER_BG};"
                f" border: 1px solid {ui_theme.DANGER_BORDER}; }}"
                if self.muted else "")

        def on_volume_changed(self, val: int):
            self.lbl_vol.setText(f"{val}%")
            save_settings({"volume": val / 100.0})
            if self.muted:
                return
            if self._ipc is not None and self._ipc.connected:
                self._ipc.set_volume(val / 100.0)
                return
            if self.duo_mode and self.streamer.is_running:
                self.streamer.ctl("volume", str(val))
            set_audio_volume(val)
            if self.vfio_mode:
                self._vfio_volume_applies = 0

        def force_resync(self, *args):
            """Kernel: HPD re-sync. VFIO: restart the userspace engine."""
            if self.vfio_mode:
                was_running = self.streamer.is_running
                if was_running:
                    self.streamer.stop()
                    self._set_launch_running(False)
                restart = (self.start_studio
                           if self._last_launch_mode == "studio"
                           else self.start_gaming_screen)
                QTimer.singleShot(500 if was_running else 0, restart)
                QTimer.singleShot(3000, self._restore_led_after_stream)
                return

            if hasattr(self, "_resyncing") and self._resyncing:
                return
            self._resyncing = True
            if hasattr(self, "btn_resync"):
                self.btn_resync.setEnabled(False)
                self.btn_resync.set_icon("rotate")
                self.btn_resync.setToolTip(
                    "Synchronizing HDMI handshake…")
            try:
                was_running = self.streamer.is_running
                if was_running:
                    self.streamer.stop()
                    self._set_launch_running(False)
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
                    hasattr(self, "btn_resync") and self.btn_resync.set_icon("rotate"),
                    hasattr(self, "btn_resync") and self.btn_resync.setToolTip(
                        "Trigger hardware HDMI handshake (HPD pulse) - also "
                        "key 'H' in window")
                ))

        def _wait_and_restart_after_resync(self, attempts=12):
            if self.streamer.is_running:
                return
            sig = self.streamer.get_signal_info()
            if sig.get("locked") or sig.get("cached") or attempts <= 1:
                self.start_gaming_screen()
            else:
                QTimer.singleShot(250, lambda: self._wait_and_restart_after_resync(attempts - 1))

        def _show_live(self, info):
            res = info.get("resolution", "No Signal")
            fps = info.get("framerate", "")
            res_clean = re.sub(r"(\d+)x(\d+)([pi]?)", r"\1 × \2\3", res)

            if self.vfio_mode:
                pill_text = (
                    "● Live · Studio"
                    if self._launch_mode == "studio"
                    else "● Live · Gaming"
                )
                self.lbl_pill.setText(pill_text)
                self.lbl_pill.setStyleSheet(
                    "background-color: #122B26; color: #5ED4B2; border: 1px solid #24584B;")
                self.lbl_hero_signal.setText(f"{res_clean} @ {fps}".strip())
                chroma_val = info.get("chroma") or "YUV 4:2:2"
                audio = self.streamer.audio_state()
                if audio.get("state") == "on":
                    audio_str = f"Audio: {audio.get('detail')}"
                    audio_kv = str(audio.get("detail") or "HDMI LPCM")
                elif audio.get("state") in ("absent", "unavailable",
                                            "unsupported", "error"):
                    audio_str = "No HDMI audio"
                    audio_kv = "No HDMI Audio"
                else:
                    audio_str = "HDMI LPCM"
                    audio_kv = "48 kHz LPCM"
                pclk_khz = int((getattr(self.streamer, "_vfio_metrics", {}) or {}).get("pclk_khz") or 0)
                pclk_str = f" • {pclk_khz / 1000.0:.1f} MHz PCLK" if pclk_khz > 0 else ""
                hdr_state = self._kv_hdr_text("HDR Tonemap", "HDR Bypass")
                self.lbl_hero_meta.setText(
                    f"{chroma_val}{pclk_str} • {hdr_state} • {audio_str}")
                if hasattr(self, "lbl_kv_color"):
                    self.lbl_kv_color.setText(f"{chroma_val} · {hdr_state}")
                if hasattr(self, "lbl_kv_pclk"):
                    pclk_kv = f"{pclk_khz / 1000.0:.1f} MHz" if pclk_khz > 0 else "Locked"
                    self.lbl_kv_pclk.setText(self._kv_link_text(pclk_kv))
                if hasattr(self, "lbl_kv_audio"):
                    self.lbl_kv_audio.setText(audio_kv)
                if hasattr(self, "lbl_audio_fmt"):
                    self._set_audio_status(self.streamer.audio_format_label())
                self._update_telemetry_cards()
                self._refresh_stage_overlay()
                return

            hdr_str = f" • {info['hdr']}" if info.get("hdr") else ""
            chroma_str = f" • {info['chroma']}" if info.get("chroma") else ""
            self.lbl_pill.setText(f"● {res} @ {fps}{chroma_str}{hdr_str}")
            self.lbl_pill.setStyleSheet(
                "background-color: #122B26; color: #5ED4B2; border: 1px solid #24584B;")
            self.lbl_hero_signal.setText(f"{res_clean} @ {fps}".strip())
            hdr_val = info.get('hdr', 'SDR')
            chroma_val = info.get('chroma', 'YUV 4:2:2')
            self.lbl_hero_meta.setText(f"Chroma: {chroma_val} • Color: {hdr_val} • Stereo LPCM (PipeWire)")
            if hasattr(self, "lbl_kv_color"):
                self.lbl_kv_color.setText(f"{chroma_val} · {hdr_val}")
            if hasattr(self, "lbl_kv_pclk"):
                self.lbl_kv_pclk.setText("V4L2 Kernel Timing")
            if hasattr(self, "lbl_kv_audio"):
                self.lbl_kv_audio.setText("Stereo LPCM (PipeWire)")
            self._update_telemetry_cards()
            self._refresh_stage_overlay()

        def _set_vfio_standby(self, info):
            self.lbl_pill.setText("○ Standby")
            self.lbl_pill.setStyleSheet(
                "background-color: #1C192C; color: #ACA5C0; border: 1px solid #2A2540;")
            if info.get("cached"):
                res = info.get("resolution", "No Signal")
                fps = info.get("framerate", "")
                res_clean = re.sub(r"(\d+)x(\d+)([pi]?)", r"\1 × \2\3", res)
                self.lbl_hero_signal.setText(f"{res_clean} @ {fps}".strip())
                self.lbl_hero_meta.setText(
                    self._kv_link_text("Standby").replace(" · ", " • ")
                    + " • Waiting for HDMI input")
            else:
                self.lbl_hero_signal.setText("Ready for Gaming")
                self.lbl_hero_meta.setText(
                    self._kv_link_text("Standby").replace(" · ", " • ")
                    + " • Waiting for HDMI input")
            if hasattr(self, "lbl_kv_color"):
                hdr_state = self._kv_hdr_text("HDR Tonemap", "SDR / Bypass")
                self.lbl_kv_color.setText(f"YUV 4:2:2 · {hdr_state}")
            if hasattr(self, "lbl_kv_pclk"):
                self.lbl_kv_pclk.setText(self._kv_link_text("— MHz"))
            if hasattr(self, "lbl_kv_audio"):
                self.lbl_kv_audio.setText("Standby")
            if hasattr(self, "lbl_audio_fmt"):
                self._set_audio_status("No Audio")
            self._update_telemetry_cards()
            self._refresh_stage_overlay()

        def _update_status_vfio(self):
            if self._handoff_update():
                return
            if self._remote and not self.streamer.is_running:
                # The remote streamer owns these values; its 1 Hz snapshot
                # already drives the hero labels through _on_remote_status.
                return
            if hasattr(self, "lbl_audio_fmt"):
                self._set_audio_status(
                    self.streamer.audio_format_label()
                    if self.streamer.is_running else "No Audio")
            if self.streamer.is_running:
                if not self.streamer.poll():
                    self._set_launch_running(False)
                    QTimer.singleShot(900, self._restore_led_after_stream)
                    if not self.streamer._vfio_saw_output:
                        self._set_vfio_notice(
                            "Userspace Engine Stopped",
                            "See ~/.config/everlive/streamer.log for the hardware error")
                    else:
                        self._set_vfio_standby(self.streamer.get_signal_info())
                    return

                info = self.streamer.get_signal_info()
                self._live = info
                if info.get("locked"):
                    # Re-apply a few times: the SDL sink-input appears only after
                    # the first audio period, and the status poll is cheap.
                    if self._vfio_volume_applies < 4:
                        if self.vfio_mode:
                            set_audio_stream_unity()
                        else:
                            set_audio_volume(self.slider_vol.value())
                        set_audio_mute(self.muted)
                        self._vfio_volume_applies += 1
                    self._show_live(info)
                elif info.get("starting"):
                    self.lbl_hero_signal.setText("Starting Userspace Engine…")
                    self.lbl_hero_meta.setText(
                        "DMA ring initialising • SDL2 video/audio")
                    self._update_telemetry_cards()
                else:
                    self.lbl_hero_signal.setText("No HDMI Signal")
                    self.lbl_hero_meta.setText(
                        "Engine running • waiting for stable HDMI input")
                    self._update_telemetry_cards()
                return

            info = self.streamer.get_signal_info()
            self._live = info
            if time.time() < self._vfio_notice_until:
                self._update_telemetry_cards()
                return
            self._set_vfio_standby(info)

        def update_status(self):
            self._poll_recording_state()
            if self.vfio_mode:
                self._update_status_vfio()
                return

            # While the gaming screen is open, only read the cached driver format and HDR control.
            # query-dv-timings hits the HDMI receiver and stalls the stream.
            if self.streamer.is_running:
                if not self.streamer.poll():
                    # Window was closed by user
                    self._set_launch_running(False)
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
                self._update_telemetry_cards()
                return

            info = self.streamer.get_signal_info()
            self._live = info
            if hasattr(self, "lbl_audio_fmt"):
                self._set_audio_status(
                    info.get("audio", "No Audio")
                    if info.get("locked") else "No Audio")
            if info["locked"]:
                self._show_live(info)
            else:
                self.lbl_pill.setText("○ Standby")
                self.lbl_pill.setStyleSheet(
                    "background-color: #1C192C; color: #ACA5C0; border: 1px solid #2A2540;")
                self.lbl_hero_signal.setText("No HDMI Signal")
                self.lbl_hero_meta.setText("Waiting for input signal (e.g. power on console or PC)...")
                self._update_telemetry_cards()

        def _init_tray(self):
            if not QSystemTrayIcon.isSystemTrayAvailable():
                self.tray_icon = None
                return

            icon_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "everlive.png")
            icon = QIcon(icon_path) if os.path.isfile(icon_path) else self.windowIcon()

            self.tray_icon = QSystemTrayIcon(icon, self)
            self.tray_icon.setToolTip("EverLive - Console Hub")

            self.tray_menu = QMenu()
            self.tray_menu.setStyleSheet("""
                QMenu {
                    background-color: #12151f;
                    color: #e2e8f0;
                    border: 1px solid #1f2537;
                    padding: 4px;
                    font-size: 13px;
                }
                QMenu::item {
                    padding: 6px 20px;
                    border-radius: 12px;
                }
                QMenu::item:selected {
                    background-color: #2563eb;
                    color: #ffffff;
                }
                QMenu::separator {
                    height: 1px;
                    background-color: #1f2537;
                    margin: 4px 8px;
                }
            """)

            action_show = QAction("Open Console Hub", self)
            action_show.triggered.connect(self._restore_from_tray)
            self.tray_menu.addAction(action_show)

            self.tray_menu.addSeparator()

            self.tray_act_stream = QAction("Start Gaming Screen", self)
            self.tray_act_stream.triggered.connect(self.toggle_stream)
            self.tray_menu.addAction(self.tray_act_stream)

            self.tray_act_studio = QAction("Start Studio / Headless (OBS)", self)
            self.tray_act_studio.triggered.connect(self.toggle_studio)
            self.tray_menu.addAction(self.tray_act_studio)

            self.tray_menu.addSeparator()

            action_quit = QAction("Quit EverLive", self)
            action_quit.triggered.connect(self._force_quit)
            self.tray_menu.addAction(action_quit)

            self.tray_icon.setContextMenu(self.tray_menu)
            self.tray_icon.activated.connect(self._on_tray_activated)
            self.tray_icon.show()

        def _update_tray_menu(self):
            if not getattr(self, "tray_act_stream", None) or not getattr(self, "tray_act_studio", None):
                return
            if self._launch_mode == "studio":
                self.tray_act_stream.setText("Switch to Gaming Screen")
                self.tray_act_studio.setText("Stop Studio")
            elif self._launch_mode == "gaming":
                self.tray_act_stream.setText("Stop Gaming Screen")
                self.tray_act_studio.setText("Switch to Studio (Headless)")
            else:
                self.tray_act_stream.setText("Start Gaming Screen")
                self.tray_act_studio.setText("Start Studio / Headless (OBS)")

        def _restore_from_tray(self):
            self.showNormal()
            self.raise_()
            self.activateWindow()

        def _on_tray_activated(self, reason):
            if reason in (QSystemTrayIcon.Trigger, QSystemTrayIcon.DoubleClick):
                if self.isVisible() and not self.isMinimized():
                    self.hide()
                else:
                    self._restore_from_tray()

        def _release_remote_preview(self):
            """Return a docked external engine to headless before teardown."""
            if not (self._remote and self._remote_docked):
                return
            if self._ipc is None or not self._ipc.connected:
                return
            done = {"ok": False}
            self._ipc.set_mode_async("headless", None,
                                     lambda _msg: done.update(ok=True))
            # Let the socket notifier deliver the reply while the Qt parent
            # still exists.  The engine also returns to headless by itself if
            # the embedding window disappears.
            deadline = time.time() + 1.0
            while time.time() < deadline and not done["ok"]:
                QApplication.processEvents()
                if self._ipc is None or not self._ipc.connected:
                    break
                time.sleep(0.02)
            self._remote_docked = False

        def _restore_headless_service(self):
            """Restart the DUO unit if this Hub session stopped it."""
            if not self._service_was_active:
                return
            self._service_was_active = False
            print("[*] Restarting opengc570d-userd.service for headless capture.")
            _start_duo_service()

        def _restore_service_after_failed_start(self):
            """Undo a service stop when the local engine could not start."""
            if not getattr(self.streamer, "_stopped_duo_service", False):
                return
            self.streamer._stopped_duo_service = False
            self._service_was_active = True

            def _try():
                if _vfio_streamer_running():
                    QTimer.singleShot(500, _try)
                    return
                self._service_was_active = False
                _start_duo_service()

            QTimer.singleShot(500, _try)

        def _force_quit(self):
            self._switch_pending = None
            if getattr(self, "recorder", None) and self.recorder.is_recording:
                self.recorder.stop()
            self._release_remote_preview()
            self._multiview_stop()
            self._webcam_stop()
            self._ipc_shutdown = True
            if self._ipc is not None:
                self._ipc.close()
            was_running = self.streamer.is_running
            self.streamer.stop()
            self._restore_headless_service()
            if self.vfio_mode and was_running:
                self._restore_led_after_stream()
            if hasattr(self, "tray_icon") and self.tray_icon:
                self.tray_icon.hide()
            QApplication.quit()

        def closeEvent(self, event):
            # Do not let a pending handover timer relaunch the streamer.
            self._switch_pending = None
            was_running = self.streamer.is_running

            # Close to Tray: clicking [X] hides the window into the system tray
            # (not when closing to restart on another capture card).
            if not getattr(self, "_card_restart", None) and getattr(self, "chk_tray", None) and self.chk_tray.isChecked() and getattr(self, "tray_icon", None):
                event.ignore()
                self.hide()
                if not getattr(self, "_tray_notified", False):
                    self._tray_notified = True
                    self.tray_icon.showMessage(
                        "EverLive hidden to tray",
                        "The Console Hub is minimized to the system tray. Click the tray icon to reopen.",
                        QSystemTrayIcon.Information,
                        3000)
                return

            self._ipc_shutdown = True
            if getattr(self, "recorder", None) and self.recorder.is_recording:
                self.recorder.stop()
            self._multiview_stop()
            self._webcam_stop()
            self._release_remote_preview()
            if self._ipc is not None:
                self._ipc.close()
            self.streamer.stop()
            self._restore_headless_service()
            if self.vfio_mode and was_running:
                self._restore_led_after_stream()
            if hasattr(self, "tray_icon") and self.tray_icon:
                self.tray_icon.hide()
            event.accept()


def _destroy_qt_objects(app, window):
    """Tear the Qt object tree down while Python still owns it.

    Leaving it to PySide's interpreter-exit cleanup destroys some QObjects
    twice (segfault or "corrupted double-linked list" abort in
    PySide::destroyQCoreApplication on every quit).
    """
    try:
        tray = getattr(window, "tray_icon", None)
        if tray is not None:
            tray.hide()
        window.hide()
        window.deleteLater()
        app.sendPostedEvents(None, QEvent.Type.DeferredDelete)
        app.processEvents()
    except Exception:
        pass


def _restart_on_card(pci: str):
    """Re-launch the Hub on another capture card (card selector).

    In a git checkout this goes through play.sh, which builds the card's
    engine and binds it to vfio-pci first; an installed Hub re-executes
    itself.
    """
    env = dict(os.environ)
    env["EVERLIVE_CARD_PCI"] = pci
    env.pop("EVERLIVE_CARD", None)
    play = os.path.normpath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "play.sh"))
    print(f"[*] Restarting the Console Hub on capture card {pci}...")
    sys.stdout.flush()
    try:
        if os.access(play, os.X_OK):
            os.execve(play, [play], env)
        args = [a for a in sys.argv[1:] if a not in ("--tray", "--minimized")]
        os.execve(sys.executable,
                  [sys.executable, os.path.abspath(__file__)] + args, env)
    except OSError as exc:
        print(f"[!] Restart failed ({exc}); start EverLive again to use {pci}.")


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
    parser.add_argument("--aspect", choices=["auto", "16:9", "4:3", "stretch"], default="auto", help="Aspect ratio override")
    parser.add_argument("--v4l2", "--loopback", dest="v4l2", nargs="?",
                        const=V4L2_DEFAULT_DEVICE, default=None,
                        help="Publish native YUYV frames to a v4l2loopback device "
                             f"(default: {V4L2_DEFAULT_DEVICE})")
    parser.add_argument("--headless", "--studio", dest="headless", action="store_true",
                        help="Capture and publish/play without an SDL window")
    parser.add_argument("--sdl", action="store_true",
                        help="Open the interactive SDL2 gaming window")
    parser.add_argument("--port", type=int, choices=[1, 2], default=None,
                        help="Select initial HDMI input on Live Gamer DUO (1 or 2)")
    parser.add_argument("--pip", action="store_true",
                        help="Enable picture-in-picture on Live Gamer DUO")
    parser.add_argument("--v4l2-2", "--loopback-2", dest="v4l2_2", default=None,
                        help="Secondary v4l2loopback device for Live Gamer DUO HDMI 2")
    parser.add_argument("--copy-edid", action="store_true",
                        help="Copy downstream EDID on Live Gamer DUO")
    parser.add_argument("--scale", choices=["fit", "integer"], default=None,
                        help="Scaling mode override (fit or integer)")
    parser.add_argument("--tray", "--minimized", dest="tray", action="store_true",
                        help="Launch Console Hub directly hidden into the system tray")
    parser.add_argument("--wave", action="store_true",
                        help="Set GC573 LED to multi-zone rainbow wave")
    parser.add_argument("--rainbow", action="store_true",
                        help="Set GC573 LED to whole-card spectrum cycle")
    parser.add_argument("--no-loopback", "--skip-loopback", dest="no_loopback", action="store_true",
                        help="Run strictly module-free without v4l2loopback")
    args = parser.parse_args()

    if args.no_loopback:
        save_settings({"v4l2_loopback": False})
    if args.port in (1, 2):
        save_settings({"duo_input": args.port - 1})
    if args.copy_edid:
        save_settings({"duo_edid_bridge": False})
    if args.scale in SCALE_MODES:
        save_settings({"scale_mode": args.scale})

    if args.wave:
        args.led = "wave"
    elif args.rainbow:
        args.led = "rainbow"

    # Headless and virtual-camera launches are streamer-only unless tray is requested.
    if (args.headless or args.v4l2) and not args.tray:
        args.cli = True

    if args.led is not None:
        key = args.led.lower().strip()
        val = LED_NAME_MAP.get(key)
        if val is None:
            try:
                val = int(key)
            except ValueError:
                pass
        if val is not None and 0 <= val <= 9:
            hw = detect_gc573_driver()
            model = hw.get("model", MODEL_GC573)
            model_name = "GC570D" if model == MODEL_GC570D else "GC573"
            if model == MODEL_HWSUHD:
                print("[*] The selected HWS UHD card has no LEDs; nothing to set.")
            elif hw.get("mode") == "vfio":
                save_settings({"led_mode": val})
                is_launching = args.cli or args.tray or any(
                    arg in sys.argv for arg in [
                        "-f", "--fullscreen", "-p", "--preset",
                        "--headless", "--v4l2", "--loopback",
                        "--streamer-mode", "-s"
                    ]
                )
                if model == MODEL_GC570D:
                    if _vfio_streamer_running():
                        if set_gc570d_led(val):
                            print(f"[+] {model_name} LED effect set to mode {val}.")
                        else:
                            print(f"[*] {model_name} streamer is running; LED mode {val} saved for the next session.")
                    else:
                        print(f"[*] {model_name} LED mode {val} saved; it is applied by the engine.")
                elif _vfio_streamer_running():
                    print(f"[*] Userspace streamer is running; LED mode {val} saved for the next session.")
                elif not is_launching:
                    if set_vfio_led(val):
                        print(f"[+] GC573 LED lighting set to mode {val} via everlive_userd.")
                    else:
                        print(f"[!] Failed to set GC573 LED to mode {val} via everlive_userd.")
                else:
                    print(f"[+] GC573 LED lighting set to mode {val} for stream.")
            else:
                if write_led_mode(val):
                    print(f"[+] {model_name} LED lighting set to mode {val}.")
                else:
                    print(f"[!] Failed to set {model_name} LED to mode {val}.")
        else:
            print(f"[!] Invalid LED mode '{args.led}'. Valid: {list(LED_NAME_MAP.keys())}")
        if not args.cli and not args.tray and not any(arg in sys.argv for arg in ["-f", "--fullscreen", "-p", "--preset", "--headless", "--v4l2", "--loopback"]):
            sys.exit(0)

    streamer = GameStreamer(vdev=args.device, adev=args.audio, force_mpv=args.mpv)
    streamer.aspect = args.aspect
    if args.fullscreen or args.streamer_mode:
        streamer._cli_autostart = True

    if streamer.vfio_mode:
        print(f"[*] {streamer.model_label}: Userspace VFIO engine "
              f"(PCI {streamer.hw_info.get('pci') or 'auto'}), "
              "no V4L2/ALSA device required.")
        if args.device or args.audio or args.mpv:
            print("[*] Note: --device/--audio/--mpv apply only to the kernel V4L2 path.")
    else:
        print(f"[*] {streamer.model_label} Hardware: Video={streamer.vdev}, "
              f"Audio={streamer.adev} "
              f"(driver={streamer.hw_info.get('driver') or 'unknown'})")

    # Graceful shutdown on Ctrl+C
    hub = {"window": None}

    def sig_handler(sig, frame):
        win = hub["window"]
        if win is not None:
            # Multiview engines run in their own sessions; stop them too.
            win._multiview_stop()
            win._webcam_stop()
            # A card switch replaces the window's engine wrapper.
            win.streamer.stop()
        streamer.stop()
        sys.exit(0)
    signal.signal(signal.SIGINT, sig_handler)
    signal.signal(signal.SIGTERM, sig_handler)

    if args.cli or not HAS_QT:
        print("[*] Launching Gaming Screen in low-latency mode...")
        if streamer.vfio_mode:
            engine = os.path.basename(streamer.streamer_bin)
            print(f"[*] Userspace VFIO engine: video and HDMI audio run inside {engine}.")
        else:
            info = streamer.get_signal_info()
            chroma_disp = f" [{info['chroma']}]" if info.get('chroma') else ""
            print(f"[*] Signal: {info['resolution']} {info['framerate']}{chroma_disp}".strip())
            if not info["locked"]:
                print(f"[!] Warning: No active HDMI signal detected on {streamer.model_label}.")
                print("    Please check if the video source is powered on and sending video.")
        cli_settings = load_settings()
        if not streamer.start(preset_name=args.preset, fullscreen=args.fullscreen, volume=args.volume, vsync=args.vsync, streamer_mode=args.streamer_mode, v4l2_device=args.v4l2, headless=args.headless, aspect=args.aspect, framegen=cli_settings.get("framegen_mode", "off"), fg_multiplier=cli_settings.get("fg_multiplier", 2), fg_dedup=cli_settings.get("fg_dedup", False), fg_flow_scale=cli_settings.get("fg_flow_scale", 1.0), fg_perf_mode=cli_settings.get("fg_perf_mode", False), fg_low_latency=cli_settings.get("fg_low_latency", True)):
            sys.exit(1)
        if streamer.vfio_mode:
            # The SDL2 sink-input only exists once audio starts flowing.
            # Ensure host PipeWire stream is at 100% unity gain so --volume
            # applies cleanly without double-scaling.
            time.sleep(1.5)
            set_audio_stream_unity()
        print("[+] Window opened. Press 'q' in window or Ctrl+C to exit.")
        while streamer.poll():
            time.sleep(0.5)
        streamer.stop()
        if streamer.vfio_mode and streamer.model == MODEL_GC573:
            saved_led = load_settings().get("led_mode")
            mode = saved_led if isinstance(saved_led, int) and 0 <= saved_led <= 9 else 4
            if not (_proc_running("everlive_userd") and mode == 4):
                set_vfio_led(mode)
    else:
        if os.environ.get("DISPLAY"):
            os.environ.setdefault("QT_QPA_PLATFORM", "xcb")
        QGuiApplication.setDesktopFileName("everlive")
        app = QApplication(sys.argv)
        app.setApplicationName("EverLive")
        icon_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "everlive.png")
        if os.path.isfile(icon_path):
            app.setWindowIcon(QIcon(icon_path))
        window = MainWindow(streamer, vsync=args.vsync, streamer_mode=args.streamer_mode,
                            headless_start=args.headless, v4l2_device=args.v4l2)
        hub["window"] = window
        if not args.tray:
            window.show()
        code = app.exec()
        window._multiview_stop()
        window._webcam_stop()
        hub["window"] = None
        restart_pci = getattr(window, "_card_restart", None)
        _destroy_qt_objects(app, window)
        del window
        if restart_pci:
            _restart_on_card(restart_pci)
        sys.exit(code)


if __name__ == "__main__":
    main()
