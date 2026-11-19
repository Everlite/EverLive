#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run and record every installed capture card at once.

Each card gets its own userspace engine process.  The engines already keep
their VFIO device, frame store (``/dev/shm/<model>_v4l2_shm``) and virtual
camera node apart; this module separates the two resources they would
otherwise share:

* the control socket: every engine binds its own
  ``<runtime>/everlive-<model>-<pci>.sock`` (via ``EVERLIVE_SOCKET``), so
  the default ``everlive.sock`` stays free for the Console Hub;
* the audio: the GC573 and HWS UHD engines mirror their audio into their
  own PipeWire sink (``--audio-tap=everlive-<model>-<pci>-audio``) and
  recordings read that sink's monitor.  The GC570D publishes its own
  per-input sinks.

Recording starts one ``ffmpeg`` per input with ``build_recording_plan``.
All files of one take share a timestamp, so they line up in an editor.

The module is Qt-free; ``play.sh --all`` prepares every card (build,
cap_ipc_lock, vfio-pci binding) and then runs it.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import select
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from typing import Dict, List, Optional, Sequence, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import app_integrations  # noqa: E402
import everlive_client  # noqa: E402
import everlive_recorder  # noqa: E402
from card_profile import (  # noqa: E402
    MODEL_GC570D,
    MODEL_GC573,
    MODEL_HWSUHD,
    detect_pci_cards,
    static_profile,
)

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.path.dirname(HERE)

# Build-tree binary, then the installed name on $PATH.
ENGINE_BINARIES = {
    MODEL_GC573: ("opengc573-userd", "gc573_user_streamer"),
    MODEL_GC570D: ("opengc570d-userd", "opengc570d-userd"),
    MODEL_HWSUHD: ("openhws-userd", "hws_user_streamer"),
}

SHORT_NAMES = {MODEL_GC573: "GC573", MODEL_GC570D: "GC570D",
               MODEL_HWSUHD: "HWS UHD"}

# GC570D inputs publish fixed PipeWire sinks (see resolve_pulse_monitor).
GC570D_AUDIO_SOURCES = ("gc570d_hdmi1_audio", "gc570d_hdmi2_audio")

FALLBACK_GEOMETRY = (1920, 1080, 60.0)



def log(msg: str) -> None:
    print(f"[multi] {msg}", flush=True)


# ---- naming ------------------------------------------------------------------

def pci_slug(pci: str) -> str:
    """``0000:84:00.0`` -> ``84-00-0``; a non-zero domain is kept."""
    text = str(pci or "").lower()
    if text.startswith("0000:"):
        text = text[5:]
    return re.sub(r"[^0-9a-z]+", "-", text).strip("-") or "card"


def session_socket_path(model: str, pci: str,
                        runtime_dir: Optional[str] = None) -> str:
    base = runtime_dir or everlive_client.runtime_dir()
    return os.path.join(base, f"everlive-{model}-{pci_slug(pci)}.sock")


def audio_tap_name(model: str, pci: str) -> str:
    return f"everlive-{model}-{pci_slug(pci)}-audio"


def card_label(card: Dict[str, Optional[str]]) -> str:
    pci = str(card.get("pci") or "")
    if pci.startswith("0000:"):
        pci = pci[5:]
    return f"{SHORT_NAMES.get(card.get('model') or '', 'Card')} · {pci}"


# ---- engines -----------------------------------------------------------------

def find_engine_binary(model: str, tools_dir: str = TOOLS_DIR) -> Optional[str]:
    entry = ENGINE_BINARIES.get(model)
    if entry is None:
        return None
    subdir, name = entry
    local = os.path.join(tools_dir, subdir, name)
    if os.path.isfile(local) and os.access(local, os.X_OK):
        return local
    return shutil.which(name)


