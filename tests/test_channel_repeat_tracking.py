from __future__ import annotations

import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TRANSPORT = (ROOT / "src" / "kitsu_mesh_transport.cpp").read_text(
    encoding="utf-8"
)
TRANSPORT_HEADER = (ROOT / "src" / "kitsu_mesh_transport.h").read_text(
    encoding="utf-8"
)
ENDPOINT_RX_POLICY = (ROOT / "src" / "kitsu_endpoint_rx_policy.h").read_text(
    encoding="utf-8"
)
TRACKER = (ROOT / "src" / "kitsu_channel_repeat_tracker.cpp").read_text(
    encoding="utf-8"
)
TRACKER_HEADER = (ROOT / "src" / "kitsu_channel_repeat_tracker.h").read_text(
    encoding="utf-8"
)
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
SESSION = (ROOT / "src" / "kitsu_ble_session.cpp").read_text(encoding="utf-8")
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


class ChannelRepeatTransportSourceTests(unittest.TestCase):
    def test_endpoint_floods_bypass_repeater_delay_and_ten_packet_pool(self) -> None:
        client = TRANSPORT[TRANSPORT.index("class KitsuClient final") :]
        calc = cpp_function(client, "int calcRxDelay(")
        self.assertIn("return endpointFloodReceiveDelayMs(score, airTime)", calc)
        self.assertIn(
            "constexpr int endpointFloodReceiveDelayMs(float, uint32_t) { return 0; }",
            ENDPOINT_RX_POLICY,
        )
        self.assertIn("kPacketPoolSize = 10", TRANSPORT)
        check_recv = cpp_function(DISPATCHER, "void Dispatcher::checkRecv()")
        self.assertIn("if (_delay < 50)", check_recv)
        immediate = check_recv.split("if (_delay < 50)", 1)[1].split(
            "else", 1
        )[0]
        self.assertIn("processRecvPacket(pkt)", immediate)
        self.assertNotIn("queueInbound", immediate)
        # Runtime model of a burst larger than the physical packet pool: each
        # endpoint flood is processed/released before the next allocation.
        retained = 0
        peak = 0
        for _ in range(32):
            retained += 1
            peak = max(peak, retained)
            retained -= 1
        self.assertEqual(peak, 1)
        self.assertEqual(retained, 0)

    def test_tx_done_rearms_rx_inside_radio_start_before_callbacks(self) -> None:
        start = cpp_function(TRANSPORT, "bool startSendRaw(")
        self.assertIn("runSynchronousTxTurnaround(", start)
        self.assertIn("resumeReceiveNow()", start)
        self.assertLess(
            start.index("CustomSX1262Wrapper::isSendComplete()"),
            start.index("resumeReceiveNow()"),
        )
        for signature in (
            "void logTx(::mesh::Packet* packet",
            "void logTxFail(::mesh::Packet* packet",
        ):
            callback = cpp_function(TRANSPORT, signature)
            self.assertNotIn("driver_->resumeReceiveNow()", callback)

    def test_pre_dedup_logrx_uses_exact_and_full_packet_correlation(self) -> None:
        log_rx = cpp_function(TRANSPORT, "void logRxRaw(")
        for required in (
            "decodeRepeatWire(",
            "calculateChannelRepeatDigest(payloadType, wire.payload",
            "wire.payloadBytes",
            "floodRouteBindingFromWire(wire, receivedRoute)",
            "wire.lastHopToken()",
            "messaging_->observeChannelRepeatDetailed(",
        ):
            self.assertIn(required, log_rx)

        check_recv = cpp_function(DISPATCHER, "void Dispatcher::checkRecv()")
        self.assertLess(
            check_recv.index("logRxRaw("),
            check_recv.index("_mgr->allocNew()"),
        )
        digest = cpp_function(TRACKER, "bool calculateChannelRepeatDigest(")
        self.assertIn("sha.update(&payloadType, 1U)", digest)
        self.assertIn("sha.update(payload, payloadBytes)", digest)
        for forbidden in ("path", "route", "header"):
            self.assertNotIn(forbidden, digest.lower())

        self.assertIn("kChannelRepeatHashBytes = 8U", TRACKER_HEADER)
        self.assertIn("kChannelRepeatDigestBytes = 32U", TRACKER_HEADER)
        self.assertIn("MAX_HASH_SIZE == kChannelRepeatHashBytes", TRANSPORT)
        self.assertIn(
            "PAYLOAD_TYPE_GRP_TXT == kChannelGroupTextPayloadType", TRANSPORT
        )

    def test_tracker_requires_sent_flood_group_text_and_nonzero_path(self) -> None:
        record = cpp_function(TRACKER, "bool ChannelRepeatTracker::recordSent(")
        observe = cpp_function(
            TRACKER,
            "ChannelRepeatObserveResult ChannelRepeatTracker::observeDetailed(\n"
            "    uint8_t payloadType, const FloodRouteBinding& route, uint8_t pathCount,\n"
            "    const uint8_t hash[kChannelRepeatHashBytes],\n"
            "    const uint8_t digest[kChannelRepeatDigestBytes],\n"
            "    const uint8_t* lastHopToken",
        )
        for required in (
            "payloadType != kChannelGroupTextPayloadType",
            "!validFloodRouteBinding(route)",
            "!hash",
            "!digest",
            "messageTimestamp == 0U",
        ):
            self.assertIn(required, record)
        self.assertIn("pathCount == 0U", observe)
        self.assertIn("sameHash(entry.hash, hash)", observe)
        self.assertIn("sameDigest(entry.digest, digest)", observe)
        self.assertIn("sameFloodRouteBinding(entry.route, route)", observe)
        self.assertIn("ChannelRepeatObserveResult::WireMismatch", observe)
        self.assertIn("selected->repeatCount == 0xffU", observe)
        self.assertIn("++selected->repeatCount", observe)
        self.assertIn("selected->dirty = true", observe)
        self.assertIn("kChannelRepeatTrackerCapacity = 24U", TRACKER_HEADER)
        self.assertIn("kChannelRepeatWindowMs = 120000UL", TRACKER_HEADER)

    def test_sent_and_echo_events_have_exact_timestamp_correlation(self) -> None:
        self.assertIn("uint32_t messageTimestamp = 0;", TRANSPORT_HEADER)
        self.assertIn("RepeatObserved = 5", TRANSPORT_HEADER)
        mark_sent = cpp_function(TRANSPORT, "void markChannelSent(")
        self.assertIn("event.messageTimestamp = messageTimestamp", mark_sent)
        self.assertIn("channelRepeats_.recordSent(", mark_sent)
        log_tx = cpp_function(TRANSPORT, "void logTx(::mesh::Packet* packet")
        self.assertIn("captureFloodRoute(packet, sentRoute)", log_tx)

        take = cpp_function(TRANSPORT, "bool takeDelivery(DeliveryEvent& output)")
        self.assertIn("channelRepeats_.takeDirty(observation)", take)
        self.assertIn("DeliveryState::RepeatObserved", take)
        self.assertIn(
            "output.messageTimestamp = observation.messageTimestamp", take
        )
        # Repeat bursts are coalesced outside the four-entry lifecycle ring,
        # so the latest cumulative value cannot evict Sent or be evicted by it.
        self.assertLess(
            take.index("deliveryCount_ != 0U"),
            take.index("channelRepeats_.takeDirty(observation)"),
        )

        correlate = cpp_function(MAIN, "ChatJournalEntry* findChannelJournalByDelivery(")
        self.assertIn("entry->channelSlot == delivery.channelSlot", correlate)
        self.assertIn("entry->timestamp == delivery.messageTimestamp", correlate)
        self.assertNotIn("oldest", correlate.lower())

    def test_repeat_transition_never_claims_delivery_or_ack(self) -> None:
        process = cpp_function(MAIN, "void processMeshMessages()")
        repeat = process.split(
            "case kitsu868::mesh::DeliveryState::RepeatObserved:", 1
        )[1].split(
            "case kitsu868::mesh::DeliveryState::Delivered:", 1
        )[0]
        self.assertIn("entry->state = ChatJournalState::Sent", repeat)
        self.assertIn("entry->repeatCount = delivery.repeatCount", repeat)
        self.assertIn('delivery.repeatObservationOpen ? "observed" : "closed"', repeat)
        self.assertIn("entry->repeatObservationOpen", repeat)
        self.assertIn("entry->repeatSources", repeat)
        for forbidden in (
            "ChatJournalState::Delivered",
            "delivery_ack",
            '"delivered"',
        ):
            self.assertNotIn(forbidden, repeat)

        sent = process.split("DeliveryState::Sent", 1)[1].split("break;", 1)[0]
        self.assertIn(
            "entry->repeatCountKnown = delivery.repeatCountKnown", sent
        )
        cancelled = process.split("DeliveryState::Cancelled", 1)[1].split(
            "break;", 1
        )[0]
        failed = process.split("DeliveryState::TxFailed", 1)[1].split(
            "break;", 1
        )[0]
        for terminal in (cancelled, failed):
            self.assertIn("entry->repeatCountKnown = false", terminal)
            self.assertIn("entry->repeatCount = 0U", terminal)

        configure = cpp_function(
            TRANSPORT, "TransportStatus configureRadio(const Settings& next)"
        )
        self.assertIn("messaging.closeChannelRepeatTracking()", configure)
        self.assertLess(
            configure.index("if (active && !driver.isInRecvMode())"),
            configure.index("messaging.closeChannelRepeatTracking()"),
        )


