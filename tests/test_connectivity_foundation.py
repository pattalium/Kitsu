from __future__ import annotations

import csv
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class ConnectivityFoundationTests(unittest.TestCase):
    def test_partition_table_preserves_apps_and_reserves_connectivity(self) -> None:
        rows: dict[str, tuple[str, str, int, int]] = {}
        with (ROOT / "partitions_kitsu_8MB.csv").open(
            "r", encoding="utf-8", newline=""
        ) as source:
            for raw in csv.reader(source):
                if not raw or raw[0].lstrip().startswith("#"):
                    continue
                name, kind, subtype, offset, size = (part.strip() for part in raw[:5])
                rows[name] = (kind, subtype, int(offset, 0), int(size, 0))

        self.assertEqual(rows["nvs"], ("data", "nvs", 0x9000, 0x5000))
        self.assertEqual(rows["otadata"], ("data", "ota", 0xE000, 0x2000))
        self.assertEqual(rows["app0"], ("app", "ota_0", 0x10000, 0x330000))
        self.assertEqual(rows["app1"], ("app", "ota_1", 0x340000, 0x330000))
        self.assertEqual(rows["spiffs"], ("data", "spiffs", 0x670000, 0x140000))
        self.assertEqual(rows["kitsu_conn"], ("data", "0x40", 0x7B0000, 0x40000))
        self.assertEqual(rows["coredump"], ("data", "coredump", 0x7F0000, 0x10000))
        self.assertIn(
            "board_build.partitions = partitions_kitsu_8MB.csv",
            (ROOT / "platformio.ini").read_text(encoding="utf-8"),
        )

    def test_every_pack_capacity_guard_uses_the_shrunken_slot(self) -> None:
        expected = {
            "tools/install_pack.py": r"PACK_PARTITION_BYTES\s*=\s*0x140000",
            "tools/make_sprites.cjs": r"PACK_PARTITION_BYTES\s*=\s*0x140000",
            "tools/test_sprite_alignment.py": r"PACK_PARTITION_BYTES\s*=\s*0x140000",
            "tools/test_v07.py": r'"pack_capacity"\s*:\s*0x140000',
        }
        for relative, pattern in expected.items():
            text = (ROOT / relative).read_text(encoding="utf-8")
            self.assertRegex(text, pattern, relative)

    def test_ble_contract_exposes_receipts_and_safe_status_only(self) -> None:
        source = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        for operation in ("wifi.configure", "wifi.retry", "gateway.configure"):
            self.assertIn(operation, source)
        for schema in (
            "kitsu.wifi-config.v1",
            "kitsu.wifi-retry.v1",
            "kitsu.gateway-config.v2",
        ):
            self.assertIn(schema, source)
        for field in (
            "wifi_configured",
            "wifi_state",
            "gateway_configured",
            "gateway_enrolled",
            "lan_state",
        ):
            self.assertIn(field, source)
        self.assertIn("settimeofday(&wallClock, nullptr)", source)
        # Provisioning receipts and state never include these secret fields.
        receipt = source[source.index("void appendConfigurationReceipt") :]
        receipt = receipt[: receipt.index("bool configureWifi")]
        self.assertNotIn("passphrase", receipt)
        self.assertNotIn("ca_cert_der_b64", receipt)
        self.assertNotIn("spki_sha256_b64", receipt)

    def test_gateway_v2_has_distinct_bootstrap_and_steady_ports(self) -> None:
        config = (ROOT / "src/kitsu_connectivity_config.cpp").read_text(
            encoding="utf-8"
        )
        header = (ROOT / "src/kitsu_connectivity_config.h").read_text(
            encoding="utf-8"
        )
        self.assertIn("uint16_t bootstrapPort = 0U", header)
        decoder = config.split("ConfigResult decodeGatewayConfig", 1)[1].split(
            "ConnectionConfigStore::ConnectionConfigStore", 1
        )[0]
        self.assertIn('findField(fields, count, "bootstrap_port"', decoder)
        self.assertIn("count != 7U", decoder)
        self.assertIn("parsedPort == parsedBootstrapPort", decoder)
        self.assertIn("kLegacyConnectionVersion = 1U", config)
        self.assertIn("kConnectionVersion = 2U", config)
        self.assertIn("steadyPort == kLegacySteadyPort", config)
        self.assertIn("kLegacyBootstrapPort", config)

    def test_cold_boot_time_recovery_is_async_and_provenance_gated(self) -> None:
        runtime = (ROOT / "src/kitsu_connectivity_runtime.cpp").read_text(
            encoding="utf-8"
        )
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        self.assertIn('configTime(0, 0, "time.cloudflare.com"', runtime)
        self.assertIn("configured.gatewayConfigured &&", runtime)
        self.assertIn("SNTP_SYNC_STATUS_COMPLETED", runtime)
        self.assertIn("kNetworkTimeStabilityMs", runtime)
        self.assertIn("TrustedTimeSource::NetworkTime", runtime)
        self.assertIn("TrustedTimeSource::AuthenticatedBle", runtime)
        self.assertIn("!remoteConnectivityAllowed_", runtime)
        self.assertIn("trustedWallClock(int64_t& epoch", runtime)
        self.assertIn("wifiRuntime.noteAuthenticatedTime(epoch)", main)
        self.assertIn("meshTransport.setEpoch", main)

    def test_wifi_begin_requires_healthy_reflashable_store(self) -> None:
        source = (ROOT / "src/kitsu_connectivity_runtime.cpp").read_text(
            encoding="utf-8"
        )
        start = source.index("bool Esp32WifiRuntime::startAssociation()")
        end = source.index("void Esp32WifiRuntime::stopAssociation()")
        body = source[start:end]
        self.assertLess(body.index("!remoteConnectivityAllowed_"), body.index("WiFi.begin("))
        self.assertLess(body.index("WiFi.persistent(false)"), body.index("WiFi.begin("))
        policy = source[source.index("WifiPolicyAction ConnectivityPolicy::tick") : start]
        self.assertIn("WifiRuntimeState::ConnectivityUnavailable", policy)
        self.assertIn("WifiPolicyAction::Stop", policy)

    def test_wifi_is_independent_and_verified_commits_reload_the_driver(self) -> None:
        runtime = (ROOT / "src/kitsu_connectivity_runtime.cpp").read_text(
            encoding="utf-8"
        )
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        policy = runtime.split("WifiPolicyAction ConnectivityPolicy::tick", 1)[1]
        policy = policy.split("ConnectivityRuntimeStatus ConnectivityPolicy::status", 1)[0]
        self.assertIn("if (!prerequisites.wifiConfigured)", policy)
        self.assertNotIn(
            "!prerequisites.wifiConfigured || !prerequisites.gatewayConfigured",
            policy,
        )
        self.assertIn("LanRuntimeState::BleActive", policy)
        self.assertNotIn("priorBleAuthenticated_", policy)
        commit = main.split("bool configureWifi", 1)[1].split(
            "bool configureGateway", 1
        )[0]
        self.assertLess(
            commit.index("connectionConfigStore.commitWifi(config)"),
            commit.index("wifiRuntime.requestCredentialReload()"),
        )
        loop = runtime.split("void Esp32WifiRuntime::loop", 1)[1].split(
            "void Esp32WifiRuntime::stop", 1
        )[0]
        self.assertIn("credentialReloadRequested_", loop)
        self.assertIn("stopAssociation()", loop)
        self.assertIn("policy_.reset()", loop)

    def test_credentials_do_not_use_plaintext_preferences(self) -> None:
        for relative in (
            "src/kitsu_connectivity_config.cpp",
            "src/kitsu_esp32_connectivity.cpp",
        ):
            source = (ROOT / relative).read_text(encoding="utf-8")
            self.assertNotIn("Preferences", source)
        header = (ROOT / "src/kitsu_connectivity_config.h").read_text(
            encoding="utf-8"
        )
        self.assertRegex(
            header,
            re.compile(r"sizeof\(ConnectionConfigStore\)\s*<\s*15U\s*\*\s*1024U"),
        )


if __name__ == "__main__":
    unittest.main()
