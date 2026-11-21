#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the bundled Font Awesome icon helpers."""

import os
import re
import sys
import unittest
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import fa_icons  # noqa: E402

VIEWER = Path(__file__).resolve().parents[1] / "everlive_viewer.py"


class MappingTests(unittest.TestCase):
    def test_bundled_fonts_exist(self):
        for filename in ("fa-solid-900.ttf", "fa-brands-400.ttf",
                         "LICENSE.txt"):
            path = os.path.join(fa_icons.FONT_DIR, filename)
            self.assertTrue(os.path.isfile(path), filename)

    def test_known_codepoints(self):
        self.assertEqual(fa_icons.glyph("play"), "\uf04b")
        self.assertEqual(fa_icons.glyph("discord"), "\uf392")
        self.assertEqual(fa_icons.glyph("github"), "\uf09b")
        self.assertEqual(fa_icons.glyph("help"), "\uf059")

    def test_unknown_icon_has_fallback(self):
        self.assertEqual(fa_icons.glyph("does-not-exist"), "?")
        self.assertEqual(fa_icons.fallback_text("does-not-exist"), "?")

    def test_every_icon_has_a_fallback(self):
        for name in fa_icons.ICONS:
            self.assertTrue(fa_icons.fallback_text(name), name)

    def test_viewer_icon_names_exist(self):
        text = VIEWER.read_text(encoding="utf-8")
        used = set(re.findall(
            r'(?:IconButton\(|set_icon\(|set_icon_name\()\s*"([^"]+)"',
            text))
        missing = sorted(used - set(fa_icons.ICONS))
        self.assertEqual(missing, [])


@unittest.skipUnless(fa_icons.HAS_QT, "PySide6 is not installed")
class RenderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from PySide6.QtWidgets import QApplication
        cls.app = QApplication.instance() or QApplication([])
        fa_icons.load_fonts()

    def test_fonts_load(self):
        self.assertTrue(fa_icons.available())

    def test_icons_render(self):
        for name in ("play", "stop", "discord", "github", "help",
                     "volume", "mute", "expand", "external",
                     "maximize", "minimize", "chart-bar", "ellipsis",
                     "xmark", "keyboard"):
            icon = fa_icons.icon(name, "#E7EBF2", 18)
            self.assertFalse(icon.isNull(), name)
            self.assertFalse(icon.pixmap(18, 18).isNull(), name)

    def test_checked_state_uses_active_colour(self):
        from PySide6.QtGui import QIcon
        icon = fa_icons.icon(
            "camera", "#9AA0B0", 24, active_color="#FFFFFF")
        off = icon.pixmap(24, 24, QIcon.Normal, QIcon.Off).toImage()
        on = icon.pixmap(24, 24, QIcon.Normal, QIcon.On).toImage()
        self.assertNotEqual(off, on)

    def test_brand_and_solid_families(self):
        self.assertEqual(fa_icons._FAMILIES.get("solid"),
                         "Font Awesome 6 Free")
        self.assertEqual(fa_icons._FAMILIES.get("brands"),
                         "Font Awesome 6 Brands")


if __name__ == "__main__":
    unittest.main()
