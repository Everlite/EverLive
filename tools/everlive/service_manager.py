#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Manage the EverLive background capture user service.

The service keeps the capture engine, HDMI passthrough, shared frame store and
optional virtual-camera outputs alive without the Console Hub.  Everything here
is Qt-free so it can be unit-tested and reused by CLI tooling.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Sequence, Tuple


UNIT_NAME = "everlive.service"
GC570D_UNIT_NAME = "opengc570d-userd.service"
AUDIO_TAP_NODE = "everlive-capture-audio"
CONFIG_DIR = Path(os.path.expanduser("~/.config/everlive"))
SERVICE_ENV = CONFIG_DIR / "service.env"
USER_UNIT_DIR = Path(os.path.expanduser("~/.config/systemd/user"))

_RESTART_POLICY = """\
[Unit]
Description=EverLive Universal Userspace Capture Service
After=pipewire.service wireplumber.service
Wants=pipewire.service

[Service]
Type=simple
ExecStart={exec_start}
Restart=on-failure
RestartSec=2
LimitMEMLOCK=infinity

[Install]
WantedBy=default.target
"""


@dataclass(frozen=True)
class ServiceStatus:
    installed: bool = False
    enabled: bool = False
    active: bool = False
    unit_path: str = ""
    service_path: str = ""
    detail: str = ""

    @property
    def label(self) -> str:
        if self.active:
            return "Background service active"
        if self.enabled:
            return "Background service enabled · waiting to start"
        if self.installed:
            return "Background service installed · disabled"
        return "Background service not installed"

    @property
    def tone(self) -> str:
        if self.active:
            return "live"
        if self.installed:
            return "idle"
        return "error"


def _run(cmd: Sequence[str], timeout: float = 4.0):
    try:
        return subprocess.run(
            list(cmd), capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.SubprocessError):
        return None


def systemd_available() -> bool:
    if shutil.which("systemctl") is None:
        return False
    out = _run(["systemctl", "--user", "show-environment"], timeout=2.0)
    return out is not None and out.returncode == 0


def find_service_script() -> Optional[str]:
    found = shutil.which("everlive-service")
    if found:
        return found
    here = Path(__file__).resolve().parent
    candidates = [
        here.parent / "libeverlive-core" / "everlive-service.sh",
        Path("/usr/local/bin/everlive-service"),
        Path("/usr/bin/everlive-service"),
        Path("/usr/lib/everlive/tools/libeverlive-core/everlive-service.sh"),
        Path("/usr/local/lib/everlive/tools/libeverlive-core/everlive-service.sh"),
    ]
    for path in candidates:
        if path.is_file():
            return str(path)
    return None


def _user_unit_path() -> Path:
    return USER_UNIT_DIR / UNIT_NAME


def unit_fragment_path() -> str:
    out = _run(["systemctl", "--user", "show", "-p", "FragmentPath",
                UNIT_NAME], timeout=2.0)
    if out is not None and out.returncode == 0:
        for line in out.stdout.splitlines():
            if line.startswith("FragmentPath="):
                return line.split("=", 1)[1].strip()
    for candidate in (
            _user_unit_path(),
            Path("/usr/local/lib/systemd/user") / UNIT_NAME,
            Path("/usr/lib/systemd/user") / UNIT_NAME):
        if candidate.is_file():
            return str(candidate)
    return ""


def _is_enabled(unit: str = UNIT_NAME) -> bool:
    out = _run(["systemctl", "--user", "is-enabled", "--quiet", unit],
               timeout=2.0)
    return out is not None and out.returncode == 0


def _is_active(unit: str = UNIT_NAME) -> bool:
    out = _run(["systemctl", "--user", "is-active", "--quiet", unit],
               timeout=2.0)
    return out is not None and out.returncode == 0


def status() -> ServiceStatus:
    fragment = unit_fragment_path()
    service = find_service_script() or ""
    installed = bool(fragment) or _is_enabled() or _is_active()
    detail = ""
    if not systemd_available():
        detail = "systemd user session unavailable"
    elif not installed:
        detail = "Install the background service to keep OBS fed after the Hub closes."
    return ServiceStatus(
        installed=installed,
        enabled=_is_enabled(),
        active=_is_active(),
        unit_path=fragment,
        service_path=service,
        detail=detail,
    )


def render_unit(exec_start: str) -> str:
    return _RESTART_POLICY.format(exec_start=exec_start)


