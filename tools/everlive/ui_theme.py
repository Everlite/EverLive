#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""EverLive Console Hub design tokens and application stylesheet.

The v1.7.0 shell carried its palette inline in ``everlive_viewer.init_ui``.
Centralising it here keeps the mockup palette in one place and lets the
widget modules style themselves without copying hex strings around.
"""

from __future__ import annotations

# ---------------------------------------------------------------------------
# Palette (sampled from the Console Hub mockups)
# ---------------------------------------------------------------------------
BG = "#12151F"
SURFACE = "#1A1D2B"
SURFACE_RAISED = "#202334"
SURFACE_SUNKEN = "#161927"
SURFACE_HOVER = "#262A3C"
BORDER = "#2A2D3E"
BORDER_STRONG = "#3A3E54"

TEXT = "#E7EBF2"
TEXT_MUTED = "#9AA0B0"
TEXT_DIM = "#6E7485"

ACCENT = "#D93B86"
ACCENT_SOFT = "#C689F0"
ACCENT_DEEP = "#A92C66"

BLUE = "#39598A"
BLUE_HOVER = "#43679C"
BLUE_TEXT = "#E4EEFF"

MINT = "#7FD8C0"
MINT_BG = "#23423B"
MINT_BORDER = "#3C6B63"

WARN = "#E8B36B"
WARN_BG = "#3A2E1C"
WARN_BORDER = "#6B5734"

DANGER = "#FF6B7A"
DANGER_BG = "#3B1D25"
DANGER_BORDER = "#7A2A38"

CHIP_BG = "#211F2E"
CHIP_BORDER = "#3A3650"

# Spacing / geometry shared by the shell.
PAD = 14
GAP = 10
RADIUS = 12
RADIUS_CSS = "12px"
HEADER_HEIGHT = 54
BOTTOM_BAR_HEIGHT = 48
INSPECTOR_WIDTH = 352

FONT_STACK = ('-apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, '
              '"Helvetica Neue", Arial, sans-serif')
MONO_STACK = '"JetBrains Mono", "Consolas", monospace'

TOKENS = {
    "BG": BG,
    "SURFACE": SURFACE,
    "SURFACE_RAISED": SURFACE_RAISED,
    "SURFACE_SUNKEN": SURFACE_SUNKEN,
    "SURFACE_HOVER": SURFACE_HOVER,
    "BORDER": BORDER,
    "BORDER_STRONG": BORDER_STRONG,
    "TEXT": TEXT,
    "TEXT_MUTED": TEXT_MUTED,
    "TEXT_DIM": TEXT_DIM,
    "ACCENT": ACCENT,
    "ACCENT_SOFT": ACCENT_SOFT,
    "ACCENT_DEEP": ACCENT_DEEP,
    "BLUE": BLUE,
    "BLUE_HOVER": BLUE_HOVER,
    "BLUE_TEXT": BLUE_TEXT,
    "MINT": MINT,
    "MINT_BG": MINT_BG,
    "MINT_BORDER": MINT_BORDER,
    "WARN": WARN,
    "WARN_BG": WARN_BG,
    "WARN_BORDER": WARN_BORDER,
    "DANGER": DANGER,
    "DANGER_BG": DANGER_BG,
    "DANGER_BORDER": DANGER_BORDER,
    "CHIP_BG": CHIP_BG,
    "CHIP_BORDER": CHIP_BORDER,
    "RADIUS": RADIUS_CSS,
    "FONT_STACK": FONT_STACK,
    "MONO_STACK": MONO_STACK,
}

# ---------------------------------------------------------------------------
# Stylesheet
# ---------------------------------------------------------------------------
# Tokens are substituted with ``str.replace`` so the QSS braces stay literal
# and readable (unlike an f-string, which would need every brace doubled).
_QSS = """
QMainWindow {
    background-color: @BG@;
}
QDialog {
    background-color: @BG@;
    color: @TEXT@;
}
QWidget {
    color: @TEXT@;
    font-family: @FONT_STACK@;
    font-size: 12.5px;
}
QLabel {
    color: @TEXT@;
    font-size: 12.5px;
}
QFrame#headerBar {
    background-color: @SURFACE_RAISED@;
    border: 1px solid @BORDER@;
    border-radius: @RADIUS@;
}
QFrame#videoStageFrame {
    background-color: @SURFACE@;
    border: 1px solid @BORDER@;
    border-radius: 16px;
}
QFrame#telemetryCard {
    background-color: @SURFACE@;
    border: 1px solid @BORDER@;
    border-radius: 14px;
}
QFrame#cardFrame {
    background-color: @SURFACE@;
    border: 1px solid @BORDER@;
    border-radius: 12px;
}
QFrame#handshakeCard {
    background-color: @SURFACE_SUNKEN@;
    border: 1px solid @BORDER_STRONG@;
    border-radius: 11px;
}
QFrame#bottomBar {
    background-color: @SURFACE_RAISED@;
    border: 1px solid @BORDER@;
    border-radius: @RADIUS@;
}
QFrame#bottomBar QLabel {
    color: @TEXT_MUTED@;
}
QPushButton#btnBar {
    background: transparent;
    border: 1px solid transparent;
    color: @TEXT_MUTED@;
    border-radius: 8px;
    padding: 4px 10px;
    font-size: 12px;
    font-weight: 600;
}
QPushButton#btnBar:hover {
    background-color: @SURFACE_HOVER@;
    border-color: @BORDER@;
    color: @TEXT@;
}
QPushButton#btnBar:checked {
    background-color: @BLUE@;
    border-color: @BLUE_HOVER@;
    color: @BLUE_TEXT@;
}

