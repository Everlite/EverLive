#!/usr/bin/env python3
"""GC553G2 (AVerMedia Live Gamer ULTRA 2.1, USB 07ca:2553) Linux control module.

Provides USB/sysfs discovery, V4L2 format parsing, AVerMedia 0xA1 AT command
packet encoding/decoding, UVC Extension Unit 4 control transfers via libusb-1.0,
and HID LampArray RGB control.

Protocol and USB/HID control sequences adapted from accola88/GC553G2-Linux-Utility
(https://github.com/accola88/GC553G2-Linux-Utility), MIT License.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import ctypes as C
import ctypes.util
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
from typing import Any, Dict, Iterator, List, Optional, Tuple


GC553G2_VID = "07ca"
GC553G2_PID = "2553"
SUPPORTED_WRITE_FIRMWARE = "1.0.10.2"

SYS_USB_DEFAULT = Path("/sys/bus/usb/devices")
SYS_VIDEO_DEFAULT = Path("/sys/class/video4linux")
SYS_HIDRAW_DEFAULT = Path("/sys/class/hidraw")

EDID_MODES: Dict[str, int] = {"merge": 4, "copy": 1, "internal": 0}
EDID_NAMES: Dict[int, str] = {code: name for name, code in EDID_MODES.items()}

HDMI_PRESETS: Dict[str, int] = {"2.1-144": 0, "2.1-120": 1, "2.0": 2}
HDMI_PRESET_NAMES: Dict[int, str] = {
    **{code + offset: name for name, code in HDMI_PRESETS.items() for offset in (0, 4)},
    3: "1.4",
}

LAMP_DESCRIPTOR_SHA256 = (
    "82475fa450cf24b689ecebcf0b342470412c19ac4f1970ff6a4fad66840b6d39"
)

_LIBUSB_NO_DEVICE = -4


def _read_sysfs_text(device: Path, name: str) -> str:
    return (device / name).read_text(encoding="utf-8", errors="ignore").strip()


# ---------------------------------------------------------------------------
# Sysfs USB & V4L2 discovery
# ---------------------------------------------------------------------------

def find_gc553g2_devices(
    port: Optional[str] = None,
    sys_usb: Path = SYS_USB_DEFAULT,
) -> List[Path]:
    """Return sorted sysfs paths for connected GC553G2 (07ca:2553) USB devices."""
    if not sys_usb.is_dir():
        return []
    matches: List[Path] = []
    for device in sys_usb.iterdir():
        if port and device.name != port:
            continue
        try:
            vid = _read_sysfs_text(device, "idVendor").lower()
            pid = _read_sysfs_text(device, "idProduct").lower()
            if (vid, pid) == (GC553G2_VID, GC553G2_PID):
                matches.append(device)
        except OSError:
            continue
    return sorted(matches, key=lambda p: p.name)


def find_gc553g2_device(
    port: Optional[str] = None,
    sys_usb: Path = SYS_USB_DEFAULT,
) -> Path:
    """Return the single matching GC553G2 sysfs device or raise RuntimeError."""
    devices = find_gc553g2_devices(port=port, sys_usb=sys_usb)
    if not devices:
        raise RuntimeError("No GC553G2 (07ca:2553) USB device detected.")
    if len(devices) > 1:
        ports = ", ".join(d.name for d in devices)
        raise RuntimeError(
            f"Multiple GC553G2 devices detected ({ports}); select a USB port."
        )
    return devices[0]


def find_gc553g2_video_node(
    device: Path,
    sys_video: Path = SYS_VIDEO_DEFAULT,
) -> Optional[str]:
    """Return the primary /dev/videoN capture node (index 0) for a GC553G2."""
    if not sys_video.is_dir():
        return None
    usb_root = device.resolve()
    candidates: List[Path] = []
    for node in sorted(sys_video.iterdir(), key=lambda p: p.name):
        if not node.name.startswith("video"):
            continue
        try:
            if _read_sysfs_text(node, "index") != "0":
                continue
            target = (node / "device").resolve()
        except OSError:
            continue
        if usb_root == target or usb_root in target.parents:
            candidates.append(node)
    if not candidates:
        return None
    return f"/dev/{candidates[0].name}"


def read_usb_identity(
    device: Path,
    sys_video: Path = SYS_VIDEO_DEFAULT,
) -> Dict[str, Any]:
    """Read sysfs USB identity, link speed, serial, and /dev/video* node."""
    def _opt_int(name: str) -> Optional[int]:
        try:
            return int(_read_sysfs_text(device, name))
        except (OSError, ValueError):
            return None

    def _opt_str(name: str) -> str:
        try:
            return _read_sysfs_text(device, name)
        except OSError:
            return ""

    speed_mbps = _opt_int("speed")
    return {
        "port": device.name,
        "bus": _opt_int("busnum"),
        "address": _opt_int("devnum"),
        "speed_mbps": speed_mbps,
        "superspeed_10g": speed_mbps is not None and speed_mbps >= 10000,
        "serial": _opt_str("serial"),
        "product": _opt_str("product") or "Live Gamer ULTRA 2.1",
        "video_node": find_gc553g2_video_node(device, sys_video=sys_video),
    }


# ---------------------------------------------------------------------------
# V4L2 format parsing
# ---------------------------------------------------------------------------

def parse_v4l2_formats(output: str) -> List[Dict[str, Any]]:
    """Parse `v4l2-ctl --list-formats-ext` output into structured mode entries."""
    modes: List[Dict[str, Any]] = []
    fmt: Optional[str] = None
    size: Optional[Tuple[int, int]] = None
    current_fps: List[float] = []

    def _flush() -> None:
        if fmt and size and current_fps:
            modes.append(
                {
                    "format": fmt,
                    "width": size[0],
                    "height": size[1],
                    "fps": sorted(set(current_fps)),
                }
            )

    for line in output.splitlines():
        fmt_match = re.search(r"'\s*([A-Za-z0-9]{4})\s*'", line)
        if fmt_match and "[" in line:
            _flush()
            fmt = fmt_match.group(1)
            size = None
            current_fps = []
            continue
        size_match = re.search(r"Size:\s+Discrete\s+(\d+)x(\d+)", line)
        if size_match:
            _flush()
            size = (int(size_match.group(1)), int(size_match.group(2)))
            current_fps = []
            continue
        fps_match = re.search(r"\((\d+(?:\.\d+)?)\s*fps\)", line)
        if fps_match and fmt and size:
            current_fps.append(float(fps_match.group(1)))
    _flush()
    return modes


def query_v4l2_formats(video_node: str) -> List[Dict[str, Any]]:
    """Run `v4l2-ctl --list-formats-ext` on `video_node` and return parsed modes."""
    try:
        out = subprocess.check_output(
            ["v4l2-ctl", "-d", video_node, "--list-formats-ext"],
            stderr=subprocess.STDOUT,
            text=True,
            timeout=3.0,
        )
        return parse_v4l2_formats(out)
    except (OSError, subprocess.SubprocessError):
        return []


def reported_4k_mjpeg_rates(modes: List[Dict[str, Any]]) -> List[int]:
    """Return sorted integer frame rates advertised for 3840x2160 MJPG."""
    rates = {
        round(fps)
        for m in modes
        if m.get("format") == "MJPG" and (m.get("width"), m.get("height")) == (3840, 2160)
        for fps in m.get("fps", ())
    }
    return sorted(rates)


def has_high_fps_4k_mjpeg(modes: List[Dict[str, Any]]) -> bool:
    """Return True when 4K 120/144 Hz MJPEG modes are advertised."""
    return bool(set(reported_4k_mjpeg_rates(modes)) & {120, 144})


# ---------------------------------------------------------------------------
# Pure-Python AVerMedia 0xA1 AT packet protocol
# ---------------------------------------------------------------------------

def build_at_packet(command: int, data: bytes = b"") -> bytes:
    """Encode an AVerMedia 0xA1 AT command packet with two's-complement checksum."""
    body = command.to_bytes(4, "little") + data
    if len(body) < 127:
        header = bytes([0xA1, (len(body) + 2) & 127, 0, 0])
    else:
        length = len(body) + 1
        header = bytes([0xA1, 0x80 | (length >> 8), length & 255, 0])
    payload = header + body
    return payload + bytes([(-sum(payload)) & 255])