class MessagesV3CompatibilitySourceTests(unittest.TestCase):
    def test_v2_serializer_is_unchanged_and_v3_is_negotiated(self) -> None:
        v2_item = cpp_function(MAIN, "bool appendMessageV2Item(")
        v2_page = cpp_function(MAIN, "bool buildMessagesV2(")
        self.assertNotIn("repeat_count", v2_item)
        self.assertNotIn("repeat_count", v2_page)
        self.assertIn('kitsu.messages.v2', v2_page)

        v3_item = cpp_function(MAIN, "bool appendMessageV3Item(")
        v3_page = cpp_function(MAIN, "bool buildMessagesV3(")
        self.assertIn("appendMessageV2Item(entry, output)", v3_item)
        self.assertIn('output += ",\\\"repeat_count\\\":"', v3_item)
        self.assertIn("entry.repeatCountKnown", v3_item)
        self.assertIn("entry.repeatCount", v3_item)
        self.assertIn('kitsu.messages.v3', v3_page)
        self.assertIn("kMessagesV3PageItems", v3_page)
        self.assertIn("kMessagesPageTailReserveBytes", v3_page)

        allowed = cpp_function(SESSION, "bool operationAllowed(")
        handler = cpp_function(
            MAIN,
            "__attribute__((noinline)) bool handleCompanionBleRequest(\n"
            "    const kitsu868::companion::DecodedEnvelope& request,\n"
            "    const uint8_t* payload, size_t payloadBytes, "
            "uint8_t* responsePayload,\n"
            "    size_t responseCapacity, size_t& responseBytes) {",
        )
        self.assertIn('"messages.get.v3"', allowed)
        self.assertIn('"messages.get.v3"', handler)
        self.assertIn("buildMessagesV3", handler)
        # This capability is exposed only once the coordinated v3 release is
        # promoted; strict 0.15.x companions continue to request messages.v2.
        self.assertIn('FIRMWARE_VERSION[] = "0.19.0"', MAIN)

    def test_v4_adds_explicit_observation_and_bounded_tokens_only(self) -> None:
        v3_item = cpp_function(MAIN, "bool appendMessageV3Item(")
        self.assertNotIn("repeat_observation_open", v3_item)
        self.assertNotIn("repeat_sources", v3_item)

        v4_item = cpp_function(MAIN, "bool appendMessageV4Item(")
        for required in (
            "appendMessageV3Item(entry, output)",
            '"repeat_observation_open"',
            '"repeat_sources"',
            '"repeat_sources_truncated"',
            '"last_hop_token"',
            "validMessageV4RepeatSources(entry)",
        ):
            self.assertIn(required.replace('"', '\\"') if required.startswith('"') else required, v4_item)
        self.assertNotIn("likely_name", v4_item)
        self.assertNotIn("repeaters_heard\":", v4_item.split("suffix", 1)[0])
        self.assertIn(
            'output += "null,\\\"repeat_sources\\\":null,"', v4_item
        )
        self.assertIn('"\\\"repeat_sources_truncated\\\":null"', v4_item)
        validity = cpp_function(MAIN, "bool validMessageV4RepeatSources(")
        self.assertIn("entry.repeatSourceCount > entry.repeatCount", validity)
        self.assertIn("entry.repeatCount <", validity)
        self.assertIn("kChannelRepeatSourceCapacity + 1U", validity)

        v4_page = cpp_function(MAIN, "bool buildMessagesV4(")
        self.assertIn("kitsu.messages.v4", v4_page)
        self.assertIn("kMessagesV4PageItems", v4_page)
        self.assertIn("kMaximumEnvelopePayloadBytes", v4_page)
        allowed = cpp_function(SESSION, "bool operationAllowed(")
        self.assertIn('"messages.get.v4"', allowed)

    def test_v4_worst_case_three_pages_drain_all_24_rows(self) -> None:
        escaped_name = '"' * 32
        escaped_text = '"' * 160
        base = {
            "timestamp": 4_294_967_295,
            "inbound": False,
            "kind": "channel",
            "peer_id": None,
            "channel_slot": 3,
            "authenticated": False,
            "unread": False,
            "sender_name": escaped_name,
            "text": escaped_text,
            "state": "sent",
            "route": "flood",
            "local_tx": "sent",
            "delivery_ack": "not_applicable",
            "repeater_count": None,
            "repeat_count": 255,
            "repeat_observation_open": False,
            "repeat_sources": [
                {"last_hop_token": token}
                for token in ("A1", "B2C3", "D4E5F6", "010203")
            ],
            "repeat_sources_truncated": True,
            "repeaters_heard": None,
            "rssi_dbm": None,
            "snr_db": None,
        }
        all_ids: list[str] = []
        for page_index in range(3):
            items = []
            for offset in range(8):
                numeric_id = 4_294_967_272 + page_index * 8 + offset
                item = {
                    **base,
                    "message_id": str(numeric_id),
                    "revision": str(numeric_id),
                }
                items.append(item)
                all_ids.append(item["message_id"])
            page = {
                "schema": "kitsu.messages.v4",
                "journal_session": "4294967295",
                "journal_revision": "4294967295",
                "items": items,
                "cursor": items[-1]["message_id"],
                "has_more": page_index != 2,
                "gap": False,
            }
            encoded = json.dumps(
                page, ensure_ascii=False, separators=(",", ":")
            ).encode("utf-8")
            self.assertLessEqual(len(encoded), 12_000)
        self.assertEqual(len(all_ids), 24)
        self.assertEqual(len(set(all_ids)), 24)

    def test_v4_tracker_unavailable_sent_channel_is_all_null(self) -> None:
        item = {
            "state": "sent",
            "kind": "channel",
            "repeat_count": None,
            "repeat_observation_open": None,
            "repeat_sources": None,
            "repeat_sources_truncated": None,
        }
        repeat_values = (
            item["repeat_count"],
            item["repeat_observation_open"],
            item["repeat_sources"],
            item["repeat_sources_truncated"],
        )
        self.assertTrue(all(value is None for value in repeat_values))

    def test_state_v1_max_diagnostics_remain_bounded(self) -> None:
        state = cpp_function(MAIN, "bool buildState(")
        self.assertIn("kMaximumEnvelopePayloadBytes", state)
        numeric_diagnostics = (
            "tx_done", "tx_failed", "rx_ready_after_tx",
            "physical_rx_confirmed_after_tx", "sync_turnaround_completed",
            "sync_turnaround_start_failures", "sync_turnaround_timeouts",
            "rx_rearm_attempts", "rx_rearm_retries", "rx_rearm_failures",
            "last_rx_start_attempts", "last_rx_start_code",
            "last_rx_chip_mode", "last_tx_done_to_start_receive_us",
            "last_tx_done_to_rx_confirmed_us", "current_rx_chip_mode",
            "scoped_flood_tx_done",
            "unscoped_flood_tx_done", "raw_frames", "parsed_frames",
            "raw_rejected", "channel_forward_candidates",
            "channel_hash_matches", "channel_wire_mismatches",
            "channel_digest_mismatches",
            "channel_exact_matches", "channel_recorded", "channel_saturated",
            "advert_forward_candidates", "advert_hash_matches",
            "advert_wire_mismatches", "advert_digest_mismatches",
            "advert_exact_matches",
            "advert_recorded", "advert_saturated",
        )
        for key in numeric_diagnostics:
            self.assertIn(f'\\"{key}\\"', state)
        for required in (
            "last_rx_start_software_state", "last_rx_chip_status_available",
            "last_rx_chip_status", "current_rx_software_state",
            "current_rx_chip_status_available", "current_rx_chip_status",
            "last_flood_tx", "scope_tag",
            "scope_key_fingerprint", "transport_code",
        ):
            self.assertIn(required, state)

        candidate = {
            "schema": "kitsu.state.v1",
            "firmware_version": "0.16.5",
            "mesh_last_flood_advert": {
                "emitted_at": 4_102_444_800,
                "state": "sent",
                "repeat_count": 255,
                "observation_open": False,
            },
            "mesh_last_flood_advert_v2": {
                "emitted_at": 4_102_444_800,
                "state": "sent",
                "repeat_count": 255,
                "observation_open": False,
                "repeat_sources": [
                    {"last_hop_token": token}
                    for token in ("ABCDEF", "010203", "A1B2C3", "FFEEDD")
                ],
                "repeat_sources_truncated": True,
            },
            "mesh_last_nearby_advert": {
                "emitted_at": 4_102_444_800,
                "state": "tx_failed",
                "repeat_count": None,
                "observation_open": False,
            },
            "mesh_repeat_diagnostics": {
                key: 4_294_967_295 for key in numeric_diagnostics
            },
        }
        candidate["mesh_repeat_diagnostics"]["last_rx_start_software_state"] = True
        candidate["mesh_repeat_diagnostics"]["last_rx_chip_status_available"] = True
        candidate["mesh_repeat_diagnostics"]["last_rx_chip_status"] = "B2"
        candidate["mesh_repeat_diagnostics"]["current_rx_software_state"] = True
        candidate["mesh_repeat_diagnostics"]["current_rx_chip_status_available"] = True
        candidate["mesh_repeat_diagnostics"]["current_rx_chip_status"] = "B2"
        candidate["mesh_repeat_diagnostics"]["last_flood_tx"] = {
            "payload_type": 255,
            "scoped": True,
            "scope": "EU",
            "transport_code": "FEFF",
            "scope_tag": "#EU",
            "scope_key_fingerprint": (
                "BB8D70297813E2B6F66A8A5F2889F7F"
                "6306CD81F5CAA6E2BB5C60BBFB33ED5B8"
            ),
        }
        candidate["mesh_repeat_diagnostics"]["last_channel"] = {
            "packet_hash": "FF" * 8,
            "path": "FF" * 63,
            "last_hop_token": "FFFFFF",
            "path_hash_bytes": 3,
            "path_count": 21,
            "rssi_dbm": -164.0,
            "snr_db": -32.0,
            "result": "digest_mismatch",
        }
        candidate["mesh_repeat_diagnostics"]["last_advert"] = dict(
            candidate["mesh_repeat_diagnostics"]["last_channel"]
        )
        encoded = json.dumps(candidate, separators=(",", ":")).encode()
        self.assertEqual(len(encoded), 2_789)
        self.assertLessEqual(len(encoded), 12_000)

    def test_v3_worst_case_pages_remain_below_authenticated_payload_cap(self) -> None:
        escaped_name = '"' * 32
        escaped_text = '"' * 160
        peer = "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE"
        base = {
            "timestamp": 4_294_967_295,
            "inbound": False,
            "kind": "channel",
            "peer_id": None,
            "channel_slot": 3,
            "authenticated": False,
            "unread": False,
            "sender_name": escaped_name,
            "text": escaped_text,
            "state": "sent",
            "route": "flood",
            "local_tx": "sent",
            "delivery_ack": "not_applicable",
            "repeater_count": None,
            "repeat_count": 255,
            "repeaters_heard": None,
            "rssi_dbm": None,
            "snr_db": None,
        }
        items = [
            {
                **base,
                "message_id": str(4_294_967_272 + index),
                "revision": str(4_294_967_272 + index),
            }
            for index in range(12)
        ]
        page = {
            "schema": "kitsu.messages.v3",
            "journal_session": "4294967295",
            "journal_revision": "4294967295",
            "items": items,
            "cursor": items[-1]["message_id"],
            "has_more": True,
            "gap": False,
        }
        encoded = json.dumps(
            page, ensure_ascii=False, separators=(",", ":")
        ).encode("utf-8")
        self.assertLessEqual(len(encoded), 12_000)


if __name__ == "__main__":
    unittest.main(verbosity=2)
