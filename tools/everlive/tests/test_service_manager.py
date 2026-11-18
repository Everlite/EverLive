#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for the background capture service manager."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import service_manager as sm  # noqa: E402


def _result(code: int = 0, stdout: str = "", stderr: str = ""):
    return SimpleNamespace(returncode=code, stdout=stdout, stderr=stderr)


class UnitTests(unittest.TestCase):
    def test_render_unit(self) -> None:
        text = sm.render_unit("/usr/local/bin/everlive-service")
        self.assertIn("ExecStart=/usr/local/bin/everlive-service", text)
        self.assertIn("WantedBy=default.target", text)
        self.assertIn("Restart=on-failure", text)

    def test_write_service_args(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            config = Path(tmp)
            with mock.patch.object(sm, "CONFIG_DIR", config), \
                    mock.patch.object(sm, "SERVICE_ENV",
                                      config / "service.env"):
                args = sm.write_service_args(
                    "/dev/video10", audio_tap=True)
                self.assertIn("--v4l2 /dev/video10", args)
                self.assertIn("--audio-tap everlive-capture-audio", args)
                self.assertEqual(
                    sm.read_service_args(), args)

    def test_status_reports_active_and_enabled(self) -> None:
        def fake_run(cmd, timeout=4.0):
            if "show-environment" in cmd:
                return _result()
            if "show" in cmd and "FragmentPath" in cmd:
                return _result(stdout="FragmentPath=/usr/lib/systemd/user/everlive.service\n")
            if "is-enabled" in cmd:
                return _result()
            if "is-active" in cmd:
                return _result()
            return _result(1)

        with mock.patch.object(sm, "_run", side_effect=fake_run), \
                mock.patch.object(sm, "find_service_script",
                                  return_value="/usr/bin/everlive-service"):
            status = sm.status()
        self.assertTrue(status.installed)
        self.assertTrue(status.enabled)
        self.assertTrue(status.active)
        self.assertEqual(status.label, "Background service active")
        self.assertEqual(status.tone, "live")

    def test_install_writes_user_unit(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            user_dir = Path(tmp) / "systemd/user"
            user_unit = user_dir / sm.UNIT_NAME
            with mock.patch.object(sm, "USER_UNIT_DIR", user_dir), \
                    mock.patch.object(sm, "find_service_script",
                                      return_value="/usr/bin/everlive-service"), \
                    mock.patch.object(sm, "systemd_available",
                                      return_value=True), \
                    mock.patch.object(sm, "unit_fragment_path",
                                      return_value=""), \
                    mock.patch.object(sm, "_run",
                                      return_value=_result()):
                # unit_fragment_path is patched, but _user_unit_path uses the
                # patched USER_UNIT_DIR.
                ok, message = sm.install_unit()
                self.assertTrue(ok, message)
                self.assertTrue(user_unit.is_file())
                self.assertIn("everlive-service", user_unit.read_text())

    def test_uninstall_refuses_package_unit(self) -> None:
        with mock.patch.object(sm, "unit_fragment_path",
                               return_value="/usr/lib/systemd/user/everlive.service"):
            ok, message = sm.uninstall_unit()
        self.assertFalse(ok)
        self.assertIn("package", message.lower())


if __name__ == "__main__":
    unittest.main()
