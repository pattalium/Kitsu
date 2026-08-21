from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
ACTION = (ROOT / "src" / "kitsu_gateway_action_runtime.cpp").read_text(
    encoding="utf-8"
)
ACTION_HEADER = (
    ROOT / "src" / "kitsu_gateway_action_runtime.h"
).read_text(encoding="utf-8")
ESP_STORAGE = (
    ROOT / "src" / "kitsu_esp32_gateway_action.cpp"
).read_text(encoding="utf-8")


class GatewayMainIntegrationSourceTests(unittest.TestCase):
    def test_main_instantiates_and_services_real_steady_lan(self):
        required = (
            "Esp32GatewayLanTlsTransport gatewayLanTls",
            "KitsuDeviceSecurityLanSequenceStore",
            "KitsuGatewayLanRuntime gatewayLanRuntime",
            "GatewayLanActionDispatcher gatewayLanActions",
            "serviceGatewayLan(now, bleSession.applicationAuthenticated)",
        )
        for token in required:
            self.assertIn(token, MAIN)

        service = MAIN.split("void serviceGatewayLan", 1)[1].split(
            "void initMesh", 1
        )[0]
        self.assertIn("WifiRuntimeState::Connected", service)
        self.assertIn("trustedGatewayWallClock(epoch)", service)
        self.assertIn("!configured.gatewayEnrolled", service)
        self.assertIn("GatewayLanServiceState::EnrollmentPending", service)
        self.assertLess(
            service.index("!configured.gatewayEnrolled"),
            service.index("!deviceSecurity.remoteConnectivityAllowed()"),
        )
        self.assertIn("authenticatedBleSession", service)
        self.assertIn("stopGatewayLanRuntime();", service)
        self.assertIn("configured.generation != gatewayLanConfigGeneration", service)
        self.assertIn(
            "connectionConfigStore, gatewayLanSequences, gatewayLanCrypto,",
            service,
        )
        self.assertIn(
            "gatewayLanReplayStore, gatewayLanActions, gatewayLanTls", service
        )

    def test_enrollment_uses_distinct_bootstrap_port_and_async_polling(self):
        service = MAIN.split("void serviceGatewayEnrollment", 1)[1].split(
            "void stopGatewayLanRuntime", 1
        )[0]
        self.assertIn("trust.port = config->bootstrapPort", service)
        self.assertIn("gatewayBootstrap.beginExchangeAndInstall(", service)
        self.assertIn("gatewayBootstrap.pollExchangeAndInstall()", service)
        self.assertIn("GatewayBootstrapResult::InProgress", service)
        self.assertNotIn("gatewayBootstrap.exchangeAndInstall(", MAIN)
        bootstrap = (ROOT / "src" / "kitsu_gateway_bootstrap.cpp").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("KitsuGatewayBootstrap::exchangeAndInstall", bootstrap)
        self.assertIn("!deviceSecurity.remoteConnectivityAllowed()", service)

    def test_native_client_lan_state_is_authoritative_runtime_state(self):
        state = MAIN.split("bool buildState", 1)[1].split(
            "void appendObservationTime", 1
        )[0]
        legacy = state.split('output += ",\\\"lan_state\\\":\\\"";', 1)[1]
        legacy = legacy.split(
            'output += "\\\",\\\"gateway_lan_state\\\":\\\"";', 1
        )[0]
        self.assertIn("gatewayLanServiceStateName(gatewayLanServiceState)", legacy)
        self.assertNotIn("lanRuntimeStateName", legacy)

    def test_remote_actions_reuse_direct_executor_and_stay_allowlisted(self):
        executor = MAIN.split("class FirmwareGatewayLanActionExecutor", 1)[1]
        executor = executor.split("FirmwareGatewayLanPayloadQueue", 1)[0]
        self.assertIn("companion_api::applyAction(", executor)

        for action in (
            '"companion.pet"',
            '"companion.feed"',
            '"companion.play"',
            '"companion.listen_once"',
            '"message.send"',
        ):
            self.assertIn(action, ACTION)
        for forbidden in (
            '"sync.pull" == 0',
            '"clock.sync" == 0',
            '"mesh.configure" == 0',
            "Serial.",
        ):
            self.assertNotIn(forbidden, ACTION)

    def test_signed_long_deadline_is_narrowed_only_for_local_execution(self):
        self.assertIn("metadata.expiresEpoch < directWindowEnd", ACTION)
        self.assertIn("kBleActionMaximumExpirySeconds", ACTION)
        self.assertIn("executionExpiresEpoch", ACTION)
        # The replay reservation is made by the LAN decoder with the original
        # signed metadata expiry; the local bridge receives a separate value.
        protocol = (ROOT / "src" / "kitsu_lan_protocol.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("replayStore.acceptAction(", protocol)
        self.assertIn(
            "candidate.actionId, candidate.expiresEpoch, nowEpoch", protocol
        )

    def test_action_replay_is_two_slot_crc_readback_verified(self):
        self.assertIn("GatewayLanReplayStorageResult readSlot", ACTION_HEADER)
        self.assertIn("crc32", ACTION)
        self.assertIn("verificationBytes == sizeof(Blob)", ACTION)
        self.assertIn("memcmp(verification, &candidate", ACTION)
        self.assertIn("expiresEpoch < acceptedEpoch", ACTION)
        self.assertIn('constexpr const char* kReplayKeys[2] = {"rx0", "rx1"}', ESP_STORAGE)
        self.assertIn("preferences_.putBytes", ESP_STORAGE)

    def test_status_and_signed_snapshot_are_bounded_and_secret_free(self):
        self.assertIn("KITSU_GATEWAY_LAN state=%s result=%s", MAIN)
        self.assertIn("gateway_lan_last_result", MAIN)
        self.assertIn("gateway_lan_queue_depth", MAIN)
        self.assertIn("gatewayLanPayloadQueue.canEnqueue(1U, byteCount)", MAIN)
        self.assertIn('"companion.snapshot"', MAIN)
        self.assertIn("kitsu.companion-snapshot.v1", MAIN)
        self.assertIn("gatewaySnapshotTruthHash(now)", MAIN)
        self.assertIn("kMeshChannelCapacity == 4U", MAIN)
        snapshot = MAIN.split("bool buildGatewaySnapshot", 1)[1].split(
            "void reportGatewayLanStatus", 1
        )[0]
        for field in (
            "firmware_version",
            "remote_connectivity_allowed",
            "wifi",
            "gateway",
            "channels",
            "configured",
            "enrolled",
            "lan_state",
            "max_utf8_bytes",
        ):
            self.assertIn(field, snapshot)
        for forbidden in (
            "passphrase",
            "ssid",
            "caCertificate",
            "spki",
            "backendHmac",
            "privateKey",
        ):
            self.assertNotIn(forbidden, snapshot)
        status = MAIN.split("void reportGatewayLanStatus", 1)[1].split(
            "void serviceGatewayLan", 1
        )[0]
        for forbidden in ("action_id", "params", "payload", "secret", "key="):
            self.assertNotIn(forbidden, status)


if __name__ == "__main__":
    unittest.main()
