#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Engine binary resolution for checkout and system installs."""

from __future__ import annotations

import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

try:
    import everlive_viewer as ev  # noqa: E402
except ImportError as exc:  # pragma: no cover - Qt missing on the runner
    ev = None
    _IMPORT_ERROR = str(exc)


@unittest.skipIf(ev is None, "everlive_viewer not importable")
class EnginePathTests(unittest.TestCase):
    def test_prefers_executable_in_app_tree(self) -> None:
        local = "/app/tools/opengc573-userd/gc573_user_streamer"
        with mock.patch.object(ev, "userd_dir",
                              return_value="/app/tools/opengc573-userd"), \
                mock.patch.object(ev.os.path, "isfile", return_value=True), \
                mock.patch.object(ev.os, "access", return_value=True), \
                mock.patch.object(ev.shutil, "which",
                                  return_value="/usr/local/bin/gc573_user_streamer"):
            self.assertEqual(ev.userd_bin("gc573_user_streamer"), local)

    def test_falls_back_to_system_path(self) -> None:
        with mock.patch.object(ev, "userd_dir",
                              return_value="/app/tools/opengc573-userd"), \
                mock.patch.object(ev.os.path, "isfile", return_value=False), \
                mock.patch.object(ev.shutil, "which",
                                  return_value="/usr/local/bin/gc573_user_streamer"):
            self.assertEqual(ev.userd_bin("gc573_user_streamer"),
                             "/usr/local/bin/gc573_user_streamer")

    def test_returns_app_tree_path_when_missing_everywhere(self) -> None:
        local = os.path.join("/app/tools/opengc570d-userd", "opengc570d-userd")
        with mock.patch.object(ev, "userd_dir",
                              return_value="/app/tools/opengc570d-userd"), \
                mock.patch.object(ev.os.path, "isfile", return_value=False), \
                mock.patch.object(ev.shutil, "which", return_value=None):
            self.assertEqual(ev.userd_bin("opengc570d-userd", ev.MODEL_GC570D),
                             local)


if __name__ == "__main__":
    unittest.main()
