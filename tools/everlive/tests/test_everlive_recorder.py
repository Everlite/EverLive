#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for the direct video/audio recording helper."""

import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import everlive_recorder as rec  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parents[3]
GC573_PRELOAD = REPO_ROOT / "tools" / "opengc573-userd" / "libgc573-v4l2.so"


class RecordingPlanTests(unittest.TestCase):
    def test_resolve_recording_fps_auto_and_explicit(self) -> None:
        self.assertEqual(rec.resolve_recording_fps("auto", 60.0, "off", 2), 60)
        self.assertEqual(rec.resolve_recording_fps("auto", 60.0, "2x", 2), 120)
        self.assertEqual(rec.resolve_recording_fps("auto", 60.0, "3x", 3), 180)
        self.assertEqual(rec.resolve_recording_fps("auto", 60.0, "4x", 4), 240)
        self.assertEqual(rec.resolve_recording_fps("auto", 120.0, "4x", 4), 240)
        self.assertEqual(rec.resolve_recording_fps("240", 60.0, "off", 2), 240)
        self.assertEqual(rec.resolve_recording_fps("30", 120.0, "4x", 4), 30)
        self.assertEqual(rec.resolve_recording_fps("10", 60.0, "off", 2), 24)
        self.assertEqual(rec.resolve_recording_fps("500", 60.0, "off", 2), 240)

    def test_resolve_capture_source(self) -> None:
        self.assertEqual(
            rec.resolve_capture_source(
                "auto", framegen_active=True, window_id=12345, display=":0"
            ),
            "window",
        )
        self.assertEqual(
            rec.resolve_capture_source(
                "auto", framegen_active=False, window_id=12345, display=":0"
            ),
            "shm",
        )
        self.assertEqual(
            rec.resolve_capture_source(
                "auto", framegen_active=True, window_id=0, display=":0"
            ),
            "shm",
        )
        self.assertEqual(
            rec.resolve_capture_source(
                "window", framegen_active=False, window_id=0, display=":0"
            ),
            "shm",
        )
        self.assertEqual(
            rec.resolve_capture_source(
                "window", framegen_active=False, window_id=99, display=""
            ),
            "shm",
        )
        self.assertEqual(
            rec.resolve_capture_source(
                "shm", framegen_active=True, window_id=12345, display=":0"
            ),
            "shm",
        )

    def test_build_plan_gc573_shm_240fps(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            plan = rec.build_recording_plan(
                output_dir=tmp,
                model="gc573",
                vfio_mode=True,
                preload_library="/usr/local/lib/libgc573-v4l2.so",
                width=1920,
                height=1080,
                source_fps=60.0,
                fps_setting="240",
                source_setting="shm",
                encoder_setting="libx264",
                quality_setting="high",
                container_format="mkv",
                audio_source="everlive-capture-audio.monitor",
                resolved_encoder="libx264",
            )
            self.assertEqual(plan.env.get("LD_PRELOAD"), "/usr/local/lib/libgc573-v4l2.so")
            self.assertEqual(plan.fps, 240)
            self.assertEqual(plan.source, "shm")
            self.assertTrue(plan.output_path.endswith("_240fps.mkv"))
            self.assertIn("/dev/video63", plan.cmd)
            self.assertIn("-framerate", plan.cmd)
            self.assertEqual(plan.cmd[plan.cmd.index("-framerate") + 1], "240")
            self.assertIn("-r", plan.cmd)
            self.assertEqual(plan.cmd[plan.cmd.index("-r") + 1], "240")
            self.assertIn("everlive-capture-audio.monitor", plan.cmd)
            self.assertNotIn("+faststart", plan.cmd)

    def test_build_plan_gc570d_hdmi2_mp4(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            plan = rec.build_recording_plan(
                output_dir=tmp,
                model="gc570d",
                duo_input=1,
                vfio_mode=True,
                preload_library="/usr/local/lib/libgc570d-v4l2.so",
                width=1920,
                height=1080,
                source_fps=60.0,
                fps_setting="60",
                source_setting="shm",
                quality_setting="ultra",
                container_format="mp4",
                resolved_encoder="libx264",
            )
            self.assertEqual(plan.env.get("LD_PRELOAD"), "/usr/local/lib/libgc570d-v4l2.so")
            self.assertIn("/dev/video61", plan.cmd)
            self.assertTrue(plan.output_path.endswith(".mp4"))
            self.assertIn("+faststart", plan.cmd)
            self.assertEqual(plan.cmd[plan.cmd.index("-crf") + 1], "14")

    def test_build_plan_window_vaapi_framegen(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            plan = rec.build_recording_plan(
                output_dir=tmp,
                model="gc573",
                vfio_mode=True,
                width=2560,
                height=1440,
                source_fps=60.0,
                fps_setting="auto",
                source_setting="auto",
                framegen_mode="4x",
                fg_multiplier=4,
                window_id=0x12345,
                display=":0",
                vaapi_device="/dev/dri/renderD128",
                resolved_encoder="h264_vaapi",
            )
            self.assertEqual(plan.source, "window")
            self.assertEqual(plan.fps, 240)
            self.assertEqual(plan.env, {})
            self.assertIn("x11grab", plan.cmd)
            self.assertIn(str(0x12345), plan.cmd)
            self.assertIn("vaapi=va:/dev/dri/renderD128", plan.cmd)
            vf = plan.cmd[plan.cmd.index("-vf") + 1]
            self.assertIn("format=nv12,hwupload", vf)

    def test_encoder_fallback_selection(self) -> None:
        rec._ENCODER_PROBE_CACHE.clear()
        with mock.patch.object(rec, "find_vaapi_device", return_value="/dev/dri/renderD128"), \
             mock.patch.object(rec, "probe_encoder_available", side_effect=lambda e, _d=None: e == "h264_vaapi"):
            self.assertEqual(rec.resolve_encoder("auto"), "h264_vaapi")
            self.assertEqual(rec.resolve_encoder("h264_nvenc"), "h264_vaapi")

        with mock.patch.object(rec, "find_vaapi_device", return_value=None), \
             mock.patch.object(rec, "probe_encoder_available", side_effect=lambda e, _d=None: e == "libx264"):
            self.assertEqual(rec.resolve_encoder("auto"), "libx264")

    def test_resolve_pulse_monitor_modes(self) -> None:
        self.assertIsNone(rec.resolve_pulse_monitor(audio_mode="mute"))
        self.assertEqual(
            rec.resolve_pulse_monitor(audio_mode="everlive", model="gc570d", duo_input=0),
            "gc570d_hdmi1_audio",
        )
        self.assertEqual(
            rec.resolve_pulse_monitor(audio_mode="everlive", model="gc570d", duo_input=1),
            "gc570d_hdmi2_audio",
        )
        with mock.patch.object(rec, "ensure_isolated_audio_links", return_value=True) as spy:
            self.assertEqual(
                rec.resolve_pulse_monitor(
                    audio_tap_active=False, audio_mode="everlive", model="gc573"
                ),
                "everlive-capture-audio.monitor",
            )
            spy.assert_called_once_with("everlive-capture-audio")
        with mock.patch.object(rec, "ensure_isolated_audio_links", return_value=True) as spy:
            self.assertEqual(
                rec.resolve_pulse_monitor(
                    audio_tap_active=True, audio_mode="everlive", model="gc573"
                ),
                "everlive-capture-audio.monitor",
            )
            spy.assert_not_called()
        with mock.patch("shutil.which", return_value="/usr/bin/pactl"), \
             mock.patch("subprocess.check_output", return_value="alsa_output.usb.sink\n"):
            self.assertEqual(
                rec.resolve_pulse_monitor(audio_mode="desktop", model="gc573"),
                "alsa_output.usb.sink.monitor",
            )

    def test_ensure_isolated_audio_links(self) -> None:
        already_linked = (
            "everlive-capture-audio:playback_FL\n"
            "  |<- EverLive Capture Tap:output_FL\n"
        )
        with mock.patch("shutil.which", return_value="/usr/bin/pw-link"), \
             mock.patch("subprocess.check_output", return_value=already_linked), \
             mock.patch("subprocess.run") as run_spy:
            self.assertTrue(rec.ensure_isolated_audio_links())
            run_spy.assert_not_called()

        unlinked = (
            "everlive-capture-audio:playback_FL\n"
            "everlive-capture-audio:playback_FR\n"
            "alsa_output.usb:playback_FL\n"
            "  |<- EverLive Audio:output_FL\n"
        )
        outputs = (
            "vesktop:output_FL\n"
            "EverLive Audio:output_FL\n"
            "EverLive Audio:output_FR\n"
        )
        with mock.patch("shutil.which", return_value="/usr/bin/pw-link"), \
             mock.patch("subprocess.check_output", side_effect=[unlinked, outputs]), \
             mock.patch("subprocess.run", return_value=mock.MagicMock(returncode=0)) as run_spy:
            self.assertTrue(rec.ensure_isolated_audio_links())
            self.assertEqual(run_spy.call_count, 2)
            self.assertEqual(
                run_spy.call_args_list[0].args[0],
                ["/usr/bin/pw-link", "EverLive Audio:output_FL", "everlive-capture-audio:playback_FL"],
            )
            self.assertEqual(
                run_spy.call_args_list[1].args[0],
                ["/usr/bin/pw-link", "EverLive Audio:output_FR", "everlive-capture-audio:playback_FR"],
            )


class RecorderLifecycleTests(unittest.TestCase):
    def test_mocked_start_poll_stop_lifecycle(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out_file = Path(tmp) / "test.mkv"
            out_file.write_bytes(b"\x1a\x45\xdf\xa3" * 16)
            log_file = Path(tmp) / "rec.log"
            recorder = rec.EverLiveRecorder(log_path=str(log_file))
            plan = rec.RecordingPlan(
                cmd=["ffmpeg", "-y", str(out_file)],
                env={},
                output_path=str(out_file),
                fps=60,
                source="shm",
                encoder="libx264",
                width=1920,
                height=1080,
                audio_source=None,
            )

            fake_proc = mock.MagicMock()
            fake_proc.poll.return_value = None
            fake_proc.pid = 999999
            fake_proc.stdin = mock.MagicMock()

            with mock.patch("shutil.which", return_value="/usr/bin/ffmpeg"), \
                 mock.patch("subprocess.Popen", return_value=fake_proc):
                ok, msg = recorder.start(plan)
                self.assertTrue(ok, msg)
                self.assertTrue(recorder.is_recording)
                self.assertTrue(recorder.poll())
                self.assertEqual(recorder.file_size_bytes, 64)

                fake_proc.poll.side_effect = [None, 0]
                ok_stop, msg_stop = recorder.stop()
                self.assertTrue(ok_stop, msg_stop)
                fake_proc.stdin.write.assert_called_with(b"q\n")
                fake_proc.wait.assert_called()
                self.assertFalse(recorder.is_recording)

    @unittest.skipUnless(
        bool(shutil.which("ffmpeg")) and GC573_PRELOAD.is_file(),
        "requires ffmpeg and compiled libgc573-v4l2.so",
    )
    def test_live_shm_preload_ffmpeg_smoke(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out_file = Path(tmp) / "shm_smoke.mkv"
            log_file = Path(tmp) / "rec_smoke.log"
            recorder = rec.EverLiveRecorder(log_path=str(log_file))
            plan = rec.build_recording_plan(
                output_dir=tmp,
                model="gc573",
                vfio_mode=True,
                preload_library=str(GC573_PRELOAD),
                width=1920,
                height=1080,
                source_fps=60.0,
                fps_setting="240",
                source_setting="shm",
                encoder_setting="libx264",
                quality_setting="balanced",
                container_format="mkv",
                audio_source=None,
                resolved_encoder="libx264",
                filename=out_file.name,
            )
            ok, msg = recorder.start(plan)
            self.assertTrue(ok, f"start failed: {msg}")
            self.assertTrue(recorder.is_recording)
            ok_stop, msg_stop = recorder.stop()
            self.assertTrue(ok_stop, msg_stop)
            self.assertTrue(out_file.is_file())
            self.assertGreater(out_file.stat().st_size, 0)


if __name__ == "__main__":
    unittest.main()
