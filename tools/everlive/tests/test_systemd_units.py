#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Regression tests for the systemd user-unit hardening.

EverLive has three sources for the same unit: the two checked-in service
files and the fallback template rendered by service_manager.py.  Keep the
safe hardening subset in sync and make sure options that would disable the
CAP_IPC_LOCK based VFIO memlock path do not creep back in.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "tools" / "everlive"))

import service_manager as sm  # noqa: E402

UNIT_FILES = (
    REPO_ROOT / "tools" / "libeverlive-core" / "everlive.service",
    REPO_ROOT / "tools" / "opengc570d-userd" / "opengc570d-userd.service",
)

REQUIRED_SERVICE_SETTINGS = {
    "RestrictAddressFamilies": "AF_UNIX AF_INET AF_INET6",
    "SystemCallArchitectures": "native",
    "SystemCallFilter": "@system-service",
    "LockPersonality": "yes",
    "RestrictSUIDSGID": "yes",
}

# These are intentionally deferred: NNP blocks file capabilities, while the
# filesystem options imply a user namespace that drops host CAP_IPC_LOCK.
FORBIDDEN_SERVICE_SETTINGS = (
    "NoNewPrivileges",
    "ProtectSystem",
    "ProtectHome",
    "PrivateTmp",
)


def unit_settings(text: str, target_section: str) -> dict[str, str]:
    """Return key/value pairs from the requested section of a unit file."""
    settings: dict[str, str] = {}
    section = ""
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            continue
        if section != target_section or "=" not in line:
            continue
        key, value = line.split("=", 1)
        settings[key.strip()] = value.strip()
    return settings


def service_settings(text: str) -> dict[str, str]:
    """Return key/value pairs from the [Service] section of a unit file."""
    return unit_settings(text, "Service")


class UnitHardeningTests(unittest.TestCase):
    def assert_hardened(self, text: str, source: str) -> None:
        unit = unit_settings(text, "Unit")
        settings = service_settings(text)

        with self.subTest(source=source, setting="StartLimitIntervalSec"):
            self.assertEqual("60", unit.get("StartLimitIntervalSec"))
        with self.subTest(source=source, setting="StartLimitBurst"):
            self.assertEqual("5", unit.get("StartLimitBurst"))
        with self.subTest(source=source, setting="Restart"):
            self.assertEqual("on-failure", settings.get("Restart"))
        with self.subTest(source=source, setting="RestartSec"):
            self.assertEqual("2", settings.get("RestartSec"))

        for key, expected in REQUIRED_SERVICE_SETTINGS.items():
            with self.subTest(source=source, setting=key):
                self.assertEqual(expected, settings.get(key))

        for key in FORBIDDEN_SERVICE_SETTINGS:
            with self.subTest(source=source, setting=key):
                self.assertNotIn(key, settings)

        exec_start = settings.get("ExecStart", "")
        with self.subTest(source=source, setting="ExecStart"):
            self.assertTrue(exec_start.startswith("/"), exec_start)
        with self.subTest(source=source, setting="LimitMEMLOCK"):
            self.assertNotEqual("infinity", settings.get("LimitMEMLOCK"))

    def test_checked_in_units_are_hardened(self) -> None:
        for path in UNIT_FILES:
            with self.subTest(unit=path.name):
                self.assert_hardened(path.read_text(encoding="utf-8"), str(path))

    def test_generated_unit_is_hardened(self) -> None:
        unit = sm.render_unit("/usr/local/bin/everlive-service")
        self.assert_hardened(unit, "service_manager.render_unit")


if __name__ == "__main__":
    unittest.main()
