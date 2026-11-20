#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the Hub -> engine OSD mode handshake."""

from __future__ import annotations

import os
import sys
import unittest
from types import SimpleNamespace

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import everlive_viewer  # noqa: E402


@unittest.skipUnless(everlive_viewer.HAS_QT, "PySide6 is not installed")
class OsdModeTests(unittest.TestCase):
    class FakeIpc:
        connected = True

        def __init__(self):
            self.calls = []
            self.record_calls = []

        def set_osd_mode(self, mode):
            self.calls.append(mode)
            return True

        def set_recording(self, active):
            self.record_calls.append(active)
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

    def test_embedded_window_requests_card(self):
        window = self._window(fullscreen=False)
        everlive_viewer.MainWindow._sync_osd_mode(window)
        self.assertEqual(window._ipc.calls, ["card"])

    def test_cinema_fullscreen_requests_card(self):
        window = self._window(fullscreen=True)
        everlive_viewer.MainWindow._sync_osd_mode(window)
        self.assertEqual(window._ipc.calls, ["card"])

    def test_popout_window_requests_card(self):
        window = self._window(fullscreen=False, popout=True)
        everlive_viewer.MainWindow._sync_osd_mode(window)
        self.assertEqual(window._ipc.calls, ["card"])

    def test_record_state_follows_recorder_on_attach(self):
        window = self._window()
        window.recorder = SimpleNamespace(is_recording=True)
        everlive_viewer.MainWindow._sync_osd_mode(window)
        self.assertEqual(window._ipc.record_calls, [True])
        self.assertTrue(window._osd_recording_state)

    def test_toggle_record_hub_action_is_queued(self):
        window = SimpleNamespace(
            _vfio_saw_output=False,
            _pending_hub_actions=[],
        )
        everlive_viewer.GameStreamer._parse_streamer_line(
            window, "[HUB] TOGGLE_RECORD")
        self.assertEqual(window._pending_hub_actions, ["toggle_record"])

    def test_no_ipc_connection_is_a_no_op(self):
        window = self._window(fullscreen=True)
        window._ipc.connected = False
        everlive_viewer.MainWindow._sync_osd_mode(window)
        self.assertEqual(window._ipc.calls, [])


if __name__ == "__main__":
    unittest.main()
