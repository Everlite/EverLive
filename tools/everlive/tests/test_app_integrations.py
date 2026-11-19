#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for per-application virtual-camera launchers."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import app_integrations as ai  # noqa: E402


def _write_desktop(directory: Path, name: str, text: str) -> Path:
    path = directory / name
    path.write_text(text, encoding="utf-8")
    return path


class ParsingTests(unittest.TestCase):
    def test_parse_native_app(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = _write_desktop(Path(tmp), "com.obsproject.Studio.desktop", """
[Desktop Entry]
Type=Application
Name=OBS Studio
Exec=obs %F
Icon=obs
TryExec=/bin/sh
""")
            entry = ai._parse_desktop(path)
            self.assertIsNotNone(entry)
            assert entry is not None
            self.assertEqual(entry.name, "OBS Studio")
            self.assertFalse(entry.sandboxed)
            self.assertTrue(ai._matches_known_app(entry))

    def test_parse_flatpak_app(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = _write_desktop(Path(tmp), "com.discordapp.Discord.desktop", """
[Desktop Entry]
Type=Application
Name=Discord
Exec=/usr/bin/flatpak run --branch=stable com.discordapp.Discord %U
Icon=com.discordapp.Discord
X-Flatpak=com.discordapp.Discord
""")
            entry = ai._parse_desktop(path)
            self.assertIsNotNone(entry)
            assert entry is not None
            self.assertTrue(entry.flatpak)
            self.assertTrue(entry.sandboxed)

    def test_ignores_discord_game_entries(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = _write_desktop(Path(tmp), "discord-123.desktop", """
[Desktop Entry]
Type=Application
Name=Game 123
Exec=/usr/bin/game %u
""")
            entry = ai._parse_desktop(path)
            self.assertIsNotNone(entry)
            assert entry is not None
            self.assertFalse(ai._matches_known_app(entry))


class LauncherTests(unittest.TestCase):
    def test_create_and_remove(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp)
            apps_dir = home / ".local/share/applications"
            apps_dir.mkdir(parents=True)
            library = home / "libgc570d-v4l2.so"
            library.write_bytes(b"\x7fELF")
            with mock.patch.object(Path, "home", return_value=home):
                app = ai.AppEntry(
                    desktop_id="discord.desktop",
                    name="Discord",
                    exec_line="/usr/bin/discord %U",
                    icon="discord",
                    desktop_path=str(apps_dir / "discord.desktop"),
                    flatpak=False,
                    snap=False,
                )
                ok, message = ai.create_launcher(app, str(library))
                self.assertTrue(ok, message)
                path = ai.launcher_path(app)
                self.assertTrue(path.is_file())
                text = path.read_text(encoding="utf-8")
                self.assertIn("X-EverLive-Preload=true", text)
                self.assertIn(f"LD_PRELOAD={library}", text)
                ok, message = ai.remove_launcher(app)
                self.assertTrue(ok, message)
                self.assertFalse(path.exists())

    def test_flatpak_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp)
            library = home / "libgc570d-v4l2.so"
            library.write_bytes(b"\x7fELF")
            app = ai.AppEntry(
                desktop_id="com.discordapp.Discord.desktop",
                name="Discord",
                exec_line="/usr/bin/flatpak run com.discordapp.Discord",
                icon="discord",
                desktop_path="",
                flatpak=True,
                snap=False,
            )
            with mock.patch.object(Path, "home", return_value=home):
                ok, message = ai.create_launcher(app, str(library))
            self.assertFalse(ok)
            self.assertIn("sandbox", message.lower())

    def test_preload_library_lookup(self) -> None:
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as tmp:
            profile = SimpleNamespace(
                preload_library="libeverlive-test-camera.so")
            library = Path(tmp) / profile.preload_library
            library.write_bytes(b"\x7fELF")
            found = ai.find_preload_library(profile, [tmp])
            self.assertEqual(found, str(library))


if __name__ == "__main__":
    unittest.main()