def build_engine_command(
    card: Dict[str, Optional[str]],
    binary: str,
    *,
    socket: str,
    headless: bool = False,
    parent_winid: int = 0,
    silent_playback: bool = False,
    extra_args: Sequence[str] = (),
) -> Tuple[List[str], Dict[str, str]]:
    """Engine argv and the environment additions for one card.

    ``parent_winid`` embeds the engine's SDL window into that X11 window
    (Console Hub multiview tile).  ``silent_playback`` prepares the engine's
    local playback stream to be muted on its own (see
    :meth:`MultiCardRunner.apply_playback_mute`); the audio tap is a
    separate stream, so recordings keep their sound.
    """
    model = card["model"]
    pci = card["pci"]
    env = {"EVERLIVE_SOCKET": socket}
    embed = int(parent_winid or 0) if not headless else 0
    if model == MODEL_GC570D:
        cmd = [binary, "--pci", pci, "--headless" if headless else "--sdl"]
    else:
        # Both engines parse --audio-tap with an optional argument, which
        # only binds in the --opt=value form.
        cmd = [binary, pci, f"--audio-tap={audio_tap_name(model, pci)}"]
        if headless:
            cmd.append("--headless")
        elif embed > 0:
            cmd.extend(["--parent-winid", str(embed)])
    if embed > 0:
        # The GC570D engine only reads the environment variable.
        env["EVERLIVE_PARENT_WINID"] = str(embed)
        env["SDL_VIDEODRIVER"] = "x11"
    if silent_playback:
        # SDL plays through the PulseAudio API, whose PULSE_PROP_* variables
        # reach only that playback stream (PIPEWIRE_PROPS would also hit the
        # native audio-tap stream).  Without restore, WirePlumber neither
        # remembers the mute applied here nor carries it over to a normal
        # session of the same engine.
        env["SDL_AUDIODRIVER"] = "pulseaudio"
        env["PULSE_PROP_state.restore-props"] = "false"
        env["PULSE_PROP_state.restore-target"] = "false"
    cmd.extend(extra_args)
    return cmd, env


def _pactl(*args: str, timeout: float = 3.0) -> Optional[str]:
    pactl = shutil.which("pactl")
    if not pactl:
        return None
    try:
        res = subprocess.run([pactl, *args], capture_output=True, text=True,
                             timeout=timeout)
    except (OSError, subprocess.SubprocessError):
        return None
    return res.stdout.strip() if res.returncode == 0 else None


def list_sink_inputs() -> List[Dict[str, object]]:
    """``[{"id", "pid", "sink", "mute", "volume"}]`` for every playback stream.

    ``volume`` is the first channel's level in percent.
    """
    pactl = shutil.which("pactl")
    if not pactl:
        return []
    try:
        res = subprocess.run([pactl, "--format=json", "list", "sink-inputs"],
                             capture_output=True, text=True, timeout=3.0)
        items = json.loads(res.stdout or "[]") if res.returncode == 0 else []
    except (OSError, subprocess.SubprocessError, ValueError):
        return []
    sinks = {}
    out = _pactl("list", "short", "sinks") or ""
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) > 1:
            sinks[parts[0]] = parts[1]
    streams = []
    for item in items if isinstance(items, list) else []:
        props = item.get("properties") or {}
        try:
            pid = int(props.get("application.process.id") or 0)
        except (TypeError, ValueError):
            pid = 0
        volume = 100
        channels = item.get("volume") or {}
        if isinstance(channels, dict) and channels:
            first = next(iter(channels.values()))
            try:
                volume = int(str(first.get("value_percent", "100%")).rstrip("%"))
            except (AttributeError, TypeError, ValueError):
                pass
        streams.append({
            "id": str(item.get("index")),
            "pid": pid,
            "sink": sinks.get(str(item.get("sink")), str(item.get("sink"))),
            "mute": bool(item.get("mute")),
            "volume": volume,
        })
    return streams


@dataclass
class RecordingSource:
    """One recordable input: a card, or one HDMI input of a GC570D."""

    label: str
    model: str
    pci: str
    input_index: int
    camera_node: str
    preload_library: Optional[str]
    audio_source: Optional[str]
    recorder: Optional[everlive_recorder.EverLiveRecorder] = None
    active: bool = False


