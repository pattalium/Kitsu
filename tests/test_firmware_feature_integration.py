from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")


def cpp_function(source: str, signature: str) -> str:
    start = 0
    while True:
        start = source.index(signature, start)
        opening = source.index("{", start)
        terminator = source.find(";", start, opening)
        if terminator == -1:
            break
        start += len(signature)
    depth = 0
    for cursor in range(opening, len(source)):
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
            if depth == 0:
                return source[start : cursor + 1]
    raise AssertionError(f"unterminated C++ function: {signature}")


class FirmwareFeatureIntegrationSourceTests(unittest.TestCase):
    def test_progression_tick_is_transactional_and_does_not_drop_callbacks(self):
        tick = cpp_function(MAIN, "void tickCompanionProgression(")
        for required in (
            "writeSnapshot(before",
            "writeSnapshot(after",
            "memcmp(before, after, snapshotBytes) != 0",
            "changed && !persistCompanionProgression()",
            "restoreSnapshot(\n        before, snapshotBytes",
            "progressionLastSessionDay = previousDay",
            "progressionLastMinute = previousMinute",
            "callbackReady = canPresent && !havePending",
            "progressionPendingLine = sessionLine",
        ):
            self.assertIn(required, tick)

    def test_profile_and_adventure_changes_roll_back_when_storage_fails(self):
        profile = cpp_function(MAIN, "bool executeProfileCommand(")
        self.assertGreaterEqual(profile.count("captureCompanionProgression(before)"), 2)
        self.assertGreaterEqual(profile.count("restoreCompanionProgression(before)"), 2)
        self.assertGreaterEqual(
            profile.count(
                "const kitsu868::activities::ActivityState before = "
                "activitySuite.snapshot()"
            ),
            3,
        )
        self.assertGreaterEqual(profile.count("activitySuite.restore(before)"), 3)

        persist = cpp_function(MAIN, "bool persistAdventureMutation(")
        self.assertLess(
            persist.index("persistAdventureProgression()"),
            persist.index("adventureProgression.restore(before)"),
        )
        adventure = cpp_function(MAIN, "bool executeAdventureCommand(")
        self.assertGreaterEqual(adventure.count("persistAdventureMutation(before"), 8)

    def test_game_activity_radio_and_clock_have_mutual_busy_guards(self):
        game = cpp_function(MAIN, "void startGame(")
        self.assertIn("if (radioListening)", game)
        self.assertIn("if (activityRuntimeBusy())", game)

        activity = cpp_function(MAIN, "bool startActivity(")
        self.assertIn("radioListening || activeGame != ActiveGame::None", activity)

        listening = cpp_function(MAIN, "bool startListening(")
        self.assertIn("activeGame != ActiveGame::None", listening)
        self.assertIn("if (activityRuntimeBusy())", listening)

        clock = cpp_function(MAIN, "void beginClockEditor(")
        self.assertIn('foregroundTransitionBlocked("clock")', clock)


if __name__ == "__main__":
    unittest.main()