def build_descriptor_switch_packet(index: int) -> bytes:
    """Encode AT 0xB0 command to toggle USB descriptor index (0=60Hz, 1=120/144Hz)."""
    if type(index) is not int or index not in (0, 1):
        raise ValueError("Descriptor index must be 0 or 1")
    data = (1).to_bytes(4, "little") + index.to_bytes(4, "little")
    return build_at_packet(0xB0, data)


def decode_at_response(raw: bytes, *, allow_short: bool = False) -> bytes:
    """Validate checksum/header of an 0xA1 AT response and return its payload."""
    if len(raw) < 3 or raw[0] != 0xA1 or (sum(raw) & 255) != 0:
        raise RuntimeError("Invalid AT response header or checksum")
    if raw[1] & 0x80:
        if len(raw) < 5 or raw[3] != 0:
            raise RuntimeError("Invalid extended AT response header")
        encoded_length = ((raw[1] & 0x7F) << 8) | raw[2]
        if encoded_length != len(raw) - 4:
            raise RuntimeError("Extended AT response length mismatch")
        return raw[4:-1]
    if not allow_short:
        raise RuntimeError("Unexpected short AT response format")
    if raw[1] != len(raw) - 3:
        raise RuntimeError("Short AT response length mismatch")
    if raw[1] == 0:
        return b""
    if len(raw) < 5 or raw[2:4] != b"\x00\x00":
        raise RuntimeError("Invalid short AT response header")
    return raw[4:-1]


