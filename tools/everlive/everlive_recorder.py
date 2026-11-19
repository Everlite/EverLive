#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Direct video/audio recording helper for EverLive.

Drives ``ffmpeg`` completely independently of Qt so capture pipelines can be
configured, probed, and unit-tested without a display server.

Sources supported:
- ``shm``: Opens the lock-free ``everlive_framestore`` via ``LD_PRELOAD``
  (``libgc573-v4l2.so`` / ``libgc570d-v4l2.so``) or a native V4L2 node,
  delivering clean, zero-OSD HDMI frames up to 240 FPS without any display
  grabbing overhead.
- ``window``: Captures the rendered EverLive gaming viewport via ``x11grab``
  so post-FrameGen (120/180/240 FPS) and on-screen overlays are recorded as seen.
"""

from __future__ import annotations

import glob
import os
import shlex
import shutil
import signal
import subprocess
import time
from dataclasses import dataclass
from datetime import datetime
from typing import Dict, List, Optional, Tuple


REC_FPS_OPTIONS = [
    ("Auto (Match FPS)", "auto"),
    ("240 FPS (4× FG)", "240"),
    ("180 FPS (3× FG)", "180"),
    ("144 FPS", "144"),
    ("120 FPS (2× FG)", "120"),
    ("60 FPS", "60"),
    ("30 FPS", "30"),
]
REC_FPS_KEYS = {key for _, key in REC_FPS_OPTIONS}

REC_SOURCE_OPTIONS = [
    ("Auto (SHM / Viewport)", "auto"),
    ("Direct Hardware (SHM)", "shm"),
    ("Gaming Viewport (FG / OSD)", "window"),
]
REC_SOURCE_KEYS = {key for _, key in REC_SOURCE_OPTIONS}

REC_ENCODER_OPTIONS = [
    ("Auto (GPU → CPU)", "auto"),
    ("H.264 · VAAPI (AMD / Intel)", "h264_vaapi"),
    ("HEVC · VAAPI (AMD / Intel)", "hevc_vaapi"),
    ("H.264 · NVENC (NVIDIA)", "h264_nvenc"),
    ("HEVC · NVENC (NVIDIA)", "hevc_nvenc"),
    ("H.264 · x264 (CPU)", "libx264"),
]
REC_ENCODER_KEYS = {key for _, key in REC_ENCODER_OPTIONS}

REC_QUALITY_OPTIONS = [
    ("High (CQP 18)", "high"),
    ("Ultra (CQP 14)", "ultra"),
    ("Balanced (CQP 22)", "balanced"),
]
REC_QUALITY_KEYS = {key for _, key in REC_QUALITY_OPTIONS}

REC_AUDIO_OPTIONS = [
    ("EverLive HDMI Only", "everlive"),
    ("Full Desktop Audio", "desktop"),
    ("Muted (Video Only)", "mute"),
]
REC_AUDIO_KEYS = {key for _, key in REC_AUDIO_OPTIONS}

REC_FORMAT_OPTIONS = [
    ("MKV", "mkv"),
    ("MP4", "mp4"),
]
REC_FORMAT_KEYS = {key for _, key in REC_FORMAT_OPTIONS}

_QUALITY_QP = {
    "ultra": (14, 14),
    "high": (18, 18),
    "balanced": (22, 21),
}

_ENCODER_PROBE_CACHE: Dict[Tuple[str, str], bool] = {}

DEFAULT_REC_LOG_PATH = os.path.expanduser("~/.config/everlive/recorder.log")


@dataclass(frozen=True)
class RecordingPlan:
    cmd: List[str]
    env: Dict[str, str]
    output_path: str
    fps: int
    source: str
    encoder: str
    width: int
    height: int
    audio_source: Optional[str]


def _read_xdg_videos_dir() -> str:
    env_dir = os.environ.get("XDG_VIDEOS_DIR", "").strip()
    if env_dir:
        return os.path.expandvars(os.path.expanduser(env_dir))

    user_dirs = os.path.expanduser("~/.config/user-dirs.dirs")
    if os.path.isfile(user_dirs):
        try:
            with open(user_dirs, "r", encoding="utf-8", errors="ignore") as f:
                for raw in f:
                    line = raw.strip()
                    if not line or line.startswith("#"):
                        continue
                    if not line.startswith("XDG_VIDEOS_DIR="):
                        continue
                    val = line.split("=", 1)[1].strip()
                    parsed = shlex.split(val)
                    if parsed:
                        expanded = os.path.expandvars(
                            parsed[0].replace("$HOME", os.path.expanduser("~"))
                        )
                        if expanded:
                            return os.path.expanduser(expanded)
        except OSError:
            pass
    return os.path.expanduser("~/Videos")


def default_recording_dir() -> str:
    """Return the default EverLive recordings directory."""
    return os.path.join(_read_xdg_videos_dir(), "EverLive")


def resolve_recording_fps(
    fps_setting: str,
    source_fps: float = 60.0,
    framegen_mode: str = "off",
    fg_multiplier: int = 2,
) -> int:
    """Resolve the target recording frame rate, clamped to [24, 240]."""
    setting = str(fps_setting or "auto").strip().lower()
    if setting != "auto":
        try:
            explicit = int(float(setting))
            return max(24, min(240, explicit))
        except (TypeError, ValueError):
            pass

    base = int(round(source_fps)) if source_fps and source_fps >= 24.0 else 60
    fg = str(framegen_mode or "off").strip().lower()
    if fg not in ("off", "", "none") and fg_multiplier in (2, 3, 4):
        base *= int(fg_multiplier)
    return max(24, min(240, base))


def resolve_capture_source(
    source_setting: str,
    *,
    framegen_active: bool,
    window_id: Optional[int],
    display: Optional[str] = None,
) -> str:
    """Choose between ``'shm'`` (direct hardware feed) and ``'window'``."""
    setting = str(source_setting or "auto").strip().lower()
    disp = display if display is not None else os.environ.get("DISPLAY", "")
    try:
        win_ok = window_id is not None and int(window_id) > 0 and bool(disp)
    except (TypeError, ValueError):
        win_ok = False

    if setting == "window":
        return "window" if win_ok else "shm"
    if setting == "shm":
        return "shm"
    return "window" if (framegen_active and win_ok) else "shm"


def find_vaapi_device() -> Optional[str]:
    """Return the first available DRM render node, or ``None``."""
    for path in sorted(glob.glob("/dev/dri/renderD*")):
        if os.path.exists(path):
            return path
    return None


def probe_encoder_available(
    encoder: str, vaapi_device: Optional[str] = None
) -> bool:
    """Verify with a 1-frame ffmpeg test that @encoder opens on this system."""
    enc = str(encoder or "").strip()
    if not enc:
        return False
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        return False

    is_vaapi = enc.endswith("_vaapi")
    dev = (vaapi_device if vaapi_device is not None else find_vaapi_device()) if is_vaapi else ""
    if is_vaapi and not dev:
        return False

    cache_key = (enc, dev or "")
    if cache_key in _ENCODER_PROBE_CACHE:
        return _ENCODER_PROBE_CACHE[cache_key]

    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error"]
    if is_vaapi:
        cmd.extend(["-init_hw_device", f"vaapi=va:{dev}"])
    cmd.extend(["-f", "lavfi", "-i", "color=c=black:s=128x128:r=30"])
    if is_vaapi:
        cmd.extend(["-vf", "format=nv12,hwupload"])
    else:
        cmd.extend(["-pix_fmt", "yuv420p"])
    cmd.extend(["-c:v", enc, "-frames:v", "1", "-f", "null", "-"])

    ok = False
    try:
        res = subprocess.run(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3.0
        )
        ok = res.returncode == 0
    except (OSError, subprocess.SubprocessError):
        ok = False

    _ENCODER_PROBE_CACHE[cache_key] = ok
    return ok


def resolve_encoder(
    requested: str, vaapi_device: Optional[str] = None
) -> str:
    """Resolve a working video encoder with automatic HW -> SW fallback."""
    req = str(requested or "auto").strip()
    dev = vaapi_device if vaapi_device is not None else find_vaapi_device()

    if req != "auto" and req in REC_ENCODER_KEYS:
        if probe_encoder_available(req, dev):
            return req

    for cand in ("h264_nvenc", "h264_vaapi", "libx264"):
        if probe_encoder_available(cand, dev):
            return cand
    return "libx264"


def ensure_isolated_audio_links(
    audio_tap_node: str = "everlive-capture-audio",
) -> bool:
    """Link ``EverLive Audio`` into @audio_tap_node when not already linked.

    When a session was started without ``--audio-tap``, the SDL audio stream
    still publishes ``EverLive Audio:output_<CH>`` in PipeWire.  Linking those
    ports into the isolated ``everlive-capture-audio`` null sink lets
    ``ffmpeg`` record pure HDMI audio without capturing desktop apps such as
    Discord.
    """
    pw_link = shutil.which("pw-link")
    if not pw_link or not audio_tap_node:
        return False

    target_prefix = f"{audio_tap_node}:playback_"
    try:
        links_out = subprocess.check_output(
            [pw_link, "-l"],
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=1.5,
        )
        in_target_port = False
        for raw in links_out.splitlines():
            if not raw.startswith((" ", "\t")):
                in_target_port = raw.strip().startswith(target_prefix)
            elif in_target_port and "|<-" in raw:
                return True
    except (OSError, subprocess.SubprocessError):
        return False

    try:
        ports_out = subprocess.check_output(
            [pw_link, "-o"],
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=1.5,
        )
    except (OSError, subprocess.SubprocessError):
        return False

    linked_any = False
    for raw in ports_out.splitlines():
        port = raw.strip()
        if not port.startswith("EverLive Audio:output_"):
            continue
        channel = port.split("EverLive Audio:output_", 1)[1]
        if channel not in ("FL", "FR", "FC", "LFE", "RL", "RR"):
            continue
        try:
            res = subprocess.run(
                [pw_link, port, f"{target_prefix}{channel}"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=1.5,
            )
            if res.returncode == 0:
                linked_any = True
        except (OSError, subprocess.SubprocessError):
            pass
    return linked_any


def resolve_pulse_monitor(
    audio_tap_active: bool = False,
    audio_tap_node: str = "everlive-capture-audio",
    *,
    audio_mode: str = "everlive",
    model: str = "gc573",
    duo_input: int = 0,
) -> Optional[str]:
    """Return the PulseAudio/PipeWire audio source name for recording."""
    mode = str(audio_mode or "everlive").strip().lower()
    if mode == "mute":
        return None

    if mode == "everlive":
        if str(model).lower() == "gc570d":
            return "gc570d_hdmi2_audio" if int(duo_input or 0) == 1 else "gc570d_hdmi1_audio"
        if not audio_tap_active:
            ensure_isolated_audio_links(audio_tap_node)
        return f"{audio_tap_node}.monitor" if audio_tap_node else None

    pactl = shutil.which("pactl")
    if not pactl:
        return None
    try:
        sink = subprocess.check_output(
            [pactl, "get-default-sink"],
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=1.5,
        ).strip()
        if sink and not sink.startswith("@"):
            return f"{sink}.monitor"
    except (OSError, subprocess.SubprocessError):
        pass
    return None


def _unique_output_path(
    directory: str,
    model: str,
    fps: int,
    ext: str,
    filename: Optional[str] = None,
) -> str:
    base_dir = os.path.expanduser(directory) if directory else default_recording_dir()
    slug = re_sub_slug(model)
    ext_clean = "mp4" if str(ext).lower() == "mp4" else "mkv"
    if filename:
        stem, existing_ext = os.path.splitext(filename)
        if not existing_ext:
            filename = f"{filename}.{ext_clean}"
        candidate = os.path.join(base_dir, filename)
    else:
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        candidate = os.path.join(base_dir, f"EverLive_{slug}_{stamp}_{fps}fps.{ext_clean}")

    if not os.path.exists(candidate):
        return candidate

    stem, suffix = os.path.splitext(candidate)
    for idx in range(1, 1000):
        alt = f"{stem}_{idx}{suffix}"
        if not os.path.exists(alt):
            return alt
    return candidate


def re_sub_slug(model: str) -> str:
    cleaned = "".join(ch for ch in str(model or "capture").upper() if ch.isalnum())
    return cleaned or "CAPTURE"


def _shm_v4l2_node(model: str, duo_input: int, v4l2_device: Optional[str]) -> str:
    if v4l2_device:
        return v4l2_device
    if str(model).lower() == "gc570d":
        return "/dev/video61" if int(duo_input) == 1 else "/dev/video60"
    if str(model).lower() == "hwsuhd":
        return "/dev/video64"
    return "/dev/video63"


def build_recording_plan(
    *,
    output_dir: str = "",
    model: str = "gc573",
    duo_input: int = 0,
    vfio_mode: bool = True,
    v4l2_device: Optional[str] = None,
    preload_library: Optional[str] = None,
    width: int = 1920,
    height: int = 1080,
    source_fps: float = 60.0,
    fps_setting: str = "auto",
    source_setting: str = "auto",
    framegen_mode: str = "off",
    fg_multiplier: int = 2,
    encoder_setting: str = "auto",
    quality_setting: str = "high",
    container_format: str = "mkv",
    window_id: Optional[int] = None,
    display: Optional[str] = None,
    audio_source: Optional[str] = None,
    vaapi_device: Optional[str] = None,
    resolved_encoder: Optional[str] = None,
    filename: Optional[str] = None,
    ffmpeg_bin: str = "ffmpeg",
) -> RecordingPlan:
    """Build a complete ``RecordingPlan`` (command, env, output path)."""
    even_w = max(2, int(width) & ~1)
    even_h = max(2, int(height) & ~1)
    fps = resolve_recording_fps(
        fps_setting, source_fps=source_fps,
        framegen_mode=framegen_mode, fg_multiplier=fg_multiplier,
    )
    framegen_active = str(framegen_mode or "off").strip().lower() not in ("off", "", "none")
    source = resolve_capture_source(
        source_setting,
        framegen_active=framegen_active,
        window_id=window_id,
        display=display,
    )
    vdev = vaapi_device if vaapi_device is not None else find_vaapi_device()
    encoder = resolved_encoder or resolve_encoder(encoder_setting, vaapi_device=vdev)
    qual = str(quality_setting or "high").strip().lower()
    qp_val, crf_val = _QUALITY_QP.get(qual, (18, 18))
    container = "mp4" if str(container_format).lower() == "mp4" else "mkv"
    out_path = _unique_output_path(output_dir, model, fps, container, filename=filename)

    env: Dict[str, str] = {}
    cmd: List[str] = [ffmpeg_bin, "-hide_banner", "-loglevel", "warning"]

    if encoder in ("h264_vaapi", "hevc_vaapi"):
        va_node = vdev or "/dev/dri/renderD128"
        cmd.extend(["-init_hw_device", f"vaapi=va:{va_node}", "-filter_hw_device", "va"])

    if source == "window":
        disp = display if display is not None else os.environ.get("DISPLAY", ":0")
        cmd.extend([
            "-f", "x11grab",
            "-thread_queue_size", "1024",
            "-draw_mouse", "0",
            "-framerate", str(fps),
            "-window_id", str(int(window_id or 0)),
            "-i", disp,
        ])
    else:
        if vfio_mode:
            node = _shm_v4l2_node(model, duo_input, v4l2_device)
            if preload_library:
                env["LD_PRELOAD"] = preload_library
            cmd.extend([
                "-f", "v4l2",
                "-thread_queue_size", "1024",
                "-input_format", "yuyv422",
                "-framerate", str(fps),
                "-video_size", f"{even_w}x{even_h}",
                "-i", node,
            ])
        else:
            node = v4l2_device or "/dev/video0"
            cmd.extend([
                "-f", "v4l2",
                "-thread_queue_size", "1024",
                "-framerate", str(fps),
                "-video_size", f"{even_w}x{even_h}",
                "-i", node,
            ])

    if audio_source:
        cmd.extend(["-f", "pulse", "-thread_queue_size", "1024", "-i", audio_source])

    if encoder in ("h264_vaapi", "hevc_vaapi"):
        cmd.extend([
            "-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2,format=nv12,hwupload",
            "-c:v", encoder,
            "-qp", str(qp_val),
        ])
    elif encoder in ("h264_nvenc", "hevc_nvenc"):
        cmd.extend([
            "-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2,format=yuv420p",
            "-c:v", encoder,
            "-preset", "p4",
            "-rc", "constqp",
            "-qp", str(qp_val),
        ])
    else:
        cmd.extend([
            "-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2,format=yuv420p",
            "-c:v", encoder,
            "-preset", "veryfast",
            "-crf", str(crf_val),
        ])

    cmd.extend(["-r", str(fps)])

    if audio_source:
        cmd.extend(["-c:a", "aac", "-b:a", "192k", "-ar", "48000"])

    if container == "mp4":
        cmd.extend(["-movflags", "+faststart"])

    cmd.extend(["-y", out_path])

    return RecordingPlan(
        cmd=cmd,
        env=env,
        output_path=out_path,
        fps=fps,
        source=source,
        encoder=encoder,
        width=even_w,
        height=even_h,
        audio_source=audio_source,
    )


def format_duration(seconds: float) -> str:
    total = max(0, int(seconds))
    hours, rem = divmod(total, 3600)
    mins, secs = divmod(rem, 60)
    if hours > 0:
        return f"{hours:02d}:{mins:02d}:{secs:02d}"
    return f"{mins:02d}:{secs:02d}"


def format_bytes(size: int) -> str:
    val = float(max(0, size))
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if val < 1024.0 or unit == "TB":
            if unit in ("B", "KB"):
                return f"{int(val)} {unit}"
            return f"{val:.1f} {unit}"
        val /= 1024.0
    return f"{int(size)} B"


class EverLiveRecorder:
    """Manage the ``ffmpeg`` child process for direct capture recording."""

    def __init__(self, log_path: str = DEFAULT_REC_LOG_PATH):
        self._proc: Optional[subprocess.Popen] = None
        self._plan: Optional[RecordingPlan] = None
        self._started_at: float = 0.0
        self._log_path: str = log_path
        self._log_file = None
        self.last_error: str = ""

    @property
    def log_path(self) -> str:
        return self._log_path

    @property
    def current_plan(self) -> Optional[RecordingPlan]:
        return self._plan

    @property
    def is_recording(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    @property
    def elapsed_seconds(self) -> float:
        if not self._started_at or self._proc is None:
            return 0.0
        return max(0.0, time.monotonic() - self._started_at)

    @property
    def file_size_bytes(self) -> int:
        if self._plan is None:
            return 0
        try:
            return os.path.getsize(self._plan.output_path)
        except OSError:
            return 0

    def format_elapsed(self) -> str:
        return format_duration(self.elapsed_seconds)

    def format_size(self) -> str:
        return format_bytes(self.file_size_bytes)

    def _close_log(self):
        if self._log_file is not None:
            try:
                self._log_file.close()
            except OSError:
                pass
            self._log_file = None

    def _read_log_tail(self, max_lines: int = 8) -> str:
        if not self._log_path or not os.path.isfile(self._log_path):
            return ""
        try:
            with open(self._log_path, "r", encoding="utf-8", errors="ignore") as f:
                lines = [ln.strip() for ln in f.read().splitlines() if ln.strip()]
            return " | ".join(lines[-max_lines:])
        except OSError:
            return ""

    def start(self, plan: RecordingPlan) -> Tuple[bool, str]:
        if self.is_recording:
            return True, "Recording is already running."

        ffmpeg_bin = plan.cmd[0] if plan.cmd else "ffmpeg"
        if not (os.path.isfile(ffmpeg_bin) or shutil.which(ffmpeg_bin)):
            self.last_error = "ffmpeg was not found in PATH."
            return False, self.last_error

        self.last_error = ""
        out_dir = os.path.dirname(plan.output_path)
        if out_dir:
            try:
                os.makedirs(out_dir, exist_ok=True)
            except OSError as exc:
                self.last_error = f"Could not create recording directory: {exc}"
                return False, self.last_error

        log_dir = os.path.dirname(self._log_path)
        if log_dir:
            os.makedirs(log_dir, exist_ok=True)
        self._close_log()
        try:
            self._log_file = open(self._log_path, "w", encoding="utf-8")
        except OSError:
            self._log_file = None

        env = os.environ.copy()
        env.update(plan.env)

        try:
            proc = subprocess.Popen(
                plan.cmd,
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,
                stderr=self._log_file if self._log_file is not None else subprocess.DEVNULL,
                env=env,
                start_new_session=True,
            )
        except OSError as exc:
            self._close_log()
            self.last_error = f"Failed to launch ffmpeg: {exc}"
            return False, self.last_error

        self._proc = proc
        self._plan = plan
        self._started_at = time.monotonic()

        # Catch immediate startup errors (missing device, encoder failure, etc.).
        time.sleep(0.25)
        rc = proc.poll()
        if rc is not None:
            self._close_log()
            tail = self._read_log_tail()
            self.last_error = tail or f"ffmpeg exited immediately with code {rc}"
            self._proc = None
            return False, self.last_error

        return True, f"Recording started → {os.path.basename(plan.output_path)}"

    def poll(self) -> bool:
        """Return True while recording is active; record errors if it died."""
        if self._proc is None:
            return False
        rc = self._proc.poll()
        if rc is None:
            return True
        self._close_log()
        if rc != 0:
            tail = self._read_log_tail()
            self.last_error = tail or f"ffmpeg exited unexpectedly (code {rc})"
        self._proc = None
        return False

    def stop(self) -> Tuple[bool, str]:
        """Gracefully finalize the active recording so container headers flush."""
        if self._proc is None:
            return False, "No active recording."

        proc = self._proc
        plan = self._plan

        if proc.poll() is None:
            # Writing 'q' lets ffmpeg write the MKV cue / MP4 moov atom cleanly.
            if proc.stdin is not None:
                try:
                    proc.stdin.write(b"q\n")
                    proc.stdin.flush()
                    proc.stdin.close()
                except OSError:
                    pass
            try:
                proc.wait(timeout=4.0)
            except subprocess.TimeoutExpired:
                for sig, wait_s in ((signal.SIGINT, 2.0), (signal.SIGTERM, 2.0)):
                    try:
                        os.killpg(os.getpgid(proc.pid), sig)
                    except OSError:
                        try:
                            proc.send_signal(sig)
                        except OSError:
                            pass
                    try:
                        proc.wait(timeout=wait_s)
                        break
                    except subprocess.TimeoutExpired:
                        continue
                else:
                    try:
                        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                    except OSError:
                        try:
                            proc.kill()
                        except OSError:
                            pass
                    try:
                        proc.wait(timeout=1.0)
                    except Exception:
                        pass

        self._close_log()
        self._proc = None
        path = plan.output_path if plan is not None else ""
        size_txt = format_bytes(self.file_size_bytes)
        if path and os.path.isfile(path):
            return True, f"Saved {os.path.basename(path)} ({size_txt})"
        return True, "Recording stopped."