def recording_sources(card: Dict[str, Optional[str]],
                      preload_library: Optional[str] = None
                      ) -> List[RecordingSource]:
    model = card["model"]
    pci = card["pci"]
    profile = static_profile(model)
    sources = []
    for idx, node in enumerate(profile.preload_nodes):
        if model == MODEL_GC570D:
            audio = GC570D_AUDIO_SOURCES[idx] if idx < len(GC570D_AUDIO_SOURCES) else None
            label = f"{card_label(card)} HDMI {idx + 1}"
        else:
            audio = f"{audio_tap_name(model, pci)}.monitor"
            label = card_label(card)
        sources.append(RecordingSource(
            label=label, model=model, pci=pci, input_index=idx,
            camera_node=node, preload_library=preload_library,
            audio_source=audio))
    return sources


@dataclass
class CardSession:
    card: Dict[str, Optional[str]]
    socket: str
    cmd: List[str] = field(default_factory=list)
    env: Dict[str, str] = field(default_factory=dict)
    proc: Optional[subprocess.Popen] = None
    log_path: str = ""
    sources: List[RecordingSource] = field(default_factory=list)
    parent_winid: int = 0
    listening: bool = False
    volume: int = 100  # local playback level in percent (0..150)

    @property
    def label(self) -> str:
        return card_label(self.card)

    @property
    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def status(self, timeout: float = 0.5) -> Optional[dict]:
        return everlive_client.call("status.get", path=self.socket,
                                    timeout=timeout)


def signal_geometry(status: Optional[dict]) -> Tuple[int, int, float, bool]:
    """``(width, height, fps, locked)`` from a ``status.get`` result."""
    if not isinstance(status, dict):
        w, h, fps = FALLBACK_GEOMETRY
        return w, h, fps, False
    sig = status.get("signal") or {}
    locked = bool(sig.get("locked"))
    try:
        w = int(sig.get("width") or 0)
        h = int(sig.get("height") or 0)
        fps = float(sig.get("rate_hz") or 0.0)
    except (TypeError, ValueError):
        w = h = 0
        fps = 0.0
    if not locked or w <= 0 or h <= 0:
        fw, fh, ffps = FALLBACK_GEOMETRY
        return fw, fh, ffps, False
    return w, h, (fps if fps >= 24.0 else FALLBACK_GEOMETRY[2]), True


def take_filename_for(model: str, pci: str, input_index: int, stamp: str,
                      fps: int, container: str) -> str:
    """File name inside one take: ``EverLive_<stamp>_<MODEL>_<pci>[_HDMIn]_<fps>fps``."""
    ext = "mp4" if container == "mp4" else "mkv"
    slug = everlive_recorder.re_sub_slug(model)
    suffix = f"_HDMI{input_index + 1}" if model == MODEL_GC570D else ""
    return f"EverLive_{stamp}_{slug}_{pci_slug(pci)}{suffix}_{fps}fps.{ext}"


def take_filename(source: RecordingSource, stamp: str, fps: int,
                  container: str) -> str:
    return take_filename_for(source.model, source.pci, source.input_index,
                             stamp, fps, container)


def take_stamp() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S")


