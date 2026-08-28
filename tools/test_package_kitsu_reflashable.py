#!/usr/bin/env python3
"""Fail-closed tests for the local-only Kitsu serial candidate packager."""

from __future__ import annotations

import copy
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import package_kitsu_reflashable as packager  # noqa: E402


EXPECTED_TOP_LEVEL = {
    "schema",
    "created_at",
    "artifact_status",
    "firmware_version",
    "release_channel",
    "device_class",
    "checksum_index",
    "build_profile",
    "firmware_identity",
    "rollback_configuration",
    "partition_layout",
    "security_profile",
    "product_contract",
    "operations",
    "preserves",
    "release_requirements",
    "flash_artifacts",
    "serial_flash",
    "warnings",
}

EXPECTED_ROLES = [
    "bootloader",
    "partition_table",
    "application_app0",
    "ota_journal_app0_clear",
    "application_app1",
    "ota_journal_app1_clear",
    "legacy_connectivity_clear",
]
EXPECTED_PARTITIONS = [
    "bootloader",
    "partition_table",
    "app0",
    "app0_journal",
    "app1",
    "app1_journal",
    "retired_legacy_connectivity",
]
EXPECTED_OFFSETS = [
    0x000000,
    0x008000,
    0x010000,
    0x33F000,
    0x340000,
    0x66F000,
    0x7B0000,
]


def run(command: list[str], *, success: bool) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        command, check=False, capture_output=True, text=True, timeout=60
    )
    if success and completed.returncode != 0:
        raise AssertionError(
            f"command failed ({completed.returncode}):\n"
            f"stdout={completed.stdout}\nstderr={completed.stderr}"
        )
    if not success and completed.returncode == 0:
        raise AssertionError(f"command unexpectedly succeeded: {command}")
    return completed


def raises_system_exit(callback: Callable[[], None], expected: str) -> None:
    try:
        callback()
    except SystemExit as error:
        assert expected in str(error), (expected, str(error))
    else:
        raise AssertionError("operation unexpectedly succeeded")


