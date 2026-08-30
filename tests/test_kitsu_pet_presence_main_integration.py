from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")


def cpp_function(source: str, signature: str) -> str:
    """Return a C++ function body while skipping an earlier declaration."""
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


class PetPresenceMainIntegrationSourceTests(unittest.TestCase):
    def test_tracker_is_bounded_and_owned_by_the_nearby_pet_path(self) -> None:
        self.assertIn('#include "kitsu_pet_presence.h"', MAIN)
        self.assertIn(
            "kitsu868::presence::PetPresenceTracker petPresenceTracker;", MAIN
        )
        self.assertIn("NEARBY_NEIGHBOR_CAPACITY = 8U", MAIN)

        observe = cpp_function(MAIN, "ObserveResult observePetPresence(")
        self.assertIn("petPresenceTracker.observe(observation)", observe)
        self.assertIn("observation.uid = packet.sourceUid", observe)
        self.assertIn("observation.rssiDbm = rssi", observe)
        self.assertIn("observation.snrDb = snr", observe)
        self.assertIn("observation.observedAtMs = now", observe)
        for forbidden in (
            "companionBrain.onEncounter",
            "saveState(",
            "persist",
            "meshTransport.",
        ):
            self.assertNotIn(forbidden, observe)

    def test_heartbeats_are_periodic_only_during_normal_listen(self) -> None:
        send = cpp_function(MAIN, "bool sendNearbyPresence(")
        self.assertIn(
            "presence.type = kitsu868::nearby::PacketType::Presence", send
        )
        self.assertIn("const bool sent = sendNearbyPacket(presence)", send)
        self.assertIn("NEARBY_PRESENCE_HEARTBEAT_MIN_MS", send)
        self.assertIn("NEARBY_PRESENCE_HEARTBEAT_MAX_MS", send)
        self.assertIn("NEARBY_PRESENCE_RETRY_MS", send)

        tick = cpp_function(MAIN, "void tickPetPresence(")
        self.assertIn("petPresenceTracker.expire(now)", tick)
        self.assertIn("if (!radioListening || partyRuntimeBusy()) return;", tick)
        self.assertIn("now - nextNearbyPresenceAt", tick)
        self.assertIn("sendNearbyPresence()", tick)
        for forbidden in (
            "saveState(",
            "persist",
            "onEncounter(",
            "introduce(",
            "sendFloodAdvert",
        ):
            self.assertNotIn(forbidden, tick)

        start = cpp_function(MAIN, "bool startListening(")
        self.assertIn("nextNearbyPresenceAt = 0U", start)
        self.assertIn("(void)sendNearbyPresence()", start)
        stop = cpp_function(MAIN, "void stopListening(")
        self.assertIn("nextNearbyPresenceAt = 0U", stop)

    def test_receive_gates_run_before_tracking_and_rewards_stay_separate(self) -> None:
        receive = cpp_function(MAIN, "void processNearbyPresence(")
        gate = receive.index("if (!radioListening")
        first_observe = receive.index("observePetPresence(")
        encounter = receive.index("companionBrain.onEncounter(")
        second_observe = receive.index("observePetPresence(", encounter)

        self.assertLess(gate, first_observe)
        self.assertIn("packet.sourceUid == ownUidSuffix()", receive[gate:first_observe])
        self.assertIn("packet.targetUid != ownUidSuffix()", receive[gate:first_observe])
        self.assertLess(first_observe, encounter)
        self.assertGreater(second_observe, encounter)
        self.assertEqual(2, receive.count("observePetPresence("))
        self.assertIn("now, false", receive[first_observe:encounter])
        self.assertIn("now, !result.newEncounter", receive[second_observe:])
        self.assertIn("if (result.newEncounter)", receive)
        self.assertIn("recordSuccessfulEncounterTrigger(", receive)

    def test_listen_ui_reports_signal_state_without_direction_or_distance(self) -> None:
        render = cpp_function(MAIN, "void renderListen(")
        for expected in (
            "petPresenceTracker.summary()",
            "SignalTrend::Approaching",
            "SIGNAL STRONGER",
            "SignalTrend::Leaving",
            "SIGNAL FADING",
            "SignalBand::Weak",
            'signalLabel = "SIGNAL WEAK"',
            "SignalBand::Medium",
            'signalLabel = "SIGNAL MEDIUM"',
            "SignalBand::Strong",
            'signalLabel = "SIGNAL STRONG"',
            'String("GROUP ")',
            'detailLabel = "FAMILIAR"',
        ):
            self.assertIn(expected, render)
        lowered = render.lower()
        for forbidden in ("bearing", "direction", "distance", "nearest"):
            self.assertNotIn(forbidden, lowered)

    def test_presence_events_do_not_interrupt_finite_reactions(self) -> None:
        present = cpp_function(MAIN, "void presentPetPresenceEvents(")
        self.assertIn("activeAnimation.active && activeAnimation.finite", present)
        self.assertIn("EventGroupStarted", present)
        self.assertIn("EventAppeared", present)
        self.assertIn("EventFamiliar", present)
        self.assertIn("EventApproaching", present)
        self.assertIn("EventLeaving", present)
        self.assertIn("EventGone", present)

        loop = cpp_function(MAIN, "void loop() {")
        self.assertLess(
            loop.index("processNearbyRadio()"), loop.index("tickPetPresence(now)")
        )


if __name__ == "__main__":
    unittest.main()
