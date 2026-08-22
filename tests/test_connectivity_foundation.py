from __future__ import annotations

import csv
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class LocalConnectivityFoundationTests(unittest.TestCase):
    def test_partition_table_preserves_dual_apps_and_local_state(self) -> None:
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

    def test_every_pack_capacity_guard_uses_the_pack_slot(self) -> None:
        expected = {
            "tools/install_pack.py": r"PACK_PARTITION_BYTES\s*=\s*0x140000",
            "tools/make_sprites.cjs": r"PACK_PARTITION_BYTES\s*=\s*0x140000",
            "tools/test_sprite_alignment.py": r"PACK_PARTITION_BYTES\s*=\s*0x140000",
            "tools/test_v07.py": r'"pack_capacity"\s*:\s*0x140000',
        }
        for relative, pattern in expected.items():
            text = (ROOT / relative).read_text(encoding="utf-8")
            self.assertRegex(text, pattern, relative)

    def test_authenticated_ble_clock_is_local_and_updates_mesh(self) -> None:
        source = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        clock = source.split("bool syncClock", 1)[1].split(
            "bool configureMesh", 1
        )[0]
        self.assertIn("meshTransport.setEpoch(epoch)", clock)
        self.assertIn("settimeofday(&wallClock, nullptr)", clock)
        self.assertNotIn("wifiRuntime", clock)
        self.assertNotIn("gateway", clock.lower())

    def test_controller_authority_is_encrypted_and_fail_closed(self) -> None:
        header = (ROOT / "src/kitsu_device_security.h").read_text(encoding="utf-8")
        source = (ROOT / "src/kitsu_device_security.cpp").read_text(encoding="utf-8")
        self.assertIn("revokeAuthenticatedController", header)
        self.assertIn("controllerRetirementPending", header)
        self.assertIn("storage_->clearSlot(retired)", source)
        self.assertIn("retirePreviousSlot()", source)
        self.assertIn("applicationEncrypted = true", header)
        for forbidden in (
            "remoteConnectivityAllowed",
            "copyLanAuthKey",
            "rotateLanKeyAfterPhysicalConfirmation",
            "acceptLanRxSequence",
            "reserveLanTxSequenceBlock",
            "deriveConnectionStoreKey",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)
        self.assertIn("retiredMaterialPresent", source)
        self.assertIn("cursor += kRetiredKeyBytes", source)
        self.assertIn("cursor += kRetiredCounterBytes", source)

    def test_active_esp32_security_has_no_enrollment_crypto_adapter(self) -> None:
        header = (ROOT / "src/kitsu_esp32_security.h").read_text(encoding="utf-8")
        source = (ROOT / "src/kitsu_esp32_security.cpp").read_text(encoding="utf-8")
        for forbidden in (
            "kitsu_enrollment.h",
            "Esp32EnrollmentPlatformCrypto",
            "createP256CsrDer",
            "certificateBindsKeyAndCompanion",
            "mbedtls/x509_csr.h",
        ):
            self.assertNotIn(forbidden, header)
            self.assertNotIn(forbidden, source)

    def test_legacy_connectivity_retirement_is_fixed_and_fail_closed(self) -> None:
        header = (ROOT / "src/kitsu_legacy_connectivity_retirement.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/kitsu_legacy_connectivity_retirement.cpp").read_text(
            encoding="utf-8"
        )
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        for literal in (
            'kLegacyConnectivityPartitionLabel[] = "kitsu_conn"',
            'kLegacyLanReplayNamespace[] = "kitsu_lan_act"',
            "kLegacyConnectivityPartitionType = 0x01U",
            "kLegacyConnectivityPartitionSubtype = 0x40U",
            "kLegacyConnectivityPartitionAddress = 0x7b0000UL",
            "kLegacyConnectivityPartitionBytes = 0x40000U",
        ):
            self.assertIn(literal, header)
        self.assertIn("eraseEntirePartition()", header)
        self.assertNotIn("erasePartition(size_t", header)
        self.assertIn("esp_partition_find_first(", source)
        self.assertIn("exactEsp32Partition(partition_)", source)
        self.assertIn("esp_partition_erase_range(partition_, 0U,", source)
        self.assertIn("verifyErased(platform, readbackErased)", source)
        self.assertIn("PartitionReadbackFailed", source)
        self.assertIn("nvs_open(kLegacyLanReplayNamespace, NVS_READONLY", source)
        self.assertIn("nvs_erase_all(handle)", source)

        setup = main.split("void setup()", 1)[1].split("void loop()", 1)[0]
        self.assertIn("KitsuLegacyConnectivityRetirement::run(", setup)
        self.assertLess(
            setup.index("KitsuLegacyConnectivityRetirement::run("),
            setup.index("companionBle.begin()"),
        )
        self.assertLess(
            setup.index("KitsuLegacyConnectivityRetirement::run("),
            setup.index("loadState();"),
        )
        self.assertIn(
            "legacyConnectivityRetirementReady &&\n      companionBle.begin()",
            setup,
        )
        self.assertIn(
            "legacyConnectivityRetirementReady &&\n      connectivitySecurityReady && bleReady",
            setup,
        )
        loop = main.split("void loop()", 1)[1]
        self.assertIn(
            "legacyConnectivityRetirementReady &&\n"
            "                       connectivitySecurityReady && companionBle.ready()",
            loop,
        )

    def test_normal_build_excludes_legacy_network_sources(self) -> None:
        profile = (ROOT / "platformio.ini").read_text(encoding="utf-8")
        excluded = set(re.findall(r"-<([^>]+)>", profile))
        self.assertTrue(
            {
                "kitsu_connectivity_config.cpp",
                "kitsu_connectivity_runtime.cpp",
                "kitsu_enrollment.cpp",
                "kitsu_esp32_connectivity.cpp",
                "kitsu_esp32_gateway_action.cpp",
                "kitsu_esp32_gateway_tls.cpp",
                "kitsu_gateway_action_runtime.cpp",
                "kitsu_gateway_bootstrap.cpp",
                "kitsu_gateway_enrollment_flow.cpp",
                "kitsu_gateway_lan_runtime.cpp",
                "kitsu_lan_protocol.cpp",
                "kitsu_mobile_relay.cpp",
            }.issubset(excluded)
        )
        self.assertNotIn("kitsu_legacy_connectivity_retirement.cpp", excluded)


if __name__ == "__main__":
    unittest.main()
