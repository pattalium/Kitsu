from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
SESSION = (ROOT / "src" / "kitsu_ble_session.cpp").read_text(encoding="utf-8")
PROFILE = (ROOT / "platformio.ini").read_text(encoding="utf-8")


class LocalOnlyMainIntegrationSourceTests(unittest.TestCase):
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
            "channels.get",
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
