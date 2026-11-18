#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Per-application virtual-camera launchers for the module-free path.

The engine publishes frames into the shared frame store.  A preload library
exposes those frames as a V4L2 node inside one process, so OBS, Discord or a
browser can use the camera without v4l2loopback.  This module creates separate
desktop shortcuts instead of setting a session-wide ``LD_PRELOAD``.
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Sequence, Tuple


def application_dirs() -> Tuple[Path, ...]:
    return (
        Path.home() / ".local/share/applications",
        Path("/usr/local/share/applications"),
        Path("/usr/share/applications"),
        Path("/var/lib/flatpak/exports/share/applications"),
        Path.home() / ".local/share/flatpak/exports/share/applications",
        Path("/var/lib/snapd/desktop/applications"),
    )

# Exact desktop IDs and display names, plus a few stable prefixes.  Matching
# on arbitrary substrings catches Discord game activity entries and launchers
# such as "OBS Now Playing", which are not camera apps.
KNOWN_APPS = {
    "obs": (
        {"obs", "com.obsproject.studio"},
        (),
        {"obs studio"},
    ),
    "discord": (
        {"discord", "com.discordapp.discord"},
        (),
        {"discord"},
    ),
    "chrome": (
        {"google-chrome", "google-chrome-stable", "chrome"},
        ("google-chrome-",),
        {"google chrome"},
    ),
    "chromium": (
        {"chromium", "chromium-browser"},
        ("chromium-",),
        {"chromium"},
    ),
    "zoom": (
        {"zoom", "us.zoom.zoom"},
        (),
        {"zoom"},
    ),
    "firefox": (
        {"firefox", "org.mozilla.firefox"},
        (),
        {"firefox"},
    ),
}


@dataclass(frozen=True)
class AppEntry:
    desktop_id: str
    name: str
    exec_line: str
    icon: str
    desktop_path: str
    flatpak: bool
    snap: bool

    @property
    def sandboxed(self) -> bool:
        return self.flatpak or self.snap

    @property
    def badge(self) -> str:
        if self.flatpak:
            return "Flatpak"
        if self.snap:
            return "Snap"
        return "Native"


def _parse_desktop(path: Path) -> Optional[AppEntry]:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    section = ""
    values = {}
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            continue
        if section != "Desktop Entry" or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values.setdefault(key.strip(), value.strip())
    if values.get("Type", "Application") != "Application":
        return None
    if values.get("Hidden", "").lower() == "true":
        return None
    exec_line = values.get("Exec", "")
    if not exec_line:
        return None
    name = values.get("Name", path.stem)
    desktop_id = path.name
    flatpak = bool(values.get("X-Flatpak")) or "flatpak run" in exec_line
    snap = exec_line.startswith("snap run ") or "/snap/" in exec_line
    try_exec = values.get("TryExec", "")
    if try_exec and not shutil.which(try_exec) and not Path(try_exec).exists():
        return None
    return AppEntry(
        desktop_id=desktop_id,
        name=name,
        exec_line=exec_line,
        icon=values.get("Icon", ""),
        desktop_path=str(path),
        flatpak=flatpak,
        snap=snap,
    )


def _matches_known_app(entry: AppEntry) -> bool:
    stem = entry.desktop_id.lower()
    if stem.endswith(".desktop"):
        stem = stem[:-8]
    name = entry.name.lower()
    for ids, prefixes, names in KNOWN_APPS.values():
        if stem in ids or name in names:
            return True
        if any(stem.startswith(prefix) for prefix in prefixes):
            return True
    return False


def find_camera_apps() -> List[AppEntry]:
    """Return installed apps that commonly use a camera, best match first."""
    seen = set()
    apps: List[AppEntry] = []
    for directory in application_dirs():
        if not directory.is_dir():
            continue
        for path in sorted(directory.glob("*.desktop")):
            entry = _parse_desktop(path)
            if entry is None or not _matches_known_app(entry):
                continue
            key = entry.desktop_id.lower()
            if key in seen:
                continue
            seen.add(key)
            apps.append(entry)
    apps.sort(key=lambda app: (app.sandboxed, app.name.lower()))
    return apps


