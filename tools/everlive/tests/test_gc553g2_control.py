"""Unit tests for GC553G2 Branch B (USB/UVC) discovery, protocol, and viewer fallback."""

from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
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

    @unittest.skipUnless(everlive_viewer.HAS_QT, "PySide6 is not installed")
    def test_closing_usb_hub_to_tray_stops_capture(self):
        hub = SimpleNamespace(
            uvc_mode=True, streamer=SimpleNamespace(is_running=True),
            chk_tray=mock.Mock(), tray_icon=mock.Mock(), _tray_notified=True,
            _stop_stream=mock.Mock(), hide=mock.Mock(),
        )
        hub.chk_tray.isChecked.return_value = True
        event = mock.Mock()

        everlive_viewer.MainWindow.closeEvent(hub, event)

        hub._stop_stream.assert_called_once_with()
        event.ignore.assert_called_once_with()
        hub.hide.assert_called_once_with()

    def test_player_exit_releases_separate_audio_process(self):
        hw = {"mode": "uvc", "model": everlive_viewer.MODEL_GC553G2}
        streamer = everlive_viewer.GameStreamer(vdev="/dev/video4", adev="none", card=hw)
        streamer.is_running = True
        streamer.mpv_proc = mock.Mock(pid=22)
        streamer.mpv_proc.poll.return_value = 0
        audio = mock.Mock(pid=23)
        streamer.pwplay_proc = audio
        with mock.patch("os.getpgid", side_effect=ProcessLookupError):
            self.assertFalse(streamer.poll())

        audio.terminate.assert_called_once_with()
        audio.wait.assert_called_once()
        self.assertIsNone(streamer.pwplay_proc)
        self.assertFalse(streamer.is_running)

    def test_uvc_status_uses_mpv_video_output(self):
        hw = {"mode": "uvc", "model": everlive_viewer.MODEL_GC553G2}
        streamer = everlive_viewer.GameStreamer(vdev="/dev/video4", adev="none", card=hw)
        streamer._mpv_runtime = SimpleNamespace(name="/tmp/everlive-test")
        replies = io.BytesIO(
            b'{"event":"video-reconfig"}\n'
            b'{"request_id":2,"data":59.94,"error":"success"}\n'
            b'{"request_id":1,"data":{"w":1920,"h":1080},"error":"success"}\n'
            b'{"request_id":3,"data":59.83,"error":"success"}\n'
            b'{"request_id":4,"data":2,"error":"success"}\n'
            b'{"request_id":5,"data":{"fresh":[{"last":1200000},{"last":400000}]},"error":"success"}\n'
            b'{"request_id":6,"data":true,"error":"success"}\n'
        )
        with mock.patch.object(everlive_viewer.socket, "socket") as connect:
            client = connect.return_value.__enter__.return_value
            client.makefile.return_value = replies
            info = streamer.get_signal_info()

        self.assertTrue(info["locked"])
        self.assertEqual(info["resolution"], "1920x1080")
        self.assertEqual(info["framerate"], "59.94 FPS")
        self.assertEqual(info["filter_fps"], 59.83)
        self.assertEqual(info["vo_drops"], 2)
        self.assertEqual(info["render_ms"], 1.6)
        self.assertTrue(info["fullscreen"])

    def test_game_fps_estimate_requires_consistent_differences_at_60_hz(self):
        with mock.patch.object(everlive_viewer.time, "monotonic", return_value=102):
            samples = [(101, diff) for diff in [0.1, 1.3] * 60]
            self.assertEqual(everlive_viewer._estimate_uvc_game_fps(samples, 59.94), 29.97)
            self.assertIsNone(everlive_viewer._estimate_uvc_game_fps(samples, 120))
            continuous = [(101, 1.4)] * 120
            self.assertIsNone(everlive_viewer._estimate_uvc_game_fps(continuous, 60))
            static = [(101, 0.02)] * 120
            self.assertIsNone(everlive_viewer._estimate_uvc_game_fps(static, 60))

    def test_nvidia_telemetry_parses_gpu_utilization_and_vram_in_gib(self):
        with mock.patch("subprocess.check_output", return_value="42, 9216, 32607\n"):
            metrics = everlive_viewer._read_nvidia_metrics()
        self.assertEqual(metrics["gpu_pct"], 42)
        self.assertEqual(metrics["vram_used_gb"], 9)
        self.assertAlmostEqual(metrics["vram_total_gb"], 31.8427734375)

    @unittest.skipUnless(everlive_viewer.HAS_QT, "PySide6 is not installed")
    def test_usb_telemetry_separates_capture_and_pre_framegen_fps_in_fullscreen(self):
        streamer = mock.Mock(is_running=True, uvc_framegen=True, vdev="/dev/video4")
        hub = mock.Mock(
            streamer=streamer, _gc553_settings={"speed_mbps": 10000}, _cinema_fullscreen=True,
            _live={"locked": True, "capture_fps": 60, "filter_fps": 30,
                   "render_ms": 1.6, "vo_drops": 2},
        )
        hub.chk_osd.isChecked.return_value = True
        hub.chk_streamer.isChecked.return_value = False

        everlive_viewer.MainWindow._update_uvc_telemetry(hub, {})

        hub.lbl_fps_hero.setText.assert_called_once_with("30.0")
        hub.lbl_fps_unit.setText.assert_called_once_with("fps before LSFG")
        hub.lbl_kv_route.setText.assert_called_once_with("V4L2 (/dev/video4)")
        hub.lbl_drops_counter.setText.assert_called_once_with("2 player drops")
        streamer.show_uvc_fullscreen_stats.assert_called_once_with(
            "30.0 FPS (before LSFG)  ·  GPU render 1.60 ms")

    def test_live_hdmi_status_uses_uvc_driver_without_detaching(self):
        timing = bytearray(40)
        struct.pack_into("<HH", timing, 6, 1440, 2560)
        struct.pack_into("<H", timing, 14, 12000)
        with mock.patch.object(gc553, "find_gc553g2_device"), \
             mock.patch.object(gc553, "find_gc553g2_video_node", return_value="/dev/video4"), \
             mock.patch.object(gc553, "open_uvc_control") as libusb, \
             mock.patch("builtins.open", mock.mock_open()) as node, \
             mock.patch.object(gc553, "_read_at_reply", side_effect=[b"\x01\x00\x00\x00", timing]) as read:
            node.return_value.fileno.return_value = 42
            status = gc553.query_live_hdmi_status()

        libusb.assert_not_called()
        read.assert_has_calls([
            mock.call(None, 42, 0x38, allow_short=True),
            mock.call(None, 42, 0x37, allow_short=True),
        ])
        self.assertTrue(status["hdmi_stable"])
        self.assertEqual(status["hdmi_timing"]["refresh_hz"], 120.0)

    def test_uvc_ioctl_packs_extension_unit_and_returns_control_bytes(self):
        def ioctl(fd, request, query):
            self.assertEqual((fd, request), (42, gc553._UVCIOC_CTRL_QUERY))
            self.assertEqual((query.unit, query.selector, query.query, query.size), (4, 2, 0x85, 2))
            query.data[0], query.data[1] = 52, 0

        with mock.patch.object(gc553.fcntl, "ioctl", side_effect=ioctl):
            self.assertEqual(gc553._read_uvc_control(None, 42, 2, 0x85, 2), b"4\x00")

    def test_empty_settings_replies_preserve_live_hdmi_status(self):
        for empty_command in (0x4E, 0x56, 0x76, 0x72):
            def exchange(usb, handle, packet):
                command = int.from_bytes(packet[4:8], "little")
                if command == empty_command:
                    return bytes.fromhex("a1 00 5f")
                payload = {0x38: bytes(4), 0x77: b"test-firmware\x00",
                           0xB1: bytes(132)}.get(command, b"\x01")
                length = len(payload) + 1
                raw = bytes([0xA1, 0x80 | (length >> 8), length & 255, 0]) + payload
                return raw + bytes([(-sum(raw)) & 255])

            with self.subTest(command=hex(empty_command)), \
                 mock.patch.object(gc553, "find_gc553g2_device"), \
                 mock.patch.object(gc553, "find_gc553g2_video_node", return_value="/dev/video4"), \
                 mock.patch("builtins.open", mock.mock_open()), \
                 mock.patch.object(gc553, "_exchange_at", side_effect=exchange), \
                 mock.patch.object(gc553.time, "sleep"):
                status = gc553.query_live_hdmi_status(include_settings=True)

            self.assertFalse(status["hdmi_stable"])
            self.assertIsNone(status["hdmi_timing"])
            self.assertNotIn("settings", status)
            self.assertIn(f"AT query {empty_command:#04x}", status["settings_error"])
            self.assertIn("Missing AT response payload", status["settings_error"])

    def test_empty_short_reply_is_retried_before_reading_setting(self):
        payload = b"\x01"
        raw = bytes([0xA1, 3, 0, 0]) + payload
        valid_reply = raw + bytes([(-sum(raw)) & 255])
        with mock.patch.object(gc553, "_exchange_at", side_effect=[
                bytes.fromhex("a1 00 5f"), valid_reply]) as exchange, \
             mock.patch.object(gc553.time, "sleep"):
            self.assertEqual(gc553._read_at_reply(None, 42, 0x4E, allow_short=True), payload)
        self.assertEqual(exchange.call_count, 2)

    def test_usb_can_be_selected_alongside_pcie(self):
        pci = {"pci": "0000:01:00.0", "mode": "vfio", "model": "gc573"}
        usb = {"pci": None, "usb_port": "2-1", "mode": "uvc", "model": "gc553g2"}
        with mock.patch.dict("os.environ", {"EVERLIVE_CARD_PCI": "usb:2-1"}):
            self.assertIs(everlive_viewer.select_capture_card([pci, usb]), usb)

    def test_usb_framegen_switches_to_timed_vulkan_only_when_dependencies_exist(self):
        hw = {"mode": "uvc", "model": everlive_viewer.MODEL_GC553G2}
        streamer = everlive_viewer.GameStreamer(vdev="/dev/video4", adev="none", card=hw)
        for ready in (True, False):
            with self.subTest(ready=ready), \
                 mock.patch("os.path.exists", return_value=True), \
                 mock.patch("subprocess.run"), \
                 mock.patch("subprocess.Popen") as launch, \
                 mock.patch("shutil.which", return_value="/usr/bin/mpv"), \
                 mock.patch.object(streamer, "hdr_label", return_value=""), \
                 mock.patch.object(streamer, "get_active_format", return_value=(None, None)), \
                 mock.patch.object(everlive_viewer, "probe_lsfg_vk", return_value={
                     "layer_found": ready, "dll_found": ready, "dll_path": "/test/lsfg-vk.dll",
                 }):
                self.assertTrue(streamer.start("1920x1080 @ 60 FPS", parent_winid=84,
                                               uvc_framegen=True))
                args = launch.call_args.args[0]
                self.assertEqual(streamer.uvc_framegen, ready)
                self.assertIn("--wid=84", args)
                if ready:
                    self.assertIn("--gpu-context=x11vk", args)
                    self.assertIn("--vulkan-swap-mode=fifo", args)
                    self.assertNotIn("--opengl-swapinterval=0", args)
                    self.assertIn("--vf-add=lavfi=[mpdecimate=max=1,fps=30]", args)
                    self.assertNotIn("--untimed", args)
                    self.assertEqual(launch.call_args.kwargs["env"]["LSFGVK_MULTIPLIER"], "2")
                else:
                    self.assertIn("--gpu-context=x11egl", args)
                    self.assertIn("--untimed", args)
                    self.assertEqual(launch.call_args.kwargs["env"]["DISABLE_LSFGVK"], "1")
                    self.assertIn("missing", streamer.uvc_framegen_notice)
                with mock.patch("os.getpgid", side_effect=ProcessLookupError):
                    streamer.stop()

    def test_usb_framegen_flag_never_changes_pcie_launch(self):
        for model in ("gc573", "gc570d", "hwsuhd"):
            with self.subTest(model=model):
                streamer = everlive_viewer.GameStreamer(
                    card={"model": model, "mode": "vfio", "pci": "0000:01:00.0"})
                with mock.patch.object(streamer, "_start_vfio", return_value=True) as launch, \
                     mock.patch.object(everlive_viewer, "probe_lsfg_vk") as probe:
                    self.assertTrue(streamer.start(framegen="30to60", uvc_framegen=True))
                self.assertEqual(launch.call_args.kwargs["framegen"], "30to60")
                self.assertNotIn("uvc_framegen", launch.call_args.kwargs)
                probe.assert_not_called()

    @unittest.skipUnless(everlive_viewer.HAS_QT, "PySide6 is not installed")
    def test_vulkan_start_timeout_stops_audio_and_restarts_at_most_once(self):
        streamer = mock.Mock(is_running=True, uvc_framegen=True, uvc_framegen_deadline=10.0)
        streamer.mpv_proc.poll.return_value = None
        streamer.get_signal_info.return_value = {"locked": False, "audio": "No Audio"}
        hub = mock.Mock(streamer=streamer)

        def stop():
            streamer.is_running = False
            streamer.uvc_framegen = False
            streamer.uvc_framegen_deadline = 0.0

        def start(*, allow_uvc_framegen):
            self.assertFalse(allow_uvc_framegen)
            streamer.is_running = True

        hub._stop_stream.side_effect = stop
        hub.start_gaming_screen.side_effect = start
        with mock.patch.object(everlive_viewer.time, "monotonic", return_value=11.0):
            everlive_viewer.MainWindow._update_uvc_status(hub)
            everlive_viewer.MainWindow._update_uvc_status(hub)

        hub._stop_stream.assert_called_once_with()
        hub.start_gaming_screen.assert_called_once_with(allow_uvc_framegen=False)
        self.assertIn("no video", hub.lbl_uvc_framegen.setText.call_args.args[0])

    @unittest.skipUnless(everlive_viewer.HAS_QT, "PySide6 is not installed")
    def test_clean_exit_does_not_restart_usb_framegen(self):
        streamer = mock.Mock(is_running=True, uvc_framegen=True, uvc_framegen_deadline=10.0)
        streamer.mpv_proc.returncode = 0
        streamer.get_signal_info.return_value = {"locked": False, "audio": "No Audio"}
        hub = mock.Mock(streamer=streamer)

        def poll():
            streamer.is_running = False
            streamer.uvc_framegen = False
            streamer.uvc_framegen_deadline = 0.0
            return False

        streamer.poll.side_effect = poll
        everlive_viewer.MainWindow._update_uvc_status(hub)

        hub._set_launch_running.assert_called_once_with(False)
        hub.start_gaming_screen.assert_not_called()

    @unittest.skipUnless(everlive_viewer.HAS_QT, "PySide6 is not installed")
    def test_failed_vulkan_exit_restarts_standard_playback(self):
        streamer = mock.Mock(is_running=True, uvc_framegen=True, uvc_framegen_deadline=10.0)
        player = streamer.mpv_proc
        streamer.get_signal_info.return_value = {"locked": False, "audio": "No Audio"}
        hub = mock.Mock(streamer=streamer)

        def poll():
            player.returncode = 2
            streamer.is_running = False
            streamer.uvc_framegen = False
            streamer.uvc_framegen_deadline = 0.0
            return False

        streamer.poll.side_effect = poll
        everlive_viewer.MainWindow._update_uvc_status(hub)

        hub.start_gaming_screen.assert_called_once_with(allow_uvc_framegen=False)
        self.assertIn("exited with an error", hub.lbl_uvc_framegen.setText.call_args.args[0])

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
            self.assertIn("--swapchain-depth=1", mpv_cmd)
            self.assertIn("--opengl-swapinterval=0", mpv_cmd)
            self.assertIn("--opengl-glfinish=yes", mpv_cmd)
            self.assertFalse(any(arg.startswith("--vf-pre=") for arg in mpv_cmd))
            self.assertNotIn("pass_fds", mock_popen.call_args.kwargs)
            streamer.stop()

            ok_60 = streamer.start(preset_name="3840x2160 @ 60 FPS", vsync=True)
            self.assertTrue(ok_60)
            mpv_cmd_60 = mock_popen.call_args[0][0]
            self.assertIn("--demuxer-lavf-o-add=input_format=yuyv422", mpv_cmd_60)
            self.assertIn("--demuxer-lavf-o-add=framerate=60", mpv_cmd_60)
            self.assertIn("--opengl-swapinterval=1", mpv_cmd_60)
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