def decode_hdmi_timing(data: bytes) -> Dict[str, Any]:
    """Decode the 40-byte AT 0x37 HDMI input timing payload."""
    if len(data) < 40:
        raise RuntimeError("Incomplete HDMI input timing response")
    interlaced = bool(data[0] & 1)
    height, width = struct.unpack_from("<HH", data, 6)
    refresh_hz = struct.unpack_from("<H", data, 14)[0] / 100.0
    if not (width and height and refresh_hz):
        raise RuntimeError("HDMI input timing is not available")
    return {
        "width": width,
        "height": height * 2 if interlaced else height,
        "refresh_hz": refresh_hz,
        "interlaced": interlaced,
    }


# ---------------------------------------------------------------------------
# libusb-1.0 UVC Extension Unit 4 transport
# ---------------------------------------------------------------------------

def _load_libusb():
    path = ctypes.util.find_library("usb-1.0")
    if not path:
        raise RuntimeError("libusb-1.0 shared library not found")
    usb = C.CDLL(path)
    signatures = {
        "init": ([C.POINTER(C.c_void_p)], C.c_int),
        "exit": ([C.c_void_p], None),
        "close": ([C.c_void_p], None),
        "wrap_sys_device": ([C.c_void_p, C.c_ssize_t, C.POINTER(C.c_void_p)], C.c_int),
        "kernel_driver_active": ([C.c_void_p, C.c_int], C.c_int),
        "detach_kernel_driver": ([C.c_void_p, C.c_int], C.c_int),
        "attach_kernel_driver": ([C.c_void_p, C.c_int], C.c_int),
        "claim_interface": ([C.c_void_p, C.c_int], C.c_int),
        "release_interface": ([C.c_void_p, C.c_int], C.c_int),
        "control_transfer": (
            [
                C.c_void_p,
                C.c_uint8,
                C.c_uint8,
                C.c_uint16,
                C.c_uint16,
                C.POINTER(C.c_ubyte),
                C.c_uint16,
                C.c_uint,
            ],
            C.c_int,
        ),
        "error_name": ([C.c_int], C.c_char_p),
    }
    for name, (args, restype) in signatures.items():
        fn = getattr(usb, f"libusb_{name}")
        fn.argtypes, fn.restype = args, restype
    return usb


def _usb_check(usb, code: int) -> int:
    if code < 0:
        raise RuntimeError(usb.libusb_error_name(code).decode("ascii", "replace"))
    return code


class _UvcXuQuery(C.Structure):
    _fields_ = [
        ("unit", C.c_uint8), ("selector", C.c_uint8), ("query", C.c_uint8),
        ("size", C.c_uint16), ("data", C.POINTER(C.c_ubyte)),
    ]


# Linux _IOWR('u', 0x21, struct uvc_xu_control_query).
_UVCIOC_CTRL_QUERY = (3 << 30) | (C.sizeof(_UvcXuQuery) << 16) | (ord("u") << 8) | 0x21


def _query_uvc_driver(fd: int, selector: int, operation: int, content: bytes) -> bytes:
    buf = (C.c_ubyte * len(content)).from_buffer_copy(content)
    query = _UvcXuQuery(4, selector, operation, len(buf), buf)
    try:
        fcntl.ioctl(fd, _UVCIOC_CTRL_QUERY, query)
    except OSError as exc:
        raise RuntimeError(f"UVC extension unit query failed: {exc.strerror}") from exc
    return bytes(buf)


def _read_uvc_control(usb, handle, selector: int, operation: int, length: int) -> bytes:
    if operation not in (0x81, 0x85, 0x86) or not 1 <= length <= 4096:
        raise ValueError("Only bounded UVC GET requests are allowed")
    if usb is None:
        return _query_uvc_driver(handle, selector, operation, bytes(length))
    buf = (C.c_ubyte * length)()
    count = _usb_check(
        usb,
        usb.libusb_control_transfer(
            handle, 0xA1, operation, selector << 8, 4 << 8, buf, length, 1000
        ),
    )
    if count != length:
        raise RuntimeError(f"Short UVC reply: {count} of {length} bytes")
    return bytes(buf)


def _write_uvc_control(usb, handle, selector: int, content: bytes) -> None:
    if usb is None:
        _query_uvc_driver(handle, selector, 0x01, content)
        return
    buf = (C.c_ubyte * len(content)).from_buffer_copy(content)
    count = _usb_check(
        usb,
        usb.libusb_control_transfer(
            handle, 0x21, 0x01, selector << 8, 4 << 8, buf, len(content), 1000
        ),
    )
    if count != len(content):
        raise RuntimeError(f"Incomplete UVC transfer: {count}/{len(content)} bytes")


