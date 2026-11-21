#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate the engine OSD theme header from the Console Hub tokens.

The Hub is the design-system owner.  The engine renderer is C so it can draw
inside the video frame loop without Qt or Python, but it consumes the same
palette, control rhythm and state colours through this checked-in header.

Run ``python3 tools/everlive/gen_osd_theme.py`` to regenerate it, or
``--check`` to fail when the checked-in copy is stale.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ui_theme  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
TARGET = (REPO_ROOT / "tools" / "libeverlive-core" / "osd" /
          "everlive_osd_theme.h")

# The OSD is authored at 1080p with Hub measurements multiplied by this
# factor.  At 720p (the common embedded/pop-out height) scale == 2/3, so the
# engine then renders the Hub's 36 px controls and 16 px padding 1:1.
HUB_SCALE = 1.0


def _rgb(value: str) -> int:
    return int(value.lstrip("#"), 16)


# Colours are deliberately semantic rather than component-specific, so the
# renderer can switch between Hub states without hard-coded hex strings.
_COLORS = {
    "CARD_BG": ui_theme.SURFACE_RAISED,
    "CARD_BORDER": ui_theme.BORDER,
    "GROUP_BORDER": ui_theme.GROUP_BORDER,
    "TITLE": ui_theme.TEXT,
    "TEXT_MUTED": ui_theme.TEXT_MUTED,
    "TEXT_DIM": ui_theme.TEXT_DIM,
    "INPUT_BG": ui_theme.INPUT_BG,
    "INPUT_BG_HOVER": ui_theme.INPUT_HOVER,
    "INPUT_BORDER": ui_theme.INPUT_BORDER,
    "SEGMENT_BG": ui_theme.SURFACE_SUNKEN,
    "CHIP_BG": ui_theme.SURFACE_RAISED,
    "CHIP_BORDER": ui_theme.CHIP_BORDER,
    "HOVER_BG": ui_theme.SURFACE_HOVER,
    "HOVER_BORDER": ui_theme.BORDER_STRONG,
    "TOOLTIP_BG": ui_theme.SURFACE_RAISED,
    "TOOLTIP_BORDER": ui_theme.GROUP_BORDER,
    "KBD_BG": ui_theme.KBD_BG,
    "DIVIDER": ui_theme.GROUP_BORDER,
    "TRACK": ui_theme.SURFACE_HOVER,
    "KNOB": ui_theme.KNOB,
    "LIVE": ui_theme.MINT,
    "LIVE_OFF": ui_theme.TEXT_DIM,
    "MINT_BG": ui_theme.MINT_BG,
    "MINT_BORDER": ui_theme.MINT_BORDER,
    "MINT": ui_theme.MINT,
    "WARN_BG": ui_theme.WARN_BG,
    "WARN_BORDER": ui_theme.WARN_BORDER,
    "WARN": ui_theme.WARN,
    "DANGER_BG": ui_theme.DANGER_BG,
    "DANGER_BORDER": ui_theme.DANGER_BORDER,
    "DANGER": ui_theme.DANGER,
    "BLUE": ui_theme.BLUE,
    "BLUE_BORDER": ui_theme.BLUE_HOVER,
    "BLUE_TEXT": ui_theme.BLUE_TEXT,
    "ON_ACCENT": ui_theme.ON_ACCENT,
}

