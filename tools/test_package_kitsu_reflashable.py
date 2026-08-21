#!/usr/bin/env python3
"""Fail-closed tests for the generic Kitsu reflashable release packager."""

from __future__ import annotations

import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
import os


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
    "partition_layout",
    "security_profile",
    "network_security",
    "release_requirements",
    "flash_artifacts",
    "serial_flash",
    "warnings",
}


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


def make_esp_image(path: Path, segments: list[tuple[int, bytes]]) -> None:
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = len(segments)
    header[2] = 2  # DIO
    header[3] = 0x4F  # 8 MiB, 80 MHz (metadata is independently checked by esptool)
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
build_flags =
    -DKITSU_SECURITY_MODE_REFLASHABLE=1
    -DCORE_DEBUG_LEVEL=0
"""
        + extra_section,
        encoding="utf-8",
    )
    shutil.copyfile(ROOT / packager.PARTITION_LAYOUT, project / packager.PARTITION_LAYOUT)


def make_fake_esptool(path: Path, *, reject: bool = False) -> None:
    path.write_text(
        """#!/usr/bin/env python3
import hashlib
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
        [(0x3C000020, b"A" * 257), (0x40374000, b"C" * 191)],
    )
    tool = root / "fake_esptool.py"
    make_fake_esptool(tool)
    return project, build, tool


def command(project: Path, build: Path, output: Path, tool: Path) -> list[str]:
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
        "--firmware-version",
        "0.10.0",
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


def test_success() -> None:
    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-test-") as temp:
        root = Path(temp)
        project, build, tool = fixture(root)
        output = root / "release"
        run(command(project, build, output, tool), success=True)

        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        assert set(manifest) == EXPECTED_TOP_LEVEL
        assert manifest["schema"] == packager.SCHEMA
        assert manifest["artifact_status"] == packager.ARTIFACT_STATUS
        assert manifest["release_channel"] == packager.RELEASE_CHANNEL
        assert "device_id" not in manifest
        assert manifest["build_profile"]["environment"] == packager.ENVIRONMENT
        assert manifest["build_profile"]["framework"] == "arduino"

        security = manifest["security_profile"]
        assert security == {
            "mode": "reflashable",
            "secure_boot": False,
            "flash_encryption": False,
            "nvs_encryption": False,
            "hardware_root_protected": False,
            "firmware_images_encrypted": False,
            "application_layer_encryption": True,
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
        assert all(value is False for value in manifest["release_requirements"].values())
        assert manifest["partition_layout"]["encrypted_partition_flags"] is False
        assert [entry["offset"] for entry in manifest["flash_artifacts"]] == [
            0x000000,
            0x008000,
            0x00E000,
            0x010000,
        ]
        assert all(not entry["secure_boot_signed"] for entry in manifest["flash_artifacts"])
        assert all(not entry["encrypted"] for entry in manifest["flash_artifacts"])
        assert (output / "images" / "0x00E000-ota-data-initial.bin").read_bytes() == (
            b"\xff" * 0x2000
        )
        command_text = " ".join(manifest["serial_flash"]["command"]).lower()
        assert "write_flash" in command_text and "--verify" in command_text
        assert "espefuse" not in command_text
        assert "burn_" not in command_text
        assert "--encrypt" not in command_text
        assert manifest["serial_flash"]["erase_required"] is False
        assert manifest["serial_flash"]["validated_with"] == "esptool.py v4.11.0"
        warning_codes = {warning["code"] for warning in manifest["warnings"]}
        assert warning_codes == {
            "PHYSICAL_ACCESS_CAN_REPLACE_FIRMWARE",
            "NO_VERIFIED_BOOT_CHAIN",
            "APPLICATION_ENCRYPTION_NOT_HARDWARE_ROOTED",
            "SERIAL_RECOVERY_INTENTIONALLY_PRESERVED",
            "NETWORK_AUTH_RETAINED",
        }
        flashing = (output / "FLASHING.txt").read_text(encoding="utf-8")
        assert "stock MeshCore firmware remain available" in flashing
        verify_checksum_index(output)


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
        make_esp_image(build / "firmware.bin", [(0x3C000020, b"A" * 64), (0, b"P" * 64)])
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

    with tempfile.TemporaryDirectory(prefix="kitsu-reflashable-sdkconfig-") as temp:
        root = Path(temp)
        project, build, tool = fixture(root)
        (project / "sdkconfig.defaults").write_text("CONFIG_SECURE_BOOT=y\n", encoding="ascii")
        output = root / "release"
        completed = run(command(project, build, output, tool), success=False)
        assert "sdkconfig.defaults remains active-looking" in completed.stderr
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
    assert 'env.Exit(1)' in guard


def test_runner_pins_reviewed_esptool_runtime() -> None:
    runner = (TOOLS / "package_kitsu_reflashable.cmd").read_text(encoding="utf-8")
    normalized = runner.replace("/", "\\").lower()
    assert 'if "%firmware_version%"=="" set "firmware_version=0.10.2"' in normalized
    assert "esptool411-runtime\\scripts\\python.exe" in normalized
    assert "esptool411-runtime\\scripts\\esptool.exe" in normalized
    assert "esptool-py310" not in normalized
    assert "esptool411-bootstrap" not in normalized
    if os.name == "nt":
        tool = ROOT.parent / "esptool411-runtime" / "Scripts" / "esptool.exe"
        completed = run([str(tool), "version"], success=True)
        assert "4.11.0" in completed.stdout


def main() -> None:
    test_success()
    test_encrypted_image_rejected()
    test_signed_or_padded_images_rejected()
    test_encrypted_partition_rejected()
    test_active_secure_profiles_rejected()
    test_sensitive_input_and_tool_failure_rejected()
    test_wrong_build_environment_and_nonempty_output_rejected()
    test_legacy_production_entrypoints_withdrawn()
    test_runner_pins_reviewed_esptool_runtime()
    print("Kitsu reflashable package tests passed (13 positive/negative guard scenarios).")


if __name__ == "__main__":
    main()
