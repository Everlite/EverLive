#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""OSD bar position migration and choices."""

from __future__ import annotations

import os
import sys
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import everlive_viewer  # noqa: E402


class OsdPositionTests(unittest.TestCase):
    def test_new_values_are_preserved(self):
        self.assertEqual(
            everlive_viewer.normalize_osd_position("top"), "top")
        self.assertEqual(
            everlive_viewer.normalize_osd_position("bottom"), "bottom")

    def test_legacy_corners_map_to_edges(self):
        for value in ("tr", "tl", "top-right", "top-left"):
            self.assertEqual(
                everlive_viewer.normalize_osd_position(value), "top")
        for value in ("br", "bl", "bottom-right", "bottom-left"):
            self.assertEqual(
                everlive_viewer.normalize_osd_position(value), "bottom")

    def test_unknown_values_default_to_top(self):
        self.assertEqual(
            everlive_viewer.normalize_osd_position("nowhere"), "top")
        self.assertEqual(
            everlive_viewer.normalize_osd_position(None), "top")

    def test_hub_offers_top_and_bottom_only(self):
        self.assertEqual(everlive_viewer.OSD_POSITIONS, ("top", "bottom"))
        self.assertEqual(
            everlive_viewer.OSD_POSITION_OPTIONS,
            [("Top", "top"), ("Bottom", "bottom")])
        self.assertEqual(
            everlive_viewer.DEFAULT_SETTINGS["osd_position"], "top")


if __name__ == "__main__":
    unittest.main()
