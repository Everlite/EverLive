#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for the multi-card runner (everlive_multi.py)."""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import card_profile  # noqa: E402
import everlive_multi as em  # noqa: E402


def _card(pci: str, model: str, mode: str = "vfio") -> dict:
    return {"pci": pci, "model": model, "mode": mode,
            "driver": "vfio-pci" if mode == "vfio" else None, "subsystem": None}


GC573 = _card("0000:85:00.0", "gc573")
GC570D = _card("0000:03:00.0", "gc570d")
HWS = _card("0000:84:00.0", "hwsuhd")


class NamingTests(unittest.TestCase):
    def test_pci_slug(self) -> None:
        self.assertEqual(em.pci_slug("0000:84:00.0"), "84-00-0")
        self.assertEqual(em.pci_slug("0001:02:00.1"), "0001-02-00-1")

    def test_sockets_and_taps_are_per_card(self) -> None:
        a = em.session_socket_path("gc573", GC573["pci"], runtime_dir="/run/user/1000")
        b = em.session_socket_path("hwsuhd", HWS["pci"], runtime_dir="/run/user/1000")
        self.assertEqual(a, "/run/user/1000/everlive-gc573-85-00-0.sock")
        self.assertNotEqual(a, b)
        self.assertNotIn("everlive.sock", (os.path.basename(a), os.path.basename(b)))
        self.assertEqual(em.audio_tap_name("hwsuhd", HWS["pci"]),
                         "everlive-hwsuhd-84-00-0-audio")
        self.assertLess(len(a), 108)

    def test_take_filenames_share_stamp_and_differ(self) -> None:
        srcs = em.recording_sources(GC573) + em.recording_sources(HWS)
        names = {em.take_filename(s, "20261119_120000", 60, "mkv") for s in srcs}
        self.assertEqual(len(names), 2)
        for name in names:
            self.assertTrue(name.startswith("EverLive_20261119_120000_"))
            self.assertTrue(name.endswith("_60fps.mkv"))


class EngineCommandTests(unittest.TestCase):
    def test_gc573(self) -> None:
        cmd, env = em.build_engine_command(GC573, "/bin/gc573", socket="/s.sock")
        self.assertEqual(cmd, ["/bin/gc573", "0000:85:00.0",
                               "--audio-tap=everlive-gc573-85-00-0-audio"])
        self.assertEqual(env, {"EVERLIVE_SOCKET": "/s.sock"})

    def test_hws_headless(self) -> None:
        cmd, _ = em.build_engine_command(HWS, "/bin/hws", socket="/s", headless=True)
        self.assertEqual(cmd[1], "0000:84:00.0")
        self.assertIn("--audio-tap=everlive-hwsuhd-84-00-0-audio", cmd)
        self.assertEqual(cmd[-1], "--headless")

    def test_gc570d(self) -> None:
        cmd, _ = em.build_engine_command(GC570D, "/bin/duo", socket="/s")
        self.assertEqual(cmd, ["/bin/duo", "--pci", "0000:03:00.0", "--sdl"])
        cmd, _ = em.build_engine_command(GC570D, "/bin/duo", socket="/s", headless=True)
        self.assertEqual(cmd[-1], "--headless")


class RecordingSourceTests(unittest.TestCase):
    def test_single_input_cards(self) -> None:
        (gc,) = em.recording_sources(GC573, "/lib/libgc573-v4l2.so")
        self.assertEqual(gc.camera_node, "/dev/video63")
        self.assertEqual(gc.audio_source, "everlive-gc573-85-00-0-audio.monitor")
        (hws,) = em.recording_sources(HWS)
        self.assertEqual(hws.camera_node, "/dev/video64")
        self.assertNotEqual(gc.audio_source, hws.audio_source)

    def test_duo_has_two_inputs(self) -> None:
        srcs = em.recording_sources(GC570D)
        self.assertEqual([s.camera_node for s in srcs], ["/dev/video60", "/dev/video61"])
        self.assertEqual([s.audio_source for s in srcs],
                         ["gc570d_hdmi1_audio", "gc570d_hdmi2_audio"])


