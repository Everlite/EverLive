#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for the webcam bridge helpers and the recorder's PiP overlay."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import everlive_cam as cam  # noqa: E402
import everlive_recorder as rec  # noqa: E402

CARD_LIB = "/usr/local/lib/libgc573-v4l2.so"
CAM_LIB = "/usr/local/lib/libeverlive-cam-v4l2.so"


def _plan(tmp: str, **kw) -> rec.RecordingPlan:
    args = dict(
        output_dir=tmp, model="gc573", vfio_mode=True,
        preload_library=CARD_LIB, width=3840, height=2160, source_fps=60.0,
        source_setting="shm", encoder_setting="libx264",
        resolved_encoder="libx264", container_format="mkv",
    )
    args.update(kw)
    return rec.build_recording_plan(**args)


class SettingsTests(unittest.TestCase):
    def test_parse_mode(self) -> None:
        self.assertEqual(cam.parse_mode("1920x1080@60"), (1920, 1080, 60))
        self.assertEqual(cam.parse_mode("garbage"), (1280, 720, 60))
        self.assertEqual(cam.parse_mode("99999x1@30"), (1280, 720, 60))

    def test_normalize_settings_fills_and_repairs(self) -> None:
        cfg = cam.normalize_settings({
            "pip_enabled": 1, "pip_corner": "middle", "pip_size": 90,
            "pip_mode": "bad", "unrelated": True})
        self.assertIs(cfg["pip_enabled"], True)
        self.assertEqual(cfg["pip_corner"], "br")
        self.assertEqual(cfg["pip_size"], 50)
        self.assertEqual(cfg["pip_mode"], "1280x720@60")
        self.assertNotIn("unrelated", cfg)
        self.assertEqual(cam.normalize_settings({})["pip_enabled"], False)

    def test_option_keys_are_valid(self) -> None:
        for _label, corner in cam.PIP_CORNER_OPTIONS:
            self.assertIn(corner, rec.PIP_CORNERS)
        for _label, mode in cam.PIP_MODE_OPTIONS:
            self.assertEqual(f"{cam.parse_mode(mode)[0]}x{cam.parse_mode(mode)[1]}"
                             f"@{cam.parse_mode(mode)[2]}", mode)


class CommandTests(unittest.TestCase):
    def test_parse_device_list(self) -> None:
        text = "/dev/video0\tElgato Facecam 4K\n\njunk line\n/dev/video4\t\n"
        self.assertEqual(cam.parse_device_list(text), [
            ("/dev/video0", "Elgato Facecam 4K"),
            ("/dev/video4", "/dev/video4"),
        ])

    def test_camd_command(self) -> None:
        self.assertEqual(
            cam.build_camd_command("/x/everlive_camd", "/dev/video0", "1920x1080@60"),
            ["/x/everlive_camd", "--size", "1920x1080", "--fps", "60",
             "--device", "/dev/video0"])
        self.assertNotIn("--device", cam.build_camd_command("camd", "", "1280x720@30"))

    def test_parse_v4l2_controls(self) -> None:
        sample = """
User Controls

                     brightness 0x00980900 (int)    : min=0 max=255 step=1 default=128 value=140 flags=has-min-max
        white_balance_automatic 0x0098090c (bool)   : default=1 value=1
           power_line_frequency 0x00980918 (menu)   : min=0 max=2 default=2 value=2 (60 Hz)
				0: Disabled
				1: 50 Hz
				2: 60 Hz
      white_balance_temperature 0x0098091a (int)    : min=2800 max=6500 step=1 default=4600 value=4600 flags=inactive, has-min-max

Camera Controls

                  auto_exposure 0x009a0901 (menu)   : min=0 max=3 default=3 value=3 (Aperture Priority Mode)
				1: Manual Mode
				3: Aperture Priority Mode
     exposure_dynamic_framerate 0x009a0903 (bool)   : default=0 value=1
        """
        ctrls = cam.parse_v4l2_controls(sample)
        self.assertEqual(len(ctrls), 6)
        self.assertEqual(ctrls["brightness"]["value"], 140)
        self.assertEqual(ctrls["brightness"]["default"], 128)
        self.assertEqual(ctrls["brightness"]["min"], 0)
        self.assertEqual(ctrls["brightness"]["max"], 255)
        self.assertEqual(ctrls["white_balance_automatic"]["type"], "bool")
        self.assertTrue(ctrls["white_balance_temperature"].get("inactive"))
        self.assertEqual(ctrls["power_line_frequency"]["menu"][1], "50 Hz")
        self.assertEqual(ctrls["auto_exposure"]["menu"][1], "Manual Mode")

    def test_preview_command_pipes_rgb_frames(self) -> None:
        cmd, env = cam.build_preview_command(CAM_LIB, 640, 360, 30, ffmpeg="ffmpeg")
        self.assertEqual(cmd[0], "ffmpeg")
        self.assertEqual(cmd[cmd.index("-i") + 1], cam.CAM_NODE)
        self.assertEqual(cmd[cmd.index("-video_size") + 1], "640x360")
        self.assertEqual(cmd[cmd.index("-pix_fmt") + 1], "rgb24")
        self.assertEqual(cmd[-1], "pipe:1")
        self.assertEqual(env, {"LD_PRELOAD": CAM_LIB})

    def test_pip_overlay_from_settings(self) -> None:
        pip = cam.pip_overlay({"pip_corner": "tl", "pip_size": 33,
                               "pip_mode": "1920x1080@60"}, CAM_LIB)
        self.assertEqual((pip.corner, pip.size_pct, pip.fps), ("tl", 33, 60))
        self.assertEqual(pip.node, "/dev/video65")
        self.assertEqual(pip.preload_library, CAM_LIB)

    def test_bridge_unavailable_without_binaries(self) -> None:
        with mock.patch.object(cam, "find_camd", return_value=None), \
                mock.patch.object(cam, "find_cam_preload", return_value=None):
            bridge = cam.CameraBridge()
        self.assertFalse(bridge.available)
        self.assertFalse(bridge.start())
        self.assertIn("make cam", bridge.last_error)