def find_preload_library(profile, extra_dirs: Sequence[str] = ()) -> Optional[str]:
    """Locate the card-specific LD_PRELOAD virtual-camera library."""
    library = getattr(profile, "preload_library", "")
    if not library:
        return None
    here = Path(__file__).resolve().parent
    # Prefer a build-tree library when running from source; installed trees
    # only have the library in the system library directory.
    candidates = [
        here.parent / "opengc573-userd" / library,
        here.parent / "opengc570d-userd" / library,
        Path("/usr/local/lib") / library,
        Path("/usr/lib") / library,
        Path("/usr/lib64") / library,
        Path.home() / ".local/lib" / library,
    ]
    candidates.extend(Path(directory) / library for directory in extra_dirs)
    for path in candidates:
        if path.is_file():
            return str(path)
    return None


def launcher_path(app: AppEntry) -> Path:
    return Path.home() / ".local/share/applications" / (
        f"{app.desktop_id}-everlive.desktop")


def launcher_exists(app: AppEntry) -> bool:
    return launcher_path(app).is_file()


def _desktop_escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace("\n", "\\n")


def create_launcher(app: AppEntry, library: str) -> Tuple[bool, str]:
    if app.sandboxed:
        return False, (
            f"{app.name} is a {app.badge} app. LD_PRELOAD cannot cross its "
            "sandbox; install EverLive inside the sandbox or use the "
            "v4l2loopback node instead.")
    if not Path(library).is_file():
        return False, f"Preload library not found: {library}"
    path = launcher_path(app)
    path.parent.mkdir(parents=True, exist_ok=True)
    exec_line = f"env LD_PRELOAD={shlex.quote(library)} {app.exec_line}"
    content = (
        "[Desktop Entry]\n"
        "Type=Application\n"
        f"Name={_desktop_escape(app.name)} (EverLive Camera)\n"
        f"Comment=Launch {_desktop_escape(app.name)} with the EverLive "
        "virtual camera\n"
        f"Exec={_desktop_escape(exec_line)}\n"
        f"Icon={_desktop_escape(app.icon)}\n"
        "Terminal=false\n"
        "Categories=AudioVideo;Video;\n"
        "X-EverLive-Preload=true\n"
        f"X-EverLive-Base={_desktop_escape(app.desktop_path)}\n"
    )
    try:
        path.write_text(content, encoding="utf-8")
    except OSError as exc:
        return False, f"Could not write {path}: {exc}"
    _refresh_desktop_database()
    return True, f"Created {path.name} in the application menu."


def remove_launcher(app: AppEntry) -> Tuple[bool, str]:
    path = launcher_path(app)
    if not path.is_file():
        return False, "No EverLive shortcut exists for this app."
    try:
        if "X-EverLive-Preload=true" not in path.read_text(
                encoding="utf-8", errors="replace"):
            return False, "That shortcut was not created by EverLive."
    except OSError:
        return False, "Could not read the existing shortcut."
    try:
        path.unlink()
    except OSError as exc:
        return False, f"Could not remove {path}: {exc}"
    _refresh_desktop_database()
    return True, f"Removed {path.name}."


def _refresh_desktop_database() -> None:
    updater = shutil.which("update-desktop-database")
    if not updater:
        return
    try:
        subprocess.run(
            [updater, str(Path.home() / ".local/share/applications")],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            timeout=3.0)
    except (OSError, subprocess.SubprocessError):
        pass


def launch_command(app: AppEntry, library: str) -> str:
    return f"env LD_PRELOAD={shlex.quote(library)} {app.exec_line}"


def test_camera(library: str, node: str, timeout: float = 5.0) -> Tuple[bool, str]:
    """Open @node through the preload shim and return diagnostic output."""
    env = dict(os.environ)
    env["LD_PRELOAD"] = library
    if shutil.which("v4l2-ctl"):
        cmd = ["v4l2-ctl", "-d", node, "--all"]
    elif shutil.which("ffmpeg"):
        cmd = ["ffmpeg", "-hide_banner", "-f", "v4l2", "-i", node,
               "-frames:v", "1", "-f", "null", "-"]
    else:
        return False, "Install v4l2-ctl or ffmpeg to test the camera."
    try:
        out = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout, env=env)
    except (OSError, subprocess.SubprocessError) as exc:
        return False, str(exc)
    text = ((out.stdout or "") + (out.stderr or "")).strip()
    return out.returncode == 0, text or "Camera opened successfully."
