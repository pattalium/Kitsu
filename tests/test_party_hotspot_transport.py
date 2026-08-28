from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TRANSPORT = (ROOT / "src" / "kitsu_mesh_transport.cpp").read_text(
    encoding="utf-8"
)
TRANSPORT_HEADER = (ROOT / "src" / "kitsu_mesh_transport.h").read_text(
    encoding="utf-8"
)
PARTY_HEADER = (ROOT / "src" / "kitsu_party_hotspot.h").read_text(
    encoding="utf-8"
)


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


class PartyHotspotTransportSourceTests(unittest.TestCase):
    def test_bounded_queue_covers_exact_party_wire(self) -> None:
        self.assertIn('#include "kitsu_party_hotspot.h"', TRANSPORT)
        self.assertRegex(PARTY_HEADER, r"kMagic0\s*=\s*0x50U")
        self.assertRegex(PARTY_HEADER, r"kMagic1\s*=\s*0x38U")
        self.assertRegex(PARTY_HEADER, r"kWireBytes\s*=\s*30U")
        capacity = int(
            re.search(
                r"kNearbyRadioFrameBytes\s*=\s*(\d+)U", TRANSPORT_HEADER
            ).group(1)
        )
        self.assertGreaterEqual(capacity, 30)
        self.assertIn("party::kWireBytes <= kNearbyRadioFrameBytes", TRANSPORT)
        self.assertIn("nearby::kWireBytes <= kNearbyRadioFrameBytes", TRANSPORT)
        self.assertIn("nearby-v2 or party-v1", TRANSPORT_HEADER)

    def test_raw_admission_keeps_nearby_and_adds_exact_party_candidate(self) -> None:
        candidate = cpp_function(
            TRANSPORT, "bool isKitsuDirectRadioCandidate("
        )
        for required in (
            "byteCount == nearby::kWireBytes",
            "bytes[0] == nearby::kMagic0",
            "bytes[1] == nearby::kMagic1",
            "bytes[2] == nearby::kProtocolVersion",
            "byteCount == party::kWireBytes",
            "bytes[0] == party::kMagic0",
            "bytes[1] == party::kMagic1",
            "bytes[2] == party::kProtocolVersion",
            "nearbyCandidate || partyCandidate",
        ):
            self.assertIn(required, candidate)

        raw = cpp_function(TRANSPORT, "void logRxRaw(")
        self.assertIn("isKitsuDirectRadioCandidate(raw", raw)
        self.assertIn("sink_->captureNearbyRadio(raw", raw)
        self.assertLess(
            raw.index("isKitsuDirectRadioCandidate(raw"),
            raw.index("decodeRepeatWire("),
        )
        self.assertLess(
            raw.index("sink_->captureNearbyRadio(raw"),
            raw.index("decodeRepeatWire("),
        )

    def test_capture_fully_validates_both_families_and_preserves_bytes(self) -> None:
        classify = cpp_function(
            TRANSPORT, "DirectRadioFrameKind classifyKitsuDirectRadioFrame("
        )
        for required in (
            "nearby::decode(bytes, byteCount, packet) != nearby::Status::Ok",
            "party::decode(bytes, byteCount, packet) != party::Status::Ok",
            "nearby::PacketType::Presence",
            "party::PacketType::Beacon",
            "DirectRadioFrameKind::Invalid",
        ):
            self.assertIn(required, classify)

        implementation = TRANSPORT[
            TRANSPORT.index("struct KitsuMeshTransport::Impl") :
        ]
        capture = cpp_function(implementation, "void captureNearbyRadio(")
        self.assertIn("classifyKitsuDirectRadioFrame(bytes, byteCount)", capture)
        self.assertIn("DirectRadioFrameKind::Invalid", capture)
        self.assertIn("memcpy(frame.bytes, bytes, byteCount)", capture)
        self.assertIn("frame.length = static_cast<uint8_t>(byteCount)", capture)
        self.assertNotIn("encode(", capture)
        self.assertLess(
            capture.index("classifyKitsuDirectRadioFrame(bytes, byteCount)"),
            capture.index("memcpy(frame.bytes, bytes, byteCount)"),
        )

    def test_sender_validates_then_sends_original_party_or_nearby_frame(self) -> None:
        send = cpp_function(
            TRANSPORT,
            "TransportStatus KitsuMeshTransport::sendNearbyRadioFrame(",
        )
        for required in (
            "classifyKitsuDirectRadioFrame(bytes, byteCount)",
            "frameKind == DirectRadioFrameKind::Invalid",
            "usesDiscoveryCooldown(frameKind)",
            "armOneShotForPacket(settings, true, bytes, byteCount)",
            "sendDirectOneShotRaw(\n      bytes, static_cast<int>(byteCount))",
        ):
            self.assertIn(required, send)
        self.assertNotIn("encode(", send)
        self.assertLess(
            send.index("classifyKitsuDirectRadioFrame(bytes, byteCount)"),
            send.index("armOneShotForPacket(settings, true, bytes, byteCount)"),
        )

        cooldown = cpp_function(TRANSPORT, "bool usesDiscoveryCooldown(")
        self.assertIn("DirectRadioFrameKind::NearbyPresence", cooldown)
        self.assertIn("DirectRadioFrameKind::PartyBeacon", cooldown)
        self.assertNotIn("DirectRadioFrameKind::PartySession", cooldown)


if __name__ == "__main__":
    unittest.main()
