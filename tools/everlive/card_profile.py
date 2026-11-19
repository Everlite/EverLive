#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Capture-card capabilities for the Console Hub.

The engine's card registry is the source of truth.  ``everlive-ctl detect``
reports the installed model and its capability list; when the CLI is missing
or a synthetic UI profile is requested, a static descriptor is used instead.

This module is intentionally Qt-free so it can be unit-tested without a
display and reused by future CLI tools.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from typing import FrozenSet, Iterable, Optional, Tuple


MODEL_GC573 = "gc573"
MODEL_GC570D = "gc570d"

CAP_VIDEO = "video"
CAP_AUDIO = "audio"
CAP_PASSTHROUGH = "passthrough"
CAP_EDID = "edid"
CAP_4K60 = "4k60"
CAP_4K120 = "4k120"
CAP_1440P120 = "1440p120"
CAP_1080P240 = "1080p240"
CAP_1080P60 = "1080p60"
CAP_HDR = "hdr"
CAP_DUAL_INPUT = "dual_input"
CAP_RGB_LED = "rgb_led"
CAP_BITSTREAM_AUDIO = "bitstream_audio"
CAP_FRAMEGEN = "framegen"
CAP_HARDWARE_SPLITTER = "hardware_splitter"

_CAPABILITY_NAMES = {
    "video-capture": CAP_VIDEO,
    "audio-capture": CAP_AUDIO,
    "passthrough": CAP_PASSTHROUGH,
    "edid-passthrough": CAP_EDID,
    "4k60": CAP_4K60,
    "4k120": CAP_4K120,
    "1440p120": CAP_1440P120,
    "1080p240": CAP_1080P240,
    "1080p60": CAP_1080P60,
    "hdr10": CAP_HDR,
    "dual-input": CAP_DUAL_INPUT,
    "rgb-led": CAP_RGB_LED,
    "bitstream-audio (ac3/dts)": CAP_BITSTREAM_AUDIO,
    "lossless scaling (lsfg-vk)": CAP_FRAMEGEN,
    "hardware-splitter": CAP_HARDWARE_SPLITTER,
}


@dataclass(frozen=True)
class CardProfile:
    model: str
    display_name: str
    capabilities: FrozenSet[str]
    input_count: int
    v4l2_nodes: Tuple[str, ...]
    preload_nodes: Tuple[str, ...]
    preload_library: str
    source: str = "static"

    def supports(self, capability: str) -> bool:
        return capability in self.capabilities

    @property
    def supports_framegen(self) -> bool:
        return self.supports(CAP_FRAMEGEN)

    @property
    def supports_hdr(self) -> bool:
        return self.supports(CAP_HDR)

    @property
    def supports_dual_input(self) -> bool:
        return self.supports(CAP_DUAL_INPUT)

    @property
    def supports_rgb_led(self) -> bool:
        return self.supports(CAP_RGB_LED)

    @property
    def is_duo(self) -> bool:
        return self.model == MODEL_GC570D


def _profile(
    model: str,
    display_name: str,
    capabilities: Iterable[str],
    input_count: int,
    v4l2_nodes: Tuple[str, ...],
    preload_nodes: Tuple[str, ...],
    preload_library: str,
    source: str = "static",
) -> CardProfile:
    return CardProfile(
        model=model,
        display_name=display_name,
        capabilities=frozenset(capabilities),
        input_count=input_count,
        v4l2_nodes=v4l2_nodes,
        preload_nodes=preload_nodes,
        preload_library=preload_library,
        source=source,
    )


GC573_PROFILE = _profile(
    MODEL_GC573,
    "GC573 · Live Gamer 4K",
    (
        CAP_VIDEO, CAP_AUDIO, CAP_PASSTHROUGH, CAP_EDID,
        CAP_4K60, CAP_1440P120, CAP_1080P240, CAP_1080P60,
        CAP_HDR, CAP_RGB_LED, CAP_BITSTREAM_AUDIO, CAP_FRAMEGEN,
    ),
    1,
    ("/dev/video10",),
    ("/dev/video63",),
    "libgc573-v4l2.so",
)

GC570D_PROFILE = _profile(
    MODEL_GC570D,
    "GC570D · Live Gamer DUO",
    (
        CAP_VIDEO, CAP_AUDIO, CAP_PASSTHROUGH, CAP_EDID,
        CAP_1080P60, CAP_DUAL_INPUT, CAP_RGB_LED, CAP_HARDWARE_SPLITTER,
    ),
    2,
    ("/dev/video10", "/dev/video11"),
    ("/dev/video60", "/dev/video61"),
    "libgc570d-v4l2.so",
)

_STATIC_PROFILES = {
    MODEL_GC573: GC573_PROFILE,
    MODEL_GC570D: GC570D_PROFILE,
}


def _detect_from_sysfs() -> Optional[str]:
    base = "/sys/bus/pci/devices"
    try:
        entries = os.listdir(base)
    except OSError:
        return None
    for entry in entries:
        dev = os.path.join(base, entry)
        try:
            with open(os.path.join(dev, "vendor")) as f:
                vendor = f.read().strip().lower()
            with open(os.path.join(dev, "device")) as f:
                device = f.read().strip().lower()
        except OSError:
            continue
        if vendor != "0x1461":
            continue
        if device not in ("0x0054", "0x0055"):
            continue
        if device == "0x0055":
            return MODEL_GC573
        try:
            with open(os.path.join(dev, "subsystem_device")) as f:
                subsystem = f.read().strip().lower()
        except OSError:
            subsystem = ""
        return MODEL_GC570D if subsystem == "0x5700" else MODEL_GC573
    return None


def _parse_ctl_detect(text: str) -> Optional[CardProfile]:
    model = None
    capabilities: set[str] = set()
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("Model Slug:"):
            model = stripped.split(":", 1)[1].strip().lower()
        elif stripped.startswith("Capabilities:"):
            payload = stripped.split(":", 1)[1].strip()
            for raw in payload.split(","):
                name = raw.strip().lower()
                cap = _CAPABILITY_NAMES.get(name)
                if cap:
                    capabilities.add(cap)
    if model not in _STATIC_PROFILES:
        return None
    base = _STATIC_PROFILES[model]
    return CardProfile(
        model=base.model,
        display_name=base.display_name,
        capabilities=frozenset(capabilities) if capabilities else base.capabilities,
        input_count=base.input_count,
        v4l2_nodes=base.v4l2_nodes,
        preload_nodes=base.preload_nodes,
        preload_library=base.preload_library,
        source="everlive-ctl",
    )


def detect_card_profile(force_model: Optional[str] = None) -> CardProfile:
    """Return the installed card profile or a static fallback.

    ``EVERLIVE_CARD_PROFILE=gc573|gc570d`` is a development-only override used
    to exercise GC573 UI states without hardware.  It does not affect engine
    detection or hardware access.
    """
    override = (force_model or os.environ.get("EVERLIVE_CARD_PROFILE", "")).strip().lower()
    if override in _STATIC_PROFILES:
        return _STATIC_PROFILES[override]

    ctl = shutil.which("everlive-ctl")
    if ctl:
        try:
            out = subprocess.run(
                [ctl, "detect"], capture_output=True, text=True, timeout=1.5)
            if out.returncode == 0:
                profile = _parse_ctl_detect(out.stdout)
                if profile is not None:
                    return profile
        except (OSError, subprocess.SubprocessError, ValueError):
            pass

    model = _detect_from_sysfs()
    if model in _STATIC_PROFILES:
        return _STATIC_PROFILES[model]
    return GC573_PROFILE
