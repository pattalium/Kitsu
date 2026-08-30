import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
SESSION = (ROOT / "src" / "kitsu_ble_session.cpp").read_text(encoding="utf-8")
PERMISSIONS = (ROOT / "src" / "kitsu_controller_permissions.cpp").read_text(
    encoding="utf-8"
)
FOCUS = (ROOT / "src" / "kitsu_focus_session.cpp").read_text(encoding="utf-8")
PRESENCE = (ROOT / "src" / "kitsu_pet_presence.cpp").read_text(encoding="utf-8")


class PetCompanionFeatureIntegrationTest(unittest.TestCase):
    def test_new_pet_operations_are_authenticated_and_dispatched(self):
        operations = (
            "companion.profile.get.v1",
            "companion.profile.nickname.set.v1",
            "companion.request.answer.v1",
            "companion.question.answer.v1",
            "companion.presentation.open.v1",
            "companion.presentation.read.v1",
            "companion.presentation.close.v1",
            "focus.state.get.v1",
            "focus.start.v1",
            "focus.stop.v1",
            "focus.cancel.v1",
            "focus.ack.v1",
            "adventure.state.get.v1",
            "adventure.walk.start.v1",
            "adventure.walk.sync.v1",
            "adventure.walk.location.v1",
            "adventure.walk.decide.v1",
            "adventure.walk.finish.v1",
            "adventure.walk.ack.v1",
            "adventure.privacy.set.v1",
            "adventure.home.set.v1",
        )
        allowed = PERMISSIONS
        dispatch = MAIN.split("handleCompanionBleRequest(", 2)[2].split(
            "return companion_api::copyResponse", 1
        )[0]
        for operation in operations:
            self.assertIn(f'"{operation}"', allowed)
            self.assertIn(f'"{operation}"', dispatch)

    def test_focus_is_durable_but_does_not_drive_care_or_sleep(self):
        load = MAIN.split("void loadState()", 1)[1].split(
            "// The OLED remains", 1
        )[0]
        loop = MAIN.split("void loop()", 1)[1]
        self.assertIn("loadFocusState();", load)
        self.assertIn("tickFocus(now);", loop)
        self.assertIn('writePreferenceRecord("focus_v1", state)', MAIN)
        self.assertIn("FOCUS_CHECKPOINT_MS", MAIN)
        for forbidden in (
            "wisp.energy",
            "wisp.affection",
            "wisp.sleeping",
            "quietHours",
            "activitySuite.start",
        ):
            self.assertNotIn(forbidden, FOCUS)

    def test_walk_sync_uses_absolute_totals_and_rejects_stale_samples(self):
        walk = MAIN.split("struct WalkSyncRequest", 1)[1].split(
            "bool startFunExpedition", 1
        )[0]
        self.assertIn('"steps_total"', walk)
        self.assertIn('"distance_meters_total"', walk)
        self.assertIn("request.stepsTotal < current.steps", walk)
        self.assertIn("request.distanceTotal < current.routeDistanceMeters", walk)
        self.assertIn("request.stepsTotal - current.steps", walk)
        self.assertIn("request.distanceTotal - current.routeDistanceMeters", walk)

    def test_pet_engines_do_not_depend_on_meshcore_or_inventory(self):
        for source in (FOCUS, PRESENCE):
            self.assertNotIn("MeshCore", source)
            self.assertNotIn("meshTransport", source)
            self.assertNotIn("gift", source.lower())
            self.assertNotIn("inventory", source.lower())


if __name__ == "__main__":
    unittest.main()