QPushButton#deckHeader {
    background: transparent;
    border: none;
    color: @TEXT@;
    font-size: 12px;
    font-weight: 700;
    letter-spacing: 0.9px;
    text-align: left;
    padding: 4px 0px;
}
QPushButton#deckHeader:hover {
    color: @ACCENT@;
}
QPushButton#deckHeader:checked {
    color: @TEXT@;
}
QLabel#logoTitle {
    font-size: 15px;
    font-weight: 800;
    letter-spacing: 1.0px;
    color: @TEXT@;
}
QLabel#logoVersion {
    color: @TEXT_DIM@;
    font-size: 11px;
    font-weight: 600;
}
QLabel#connDot {
    color: @MINT@;
    font-size: 11px;
}
QFrame#sourceGroup {
    background-color: @SURFACE_RAISED@;
    border: 1px solid @BORDER@;
    border-radius: 10px;
}
QLabel#sourceName {
    color: @TEXT@;
    background: transparent;
    padding: 0px 2px;
    font-size: 12.5px;
    font-weight: 800;
    letter-spacing: 0.6px;
}
QWidget#sourceSegments {
    background-color: @SURFACE_SUNKEN@;
    border: 1px solid @BORDER@;
    border-radius: 8px;
}
QLabel#badgeModel {
    color: @TEXT@;
    background-color: @CHIP_BG@;
    border: 1px solid @CHIP_BORDER@;
    border-radius: 6px;
    padding: 0px 9px;
    font-size: 10.5px;
    font-weight: 700;
    letter-spacing: 0.5px;
}
QLabel#pillLive {
    border-radius: 12px;
    padding: 0px 11px;
    font-size: 11px;
    font-weight: 600;
}
QLabel#sectionTitle {
    color: @TEXT_MUTED@;
    font-size: 11.5px;
    font-weight: 700;
    letter-spacing: 0.8px;
}
QLabel#fieldLabel {
    color: @TEXT_MUTED@;
    font-size: 12.5px;
}
QLabel#hintText {
    color: @TEXT_DIM@;
    font-size: 11.5px;
}
QLabel#kvKey {
    color: @TEXT_DIM@;
    font-size: 12px;
}
QLabel#kvVal {
    color: @TEXT@;
    font-family: @MONO_STACK@;
    font-size: 12px;
    font-weight: 600;
}
QLabel#heroSignal {
    color: @TEXT@;
    font-family: @MONO_STACK@;
    font-size: 14px;
    font-weight: 700;
}
QLabel#heroMeta {
    color: @TEXT_MUTED@;
    font-size: 11px;
}
QLabel#fpsHero {
    color: @MINT@;
    font-family: @MONO_STACK@;
    font-size: 23px;
    font-weight: 800;
}
QLabel#fpsUnit {
    color: @TEXT_MUTED@;
    font-size: 11px;
    font-weight: 600;
}
QLabel#teleValue {
    color: @TEXT@;
    font-size: 13px;
    font-weight: 700;
}
QLabel#teleSub {
    color: @TEXT_MUTED@;
    font-size: 11px;
}
QLabel#stageBadge {
    color: @ACCENT@;
    background-color: #261628;
    border: 1px solid #4A2444;
    border-radius: 11px;
    padding: 3px 12px;
    font-size: 10px;
    font-weight: 700;
    letter-spacing: 0.9px;
}
QLabel#stageTitle {
    color: @TEXT@;
    font-size: 20px;
    font-weight: 700;
}
QLabel#stageSub {
    color: @TEXT_MUTED@;
    font-size: 12px;
}
QLabel#audioFormat {
    color: @ACCENT_SOFT@;
    background-color: #211B36;
    border: 1px solid #382E5C;
    border-radius: 6px;
    padding: 0px 9px;
    font-size: 11px;
    font-weight: 600;
}
QLabel#footerText {
    color: @TEXT_DIM@;
    font-size: 11px;
}
QLabel#toggleLabel {
    color: @TEXT@;
    font-size: 11.5px;
}
QLabel#toggleHint {
    color: @TEXT_DIM@;
    font-size: 10.5px;
}
QLabel#statusChip {
    border-radius: 14px;
    padding: 0px 12px;
    font-size: 11px;
    font-weight: 700;
    letter-spacing: 0.3px;
}
QLabel#obsoleteBadge {
    color: @WARN@;
    background-color: @WARN_BG@;
    border: 1px solid @WARN_BORDER@;
    border-radius: 7px;
    padding: 1px 7px;
    font-size: 9px;
    font-weight: 700;
    letter-spacing: 0.6px;
}