@contextmanager
def open_uvc_control(
    device: Path,
    sys_video: Path = SYS_VIDEO_DEFAULT,
) -> Iterator[Tuple[Any, Any]]:
    """Temporarily detach uvcvideo interface 0, claim XU 4, and restore on exit."""
    usb = _load_libusb()
    info = read_usb_identity(device, sys_video=sys_video)
    if info["bus"] is None or info["address"] is None:
        raise RuntimeError("Missing USB bus/address in sysfs for GC553G2")
    dev_path = f"/dev/bus/usb/{info['bus']:03d}/{info['address']:03d}"
    ctx = C.c_void_p()
    handle = C.c_void_p()

    with open(dev_path, "r+b", buffering=0) as usb_file:
        descriptor = usb_file.read(18)
        if len(descriptor) != 18 or descriptor[8:12] != bytes.fromhex("ca 07 53 25"):
            raise RuntimeError("USB device identity mismatch while opening GC553G2")
        _usb_check(usb, usb.libusb_init(C.byref(ctx)))
        claimed = False
        detached = False
        active: Dict[int, int] = {}
        try:
            _usb_check(usb, usb.libusb_wrap_sys_device(ctx, usb_file.fileno(), C.byref(handle)))
            active = {
                i: _usb_check(usb, usb.libusb_kernel_driver_active(handle, i))
                for i in (0, 1)
            }
            if active[0]:
                _usb_check(usb, usb.libusb_detach_kernel_driver(handle, 0))
                detached = True
            _usb_check(usb, usb.libusb_claim_interface(handle, 0))
            claimed = True
            yield usb, handle
        finally:
            try:
                if claimed:
                    code = usb.libusb_release_interface(handle, 0)
                    if code != _LIBUSB_NO_DEVICE:
                        _usb_check(usb, code)
            finally:
                try:
                    if detached:
                        for iface in (0, 1):
                            state = usb.libusb_kernel_driver_active(handle, iface)
                            if state == _LIBUSB_NO_DEVICE:
                                break
                            if active.get(iface) and _usb_check(usb, state) == 0:
                                code = usb.libusb_attach_kernel_driver(handle, iface)
                                if code != _LIBUSB_NO_DEVICE:
                                    _usb_check(usb, code)
                finally:
                    if handle:
                        usb.libusb_close(handle)
                    usb.libusb_exit(ctx)


def _exchange_at(usb, handle, payload: bytes) -> bytes:
    _write_uvc_control(usb, handle, 2, len(payload).to_bytes(2, "little"))
    _write_uvc_control(usb, handle, 1, payload)
    time.sleep(0.05)
    status_len = int.from_bytes(_read_uvc_control(usb, handle, 2, 0x85, 2), "little")
    _read_uvc_control(usb, handle, 2, 0x81, status_len)
    resp_len = int.from_bytes(_read_uvc_control(usb, handle, 1, 0x85, 2), "little")
    return _read_uvc_control(usb, handle, 1, 0x81, resp_len)


def _read_at_reply(usb, handle, command: int, *, allow_short: bool = False) -> bytes:
    last_error: Optional[Exception] = None
    for delay in (0.0, 0.15, 0.35):
        if delay:
            time.sleep(delay)
        try:
            raw = _exchange_at(usb, handle, build_at_packet(command))
            data = decode_at_response(raw, allow_short=allow_short)
            if not data:
                raise RuntimeError("Missing AT response payload")
            return data
        except RuntimeError as exc:
            last_error = exc
    raise RuntimeError(f"AT query {command:#04x}: {last_error}")


def _write_at_setting(
    usb,
    handle,
    command: int,
    data: bytes,
    expected_echoes: Tuple[bytes, ...],
) -> None:
    raw = _exchange_at(usb, handle, build_at_packet(command, data))
    resp = decode_at_response(raw, allow_short=True)
    if resp != b"" and resp not in expected_echoes:
        raise RuntimeError(
            f"AT write {command:#04x} acknowledgement mismatch: {resp.hex(' ')}"
        )


def _require_supported_firmware(usb, handle) -> str:
    fw = _read_at_reply(usb, handle, 0x77).split(b"\x00", 1)[0].decode("ascii", "replace")
    if fw != SUPPORTED_WRITE_FIRMWARE:
        raise RuntimeError(
            f"Write operations are validated for firmware {SUPPORTED_WRITE_FIRMWARE}, found {fw}"
        )
    return fw


# ---------------------------------------------------------------------------
# High-level device queries & controls
# ---------------------------------------------------------------------------

