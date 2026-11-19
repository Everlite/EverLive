#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the Hub -> engine fullscreen OSD mode handshake."""

from __future__ import annotations

import os
import sys
import unittest
from types import SimpleNamespace

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import everlive_viewer  # noqa: E402


@unittest.skipUnless(everlive_viewer.HAS_QT, "PySide6 is not installed")
class CinemaOsdModeTests(unittest.TestCase):
    class FakeIpc:
        connected = True

        def __init__(self):
            self.calls = []

        def set_osd_mode(self, mode):
            self.calls.append(mode)
            return True

    class FakeCheck:
        def __init__(self, checked=False):
            self.checked = checked

        def isChecked(self):  # noqa: N802 (Qt casing)
            return self.checked

    def _window(self, fullscreen=False, popout=False):
        return SimpleNamespace(
            _ipc=self.FakeIpc(),
            _cinema_fullscreen=fullscreen,
            chk_popout=self.FakeCheck(popout),
        )

    def test_cinema_fullscreen_requests_card(self):
        window = self._window(fullscreen=True)
        everlive_viewer.MainWindow._sync_osd_mode(window)
        self.assertEqual(window._ipc.calls, ["card"])

    def test_leaving_cinema_restores_auto(self):
        window = self._window(fullscreen=False)
        everlive_viewer.MainWindow._sync_osd_mode(window)
        self.assertEqual(window._ipc.calls, ["auto"])

    def test_popout_keeps_engine_auto_detection(self):
        window = self._window(fullscreen=True, popout=True)
        everlive_viewer.MainWindow._sync_osd_mode(window)
        self.assertEqual(window._ipc.calls, ["auto"])

    def test_no_ipc_connection_is_a_no_op(self):
        window = self._window(fullscreen=True)
        window._ipc.connected = False
        everlive_viewer.MainWindow._sync_osd_mode(window)
        self.assertEqual(window._ipc.calls, [])


if __name__ == "__main__":
    unittest.main()
