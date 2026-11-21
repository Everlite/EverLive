#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Keep the engine OSD theme in sync with the Console Hub design system."""

from __future__ import annotations

import os
import re
import sys
import unittest

EVERLIVE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, EVERLIVE_DIR)

import fa_icons  # noqa: E402
import gen_osd_theme  # noqa: E402
import ui_theme  # noqa: E402

OSD_DIR = os.path.join(
    os.path.dirname(EVERLIVE_DIR), "libeverlive-core", "osd")
HEADER = os.path.join(OSD_DIR, "everlive_osd.h")
THEME = os.path.join(OSD_DIR, "everlive_osd_theme.h")


def _defines(path: str) -> dict[str, int]:
    values: dict[str, int] = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            match = re.match(
                r"#define\s+EVERLIVE_OSD_(\w+)\s+0x([0-9A-Fa-f]+)", line)
            if match:
                values[match.group(1)] = int(match.group(2), 16)
    return values


class OsdThemeTests(unittest.TestCase):
    def test_osd_icons_use_bundled_font_awesome(self):
        values = _defines(HEADER)
        available = {glyph for _style, glyph in fa_icons.ICONS.values()}
        for name, value in values.items():
            if name.startswith("ICON_"):
                self.assertIn(chr(value), available, name)

    def test_stats_panel_icons(self):
        values = _defines(HEADER)
        self.assertEqual(values["ICON_CHART"], 0xF080)
        self.assertEqual(values["ICON_XMARK"], 0xF00D)

    def test_pip_uses_window_restore_and_fullscreen_uses_maximize(self):
        values = _defines(HEADER)
        self.assertEqual(values["ICON_PIP"], 0xF2D2)
        self.assertEqual(values["ICON_MAXIMIZE"], 0xF31E)
        self.assertEqual(values["ICON_MINIMIZE"], 0xF78C)

    def test_stats_button_uses_blue_checked_state(self):
        source = os.path.join(OSD_DIR, "everlive_osd.c")
        with open(source, encoding="utf-8") as fh:
            text = fh.read()
        start = text.index("DRAW_ICON_BUTTON(EVERLIVE_OSD_ACTION_STATS")
        block = text[start:text.index(");", start) + 2]
        self.assertIn("EVERLIVE_OSD_BLUE", block)
        self.assertNotIn("EVERLIVE_OSD_MINT", block)

    def test_generated_theme_header_is_current(self):
        with open(THEME, encoding="utf-8") as fh:
            current = fh.read()
        self.assertEqual(
            gen_osd_theme.build_header(), current,
            "run python3 tools/everlive/gen_osd_theme.py")

    def test_hub_scale_lands_on_hub_pixels_at_720p(self):
        self.assertEqual(gen_osd_theme.HUB_SCALE, 1.0)

    def test_shared_colors_match_gui(self):
        values = _defines(THEME)
        expected = {
            "CARD_BG": ui_theme.SURFACE_RAISED,
            "CARD_BORDER": ui_theme.BORDER,
            "GROUP_BORDER": ui_theme.GROUP_BORDER,
            "TITLE": ui_theme.TEXT,
            "TEXT_MUTED": ui_theme.TEXT_MUTED,
            "TEXT_DIM": ui_theme.TEXT_DIM,
            "INPUT_BG": ui_theme.INPUT_BG,
            "INPUT_BG_HOVER": ui_theme.INPUT_HOVER,
            "INPUT_BORDER": ui_theme.INPUT_BORDER,
            "SEGMENT_BG": ui_theme.SURFACE_SUNKEN,
            "CHIP_BG": ui_theme.SURFACE_RAISED,
            "CHIP_BORDER": ui_theme.CHIP_BORDER,
            "HOVER_BG": ui_theme.SURFACE_HOVER,
            "HOVER_BORDER": ui_theme.BORDER_STRONG,
            "TOOLTIP_BG": ui_theme.SURFACE_RAISED,
            "TOOLTIP_BORDER": ui_theme.GROUP_BORDER,
            "KBD_BG": ui_theme.KBD_BG,
            "DIVIDER": ui_theme.GROUP_BORDER,
            "TRACK": ui_theme.SURFACE_HOVER,
            "KNOB": ui_theme.KNOB,
            "LIVE": ui_theme.MINT,
            "LIVE_OFF": ui_theme.TEXT_DIM,
            "MINT_BG": ui_theme.MINT_BG,
            "MINT_BORDER": ui_theme.MINT_BORDER,
            "MINT": ui_theme.MINT,
            "WARN_BG": ui_theme.WARN_BG,
            "WARN_BORDER": ui_theme.WARN_BORDER,
            "WARN": ui_theme.WARN,
            "DANGER_BG": ui_theme.DANGER_BG,
            "DANGER_BORDER": ui_theme.DANGER_BORDER,
            "DANGER": ui_theme.DANGER,
            "BLUE": ui_theme.BLUE,
            "BLUE_BORDER": ui_theme.BLUE_HOVER,
            "BLUE_TEXT": ui_theme.BLUE_TEXT,
            "ON_ACCENT": ui_theme.ON_ACCENT,
        }
        for name, gui_value in expected.items():
            self.assertIn(name, values, name)
            self.assertEqual(values[name], int(gui_value.lstrip("#"), 16),
                             name)


if __name__ == "__main__":
    unittest.main()