def _read_hdmi_status(usb, handle) -> Dict[str, Any]:
    data = _read_at_reply(usb, handle, 0x38, allow_short=True)
    if len(data) < 4:
        raise RuntimeError("Incomplete HDMI input lock response")
    stable = int.from_bytes(data[:4], "little") == 1
    timing = (
        decode_hdmi_timing(_read_at_reply(usb, handle, 0x37, allow_short=True))
        if stable else None
    )
    return {"hdmi_stable": stable, "hdmi_timing": timing}


def _read_at_settings(usb, handle) -> Dict[str, Any]:
    fw = _read_at_reply(usb, handle, 0x77).split(b"\x00", 1)[0].decode("ascii", "replace")
    idx_data = _read_at_reply(usb, handle, 0xB1)
    desc_index = idx_data[4] if len(idx_data) == 132 and idx_data[:4] == b"\x01\x00\x00\x00" else None
    edid_code = _read_at_reply(usb, handle, 0x4E, allow_short=True)[0]
    preset_code = _read_at_reply(usb, handle, 0x56, allow_short=True)[0]
    return {
        "firmware": fw,
        "descriptor_index": desc_index,
        "edid_mode": EDID_NAMES.get(edid_code, f"unknown({edid_code})"),
        "hdmi_preset": HDMI_PRESET_NAMES.get(preset_code, f"unknown({preset_code})"),
        "hdcp_detection": bool(_read_at_reply(usb, handle, 0x76, allow_short=True)[0]),
        "hdr_to_sdr": bool(_read_at_reply(usb, handle, 0x72, allow_short=True)[0]),
    }


def query_live_hdmi_status(
    port: Optional[str] = None,
    *,
    include_settings: bool = False,
    sys_usb: Path = SYS_USB_DEFAULT,
    sys_video: Path = SYS_VIDEO_DEFAULT,
) -> Dict[str, Any]:
    """Read HDMI lock and timing through uvcvideo without detaching the driver."""
    device = find_gc553g2_device(port=port, sys_usb=sys_usb)
    node = find_gc553g2_video_node(device, sys_video=sys_video)
    if not node:
        raise RuntimeError("GC553G2 capture node unavailable")
    with open(node, "r+b", buffering=0) as video:
        fd = video.fileno()
        status = _read_hdmi_status(None, fd)
        if include_settings:
            try:
                status["settings"] = _read_at_settings(None, fd)
            except (RuntimeError, OSError) as exc:
                status["settings_error"] = str(exc)
        return status


def query_device_status(
    port: Optional[str] = None,
    *,
    include_at: bool = True,
    sys_usb: Path = SYS_USB_DEFAULT,
    sys_video: Path = SYS_VIDEO_DEFAULT,
) -> Dict[str, Any]:
    """Return GC553G2 USB/V4L2 status and (optionally) AT HDMI/EDID telemetry."""
    device = find_gc553g2_device(port=port, sys_usb=sys_usb)
    info = read_usb_identity(device, sys_video=sys_video)
    modes = query_v4l2_formats(info["video_node"]) if info.get("video_node") else []
    info["modes"] = modes
    info["mjpeg_4k_rates"] = reported_4k_mjpeg_rates(modes)
    info["high_fps_4k_ready"] = has_high_fps_4k_mjpeg(modes)

    if not include_at:
        return info

    try:
        with open_uvc_control(device, sys_video=sys_video) as (usb, handle):
            info.update(_read_hdmi_status(usb, handle))
            info.update(_read_at_settings(usb, handle))
    except Exception as exc:
        info["at_error"] = str(exc)
    return info


def set_edid_mode(
    mode: str,
    port: Optional[str] = None,
    sys_usb: Path = SYS_USB_DEFAULT,
    sys_video: Path = SYS_VIDEO_DEFAULT,
) -> Dict[str, Any]:
    """Set GC553G2 EDID mode ('merge', 'copy', or 'internal')."""
    if mode not in EDID_MODES:
        raise ValueError(f"Unsupported EDID mode '{mode}'. Valid: {list(EDID_MODES)}")
    device = find_gc553g2_device(port=port, sys_usb=sys_usb)
    with open_uvc_control(device, sys_video=sys_video) as (usb, handle):
        _require_supported_firmware(usb, handle)
        mode_data = EDID_MODES[mode].to_bytes(4, "little")
        _write_at_setting(usb, handle, 0x4D, mode_data, (mode_data,))
        time.sleep(0.2)
        readback = _read_at_reply(usb, handle, 0x4E, allow_short=True)[0]
        if readback != EDID_MODES[mode]:
            raise RuntimeError("EDID mode readback differs from requested mode")
    return {"edid_mode": mode, "changed": True}


