#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the EverLive control-socket client command surface."""

from __future__ import annotations

import os
import sys
import unittest
from unittest import mock

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import everlive_client  # noqa: E402


@unittest.skipUnless(everlive_client.QSocketNotifier is not None,
                     "PySide6 is not installed")
class ClientCommandTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from PySide6.QtWidgets import QApplication
        cls.app = QApplication.instance() or QApplication([])

    def test_set_osd_mode_sends_normalized_mode(self):
        client = everlive_client.EverLiveIpcClient()
        with mock.patch.object(client, "request", return_value=True) as request:
            self.assertTrue(client.set_osd_mode("CARD"))
            request.assert_called_once_with("osd.mode.set", {"mode": "card"})

    def test_set_osd_mode_preserves_auto_and_compact(self):
        client = everlive_client.EverLiveIpcClient()
        with mock.patch.object(client, "request", return_value=True) as request:
            client.set_osd_mode("auto")
            client.set_osd_mode("compact")
            self.assertEqual(
                request.call_args_list,
                [mock.call("osd.mode.set", {"mode": "auto"}),
                 mock.call("osd.mode.set", {"mode": "compact"})])


if __name__ == "__main__":
    unittest.main()
