#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for capture card detection and the Hub's card selector."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

try:
    import everlive_viewer as ev  # noqa: E402
except ImportError as exc:  # pragma: no cover - Qt missing on the runner
    ev = None
    _IMPORT_ERROR = str(exc)


def _card(pci: str, model: str, mode: str = "vfio") -> dict:
    return {"pci": pci, "model": model, "mode": mode, "driver": "vfio-pci",
            "subsystem": None}


GC573 = _card("0000:85:00.0", "gc573")
GC570D = _card("0000:03:00.0", "gc570d")
HWS = _card("0000:84:00.0", "hwsuhd")


@unittest.skipIf(ev is None, "everlive_viewer not importable")
class SelectCardTests(unittest.TestCase):
    def _select(self, cards, env=None, saved=None):
        settings = {} if saved is None else {"card_pci": saved}
        with mock.patch.dict(os.environ, env or {}, clear=False), \
                mock.patch.object(ev, "load_settings", return_value=settings):
            for key in ("EVERLIVE_CARD_PCI", "EVERLIVE_CARD"):
                if key not in (env or {}):
                    os.environ.pop(key, None)
            return ev.select_capture_card(cards)

    def test_no_cards(self) -> None:
        self.assertIsNone(self._select([]))

    def test_default_prefers_first_avermedia(self) -> None:
        # Same as before the selector: first AVerMedia card in PCI order.
        self.assertIs(self._select([GC570D, HWS, GC573]), GC570D)
        self.assertIs(self._select([HWS, GC573]), GC573)

    def test_hws_only_when_alone(self) -> None:
        self.assertIs(self._select([HWS]), HWS)

    def test_saved_choice(self) -> None:
        self.assertIs(self._select([GC573, HWS], saved=HWS["pci"]), HWS)

    def test_saved_choice_for_missing_card_is_ignored(self) -> None:
        self.assertIs(self._select([GC573, HWS], saved="0000:01:00.0"), GC573)

    def test_model_env_beats_saved(self) -> None:
        self.assertIs(self._select([GC573, HWS], env={"EVERLIVE_CARD": "gc573"},
                                   saved=HWS["pci"]), GC573)

    def test_pci_env_beats_everything(self) -> None:
        self.assertIs(self._select([GC573, HWS],
                                   env={"EVERLIVE_CARD_PCI": HWS["pci"],
                                        "EVERLIVE_CARD": "gc573"},
                                   saved=GC573["pci"]), HWS)

    def test_unknown_pci_env_falls_through(self) -> None:
        self.assertIs(self._select([GC573, HWS],
                                   env={"EVERLIVE_CARD_PCI": "0000:01:00.0"}),
                      GC573)

    def test_display_name(self) -> None:
        self.assertEqual(ev.card_display_name(GC573), "GC573 · 85:00.0")
        self.assertEqual(ev.card_display_name(HWS), "HWS UHD · 84:00.0")


@unittest.skipIf(ev is None, "everlive_viewer not importable")
class IdentifyCardTests(unittest.TestCase):
    def _dev(self, vendor: str, device: str, subsystem: str) -> str:
        root = tempfile.mkdtemp()
        self.addCleanup(lambda: __import__("shutil").rmtree(root))
        for name, value in (("vendor", vendor), ("device", device),
                            ("subsystem_device", subsystem)):
            with open(os.path.join(root, name), "w") as f:
                f.write(value + "\n")
        return root

    def test_models(self) -> None:
        self.assertEqual(ev._identify_card(self._dev("0x1461", "0x0054", "0x5730")), "gc573")
        self.assertEqual(ev._identify_card(self._dev("0x1461", "0x0054", "0x5700")), "gc570d")
        self.assertEqual(ev._identify_card(self._dev("0x1461", "0x0055", "0x0000")), "gc573")
        self.assertEqual(ev._identify_card(self._dev("0x8888", "0x8581", "0x0007")), "hwsuhd")

    def test_unsupported(self) -> None:
        # Other HWS models (different subsystem) and unrelated devices.
        self.assertIsNone(ev._identify_card(self._dev("0x8888", "0x8581", "0x0001")))
        self.assertIsNone(ev._identify_card(self._dev("0x10de", "0x2684", "0x0000")))


if __name__ == "__main__":
    unittest.main()