class GeometryTests(unittest.TestCase):
    def test_corners(self) -> None:
        def at(corner: str):
            return rec.pip_geometry(rec.PipOverlay(corner=corner, size_pct=25), 1920, 1080)
        x, y, w, h = at("br")
        self.assertEqual((w, h), (480, 270))
        self.assertEqual((x + w, y + h), (1920 - 48, 1080 - 48))
        self.assertEqual(at("tl")[:2], (48, 48))
        self.assertEqual(at("tr")[:2], (1920 - 48 - 480, 48))
        self.assertEqual(at("bl")[:2], (48, 1080 - 48 - 270))

    def test_size_is_clamped_and_even(self) -> None:
        _x, _y, w, h = rec.pip_geometry(rec.PipOverlay(size_pct=99), 1001, 563)
        self.assertEqual(w % 2, 0)
        self.assertEqual(h % 2, 0)
        self.assertLessEqual(w, 501)


class RecordingPlanTests(unittest.TestCase):
    def test_no_pip_keeps_simple_filter(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            plan = _plan(tmp)
        self.assertIn("-vf", plan.cmd)
        self.assertNotIn("-filter_complex", plan.cmd)
        self.assertIsNone(plan.pip)

    def test_pip_adds_camera_input_and_overlay(self) -> None:
        pip = rec.PipOverlay(preload_library=CAM_LIB, corner="br", size_pct=25, fps=30)
        with tempfile.TemporaryDirectory() as tmp:
            plan = _plan(tmp, pip=pip, audio_source="everlive.monitor")
        cmd = plan.cmd
        self.assertEqual(plan.env["LD_PRELOAD"], f"{CARD_LIB}:{CAM_LIB}")
        inputs = [cmd[i + 1] for i, a in enumerate(cmd) if a == "-i"]
        self.assertEqual(inputs, ["/dev/video63", "/dev/video65", "everlive.monitor"])
        self.assertNotIn("-vf", cmd)
        graph = cmd[cmd.index("-filter_complex") + 1]
        self.assertIn("[1:v]scale=960:-2[pip]", graph)
        self.assertIn("overlay=main_w-overlay_w-96:main_h-overlay_h-96", graph)
        self.assertTrue(graph.endswith("format=yuv420p[v]"))
        maps = [cmd[i + 1] for i, a in enumerate(cmd) if a == "-map"]
        self.assertEqual(maps, ["[v]", "2:a"])
        self.assertIs(plan.pip, pip)

    def test_pip_top_left_without_audio(self) -> None:
        pip = rec.PipOverlay(preload_library=CAM_LIB, corner="tl", size_pct=20)
        with tempfile.TemporaryDirectory() as tmp:
            plan = _plan(tmp, pip=pip, width=1920, height=1080)
        graph = plan.cmd[plan.cmd.index("-filter_complex") + 1]
        self.assertIn("overlay=48:48", graph)
        maps = [plan.cmd[i + 1] for i, a in enumerate(plan.cmd) if a == "-map"]
        self.assertEqual(maps, ["[v]"])

    def test_pip_exact_rect(self) -> None:
        pip = rec.PipOverlay(preload_library=CAM_LIB, rect=(1582, 856, 400, 224))
        with tempfile.TemporaryDirectory() as tmp:
            plan = _plan(tmp, pip=pip)
        graph = plan.cmd[plan.cmd.index("-filter_complex") + 1]
        self.assertIn("[1:v]scale=400:224[pip]", graph)
        self.assertIn("overlay=1582:856:", graph)

    def test_vaapi_chain_kept_after_overlay(self) -> None:
        pip = rec.PipOverlay(preload_library=CAM_LIB)
        with tempfile.TemporaryDirectory() as tmp:
            plan = _plan(tmp, pip=pip, encoder_setting="h264_vaapi",
                         resolved_encoder="h264_vaapi",
                         vaapi_device="/dev/dri/renderD128")
        graph = plan.cmd[plan.cmd.index("-filter_complex") + 1]
        self.assertTrue(graph.endswith("format=nv12,hwupload[v]"))


if __name__ == "__main__":
    unittest.main()