# Geometry and type are the Hub's own measurements.  Keep this table in the
# same units as ui_theme.py: the renderer multiplies them by HUB_SCALE.
_GEOMETRY = [
    ("BAR_PAD_X", 16.0, "header/bottom-bar horizontal padding"),
    ("BAR_PAD_Y", 14.0, "header vertical padding around 36 px controls"),
    ("GAP", 10.0, "default control gap"),
    ("INFO_GAP", 20.0, "identity cluster to control group"),
    ("CONTROL_H", 36.0, "header/bottom-bar control height"),
    ("CONTROL_PAD_X", 14.0, "button/combo horizontal padding"),
    ("CONTROL_GAP", 8.0, "icon-to-label gap inside a control"),
    ("ICON_BUTTON", 36.0, "square icon-button edge"),
    ("ICON_GAP", 10.0, "icon-button spacing"),
    ("ICON_SIZE", 18.0, "icon glyph box"),
    ("CHEVRON_SIZE", 12.0, "combo chevron glyph"),
    ("RADIUS_CARD", 12.0, "cards and the OSD bar"),
    ("RADIUS_GROUP", 6.0, "segmented groups and combos"),
    ("RADIUS_CONTROL", 8.0, "buttons, chips and status pills"),
    ("RADIUS_POPUP", 6.0, "combo dropdown"),
    ("RADIUS_TOOLTIP", 8.0, "tooltip and keycap container"),
    ("RADIUS_TOAST", 8.0, "status-pill notification"),
    ("TITLE_SIZE", 15.0, "source-name size"),
    ("TITLE_TRACKING", 0.6, "source-name letter spacing"),
    ("LABEL_SIZE", 14.0, "button/segment label size"),
    ("DETAIL_SIZE", 12.5, "signal/telemetry pill"),
    ("TOOLTIP_SIZE", 13.0, "tooltip label"),
    ("KEY_SIZE", 12.0, "shortcut keycap"),
    ("TOAST_SIZE", 12.5, "notification text"),
    ("TOAST_H", 36.0, "notification pill height"),
    ("TOAST_PAD_X", 12.0, "notification horizontal padding"),
    ("TOAST_DOT", 6.0, "notification tone dot"),
    ("POPUP_ROW_H", 32.0, "combo dropdown row"),
    ("POPUP_PAD", 5.0, "combo dropdown vertical padding"),
    ("POPUP_TEXT_SIZE", 14.0, "combo dropdown label"),
    ("POPUP_ITEM_PAD_X", 14.0, "combo dropdown item padding"),
    ("VOLUME_W", 250.0, "volume popup width"),
    ("VOLUME_H", 56.0, "volume popup height"),
    ("TRACK_H", 4.0, "slider track"),
    ("KNOB_SIZE", 14.0, "slider knob"),
    ("DOT_SIZE", 8.0, "live status dot"),
    ("DIVIDER_W", 1.0, "hairline divider"),
    ("DETAIL_PAD_X", 12.0, "signal pill horizontal padding"),
    ("DETAIL_PAD_Y", 9.0, "signal pill vertical padding"),
    ("TOOLTIP_PAD", 8.0, "tooltip horizontal padding"),
    ("TOOLTIP_GAP", 10.0, "tooltip column gap"),
    ("KEY_PAD_X", 6.0, "keycap horizontal padding"),
    ("NOTIFY_GAP", 10.0, "bar-to-notification gap"),
]


def build_header() -> str:
    lines = [
        "/* SPDX-License-Identifier: GPL-2.0-only */",
        "/*",
        " * Generated by tools/everlive/gen_osd_theme.py.",
        " * Source of truth: tools/everlive/ui_theme.py.",
        " * Do not edit by hand; run the generator instead.",
        " */",
        "",
        "#ifndef EVERLIVE_OSD_THEME_H",
        "#define EVERLIVE_OSD_THEME_H",
        "",
        f"#define EVERLIVE_OSD_HUB_SCALE {HUB_SCALE:.1f}f",
        "",
        "/* Palette */",
    ]
    for name, value in _COLORS.items():
        lines.append(f"#define EVERLIVE_OSD_{name}\t0x{_rgb(value):06X}")
    lines.extend([
        f"#define EVERLIVE_OSD_CARD_ALPHA\t{round(ui_theme.OSD_CARD_ALPHA * 255)}"
        "\t/* bar transparency */",
        "",
        "/* Hub geometry and typography (multiply by HUB_SCALE * scale). */",
    ])
    for name, value, comment in _GEOMETRY:
        lines.append(f"#define EVERLIVE_OSD_{name}\t{value:.1f}f\t/* {comment} */")
    lines.extend(["", "#endif /* EVERLIVE_OSD_THEME_H */", ""])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="exit non-zero when the checked-in header differs")
    args = parser.parse_args()
    header = build_header()
    if args.check:
        try:
            current = TARGET.read_text(encoding="utf-8")
        except FileNotFoundError:
            current = ""
        if current != header:
            print(f"{TARGET} is stale; run "
                  "python3 tools/everlive/gen_osd_theme.py", file=sys.stderr)
            return 1
        return 0
    TARGET.write_text(header, encoding="utf-8")
    print(f"wrote {TARGET}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
