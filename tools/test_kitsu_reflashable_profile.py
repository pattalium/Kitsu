#!/usr/bin/env python3
"""Offline, fail-closed audit for Kitsu's repurposable owner image."""

from __future__ import annotations

import csv
import os
import re
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ENVIRONMENT = "heltec_wifi_lora_32_V3_reflashable"


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_selected_environment() -> None:
    platformio = read("platformio.ini")
    assert f"default_envs = {ENVIRONMENT}" in platformio
    assert f"[env:{ENVIRONMENT}]" in platformio
    assert platformio.count("[env:") == 1, "one owner image must be selectable"
    assert "framework = arduino" in platformio
    assert "framework = arduino, espidf" not in platformio
    assert "board_build.partitions = partitions_kitsu_8MB.csv" in platformio
    assert "board_upload.offset_address = 0x050000" in platformio
    assert "extra_scripts = post:tools/platformio_kitsu_upload_guard.py" in platformio
    assert "-DKITSU_SECURITY_MODE_REFLASHABLE=1" in platformio
    assert "upload_protocol = esptool" in platformio
    assert "KITSU_PRODUCTION_PROFILE" not in platformio
    assert "KITSU_CONNECTIVITY_DEVELOPMENT" not in platformio
    assert "partitions_kitsu_production_8MB.csv" not in platformio


def test_partition_layout_is_unencrypted_and_recoverable() -> None:
    rows: dict[str, tuple[int, int, str]] = {}
    with (ROOT / "partitions_kitsu_8MB.csv").open(
        "r", encoding="utf-8", newline=""
    ) as stream:
        for raw in csv.reader(stream):
            if not raw or raw[0].lstrip().startswith("#"):
                continue
            name = raw[0].strip()
            rows[name] = (int(raw[3].strip(), 0), int(raw[4].strip(), 0),
                          ",".join(raw[5:]).strip().lower())
    expected = {
        "nvs": (0x9000, 0x40000),
        "otadata": (0x49000, 0x2000),
        "app0": (0x50000, 0x300000),
        "app1": (0x350000, 0x300000),
        "spiffs": (0x670000, 0x140000),
        "kitsu_conn": (0x7B0000, 0x40000),
        "coredump": (0x7F0000, 0x10000),
    }
    assert set(rows) == set(expected)
    for name, (offset, size) in expected.items():
        assert rows[name][:2] == (offset, size), name
        assert "encrypted" not in rows[name][2], name


def test_no_one_way_sdkconfig_or_active_profile() -> None:
    assert not (ROOT / "sdkconfig.defaults").exists()
    assert not (ROOT / "sdkconfig.heltec_wifi_lora_32_V3_production").exists()
    assert "heltec_wifi_lora_32_V3_production" not in read("platformio.ini")


def test_compile_guard_rejects_one_way_features() -> None:
    guard = read("src/kitsu_reflashable_profile.h")
    required = (
        "CONFIG_SECURE_BOOT",
        "CONFIG_SECURE_BOOT_V2_ENABLED",
        "CONFIG_SECURE_FLASH_ENC_ENABLED",
        "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE",
        "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT",
        "CONFIG_NVS_ENCRYPTION",
        "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK",
        "CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE",
        "CONFIG_SECURE_DISABLE_ROM_DL_MODE",
    )
    for token in required:
        assert token in guard, token
    for literal in (
        'kSelectedSecurityModeName = "reflashable"',
        "kSecureBootEnabled = false",
        "kFlashEncryptionEnabled = false",
        "kNvsEncryptionEnabled = false",
        "kHardwareRootProtected = false",
        "kApplicationRecordsEncrypted = true",
        "kEfuseProgrammingAllowed = false",
    ):
        assert literal in guard, literal


