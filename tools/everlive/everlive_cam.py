# SPDX-License-Identifier: GPL-2.0-only
"""EverLive webcam bridge: process control and picture-in-picture helpers.

``everlive_camd`` opens the webcam once and publishes it into the frame store
``/dev/shm/everlive_cam_v4l2_shm``; ``libeverlive-cam-v4l2.so`` presents that
store as ``/dev/video65``.  The Console Hub's PiP preview (an ffmpeg reader
piping RGB frames to the Hub),
the recorder's overlay input and OBS can then all read the camera at once.

Qt-free so it can be unit tested and reused by the CLI tools.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

import everlive_recorder

CAM_NODE = "/dev/video65"
CAM_SHM = "/dev/shm/everlive_cam_v4l2_shm"
CAMD_BINARY = "everlive_camd"
CAM_PRELOAD = "libeverlive-cam-v4l2.so"

TOOLS_DIR = Path(__file__).resolve().parent.parent

# (label, setting key) for the Hub's combos.
PIP_CORNER_OPTIONS: Tuple[Tuple[str, str], ...] = (
    ("Bottom right", "br"),
    ("Bottom left", "bl"),
    ("Top right", "tr"),
    ("Top left", "tl"),
)
PIP_SIZE_OPTIONS: Tuple[Tuple[str, int], ...] = (
    ("Small (15%)", 15),
    ("Medium (20%)", 20),
    ("Large (25%)", 25),
    ("Extra large (33%)", 33),
)
# (label, "WxH@fps")
PIP_MODE_OPTIONS: Tuple[Tuple[str, str], ...] = (
    ("720p · 30 FPS", "1280x720@30"),
    ("720p · 60 FPS", "1280x720@60"),
    ("1080p · 30 FPS", "1920x1080@30"),
    ("1080p · 60 FPS", "1920x1080@60"),
)

DEFAULT_SETTINGS: Dict[str, object] = {
    "pip_enabled": False,
    "pip_device": "",
    "pip_corner": "br",
    "pip_size": 20,
    "pip_mode": "1280x720@60",
}


def parse_mode(mode: str) -> Tuple[int, int, int]:
    """``"1280x720@30"`` -> ``(1280, 720, 30)``; bad input gives 720p60."""
    try:
        size, fps = str(mode).split("@", 1)
        w, h = size.lower().split("x", 1)
        w_i, h_i, fps_i = int(w), int(h), int(fps)
        if 160 <= w_i <= 3840 and 120 <= h_i <= 2160 and 1 <= fps_i <= 120:
            return w_i, h_i, fps_i
    except (TypeError, ValueError):
        pass
    return 1280, 720, 60


def normalize_settings(settings: Dict[str, object]) -> Dict[str, object]:
    """PiP settings with defaults filled in and bad values replaced."""
    out = dict(DEFAULT_SETTINGS)
    out.update({k: settings[k] for k in DEFAULT_SETTINGS if k in settings})
    out["pip_enabled"] = bool(out["pip_enabled"])
    out["pip_device"] = str(out["pip_device"] or "")
    if out["pip_corner"] not in everlive_recorder.PIP_CORNERS:
        out["pip_corner"] = "br"
    try:
        out["pip_size"] = max(5, min(50, int(out["pip_size"])))
    except (TypeError, ValueError):
        out["pip_size"] = 20
    w, h, fps = parse_mode(str(out["pip_mode"]))
    out["pip_mode"] = f"{w}x{h}@{fps}"
    return out


def find_camd(tools_dir: Path = TOOLS_DIR) -> Optional[str]:
    local = tools_dir / "everlive-cam" / CAMD_BINARY
    if local.is_file() and os.access(local, os.X_OK):
        return str(local)
    return shutil.which(CAMD_BINARY)


def find_cam_preload(tools_dir: Path = TOOLS_DIR,
                     extra_dirs: Sequence[str] = ()) -> Optional[str]:
    candidates = [
        tools_dir / "everlive-cam" / CAM_PRELOAD,
        Path("/usr/local/lib") / CAM_PRELOAD,
        Path("/usr/lib") / CAM_PRELOAD,
        Path("/usr/lib64") / CAM_PRELOAD,
        Path.home() / ".local/lib" / CAM_PRELOAD,
    ]
    candidates.extend(Path(d) / CAM_PRELOAD for d in extra_dirs)
    for path in candidates:
        if path.is_file():
            return str(path)
    return None


def parse_device_list(text: str) -> List[Tuple[str, str]]:
    """``everlive_camd --list`` output -> ``[(path, name), ...]``."""
    devices = []
    for line in text.splitlines():
        path, _sep, name = line.partition("\t")
        path = path.strip()
        if path.startswith("/dev/video"):
            devices.append((path, name.strip() or path))
    return devices


def list_webcams(camd: Optional[str] = None) -> List[Tuple[str, str]]:
    camd = camd or find_camd()
    if not camd:
        return []
    try:
        out = subprocess.run([camd, "--list"], capture_output=True, text=True,
                             timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    return parse_device_list(out)


def build_camd_command(camd: str, device: str = "", mode: str = "1280x720@30"
                       ) -> List[str]:
    w, h, fps = parse_mode(mode)
    cmd = [camd, "--size", f"{w}x{h}", "--fps", str(fps)]
    if device:
        cmd.extend(["--device", device])
    return cmd


def pip_overlay(settings: Dict[str, object],
                preload_library: Optional[str]) -> everlive_recorder.PipOverlay:
    cfg = normalize_settings(settings)
    _w, _h, fps = parse_mode(str(cfg["pip_mode"]))
    return everlive_recorder.PipOverlay(
        node=CAM_NODE,
        preload_library=preload_library,
        corner=str(cfg["pip_corner"]),
        size_pct=int(cfg["pip_size"]),
        fps=fps,
    )


def parse_v4l2_controls(text: str) -> Dict[str, dict]:
    """Parse ``v4l2-ctl -L`` output into a dictionary of control metadata."""
    controls: Dict[str, dict] = {}
    current_ctrl: Optional[dict] = None
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if current_ctrl and current_ctrl.get("type") == "menu" and re.match(r"^\d+:\s+", line):
            m_menu = re.match(r"^(\d+):\s+(.*)$", line)
            if m_menu:
                val, desc = int(m_menu.group(1)), m_menu.group(2).strip()
                current_ctrl.setdefault("menu", {})[val] = desc
            continue

        m = re.match(r"^([a-z0-9_]+)\s+0x[0-9a-f]+\s+\(([a-z]+)\)\s*:\s*(.*)$", line)
        if m:
            name, ctype, rest = m.groups()
            props: Dict[str, Any] = {"type": ctype, "name": name, "menu": {}}
            if "inactive" in rest:
                props["inactive"] = True
            for part in rest.split():
                part = part.rstrip(",")
                if "=" in part:
                    k, v = part.split("=", 1)
                    v = v.rstrip(",")
                    try:
                        props[k] = int(v)
                    except ValueError:
                        props[k] = v
            controls[name] = props
            current_ctrl = props
        else:
            current_ctrl = None
    return controls


def query_camera_controls(device: str = "") -> Dict[str, dict]:
    """Query available V4L2 hardware controls, ranges and current values."""
    dev = device
    if not dev:
        cams = list_webcams()
        if not cams:
            return {}
        dev = cams[0][0]
    v4l2_ctl = shutil.which("v4l2-ctl")
    if not v4l2_ctl or not os.path.exists(dev):
        return {}
    try:
        res = subprocess.run(
            [v4l2_ctl, "-d", dev, "-L"],
            capture_output=True, text=True, timeout=2.0)
        if res.returncode == 0:
            return parse_v4l2_controls(res.stdout)
    except Exception:
        pass
    return {}


def set_camera_control(device: str, name: str, value: int) -> bool:
    """Set a single V4L2 control on ``device`` via v4l2-ctl."""
    dev = device
    if not dev:
        cams = list_webcams()
        if not cams:
            return False
        dev = cams[0][0]
    v4l2_ctl = shutil.which("v4l2-ctl")
    if not v4l2_ctl or not os.path.exists(dev):
        return False
    try:
        res = subprocess.run(
            [v4l2_ctl, "-d", dev, f"--set-ctrl={name}={int(value)}"],
            capture_output=True, text=True, timeout=2.0)
        return res.returncode == 0
    except Exception:
        return False


def reset_camera_controls(device: str = "") -> bool:
    """Reset all available controls on ``device`` to hardware defaults."""
    ctrls = query_camera_controls(device)
    if not ctrls:
        return False
    dev = device
    if not dev:
        cams = list_webcams()
        if not cams:
            return False
        dev = cams[0][0]
    v4l2_ctl = shutil.which("v4l2-ctl")
    if not v4l2_ctl:
        return False
    args = []
    for name, c in ctrls.items():
        if "default" in c:
            args.append(f"--set-ctrl={name}={c['default']}")
    if not args:
        return True
    try:
        res = subprocess.run(
            [v4l2_ctl, "-d", dev] + args,
            capture_output=True, text=True, timeout=3.0)
        return res.returncode == 0
    except Exception:
        return False


PREVIEW_SIZE = (640, 360)
# The shim hands out frames on its own timer, not when the bridge publishes;
# polling at 60 Hz keeps a new camera frame's wait under ~17 ms.
PREVIEW_FPS = 60


def build_preview_command(preload_library: str,
                          width: int = PREVIEW_SIZE[0],
                          height: int = PREVIEW_SIZE[1],
                          fps: int = PREVIEW_FPS, ffmpeg: str = "ffmpeg"
                          ) -> Tuple[List[str], Dict[str, str]]:
    """ffmpeg argv/env that writes the camera to stdout as raw RGB24 frames.

    The shim scales to ``width`` x ``height``; the Hub paints each frame
    (``width * height * 3`` bytes) itself, so the preview needs no embedded
    player and no GPU context.
    """
    cmd = [
        ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin",
        "-probesize", "32", "-analyzeduration", "0",
        "-fflags", "nobuffer", "-flags", "low_delay",
        "-f", "v4l2", "-input_format", "yuyv422",
        "-video_size", f"{int(width)}x{int(height)}",
        "-framerate", str(int(fps)),
        "-i", CAM_NODE,
        "-an", "-f", "rawvideo", "-pix_fmt", "rgb24", "-fps_mode", "passthrough",
        "pipe:1",
    ]
    return cmd, {"LD_PRELOAD": preload_library}


def _stop_process(proc: Optional[subprocess.Popen], timeout: float = 3.0) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=timeout)


@dataclass
class CameraBridge:
    """Runs ``everlive_camd`` and, optionally, the preview reader."""

    log_path: Optional[str] = None
    camd: Optional[str] = None
    preload_library: Optional[str] = None
    proc: Optional[subprocess.Popen] = None
    preview: Optional[subprocess.Popen] = None
    device: str = ""
    mode: str = ""
    last_error: str = ""

    def __post_init__(self) -> None:
        self.camd = self.camd or find_camd()
        self.preload_library = self.preload_library or find_cam_preload()

    @property
    def available(self) -> bool:
        return bool(self.camd and self.preload_library)

    @property
    def running(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    @property
    def preview_running(self) -> bool:
        return self.preview is not None and self.preview.poll() is None

    def start(self, device: str = "", mode: str = "1280x720@30",
              wait: float = 3.0) -> bool:
        """Start (or restart for a new device/mode) the bridge.

        Returns once the frame store exists, the bridge exits, or ``wait``
        seconds pass.
        """
        if self.running and device == self.device and mode == self.mode:
            return True
        self.stop()
        if not self.available:
            self.last_error = ("Webcam bridge not built (make cam): "
                               f"{CAMD_BINARY} / {CAM_PRELOAD} missing.")
            return False
        cmd = build_camd_command(self.camd or CAMD_BINARY, device, mode)
        log = None
        try:
            if self.log_path:
                os.makedirs(os.path.dirname(self.log_path), exist_ok=True)
                log = open(self.log_path, "ab")
            self.proc = subprocess.Popen(
                cmd, stdout=log or subprocess.DEVNULL,
                stderr=subprocess.STDOUT, start_new_session=True)
        except OSError as exc:
            self.last_error = f"Cannot start {CAMD_BINARY}: {exc}"
            return False
        finally:
            if log is not None:
                log.close()
        self.device, self.mode = device, mode
        deadline = time.monotonic() + wait
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                self.last_error = (f"{CAMD_BINARY} exited (code "
                                   f"{self.proc.returncode}); see {self.log_path}")
                self.proc = None
                return False
            if os.path.exists(CAM_SHM):
                self.last_error = ""
                return True
            time.sleep(0.05)
        return self.running

    def start_preview(self, width: int = PREVIEW_SIZE[0],
                      height: int = PREVIEW_SIZE[1],
                      fps: int = PREVIEW_FPS) -> bool:
        """Start the preview reader; frames arrive on ``self.preview.stdout``."""
        self.stop_preview()
        ffmpeg = shutil.which("ffmpeg")
        if not (ffmpeg and self.preload_library and self.running):
            if not ffmpeg:
                self.last_error = "ffmpeg is needed for the webcam preview."
            return False
        cmd, env = build_preview_command(self.preload_library, width, height,
                                         fps, ffmpeg)
        try:
            self.preview = subprocess.Popen(
                cmd, env=dict(os.environ, **env), stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                bufsize=0, start_new_session=True)
        except OSError as exc:
            self.last_error = f"Cannot start the preview: {exc}"
            return False
        return True

    def stop_preview(self) -> None:
        _stop_process(self.preview)
        self.preview = None

    def stop(self) -> None:
        self.stop_preview()
        _stop_process(self.proc)
        self.proc = None

    def pids(self) -> List[int]:
        return [p.pid for p in (self.proc, self.preview)
                if p is not None and p.poll() is None]