def install_unit() -> Tuple[bool, str]:
    """Install a user unit when the package did not provide one."""
    if unit_fragment_path() and Path(unit_fragment_path()).is_file():
        return True, "Background service unit is already installed."
    if not systemd_available():
        return False, "The systemd user session is not available."
    service = find_service_script()
    if not service:
        return False, (
            "everlive-service was not found. Reinstall EverLive or run "
            "make install first.")
    USER_UNIT_DIR.mkdir(parents=True, exist_ok=True)
    path = _user_unit_path()
    path.write_text(render_unit(service), encoding="utf-8")
    out = _run(["systemctl", "--user", "daemon-reload"], timeout=5.0)
    if out is None or out.returncode != 0:
        return False, "Unit written, but systemctl --user daemon-reload failed."
    return True, f"Installed {path}."


def enable() -> Tuple[bool, str]:
    out = _run(["systemctl", "--user", "enable", "--now", UNIT_NAME],
               timeout=8.0)
    if out is not None and out.returncode == 0:
        return True, "Background service enabled at login and started."
    if out is not None and out.returncode != 0:
        return False, (out.stderr or out.stdout or
                       "Failed to enable the background service.").strip()
    return False, "systemctl --user is unavailable."


def disable() -> Tuple[bool, str]:
    out = _run(["systemctl", "--user", "disable", "--now", UNIT_NAME],
               timeout=8.0)
    if out is not None and out.returncode == 0:
        return True, "Background service stopped and disabled."
    if out is not None and out.returncode != 0:
        return False, (out.stderr or out.stdout or
                       "Failed to disable the background service.").strip()
    return False, "systemctl --user is unavailable."


def start() -> Tuple[bool, str]:
    out = _run(["systemctl", "--user", "start", UNIT_NAME], timeout=8.0)
    if out is not None and out.returncode == 0:
        return True, "Background service started."
    if out is not None and out.returncode != 0:
        return False, (out.stderr or out.stdout or
                       "Failed to start the background service.").strip()
    return False, "systemctl --user is unavailable."


def stop() -> Tuple[bool, str]:
    out = _run(["systemctl", "--user", "stop", UNIT_NAME], timeout=8.0)
    if out is not None and out.returncode == 0:
        return True, "Background service stopped."
    if out is not None and out.returncode != 0:
        return False, (out.stderr or out.stdout or
                       "Failed to stop the background service.").strip()
    return False, "systemctl --user is unavailable."


def uninstall_unit() -> Tuple[bool, str]:
    """Remove a user-installed unit; never delete a distro-owned unit."""
    fragment = unit_fragment_path()
    if not fragment:
        return False, "No background service unit is installed."
    path = Path(fragment)
    if path.parent != USER_UNIT_DIR:
        return False, (
            "The unit is owned by the package. Disable it instead, or "
            "remove it with your package manager.")
    stop()
    _run(["systemctl", "--user", "disable", UNIT_NAME], timeout=5.0)
    try:
        path.unlink()
    except OSError as exc:
        return False, f"Could not remove {path}: {exc}"
    _run(["systemctl", "--user", "daemon-reload"], timeout=5.0)
    return True, "Background service unit removed."


def logs(lines: int = 120) -> str:
    out = _run(["journalctl", "--user", "-u", UNIT_NAME,
                "-n", str(max(1, lines)), "--no-pager"], timeout=5.0)
    if out is not None and out.stdout.strip():
        return out.stdout.strip()
    out = _run(["systemctl", "--user", "status", UNIT_NAME, "--no-pager"],
               timeout=5.0)
    if out is not None and (out.stdout.strip() or out.stderr.strip()):
        return (out.stdout or out.stderr).strip()
    return "No service log is available yet."


def read_service_args() -> str:
    try:
        text = SERVICE_ENV.read_text(encoding="utf-8")
    except OSError:
        return ""
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("EVERLIVE_SERVICE_ARGS="):
            value = line.split("=", 1)[1].strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
                value = value[1:-1]
            return value
    return ""


def write_service_args(v4l2_device: str = "", audio_tap: bool = False) -> str:
    parts = []
    if v4l2_device:
        parts.extend(["--v4l2", v4l2_device])
    if audio_tap:
        parts.extend(["--audio-tap", AUDIO_TAP_NODE])
    args = " ".join(parts)
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    SERVICE_ENV.write_text(
        "# Managed by the EverLive Console Hub.\n"
        f'EVERLIVE_SERVICE_ARGS="{args}"\n', encoding="utf-8")
    return args


def service_env_path() -> str:
    return str(SERVICE_ENV)
