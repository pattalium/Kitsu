from __future__ import annotations

import hashlib
import hmac
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TRANSPORT = (ROOT / "src" / "kitsu_mesh_transport.cpp").read_text(
    encoding="utf-8"
)
SCOPE_HEADER = (ROOT / "src" / "kitsu_transport_scope.h").read_text(
    encoding="utf-8"
)
SCOPE = (ROOT / "src" / "kitsu_transport_scope.cpp").read_text(
    encoding="utf-8"
)
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")


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


class TransportScopeTests(unittest.TestCase):
    def test_eu_key_and_transport_code_match_pinned_meshcore_vectors(self) -> None:
        key = hashlib.sha256(b"#EU").digest()[:16]
        self.assertEqual(key.hex().upper(), "04254DA6A9AE9020AD2AF91517413347")
        self.assertIn('kDefaultTransportScopeName[] = "EU"', SCOPE_HEADER)
        self.assertIn('kDefaultTransportScopeTag[] = "#EU"', SCOPE_HEADER)
        fingerprint = hashlib.sha256(key).hexdigest().upper()
        self.assertEqual(
            fingerprint,
            "BB8D70297813E2B6F66A8A5F2889F7F6306CD81F5CAA6E2BB5C60BBFB33ED5B8",
        )
        self.assertIn(f'"{fingerprint}"', SCOPE_HEADER)
        literals = bytes(
            int(value, 16)
            for value in re.findall(
                r"0x([0-9a-f]{2})", SCOPE.split("kDefaultTransportScopeKey", 1)[1]
            )[:16]
        )
        self.assertEqual(literals, key)

        def wire_code(payload_type: int, payload: bytes) -> bytes:
            digest = hmac.new(key, bytes([payload_type]) + payload, hashlib.sha256).digest()
            code = int.from_bytes(digest[:2], "little")
            if code == 0:
                code = 1
            elif code == 0xFFFF:
                code = 0xFFFE
            return code.to_bytes(2, "little")

        self.assertEqual(wire_code(0x05, b"hello").hex().upper(), "8F7A")
        self.assertEqual(wire_code(0x04, b"hello").hex().upper(), "B961")
        self.assertEqual(wire_code(0x05, bytes.fromhex("87710000")), b"\x01\x00")
        self.assertEqual(wire_code(0x05, bytes.fromhex("C2680000")), b"\xFE\xFF")

    def test_one_helper_selects_exactly_one_legacy_or_transport_overload(self) -> None:
        calls = list(re.finditer(r"^\s*sendFlood\(", TRANSPORT, re.MULTILINE))
        self.assertEqual(len(calls), 2)
        helper = cpp_function(TRANSPORT, "bool sendFloodRoute(")
        self.assertEqual(helper.count("sendFlood("), 2)
        self.assertIn("sendFlood(packet, delayMillis)", helper)
        self.assertIn("sendFlood(packet, codes, delayMillis)", helper)
        self.assertIn("if (regionScope == ChannelRegionScope::Legacy)", helper)
        prepare = cpp_function(TRANSPORT, "bool prepareFloodRoute(")
        for required in (
            "ROUTE_TYPE_FLOOD",
            "calculateDefaultTransportCode(",
            "ROUTE_TYPE_TRANSPORT_FLOOD",
            "packet->transport_codes[0] = code",
            "packet->transport_codes[1] = 0U",
            "packet->setPathHashSizeAndCount(1U, 0U)",
        ):
            self.assertIn(required, prepare)
        exact = cpp_function(SCOPE, "bool isExactDefaultScopedRepeat(")
        for required in (
            "wire.route != kRepeatWireRouteTransportFlood",
            "!wire.hasTransportCodes",
            "wire.pathHashSize != 1U",
            "wire.pathCount == 0U",
            "wire.transportCodes[1] != 0U",
            "wire.transportCodes[0] == expected",
        ):
            self.assertIn(required, exact)

    def test_absent_scope_producers_are_legacy_and_channel_is_explicit(self) -> None:
        direct = cpp_function(TRANSPORT, "TransportStatus sendDirectText(")
        channel = cpp_function(TRANSPORT, "TransportStatus sendChannelText(")
        peer = cpp_function(TRANSPORT, "void onPeerDataRecv(")
        ack = cpp_function(TRANSPORT, "void sendAckTo(")
        export = cpp_function(TRANSPORT, "TransportStatus KitsuMeshTransport::exportSignedAdvert(")
        introduce = cpp_function(TRANSPORT, "TransportStatus KitsuMeshTransport::introduce(")
        introduce_once = cpp_function(
            TRANSPORT, "TransportStatus KitsuMeshTransport::introduceOnce("
        )
        for body in (direct, ack, introduce):
            self.assertIn("sendFloodRoute(packet,", body)
            self.assertIn("ChannelRegionScope::Legacy", body)
        self.assertIn("sendFloodRoute(path, ChannelRegionScope::Legacy", peer)
        self.assertIn("prepareFloodRoute(packet,\n                                       ChannelRegionScope::Legacy)", export)
        self.assertIn("prepareFloodRoute(packet,\n                                         ChannelRegionScope::Legacy)", introduce_once)
        self.assertIn("sendFloodRoute(packet,\n                                      ChannelRegionScope::Legacy)", introduce_once)
        self.assertIn("prepareFloodRoute(packet, channel.regionScope)", channel)
        self.assertIn("sendFloodRoute(packet, channel.regionScope)", channel)
        self.assertLess(
            introduce_once.index("prepareFloodRoute("),
            introduce_once.index("armOneShotForPacket("),
        )
        self.assertNotIn("fallback", cpp_function(TRANSPORT, "bool sendFloodRoute(").lower())

    def test_diagnostics_prove_scope_and_exact_wire_code(self) -> None:
        log_tx = cpp_function(TRANSPORT, "void logTx(::mesh::Packet* packet")
        for required in (
            "scopedFloodTxDoneFrames",
            "unscopedFloodTxDoneFrames",
            "packet->hasTransportCodes()",
            "packet->transport_codes[0]",
        ):
            self.assertIn(required, log_tx)
        for required in (
            "scoped_flood_tx_done",
            "unscoped_flood_tx_done",
            "last_flood_tx",
            "transport_code",
            "kDefaultTransportScopeName",
            "kDefaultTransportScopeTag",
            "kDefaultTransportScopeKeyFingerprint",
            "scope_tag",
            "scope_key_fingerprint",
            "lastFloodTxTransportCode",
        ):
            self.assertIn(required, MAIN)
        self.assertEqual(MAIN.count('output += ",\\\"scope_tag\\\":";'), 2)
        self.assertEqual(
            MAIN.count('output += ",\\\"scope_key_fingerprint\\\":";'), 2
        )


if __name__ == "__main__":
    unittest.main()
