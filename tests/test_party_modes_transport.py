from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TRANSPORT = (ROOT / "src" / "kitsu_mesh_transport.cpp").read_text(
    encoding="utf-8"
)
PARTY_HEADER = (ROOT / "src" / "kitsu_party_hotspot.h").read_text(
    encoding="utf-8"
)
MODES_HEADER = (ROOT / "src" / "kitsu_party_modes.h").read_text(
    encoding="utf-8"
)
IMPLEMENTATION = TRANSPORT[TRANSPORT.index("struct KitsuMeshTransport::Impl") :]


def cpp_function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for cursor in range(opening, len(source)):
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
            if depth == 0:
                return source[start : cursor + 1]
    raise AssertionError(f"unterminated C++ function: {signature}")


class PartyModesTransportSourceTests(unittest.TestCase):
    def test_m8_exact_wire_candidate_is_admitted(self) -> None:
        self.assertIn('#include "kitsu_party_modes.h"', TRANSPORT)
        self.assertRegex(MODES_HEADER, r"kMagic0\s*=\s*0x4DU")
        self.assertRegex(MODES_HEADER, r"kMagic1\s*=\s*0x38U")
        self.assertRegex(MODES_HEADER, r"kWireBytes\s*=\s*32U")
        self.assertIn(
            "party_modes::kWireBytes <= kNearbyRadioFrameBytes", TRANSPORT
        )

        candidate = cpp_function(
            TRANSPORT, "bool isKitsuDirectRadioCandidate("
        )
        for required in (
            "byteCount == party_modes::kWireBytes",
            "bytes[0] == party_modes::kMagic0",
            "bytes[1] == party_modes::kMagic1",
            "bytes[2] == party_modes::kProtocolVersion",
            "nearbyCandidate || partyCandidate || partyModeCandidate",
        ):
            self.assertIn(required, candidate)

    def test_valid_m8_beacon_and_session_use_party_routing(self) -> None:
        classify = cpp_function(
            TRANSPORT, "DirectRadioFrameKind classifyKitsuDirectRadioFrame("
        )
        m8_start = classify.index("byteCount == party_modes::kWireBytes")
        m8_branch = classify[m8_start:]
        for required in (
            "party_modes::decode(bytes, byteCount, packet)",
            "party_modes::Status::Ok",
            "party_modes::PacketType::Beacon",
            "DirectRadioFrameKind::PartyBeacon",
            "DirectRadioFrameKind::PartySession",
        ):
            self.assertIn(required, m8_branch)

        # Both capture and transmit use this classifier, so a valid M8 Beacon
        # gets discovery cooldown while every other valid M8 packet follows the
        # established party-session route.
        capture = cpp_function(IMPLEMENTATION, "void captureNearbyRadio(")
        send = cpp_function(
            TRANSPORT, "TransportStatus KitsuMeshTransport::sendNearbyRadioFrame("
        )
        self.assertIn(
            "classifyKitsuDirectRadioFrame(bytes, byteCount)", capture
        )
        self.assertIn("classifyKitsuDirectRadioFrame(bytes, byteCount)", send)
        cooldown = cpp_function(TRANSPORT, "bool usesDiscoveryCooldown(")
        self.assertIn("DirectRadioFrameKind::PartyBeacon", cooldown)
        self.assertNotIn("DirectRadioFrameKind::PartySession", cooldown)

    def test_malformed_m8_is_rejected_after_exact_magic_admission(self) -> None:
        classify = cpp_function(
            TRANSPORT, "DirectRadioFrameKind classifyKitsuDirectRadioFrame("
        )
        m8_start = classify.index("byteCount == party_modes::kWireBytes")
        m8_branch = classify[m8_start:]
        self.assertRegex(
            m8_branch,
            r"party_modes::decode\(bytes, byteCount, packet\)\s*!=\s*"
            r"party_modes::Status::Ok\)\s*\{\s*return "
            r"DirectRadioFrameKind::Invalid;",
        )

        capture = cpp_function(IMPLEMENTATION, "void captureNearbyRadio(")
        send = cpp_function(
            TRANSPORT, "TransportStatus KitsuMeshTransport::sendNearbyRadioFrame("
        )
        self.assertIn(
            "classifyKitsuDirectRadioFrame(bytes, byteCount)", capture
        )
        self.assertIn("DirectRadioFrameKind::Invalid", capture)
        self.assertIn("frameKind == DirectRadioFrameKind::Invalid", send)

    def test_existing_p8_admission_and_classification_are_unchanged(self) -> None:
        self.assertRegex(PARTY_HEADER, r"kMagic0\s*=\s*0x50U")
        self.assertRegex(PARTY_HEADER, r"kMagic1\s*=\s*0x38U")
        self.assertRegex(PARTY_HEADER, r"kWireBytes\s*=\s*30U")

        candidate = cpp_function(
            TRANSPORT, "bool isKitsuDirectRadioCandidate("
        )
        for required in (
            "byteCount == party::kWireBytes",
            "bytes[0] == party::kMagic0",
            "bytes[1] == party::kMagic1",
            "bytes[2] == party::kProtocolVersion",
        ):
            self.assertIn(required, candidate)

        classify = cpp_function(
            TRANSPORT, "DirectRadioFrameKind classifyKitsuDirectRadioFrame("
        )
        p8_start = classify.index("byteCount == party::kWireBytes")
        m8_start = classify.index("byteCount == party_modes::kWireBytes")
        p8_branch = classify[p8_start:m8_start]
        for required in (
            "party::decode(bytes, byteCount, packet)",
            "party::Status::Ok",
            "party::PacketType::Beacon",
            "DirectRadioFrameKind::PartyBeacon",
            "DirectRadioFrameKind::PartySession",
        ):
            self.assertIn(required, p8_branch)


if __name__ == "__main__":
    unittest.main()
