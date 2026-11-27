#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for the Hub main-thread stall watchdog (P2-7 / #107)."""

from __future__ import annotations

import os
import sys
import time
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "tools" / "everlive"))

import everlive_viewer as ev  # noqa: E402


class HubWatchdogTests(unittest.TestCase):
    def test_disabled_by_default_has_zero_overhead(self) -> None:
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("EVERLIVE_DEBUG", None)
            self.assertFalse(ev._hub_debug_enabled())
            self.assertIsNone(ev.install_main_thread_watchdog(object()))

    def test_enabled_env_values(self) -> None:
        for val in ("1", "true", "YES"):
            with mock.patch.dict(os.environ, {"EVERLIVE_DEBUG": val}):
                self.assertTrue(ev._hub_debug_enabled())
        with mock.patch.dict(os.environ, {"EVERLIVE_DEBUG": "0"}):
            self.assertFalse(ev._hub_debug_enabled())

    def test_stall_triggers_once_until_next_beat(self) -> None:
        seen = []
        wd = ev.MainThreadWatchdog(
            threshold_ms=50.0,
            poll_interval_s=0.005,
            on_stall=lambda ms, site: seen.append((ms, site)),
        )
        t0 = wd._last_beat
        self.assertFalse(wd.check_once(now=t0 + 0.030))
        self.assertEqual(seen, [])

        self.assertTrue(wd.check_once(now=t0 + 0.065))
        self.assertEqual(len(seen), 1)
        self.assertGreater(seen[0][0], 50.0)
        self.assertIn(".py:", seen[0][1])

        # Repeated checks during the same stall do not spam.
        self.assertFalse(wd.check_once(now=t0 + 0.120))
        self.assertEqual(len(seen), 1)

        # Beat re-arms the watchdog for future stalls.
        wd.beat()
        t1 = wd._last_beat
        self.assertTrue(wd.check_once(now=t1 + 0.075))
        self.assertEqual(len(seen), 2)

    def test_background_thread_detects_real_sleep_stall(self) -> None:
        seen = []
        wd = ev.MainThreadWatchdog(
            threshold_ms=30.0,
            poll_interval_s=0.005,
            on_stall=lambda ms, site: seen.append((ms, site)),
        )
        wd.start()
        try:
            time.sleep(0.060)
        finally:
            wd.stop()
        self.assertEqual(len(seen), 1)
        self.assertGreaterEqual(seen[0][0], 30.0)
        self.assertIn("test_hub_watchdog.py:", seen[0][1])


if __name__ == "__main__":
    unittest.main()
