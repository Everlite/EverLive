#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the EverLive control-socket client command surface."""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
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

    def test_set_recording_mirrors_engine_state(self):
        client = everlive_client.EverLiveIpcClient()
        with mock.patch.object(client, "request", return_value=True) as request:
            self.assertTrue(client.set_recording(True))
            client.set_recording(False)
            self.assertEqual(
                request.call_args_list,
                [mock.call("record.state", {"active": True}),
                 mock.call("record.state", {"active": False})])

    def test_set_osd_mode_preserves_auto_and_compact(self):
        client = everlive_client.EverLiveIpcClient()
        with mock.patch.object(client, "request", return_value=True) as request:
            client.set_osd_mode("auto")
            client.set_osd_mode("compact")
            self.assertEqual(
                request.call_args_list,
                [mock.call("osd.mode.set", {"mode": "auto"}),
                 mock.call("osd.mode.set", {"mode": "compact"})])


class RuntimeDirectoryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="everlive-client-")
        os.chmod(self.tmp, 0o700)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_private_xdg_runtime_dir(self):
        with mock.patch.dict(os.environ,
                             {"XDG_RUNTIME_DIR": self.tmp}, clear=False):
            self.assertEqual(everlive_client.runtime_dir(), self.tmp)
            self.assertEqual(
                everlive_client.socket_path(),
                os.path.join(self.tmp, everlive_client.SOCKET_NAME))

    def test_permissive_xdg_runtime_dir_rejected(self):
        os.chmod(self.tmp, 0o755)
        with mock.patch.dict(os.environ,
                             {"XDG_RUNTIME_DIR": self.tmp}, clear=False):
            with self.assertRaises(PermissionError):
                everlive_client.runtime_dir()
            with self.assertRaises(PermissionError):
                everlive_client.socket_path()

    def test_symlinked_xdg_runtime_dir_rejected(self):
        link = os.path.join(tempfile.mkdtemp(prefix="everlive-link-"),
                            "runtime")
        try:
            os.symlink(self.tmp, link)
            with mock.patch.dict(os.environ,
                                 {"XDG_RUNTIME_DIR": link}, clear=False):
                with self.assertRaises(PermissionError):
                    everlive_client.runtime_dir()
        finally:
            os.unlink(link)
            os.rmdir(os.path.dirname(link))


if __name__ == "__main__":
    unittest.main()
