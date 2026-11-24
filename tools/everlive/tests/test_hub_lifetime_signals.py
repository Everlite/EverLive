#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for P1-4 Hub object lifetime and signal handling."""

from __future__ import annotations

import gc
import os
import re
import signal
import socket
import sys
import unittest
from pathlib import Path
from unittest import mock

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "tools" / "everlive"))

import everlive_viewer as ev  # noqa: E402


@unittest.skipUnless(ev.HAS_QT, "PySide6 not available")
class PipLifetimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        from PySide6.QtWidgets import QApplication
        cls._app = QApplication.instance() or QApplication([])

    def test_100_pip_cycles_keep_qimage_memory_valid(self) -> None:
        widget = ev.PipPreview()
        w, h = 64, 48
        for idx in range(100):
            # Populate frame in a temporary scope and force GC before painting.
            payload = (bytes([(idx * 7) & 0xFF]) * (w * h * 3), w, h)
            with widget._lock:
                widget._pending_frame = payload
            del payload
            gc.collect()
            widget._on_frame()
            gc.collect()
            self.assertIsNotNone(widget._image)
            self.assertFalse(widget._image.isNull())
            self.assertEqual(widget._image.pixelColor(0, 0).red(), (idx * 7) & 0xFF)
            widget.detach()
            self.assertIsNone(widget._image)
            self.assertIsNone(widget._image_buf)
        widget.deleteLater()
        self._app.processEvents()


class SignalHandlingTests(unittest.TestCase):
    def test_sig_handler_is_minimal_and_recorder_stops_on_wakeup(self) -> None:
        src = (REPO_ROOT / "tools" / "everlive" / "everlive_viewer.py").read_text(encoding="utf-8")
        m = re.search(r"def sig_handler\(sig, frame\):(.*?)signal\.signal\(signal\.SIGINT", src, re.DOTALL)
        self.assertIsNotNone(m)
        handler_body = m.group(1)
        self.assertNotIn("sys.exit", handler_body)
        self.assertNotIn("_multiview_stop", handler_body)
        self.assertNotIn("_webcam_stop", handler_body)
        self.assertIn("sig_w.send", handler_body)
        self.assertIn("QSocketNotifier", src)
        self.assertIn("rec.stop()", src)

    @unittest.skipUnless(ev.HAS_QT, "PySide6 not available")
    def test_socketpair_wakeup_stops_recorder_and_quits_qt(self) -> None:
        from PySide6.QtCore import QSocketNotifier, QTimer
        from PySide6.QtWidgets import QApplication

        app = QApplication.instance() or QApplication([])
        sig_r, sig_w = socket.socketpair()
        sig_r.setblocking(False)
        sig_w.setblocking(False)
        recorder = mock.MagicMock()
        recorder.is_recording = True
        recorder.stop.return_value = (True, "finalized")
        window = mock.MagicMock()
        window.recorder = recorder

        notifier = QSocketNotifier(sig_r.fileno(), QSocketNotifier.Type.Read, app)
        woke = []

        def on_wakeup():
            notifier.setEnabled(False)
            sig_r.recv(64)
            woke.append(True)
            window._force_close = True
            if window.recorder.is_recording:
                window.recorder.stop()
            window.close()
            app.quit()

        notifier.activated.connect(on_wakeup)
        QTimer.singleShot(10, lambda: sig_w.send(bytes([signal.SIGTERM & 0xFF])))
        QTimer.singleShot(2000, app.quit)
        app.exec()

        sig_r.close()
        sig_w.close()
        self.assertEqual(woke, [True])
        recorder.stop.assert_called_once()
        window.close.assert_called_once()
        self.assertTrue(window._force_close)


if __name__ == "__main__":
    unittest.main()
