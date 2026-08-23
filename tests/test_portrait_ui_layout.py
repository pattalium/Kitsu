"""Host audit for the fixed 64x128 Kitsu OLED canvas.

The C++ helper is the firmware authority.  These tests independently exercise
its pixel metric, audit every literal OLED label in main.cpp, and require named
coverage for every Screen variant so a future screen cannot bypass the audit.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
LAYOUT = (ROOT / "src" / "portrait_ui_layout.h").read_text(encoding="utf-8")
HOST_TEST = (ROOT / "tests" / "portrait_ui_layout_host.cpp").read_text(
    encoding="utf-8"
)

CANVAS_WIDTH = 64
CANVAS_HEIGHT = 128
CONTENT_WIDTH = 60
GLYPH_WIDTH = 5
GLYPH_HEIGHT = 7


def text_width(characters: int, scale: int, advance: int) -> int:
    if not characters:
        return 0
    return (characters - 1) * advance + GLYPH_WIDTH * scale


def plan_text(characters: int, preferred_scale: int = 1) -> tuple[int, int, int, bool]:
    preferred_scale = max(1, min(2, preferred_scale))
    if not characters:
        return preferred_scale, 6 * preferred_scale, 0, False
    for scale in range(preferred_scale, 0, -1):
        for advance in (6 * scale, 5 * scale):
            width = text_width(characters, scale, advance)
            if width <= CONTENT_WIDTH:
                return scale, advance, width, False
    capacity = CONTENT_WIDTH // GLYPH_WIDTH
    return 1, GLYPH_WIDTH, text_width(capacity, 1, GLYPH_WIDTH), True


class PortraitPlannerContractTest(unittest.TestCase):
    def test_authoritative_constants_and_fail_closed_render_path(self) -> None:
        self.assertIn("constexpr int16_t kCanvasWidth = 64", LAYOUT)
        self.assertIn("constexpr int16_t kCanvasHeight = 128", LAYOUT)
        self.assertIn("constexpr int16_t kContentInset = 2", LAYOUT)
        self.assertIn("centeredTextFitsVertically", LAYOUT)
        self.assertIn("if (!plan.valid ||", MAIN)
        self.assertIn("centeredTextFitsVertically(y, plan)", MAIN)
        self.assertNotIn("(UI_WIDTH - uiTextWidth", MAIN)

    def test_scale_tracking_and_ellipsis_boundaries(self) -> None:
        self.assertEqual(plan_text(10), (1, 6, 59, False))
        self.assertEqual(plan_text(12), (1, 5, 60, False))
        self.assertEqual(plan_text(6, 2), (2, 10, 60, False))
        self.assertEqual(plan_text(13), (1, 5, 60, True))
        for length in range(0, 256):
            scale, advance, width, _ = plan_text(length, 2)
            self.assertIn(scale, (1, 2))
            self.assertIn(advance, (5, 6, 10, 12))
            self.assertLessEqual(width, CONTENT_WIDTH)

    def test_every_literal_draw_is_horizontally_and_vertically_safe(self) -> None:
        # String literals with a numeric y are the static half of the audit;
        # worst-case dynamic labels are enumerated in the native host harness.
        calls = re.findall(
            r'uiTextCentered(?:Fit)?\(\s*"([^"]*)"\s*,\s*(\d+)'
            r'(?:\s*,\s*(\d+))?',
            MAIN,
        )
        self.assertGreaterEqual(len(calls), 55)
        for label, y_text, scale_text in calls:
            with self.subTest(label=label, y=y_text):
                preferred = int(scale_text or 1)
                scale, _, width, _ = plan_text(len(label), preferred)
                y = int(y_text)
                self.assertLessEqual(width, CONTENT_WIDTH)
                self.assertGreaterEqual(y, 0)
                self.assertLessEqual(y + GLYPH_HEIGHT * scale, CANVAS_HEIGHT)

    def test_every_screen_and_phone_state_has_native_geometry_coverage(self) -> None:
        enum_match = re.search(r"enum class Screen[^\{]*\{([^}]+)\}", MAIN, re.S)
        self.assertIsNotNone(enum_match)
        screens = {
            token.strip().rstrip(",")
            for token in enum_match.group(1).splitlines()
            if token.strip()
        }
        expected = {
            "Pet",
            "Menu",
            "Connect",
            "Inbox",
            "GameMenu",
            "Game",
            "Listen",
            "Sleep",
            "Status",
            "PairPhone",
            "ControllerManager",
            "ControllerConfirm",
            "ControllerResult",
        }
        self.assertEqual(screens, expected)
        for native_test in (
            "testPetMissingPackAndHatch",
            "testMainAndGameMenus",
            "testConnectVariants",
            "testInboxVariants",
            "testActiveGames",
            "testListenAndSleep",
            "testStatusPages",
            "testPairPhoneVariants",
            "testControllerRecoveryVariants",
        ):
            self.assertIn(native_test, HOST_TEST)
        for phone_state in (
            "unavailable",
            "comparison",
            "grant",
            "authenticated",
            "securing",
            "open",
            "closed",
        ):
            self.assertRegex(HOST_TEST, rf"const Label {phone_state}\[\]")

    def test_dynamic_and_multiline_paths_are_bounded(self) -> None:
        self.assertIn("oledSafeText(rawText.c_str())", MAIN)
        self.assertIn("portrait::lineCapacity", MAIN)
        self.assertIn("line += \"..\"", MAIN)
        self.assertIn('String(minutes / 1440U) + " DAYS"', MAIN)
        self.assertIn('uiTextCentered(String(seconds) + "S", 49, 2)', MAIN)
        self.assertIn('uiTextCentered(passkey, 42, 2)', MAIN)
        self.assertIn('uiTextCentered("WINDOW", 29, 2)', MAIN)
        self.assertIn('uiTextCentered("CLOSED", 48, 2)', MAIN)

    def test_nine_item_indicator_is_width_aware(self) -> None:
        # 9 * 3 px dots plus eight legacy 5 px gaps was 67 px.  The shared
        # planner reduces the gap to 4 px: 59 px, centered inside 64 px.
        count, size, max_width = 9, 3, CONTENT_WIDTH
        gap = min(5, (max_width - count * size) // (count - 1))
        self.assertEqual(gap, 4)
        self.assertEqual(count * size + (count - 1) * gap, 59)
        self.assertIn("portrait::planDots", MAIN)


if __name__ == "__main__":
    unittest.main(verbosity=2)