def set_hdmi_preset(
    preset: str,
    port: Optional[str] = None,
    sys_usb: Path = SYS_USB_DEFAULT,
    sys_video: Path = SYS_VIDEO_DEFAULT,
) -> Dict[str, Any]:
    """Set GC553G2 HDMI version preset ('2.1-144', '2.1-120', or '2.0')."""
    if preset not in HDMI_PRESETS:
        raise ValueError(f"Unsupported HDMI preset '{preset}'. Valid: {list(HDMI_PRESETS)}")
    target_code = HDMI_PRESETS[preset]
    device = find_gc553g2_device(port=port, sys_usb=sys_usb)
    with open_uvc_control(device, sys_video=sys_video) as (usb, handle):
        _require_supported_firmware(usb, handle)
        data = bytes([target_code])
        _write_at_setting(usb, handle, 0x55, data, (data, target_code.to_bytes(4, "little")))
        time.sleep(0.2)
        readback = _read_at_reply(usb, handle, 0x56, allow_short=True)[0]
        if HDMI_PRESET_NAMES.get(readback) != preset:
            raise RuntimeError("HDMI preset readback differs from requested preset")
    return {"hdmi_preset": preset, "changed": True}


def set_hdcp_detection(
    enabled: bool,
    port: Optional[str] = None,
    sys_usb: Path = SYS_USB_DEFAULT,
    sys_video: Path = SYS_VIDEO_DEFAULT,
) -> Dict[str, Any]:
    """Enable or disable GC553G2 HDCP detection."""
    device = find_gc553g2_device(port=port, sys_usb=sys_usb)
    with open_uvc_control(device, sys_video=sys_video) as (usb, handle):
        _require_supported_firmware(usb, handle)
        data = int(bool(enabled)).to_bytes(4, "little")
        _write_at_setting(usb, handle, 0x03, data, (data, data[:1]))
        time.sleep(0.2)
        actual = bool(_read_at_reply(usb, handle, 0x76, allow_short=True)[0])
        if actual != bool(enabled):
            raise RuntimeError("HDCP detection readback differs from requested state")
    return {"hdcp_detection": bool(enabled), "changed": True}


def set_hdr_to_sdr(
    enabled: bool,
    port: Optional[str] = None,
    sys_usb: Path = SYS_USB_DEFAULT,
    sys_video: Path = SYS_VIDEO_DEFAULT,
) -> Dict[str, Any]:
    """Enable or disable GC553G2 hardware HDR-to-SDR tone mapping."""
    device = find_gc553g2_device(port=port, sys_usb=sys_usb)
    with open_uvc_control(device, sys_video=sys_video) as (usb, handle):
        _require_supported_firmware(usb, handle)
        data = bytes([int(bool(enabled))])
        _write_at_setting(usb, handle, 0x1F, data, (data, int(bool(enabled)).to_bytes(4, "little")))
        time.sleep(0.2)
        actual = bool(_read_at_reply(usb, handle, 0x72, allow_short=True)[0])
        if actual != bool(enabled):
            raise RuntimeError("HDR-to-SDR readback differs from requested state")
    return {"hdr_to_sdr": bool(enabled), "changed": True}


def set_descriptor_index(
    index: int,
    port: Optional[str] = None,
    sys_usb: Path = SYS_USB_DEFAULT,
    sys_video: Path = SYS_VIDEO_DEFAULT,
) -> Dict[str, Any]:
    """Switch USB descriptor index (1 enables 4K120/144 MJPEG, 0 restores 60Hz)."""
    payload = build_descriptor_switch_packet(index)
    device = find_gc553g2_device(port=port, sys_usb=sys_usb)
    before = read_usb_identity(device, sys_video=sys_video)
    with open_uvc_control(device, sys_video=sys_video) as (usb, handle):
        _require_supported_firmware(usb, handle)
        old_data = _read_at_reply(usb, handle, 0xB1)
        old_index = old_data[4] if len(old_data) == 132 else None
        if old_index != index:
            _write_uvc_control(usb, handle, 2, len(payload).to_bytes(2, "little"))
            _write_uvc_control(usb, handle, 1, payload)
            time.sleep(0.2)
            try:
                status_len = int.from_bytes(_read_uvc_control(usb, handle, 2, 0x85, 2), "little")
                _read_uvc_control(usb, handle, 2, 0x81, status_len)
                resp_len = int.from_bytes(_read_uvc_control(usb, handle, 1, 0x85, 2), "little")
                reply = _read_uvc_control(usb, handle, 1, 0x81, resp_len)
                if reply != bytes.fromhex("a1 00 5f"):
                    raise RuntimeError(f"Unexpected mode switch reply: {reply.hex(' ')}")
            except RuntimeError as exc:
                if "LIBUSB_ERROR_NO_DEVICE" not in str(exc):
                    raise

    # Wait up to 12 seconds for USB re-enumeration if changed.
    deadline = time.monotonic() + 12.0
    while time.monotonic() < deadline:
        try:
            cur = find_gc553g2_device(port=port, sys_usb=sys_usb)
            cur_info = read_usb_identity(cur, sys_video=sys_video)
            if cur_info.get("video_node") and (
                old_index == index or cur_info.get("address") != before.get("address")
            ):
                modes = query_v4l2_formats(cur_info["video_node"])
                return {
                    "descriptor_index": index,
                    "changed": old_index != index,
                    "video_node": cur_info["video_node"],
                    "mjpeg_4k_rates": reported_4k_mjpeg_rates(modes),
                    "high_fps_4k_ready": has_high_fps_4k_mjpeg(modes),
                }
        except (RuntimeError, OSError):
            pass
        time.sleep(0.25)

    return {
        "descriptor_index": index,
        "changed": old_index != index,
        "video_node": before.get("video_node"),
        "reconnect_required": True,
    }


