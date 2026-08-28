from pathlib import Path
import json
import unittest


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
SESSION = (ROOT / "src" / "kitsu_ble_session.cpp").read_text(encoding="utf-8")
MESSAGE_READ = (ROOT / "src" / "kitsu_message_read_contract.cpp").read_text(
    encoding="utf-8"
)
PROFILE = (ROOT / "platformio.ini").read_text(encoding="utf-8")
COMPANION_PACK = (ROOT / "src" / "companion_pack.cpp").read_text(
    encoding="utf-8"
)
REPLACEMENT_INTENT = (
    ROOT / "src" / "companion_replacement_intent.h"
).read_text(encoding="utf-8")


class LocalOnlyMainIntegrationSourceTests(unittest.TestCase):
    def test_replacement_transaction_does_not_shrink_k868_capacity(self):
        self.assertIn("expected > partition_->size", COMPANION_PACK)
        self.assertNotIn("KITSU_REPLACEMENT", COMPANION_PACK)
        self.assertIn(
            "KITSU_COMPANION_PACK_MAX_BYTES = 0x140000U", REPLACEMENT_INTENT
        )
        self.assertIn(
            "KITSU_REPLACEMENT_PREPARED_FLASH_OFFSET = 0x7b0000U",
            REPLACEMENT_INTENT,
        )
        self.assertIn(
            "KITSU_REPLACEMENT_COMMITTED_FLASH_OFFSET = 0x7b1000U",
            REPLACEMENT_INTENT,
        )

    def test_pack_mismatch_is_non_destructive_without_one_shot_authorization(self):
        setup = MAIN.split("void setup()", 1)[1].split("void loop()", 1)[0]
        replacement = setup.split(
            "kitsu868::CompanionReplacementTransaction replacementTransaction", 1
        )[1].split("companionBrain.syncSleeping", 1)[0]
        self.assertEqual(MAIN.count("CompanionBrain::clearStoredState()"), 1)
        authorized = replacement.split("if (replacementAuthorized)", 1)[1].split(
            "else if (firstPackAssignment)", 1
        )[0]
        self.assertIn("CompanionBrain::clearStoredState()", authorized)
        self.assertIn("companionReplacementTransactionAuthorizes", replacement)
        self.assertIn("companionPack.quarantineUnapprovedReplacement()", replacement)
        self.assertIn("reason=replacement-not-authorized", replacement)
        self.assertIn("packIdentityStateSaved = saveState()", replacement)
        self.assertIn("legacyConnectivityRetirementReady &&", replacement)
        self.assertIn(
            "companionReplacementIntentValid(\n          replacementTransaction.prepared)",
            replacement,
        )
        self.assertIn(
            "replacementTransactionStorage.preparedSectorCanonical()", replacement
        )
        self.assertIn(
            "replacementTransactionStorage.committedSectorCanonical()", replacement
        )
        self.assertIn("replacementTransactionStorage.consume()", authorized)
        self.assertIn(
            "legacyConnectivityRetirementPlatform,\n"
            "              replacementPreservation",
            replacement,
        )
        self.assertIn("LegacyConnectivityPreservation::Prepared", replacement)
        self.assertIn("LegacyConnectivityPreservation::Transaction", replacement)
        self.assertLess(
            authorized.index("packIdentityStateSaved = saveState()"),
            authorized.index("replacementTransactionStorage.consume()"),
        )
        self.assertIn("pack_replacement_pending=true", authorized)
        self.assertIn("const uint32_t brainPackId = collectiblePackId != 0U", replacement)
        self.assertIn("companionBrain.begin(wisp.uid.c_str(), brainPackId)", replacement)
        self.assertNotIn("clearStoredState()", replacement.split("else {", 1)[1])

        self.assertLess(
            replacement.index("replacementTransactionStorage.beginAndRead"),
            replacement.index("KitsuLegacyConnectivityRetirement::run"),
        )
        self.assertLess(
            replacement.index("KitsuLegacyConnectivityRetirement::run"),
            replacement.index("loadState()"),
        )

    def test_normal_runtime_is_local_ble_mesh_and_companion_only(self):
        includes = MAIN.split("namespace {", 1)[0]
        for forbidden in (
            "kitsu_connectivity_runtime.h",
            "kitsu_esp32_gateway_tls.h",
            "kitsu_gateway_bootstrap.h",
            "kitsu_gateway_enrollment_flow.h",
            "kitsu_mobile_relay.h",
        ):
            self.assertNotIn(forbidden, includes)

        setup = MAIN.split("void setup()", 1)[1].split("void loop()", 1)[0]
        self.assertIn("initLocalSecurityStorage();", setup)
        self.assertIn("initMesh();", setup)
        self.assertIn("companionBle.begin()", setup)
        loop = MAIN.split("void loop()", 1)[1]
        for required in (
            "companionBle.loop(now)",
            "meshTransport.loop()",
            "tickCreature()",
            "tickProgression()",
        ):
            self.assertIn(required, loop)
        for forbidden in (
            "wifiRuntime",
            "serviceGateway",
            "mobileRelay",
            "gatewayEnrollmentFlow",
            "ActionExecutionOutcome",
            "remote_available",
            '\"remote\"',
        ):
            self.assertNotIn(forbidden, MAIN)

    def test_ble_surface_has_local_ops_and_authenticated_self_forget(self):
        allowed = SESSION.split("bool operationAllowed", 1)[1].split(
            "}  // namespace", 1
        )[0]
        for operation in (
            "state.get",
            "history.get",
            "peers.get",
            "messages.get",
            "messages.get.v2",
            "messages.mark_read",
            "channels.get",
            "channels.get.v2",
            "clock.sync",
            "mesh.configure",
            "action.apply",
            "controller.forget",
            "firmware.update.status",
            "firmware.update.begin",
            "firmware.update.write",
            "firmware.update.finish",
            "firmware.update.reboot",
            "firmware.update.abort",
        ):
            self.assertIn(f'"{operation}"', allowed)
        for forbidden in (
            "wifi.configure",
            "gateway.configure",
            "gateway.enroll",
            "mobile.relay",
        ):
            self.assertNotIn(forbidden, allowed)
        self.assertIn(
            "security_->revokeAuthenticatedController(controllerId_)", SESSION
        )
        self.assertIn("transport_->bleTransmitIdle()", SESSION)

        bridge = MAIN.split("class FirmwareBleBridge", 1)[1].split(
            "FirmwareBleBridge companionBle", 1
        )[0]
        self.assertIn("isFirmwareUpdateOperation(request.operation)", bridge)
        self.assertIn("bleOta.handleRequest(", bridge)
        self.assertLess(
            bridge.index("bleOta.handleRequest("),
            bridge.index("handleCompanionBleRequest(request"),
        )

    def test_ble_ota_boot_health_and_reboot_drain_are_wired(self):
        setup = MAIN.split("void setup()", 1)[1].split("void loop()", 1)[0]
        loop = MAIN.split("void loop()", 1)[1]
        self.assertIn("bleOta.begin(bleOtaPlatform, FIRMWARE_VERSION)", setup)
        self.assertIn("connectivitySecurityReady && bleReady", setup)
        self.assertIn("legacyConnectivityRetirementReady", setup)
        self.assertIn("bleOta.finishCriticalInitialization(", setup)
        self.assertIn("if (criticalHealth && otaInitializationAccepted)", setup)
        blocked = setup.split('Serial.printf("KITSU_BLOCKED', 1)[1].split(
            ");", 1
        )[0]
        for status in (
            "legacyConnectivityRetirementReady",
            "connectivitySecurityReady",
            "bleReady",
            "otaInitializationAccepted",
        ):
            self.assertIn(status, blocked)
        self.assertIn("companionBle.bleTransmitIdle()", loop)

    def test_authenticated_ble_refresh_events_keep_local_views_current(self):
        service = MAIN.split("void serviceCompanionBleRefresh", 1)[1].split(
            "void processMeshMessages", 1
        )[0]
        self.assertIn("status.applicationAuthenticated", service)
        self.assertIn("BleOtaState::Receiving", service)
        self.assertIn("BleOtaState::ReadyToReboot", service)
        self.assertIn("companionBle.bleTransmitIdle()", service)
        self.assertIn('"companion.refresh"', service)
        self.assertIn('\\"kind\\":\\"refresh\\"', service)
        self.assertIn("BLE_REFRESH_INTERVAL_MS", service)
        chat_event = MAIN.split("void emitChatEvent", 1)[1].split(
            "void serviceCompanionBleRefresh", 1
        )[0]
        self.assertIn("companionBleRefreshDirty = true", chat_event)
        loop = MAIN.split("void loop()", 1)[1]
        self.assertLess(
            loop.index("processMeshMessages()"),
            loop.index("serviceCompanionBleRefresh(now)"),
        )

    def test_state_is_local_truth_without_server_placeholders(self):
        state = MAIN.split("bool buildState", 1)[1].split(
            "void appendObservationTime", 1
        )[0]
        for field in (
            "device_uid",
            "firmware_version",
            "companion",
            "energy",
            "curiosity",
            "affection",
            "sleeping",
            "mesh_rx_ready",
            "mesh_enabled",
            "mesh_time_valid",
            "controller_count",
            "battery_percent",
            "pack_ready",
            "bond_level",
        ):
            self.assertIn(field, state)
        for forbidden in (
            "wifi_",
            "gateway_",
            "lan_state",
            "remote_connectivity_allowed",
        ):
            self.assertNotIn(forbidden, state)

    def test_authenticated_advert_action_is_real_and_reports_readiness(self):
        action = MAIN.split("bool applyAction", 1)[1].split(
            "bool buildState", 1
        )[0]
        self.assertIn("BleActionKind::AdvertiseOnce", action)
        self.assertIn("queueBleAdvert(command)", action)
        self.assertIn('command.kind == BleActionKind::AdvertiseOnce', action)
        self.assertIn('? "queued" : "applied"', action)

        queue = MAIN.split("queueBleAdvert(", 1)[1].split(
            "bool rejectAction", 1
        )[0]
        self.assertIn("BleAdvertScope::Nearby", queue)
        self.assertIn("BleAdvertScope::Mesh", queue)
        self.assertIn("AdvertScope::Nearby", queue)
        self.assertIn("AdvertScope::Flood", queue)
        self.assertIn("meshTransport.introduceOnce(", queue)

        state = MAIN.split("bool buildState", 1)[1].split(
            "void appendObservationTime", 1
        )[0]
        for field in (
            "mesh_identity_ready",
            "mesh_advertise_ready",
            "mesh_advertise_retry_after_ms",
            "mesh_advertise_error",
        ):
            self.assertIn(field, state)

    def test_message_gap_is_relative_to_the_requested_cursor(self):
        messages = MAIN.split("bool buildMessages", 1)[1].split(
            "}  // namespace companion_api", 1
        )[0]
        self.assertIn("firstReturnedId = entry.id", messages)
        self.assertIn("expectedFirstId = query.after + 1U", messages)
        self.assertIn("if (expectedFirstId == 0U) expectedFirstId = 1U", messages)
        self.assertIn("query.hasAfter && count != 0U", messages)
        self.assertIn("firstReturnedId != expectedFirstId", messages)
        self.assertNotIn(
            'output += chatJournalDropped != 0U ? "true" : "false"',
            messages,
        )

    def test_message_v2_is_generation_stable_and_identity_paginated(self):
        messages = MAIN.split("bool buildMessagesV2", 1)[1].split(
            "}  // namespace companion_api", 1
        )[0]
        for required in (
            "kitsu.messages.v2",
            "journal_session",
            "journal_revision",
            "cursor = entry->id",
        ):
            self.assertIn(required, messages)
        item = MAIN.split("bool appendMessageV2Item", 1)[1].split(
            "bool buildMessagesV2", 1
        )[0]
        for required in (
            "revision",
            "unread",
            "route",
            "local_tx",
            "delivery_ack",
            "repeater_count",
            "repeaters_heard",
            "rssi_dbm",
            "snr_db",
        ):
            self.assertIn(required, item)
        self.assertNotIn("cursor = entry->revision", messages)
        self.assertIn("kMessagesV2PageItems = 12U", MAIN)
        self.assertIn("kMessagesPageTailReserveBytes", messages)
        self.assertIn(
            "kitsu868::companion::kMaximumEnvelopePayloadBytes", messages
        )
        self.assertIn('FIRMWARE_VERSION[] = "0.19.0"', MAIN)
        setup = MAIN.split("void setup()", 1)[1].split("void loop()", 1)[0]
        self.assertIn("chatSession = esp_random()", setup)
        self.assertIn("if (chatSession == 0U) chatSession = 1U", setup)
        self.assertLess(
            setup.index("chatSession = esp_random()"),
            setup.index("companionBle.begin()"),
        )

    def test_message_pages_fit_worst_case_escaped_payloads(self):
        # Quotes are legal MeshCore text and double in JSON, so they are the
        # byte-heavy valid ASCII case. Exercise all 24 retained rows even
        # though the wire intentionally emits bounded 8/12-row pages.
        escaped_name = '"' * 32
        escaped_text = '"' * 160
        # Canonical base64url for a valid, nonzero 32-byte MeshCore public key.
        peer = "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE"
        oldest_id = 4_294_967_272
        v1_template = {
            "timestamp": 4294967295,
            "inbound": True,
            "kind": "direct",
            "peer_id": peer,
            "channel_slot": None,
            "authenticated": True,
            "sender_name": escaped_name,
            "text": escaped_text,
            "state": "received",
        }
        v2_template = {
            **v1_template,
            "unread": True,
            "route": "flood",
            "local_tx": "not_applicable",
            "delivery_ack": "not_applicable",
            "repeater_count": 63,
            "repeaters_heard": None,
            "rssi_dbm": -164.0,
            "snr_db": -20.0,
        }
        v1_items = [
            {**v1_template, "message_id": str(oldest_id + ordinal)}
            for ordinal in range(24)
        ]
        v2_items = [
            {
                **v2_template,
                "message_id": str(oldest_id + ordinal),
                "revision": str(oldest_id + ordinal),
            }
            for ordinal in range(24)
        ]

        def encoded(value):
            return json.dumps(
                value, ensure_ascii=False, separators=(",", ":")
            ).encode("utf-8")

        v1_page = {
            "schema": "kitsu.messages.v1",
            "items": v1_items[:8],
            "cursor": v1_items[7]["message_id"],
            "has_more": True,
            "gap": False,
        }
        def v2_page(items, has_more):
            return {
                "schema": "kitsu.messages.v2",
                "journal_session": "4294967295",
                "journal_revision": "4294967295",
                "items": items,
                "cursor": items[-1]["message_id"],
                "has_more": has_more,
                "gap": False,
            }

        v2_pages = [v2_page(v2_items[:12], True),
                    v2_page(v2_items[12:], False)]
        unpaged_v2 = v2_page(v2_items, False)
        self.assertLessEqual(len(encoded(v1_page)), 12_000)
        for page in v2_pages:
            self.assertLessEqual(len(encoded(page)), 12_000)
        self.assertGreater(len(encoded(unpaged_v2)), 12_000)
        drained_ids = [item["message_id"]
                       for page in v2_pages for item in page["items"]]
        self.assertEqual(drained_ids,
                         [item["message_id"] for item in v2_items])

    def test_message_mutations_advance_revision_before_refresh(self):
        process = MAIN.split("void processMeshMessages", 1)[1].split(
            "void tickProgression", 1
        )[0]
        for state in ("Sent", "Delivered", "TimedOut", "Cancelled", "TxFailed"):
            branch = process.split(f"DeliveryState::{state}", 1)[1].split(
                "break;", 1
            )[0]
            self.assertIn("touchChatJournal(*entry)", branch)
            self.assertIn("emitChatEvent(", branch)
        mark_read = MAIN.split("void markChatJournalRead", 1)[1].split(
            "ChatJournalEntry* chatJournalNewest", 1
        )[0]
        self.assertIn("applyChatJournalReadPlan(selected, selectedCount)", mark_read)
        apply_read = MAIN.split("uint8_t applyChatJournalReadPlan", 2)[2].split(
            "void markChatJournalRead", 1
        )[0]
        self.assertIn("touchChatJournal(chatJournal[index])", apply_read)

    def test_authenticated_mark_read_is_bounded_atomic_and_updates_physical_unread(self):
        allowed = SESSION.split("bool operationAllowed", 1)[1].split(
            "}  // namespace", 1
        )[0]
        self.assertIn('"messages.mark_read"', allowed)
        authenticated = SESSION.split(
            "bool KitsuBleSession::handleAuthenticatedEnvelope", 1
        )[1].split("void KitsuBleSession::onFrame", 1)[0]
        self.assertIn("operationAllowed(request.operation)", authenticated)
        self.assertIn("operations_->handleBleRequest(", authenticated)
        preauthenticated_dispatch = SESSION.split(
            "bool KitsuBleSession::handleAuthenticatedEnvelope", 1
        )[0].split("bool operationAllowed", 1)[0]
        self.assertNotIn("messages.mark_read", preauthenticated_dispatch)

        mark_read_contract = MAIN.split("void buildMarkReadResponse", 1)[1].split(
            "}  // namespace companion_api", 1
        )[0]
        for required in (
            "parseCommand(payload, payloadBytes, command)",
            "message_read::plan(",
            "chatSession, command, records, visibleCount, plan",
            "status != PlanStatus::Ok",
            "applyChatJournalReadPlan(selected, plan.messageCount)",
            "kitsu.messages-mark-read.v1",
            "marked_count",
            "unchanged_count",
            "journal_session",
            "journal_revision",
            "request_rejected",
        ):
            self.assertIn(required, mark_read_contract)
        handler = mark_read_contract.split("bool markMessagesRead", 1)[1]
        self.assertLess(
            handler.index("status != PlanStatus::Ok"),
            handler.index("applyChatJournalReadPlan(selected, plan.messageCount)"),
        )

        for error in (
            "journal_session_mismatch",
            "snapshot_changed",
            "message_not_inbound",
        ):
            self.assertIn(f'return "{error}"', MESSAGE_READ)
        self.assertIn("kMaximumMessageIds = 24U", (
            ROOT / "src" / "kitsu_message_read_contract.h"
        ).read_text(encoding="utf-8"))

        apply_read = MAIN.split("uint8_t applyChatJournalReadPlan", 2)[2].split(
            "void markChatJournalRead", 1
        )[0]
        self.assertIn("!chatJournal[index].inbound", apply_read)
        self.assertIn("!chatJournal[index].unread", apply_read)
        self.assertIn("touchChatJournal(chatJournal[index])", apply_read)
        self.assertIn("unreadChatMessages = unread", apply_read)
        self.assertIn("revisionBatchRequiresGenerationAdvance(", apply_read)
        self.assertLess(
            apply_read.index("revisionBatchRequiresGenerationAdvance("),
            apply_read.index("chatJournal[index].unread = false"),
        )
        local_mark_all = MAIN.split("void markChatJournalRead", 1)[1].split(
            "ChatJournalEntry* chatJournalNewest", 1
        )[0]
        self.assertIn("applyChatJournalReadPlan(selected, selectedCount)",
                      local_mark_all)

    def test_journal_generation_rotates_on_message_id_and_revision_wrap(self):
        advance_generation = MAIN.split(
            "void advanceChatJournalGeneration()", 1
        )[1].split("uint32_t allocateChatMessageId", 1)[0]
        self.assertIn("advanceJournalSession(chatSession)", advance_generation)
        self.assertIn("chatJournalRevision = 0U", advance_generation)
        self.assertIn("chatJournal[index].unread = false", advance_generation)
        self.assertIn("unreadChatMessages = 0U", advance_generation)
        allocate_id = MAIN.split("uint32_t allocateChatMessageId", 1)[1].split(
            "uint32_t allocateChatRevision", 1
        )[0]
        allocate_revision = MAIN.split("uint32_t allocateChatRevision", 1)[1].split(
            "void touchChatJournal", 1
        )[0]
        for allocator in (allocate_id, allocate_revision):
            self.assertIn("advanceChatJournalGeneration()", allocator)
        self.assertIn("return current == 0U ? 1U : current", MESSAGE_READ)

        touch = MAIN.split("void touchChatJournal", 1)[1].split(
            "ChatJournalEntry& appendChatJournal", 1
        )[0]
        self.assertIn("entry.journalSession = chatSession", touch)
        v2_selection = MAIN.split("const ChatJournalEntry* nextMessageById", 1)[1].split(
            "bool appendMessageV2Item", 1
        )[0]
        self.assertIn("candidate.journalSession != chatSession", v2_selection)
        mark_read = MAIN.split("bool markMessagesRead", 1)[1].split(
            "}  // namespace companion_api", 1
        )[0]
        self.assertIn("journalSession != chatSession", mark_read)

    def test_connect_screen_has_only_bluetooth_and_back(self):
        actions = MAIN.split("enum class ConnectionAction", 1)[1].split("};", 1)[0]
        self.assertIn("Bluetooth", actions)
        self.assertIn("Back", actions)
        self.assertNotIn("Wifi", actions)
        self.assertNotIn("Gateway", actions)

    def test_corner_bluetooth_icon_has_truthful_supported_status_glyphs(self):
        indicator = MAIN.split("char bleIndicator", 1)[1].split(
            "void uiBluetoothIcon", 1
        )[0]
        self.assertIn("if (!companionBle.ready()) return '-';", indicator)
        self.assertIn("if (link.connected) return '+';", indicator)
        self.assertIn("return '!';", indicator)
        self.assertNotIn("'~'", indicator)

        icon = MAIN.split("void uiBluetoothIcon", 1)[1].split(
            "void uiConnectionIndicators", 1
        )[0]
        self.assertIn("static constexpr uint8_t ROWS[]", icon)
        self.assertIn("uiPixel(x + column, y + row)", icon)

        placement = MAIN.split("void uiConnectionIndicators", 1)[1].split(
            "const char* bluetoothStatusLabel", 1
        )[0]
        self.assertIn("uiBluetoothIcon(2, y);", placement)
        self.assertIn("uiGlyph(bleIndicator(now), 11, y + 2, 1);", placement)

    def test_legacy_network_sources_are_excluded_from_product_build(self):
        for source in (
            "kitsu_connectivity_runtime.cpp",
            "kitsu_enrollment.cpp",
            "kitsu_esp32_gateway_tls.cpp",
            "kitsu_gateway_lan_runtime.cpp",
            "kitsu_mobile_relay.cpp",
        ):
            self.assertIn(f"-<{source}>", PROFILE)
        self.assertNotIn("-<kitsu_ble_session.cpp>", PROFILE)
        self.assertNotIn("-<kitsu_ble_ota.cpp>", PROFILE)


if __name__ == "__main__":
    unittest.main()