def make_esp_image(path: Path, segments: list[tuple[int, bytes]]) -> None:
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = len(segments)
    header[2] = 2  # DIO
    header[3] = 0x4F  # 8 MiB, 80 MHz (also checked independently by esptool)
    header[4:8] = struct.pack("<I", 0x40370000)
    header[12:14] = struct.pack("<H", 9)  # ESP32-S3
    header[23] = 1  # appended SHA-256 validation hash
    image = bytearray(header)
    checksum = 0xEF
    for address, payload in segments:
        image.extend(struct.pack("<II", address, len(payload)))
        image.extend(payload)
        for byte in payload:
            checksum ^= byte
    checksum_offset = ((len(image) + 16) // 16) * 16 - 1
    image.extend(b"\x00" * (checksum_offset - len(image)))
    image.append(checksum)
    image.extend(hashlib.sha256(image).digest())
    path.write_bytes(image)


def make_partition_binary(path: Path, *, encrypted_label: str | None = None) -> None:
    table = bytearray()
    for label, type_code, subtype, offset, size, _ in packager.EXPECTED_LAYOUT:
        encoded = label.encode("ascii") + b"\x00"
        encoded += b"\x00" * (16 - len(encoded))
        flags = 1 if label == encrypted_label else 0
        table.extend(
            struct.pack(
                "<2sBBII16sI",
                packager.PARTITION_MAGIC,
                type_code,
                subtype,
                offset,
                size,
                encoded,
                flags,
            )
        )
    digest = hashlib.md5(table).digest()  # nosec B324 - ESP partition format
    table.extend(packager.PARTITION_MD5_MAGIC + b"\xff" * 14 + digest)
    table.extend(b"\xff" * (packager.PARTITION_BINARY_BYTES - len(table)))
    path.write_bytes(table)


def make_profile(project: Path, *, extra_section: str = "") -> None:
    (project / "platformio.ini").write_text(
        """[platformio]
default_envs = heltec_wifi_lora_32_V3_reflashable

[env:heltec_wifi_lora_32_V3_reflashable]
platform = espressif32@6.13.0
board = heltec_wifi_lora_32_V3
framework = arduino
board_build.partitions = partitions_kitsu_8MB.csv
upload_protocol = esptool
build_src_filter =
    +<*>
    -<kitsu_connectivity_config.cpp>
    -<kitsu_connectivity_runtime.cpp>
    -<kitsu_enrollment.cpp>
    -<kitsu_esp32_connectivity.cpp>
    -<kitsu_esp32_gateway_action.cpp>
    -<kitsu_esp32_gateway_tls.cpp>
    -<kitsu_gateway_action_runtime.cpp>
    -<kitsu_gateway_bootstrap.cpp>
    -<kitsu_gateway_enrollment_flow.cpp>
    -<kitsu_gateway_lan_runtime.cpp>
    -<kitsu_lan_protocol.cpp>
    -<kitsu_mobile_relay.cpp>
build_flags =
    -DKITSU_SECURITY_MODE_REFLASHABLE=1
    -DCORE_DEBUG_LEVEL=0
"""
        + extra_section,
        encoding="utf-8",
    )
    shutil.copyfile(ROOT / packager.PARTITION_LAYOUT, project / packager.PARTITION_LAYOUT)
    source = project / "src"
    source.mkdir(exist_ok=True)
    (source / "main.cpp").write_text(
        'constexpr char FIRMWARE_VERSION[] = "0.14.0";\n', encoding="utf-8"
    )


def sdkconfig_path(project: Path) -> Path:
    return (
        project
        / "framework-arduinoespressif32"
        / "tools"
        / "sdk"
        / "esp32s3"
        / "qio_qspi"
        / "include"
        / "sdkconfig.h"
    )


def make_sdkconfig(
    path: Path,
    *,
    rollback: bool = True,
    anti_rollback: bool = False,
    secure_boot: bool = False,
    flash_encryption: bool = False,
) -> None:
    lines = ["#pragma once"]
    if rollback:
        lines.extend(
            (
                "#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1",
                "#define CONFIG_APP_ROLLBACK_ENABLE CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE",
            )
        )
    if anti_rollback:
        lines.append("#define CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK 1")
    if secure_boot:
        lines.extend(
            (
                "#define CONFIG_SECURE_BOOT 1",
                "#define CONFIG_SECURE_BOOT_V2_ENABLED 1",
            )
        )
    if flash_encryption:
        lines.append("#define CONFIG_SECURE_FLASH_ENC_ENABLED 1")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def bind_sdkconfig(build: Path, sdkconfig: Path) -> None:
    # Match the canonical path consumed by validate_build_sdkconfig().  Windows
    # runners can expose the temp directory through an 8.3 alias (RUNNER~1),
    # while Path.resolve() expands that alias; binding the unresolved fixture
    # path makes an otherwise valid synthetic build fail only on those hosts.
    canonical_sdkconfig = sdkconfig.resolve()
    (build / ".sconsign313.dblite").write_bytes(
        b"platformio-build-input\x00"
        + canonical_sdkconfig.as_posix().encode("utf-8")
        + b"\x00csig\x00"
        + packager.scons_content_signature(canonical_sdkconfig).encode("ascii")
        + b"\x00"
    )


def make_fake_esptool(path: Path, *, reject: bool = False) -> None:
    path.write_text(
        """#!/usr/bin/env python3
import struct
import sys
from pathlib import Path

if %s:
    raise SystemExit(3)
image = Path(sys.argv[-1]).read_bytes()
cursor = 24
for _ in range(image[1]):
    _, size = struct.unpack_from("<II", image, cursor)
    cursor += 8 + size
hash_start = ((cursor + 16) // 16) * 16
digest = image[hash_start:hash_start + 32].hex()
print("esptool.py v4.11.0")
print("Detected image type: ESP32-S3")
print("Flash size: 8MB")
print("Flash freq: 80m")
print("Flash mode: DIO")
print("Chip ID: 9 (ESP32-S3)")
print("Checksum: 0xaa (valid)")
print(f"Validation hash: {digest} (valid)")
"""
        % ("True" if reject else "False"),
        encoding="utf-8",
    )


def fixture(root: Path) -> tuple[Path, Path, Path]:
    project = root / "project"
    project.mkdir()
    make_profile(project)
    build = project / ".pio" / "build" / packager.ENVIRONMENT
    build.mkdir(parents=True)
    make_esp_image(build / "bootloader.bin", [(0x403C0000, b"B" * 192)])
    make_partition_binary(build / "partitions.bin")
    make_esp_image(
        build / "firmware.bin",
        [
            (0x3C000020, b"A" * 128 + b"0.14.0\x00" + b"A" * 121),
            (0x40374000, b"C" * 191),
        ],
    )
    sdkconfig = sdkconfig_path(project)
    make_sdkconfig(sdkconfig)
    bind_sdkconfig(build, sdkconfig)
    tool = root / "fake_esptool.py"
    make_fake_esptool(tool)
    return project, build, tool


def command(
    project: Path,
    build: Path,
    output: Path,
    tool: Path,
    *,
    version: str = "0.14.0",
) -> list[str]:
    return [
        sys.executable,
        str(TOOLS / "package_kitsu_reflashable.py"),
        "--project-root",
        str(project),
        "--build-dir",
        str(build),
        "--output-dir",
        str(output),
        "--esptool",
        str(tool),
        "--sdkconfig",
        str(sdkconfig_path(project)),
        "--firmware-version",
        version,
    ]


def assert_no_committed_output(output: Path) -> None:
    assert not output.exists() or not any(output.iterdir())


def verify_checksum_index(output: Path) -> None:
    lines = (output / "SHA256SUMS.txt").read_text(encoding="ascii").splitlines()
    assert lines
    names: set[str] = set()
    for line in lines:
        digest, relative = line.split("  ", 1)
        assert packager.SHA256_PATTERN.fullmatch(digest)
        assert relative not in names
        names.add(relative)
        path = output / Path(*relative.split("/"))
        assert path.is_file()
        assert hashlib.sha256(path.read_bytes()).hexdigest() == digest
    expected = {
        path.relative_to(output).as_posix()
        for path in output.rglob("*")
        if path.is_file() and path.name != "SHA256SUMS.txt"
    }
    assert names == expected


def make_release(root: Path) -> tuple[Path, dict[str, object]]:
    project, build, tool = fixture(root)
    output = root / "release"
    run(command(project, build, output, tool), success=True)
    manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
    return output, manifest


def test_success_exact_v2_contract() -> None:
    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-test-") as temp:
        output, manifest = make_release(Path(temp))
        assert set(manifest) == EXPECTED_TOP_LEVEL
        assert manifest["schema"] == packager.SCHEMA
        assert manifest["artifact_status"] == packager.ARTIFACT_STATUS
        assert manifest["release_channel"] == packager.RELEASE_CHANNEL
        assert manifest["firmware_version"] == "0.14.0"
        assert "device_id" not in manifest
        assert manifest["build_profile"]["environment"] == packager.ENVIRONMENT
        assert manifest["build_profile"]["platform"] == packager.PLATFORM_PACKAGE
        assert manifest["build_profile"]["framework"] == "arduino"
        assert manifest["build_profile"]["local_only_excluded_sources"] == list(
            packager.LOCAL_ONLY_EXCLUDED_SOURCES
        )

        identity = manifest["firmware_identity"]
        assert identity["version"] == "0.14.0"
        assert identity["source"] == "src/main.cpp"
        assert identity["image_marker_verified"] is True
        assert packager.SHA256_PATTERN.fullmatch(identity["source_sha256"])

        rollback = manifest["rollback_configuration"]
        assert rollback["source"].endswith("/esp32s3/qio_qspi/include/sdkconfig.h")
        assert rollback["platformio_build_signature"] == ".sconsign313.dblite"
        assert rollback["bootloader_app_rollback"] is True
        assert rollback["bootloader_app_anti_rollback"] is False
        assert rollback["secure_boot"] is False
        assert rollback["flash_encryption"] is False
        assert packager.SHA256_PATTERN.fullmatch(rollback["sha256"])
        assert len(rollback["platformio_content_csig"]) == 32
        assert all(
            character in "0123456789abcdef"
            for character in rollback["platformio_content_csig"]
        )
        assert packager.SHA256_PATTERN.fullmatch(
            rollback["platformio_build_signature_sha256"]
        )

        assert manifest["security_profile"] == {
            "mode": "reflashable",
            "secure_boot": False,
            "flash_encryption": False,
            "nvs_encryption": False,
            "hardware_root_protected": False,
            "firmware_images_encrypted": False,
            "rollback_bootloader": True,
            "anti_rollback": False,
            "local_controller_records_authenticated": True,
            "local_controller_records_encrypted": True,
            "efuse_writes": False,
            "efuse_locks": False,
            "jtag_disabled": False,
            "uart_download_disabled": False,
            "usb_download_disabled": False,
            "serial_erase_reflash_available": True,
            "full_chip_erase_available": True,
            "stock_meshcore_restore_available": True,
            "physical_extraction_reflash_can_bypass": True,
        }
        assert manifest["product_contract"] == {
            "mode": "local_only",
            "owner_control": "authenticated_bluetooth",
            "firmware_update": "authenticated_bluetooth",
            "account_required": False,
            "remote_service_required": False,
        }
        assert manifest["operations"] == {
            "erase_flash": False,
            "write_count": 7,
            "retire_legacy_connectivity": True,
            "runtime_retires_nvs_namespace": "kitsu_lan_act",
        }
        assert manifest["preserves"] == {
            "ota_data": True,
            "nvs_except_kitsu_lan_act": True,
            "companion_pack": True,
            "controller_store": True,
            "meshcore_state": True,
            "coredump": True,
            "efuses": True,
        }
        assert all(value is False for value in manifest["release_requirements"].values())
        assert manifest["partition_layout"]["encrypted_partition_flags"] is False

        records = manifest["flash_artifacts"]
        assert len(records) == 7
        assert [entry["role"] for entry in records] == EXPECTED_ROLES
        assert [entry["partition"] for entry in records] == EXPECTED_PARTITIONS
        assert [entry["offset"] for entry in records] == EXPECTED_OFFSETS
        assert [entry["offset_hex"] for entry in records] == [
            f"0x{offset:06X}" for offset in EXPECTED_OFFSETS
        ]
        assert records[2]["file"] == records[4]["file"] == "images/kitsu868-app.bin"
        assert records[2]["sha256"] == records[4]["sha256"]
        assert records[2]["bytes"] == records[4]["bytes"]
        assert records[3]["file"] == records[5]["file"]
        assert records[3]["sha256"] == records[5]["sha256"]
        assert records[3]["bytes"] == records[5]["bytes"] == 0x1000
        assert records[3]["sha256"] == packager.OTA_JOURNAL_CLEAR_SHA256
        assert records[6]["bytes"] == 0x40000
        assert records[6]["sha256"] == packager.LEGACY_CONNECTIVITY_CLEAR_SHA256
        assert all(not entry["secure_boot_signed"] for entry in records)
        assert all(not entry["encrypted"] for entry in records)
        assert records[2]["bytes"] <= packager.APP_IMAGE_BYTES_MAX
        assert all(
            records[index]["offset"] + records[index]["bytes"]
            <= records[index + 1]["offset"]
            for index in range(len(records) - 1)
        )
        assert (output / "images" / "kitsu868-ota-journal-clear.bin").read_bytes() == (
            b"\xff" * 0x1000
        )
        assert (
            output / "images" / "kitsu868-legacy-connectivity-clear.bin"
        ).read_bytes() == (b"\xff" * 0x40000)
        assert not (output / "images" / "0x00E000-ota-data-initial.bin").exists()

        serial = manifest["serial_flash"]
        assert serial["validated_with"] == "esptool.py v4.11.0"
        assert serial["write_count"] == 7
        assert serial["write_verify"] is True
        assert serial["readback_verify"] is True
        assert serial["readback_algorithm"] == "sha256"
        assert serial["erase_required"] is False
        assert serial["erase_flash"] is False
        write_command = serial["command"]
        verify_index = write_command.index("--verify")
        write_pairs = write_command[verify_index + 1 :]
        assert len(write_pairs) == 14
        assert [int(value, 0) for value in write_pairs[::2]] == EXPECTED_OFFSETS
        assert write_pairs[1::2] == [entry["file"] for entry in records]
        command_text = " ".join(write_command).lower()
        assert "write_flash" in command_text and "--verify" in command_text
        assert "erase_flash" not in command_text
        assert "espefuse" not in command_text
        assert "burn_" not in command_text
        assert "--encrypt" not in command_text

        readbacks = serial["readback_plan"]
        assert len(readbacks) == 7
        for index, (item, record) in enumerate(zip(readbacks, records, strict=True), 1):
            assert set(item) == {
                "role",
                "offset",
                "bytes",
                "expected_sha256",
                "output",
                "command",
            }
            assert item["role"] == record["role"]
            assert item["offset"] == record["offset"]
            assert item["bytes"] == record["bytes"]
            assert item["expected_sha256"] == record["sha256"]
            assert item["output"] == f"readback-{index:02d}-{record['role']}.bin"
            assert item["command"][-3:] == [
                record["offset_hex"],
                f"0x{record['bytes']:X}",
                item["output"],
            ]
            assert "read_flash" in item["command"]

        warning_codes = {warning["code"] for warning in manifest["warnings"]}
        assert warning_codes == {
            "PHYSICAL_ACCESS_CAN_REPLACE_FIRMWARE",
            "NO_VERIFIED_BOOT_CHAIN",
            "PLAINTEXT_FLASH_NOT_HARDWARE_PROTECTED",
            "SERIAL_RECOVERY_INTENTIONALLY_PRESERVED",
            "LEGACY_CONNECTIVITY_REGION_RETIRED",
        }
        flashing = (output / "FLASHING.txt").read_text(encoding="utf-8")
        assert "exactly seven bounded writes" in flashing
        assert "no whole-chip erase" in flashing
        assert "--verify" in flashing
        assert flashing.count("Expected SHA-256:") == 7
        assert "kitsu_lan_act" in flashing
        serialized_contract = (
            json.dumps(
                {
                    "security_profile": manifest["security_profile"],
                    "product_contract": manifest["product_contract"],
                    "warnings": manifest["warnings"],
                }
            ).lower()
            + flashing.lower()
        )
        for stale in ("tls", "oidc", "gateway", "network"):
            assert stale not in serialized_contract
        verify_checksum_index(output)


def test_flash_record_mutations_rejected() -> None:
    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-mutation-") as temp:
        _, manifest = make_release(Path(temp))
        original = manifest["flash_artifacts"]

        def rejected(mutator: Callable[[list[dict[str, object]]], None], expected: str) -> None:
            records = copy.deepcopy(original)
            mutator(records)
            raises_system_exit(lambda: packager.validate_flash_records(records), expected)

        rejected(lambda value: value.pop(), "exactly seven")
        rejected(lambda value: value.reverse(), "bootloader flash record")
        rejected(lambda value: value[2].update(extra=True), "fields differ")
        rejected(lambda value: value[2].update(bytes=packager.APP_IMAGE_BYTES_MAX + 1), "exceeds")
        rejected(lambda value: value[4].update(sha256="0" * 64), "exact validated app0")
        rejected(lambda value: value[5].update(file="images/different.bin"), "reviewed contract")
        rejected(lambda value: value[3].update(sha256="1" * 64), "both OTA journals")
        rejected(lambda value: value[6].update(bytes=0x3F000), "exact isolated partition")
        rejected(lambda value: value[6].update(sha256="2" * 64), "exact isolated partition")
        rejected(lambda value: value[6].update(offset=0x66F800), "reviewed contract")


def test_serial_plan_mutations_rejected() -> None:
    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-serial-mutation-") as temp:
        _, manifest = make_release(Path(temp))
        records = manifest["flash_artifacts"]
        original_command = manifest["serial_flash"]["command"]
        original_readbacks = manifest["serial_flash"]["readback_plan"]
        packager.validate_serial_plan(records, original_command, original_readbacks)

        command_value = copy.deepcopy(original_command)
        command_value.pop()
        raises_system_exit(
            lambda: packager.validate_serial_plan(records, command_value, original_readbacks),
            "seven-write plan",
        )

        command_value = copy.deepcopy(original_command)
        command_value[-2] = "0x7A0000"
        raises_system_exit(
            lambda: packager.validate_serial_plan(records, command_value, original_readbacks),
            "seven-write plan",
        )

        command_value = copy.deepcopy(original_command) + ["erase_flash"]
        raises_system_exit(
            lambda: packager.validate_serial_plan(records, command_value, original_readbacks),
            "seven-write plan",
        )

        readbacks = copy.deepcopy(original_readbacks)
        readbacks.pop()
        raises_system_exit(
            lambda: packager.validate_serial_plan(records, original_command, readbacks),
            "one readback",
        )

        readbacks = copy.deepcopy(original_readbacks)
        readbacks[3]["expected_sha256"] = "0" * 64
        raises_system_exit(
            lambda: packager.validate_serial_plan(records, original_command, readbacks),
            "ota_journal_app0_clear readback",
        )

        readbacks = copy.deepcopy(original_readbacks)
        readbacks[6]["command"][-3] = "0x7A0000"
        raises_system_exit(
            lambda: packager.validate_serial_plan(records, original_command, readbacks),
            "legacy_connectivity_clear readback",
        )


def test_version_relabel_and_image_marker_rejected() -> None:
    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-version-") as temp:
        root = Path(temp)
        project, build, tool = fixture(root)
        output = root / "release"
        completed = run(
            command(project, build, output, tool, version="0.14.1"), success=False
        )
        assert "does not match source version 0.14.0" in completed.stderr
        assert_no_committed_output(output)

    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-version-image-") as temp:
        root = Path(temp)
        project, build, tool = fixture(root)
        make_esp_image(build / "firmware.bin", [(0x3C000020, b"A" * 257)])
        output = root / "release"
        completed = run(command(project, build, output, tool), success=False)
        assert "NUL-terminated firmware version once" in completed.stderr
        assert_no_committed_output(output)


def test_sdkconfig_rollback_and_security_guards() -> None:
    cases = (
        ({"rollback": False}, "must define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE"),
        ({"anti_rollback": True}, "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK"),
        ({"secure_boot": True}, "CONFIG_SECURE_BOOT"),
        ({"flash_encryption": True}, "CONFIG_SECURE_FLASH_ENC_ENABLED"),
    )
    for options, expected in cases:
        with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-sdkconfig-") as temp:
            root = Path(temp)
            project, build, tool = fixture(root)
            make_sdkconfig(sdkconfig_path(project), **options)
            bind_sdkconfig(build, sdkconfig_path(project))
            output = root / "release"
            completed = run(command(project, build, output, tool), success=False)
            assert expected in completed.stderr, (expected, completed.stderr)
            assert_no_committed_output(output)

    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-sdk-binding-") as temp:
        root = Path(temp)
        project, build, tool = fixture(root)
        (build / ".sconsign313.dblite").write_bytes(b"different-build-input")
        output = root / "release"
        completed = run(command(project, build, output, tool), success=False)
        assert "does not bind the supplied SDK configuration" in completed.stderr
        assert_no_committed_output(output)

    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-sdk-csig-") as temp:
        root = Path(temp)
        project, build, tool = fixture(root)
        sdkconfig_path(project).write_text(
            sdkconfig_path(project).read_text(encoding="utf-8") + "\n",
            encoding="utf-8",
        )
        output = root / "release"
        completed = run(command(project, build, output, tool), success=False)
        assert "does not bind the current SDK configuration contents" in completed.stderr
        assert_no_committed_output(output)


def test_encrypted_image_rejected() -> None:
    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-encrypted-") as temp:
        root = Path(temp)
        project, build, tool = fixture(root)
        app = build / "firmware.bin"
        data = bytearray(app.read_bytes())
        data[0] ^= 0x55
        app.write_bytes(data)
        output = root / "release"
        completed = run(command(project, build, output, tool), success=False)
        assert "not a plaintext ESP image" in completed.stderr
        assert_no_committed_output(output)


def test_signed_or_padded_images_rejected() -> None:
    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-signed-") as temp:
        root = Path(temp)
        project, build, tool = fixture(root)
        app = build / "firmware.bin"
        app.write_bytes(app.read_bytes() + b"\xe7" + b"\x00" * 4095)
        output = root / "release"
        completed = run(command(project, build, output, tool), success=False)
        assert "trailing secure-boot padding/signature data" in completed.stderr
        assert_no_committed_output(output)

    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-padding-") as temp:
        root = Path(temp)
        project, build, tool = fixture(root)
        make_esp_image(
            build / "firmware.bin",
            [(0x3C000020, b"0.14.0\x00" + b"A" * 64), (0, b"P" * 64)],
        )
        output = root / "release"
        completed = run(command(project, build, output, tool), success=False)
        assert "secure-padding segment" in completed.stderr
        assert_no_committed_output(output)


def test_encrypted_partition_rejected() -> None:
    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-partition-") as temp:
        root = Path(temp)
        project, build, tool = fixture(root)
        make_partition_binary(build / "partitions.bin", encrypted_label="kitsu_conn")
        output = root / "release"
        completed = run(command(project, build, output, tool), success=False)
        assert "forbidden flags" in completed.stderr
        assert_no_committed_output(output)


def test_active_secure_profiles_rejected() -> None:
    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-profile-") as temp:
        root = Path(temp)
        project, build, tool = fixture(root)
        make_profile(
            project,
            extra_section="\n[env:heltec_wifi_lora_32_V3_production]\nframework=arduino, espidf\n",
        )
        output = root / "release"
        completed = run(command(project, build, output, tool), success=False)
        assert "remain selectable" in completed.stderr
        assert_no_committed_output(output)

    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-sdkconfig-defaults-") as temp:
        root = Path(temp)
        project, build, tool = fixture(root)
        (project / "sdkconfig.defaults").write_text("CONFIG_SECURE_BOOT=y\n", encoding="ascii")
        output = root / "release"
        completed = run(command(project, build, output, tool), success=False)
        assert "sdkconfig.defaults remains active-looking" in completed.stderr
        assert_no_committed_output(output)

    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-source-filter-") as temp:
        root = Path(temp)
        project, build, tool = fixture(root)
        profile = project / "platformio.ini"
        profile.write_text(
            profile.read_text(encoding="utf-8").replace(
                "    -<kitsu_mobile_relay.cpp>\n", ""
            ),
            encoding="utf-8",
        )
        output = root / "release"
        completed = run(command(project, build, output, tool), success=False)
        assert "exact reviewed local-only source filter" in completed.stderr
        assert_no_committed_output(output)

    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-platform-pin-") as temp:
        root = Path(temp)
        project, build, tool = fixture(root)
        profile = project / "platformio.ini"
        profile.write_text(
            profile.read_text(encoding="utf-8").replace(
                "espressif32@6.13.0", "espressif32@6.12.0"
            ),
            encoding="utf-8",
        )
        output = root / "release"
        completed = run(command(project, build, output, tool), success=False)
        assert "must pin espressif32@6.13.0" in completed.stderr
        assert_no_committed_output(output)


def test_sensitive_input_and_tool_failure_rejected() -> None:
    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-secret-") as temp:
        root = Path(temp)
        project, build, tool = fixture(root)
        (build / "device-xts.key").write_bytes(b"not-a-real-key")
        output = root / "release"
        completed = run(command(project, build, output, tool), success=False)
        assert "forbidden sensitive artifact" in completed.stderr
        assert_no_committed_output(output)

    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-tool-") as temp:
        root = Path(temp)
        project, build, tool = fixture(root)
        make_fake_esptool(tool, reject=True)
        output = root / "release"
        completed = run(command(project, build, output, tool), success=False)
        assert "esptool rejected" in completed.stderr
        assert_no_committed_output(output)


def test_wrong_build_environment_and_nonempty_output_rejected() -> None:
    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-path-") as temp:
        root = Path(temp)
        project, build, tool = fixture(root)
        wrong = build.parent / "some_other_environment"
        shutil.copytree(build, wrong)
        output = root / "release"
        completed = run(command(project, wrong, output, tool), success=False)
        assert "exact heltec_wifi_lora_32_V3_reflashable" in completed.stderr

        output.mkdir()
        (output / "owned.txt").write_text("preserve", encoding="ascii")
        completed = run(command(project, build, output, tool), success=False)
        assert "absent or empty" in completed.stderr
        assert (output / "owned.txt").read_text(encoding="ascii") == "preserve"


def test_legacy_production_entrypoints_withdrawn() -> None:
    for name in (
        "package_kitsu_production.py",
        "sign_kitsu_production_stage.py",
        "audit_kitsu_production_readiness.py",
    ):
        completed = run([sys.executable, str(TOOLS / name)], success=False)
        combined = completed.stdout + completed.stderr
        assert "WITHDRAWN_NOT_AUTHORIZED" in combined, (name, combined)
    guard = (TOOLS / "kitsu_production_guard.py").read_text(encoding="utf-8")
    assert "WITHDRAWN_NOT_AUTHORIZED" in guard
    assert "env.Exit(1)" in guard


def test_historical_stable_offset_imports_remain_defined() -> None:
    # The pinned 0.11.1 stable packager remains the public rollback path until
    # physical acceptance promotes 0.17.4. Its imports must stay loadable even
    # though the v2 candidate does not write OTA selection data.
    assert packager.OTA_DATA_OFFSET == 0x00E000
    assert packager.APP_OFFSET == packager.APP0_OFFSET == 0x010000


def test_runner_pins_candidate_and_reviewed_runtime() -> None:
    runner = (TOOLS / "package_kitsu_reflashable.cmd").read_text(encoding="utf-8")
    normalized = runner.replace("/", "\\").lower()
    assert 'if "%firmware_version%"=="" set "firmware_version=0.19.0"' in normalized
    assert "platformio-core-runtime\\scripts\\python.exe" in normalized
    assert (
        "private\\tooling\\platformio-core\\packages\\tool-esptoolpy"
        "\\esptool.py"
    ) in normalized
    assert (
        "framework-arduinoespressif32\\tools\\sdk\\esp32s3\\qio_qspi"
        "\\include\\sdkconfig.h"
    ) in normalized
    assert '--sdkconfig "%sdkconfig%"' in normalized
    assert "esptool411-runtime" not in normalized
    assert "%userprofile%\\.platformio" not in normalized
    if os.name == "nt":
        python = ROOT.parent / "platformio-core-runtime" / "Scripts" / "python.exe"
        tool = (
            ROOT.parent.parent
            / "private"
            / "tooling"
            / "platformio-core"
            / "packages"
            / "tool-esptoolpy"
            / "esptool.py"
        )
        completed = run([str(python), str(tool), "version"], success=True)
        assert "4.11.0" in completed.stdout


def main() -> None:
    test_success_exact_v2_contract()
    test_flash_record_mutations_rejected()
    test_serial_plan_mutations_rejected()
    test_version_relabel_and_image_marker_rejected()
    test_sdkconfig_rollback_and_security_guards()
    test_encrypted_image_rejected()
    test_signed_or_padded_images_rejected()
    test_encrypted_partition_rejected()
    test_active_secure_profiles_rejected()
    test_sensitive_input_and_tool_failure_rejected()
    test_wrong_build_environment_and_nonempty_output_rejected()
    test_legacy_production_entrypoints_withdrawn()
    test_historical_stable_offset_imports_remain_defined()
    test_runner_pins_candidate_and_reviewed_runtime()
    print("Kitsu local-only v2 candidate package tests passed (all contract/guard cases).")


if __name__ == "__main__":
    main()