/* Inspector tabs */
QTabWidget#inspectorTabs::pane {
    border: none;
    background: transparent;
    margin-top: 6px;
}
QTabWidget#inspectorTabs QTabBar {
    background: transparent;
    border: none;
}
QTabWidget#inspectorTabs QTabBar::tab {
    background-color: transparent;
    color: @TEXT_MUTED@;
    border: none;
    border-radius: 8px;
    padding: 8px 12px;
    margin: 3px;
    font-size: 12px;
    font-weight: 600;
}
QTabWidget#inspectorTabs QTabBar::tab:hover:!selected {
    color: @TEXT@;
    background-color: @SURFACE_HOVER@;
}
QTabWidget#inspectorTabs QTabBar::tab:selected {
    color: @TEXT@;
    background-color: @BLUE@;
    border: 1px solid @BLUE_HOVER@;
}
QScrollArea#inspectorScroll, QScrollArea#inspectorScroll > QWidget > QWidget {
    background: transparent;
    border: none;
}

/* Segmented control */
QWidget#segmentedControl {
    background-color: @SURFACE_SUNKEN@;
    border: 1px solid @BORDER@;
    border-radius: 9px;
}
QPushButton#segmentButton {
    background-color: transparent;
    color: @TEXT_MUTED@;
    border: none;
    border-radius: 7px;
    padding: 5px 14px;
    font-size: 12px;
    font-weight: 600;
}
QPushButton#segmentButton:hover:!checked {
    color: @TEXT@;
    background-color: @SURFACE_HOVER@;
}
QPushButton#segmentButton:checked {
    background-color: @BLUE@;
    color: @BLUE_TEXT@;
    border: 1px solid @BLUE_HOVER@;
}
QPushButton#segmentButton:disabled {
    color: @TEXT_DIM@;
}

/* Icon buttons */
QPushButton#iconButton {
    background-color: transparent;
    color: @TEXT_MUTED@;
    border: 1px solid transparent;
    border-radius: 9px;
    padding: 0px;
}
QPushButton#iconButton:hover {
    background-color: @SURFACE_HOVER@;
    border-color: @BORDER_STRONG@;
    color: @TEXT@;
}
QPushButton#iconButton:checked {
    background-color: @BLUE@;
    border-color: @BLUE_HOVER@;
    color: @BLUE_TEXT@;
}

