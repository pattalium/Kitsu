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


class PartyModesMainIntegrationSourceTests(unittest.TestCase):
    def test_m8_is_demultiplexed_before_p8_and_nearby(self) -> None:
        process = cpp_function(MAIN, "void processNearbyRadio(")
        m8 = process.index(
            "frame.length == kitsu868::party_modes::kWireBytes"
        )
        p8 = process.index("frame.length == kitsu868::party::kWireBytes")
        nearby = process.index("kitsu868::nearby::decode(")
        self.assertLess(m8, p8)
        self.assertLess(p8, nearby)
        self.assertIn(
            "processModePartyRadioPacket(modePacket, frame.rssi, frame.snr)",
            process[m8:p8],
        )
        self.assertIn("continue;", process[m8:p8])

    def test_p8_and_m8_share_busy_guards_and_one_leave_path(self) -> None:
        busy = cpp_function(MAIN, "bool partyRuntimeBusy(")
        for required in (
            "partyScanActive",
            "partyJoinRequested",
            "partyWelcomeAccepted",
            "modePartyScanActive",
            "modePartyJoinRequested",
            "modePartyAutoReadyPending",
            "partyHost.state().phase",
            "partyParticipant.state().phase",
            "modePartyHost.state().phase",
            "modePartyParticipant.state().phase",
        ):
            self.assertIn(required, busy)

        for signature in (
            "bool startPartyScan(",
            "bool startPartyHost(",
            "bool startModePartyScan(",
            "bool startModePartyHost(",
        ):
            start = cpp_function(MAIN, signature)
            self.assertIn("if (partyRuntimeBusy())", start)
            self.assertIn('error = "party_busy"', start)

        leave = cpp_function(MAIN, "void leavePartyHotspot(")
        for required in (
            "partyHost.cancel(",
            "modePartyHost.cancel(",
            "partyHost.reset()",
            "partyParticipant.reset()",
            "modePartyHost.reset()",
            "modePartyParticipant.reset()",
            "resetPartyRuntimeView()",
            "resetModePartyRuntimeView()",
        ):
            self.assertIn(required, leave)

        stop = cpp_function(MAIN, "void stopListeningSafely(")
        self.assertIn("if (partyRuntimeBusy())", stop)
        self.assertIn("leavePartyHotspot()", stop)

    def test_rotating_host_falls_back_to_signal_hunt_and_self_readies(self) -> None:
        rotating = cpp_function(MAIN, "bool startRotatingModePartyHost(")
        self.assertIn("kitsu868::party_modes::rotatingMode(", rotating)
        self.assertIn("return startModePartyHost(mode, error)", rotating)

        host = cpp_function(MAIN, "bool startModePartyHost(")
        fallback = host.index(
            "mode == kitsu868::party_modes::Mode::SignalHunt"
        )
        self.assertIn("return startPartyHost(error)", host[fallback:])
        self.assertIn("modePartyHost.setHostReady(true)", host)
        self.assertIn("ready=true", host)

        begin = cpp_function(MAIN, "bool beginModeParty(")
        self.assertIn("if (!modePartyHost.canStart())", begin)
        self.assertIn('error = "party_not_ready"', begin)

        tick = cpp_function(MAIN, "void tickModeParty(")
        lobby = tick.index(
            "hostPhase == kitsu868::party_modes::HostPhase::Lobby"
        )
        active = tick.index(
            "hostPhase == kitsu868::party_modes::HostPhase::Active"
        )
        lobby_path = tick[lobby:active]
        self.assertIn("modePartyHost.canStart()", lobby_path)
        self.assertIn("beginModeParty(error)", lobby_path)

    def test_active_hosts_keep_beaconing_and_accept_late_joins(self) -> None:
        tick = cpp_function(MAIN, "void tickModeParty(")
        active = tick.index(
            "hostPhase == kitsu868::party_modes::HostPhase::Active"
        )
        guest = tick.index(
            "const kitsu868::party_modes::ParticipantState guest", active
        )
        active_path = tick[active:guest]
        self.assertIn("modePartyHost.makeBeacon(now, beacon)", active_path)
        self.assertIn("sendModePartyPacket(beacon, false)", active_path)
        self.assertIn("modePartyHost.advance(now, next)", active_path)
        self.assertLess(
            active_path.index("modePartyHost.makeBeacon(now, beacon)"),
            active_path.index("modePartyHost.advance(now, next)"),
        )

        receive = cpp_function(MAIN, "void processModePartyRadioPacket(")
        self.assertIn("modePartyHostInProgress(", receive)
        self.assertIn(
            "packet.type == kitsu868::party_modes::PacketType::JoinRequest",
            receive,
        )
        self.assertIn("modePartyHost.acceptJoin(packet, now, welcome)", receive)

    def test_mutating_radio_sends_restore_the_pre_send_snapshot(self) -> None:
        cases = (
            (
                "bool startModePartyHost(",
                "modePartyHost.snapshot()",
                "sendModePartyPacket(beacon, false)",
                "modePartyHost.restore(before)",
            ),
            (
                "bool joinObservedModeParty(",
                "modePartyParticipant.snapshot()",
                "sendModePartyPacket(request, true)",
                "modePartyParticipant.restore(before)",
            ),
            (
                "bool setModePartyReady(",
                "modePartyParticipant.snapshot()",
                "sendModePartyPacket(packet, true)",
                "modePartyParticipant.restore(guest)",
            ),
            (
                "bool beginModeParty(",
                "modePartyHost.snapshot()",
                "sendModePartyPacket(roundOpen, true)",
                "modePartyHost.restore(before)",
            ),
            (
                "bool submitModePartyContribution(",
                "modePartyParticipant.snapshot()",
                "sendModePartyPacket(packet, true)",
                "modePartyParticipant.restore(guest)",
            ),
        )
        for signature, snapshot, send, restore in cases:
            with self.subTest(signature=signature):
                function = cpp_function(MAIN, signature)
                self.assertIn(snapshot, function)
                self.assertIn(send, function)
                self.assertIn(restore, function)
                self.assertLess(function.index(snapshot), function.index(send))
                self.assertLess(function.index(send), function.index(restore))

        receive = cpp_function(MAIN, "void processModePartyRadioPacket(")
        join = receive.index(
            "packet.type == kitsu868::party_modes::PacketType::JoinRequest"
        )
        ready = receive.index(
            "packet.type == kitsu868::party_modes::PacketType::Ready", join
        )
        join_path = receive[join:ready]
        self.assertIn("modePartyHost.snapshot()", join_path)
        self.assertIn("sendModePartyPacket(welcome, true)", join_path)
        self.assertIn("modePartyHost.restore(before)", join_path)

        tick = cpp_function(MAIN, "void tickModeParty(")
        # Lobby beacon, active beacon, and round/result advancement each take
        # a snapshot and restore it if their on-air mutation cannot be sent.
        self.assertGreaterEqual(tick.count("modePartyHost.snapshot()"), 5)
        self.assertGreaterEqual(tick.count("modePartyHost.restore(before)"), 3)

    def test_guest_auto_ready_is_delayed_until_the_radio_retry_window(self) -> None:
        receive = cpp_function(MAIN, "void processModePartyRadioPacket(")
        welcome = receive.index(
            "packet.type == kitsu868::party_modes::PacketType::Welcome"
        )
        round_open = receive.index(
            "kitsu868::party_modes::PacketType::RoundOpen", welcome
        )
        welcome_path = receive[welcome:round_open]
        self.assertIn("modePartyAutoReadyPending", welcome_path)
        self.assertIn(
            "admitted == kitsu868::party_modes::ParticipantPhase::Lobby",
            welcome_path,
        )
        self.assertNotIn("setModePartyReady", welcome_path)

        tick = cpp_function(MAIN, "void tickModeParty(")
        auto_ready = tick.index("modePartyAutoReadyPending &&")
        auto_path = tick[auto_ready:]
        self.assertIn(
            "guestPhase ==\n                   "
            "kitsu868::party_modes::ParticipantPhase::Lobby",
            auto_path,
        )
        self.assertIn("now - lastModePartyTxAt) >= 1000UL", auto_path)
        self.assertIn("setModePartyReady(true, error)", auto_path)

    def test_serial_surface_exposes_every_mode_party_operation_and_help(self) -> None:
        social = cpp_function(MAIN, "bool executeSocialCommand(")
        commands = {
            'command == "social scan"': "startAllPartyScans(error)",
            'command == "social host"': "startRotatingModePartyHost(error)",
            'command.startsWith("social host ")': "startModePartyHost(mode, error)",
            'command == "social join"': "joinObservedModeParty(error)",
            'command == "social ready"': "setModePartyReady(ready, error)",
            'command == "social begin"': "beginModeParty(error)",
            'command.startsWith("social contribute ")': (
                "submitModePartyContribution(value, error)"
            ),
            'command == "social leave"': "leavePartyHotspot()",
        }
        for command, action in commands.items():
            with self.subTest(command=command):
                self.assertIn(command, social)
                self.assertIn(action, social[social.index(command) :])

        help_text = (
            "social <status|leaderboard|scan|host [mode]|join|ready [0|1]|"
            "begin|contribute value|leave>"
        )
        self.assertIn(help_text, MAIN)

    def test_cooperative_outcomes_persist_under_the_raw_session_nonce(self) -> None:
        outcome = cpp_function(MAIN, "bool applyModePartySocialOutcome(")
        self.assertIn("socialProgression.snapshot()", outcome)
        self.assertIn("signalTrail.snapshot()", outcome)
        self.assertIn("signalTrail.mergeSharedMissCount(", outcome)
        self.assertIn("persistSignalEncounterState()", outcome)
        self.assertLess(
            outcome.index("signalTrail.mergeSharedMissCount("),
            outcome.index("if (!socialProgressionReady) return true"),
        )
        self.assertIn(
            "socialProgression.recordCooperativeRareEncounter(rawNonce)",
            outcome,
        )
        self.assertIn(
            "socialProgression.recordSharedTrailResult(\n"
            "            rawNonce, static_cast<uint8_t>(result.outcomeValue))",
            outcome,
        )
        self.assertIn("persistSocialProgression()", outcome)
        self.assertGreaterEqual(
            outcome.count("socialProgression.restore(before)"), 3
        )
        self.assertNotIn("modePartyRewardNonce(rawNonce)", outcome)

    def test_signal_hunt_rotation_has_truthful_serial_controls(self) -> None:
        social = cpp_function(MAIN, "bool executeSocialCommand(")
        self.assertIn('"protocol\\\":\\\"p8\\\"', social)
        begin = social.index('command == "social begin"')
        contribute = social.index('command.startsWith("social contribute ")')
        leave = social.index('command == "social leave"')
        self.assertIn("beginHostedParty(error)", social[begin:contribute])
        self.assertIn("choosePartySignal(", social[contribute:leave])
        self.assertIn("value + 1L", social[contribute:leave])
        self.assertIn("ready_not_required", social)
        self.assertIn("idleSignalHunt ? \"p8\" : \"m8\"", social)
        self.assertIn("idleSignalHunt ? \"false\" : \"true\"", social)

        scan = cpp_function(MAIN, "bool startAllPartyScans(")
        self.assertIn("startModePartyScan(error)", scan)
        self.assertIn("partyScanActive = true", scan)
        join = social.index('command == "social join"')
        ready = social.index('command == "social ready"', join)
        self.assertIn("joinObservedParty(", social[join:ready])
        self.assertIn("joinObservedModeParty(error)", social[join:ready])
        self.assertIn("modePartyParticipant.reset()", social[join:ready])
        self.assertIn("partyParticipant.reset()", social[join:ready])

    def test_legacy_rewards_get_a_domain_scoped_nonce(self) -> None:
        nonce = cpp_function(MAIN, "uint32_t modePartyRewardNonce(")
        self.assertIn('UINT32_C(0x4D385231)', nonce)  # "M8R1"
        self.assertIn("rawNonce ^", nonce)

        finish = cpp_function(MAIN, "void finishModePartyIfComplete(")
        self.assertIn(
            "const uint32_t rewardNonce = modePartyRewardNonce(rawNonce)",
            finish,
        )
        self.assertIn("showPartyCompletion(adapted, rewardNonce)", finish)
        self.assertIn(
            "recordPartyReward(adapted, rewardNonce, participants, "
            "participantCount",
            finish,
        )
        self.assertIn(
            "applyModePartySocialOutcome(mode, result, rawNonce)", finish
        )
        self.assertNotIn(
            "recordPartyReward(adapted, rawNonce", finish
        )

    def test_guest_reward_roster_contains_only_truthfully_known_uids(self) -> None:
        finish = cpp_function(MAIN, "void finishModePartyIfComplete(")
        guest = finish.index(
            "const kitsu868::party_modes::ParticipantState guest"
        )
        handled = finish.index("lastModePartyHandledSessionNonce", guest)
        guest_path = finish[guest:handled]
        self.assertIn("participants[0] = guest.localUid", guest_path)
        self.assertIn("participants[1] = guest.hostUid", guest_path)
        self.assertIn("participantCount = 2U", guest_path)
        self.assertNotIn("guest.participantCount", guest_path)

        host_path = finish[:guest]
        self.assertIn("host.members[index].active", host_path)
        self.assertIn(
            "participants[participantCount++] = host.members[index].uid",
            host_path,
        )


if __name__ == "__main__":
    unittest.main()
