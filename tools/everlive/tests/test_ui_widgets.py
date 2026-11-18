#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Tests for the reusable Console Hub widgets.

These skip cleanly when PySide6 is not installed (CI compiles the Python
modules without a Qt runtime).
"""

import os
import sys
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import ui_widgets  # noqa: E402


@unittest.skipUnless(ui_widgets.HAS_QT, "PySide6 is not installed")
class WidgetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from PySide6.QtWidgets import QApplication
        cls.app = QApplication.instance() or QApplication([])

    def test_toggle_switch_round_trip(self):
        switch = ui_widgets.ToggleSwitch()
        seen = []
        switch.toggled.connect(seen.append)

        self.assertFalse(switch.isChecked())
        switch.setChecked(True)
        self.assertTrue(switch.isChecked())
        switch.click()
        self.assertFalse(switch.isChecked())
        self.assertEqual(seen, [True, False])

    def test_toggle_switch_snaps_when_hidden(self):
        switch = ui_widgets.ToggleSwitch()
        switch.setChecked(True)
        self.assertAlmostEqual(switch.get_knob_position(), 1.0)
        switch.setChecked(False)
        self.assertAlmostEqual(switch.get_knob_position(), 0.0)

    def test_toggle_row_mirrors_checkbox(self):
        from PySide6.QtWidgets import QCheckBox
        checkbox = QCheckBox("HDR")
        row = ui_widgets.ToggleRow("HDR", "Tonemap", checkbox=checkbox)

        self.assertFalse(row.isChecked())
        checkbox.setChecked(True)
        self.assertTrue(row.isChecked())

        row.setChecked(False)
        self.assertFalse(checkbox.isChecked())

        checkbox.setEnabled(False)
        self.assertFalse(row.isEnabled())
        checkbox.setEnabled(True)
        self.assertTrue(row.isEnabled())

    def test_toggle_row_hides_with_checkbox(self):
        from PySide6.QtWidgets import QCheckBox
        checkbox = QCheckBox("Legacy")
        row = ui_widgets.ToggleRow("Legacy", checkbox=checkbox)
        checkbox.show()
        row.show()
        checkbox.setVisible(False)
        self.assertTrue(row.isHidden())

    def test_segmented_control_data_round_trip(self):
        control = ui_widgets.SegmentedControl(
            [("HDMI 1", 0), ("HDMI 2", 1)])
        seen = []
        control.currentDataChanged.connect(seen.append)

        self.assertEqual(control.currentIndex(), 0)
        self.assertEqual(control.currentData(), 0)
        self.assertEqual(control.itemData(1), 1)

        control.setCurrentData(1)
        self.assertEqual(control.currentIndex(), 1)
        self.assertEqual(control.currentData(), 1)
        self.assertEqual(seen, [1])

    def test_segmented_control_click_is_exclusive(self):
        control = ui_widgets.SegmentedControl([("Simple", "simple"),
                                               ("Advanced", "advanced")])
        control._buttons[1].click()
        self.assertEqual(control.currentData(), "advanced")
        self.assertTrue(control._buttons[1].isChecked())
        self.assertFalse(control._buttons[0].isChecked())

        control._buttons[0].click()
        self.assertEqual(control.currentData(), "simple")
        self.assertFalse(control._buttons[1].isChecked())

    def test_segmented_control_set_same_index_does_not_reemit(self):
        control = ui_widgets.SegmentedControl([("A", 1), ("B", 2)])
        seen = []
        control.currentIndexChanged.connect(seen.append)
        control.setCurrentIndex(0)
        self.assertEqual(seen, [])
        control.setCurrentIndex(1)
        control.setCurrentIndex(1)
        self.assertEqual(seen, [1])

    def test_segmented_control_click_emits(self):
        # Regression: QButtonGroup checks the clicked button before the
        # clicked signal reaches us, so the change must be tracked.
        control = ui_widgets.SegmentedControl(
            [("Simple", "simple"), ("Advanced", "advanced")])
        seen = []
        control.currentDataChanged.connect(seen.append)

        control._buttons[1].click()
        self.assertEqual(seen, ["advanced"])
        control._buttons[1].click()
        self.assertEqual(seen, ["advanced"])
        control._buttons[0].click()
        self.assertEqual(seen, ["advanced", "simple"])

    def test_set_enabled_propagates_to_segments(self):
        control = ui_widgets.SegmentedControl([("A", 1), ("B", 2)])
        control.setEnabled(False)
        self.assertFalse(control._buttons[0].isEnabled())
        self.assertFalse(control._buttons[1].isEnabled())

    def test_status_chip_tones(self):
        chip = ui_widgets.StatusChip("Standby", "idle")
        self.assertEqual(chip.state_text(), "Standby")
        self.assertEqual(chip.tone(), "idle")
        chip.set_state("Local Capture Active", "active")
        self.assertEqual(chip.tone(), "active")
        self.assertEqual(chip.text(), "Local Capture Active")
        self.assertIn("#23423B", chip.styleSheet())

    def test_status_chip_optional_dot(self):
        chip = ui_widgets.StatusChip("Live", "live", dot=True)
        self.assertTrue(chip.text().startswith("●"))

    def test_icon_button_geometry(self):
        button = ui_widgets.IconButton("help", "Help", size=30)
        self.assertEqual(button.width(), 30)
        self.assertEqual(button.height(), 30)
        self.assertEqual(button.toolTip(), "Help")
        self.assertEqual(button.objectName(), "iconButton")

    def test_icon_button_renders_font_awesome(self):
        button = ui_widgets.IconButton("github", "GitHub")
        self.assertFalse(button.icon().isNull())
        self.assertEqual(button.icon_name(), "github")
        self.assertEqual(button.text(), "")

    def test_icon_button_can_swap_icon(self):
        button = ui_widgets.IconButton("volume", "Mute")
        button.set_icon("mute")
        self.assertEqual(button.icon_name(), "mute")
        button.set_icon_name("volume")
        self.assertEqual(button.icon_name(), "volume")

    def test_icon_button_checkable(self):
        button = ui_widgets.IconButton("⧉", "Pop-out", checkable=True)
        self.assertTrue(button.isCheckable())
        button.setChecked(True)
        self.assertTrue(button.isChecked())

    def test_collapsible_card_starts_expanded(self):
        card = ui_widgets.CollapsibleCard("TELEMETRY DECK")
        self.assertFalse(card.isCollapsed())
        self.assertFalse(card.body.isHidden())
        self.assertIn("TELEMETRY DECK", card.btn_header.text())
        self.assertTrue(card.btn_header.text().startswith("▾"))

    def test_collapsible_card_round_trip(self):
        card = ui_widgets.CollapsibleCard("DECK")
        seen = []
        card.collapsedChanged.connect(seen.append)

        card.setCollapsed(True)
        self.assertTrue(card.isCollapsed())
        self.assertTrue(card.body.isHidden())
        self.assertFalse(card.btn_header.isChecked())
        self.assertTrue(card.btn_header.text().startswith("▸"))

        card.setCollapsed(False)
        self.assertFalse(card.isCollapsed())
        self.assertFalse(card.body.isHidden())
        self.assertTrue(card.btn_header.isChecked())
        self.assertEqual(seen, [True, False])

    def test_collapsible_card_same_state_does_not_emit(self):
        card = ui_widgets.CollapsibleCard("DECK")
        seen = []
        card.collapsedChanged.connect(seen.append)
        card.setCollapsed(False)
        self.assertEqual(seen, [])

    def test_collapsible_card_heights(self):
        card = ui_widgets.CollapsibleCard("DECK")
        card.set_expanded_height(120)
        self.assertEqual(card.height(), 120)
        card.setCollapsed(True)
        self.assertEqual(card.height(), card.HEADER_HEIGHT)
        card.setCollapsed(False)
        self.assertEqual(card.height(), 120)

    def test_collapsible_card_summary(self):
        card = ui_widgets.CollapsibleCard("DECK")
        card.set_summary("60.0 FPS · LOCKED")
        self.assertEqual(card.lbl_summary.text(), "60.0 FPS · LOCKED")


if __name__ == "__main__":
    unittest.main()
