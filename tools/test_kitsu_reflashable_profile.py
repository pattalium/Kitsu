#!/usr/bin/env python3
"""Offline, fail-closed audit for Kitsu's repurposable owner image."""

from __future__ import annotations

import csv
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
        "nvs": (0x9000, 0x5000),
        "otadata": (0xE000, 0x2000),
        "app0": (0x10000, 0x330000),
        "app1": (0x340000, 0x330000),
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
    compiler = (
        Path.home()
        / ".platformio"
        / "packages"
        / "toolchain-xtensa-esp32s3"
        / "bin"
        / "xtensa-esp32s3-elf-g++.exe"
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


def test_truthful_state_and_remote_path() -> None:
    main = read("src/main.cpp")
    for field in (
        '"security_mode"',
        '"secure_boot"',
        '"flash_encryption"',
        '"nvs_encryption"',
        '"hardware_root_protected"',
        '"application_encrypted"',
        '"remote_connectivity_allowed"',
    ):
        assert field.replace('"', '\\"') in main, field
    security = read("src/kitsu_device_security.cpp")
    assert "SecurityMode::Reflashable" in security
    assert "material_.reflashableMaterial" in security
    assert "status_.begun" in security
    assert "status_.productionReady" not in security
    runtime = read("src/kitsu_connectivity_runtime.cpp")
    assert "prerequisites.remoteConnectivityAllowed" in runtime
    assert "WiFi.begin(" in runtime


def main() -> None:
    test_selected_environment()
    test_partition_layout_is_unencrypted_and_recoverable()
    test_no_one_way_sdkconfig_or_active_profile()
    test_compile_guard_rejects_one_way_features()
    test_compile_guard_executes_for_every_forbidden_configuration()
    test_runtime_contains_no_efuse_or_silicon_lock_api()
    test_truthful_state_and_remote_path()
    print("Kitsu reflashable profile audit: PASS")


if __name__ == "__main__":
    main()