/* Inputs */
QComboBox {
    background-color: @SURFACE_RAISED@;
    color: @TEXT@;
    border: 1px solid @BORDER_STRONG@;
    border-radius: 8px;
    min-height: 26px;
    padding: 4px 10px;
    font-size: 12px;
}
QComboBox:hover {
    border-color: @ACCENT@;
    background-color: @SURFACE_HOVER@;
}
QComboBox::drop-down {
    border: none;
    width: 18px;
}
QComboBox QAbstractItemView {
    background-color: @SURFACE_RAISED@;
    color: @TEXT@;
    border: 1px solid @BORDER_STRONG@;
    selection-background-color: @BLUE@;
    selection-color: @BLUE_TEXT@;
}
QDoubleSpinBox {
    min-height: 22px;
    padding: 2px 8px;
    font-size: 11px;
    background-color: @SURFACE_RAISED@;
    color: @TEXT@;
    border: 1px solid @BORDER_STRONG@;
    border-radius: 8px;
}
QDoubleSpinBox QLineEdit {
    padding-left: 6px;
    padding-right: 6px;
    background: transparent;
    color: @TEXT@;
    border: none;
}
QDoubleSpinBox:hover { border-color: @ACCENT@; }
QDoubleSpinBox:focus { border-color: @ACCENT_SOFT@; }
QDoubleSpinBox:disabled {
    color: @TEXT_DIM@;
    background-color: @SURFACE_SUNKEN@;
}
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {
    subcontrol-origin: border;
    width: 0px;
    border: none;
}
QTextEdit {
    background-color: @SURFACE_SUNKEN@;
    color: @TEXT@;
    border: 1px solid @BORDER@;
    border-radius: 6px;
    padding: 6px;
}

/* Buttons */
QPushButton {
    padding: 6px 14px;
    font-size: 12.5px;
    font-weight: 600;
    border-radius: 8px;
    background-color: @SURFACE_RAISED@;
    color: @TEXT@;
    border: 1px solid @BORDER_STRONG@;
}
QPushButton:hover {
    background-color: @SURFACE_HOVER@;
    border-color: @BORDER_STRONG@;
}
QPushButton:pressed { background-color: @SURFACE_SUNKEN@; }
QPushButton:disabled { color: @TEXT_DIM@; border-color: @BORDER@; }
QPushButton#btnStepper {
    padding: 0px;
    font-size: 14px;
    font-weight: 600;
    background-color: @SURFACE_RAISED@;
    color: @TEXT@;
    border: 1px solid @BORDER_STRONG@;
    border-radius: 6px;
}
QPushButton#btnStepper:hover {
    background-color: @SURFACE_HOVER@;
    border-color: @ACCENT@;
    color: #FFFFFF;
}
QPushButton#btnStepper:pressed { background-color: @SURFACE_SUNKEN@; }
QPushButton#btnStepper:disabled {
    background-color: @SURFACE_SUNKEN@;
    color: @TEXT_DIM@;
    border-color: @BORDER@;
}
QPushButton#btnGhost {
    background-color: transparent;
    color: @TEXT_MUTED@;
    border: 1px solid @BORDER_STRONG@;
    font-weight: 500;
}
QPushButton#btnGhost:hover {
    background-color: @SURFACE_HOVER@;
    color: @TEXT@;
    border-color: @BORDER_STRONG@;
}
QPushButton#btnGhost:checked {
    background-color: @BLUE@;
    color: @BLUE_TEXT@;
    border-color: @BLUE_HOVER@;
}
QPushButton#btnMute {
    background-color: @SURFACE_RAISED@;
    color: @TEXT@;
    border: 1px solid @BORDER_STRONG@;
    border-radius: 7px;
    padding: 3px 8px;
    font-size: 11px;
    font-weight: 600;
}
QPushButton#btnMute:hover {
    background-color: @SURFACE_HOVER@;
    border-color: @ACCENT@;
}
QPushButton#btnLaunchStart {
    background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0,
        stop:0 #25856B, stop:1 #36A888);
    border: 1px solid #5ED4B2;
    color: #FFFFFF;
    font-size: 12px;
    font-weight: 700;
    padding: 5px 15px;
}
QPushButton#btnLaunchStart:hover {
    background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0,
        stop:0 #2B9679, stop:1 #40BA98);
}
QPushButton#btnLaunchStop {
    background-color: #9E3342;
    border: 1px solid #FF5D5D;
    color: #FFFFFF;
    font-size: 12px;
    font-weight: 700;
    padding: 5px 15px;
}
QPushButton#btnLaunchStop:hover { background-color: #B53B4C; }
QPushButton#btnStudioStart,
QPushButton#btnStudioSwitch {
    background-color: transparent;
    border: 1px solid @BORDER_STRONG@;
    color: @TEXT_MUTED@;
    font-size: 12px;
    font-weight: 600;
    padding: 5px 13px;
}
QPushButton#btnStudioStart:hover,
QPushButton#btnStudioSwitch:hover {
    background-color: @SURFACE_HOVER@;
    border-color: @ACCENT_SOFT@;
    color: @TEXT@;
}
QPushButton#btnLaunchSwitch {
    background-color: @BLUE@;
    border: 1px solid @BLUE_HOVER@;
    color: @BLUE_TEXT@;
    font-size: 12px;
    font-weight: 600;
    padding: 5px 14px;
}
QPushButton#btnLaunchSwitch:hover { background-color: @BLUE_HOVER@; }
QPushButton#btnStudioStop {
    background-color: #9E3342;
    border: 1px solid #FF5D5D;
    color: #FFFFFF;
    font-size: 12px;
    font-weight: 700;
    padding: 5px 13px;
}
QPushButton#btnStudioStop:hover { background-color: #B53B4C; }

