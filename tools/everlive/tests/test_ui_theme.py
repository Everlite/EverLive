#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the Console Hub design tokens."""

import os
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import ui_theme  # noqa: E402


class TokenTests(unittest.TestCase):
    def test_named_colors_are_hex(self):
        for name in ("BG", "SURFACE", "SURFACE_RAISED", "BORDER", "TEXT",
                     "TEXT_MUTED", "ACCENT", "ACCENT_SOFT", "BLUE", "MINT",
                     "WARN", "DANGER"):
            value = getattr(ui_theme, name)
            self.assertRegex(value, r"^#[0-9A-Fa-f]{6}$", name)

    def test_stylesheet_substitutes_every_token(self):
        stylesheet = ui_theme.build_stylesheet()
        self.assertEqual(ui_theme.missing_tokens(stylesheet), [])
        self.assertNotIn("@", stylesheet)
        self.assertNotRegex(stylesheet, r"@[A-Z_]+@")

    def test_stylesheet_keeps_shell_selectors(self):
        stylesheet = ui_theme.build_stylesheet()
        for selector in ("QFrame#headerBar", "QFrame#videoStageFrame",
                         "QFrame#cardFrame", "QFrame#bottomBar",
                         "QTabWidget#inspectorTabs", "QWidget#segmentedControl",
                         "QPushButton#segmentButton", "QPushButton#iconButton",
                         "QFrame#sourceGroup", "QLabel#statusChip"):
            self.assertIn(selector, stylesheet)

    def test_palette_matches_mockup_tokens(self):
        self.assertEqual(ui_theme.BG.lower(), "#12151f")
        self.assertEqual(ui_theme.ACCENT.lower(), "#d93b86")
        self.assertEqual(ui_theme.BLUE.lower(), "#3b5b8c")

    def test_primary_radius_is_uniform(self):
        stylesheet = ui_theme.build_stylesheet()
        for needle in ("QFrame#videoStageFrame",
                       "QFrame#telemetryDeck",
                       "QFrame#cardFrame",
                       "QFrame#sourceGroup",
                       "\nQPushButton {",
                       "\nQComboBox {"):
            start = stylesheet.index(needle)
            block = stylesheet[start:stylesheet.index("}", start)]
            self.assertIn("border-radius: 12px", block, needle)

    def test_no_stray_pixel_radii(self):
        import re
        stylesheet = ui_theme.build_stylesheet()
        values = set(re.findall(r"border-radius:\s*([^;]+);", stylesheet))
        self.assertLessEqual(values, {"0px", "6px", "12px"}, values)

    def test_segment_inner_edges_stay_square(self):
        stylesheet = ui_theme.build_stylesheet()
        base = stylesheet.index("QPushButton#segmentButton {")
        block = stylesheet[base:stylesheet.index("}", base)]
        self.assertIn("border-radius: 0px", block)
        first = stylesheet.index('QPushButton#segmentButton[segmentPos="first"]')
        block = stylesheet[first:stylesheet.index("}", first)]
        self.assertIn("border-top-left-radius: 6px", block)
        self.assertIn("border-bottom-left-radius: 6px", block)
        self.assertIn("border-right: 1px solid", block)

    def test_missing_tokens_reports_typos(self):
        found = ui_theme.missing_tokens("color: @NOT_A_TOKEN@;")
        self.assertEqual(found, ["@NOT_A_TOKEN@"])

    def test_radius_token_is_a_css_length(self):
        self.assertTrue(ui_theme.RADIUS_CSS.endswith("px"))
        self.assertEqual(re.sub(r"\d", "", ui_theme.RADIUS_CSS), "px")


if __name__ == "__main__":
    unittest.main()
