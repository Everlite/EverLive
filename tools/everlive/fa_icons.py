#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Font Awesome Free icon helpers for the Console Hub.

The two bundled fonts (solid + brands) are part of Font Awesome Free 6.
See ``fonts/LICENSE.txt`` for the icon (CC BY 4.0) and font (SIL OFL 1.1)
licences.
"""

from __future__ import annotations

import os

try:
    from PySide6.QtCore import QRectF, QSize, Qt
    from PySide6.QtGui import (
        QColor, QFont, QFontDatabase, QIcon, QPainter, QPixmap,
    )
    HAS_QT = True
except ImportError:  # CI compiles the viewer without PySide6
    HAS_QT = False

FONT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fonts")

_FONT_FILES = {
    "solid": "fa-solid-900.ttf",
    "brands": "fa-brands-400.ttf",
}

_FAMILIES: dict[str, str] = {}
_LOADED = False

# name: (font style, unicode codepoint)
ICONS = {
    # transport / runtime
    "play": ("solid", "\uf04b"),
    "stop": ("solid", "\uf04d"),
    "pause": ("solid", "\uf04c"),
    "record": ("solid", "\uf111"),
    "folder": ("solid", "\uf07c"),
    "rotate": ("solid", "\uf2f1"),
    "tower-broadcast": ("solid", "\uf519"),
    "power": ("solid", "\uf011"),
    "display": ("solid", "\uf390"),
    "video": ("solid", "\uf03d"),
    # window / layout
    "expand": ("solid", "\uf065"),
    "compress": ("solid", "\uf066"),
    "external": ("solid", "\uf35d"),
    "window-restore": ("solid", "\uf2d2"),
    "arrow-right": ("solid", "\uf061"),
    "arrows-left-right": ("solid", "\uf337"),
    "up-down-left-right": ("solid", "\uf0b2"),
    "gear": ("solid", "\uf013"),
    "sliders": ("solid", "\uf1de"),
    "chevron-down": ("solid", "\uf078"),
    "chevron-up": ("solid", "\uf077"),
    "right-to-bracket": ("solid", "\uf2f6"),
    "table-cells": ("solid", "\uf00a"),
    "maximize": ("solid", "\uf31e"),
    "minimize": ("solid", "\uf78c"),
    # audio
    "volume": ("solid", "\uf028"),
    "volume-high": ("solid", "\uf028"),
    "mute": ("solid", "\uf6a9"),
    "volume-xmark": ("solid", "\uf6a9"),
    "waveform": ("solid", "\uf8de"),
    "table-cells-large": ("solid", "\uf009"),
    "headphones": ("solid", "\uf025"),
    "camera": ("solid", "\uf030"),
    "eye-slash": ("solid", "\uf070"),
    # help / diagnostics
    "help": ("solid", "\uf059"),
    "info": ("solid", "\uf05a"),
    "keyboard": ("solid", "\uf11c"),
    "clipboard": ("solid", "\uf328"),
    "file-lines": ("solid", "\uf15c"),
    "download": ("solid", "\uf019"),
    "trash": ("solid", "\uf1f8"),
    # telemetry
    "chart": ("solid", "\uf201"),
    "microchip": ("solid", "\uf2db"),
    "disk": ("solid", "\uf0a0"),
    # brands
    "discord": ("brands", "\uf392"),
    "github": ("brands", "\uf09b"),
}

# Used when the bundled fonts are unavailable (e.g. an old install).
_FALLBACK_GLYPHS = {
    "play": "▶",
    "stop": "■",
    "pause": "Ⅱ",
    "record": "●",
    "folder": "📁",
    "rotate": "↻",
    "tower-broadcast": "⌁",
    "power": "⏻",
    "display": "▭",
    "video": "▣",
    "expand": "⛶",
    "compress": "⛶",
    "external": "⧉",
    "window-restore": "▣",
    "arrow-right": "→",
    "arrows-left-right": "↔",
    "up-down-left-right": "✥",
    "gear": "⚙",
    "sliders": "☰",
    "chevron-down": "▾",
    "chevron-up": "▴",
    "right-to-bracket": "→",
    "table-cells": "▦",
    "maximize": "⛶",
    "minimize": "▭",
    "volume": "🔊",
    "volume-high": "🔊",
    "mute": "🔇",
    "volume-xmark": "🔇",
    "waveform": "∿",
    "table-cells-large": "▦",
    "headphones": "🎧",
    "camera": "◉",
    "eye-slash": "🚫",
    "help": "?",
    "info": "i",
    "keyboard": "⌨",
    "clipboard": "📋",
    "file-lines": "≡",
    "download": "⤓",
    "trash": "🗑",
    "chart": "📈",
    "microchip": "▤",
    "disk": "▥",
    "discord": "◉",
    "github": "◆",
}


def load_fonts() -> bool:
    """Register the bundled Font Awesome files once."""
    global _LOADED
    if _LOADED:
        return bool(_FAMILIES)
    _LOADED = True
    if not HAS_QT:
        return False

    for style, filename in _FONT_FILES.items():
        path = os.path.join(FONT_DIR, filename)
        if not os.path.isfile(path):
            continue
        font_id = QFontDatabase.addApplicationFont(path)
        if font_id < 0:
            continue
        families = QFontDatabase.applicationFontFamilies(font_id)
        if families:
            _FAMILIES[style] = families[0]
    return bool(_FAMILIES)


def available() -> bool:
    return load_fonts()


def glyph(name: str) -> str:
    return ICONS.get(name, ("solid", "?"))[1]


def fallback_text(name: str) -> str:
    return _FALLBACK_GLYPHS.get(name, "?")


def _icon_font(style: str, pixel_size: int) -> QFont:
    family = _FAMILIES.get(style) or _FAMILIES.get("solid") or ""
    font = QFont(family)
    if style == "solid":
        font.setStyleName("Solid")
    font.setPixelSize(max(1, int(pixel_size)))
    font.setHintingPreference(QFont.PreferNoHinting)
    return font


def _render(name: str, color: str, size: int) -> QPixmap:
    scale = 2
    pixmap = QPixmap(size * scale, size * scale)
    pixmap.fill(Qt.transparent)
    pixmap.setDevicePixelRatio(scale)

    style = ICONS.get(name, ("solid", "?"))[0]
    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.Antialiasing)
    painter.setRenderHint(QPainter.TextAntialiasing)
    # The pixmap carries a 2x device-pixel ratio, so paint in logical
    # coordinates; Qt scales the result for HiDPI displays.
    painter.setFont(_icon_font(style, int(size * 0.86)))
    painter.setPen(QColor(color))
    painter.drawText(
        QRectF(0, 0, size, size), Qt.AlignCenter, glyph(name))
    painter.end()
    return pixmap


def icon(name: str, color: str = "#9AA0B0", size: int = 18,
         active_color: str | None = None,
         disabled_color: str = "#5A5F70") -> QIcon:
    """Return a themed QIcon for a Font Awesome glyph name."""
    if not load_fonts():
        return QIcon()

    result = QIcon()
    normal = _render(name, color, size)
    active = _render(name, active_color or color, size)
    result.addPixmap(normal, QIcon.Normal, QIcon.Off)
    result.addPixmap(active, QIcon.Normal, QIcon.On)
    result.addPixmap(active, QIcon.Active, QIcon.Off)
    result.addPixmap(active, QIcon.Active, QIcon.On)
    result.addPixmap(active, QIcon.Selected, QIcon.On)
    result.addPixmap(
        _render(name, disabled_color, size), QIcon.Disabled, QIcon.Off)
    return result


def icon_size(size: int) -> "QSize":
    return QSize(int(size), int(size))