/* Checkboxes (legacy rows not yet using ToggleSwitch) */
QCheckBox {
    color: @TEXT_MUTED@;
    font-size: 11.5px;
    spacing: 7px;
}
QCheckBox:hover { color: @TEXT@; }
QCheckBox:disabled { color: @TEXT_DIM@; }
QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border-radius: 4px;
    border: 1px solid @BORDER_STRONG@;
    background-color: @SURFACE_SUNKEN@;
}
QCheckBox::indicator:hover { border-color: @ACCENT@; }
QCheckBox::indicator:checked {
    background-color: @ACCENT@;
    border-color: @ACCENT@;
}

/* Sliders */
QScrollBar:vertical {
    background: transparent;
    width: 8px;
    margin: 0px;
}
QScrollBar::handle:vertical {
    background: @BORDER_STRONG@;
    border-radius: 4px;
    min-height: 26px;
}
QScrollBar::handle:vertical:hover {
    background: @TEXT_DIM@;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0px;
    background: transparent;
}
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    background: transparent;
}
QSlider::groove:horizontal {
    height: 5px;
    background: @SURFACE_HOVER@;
    border-radius: 2px;
}
QSlider::sub-page:horizontal {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
        stop:0 @ACCENT_SOFT@, stop:0.55 @ACCENT@, stop:1 @WARN@);
    border-radius: 2px;
}
QSlider::handle:horizontal {
    background: #FFFFFF;
    border: 1px solid @ACCENT@;
    width: 14px;
    margin-top: -5px;
    margin-bottom: -5px;
    border-radius: 7px;
}
QSlider::handle:horizontal:hover { background: #FFFFFF; }
"""


def build_stylesheet() -> str:
    """Return the application stylesheet with every token substituted."""
    out = _QSS
    for key, value in TOKENS.items():
        out = out.replace(f"@{key}@", value)
    return out


def missing_tokens(stylesheet: str | None = None) -> list[str]:
    """Return any ``@TOKEN@`` placeholders left after substitution.

    Used by the tests to catch a typo in a token name.
    """
    import re
    text = build_stylesheet() if stylesheet is None else stylesheet
    return sorted(set(re.findall(r"@[A-Z_]+@", text)))