class MultiCardRunner:
    """Start one engine per card and record all of them together."""

    def __init__(self, cards: Sequence[Dict[str, Optional[str]]], *,
                 headless: bool = False,
                 rec_dir: str = "",
                 encoder: str = "auto",
                 quality: str = "high",
                 container: str = "mkv",
                 log_dir: Optional[str] = None,
                 parent_winids: Optional[Dict[str, int]] = None,
                 silent_playback: bool = False,
                 extra_args: Sequence[str] = ()):
        self.headless = headless
        self.silent_playback = silent_playback
        self._last_mute_check = 0.0
        self._fast_mute_until = 0.0
        self.rec_dir = rec_dir or everlive_recorder.default_recording_dir()
        self.encoder_setting = encoder
        self.quality = quality
        self.container = "mp4" if container == "mp4" else "mkv"
        self.log_dir = log_dir or os.path.expanduser("~/.config/everlive/multi")
        self.extra_args = list(extra_args)
        self.sessions: List[CardSession] = [
            CardSession(card=dict(card),
                        socket=session_socket_path(card["model"], card["pci"]))
            for card in cards
        ]
        for sess in self.sessions:
            sess.parent_winid = int((parent_winids or {}).get(sess.card["pci"], 0))
        self._resolved_encoder: Optional[str] = None

    def session_for(self, pci: str) -> Optional[CardSession]:
        return next((s for s in self.sessions if s.card["pci"] == pci), None)

    def engine_pids(self) -> List[int]:
        return [s.proc.pid for s in self.sessions if s.alive]

    # -- engines -----------------------------------------------------------

    def start_engines(self) -> int:
        os.makedirs(self.log_dir, exist_ok=True)
        self._fast_mute_until = time.monotonic() + 10.0
        started = 0
        for sess in self.sessions:
            model = sess.card["model"]
            binary = find_engine_binary(model)
            if not binary:
                log(f"{sess.label}: engine binary not found; skipped.")
                continue
            if sess.card.get("mode") != "vfio":
                log(f"{sess.label}: not bound to vfio-pci (driver: "
                    f"{sess.card.get('driver') or 'none'}); skipped. "
                    "Run play.sh --all to bind it.")
                continue
            if everlive_client.probe(path=sess.socket) is not None:
                log(f"{sess.label}: an engine already serves {sess.socket}; skipped.")
                continue
            preload = app_integrations.find_preload_library(static_profile(model))
            sess.sources = recording_sources(sess.card, preload)
            sess.cmd, sess.env = build_engine_command(
                sess.card, binary, socket=sess.socket,
                headless=self.headless, parent_winid=sess.parent_winid,
                silent_playback=self.silent_playback,
                extra_args=self.extra_args)
            sess.log_path = os.path.join(
                self.log_dir, f"{model}-{pci_slug(sess.card['pci'])}.log")
            env = os.environ.copy()
            env.update(sess.env)
            try:
                log_file = open(sess.log_path, "w", encoding="utf-8")
            except OSError:
                log_file = subprocess.DEVNULL
            try:
                sess.proc = subprocess.Popen(
                    sess.cmd, env=env, stdin=subprocess.DEVNULL,
                    stdout=log_file, stderr=subprocess.STDOUT,
                    start_new_session=True)
            except OSError as exc:
                log(f"{sess.label}: failed to start engine: {exc}")
                continue
            finally:
                if log_file is not subprocess.DEVNULL:
                    log_file.close()
            started += 1
            log(f"{sess.label}: engine pid {sess.proc.pid}, socket "
                f"{sess.socket}, log {sess.log_path}")
        return started

    def wait_for_sockets(self, timeout: float = 15.0) -> None:
        deadline = time.monotonic() + timeout
        pending = [s for s in self.sessions if s.alive]
        while pending and time.monotonic() < deadline:
            for sess in list(pending):
                if not sess.alive:
                    pending.remove(sess)
                elif everlive_client.probe(path=sess.socket) is not None:
                    log(f"{sess.label}: control socket ready.")
                    pending.remove(sess)
            if pending:
                time.sleep(0.2)
        for sess in pending:
            log(f"{sess.label}: control socket not ready after {timeout:.0f}s "
                "(recording falls back to 1920x1080@60).")

    # -- local listening ---------------------------------------------------

    def set_listen(self, pci: str, enabled: bool) -> bool:
        """Unmute (or mute) one card's local playback.

        Only meaningful with ``silent_playback``.  Recordings are not
        affected: they read the card's audio tap.
        """
        sess = self.session_for(pci)
        if sess is None:
            return False
        sess.listening = bool(enabled)
        self.apply_playback_mute()
        return True

    def set_volume(self, pci: str, percent: int) -> bool:
        """Local playback level of one card (0..150 %); recordings keep
        the card's own level because they read its audio tap."""
        sess = self.session_for(pci)
        if sess is None:
            return False
        sess.volume = max(0, min(150, int(percent)))
        self.apply_playback_mute()
        return True

    def playback_stream_ids(self) -> List[str]:
        """Sink-input ids of the quiet engines' playback streams."""
        pids = {s.proc.pid for s in self.sessions if s.alive}
        return [st["id"] for st in list_sink_inputs() if st["pid"] in pids]

    def apply_playback_mute(self) -> None:
        """Mute every quiet engine's playback stream (not its tap) unless
        that card is being listened to, and apply its volume.

        Engines may reopen their SDL stream (format changes), so the Hub
        calls this from its poll loop.
        """
        if not self.silent_playback:
            return
        wanted = {s.proc.pid: s for s in self.sessions if s.alive}
        if not wanted:
            return
        for stream in list_sink_inputs():
            sess = wanted.get(stream["pid"])
            if sess is None:
                continue
            if stream["sink"] == audio_tap_name(sess.card["model"], sess.card["pci"]):
                continue
            mute = not sess.listening
            if stream["mute"] != mute:
                _pactl("set-sink-input-mute", stream["id"], "1" if mute else "0")
            if stream.get("volume", 100) != sess.volume:
                _pactl("set-sink-input-volume", stream["id"], f"{sess.volume}%")

    def stop_engines(self, timeout: float = 5.0) -> None:
        for sess in self.sessions:
            if sess.alive:
                try:
                    os.killpg(sess.proc.pid, signal.SIGTERM)
                except OSError:
                    sess.proc.terminate()
        deadline = time.monotonic() + timeout
        for sess in self.sessions:
            if sess.proc is None:
                continue
            try:
                sess.proc.wait(timeout=max(0.1, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                log(f"{sess.label}: engine did not exit; killing it.")
                try:
                    os.killpg(sess.proc.pid, signal.SIGKILL)
                except OSError:
                    sess.proc.kill()
                sess.proc.wait(timeout=2.0)
            sess.proc = None

    # -- recording ---------------------------------------------------------

    def configure_recording(self, *, rec_dir: Optional[str] = None,
                            encoder: Optional[str] = None,
                            quality: Optional[str] = None,
                            container: Optional[str] = None) -> None:
        if rec_dir:
            self.rec_dir = rec_dir
        if encoder and encoder != self.encoder_setting:
            self.encoder_setting = encoder
            self._resolved_encoder = None
        if quality:
            self.quality = quality
        if container:
            self.container = "mp4" if container == "mp4" else "mkv"

    @property
    def is_recording(self) -> bool:
        return any(src.active for sess in self.sessions for src in sess.sources)

    def _encoder(self) -> str:
        if self._resolved_encoder is None:
            self._resolved_encoder = everlive_recorder.resolve_encoder(
                self.encoder_setting,
                vaapi_device=everlive_recorder.find_vaapi_device())
        return self._resolved_encoder

    def build_plans(self, stamp: Optional[str] = None
                    ) -> List[Tuple[CardSession, RecordingSource,
                                    everlive_recorder.RecordingPlan]]:
        stamp = stamp or take_stamp()
        plans = []
        for sess in self.sessions:
            if not sess.alive:
                continue
            status = sess.status()
            w, h, fps, locked = signal_geometry(status)
            if sess.card["model"] == MODEL_GC570D:
                # The status reports the active input only; DUO inputs are
                # 1080p60 at most.
                w, h, fps = FALLBACK_GEOMETRY
            elif not locked:
                log(f"{sess.label}: no input signal; recording at "
                    f"{w}x{h}@{fps:.0f}.")
            for src in sess.sources:
                rec_fps = everlive_recorder.resolve_recording_fps(
                    "auto", source_fps=fps)
                plan = everlive_recorder.build_recording_plan(
                    output_dir=self.rec_dir,
                    model=src.model,
                    duo_input=src.input_index,
                    vfio_mode=True,
                    v4l2_device=src.camera_node,
                    preload_library=src.preload_library,
                    width=w,
                    height=h,
                    source_fps=fps,
                    source_setting="shm",
                    encoder_setting=self.encoder_setting,
                    resolved_encoder=self._encoder(),
                    quality_setting=self.quality,
                    container_format=self.container,
                    audio_source=src.audio_source,
                    filename=take_filename(src, stamp, rec_fps, self.container),
                )
                plans.append((sess, src, plan))
        return plans

    def start_recording(self, stamp: Optional[str] = None) -> int:
        if self.is_recording:
            return 0
        started = 0
        for sess, src, plan in self.build_plans(stamp):
            if src.recorder is None:
                src.recorder = everlive_recorder.EverLiveRecorder(
                    log_path=os.path.join(
                        self.log_dir,
                        f"rec-{src.model}-{pci_slug(src.pci)}-{src.input_index}.log"))
            ok, msg = src.recorder.start(plan)
            src.active = ok
            if ok:
                started += 1
                log(f"{src.label}: recording {plan.width}x{plan.height}@{plan.fps} "
                    f"({plan.encoder}) -> {plan.output_path}")
            else:
                log(f"{src.label}: recording failed: {msg}")
        return started

    @staticmethod
    def _stop_source(src: RecordingSource) -> None:
        if not src.active or src.recorder is None:
            return
        src.active = False
        _ok, msg = src.recorder.stop()
        log(f"{src.label}: {msg}")

    def stop_recording(self) -> None:
        for sess in self.sessions:
            for src in sess.sources:
                self._stop_source(src)

    def poll(self) -> bool:
        """Reap dead engines/recorders; False once no engine is running."""
        now = time.monotonic()
        # Check on every poll while engines are opening their playback
        # streams, so a quiet card is not heard for a moment at start.
        interval = 0.0 if now < self._fast_mute_until else 2.0
        if self.silent_playback and now - self._last_mute_check >= interval:
            self._last_mute_check = now
            self.apply_playback_mute()
        for sess in self.sessions:
            if sess.proc is not None and not sess.alive:
                rc = sess.proc.returncode
                log(f"{sess.label}: engine exited (code {rc}); see {sess.log_path}")
                for src in sess.sources:
                    self._stop_source(src)
                sess.proc = None
            for src in sess.sources:
                if src.active and not src.recorder.poll():
                    src.active = False
                    log(f"{src.label}: recording stopped: "
                        f"{src.recorder.last_error or 'ffmpeg exited'}")
        return any(s.alive for s in self.sessions)

    def print_status(self) -> None:
        for sess in self.sessions:
            if not sess.alive:
                log(f"{sess.label}: not running")
                continue
            w, h, fps, locked = signal_geometry(sess.status())
            sig = f"{w}x{h}@{fps:.2f}" if locked else "no signal"
            recs = []
            for src in sess.sources:
                if src.active:
                    rec = src.recorder
                    recs.append(f"REC {rec.format_elapsed()} {rec.format_size()}")
            log(f"{sess.label}: {sig}" + (f" · {', '.join(recs)}" if recs else ""))


# ---- CLI ---------------------------------------------------------------------

def select_cards(cards: List[Dict[str, Optional[str]]],
                 wanted: Sequence[str]) -> List[Dict[str, Optional[str]]]:
    """Filter by PCI address (``84:00.0`` or full) or model slug."""
    if not wanted:
        return list(cards)
    picked = []
    for card in cards:
        pci = str(card["pci"])
        short = pci[5:] if pci.startswith("0000:") else pci
        for item in wanted:
            item = item.strip().lower()
            if item in (pci.lower(), short.lower(), str(card["model"])):
                picked.append(card)
                break
    return picked


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="everlive_multi",
        description="Display and record every installed capture card at once.")
    p.add_argument("--list", action="store_true",
                   help="list supported cards and exit")
    p.add_argument("--cards", default="",
                   help="comma-separated PCI addresses or models (default: all)")
    p.add_argument("--record", action="store_true",
                   help="start recording all cards once the engines are up")
    p.add_argument("--duration", type=float, default=0.0,
                   help="with --record: stop and exit after N seconds")
    p.add_argument("--headless", action="store_true",
                   help="no engine windows (capture, audio tap and recording only)")
    p.add_argument("--rec-dir", default="", help="recording directory")
    p.add_argument("--encoder", default="auto",
                   choices=sorted(everlive_recorder.REC_ENCODER_KEYS))
    p.add_argument("--quality", default="high",
                   choices=sorted(everlive_recorder.REC_QUALITY_KEYS))
    p.add_argument("--format", dest="container", default="mkv",
                   choices=sorted(everlive_recorder.REC_FORMAT_KEYS))
    return p.parse_args(argv)


HELP_TEXT = "Commands: r = start/stop recording, s = status, q = quit."


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    cards = detect_pci_cards()

    if args.list:
        if not cards:
            print("No supported PCIe capture card found.")
        for card in cards:
            print(f"{card['pci']}  {card['model']:<7} {card['mode']:<8} "
                  f"driver={card['driver'] or 'none'}")
        return 0

    cards = select_cards(cards, [c for c in args.cards.split(",") if c.strip()])
    if not cards:
        log("No supported capture card selected.")
        return 1

    default = everlive_client.probe()
    if default is not None:
        log("An EverLive engine is already running on the default socket "
            f"({default.get('card') or 'card'} pid {default.get('pid')}). "
            "Close the Console Hub / single-card session first.")
        return 1

    runner = MultiCardRunner(
        cards, headless=args.headless, rec_dir=args.rec_dir,
        encoder=args.encoder, quality=args.quality, container=args.container)

    stop = {"flag": False, "toggle": False}

    def _on_term(_sig, _frm):
        stop["flag"] = True

    def _on_usr1(_sig, _frm):
        stop["toggle"] = True

    signal.signal(signal.SIGINT, _on_term)
    signal.signal(signal.SIGTERM, _on_term)
    signal.signal(signal.SIGUSR1, _on_usr1)

    if runner.start_engines() == 0:
        log("No engine started.")
        return 1
    runner.wait_for_sockets()

    record_deadline = 0.0
    if args.record:
        if runner.start_recording() and args.duration > 0:
            record_deadline = time.monotonic() + args.duration

    interactive = sys.stdin.isatty()
    if interactive:
        log(HELP_TEXT)
    log(f"Running (pid {os.getpid()}); Ctrl-C stops recordings and engines, "
        f"SIGUSR1 toggles recording.")

    try:
        while not stop["flag"]:
            if stop["toggle"]:
                stop["toggle"] = False
                if runner.is_recording:
                    runner.stop_recording()
                else:
                    runner.start_recording()
            if record_deadline and time.monotonic() >= record_deadline:
                log(f"Duration of {args.duration:.0f}s reached.")
                break
            if not runner.poll():
                log("All engines have exited.")
                break
            if interactive:
                ready, _, _ = select.select([sys.stdin], [], [], 0.5)
                if ready:
                    line = sys.stdin.readline()
                    if not line:
                        interactive = False
                        continue
                    cmd = line.strip().lower()
                    if cmd == "r":
                        stop["toggle"] = True
                    elif cmd == "s":
                        runner.print_status()
                    elif cmd in ("q", "quit", "exit"):
                        break
                    elif cmd:
                        log(HELP_TEXT)
            else:
                time.sleep(0.5)
    finally:
        runner.stop_recording()
        runner.stop_engines()
    return 0


if __name__ == "__main__":
    sys.exit(main())