# ---------------------------------------------------------------------------
# HID LampArray RGB control
# ---------------------------------------------------------------------------

def find_lamp_hidraw(
    device: Path,
    sys_hidraw: Path = SYS_HIDRAW_DEFAULT,
) -> Optional[Path]:
    """Find the hidraw sysfs node for the GC553G2 HID LampArray interface."""
    if not sys_hidraw.is_dir():
        return None
    serial = _read_sysfs_text(device, "serial")
    if not serial:
        return None
    for node in sorted(sys_hidraw.glob("hidraw*")):
        hid = node / "device"
        try:
            if not hid.resolve().name.startswith("0003:07CA:D553."):
                continue
            attrs = dict(
                line.split("=", 1)
                for line in (hid / "uevent").read_text(encoding="utf-8", errors="ignore").splitlines()
                if "=" in line
            )
            if attrs.get("HID_UNIQ") != serial:
                continue
            descriptor = (hid / "report_descriptor").read_bytes()
            if hashlib.sha256(descriptor).hexdigest() == LAMP_DESCRIPTOR_SHA256:
                return node
        except OSError:
            continue
    return None


def _hid_feature_report(file_obj, report: bytes, *, read: bool = False) -> bytearray:
    buf = bytearray(report)
    request = 0xC0000000 | (len(buf) << 16) | (ord("H") << 8) | (0x07 if read else 0x06)
    count = fcntl.ioctl(file_obj, request, buf, True)
    if count != len(buf) or buf[0] != report[0]:
        raise RuntimeError(f"Incomplete HID feature report {report[0]}: {count}/{len(buf)}")
    return buf


def set_rgb_mode(
    mode: str,
    brightness: int = 100,
    port: Optional[str] = None,
    sys_usb: Path = SYS_USB_DEFAULT,
    sys_hidraw: Path = SYS_HIDRAW_DEFAULT,
) -> Dict[str, Any]:
    """Control GC553G2 RGB LEDs ('autonomous' or '#RRGGBB')."""
    if mode != "autonomous" and re.fullmatch(r"#?[0-9a-fA-F]{6}", mode) is None:
        raise ValueError("RGB mode must be 'autonomous' or a 6-digit hex color (#RRGGBB)")
    if not 0 <= brightness <= 100:
        raise ValueError("Brightness must be between 0 and 100")

    device = find_gc553g2_device(port=port, sys_usb=sys_usb)
    node = find_lamp_hidraw(device, sys_hidraw=sys_hidraw)
    if node is None:
        raise RuntimeError("GC553G2 HID LampArray interface not found")

    dev_path = Path("/dev") / node.name
    with dev_path.open("r+b", buffering=0) as fh:
        fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
        attrs = _hid_feature_report(fh, b"\x01" + bytes(22), read=True)
        count, _, _, _, _, delay_us = struct.unpack_from("<HIIIII", attrs, 1)
        if not 1 <= count <= 256:
            raise RuntimeError(f"Unexpected LampArray count: {count}")
        if mode == "autonomous":
            _hid_feature_report(fh, b"\x06\x01")
            return {"hidraw": str(dev_path), "autonomous": True}

        rgb = bytes.fromhex(mode.lstrip("#"))
        lamps = []
        for idx in range(count):
            _hid_feature_report(fh, struct.pack("<BH", 2, idx))
            rep = _hid_feature_report(fh, b"\x03" + bytes(28), read=True)
            lamps.append(tuple(rep[23:28]))
        if not any(lamp[-1] for lamp in lamps):
            raise RuntimeError("LampArray has no programmable LEDs")
        _hid_feature_report(fh, b"\x06\x00")
        for start in range(0, len(lamps), 10):
            group = lamps[start : start + 10]
            updates = bytearray()
            for r_max, g_max, b_max, i_max, prog in group:
                levels = (r_max, g_max, b_max) if prog else (0, 0, 0)
                chans = [(c * m * brightness + 12_750) // 25_500 for c, m in zip(rgb, levels)]
                updates.extend(chans)
                updates.append(i_max if any(chans) else 0)
            finish = start + len(group)
            ids = list(range(start, finish)) + [0] * (10 - len(group))
            header = struct.pack("<BBB10H", 4, len(group), finish == len(lamps), *ids)
            if start and delay_us:
                time.sleep(delay_us / 1_000_000)
            _hid_feature_report(fh, header + updates.ljust(40, b"\x00"))
    return {"hidraw": str(dev_path), "autonomous": False, "rgb": "#" + rgb.hex()}


# ---------------------------------------------------------------------------
# Standalone CLI
# ---------------------------------------------------------------------------

def _build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="gc553g2_control.py",
        description="GC553G2 (AVerMedia Live Gamer ULTRA 2.1, USB 07ca:2553) control CLI.",
    )
    parser.add_argument("--port", default=None, help="Target USB port (e.g. 2-1.4)")
    parser.add_argument("--json", action="store_true", help="Emit JSON output")
    sub = parser.add_subparsers(dest="command", required=True)

    p_status = sub.add_parser("status", help="Query USB, V4L2, and AT HDMI status")
    p_status.add_argument("--no-at", action="store_true", help="Skip AT command probe")

    sub.add_parser("high-fps", help="Enable USB descriptor index 1 (4K120/144 MJPEG)")
    sub.add_parser("default-fps", help="Restore USB descriptor index 0 (60 Hz default)")

    p_edid = sub.add_parser("edid", help="Set EDID mode")
    p_edid.add_argument("mode", choices=sorted(EDID_MODES))

    p_preset = sub.add_parser("hdmi-preset", help="Set HDMI bandwidth preset")
    p_preset.add_argument("preset", choices=list(HDMI_PRESETS))

    p_hdcp = sub.add_parser("hdcp", help="Toggle HDCP detection")
    p_hdcp.add_argument("state", choices=["on", "off"])

    p_hdr = sub.add_parser("hdr-to-sdr", help="Toggle hardware HDR-to-SDR tonemapping")
    p_hdr.add_argument("state", choices=["on", "off"])

    p_rgb = sub.add_parser("rgb", help="Control HID LampArray RGB lighting")
    p_rgb.add_argument("mode", help="'autonomous' or hex colour '#RRGGBB'")
    p_rgb.add_argument("--brightness", type=int, default=100, help="Brightness (0..100)")
    return parser


