#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for P0-5 trusted helper checks and input validation."""

from __future__ import annotations

import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "tools" / "everlive"))

import everlive_viewer as ev  # noqa: E402
import service_manager as sm  # noqa: E402

VFIO_BIND = REPO_ROOT / "packaging" / "libexec" / "everlive-vfio-bind"


class TrustedHelperTests(unittest.TestCase):
    def test_accepts_root_owned_0755_usr_binary(self) -> None:
        st = SimpleNamespace(st_mode=stat.S_IFREG | 0o755, st_uid=0)
        with mock.patch("os.path.islink", return_value=False), \
                mock.patch("os.path.realpath", return_value="/usr/local/libexec/vfio_pci_bind.sh"), \
                mock.patch("os.lstat", return_value=st), \
                mock.patch("os.access", return_value=True):
            self.assertTrue(ev._trusted_helper("/usr/local/libexec/vfio_pci_bind.sh"))

    def test_rejects_symlinks_writable_nonroot_and_checkout_paths(self) -> None:
        good_st = SimpleNamespace(st_mode=stat.S_IFREG | 0o755, st_uid=0)

        with mock.patch("os.path.islink", return_value=True):
            self.assertFalse(ev._trusted_helper("/usr/local/libexec/vfio_pci_bind.sh"))

        with mock.patch("os.path.islink", return_value=False), \
                mock.patch("os.path.realpath", return_value="/home/user/checkout/bind.sh"), \
                mock.patch("os.lstat", return_value=good_st), \
                mock.patch("os.access", return_value=True):
            self.assertFalse(ev._trusted_helper("/home/user/checkout/bind.sh"))

        nonroot_st = SimpleNamespace(st_mode=stat.S_IFREG | 0o755, st_uid=1000)
        with mock.patch("os.path.islink", return_value=False), \
                mock.patch("os.path.realpath", return_value="/usr/local/libexec/vfio_pci_bind.sh"), \
                mock.patch("os.lstat", return_value=nonroot_st), \
                mock.patch("os.access", return_value=True):
            self.assertFalse(ev._trusted_helper("/usr/local/libexec/vfio_pci_bind.sh"))

        writable_st = SimpleNamespace(st_mode=stat.S_IFREG | 0o775, st_uid=0)
        with mock.patch("os.path.islink", return_value=False), \
                mock.patch("os.path.realpath", return_value="/usr/local/libexec/vfio_pci_bind.sh"), \
                mock.patch("os.lstat", return_value=writable_st), \
                mock.patch("os.access", return_value=True):
            self.assertFalse(ev._trusted_helper("/usr/local/libexec/vfio_pci_bind.sh"))


class BdfValidationTests(unittest.TestCase):
    def test_rejects_hostile_bdfs_before_sysfs(self) -> None:
        hostile = [
            "../../etc",
            "0000:0a:00.0;id",
            "0000:0a:00.8",
            "0000:0A:00.0",
            "0000:0a:00.0\n0000:0b:00.0",
            "0000:0a:00.0 --force",
        ]
        for bdf in hostile:
            with self.subTest(bdf=bdf):
                self.assertIsNone(ev._VALID_BDF_RE.match(bdf))
                res = subprocess.run(
                    ["sh", str(VFIO_BIND), "--card", "gc573", "--status", "--bdf", bdf],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertNotEqual(res.returncode, 0)
                self.assertIn("invalid PCI BDF", res.stderr)


class V4l2ValidationTests(unittest.TestCase):
    def test_rejects_hostile_v4l2_strings(self) -> None:
        hostile = [
            "/dev/video10;id",
            "/dev/video10 --extra",
            "/dev/null",
            "/tmp/video10",
            "/dev/video1000",
            "../dev/video10",
            "/dev/video10\nFOO=bar",
        ]
        for dev in hostile:
            with self.subTest(dev=dev):
                with self.assertRaises(ValueError):
                    sm.write_service_args(dev)

    def test_rejects_non_char_device_and_symlink_escape(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            reg = Path(td) / "regular"
            reg.write_text("not a char dev", encoding="utf-8")
            with mock.patch("os.path.lexists", return_value=True), \
                    mock.patch("os.path.realpath", return_value=str(reg)):
                with self.assertRaises(ValueError):
                    sm.validate_v4l2_device("/dev/video10")


if __name__ == "__main__":
    unittest.main()
