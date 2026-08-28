from __future__ import annotations

import csv
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def cpp_function(source: str, signature: str) -> str:
    start = 0
    while True:
        start = source.index(signature, start)
        opening = source.index("{", start)
        terminator = source.find(";", start, opening)
        if terminator == -1:
            break
        start += len(signature)
    depth = 0
    for cursor in range(opening, len(source)):
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
            if depth == 0:
                return source[start : cursor + 1]
    raise AssertionError(f"unterminated C++ function: {signature}")


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
        clock = cpp_function(source, "bool syncClock(")
        for required in (
            "const kitsu868::timekeeping::KitsuClock before = kitsuClock",
            "const uint32_t previousGeneration = clockSnapshotGeneration",
            "const uint8_t previousSlot = clockSnapshotSlot",
            "kitsuClock.setFromUnixSeconds(",
            "ClockSource::AuthenticatedApp",
            "commitClockMutation(before, previousGeneration, previousSlot, now",
        ):
            self.assertIn(required, clock)
        self.assertNotIn("wifiRuntime", clock)
        self.assertNotIn("gateway", clock.lower())

        commit = cpp_function(source, "bool commitClockMutation(")
        self.assertLess(
            commit.index("persistClockState()"),
            commit.index("applyClockToRuntime(now)"),
        )
        self.assertGreaterEqual(commit.count("kitsuClock = before"), 2)
        self.assertGreaterEqual(
            commit.count("clockSnapshotGeneration = previousGeneration"), 2
        )
        self.assertGreaterEqual(commit.count("clockSnapshotSlot = previousSlot"), 2)
        self.assertIn("before.trusted()", commit)

        runtime = cpp_function(source, "bool applyClockToRuntime(")
        self.assertIn("settimeofday(&systemTime, nullptr)", runtime)
        self.assertIn("meshTransport.setEpoch(", runtime)

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

    def test_controller_recovery_is_heltec_only_bounded_and_physical(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        session = (ROOT / "src/kitsu_ble_session.cpp").read_text(encoding="utf-8")
        security = (ROOT / "src/kitsu_device_security.cpp").read_text(
            encoding="utf-8"
        )
        storage = (ROOT / "src/kitsu_esp32_security.cpp").read_text(
            encoding="utf-8"
        )
        gatt = (ROOT / "src/kitsu_ble_gatt.cpp").read_text(encoding="utf-8")

        for literal in (
            "CONTROLLER_RECOVERY_HOLD_MS = 5000UL",
            "CONTROLLER_RECOVERY_CONFIRM_TIMEOUT_MS = 15000UL",
            "CONTROLLER_RECOVERY_BROWSE_TIMEOUT_MS = 30000UL",
            "ControllerManager",
            "ControllerConfirm",
            "ControllerResult",
            "CONTROLLER_RECOVERY_OPTION_COUNT",
            "controllerAtSlot",
            "RESET ALL",
            "HOLD PRG",
            "TAP CANCEL",
            "EXPIRES ",
        ):
            self.assertIn(literal, main)

        entry = main.split("void beginControllerRecovery", 1)[1].split(
            "bool controllerIdPresent", 1
        )[0]
        self.assertIn("disconnectForLocalControllerRecovery()", entry)
        self.assertIn("setLocalControllerRecoveryLocked(true)", main)
        self.assertIn("setLocalControllerRecoveryLocked(false)", main)
        self.assertIn("localControllerRecoveryLocked", gatt)
        self.assertIn("NimBLEDevice::stopAdvertising()", gatt)
        self.assertIn("advertiseOnDisconnect(!locked)", gatt)
        self.assertIn("localControllerRecoveryLocked ||", gatt)

        service = main.split("void serviceControllerRecovery", 1)[1].split(
            "uint16_t ownUidSuffix", 1
        )[0]
        self.assertIn("stableButton", service)
        self.assertIn("now - buttonPressedAt >= CONTROLLER_RECOVERY_HOLD_MS", service)
        self.assertIn("commitControllerRecovery(now)", service)

        commit = main.split("void commitControllerRecovery", 1)[1].split(
            "void uiWrappedText", 1
        )[0]
        self.assertIn("controllerRecoveryBleDisconnected(now)", commit)
        self.assertIn("revokeControllerAfterPhysicalConfirmation(", commit)
        self.assertIn("revokeAllControllersAfterPhysicalConfirmation(true)", commit)
        self.assertIn("StorageNeedsReboot", commit)
        self.assertIn("UNCERTAIN", main)
        self.assertIn("REBOOT NOW", main)
        self.assertNotIn("controllerRecoveryTargetId", session)

        allowed = session.split("bool operationAllowed", 1)[1].split(
            "}  // namespace", 1
        )[0]
        self.assertIn('"controller.forget"', allowed)
        for forbidden in (
            "controller.list",
            "controller.recover",
            "controller.reset",
            "controller.revoke",
        ):
            self.assertNotIn(forbidden, allowed)
            self.assertNotIn(forbidden, main)

        reset = security.split(
            "revokeAllControllersAfterPhysicalConfirmation", 1
        )[1].split("revokeAuthenticatedController", 1)[0]
        self.assertIn("if (!physicalConfirmed)", reset)
        self.assertIn("secureZero(material_.controllers", reset)
        self.assertNotIn("material_.deviceId", reset)
        self.assertNotIn("material_.deviceSecret", reset)
        self.assertNotIn("preferences_.clear", storage)
        self.assertIn("preferences_.remove(key)", storage)

    def test_ble_bond_recovery_keeps_controller_authority_and_is_local_only(self) -> None:
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        gatt = (ROOT / "src/kitsu_ble_gatt.cpp").read_text(encoding="utf-8")
        policy = (ROOT / "src/kitsu_ble_bond_recovery.h").read_text(
            encoding="utf-8"
        )

        self.assertIn("CLEAR BLE", main)
        self.assertIn("CONTROLLERS KEPT", main)
        self.assertIn("CONTROLLER_RECOVERY_HOLD_MS = 5000UL", main)
        commit = main.split("void commitControllerRecovery", 1)[1].split(
            "void uiWrappedText", 1
        )[0]
        self.assertIn("controllerRecoveryBleDisconnected(now)", commit)
        self.assertIn("captureControllerAuthorities", commit)
        self.assertIn("controllerAuthoritiesUnchanged", commit)
        self.assertIn("clearBleBondsForLocalRecovery", commit)
        self.assertIn("controllers_unchanged=%s", commit)
        for forbidden in (
            "revokeControllerAfterPhysicalConfirmation",
            "revokeAllControllersAfterPhysicalConfirmation",
            "preferences.",
            "companionPack",
            "companionBrain",
            "encounterCodes",
            "meshSettings",
        ):
            bond_branch = commit.split("if (clearBleBonds)", 1)[1].split(
                "if (!resetAll)", 1
            )[0]
            self.assertNotIn(forbidden, bond_branch)

        clear_api = gatt.split(
            "bool KitsuBleGattLink::clearAllBondsForLocalRecovery", 1
        )[1].split("int KitsuBleGattLink::bondCount", 1)[0]
        self.assertIn("localControllerRecoveryLocked", clear_api)
        self.assertIn("!impl_->connected", clear_api)
        self.assertIn("NimBLEDevice::deleteAllBonds()", clear_api)
        self.assertIn("NimBLEDevice::getNumBonds()", clear_api)
        self.assertNotIn("Preferences", clear_api)
        self.assertIn("roots[kKitsuControllerCapacity]", policy)
        self.assertIn("controllerAuthoritiesUnchanged", policy)
        self.assertIn("clearControllerAuthoritySnapshot", policy)
        self.assertIn("ble_gatts_set_clt_cfg_perm_flags", gatt)
        self.assertIn("BLE_ATT_F_WRITE_AUTHEN", gatt)
        self.assertIn("ble_bonds", main)
        self.assertIn("controllers", main)

    def test_android_status19_repair_reuses_one_saved_controller(self) -> None:
        android = ROOT / "platform/mobile/android/app/src/main/java/ptl/kitsu/app"
        transport = (android / "transport/BleKitsuTransport.kt").read_text(
            encoding="utf-8"
        )
        callback_policy = (android / "transport/GattCallbackBindingPolicy.kt").read_text(
            encoding="utf-8"
        )
        repository = (android / "repository/OwnerRepository.kt").read_text(
            encoding="utf-8"
        )
        ui = (android / "ui/KitsuSettingsScreen.kt").read_text(encoding="utf-8")

        self.assertIn('0x13 -> "bluetooth_pairing_repair_required"', callback_policy)
        callback = transport.split(
            "private val callback = object : BluetoothGattCallback()", 1
        )[1].split("private fun acceptBytes", 1)[0]
        for lifecycle in (
            "onConnectionStateChange",
            "onServicesDiscovered",
            "onDescriptorWrite",
            "onMtuChanged",
            "onCharacteristicWrite",
            "onCharacteristicChanged",
        ):
            section = callback.split(f"override fun {lifecycle}", 1)[1]
            self.assertIn("GattCallbackBindingPolicy.accepts", section, lifecycle)

        repair_transport = transport.split(
            "override suspend fun repairBluetoothPairing", 1
        )[1].split("override fun cancelPairing", 1)[0]
        self.assertIn("repairBluetoothBondWithPermission", repair_transport)
        self.assertIn("saved_controller_changed_during_repair", repair_transport)
        self.assertNotIn("ControllerPairingProtocol().pair", repair_transport)
        self.assertNotIn("saveBondedCompanion", repair_transport)
        self.assertNotIn("MAX_SAVED_KITSU", repair_transport)
        repair_bond = transport.split(
            "private suspend fun repairBluetoothBondWithPermission", 1
        )[1].split("private suspend fun connectWithPermission", 1)[0]
        self.assertIn("ensureFreshBonded", repair_bond)
        self.assertIn("ANDROID_FORGET_REQUIRED", repair_bond)
        self.assertNotIn("ControllerPairingProtocol().pair", repair_bond)
        self.assertNotIn("saveBondedCompanion", repair_bond)

        repair_repository = repository.split(
            "suspend fun repairBluetoothPairing", 1
        )[1].split("fun cancelBluetoothPairingRepair", 1)[0]
        self.assertEqual(
            repair_repository.count("coordinator.connect(userInitiated = true)"),
            1,
        )
        self.assertNotIn("pairController(", repair_repository)
        self.assertNotIn("saveBondedCompanion", repair_repository)
        self.assertNotIn("removeBondedCompanion", repair_repository)
        self.assertIn("does not consume another controller slot", ui)
        self.assertIn("encounter unlocks, and app data stay unchanged", ui)
        self.assertIn("CLEAR BLE BONDS / CONTROLLERS KEPT", ui)
        self.assertIn(
            "enabled = !updateBusy && owner.savedKitsu.size < MAX_SAVED_KITSU",
            ui,
        )
        new_pairing = transport.split(
            "private suspend fun pairControllerWithPermission", 1
        )[1].split("private suspend fun repairBluetoothBondWithPermission", 1)[0]
        self.assertIn("controller_already_saved_use_repair_or_forget", new_pairing)
        self.assertIn("saved.any", new_pairing)
        self.assertIn("saved.size >= MAX_SAVED_KITSU", new_pairing)
        self.assertLess(
            new_pairing.index("controller_already_saved_use_repair_or_forget"),
            new_pairing.index("ControllerPairingProtocol().pair"),
        )
        self.assertIn("BOND_REGISTRATION_TIMEOUT_MILLIS", transport)
        self.assertIn("notificationSubscriptionFailure(status)", transport)

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
        self.assertIn("eraseAfterReplacementPrepared()", header)
        self.assertIn("eraseAfterReplacementTransaction()", header)
        self.assertNotIn("erasePartition(size_t", header)
        self.assertIn("esp_partition_find_first(", source)
        self.assertIn("exactEsp32Partition(partition_)", source)
        self.assertIn("esp_partition_erase_range(partition_, 0U,", source)
        self.assertIn(
            "verifyErased(platform, firstRetiredByte, readbackErased)", source
        )
        self.assertIn("KITSU_REPLACEMENT_TRANSACTION_BYTES", source)
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