def test_compile_guard_executes_for_every_forbidden_configuration() -> None:
    compiler_name = "xtensa-esp32s3-elf-g++.exe" if os.name == "nt" else (
        "xtensa-esp32s3-elf-g++"
    )
    compiler = (
        Path.home()
        / ".platformio"
        / "packages"
        / "toolchain-xtensa-esp32s3"
        / "bin"
        / compiler_name
    )
    assert compiler.is_file(), compiler
    forbidden = (
        "KITSU_PRODUCTION_PROFILE",
        "CONFIG_SECURE_BOOT",
        "CONFIG_SECURE_BOOT_V2_ENABLED",
        "CONFIG_SECURE_FLASH_ENC_ENABLED",
        "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE",
        "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT",
        "CONFIG_NVS_ENCRYPTION",
        "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK",
        "CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE",
        "CONFIG_SECURE_DISABLE_ROM_DL_MODE",
    )
    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-guard-") as raw:
        temp = Path(raw)
        source = temp / "guard.cpp"
        source.write_text(
            '#include "kitsu_reflashable_profile.h"\nint main() { return 0; }\n',
            encoding="utf-8",
        )
        base = [
            str(compiler),
            "-std=gnu++17",
            "-x",
            "c++",
            "-c",
            str(source),
            "-I",
            str(ROOT / "src"),
            "-DARDUINO_ARCH_ESP32=1",
            "-DKITSU_REFLASHABLE_GUARD_HOST_TEST=1",
            "-DKITSU_SECURITY_MODE_REFLASHABLE=1",
        ]
        accepted = subprocess.run(
            base + ["-o", str(temp / "accepted.o")],
            check=False,
            capture_output=True,
            text=True,
        )
        assert accepted.returncode == 0, accepted.stderr
        for index, macro in enumerate(forbidden):
            rejected = subprocess.run(
                base + [f"-D{macro}=1", "-o", str(temp / f"rejected-{index}.o")],
                check=False,
                capture_output=True,
                text=True,
            )
            assert rejected.returncode != 0, macro
            assert "Reflashable Kitsu forbids" in rejected.stderr, (
                macro,
                rejected.stderr,
            )


def test_runtime_contains_no_efuse_or_silicon_lock_api() -> None:
    sources = "\n".join(
        path.read_text(encoding="utf-8")
        for path in sorted((ROOT / "src").glob("*"))
        if path.suffix in {".cpp", ".h"} and path.name != "kitsu_reflashable_profile.h"
    )
    forbidden = (
        "esp_efuse",
        "espefuse",
        "esp_hmac_calculate",
        "esp_secure_boot",
        "esp_flash_encryption",
        "burn_efuse",
        "write_efuse",
        "KITSU_PRODUCTION_PROFILE",
        "productionSecurityReady",
        "security_blocked",
    )
    for token in forbidden:
        assert token not in sources, token
    # The only permitted silicon identity access is the read-only Arduino API
    # for the public factory MAC: once for the display suffix and once for the
    # recoverable application-wrapping domain. It cannot program or lock bits.
    assert sources.count("ESP.getEfuseMac()") == 2
    for programming_api in (
        "esp_efuse_write",
        "esp_efuse_batch_write",
        "esp_efuse_write_field",
        "esp_efuse_write_block",
        "esp_efuse_set_write_protect",
        "esp_efuse_set_read_protect",
        "esp_efuse_burn",
    ):
        assert programming_api not in sources, programming_api
    cmake = read("src/CMakeLists.txt")
    assert not re.search(r"\bREQUIRES\b[^\n]*\befuse\b", cmake)


def test_truthful_local_only_state_and_supported_build_path() -> None:
    main = read("src/main.cpp")
    for field in (
        '"security_mode"',
        '"secure_boot"',
        '"flash_encryption"',
        '"nvs_encryption"',
        '"hardware_root_protected"',
        '"application_encrypted"',
    ):
        assert field.replace('"', '\\"') in main, field
    for forbidden in (
        "remote_connectivity_allowed",
        "WiFi.begin(",
        "wifi.configure",
        "gateway.configure",
        "gateway.enroll",
        "mobile.relay",
    ):
        assert forbidden not in main, forbidden

    security = read("src/kitsu_device_security.cpp")
    assert "SecurityMode::Reflashable" in security
    assert "status_.begun" in security
    assert "status_.productionReady" not in security

    # Root CMake deliberately makes direct ESP-IDF/mixed-framework builds
    # unreachable. src/CMakeLists.txt is archival metadata; the sole supported
    # product graph is the pinned PlatformIO environment below.
    root_cmake = read("CMakeLists.txt")
    assert "message(FATAL_ERROR" in root_cmake
    assert "Direct ESP-IDF builds are unsupported" in root_cmake
    assert "heltec_wifi_lora_32_V3_reflashable" in root_cmake
    assert "add_subdirectory" not in root_cmake

    profile = read("platformio.ini")
    legacy_sources = (
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
    )
    for source in legacy_sources:
        assert f"-<{source}>" in profile, source
    for active in (
        "kitsu_ble_action.cpp",
        "kitsu_ble_gatt.cpp",
        "kitsu_ble_session.cpp",
        "kitsu_ble_ota.cpp",
        "kitsu_device_security.cpp",
        "kitsu_esp32_security.cpp",
        "kitsu_legacy_connectivity_retirement.cpp",
    ):
        assert f"-<{active}>" not in profile, active


