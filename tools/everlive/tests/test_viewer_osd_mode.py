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


@unittest.skipUnless(everlive_viewer.HAS_QT, "PySide6 is not installed")
class HubOsdControlTests(unittest.TestCase):
    class FakeCheck:
        def __init__(self, checked=False):
            self.checked = checked
            self.enabled = True

        def isChecked(self):  # noqa: N802 (Qt casing)
            return self.checked

        def setEnabled(self, enabled):  # noqa: N802 (Qt casing)
            self.enabled = enabled

    class FakeEffect:
        def __init__(self):
            self.opacity = 1.0

        def setOpacity(self, value):  # noqa: N802 (Qt casing)
            self.opacity = value

    def _window(self, *, stats=False, quicknav=False, clean=False):
        return SimpleNamespace(
            chk_osd=self.FakeCheck(stats),
            chk_quicknav=self.FakeCheck(quicknav),
            chk_quicknav_fs=self.FakeCheck(False),
            chk_streamer=self.FakeCheck(clean),
            combo_osd_pos=self.FakeCheck(True),
            osd_pos_effect=self.FakeEffect(),
        )

    def test_position_enabled_for_quicknav_without_stats(self):
        window = self._window(stats=False, quicknav=True)
        everlive_viewer.MainWindow._update_osd_controls(window)
        self.assertTrue(window.combo_osd_pos.enabled)
        self.assertEqual(window.osd_pos_effect.opacity, 1.0)

    def test_position_enabled_for_stats_without_quicknav(self):
        window = self._window(stats=True, quicknav=False)
        everlive_viewer.MainWindow._update_osd_controls(window)
        self.assertTrue(window.combo_osd_pos.enabled)

    def test_position_disabled_when_both_off_or_clean(self):
        window = self._window(stats=False, quicknav=False)
        everlive_viewer.MainWindow._update_osd_controls(window)
        self.assertFalse(window.combo_osd_pos.enabled)
        self.assertEqual(window.osd_pos_effect.opacity, 0.45)

        window = self._window(stats=True, quicknav=True, clean=True)
        everlive_viewer.MainWindow._update_osd_controls(window)
        self.assertFalse(window.combo_osd_pos.enabled)


@unittest.skipUnless(everlive_viewer.HAS_QT, "PySide6 is not installed")
class RecordingUiTests(unittest.TestCase):
    class FakeRecorder:
        def __init__(self, active=True):
            self.is_recording = active
            self.current_plan = SimpleNamespace(fps=60) if active else None

        def format_elapsed(self):
            return "00:01:23"

        def format_size(self):
            return "12.0 MB"

    @classmethod
    def setUpClass(cls):
        from PySide6.QtWidgets import QApplication
        cls.app = QApplication.instance() or QApplication([])

    def _window(self, active=True):
        from PySide6.QtWidgets import QLabel
        from ui_widgets import IconButton, StatusChip
        return SimpleNamespace(
            recorder=self.FakeRecorder(active),
            _osd_recording_state=not active,
            _ipc=None,
            btn_record=IconButton("record", "Record"),
            chip_bar_record=StatusChip("Recording", "danger"),
            lbl_rec_status=QLabel("Standby"),
        )

    def test_recording_chip_is_conditional_and_exact(self):
        window = self._window(active=True)
        everlive_viewer.MainWindow._sync_recording_ui(window)
        self.assertEqual(window.chip_bar_record.text(), "Recording")
        self.assertFalse(window.chip_bar_record.isHidden())
        self.assertTrue(window.btn_record.property("recording"))
        self.assertEqual(window.btn_record.icon_name(), "stop")

        window = self._window(active=False)
        everlive_viewer.MainWindow._sync_recording_ui(window)
        self.assertTrue(window.chip_bar_record.isHidden())
        self.assertFalse(window.btn_record.property("recording"))
        self.assertEqual(window.btn_record.icon_name(), "record")


if __name__ == "__main__":
    unittest.main()