def _format_cli_summary(cmd: str, result: Dict[str, Any]) -> str:
    if cmd == "status":
        speed = result.get("speed_mbps") or "?"
        rates = result.get("mjpeg_4k_rates") or []
        parts = [
            f"port={result.get('port', '?')}",
            f"video={result.get('video_node') or 'none'}",
            f"usb={speed}Mbps",
            f"4k_mjpeg={rates}",
        ]
        if result.get("firmware"):
            parts.append(f"fw={result['firmware']}")
        if result.get("hdmi_timing"):
            t = result["hdmi_timing"]
            parts.append(f"hdmi={t['width']}x{t['height']}@{t['refresh_hz']:.0f}Hz")
        if result.get("at_error"):
            parts.append(f"at_error={result['at_error']}")
        return " ".join(parts)
    return " ".join(f"{k}={v}" for k, v in result.items())


def main(argv: Optional[List[str]] = None) -> int:
    """CLI entry point for querying and configuring GC553G2 devices."""
    parser = _build_arg_parser()
    args = parser.parse_args(argv)
    try:
        if args.command == "status":
            result = query_device_status(port=args.port, include_at=not args.no_at)
        elif args.command == "high-fps":
            result = set_descriptor_index(1, port=args.port)
        elif args.command == "default-fps":
            result = set_descriptor_index(0, port=args.port)
        elif args.command == "edid":
            result = set_edid_mode(args.mode, port=args.port)
        elif args.command == "hdmi-preset":
            result = set_hdmi_preset(args.preset, port=args.port)
        elif args.command == "hdcp":
            result = set_hdcp_detection(args.state == "on", port=args.port)
        elif args.command == "hdr-to-sdr":
            result = set_hdr_to_sdr(args.state == "on", port=args.port)
        elif args.command == "rgb":
            result = set_rgb_mode(args.mode, brightness=args.brightness, port=args.port)
        else:
            parser.error(f"Unknown command: {args.command}")
            return 2
    except Exception as exc:
        msg = str(exc)
        if isinstance(exc, PermissionError) or "LIBUSB_ERROR_ACCESS" in msg:
            print(
                "[!] USB/HID permission denied for GC553G2 (07ca:2553). "
                "Run 'sudo ./install.sh' to install /etc/udev/rules.d/70-everlive.rules.",
                file=sys.stderr,
            )
        else:
            print(f"[!] GC553G2 error: {msg}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(result, indent=2, default=str))
    else:
        print(_format_cli_summary(args.command, result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