def test_mesh_rx_rehydrates_verified_clients_after_successful_boot() -> None:
    main = read("src/main.cpp")
    start = main.index("void initMesh()")
    end = main.index("\n}\n\n}  // namespace", start)
    init_mesh = main[start:end]

    begin = "meshInitStatus = meshTransport.begin(meshSettings, meshIdentity);"
    stage = "(void)meshTransport.stageObservedContact("
    assert begin in init_mesh
    assert stage in init_mesh
    assert init_mesh.index(begin) < init_mesh.index(stage)
    assert (
        "meshInitStatus == kitsu868::mesh::TransportStatus::Ok &&\n"
        "      discoveryJournalReady"
    ) in init_mesh
    assert "discoveryJournal.peerAt(ordinal, peer)" in init_mesh
    assert "if (peer.type != 1U) continue;" in init_mesh
    assert "peer.publicKey, name, peer.type, peer.senderAdvertTimestamp" in init_mesh
    assert "upsertContact(" not in init_mesh
    assert "discoveryJournal.record(" not in init_mesh
    assert "discoveryJournal.flush(" not in init_mesh


def test_ble_close_telemetry_is_volatile_and_visible_in_selftest() -> None:
    header = read("src/kitsu_ble_gatt.h")
    adapter = read("src/kitsu_ble_gatt.cpp")
    session_header = read("src/kitsu_ble_session.h")
    session = read("src/kitsu_ble_session.cpp")
    main_source = read("src/main.cpp")
    selftest = main_source[
        main_source.index("void printSelfTest()"):
        main_source.index("void printSync()")
    ]

    for cause in (
        "RemoteUserTerminated",
        "SupervisionTimeout",
        "Unknown",
        "LinkRejected",
        "FrameTimedOut",
        "ProtocolViolation",
        "TransportFailure",
        "SecureLinkRejected",
        "HandshakeTimeout",
        "AuthenticationFailed",
        "SessionProtocolViolation",
        "ResponseSendFailed",
        "ControllerForget",
        "ApplicationRequest",
        "ControllerRecovery",
    ):
        assert cause in header
    for field in (
        "ble_connected",
        "ble_application_authenticated",
        "ble_last_close_available",
        "ble_last_close_cause",
        "ble_last_close_local",
        "ble_last_disconnect_reason_available",
        "ble_last_disconnect_reason",
        "ble_last_disconnect_at_ms",
        "ble_last_notify_status_available",
        "ble_last_notify_status",
    ):
        assert field.replace('"', '\\"') in selftest
    assert (
        'if (command == "status" || command == "selftest") printSelfTest();'
        in main_source
    )
    assert "case 0x13:" in adapter
    assert "BleCloseCause::RemoteUserTerminated" in adapter
    assert "case 0x08:" in adapter
    assert "BleCloseCause::SupervisionTimeout" in adapter
    assert "return BleCloseCause::Unknown;" in adapter
    assert "disconnectBle(BleCloseCause cause)" in session_header
    assert "BleCloseCause pendingCloseCause_" in session_header
    for propagated in (
        "HandshakeTimeout",
        "AuthenticationFailed",
        "SessionProtocolViolation",
        "ResponseSendFailed",
        "ControllerForget",
    ):
        assert f"BleCloseCause::{propagated}" in session
    assert "link_.disconnect(cause);" in main_source

    # The adapter owns only process-lifetime state. Diagnostics must never
    # create flash wear or become another pairing/controller data store.
    combined = header + adapter
    for persistence_api in (
        "Preferences",
        "nvs_set_",
        "nvs_commit",
        ".putUInt(",
        ".putBytes(",
        ".putString(",
    ):
        assert persistence_api not in combined


def main() -> None:
    test_selected_environment()
    test_partition_layout_is_unencrypted_and_recoverable()
    test_no_one_way_sdkconfig_or_active_profile()
    test_compile_guard_rejects_one_way_features()
    test_compile_guard_executes_for_every_forbidden_configuration()
    test_runtime_contains_no_efuse_or_silicon_lock_api()
    test_truthful_local_only_state_and_supported_build_path()
    test_mesh_rx_rehydrates_verified_clients_after_successful_boot()
    test_ble_close_telemetry_is_volatile_and_visible_in_selftest()
    print("Kitsu reflashable profile audit: PASS")


if __name__ == "__main__":
    main()
