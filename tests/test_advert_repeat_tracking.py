from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TRANSPORT = (ROOT / "src" / "kitsu_mesh_transport.cpp").read_text(
    encoding="utf-8"
)
TRANSPORT_HEADER = (ROOT / "src" / "kitsu_mesh_transport.h").read_text(
    encoding="utf-8"
)
TRACKER = (ROOT / "src" / "kitsu_advert_repeat_tracker.cpp").read_text(
    encoding="utf-8"
)
TRACKER_HEADER = (ROOT / "src" / "kitsu_advert_repeat_tracker.h").read_text(
    encoding="utf-8"
)
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
DISPATCHER = (ROOT / "lib" / "MeshCore" / "src" / "Dispatcher.cpp").read_text(
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


class AdvertRepeatTrackerSourceTests(unittest.TestCase):
    def test_tracker_contract_is_one_entry_bounded_and_retained(self) -> None:
        for required in (
            "kAdvertRepeatHashBytes = 8U",
            "kAdvertRepeatDigestBytes = 32U",
            "kAdvertPayloadType = 0x04U",
            "kAdvertRepeatWindowMs = 120000UL",
            "kAdvertMinimumEmittedAt = 1704067200UL",
            "kAdvertMaximumEmittedAt = 4102444800UL",
            "Queued = 0",
            "Sent = 1",
            "TxFailed = 2",
        ):
            self.assertIn(required, TRACKER_HEADER)
        self.assertNotIn("entries_[", TRACKER_HEADER)

        queued = cpp_function(TRACKER, "bool AdvertRepeatTracker::recordQueued(")
        self.assertIn("validAdvert(payloadType, flood, emittedAt)", queued)
        self.assertIn("status_ = FloodAdvertStatus{}", queued)
        self.assertIn("dirty_ = true", queued)

        tick = cpp_function(TRACKER, "bool AdvertRepeatTracker::tick(")
        self.assertIn("nowMs - sentAtMs_ < kAdvertRepeatWindowMs", tick)
        self.assertIn("status_.observationOpen = false", tick)
        self.assertNotIn("status_ = FloodAdvertStatus{}", tick)

    def test_exact_pre_dedup_fingerprint_and_nonzero_flood_path_are_required(self) -> None:
        log_rx = cpp_function(TRANSPORT, "void logRxRaw(")
        for required in (
            "decodeRepeatWire(",
            "calculateChannelRepeatDigest(payloadType, wire.payload",
            "wire.payloadBytes",
            "advertRepeats_.observeDetailed(",
            "floodRouteBindingFromWire(wire, receivedRoute)",
            "wire.lastHopToken()",
        ):
            self.assertIn(required, log_rx)

        check_recv = cpp_function(DISPATCHER, "void Dispatcher::checkRecv()")
        self.assertLess(
            check_recv.index("logRxRaw("),
            check_recv.index("_mgr->allocNew()"),
        )

        observe = cpp_function(
            TRACKER,
            "AdvertRepeatObserveResult AdvertRepeatTracker::observeDetailed(\n"
            "    uint8_t payloadType, const FloodRouteBinding& route, uint8_t pathCount,\n"
            "    const uint8_t hash[kAdvertRepeatHashBytes],\n"
            "    const uint8_t digest[kAdvertRepeatDigestBytes],\n"
            "    const uint8_t* lastHopToken",
        )
        for required in (
            "payloadType != kAdvertPayloadType",
            "!validFloodRouteBinding(route)",
            "pathCount == 0U",
            "!sameBytes(hash_, hash, sizeof(hash_))",
            "!sameBytes(digest_, digest, sizeof(digest_))",
            "status_.repeatCount == 0xffU",
            "++status_.repeatCount",
            "recordSource(lastHopToken, lastHopTokenBytes)",
            "sameFloodRouteBinding(route_, route)",
            "AdvertRepeatObserveResult::WireMismatch",
        ):
            self.assertIn(required, observe)

    def test_only_mesh_wide_action_arms_and_nearby_does_not_overwrite(self) -> None:
        introduce = cpp_function(
            TRANSPORT, "TransportStatus KitsuMeshTransport::introduceOnce("
        )
        self.assertIn("memcpy(&emittedAt, packet->payload + PUB_KEY_SIZE", introduce)
        self.assertEqual(introduce.count("beginFloodAdvert(packet, emittedAt)"), 1)
        self.assertLess(
            introduce.index("impl_->client.sendZeroHop(packet)"),
            introduce.index("impl_->client.beginFloodAdvert(packet, emittedAt)"),
        )
        self.assertLess(
            introduce.index("impl_->client.beginFloodAdvert(packet, emittedAt)"),
            introduce.index("impl_->client.sendFloodRoute(packet"),
        )

        begin = cpp_function(TRANSPORT, "bool beginFloodAdvert(")
        self.assertIn("packet->getPayloadType()", begin)
        self.assertIn("packet->isRouteFlood()", begin)
        self.assertIn("advertRepeats_.recordQueued", begin)

    def test_actual_tx_callbacks_cannot_leave_queued_state_stuck(self) -> None:
        log_tx = cpp_function(TRANSPORT, "void logTx(::mesh::Packet* packet")
        advert = log_tx.split("if (packet == advertPacket_)", 1)[1]
        for required in (
            "calculateChannelRepeatDigest",
            "captureFloodRoute(packet, sentRoute)",
            "advertRepeats_.markSent(",
            "bool markedSent = false",
            "if (!markedSent)",
            "advertRepeats_.markTxFailed(advertTimestamp_)",
            "advertTimestamp_ = 0U",
        ):
            self.assertIn(required, advert)

        log_fail = cpp_function(TRANSPORT, "void logTxFail(::mesh::Packet* packet")
        self.assertIn("if (packet == advertPacket_)", log_fail)
        self.assertIn("advertRepeats_.markTxFailed(advertTimestamp_)", log_fail)

    def test_cancel_and_profile_reset_are_dirty_and_profile_local(self) -> None:
        cancel_queued = cpp_function(TRANSPORT, "void cancelQueuedSends()")
        self.assertIn("advertPacket_ != inFlight", cancel_queued)
        self.assertIn("advertRepeats_.markTxFailed(advertTimestamp_)", cancel_queued)

        cancel_all = cpp_function(TRANSPORT, "void cancelAllSends()")
        self.assertIn("advertRepeats_.markTxFailed(advertTimestamp_)", cancel_all)

        clear = cpp_function(TRACKER, "void AdvertRepeatTracker::clear()")
        self.assertIn("const bool changed = status_.available", clear)
        self.assertIn("if (changed) dirty_ = true", clear)

        configure = cpp_function(
            TRANSPORT, "TransportStatus configureRadio(const Settings& next)"
        )
        self.assertLess(
            configure.index("if (active && !driver.isInRecvMode())"),
            configure.index("client.clearAdvertRepeatTracking()"),
        )
        self.assertIn("client.clearAdvertRepeatTracking()", configure)

    def test_state_v1_addition_is_nullable_strict_and_refreshes_promptly(self) -> None:
        state = cpp_function(MAIN, "bool buildState(")
        for required in (
            "mesh_last_flood_advert",
            "emitted_at",
            "repeat_count",
            "observation_open",
            'return "queued"',
            'return "sent"',
            'return "tx_failed"',
            "output += \"null\"",
            "validFloodAdvertStatus(lastFloodAdvert)",
        ):
            self.assertIn(required, state if "return \"" not in required else MAIN)

        validity = cpp_function(MAIN, "bool validFloodAdvertStatus(")
        self.assertIn("kAdvertMinimumEmittedAt", validity)
        self.assertIn("kAdvertMaximumEmittedAt", validity)
        self.assertIn("!status.repeatCountKnown", validity)
        self.assertIn("status.sourceCount", validity)
        self.assertIn("status.sourceCount > status.repeatCount", validity)
        self.assertIn("status.repeatCount <", validity)
        self.assertIn("kAdvertRepeatSourceCapacity + 1U", validity)
        self.assertIn("!status.repeatCountKnown", validity)
        self.assertIn("!status.observationOpen", validity)

        refresh = cpp_function(MAIN, "void processFloodAdvertStatus()")
        self.assertIn("takeFloodAdvertStatusChanged()", refresh)
        self.assertIn("companionBleRefreshDirty = true", refresh)
        loop = cpp_function(MAIN, "void loop()")
        self.assertLess(
            loop.index("meshTransport.loop()"),
            loop.index("processFloodAdvertStatus()"),
        )
        self.assertLess(
            loop.index("processFloodAdvertStatus()"),
            loop.index("serviceCompanionBleRefresh(now)"),
        )

        self.assertIn("lastFloodAdvertStatus(FloodAdvertStatus& output)", TRANSPORT_HEADER)
        self.assertIn("takeFloodAdvertStatusChanged()", TRANSPORT_HEADER)
        self.assertIn("mesh_last_flood_advert_v2", state)
        self.assertIn("repeat_sources_truncated", state)
        self.assertIn('#define KITSU_FIRMWARE_VERSION_LITERAL "0.20.4"', MAIN)
        self.assertIn(
            'constexpr char FIRMWARE_VERSION[] = KITSU_FIRMWARE_VERSION_LITERAL;',
            MAIN,
        )

    def test_legacy_and_v2_flood_views_share_one_snapshot(self) -> None:
        state = cpp_function(MAIN, "bool buildState(")
        self.assertEqual(
            state.count("meshTransport.lastFloodAdvertStatus(lastFloodAdvert)"),
            1,
        )
        legacy = state.split(
            'output += ",\\\"mesh_last_flood_advert\\\":";', 1
        )[1].split("// Preserve mesh_last_flood_advert", 1)[0]
        for key in ("emitted_at", "state", "repeat_count", "observation_open"):
            self.assertIn(key, legacy)
        for forbidden in ("repeat_sources", "last_hop_token", "sourcesTruncated"):
            self.assertNotIn(forbidden, legacy)

        v2 = state.split(
            'output += ",\\\"mesh_last_flood_advert_v2\\\":";', 1
        )[1].split(
            'output += ",\\\"mesh_last_nearby_advert\\\":";', 1
        )[0]
        for shared in (
            "lastFloodAdvert.emittedAt",
            "lastFloodAdvert.state",
            "lastFloodAdvert.repeatCount",
            "lastFloodAdvert.observationOpen",
        ):
            self.assertIn(shared, legacy)
            self.assertIn(shared, v2)
        self.assertIn("lastFloodAdvert.sources", v2)
        self.assertIn("lastFloodAdvert.sourcesTruncated", v2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
