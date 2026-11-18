#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Reusable Console Hub controls for the mockup shell.

These are deliberately small and framework-native: they expose the same
``isChecked`` / ``setChecked`` / ``toggled`` surface as ``QCheckBox`` so the
viewer can adopt them incrementally without rewriting its signal wiring.
"""

from __future__ import annotations

try:
    import fa_icons
    from PySide6.QtCore import (
        Property, QEasingCurve, QEvent, QPropertyAnimation, QSize, Qt, Signal,
    )
    from PySide6.QtGui import QColor, QIcon, QPainter, QPen
    from PySide6.QtWidgets import (
        QAbstractButton, QButtonGroup, QHBoxLayout, QLabel, QPushButton,
        QSizePolicy, QVBoxLayout, QWidget,
    )
    HAS_QT = True
except ImportError:  # allows --cli and CI runs without PySide6 installed
    HAS_QT = False


if HAS_QT:

    class ToggleSwitch(QAbstractButton):
        """Animated pill switch used in place of a checkbox indicator."""

        TRACK_OFF = QColor("#383B4F")
        TRACK_ON = QColor("#D93B86")
        TRACK_DISABLED = QColor("#2A2C38")
        KNOB = QColor("#FFFFFF")
        KNOB_DISABLED = QColor("#7A7E8C")

        def __init__(self, parent=None, *, width: int = 40, height: int = 22):
            super().__init__(parent)
            self._w = int(width)
            self._h = int(height)
            self._position = 0.0
            self.setCheckable(True)
            self.setCursor(Qt.PointingHandCursor)
            self.setFocusPolicy(Qt.StrongFocus)
            self.setSizePolicy(QSizePolicy.Fixed, QSizePolicy.Fixed)
            self._anim = QPropertyAnimation(self, b"knobPosition", self)
            self._anim.setDuration(130)
            self._anim.setEasingCurve(QEasingCurve.OutCubic)
            self.toggled.connect(self._animate)

        # -- animation --------------------------------------------------
        def _animate(self, checked: bool):
            self._anim.stop()
            self._anim.setStartValue(self._position)
            self._anim.setEndValue(1.0 if checked else 0.0)
            self._anim.start()

        def get_knob_position(self) -> float:
            return self._position

        def set_knob_position(self, value: float):
            self._position = max(0.0, min(1.0, float(value)))
            self.update()

        knobPosition = Property(float, get_knob_position, set_knob_position)

        # -- geometry / painting ---------------------------------------
        def sizeHint(self) -> QSize:
            return QSize(self._w, self._h)

        def minimumSizeHint(self) -> QSize:
            return QSize(self._w, self._h)

        def setChecked(self, checked: bool):  # noqa: N802 (Qt casing)
            super().setChecked(checked)
            # Snap when the caller sets state before the widget is shown.
            if not self.isVisible():
                self.set_knob_position(1.0 if checked else 0.0)

        def paintEvent(self, event):  # noqa: N802 (Qt casing)
            painter = QPainter(self)
            painter.setRenderHint(QPainter.Antialiasing)

            radius = self._h / 2.0
            if not self.isEnabled():
                track = self.TRACK_DISABLED
            else:
                off = self.TRACK_OFF
                on = self.TRACK_ON
                mix = self._position
                track = QColor(
                    int(off.red() + (on.red() - off.red()) * mix),
                    int(off.green() + (on.green() - off.green()) * mix),
                    int(off.blue() + (on.blue() - off.blue()) * mix),
                )

            painter.setPen(Qt.NoPen)
            painter.setBrush(track)
            painter.drawRoundedRect(0, 0, self._w, self._h, radius, radius)

            knob_d = self._h - 4
            travel = self._w - knob_d - 4
            knob_x = 2 + travel * self._position
            painter.setBrush(self.KNOB if self.isEnabled()
                             else self.KNOB_DISABLED)
            painter.drawEllipse(int(knob_x), 2, knob_d, knob_d)

            if self.hasFocus():
                painter.setBrush(Qt.NoBrush)
                painter.setPen(QPen(QColor("#C689F0"), 1))
                painter.drawRoundedRect(0, 0, self._w - 1, self._h - 1,
                                        radius, radius)
            painter.end()

    class ToggleRow(QWidget):
        """Label + optional hint + switch, mirroring a bound QCheckBox."""

        toggled = Signal(bool)

        def __init__(self, text: str, hint: str = "", parent=None,
                     *, checkbox=None):
            super().__init__(parent)
            self._bound = None
            layout = QHBoxLayout(self)
            layout.setContentsMargins(0, 2, 0, 2)
            layout.setSpacing(10)

            text_col = QVBoxLayout()
            text_col.setContentsMargins(0, 0, 0, 0)
            text_col.setSpacing(1)
            self.label = QLabel(text)
            self.label.setObjectName("toggleLabel")
            self.label.setWordWrap(True)
            text_col.addWidget(self.label)
            self.hint = QLabel(hint)
            self.hint.setObjectName("toggleHint")
            self.hint.setWordWrap(True)
            self.hint.setVisible(bool(hint))
            text_col.addWidget(self.hint)
            layout.addLayout(text_col, 1)

            self.switch = ToggleSwitch(self)
            self.switch.toggled.connect(self.toggled)
            layout.addWidget(self.switch, 0, Qt.AlignVCenter)

            if checkbox is not None:
                self.bind(checkbox)

        # -- QCheckBox-compatible surface ------------------------------
        def isChecked(self) -> bool:  # noqa: N802 (Qt casing)
            return self.switch.isChecked()

        def setChecked(self, checked: bool):  # noqa: N802 (Qt casing)
            self.switch.setChecked(bool(checked))

        def setText(self, text: str):  # noqa: N802 (Qt casing)
            self.label.setText(text)

        def text(self) -> str:
            return self.label.text()

        # -- binding ---------------------------------------------------
        def bind(self, checkbox):
            """Mirror an existing QCheckBox in both directions."""
            self._bound = checkbox
            checkbox.installEventFilter(self)
            checkbox.toggled.connect(self.setChecked)
            self.toggled.connect(checkbox.setChecked)
            self.setChecked(checkbox.isChecked())
            self.setEnabled(checkbox.isEnabled())
            self.setToolTip(checkbox.toolTip())
            self.setVisible(not checkbox.isHidden())
            return self

        def eventFilter(self, obj, event):  # noqa: N802 (Qt casing)
            if obj is self._bound:
                if event.type() == QEvent.EnabledChange:
                    self.setEnabled(obj.isEnabled())
                elif event.type() in (QEvent.Show, QEvent.Hide):
                    self.setVisible(not obj.isHidden())
            return False

    class SegmentedControl(QWidget):
        """Exclusive pill selector (Simple/Advanced, HDMI 1/HDMI 2)."""

        currentIndexChanged = Signal(int)
        currentDataChanged = Signal(object)

        def __init__(self, segments=None, parent=None):
            super().__init__(parent)
            self.setObjectName("segmentedControl")
            self._layout = QHBoxLayout(self)
            self._layout.setContentsMargins(2, 2, 2, 2)
            self._layout.setSpacing(2)
            self._group = QButtonGroup(self)
            self._group.setExclusive(True)
            self._buttons = []
            self._data = []
            self._last_index = -1
            if segments:
                for label, data in segments:
                    self.add_segment(label, data)

        def add_segment(self, label: str, data=None) -> int:
            index = len(self._buttons)
            button = QPushButton(str(label), self)
            button.setObjectName("segmentButton")
            button.setCheckable(True)
            button.setCursor(Qt.PointingHandCursor)
            button.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
            button.setFixedHeight(28)
            button.clicked.connect(
                lambda _checked=False, i=index: self._select(i, emit=True))
            self._group.addButton(button, index)
            self._buttons.append(button)
            self._data.append(data)
            self._layout.addWidget(button, 1)
            if index == 0:
                button.setChecked(True)
                self._last_index = 0
            return index

        # -- QComboBox-like surface used by the viewer ----------------
        def count(self) -> int:
            return len(self._buttons)

        def currentIndex(self) -> int:  # noqa: N802 (Qt casing)
            for index, button in enumerate(self._buttons):
                if button.isChecked():
                    return index
            return -1

        def currentData(self):  # noqa: N802 (Qt casing)
            index = self.currentIndex()
            return self._data[index] if 0 <= index < len(self._data) else None

        def itemData(self, index: int):  # noqa: N802 (Qt casing)
            return self._data[index] if 0 <= index < len(self._data) else None

        def setCurrentIndex(self, index: int):  # noqa: N802 (Qt casing)
            self._select(index, emit=True)

        def setCurrentData(self, data):
            for index, value in enumerate(self._data):
                if value == data:
                    self._select(index, emit=True)
                    return

        def setEnabled(self, enabled: bool):  # noqa: N802 (Qt casing)
            super().setEnabled(enabled)
            if hasattr(self, "_buttons"):
                for button in self._buttons:
                    button.setEnabled(enabled)

        def _select(self, index: int, *, emit: bool):
            if not 0 <= index < len(self._buttons):
                return
            # QButtonGroup checks the clicked button before ``clicked``
            # reaches us, so currentIndex() is already the new index here.
            # Compare against the last emitted index instead.
            changed = self._last_index != index
            self._buttons[index].setChecked(True)
            self._last_index = index
            if emit and changed:
                self.currentIndexChanged.emit(index)
                self.currentDataChanged.emit(self._data[index])

    class StatusChip(QLabel):
        """Outlined status pill with a semantic tone."""

        TONES = {
            "idle": ("#211F2E", "#9AA0B0", "#3A3650"),
            "active": ("#23423B", "#7FD8C0", "#3C6B63"),
            "live": ("#23423B", "#7FD8C0", "#3C6B63"),
            "warn": ("#3A2E1C", "#E8B36B", "#6B5734"),
            "danger": ("#3B1D25", "#FF6B7A", "#7A2A38"),
            "accent": ("#2A2036", "#C689F0", "#4A3A63"),
        }

        def __init__(self, text: str = "", tone: str = "idle", parent=None,
                     *, dot: bool = False):
            super().__init__(parent)
            self.setObjectName("statusChip")
            self.setAlignment(Qt.AlignCenter)
            self.setFixedHeight(30)
            self._dot = dot
            self._tone = "idle"
            self.set_state(text, tone)

        def set_state(self, text: str, tone: str = "idle"):  # noqa: N802
            background, foreground, border = self.TONES.get(
                tone, self.TONES["idle"])
            self._tone = tone
            self._text = str(text)
            self.setText(f"● {text}" if self._dot and text else str(text))
            self.setStyleSheet(
                "QLabel#statusChip {"
                f"background-color: {background};"
                f"color: {foreground};"
                f"border: 1px solid {border};"
                "border-radius: 14px;"
                "padding: 0px 12px;"
                "font-size: 11px;"
                "font-weight: 700;"
                "letter-spacing: 0.3px;"
                "}")
            self.setToolTip(self._text)

        def tone(self) -> str:
            return self._tone

        def state_text(self) -> str:
            return self._text

    class IconButton(QPushButton):
        """Square, borderless Font Awesome icon button."""

        def __init__(self, icon_name: str, tooltip: str = "", parent=None,
                     *, checkable: bool = False, size: int = 34,
                     icon_size: int = 18, color: str = "#9AA0B0",
                     active_color: str = "#E7EBF2"):
            super().__init__(parent)
            self.setObjectName("iconButton")
            self.setFixedSize(int(size), int(size))
            self.setCursor(Qt.PointingHandCursor)
            self.setFocusPolicy(Qt.StrongFocus)
            self.setCheckable(bool(checkable))
            self._icon_size = int(icon_size)
            self._icon_color = color
            self._icon_active = active_color
            self._icon_name = ""
            self.set_icon(icon_name)
            if tooltip:
                self.setToolTip(tooltip)

        def set_icon(self, icon_name: str):
            """Swap the glyph (normal/mute/fullscreen state changes)."""
            self._icon_name = str(icon_name)
            if fa_icons is not None and fa_icons.available():
                self.setIcon(fa_icons.icon(
                    self._icon_name, self._icon_color, self._icon_size,
                    self._icon_active))
                self.setIconSize(QSize(self._icon_size, self._icon_size))
                self.setText("")
            else:
                self.setIcon(QIcon())
                self.setText(
                    fa_icons.fallback_text(self._icon_name)
                    if fa_icons is not None else "?")

        # Compatibility with earlier call sites.
        def set_icon_name(self, icon_name: str):
            self.set_icon(icon_name)

        def icon_name(self) -> str:
            return self._icon_name