class GeometryTests(unittest.TestCase):
    def test_locked(self) -> None:
        st = {"signal": {"locked": True, "width": 3840, "height": 2160, "rate_hz": 59.94}}
        self.assertEqual(em.signal_geometry(st), (3840, 2160, 59.94, True))

    def test_unlocked_or_missing(self) -> None:
        self.assertEqual(em.signal_geometry(None), (1920, 1080, 60.0, False))
        st = {"signal": {"locked": False, "width": 0, "height": 0, "rate_hz": 0}}
        self.assertEqual(em.signal_geometry(st), (1920, 1080, 60.0, False))


class PlanTests(unittest.TestCase):
    def test_plans_per_card(self) -> None:
        runner = em.MultiCardRunner([GC573, HWS], rec_dir="/tmp/rec", encoder="libx264")
        statuses = {
            runner.sessions[0].socket: {"signal": {"locked": True, "width": 1920,
                                                   "height": 1080, "rate_hz": 120.0}},
            runner.sessions[1].socket: {"signal": {"locked": True, "width": 3840,
                                                   "height": 2160, "rate_hz": 60.0}},
        }
        for sess in runner.sessions:
            sess.sources = em.recording_sources(sess.card, "/lib/x.so")
            sess.proc = mock.Mock(poll=mock.Mock(return_value=None))
        with mock.patch.object(em.everlive_client, "call",
                               side_effect=lambda m, path=None, timeout=0: statuses[path]):
            plans = runner.build_plans(stamp="20261119_120000")
        self.assertEqual(len(plans), 2)
        (_, _, p1), (_, _, p2) = plans
        self.assertIn("/dev/video63", p1.cmd)
        self.assertIn("/dev/video64", p2.cmd)
        self.assertEqual((p1.width, p1.height, p1.fps), (1920, 1080, 120))
        self.assertEqual((p2.width, p2.height, p2.fps), (3840, 2160, 60))
        self.assertIn("everlive-gc573-85-00-0-audio.monitor", p1.cmd)
        self.assertIn("everlive-hwsuhd-84-00-0-audio.monitor", p2.cmd)
        self.assertNotEqual(p1.output_path, p2.output_path)
        self.assertTrue(os.path.basename(p1.output_path).startswith("EverLive_20261119_120000_"))


class SelectCardsTests(unittest.TestCase):
    def test_filters(self) -> None:
        cards = [HWS, GC573]
        self.assertEqual(em.select_cards(cards, []), cards)
        self.assertEqual(em.select_cards(cards, ["85:00.0"]), [GC573])
        self.assertEqual(em.select_cards(cards, ["hwsuhd"]), [HWS])
        self.assertEqual(em.select_cards(cards, ["0000:84:00.0", "gc573"]), cards)
        self.assertEqual(em.select_cards(cards, ["gc570d"]), [])


class DetectPciCardsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root)
        drivers = os.path.join(self.root, "drivers")
        os.makedirs(os.path.join(drivers, "vfio-pci"))
        self.devs = os.path.join(self.root, "devices")
        os.makedirs(self.devs)
        self._dev("0000:85:00.0", "0x1461", "0x0054", "0x5730", "vfio-pci")
        self._dev("0000:84:00.0", "0x8888", "0x8581", "0x0007", None)
        self._dev("0000:01:00.0", "0x10de", "0x2684", "0x0000", None)

    def _dev(self, bdf, vendor, device, subsystem, driver) -> None:
        path = os.path.join(self.devs, bdf)
        os.makedirs(path)
        for name, value in (("vendor", vendor), ("device", device),
                            ("subsystem_device", subsystem)):
            with open(os.path.join(path, name), "w") as f:
                f.write(value + "\n")
        if driver:
            os.symlink(os.path.join(self.root, "drivers", driver),
                       os.path.join(path, "driver"))

    def test_detect(self) -> None:
        cards = card_profile.detect_pci_cards(self.devs)
        self.assertEqual([(c["pci"], c["model"], c["mode"]) for c in cards], [
            ("0000:84:00.0", "hwsuhd", "unknown"),
            ("0000:85:00.0", "gc573", "vfio"),
        ])


if __name__ == "__main__":
    unittest.main()
