#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
EverLive control-socket client.

The userspace streamer owns the VFIO container and the BAR0 mapping
exclusively, so the Control Hub can never open the card while a session is
running.  Instead of refusing to start, the hub attaches to the running engine
through its Unix socket and mirrors the engine's own view of the hardware.

Phase 1 is read-only (``hello`` / ``ping`` / ``status.get``); Phase 2 adds the
live mutations (LED, audio gain/mute, OSD, aspect), which the streamer applies
from a lock-free command ring on its render thread.  Two entry points are
provided:

``probe()``
    A blocking, Qt-free liveness check.  Use it where no event loop is running
    yet, for example right before deciding whether to launch an engine.

``EverLiveIpcClient``
    A non-blocking Qt client driven by ``QSocketNotifier``.  It never spawns a
    thread, so GUI signals stay on the main thread.

Wire protocol: newline-delimited JSON-RPC 2.0 over ``AF_UNIX``/``SOCK_STREAM``.
"""

from __future__ import annotations

import json
import os
import socket
import time

SOCKET_NAME = "everlive.sock"
PROTO_VERSION = 1
MAX_FRAME = 65536

try:  # The hub always has Qt; keep probe() usable without it.
    from PySide6.QtCore import QObject, QSocketNotifier, Signal
except ImportError:  # pragma: no cover - only for Qt-less tooling
    QObject = None
    QSocketNotifier = None
    Signal = None


def socket_path() -> str:
    """Return the socket location the streamer binds.

    Mirrors ``resolve_socket_path()`` in ``gc573_user_ipc.c`` so both sides
    always agree, including when ``XDG_RUNTIME_DIR`` is unset (systemd-less
    sessions, SSH logins, containers).
    """
    override = os.environ.get("EVERLIVE_SOCKET")
    if override:
        return override

    base = os.environ.get("XDG_RUNTIME_DIR")
    if base and os.path.isdir(base):
        return os.path.join(base, SOCKET_NAME)

    fallback = f"/run/user/{os.getuid()}"
    if os.path.isdir(fallback):
        return os.path.join(fallback, SOCKET_NAME)

    return os.path.join(f"/tmp/everlive-{os.getuid()}", SOCKET_NAME)


def probe(timeout: float = 0.25):
    """Synchronously ask a running streamer who it is.

    Returns the ``hello`` result as a dict, or ``None`` when no streamer is
    listening.  Never raises: a stale socket, a permission error or a malformed
    reply all mean "no usable remote session".
    """
    path = socket_path()
    if not os.path.exists(path):
        return None

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    try:
        sock.connect(path)
        sock.sendall(b'{"jsonrpc":"2.0","id":1,"method":"hello"}\n')

        buf = b""
        deadline = time.monotonic() + timeout
        while b"\n" not in buf and time.monotonic() < deadline:
            chunk = sock.recv(4096)
            if not chunk:
                return None
            buf += chunk

        if b"\n" not in buf:
            return None

        message = json.loads(buf.split(b"\n", 1)[0])
        result = message.get("result")
        return result if isinstance(result, dict) else None
    except (OSError, ValueError):
        return None
    finally:
        sock.close()


if QSocketNotifier is not None:

    class EverLiveIpcClient(QObject):
        """Non-blocking remote-control client for a running streamer."""

        connectedChanged = Signal(bool)
        helloReceived = Signal(dict)
        statusReceived = Signal(dict)
        eventReceived = Signal(str, dict)
        errorRaised = Signal(str)

        def __init__(self, parent=None):
            super().__init__(parent)
            self._sock = None
            self._read_notifier = None
            self._write_notifier = None
            self._buffer = b""
            self._out = b""
            self._next_id = 1
            self._pending = {}
            self._methods = {}
            self._version = ""

        # -- state -----------------------------------------------------

        @property
        def connected(self) -> bool:
            return self._sock is not None

        @property
        def version(self) -> str:
            return self._version

        # -- lifecycle -------------------------------------------------

        def try_connect(self) -> bool:
            """Attach to the streamer if one is listening.

            Non-blocking and safe to call repeatedly: it returns ``True`` when
            already connected, ``False`` when no socket exists or the connect
            was refused.
            """
            if self._sock is not None:
                return True

            path = socket_path()
            if not os.path.exists(path):
                return False

            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.setblocking(False)
            try:
                sock.connect(path)
            except BlockingIOError:
                pass  # in progress; the read notifier completes it
            except OSError:
                sock.close()
                return False

            self._sock = sock
            self._buffer = b""
            self._out = b""
            self._read_notifier = QSocketNotifier(
                sock.fileno(), QSocketNotifier.Type.Read, self)
            self._read_notifier.activated.connect(self._on_readable)

            self.connectedChanged.emit(True)
            self.request("hello", callback=self._on_hello)
            return True

        def close(self):
            """Detach and release every notifier.  Idempotent."""
            was_connected = self._sock is not None

            if self._read_notifier is not None:
                self._read_notifier.setEnabled(False)
                self._read_notifier.deleteLater()
                self._read_notifier = None
            if self._write_notifier is not None:
                self._write_notifier.setEnabled(False)
                self._write_notifier.deleteLater()
                self._write_notifier = None

            if self._sock is not None:
                try:
                    self._sock.close()
                except OSError:
                    pass
                self._sock = None

            self._buffer = b""
            self._out = b""
            self._pending.clear()
            self._methods.clear()

            if was_connected:
                self.connectedChanged.emit(False)

        # -- requests --------------------------------------------------

        def request(self, method: str, params=None, callback=None) -> bool:
            """Send a JSON-RPC request and optionally register a reply callback."""
            if self._sock is None:
                return False

            req_id = self._next_id
            self._next_id += 1

            message = {"jsonrpc": "2.0", "id": req_id, "method": method}
            if params is not None:
                message["params"] = params

            self._methods[req_id] = method
            if callback is not None:
                self._pending[req_id] = callback

            return self._send(message)

        def notify(self, method: str, params=None) -> bool:
            """Send a notification (no reply expected)."""
            if self._sock is None:
                return False

            message = {"jsonrpc": "2.0", "method": method}
            if params is not None:
                message["params"] = params
            return self._send(message)

        def request_status(self) -> bool:
            """Ask for a fresh snapshot; the reply arrives via statusReceived."""
            return self.request("status.get")

        def request_ping(self, callback=None) -> bool:
            return self.request("ping", callback=callback)

        # -- mutations (Phase 2) ---------------------------------------

        def set_led(self, r: int, g: int, b: int) -> bool:
            """Set a static RGB colour (0-255 per channel)."""
            return self.request("led.set", {
                "r": max(0, min(255, int(r))),
                "g": max(0, min(255, int(g))),
                "b": max(0, min(255, int(b))),
            })

        def set_led_mode(self, mode: str) -> bool:
            """mode: ``static`` | ``wave`` | ``rainbow`` | ``off``."""
            return self.request("led.mode.set", {"mode": str(mode)})

        def set_volume(self, gain: float) -> bool:
            """Linear gain, 0.0 .. 1.5 (1.0 = unity). Applied with a ramp."""
            return self.request("audio.volume.set",
                                {"gain": max(0.0, min(1.5, float(gain)))})

        def set_mute(self, muted: bool) -> bool:
            return self.request("audio.mute.set", {"muted": bool(muted)})

        def set_osd(self, enabled: bool) -> bool:
            return self.request("osd.set", {"enabled": bool(enabled)})

        def set_osd_position(self, pos: str) -> bool:
            """pos: ``tr`` | ``br`` | ``bl`` | ``tl``."""
            return self.request("osd.position.set", {"pos": str(pos)})

        def set_aspect(self, mode: str) -> bool:
            """mode: ``auto`` | ``16:9`` | ``4:3`` | ``stretch``."""
            return self.request("aspect.set", {"mode": str(mode)})

        # -- wire ------------------------------------------------------

        def _send(self, message) -> bool:
            if self._sock is None:
                return False
            try:
                payload = json.dumps(message, separators=(",", ":")).encode()
            except (TypeError, ValueError):
                return False

            self._out += payload + b"\n"
            return self._flush()

        def _flush(self) -> bool:
            if self._sock is None:
                return False

            while self._out:
                try:
                    sent = self._sock.send(self._out)
                except BlockingIOError:
                    self._want_write(True)
                    return True
                except OSError:
                    self.close()
                    return False

                if sent <= 0:
                    self.close()
                    return False
                self._out = self._out[sent:]

            self._want_write(False)
            return True

        def _want_write(self, enable: bool):
            if self._sock is None:
                return
            if enable and self._write_notifier is None:
                self._write_notifier = QSocketNotifier(
                    self._sock.fileno(), QSocketNotifier.Type.Write, self)
                self._write_notifier.activated.connect(self._on_writable)
            elif not enable and self._write_notifier is not None:
                self._write_notifier.setEnabled(False)
                self._write_notifier.deleteLater()
                self._write_notifier = None

        def _on_writable(self):
            self._flush()

        def _on_readable(self):
            if self._sock is None:
                return

            while True:
                try:
                    chunk = self._sock.recv(65536)
                except BlockingIOError:
                    break
                except OSError:
                    self.close()
                    return

                if not chunk:
                    self.close()
                    return

                self._buffer += chunk
                if len(self._buffer) > MAX_FRAME * 4:
                    self.errorRaised.emit("Reply stream exceeded the frame limit")
                    self.close()
                    return

            while b"\n" in self._buffer:
                line, self._buffer = self._buffer.split(b"\n", 1)
                if not line:
                    continue
                self._dispatch(line)
                if self._sock is None:
                    return

        def _dispatch(self, raw: bytes):
            try:
                message = json.loads(raw)
            except ValueError:
                self.errorRaised.emit("Malformed reply from the streamer")
                return

            if not isinstance(message, dict):
                return

            if "method" in message:  # server-initiated event
                params = message.get("params")
                self.eventReceived.emit(
                    str(message["method"]),
                    params if isinstance(params, dict) else {})
                return

            req_id = message.get("id")
            method = self._methods.pop(req_id, None)

            error = message.get("error")
            if isinstance(error, dict):
                self.errorRaised.emit(
                    f"{error.get('message', 'request failed')} "
                    f"({error.get('code', 0)})")

            result = message.get("result")
            if method == "status.get" and isinstance(result, dict):
                self.statusReceived.emit(result)

            callback = self._pending.pop(req_id, None)
            if callback is not None:
                callback(message)

        def _on_hello(self, message):
            result = message.get("result")
            if isinstance(result, dict):
                self._version = str(result.get("version", ""))
                self.helloReceived.emit(result)

else:  # pragma: no cover - PySide6 is a hard dependency of the hub

    class EverLiveIpcClient:  # type: ignore[no-redef]
        """Placeholder that fails loudly when Qt is missing."""

        def __init__(self, *args, **kwargs):
            raise RuntimeError(
                "EverLiveIpcClient requires PySide6 (install python3-pyside6)")
