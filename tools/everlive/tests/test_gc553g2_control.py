"""Unit tests for GC553G2 Branch B (USB/UVC) discovery, protocol, and viewer fallback."""

from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

EVERLIVE_DIR = Path(__file__).resolve().parents[1]
if str(EVERLIVE_DIR) not in sys.path:
    sys.path.insert(0, str(EVERLIVE_DIR))

import card_profile  # noqa: E402
import everlive_viewer  # noqa: E402
import gc553g2_control as gc553  # noqa: E402


V4L2_SAMPLE_144 = """
ioctl: VIDIOC_ENUM_FMT
\tType: Video Capture

\t[0]: 'MJPG' (Motion-JPEG, compressed)
\t\tSize: Discrete 3840x2160
\t\t\tInterval: Discrete 0.007s (144.000 fps)
\t\t\tInterval: Discrete 0.008s (120.000 fps)
\t\t\tInterval: Discrete 0.017s (60.000 fps)
\t\tSize: Discrete 2560x1440
\t\t\tInterval: Discrete 0.008s (120.000 fps)
\t[1]: 'YUYV' (YUYV 4:2:2)
\t\tSize: Discrete 1920x1080
\t\t\tInterval: Discrete 0.017s (60.000 fps)
"""


class TestGC553G2Control(unittest.TestCase):
    def test_sysfs_usb_and_video_discovery(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sys_usb = root / "usb"
            sys_video = root / "video4linux"
            dev = sys_usb / "2-1.4"
            iface = dev / "2-1.4:1.0"
            iface.mkdir(parents=True)
            (dev / "idVendor").write_text("07ca\n")
            (dev / "idProduct").write_text("2553\n")
            (dev / "speed").write_text("10000\n")
            (dev / "busnum").write_text("2\n")
            (dev / "devnum").write_text("7\n")
            (dev / "serial").write_text("AVM12345\n")
            (dev / "product").write_text("Live Gamer ULTRA 2.1\n")

            other = sys_usb / "1-1"
            other.mkdir(parents=True)
            (other / "idVendor").write_text("1d6b\n")
            (other / "idProduct").write_text("0003\n")

            v4 = sys_video / "video4"
            v4.mkdir(parents=True)
            (v4 / "index").write_text("0\n")
            (v4 / "device").symlink_to(iface)

            v5 = sys_video / "video5"
            v5.mkdir(parents=True)
            (v5 / "index").write_text("1\n")
            (v5 / "device").symlink_to(iface)

            found = gc553.find_gc553g2_devices(sys_usb=sys_usb)
            self.assertEqual(found, [dev])

            ident = gc553.read_usb_identity(dev, sys_video=sys_video)
            self.assertEqual(ident["port"], "2-1.4")
            self.assertEqual(ident["bus"], 2)
            self.assertEqual(ident["address"], 7)
            self.assertTrue(ident["superspeed_10g"])
            self.assertEqual(ident["serial"], "AVM12345")
            self.assertEqual(ident["video_node"], "/dev/video4")

    def test_v4l2_format_parsing_and_4k_mjpeg_rates(self):
        modes = gc553.parse_v4l2_formats(V4L2_SAMPLE_144)
        self.assertEqual(len(modes), 3)
        self.assertEqual(gc553.reported_4k_mjpeg_rates(modes), [60, 120, 144])
        self.assertTrue(gc553.has_high_fps_4k_mjpeg(modes))

        only_60 = [m for m in modes if m["width"] == 3840]
        only_60[0]["fps"] = [60.0]
        self.assertFalse(gc553.has_high_fps_4k_mjpeg(only_60))

    def test_at_packet_encoding_and_decoding(self):
        pkt = gc553.build_descriptor_switch_packet(1)
        self.assertEqual(pkt[0], 0xA1)
        self.assertEqual(sum(pkt) & 255, 0)

        with self.assertRaises(ValueError):
            gc553.build_descriptor_switch_packet(2)

        # Short response with 4-byte payload
        body = b"\x01\x00\x00\x00"
        short_raw = bytes([0xA1, len(body) + 2, 0x00, 0x00]) + body
        short_raw += bytes([(-sum(short_raw)) & 255])
        self.assertEqual(gc553.decode_at_response(short_raw, allow_short=True), body)

        # Ack without payload (0xA1 0x00 0x5F)
        self.assertEqual(
            gc553.decode_at_response(bytes.fromhex("a1 00 5f"), allow_short=True),
            b"",
        )

        # Extended response (132-byte descriptor index payload)
        ext_payload = b"\x01\x00\x00\x00\x01" + bytes(127)
        ext_len = len(ext_payload) + 1
        ext_raw = bytes([0xA1, 0x80 | (ext_len >> 8), ext_len & 255, 0x00]) + ext_payload
        ext_raw += bytes([(-sum(ext_raw)) & 255])
        self.assertEqual(gc553.decode_at_response(ext_raw), ext_payload)

    def test_decode_hdmi_timing(self):
        buf = bytearray(40)
        struct.pack_into("<HH", buf, 6, 1440, 2560)
        struct.pack_into("<H", buf, 14, 12000)
        timing = gc553.decode_hdmi_timing(bytes(buf))
        self.assertEqual(timing, {"width": 2560, "height": 1440, "refresh_hz": 120.0, "interlaced": False})

        buf[0] = 1
        struct.pack_into("<HH", buf, 6, 540, 1920)
        struct.pack_into("<H", buf, 14, 5994)
        interlaced = gc553.decode_hdmi_timing(bytes(buf))
        self.assertEqual(interlaced["height"], 1080)
        self.assertTrue(interlaced["interlaced"])

    def test_viewer_detect_gc573_driver_usb_fallback(self):
        fake_usb_path = Path("/sys/bus/usb/devices/2-1.4")
        with mock.patch("glob.glob", return_value=[]), mock.patch.object(
            everlive_viewer._gc553g2_control,
            "find_gc553g2_devices",
            return_value=[fake_usb_path],
        ):
            info = everlive_viewer.detect_gc573_driver()
        self.assertEqual(info["mode"], "uvc")
        self.assertEqual(info["driver"], "uvcvideo")
        self.assertEqual(info["model"], everlive_viewer.MODEL_GC553G2)
        self.assertEqual(info["usb_port"], "2-1.4")

    def test_card_profile_gc553g2_and_usb_sysfs_fallback(self):
        profile = card_profile.detect_card_profile("gc553g2")
        self.assertEqual(profile.model, card_profile.MODEL_GC553G2)
        self.assertTrue(profile.is_uvc)
        self.assertFalse(profile.supports_framegen)
        self.assertTrue(profile.supports(card_profile.CAP_4K120))
        self.assertTrue(profile.supports(card_profile.CAP_RGB_LED))

        with tempfile.TemporaryDirectory() as tmp:
            usb_root = Path(tmp) / "usb"
            dev = usb_root / "3-2"
            dev.mkdir(parents=True)
            (dev / "idVendor").write_text("07CA\n")
            (dev / "idProduct").write_text("2553\n")
            self.assertEqual(
                card_profile._detect_usb_from_sysfs(str(usb_root)),
                card_profile.MODEL_GC553G2,
            )

            with mock.patch("os.listdir", return_value=[]), mock.patch.object(
                card_profile, "_detect_usb_from_sysfs", return_value=card_profile.MODEL_GC553G2
            ):
                self.assertEqual(card_profile._detect_from_sysfs(), card_profile.MODEL_GC553G2)

    def test_uvc_playback_uses_mpv_mjpeg_and_skips_everlive_display(self):
        hw = {
            "mode": "uvc",
            "driver": "uvcvideo",
            "model": everlive_viewer.MODEL_GC553G2,
            "usb_port": "2-1.4",
        }
        with mock.patch.object(everlive_viewer, "detect_gc573_driver", return_value=hw), \
             mock.patch.object(everlive_viewer, "find_pipewire_source_node", return_value=None):
            streamer = everlive_viewer.GameStreamer(vdev="/dev/video4", adev="none")

        self.assertTrue(streamer.uvc_mode)

        fake_proc = mock.Mock()
        fake_proc.pid = 4242
        fake_proc.poll.return_value = 0
        with mock.patch("os.path.exists", side_effect=lambda p: p == "/dev/video4"), \
             mock.patch("os.path.isfile", return_value=True), \
             mock.patch("os.access", return_value=True), \
             mock.patch("shutil.which", return_value="/usr/bin/mpv"), \
             mock.patch("subprocess.run") as mock_run, \
             mock.patch("subprocess.Popen", return_value=fake_proc) as mock_popen:
            ok = streamer.start(preset_name="3840x2160 @ 120 FPS", fullscreen=True, vsync=False)
            self.assertTrue(ok)
            v4l2_cmd = mock_run.call_args_list[0][0][0]
            self.assertIn("--set-fmt-video=width=3840,height=2160,pixelformat=MJPG", v4l2_cmd)
            self.assertIn("--set-parm=120", v4l2_cmd)

            mpv_cmd = mock_popen.call_args[0][0]
            self.assertEqual(mpv_cmd[0], "mpv")
            self.assertIn("--demuxer-lavf-o-add=input_format=mjpeg", mpv_cmd)
            self.assertIn("--demuxer-lavf-o-add=video_size=3840x2160", mpv_cmd)
            self.assertIn("--demuxer-lavf-o-add=framerate=120", mpv_cmd)
            streamer.stop()

            ok_60 = streamer.start(preset_name="3840x2160 @ 60 FPS")
            self.assertTrue(ok_60)
            mpv_cmd_60 = mock_popen.call_args[0][0]
            self.assertIn("--demuxer-lavf-o-add=input_format=yuyv422", mpv_cmd_60)
            self.assertIn("--demuxer-lavf-o-add=framerate=60", mpv_cmd_60)
            streamer.stop()

            mock_run.reset_mock()
            ok_auto = streamer.start(preset_name="Auto (HDMI Signal)")
            self.assertTrue(ok_auto)
            self.assertFalse(
                any("--set-dv-bt-timings" in c[0][0] for c in mock_run.call_args_list)
            )
            streamer.stop()

        with mock.patch("os.path.exists", side_effect=lambda p: p == "/dev/video4"), \
             mock.patch("shutil.which", return_value=None):
            ok_missing = streamer.start(preset_name="3840x2160 @ 120 FPS")
            self.assertFalse(ok_missing)
            self.assertIn("mpv", streamer.last_error)

    def test_gc553g2_cli_subcommands(self):
        buf = io.StringIO()
        status_payload = {"port": "2-1.4", "video_node": "/dev/video4", "speed_mbps": 10000}
        with mock.patch.object(gc553, "query_device_status", return_value=status_payload) as m_st, \
             redirect_stdout(buf):
            rc = gc553.main(["--port", "2-1.4", "--json", "status", "--no-at"])
        self.assertEqual(rc, 0)
        m_st.assert_called_once_with(port="2-1.4", include_at=False)
        self.assertEqual(json.loads(buf.getvalue()), status_payload)

        with mock.patch.object(gc553, "set_descriptor_index", return_value={"descriptor_index": 1}) as m_hi, \
             mock.patch.object(gc553, "set_edid_mode", return_value={"edid_mode": "internal"}) as m_ed, \
             mock.patch.object(gc553, "set_rgb_mode", return_value={"rgb": "#38bdf8"}) as m_rgb, \
             redirect_stdout(io.StringIO()):
            self.assertEqual(gc553.main(["high-fps"]), 0)
            self.assertEqual(gc553.main(["default-fps"]), 0)
            self.assertEqual(gc553.main(["edid", "internal"]), 0)
            self.assertEqual(gc553.main(["rgb", "#38BDF8", "--brightness", "80"]), 0)

        self.assertEqual(m_hi.call_args_list[0][0], (1,))
        self.assertEqual(m_hi.call_args_list[1][0], (0,))
        m_ed.assert_called_once_with("internal", port=None)
        m_rgb.assert_called_once_with("#38BDF8", brightness=80, port=None)

        err_buf = io.StringIO()
        with mock.patch.object(gc553, "set_descriptor_index", side_effect=PermissionError("denied")), \
             mock.patch("sys.stderr", err_buf):
            self.assertEqual(gc553.main(["high-fps"]), 1)
        self.assertIn("install.sh", err_buf.getvalue())


if __name__ == "__main__":
    unittest.main()
