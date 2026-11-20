#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Keep the engine OSD palette in sync with the Console Hub theme."""

from __future__ import annotations

import os
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import fa_icons  # noqa: E402
import ui_theme  # noqa: E402

HEADER = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "libeverlive-core", "osd", "everlive_osd.h")


def _defines() -> dict[str, int]:
    values: dict[str, int] = {}
    with open(HEADER, encoding="utf-8") as fh:
        for line in fh:
            match = re.match(r"#define\s+EVERLIVE_OSD_(\w+)\s+0x([0-9A-Fa-f]+)", line)
            if match:
                values[match.group(1)] = int(match.group(2), 16)
    return values


class OsdThemeTests(unittest.TestCase):
    def test_osd_icons_use_bundled_font_awesome(self):
        values = _defines()
        available = {glyph for _style, glyph in fa_icons.ICONS.values()}
        for name, value in values.items():
            if name.startswith("ICON_"):
                self.assertIn(chr(value), available, name)

    def test_shared_colors_match_gui(self):
        values = _defines()
        expected = {
            "CARD_BG": ui_theme.OSD_CARD_BG,
            "ROUTE": ui_theme.OSD_ROUTE,
            "INPUT_BG": ui_theme.OSD_INPUT_BG,
            "DIVIDER": ui_theme.OSD_DIVIDER,
            "LIVE": ui_theme.OSD_LIVE,
            "BORDER": ui_theme.GROUP_BORDER,
            "TITLE": ui_theme.TEXT,
            "CHIP_BG": ui_theme.SURFACE_RAISED,
            "HOVER_BORDER": ui_theme.BORDER_STRONG,
            "CHIP_BORDER": ui_theme.CHIP_BORDER,
            "TOOLTIP_BG": ui_theme.SURFACE_SUNKEN,
            "DANGER_BG": ui_theme.DANGER_BG,
            "DANGER_BORDER": ui_theme.DANGER_BORDER,
            "BLUE": ui_theme.BLUE,
            "BLUE_BORDER": ui_theme.BLUE_HOVER,
            "BLUE_TEXT": ui_theme.BLUE_TEXT,
        }
        for name, gui_value in expected.items():
            self.assertIn(name, values, name)
            self.assertEqual(values[name], int(gui_value.lstrip("#"), 16), name)


if __name__ == "__main__":
    unittest.main()
