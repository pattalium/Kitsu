#!/usr/bin/env python3
"""Create a local-only, plaintext Kitsu serial-bootstrap candidate.

This is deliberately not a provisioning tool.  It accepts no device identity,
private key, XTS key, HMAC key, signing stage, or eFuse plan.  It validates the
active PlatformIO profile, the exact build-bound SDK configuration, the exact
unencrypted partition layout, and each ESP image before committing a seven-write
serial-flash bundle atomically.

The bootstrap preserves owner data while retiring the isolated legacy
connectivity region. Physical access can still replace firmware or read
plaintext flash. That tradeoff is intentional.
"""

from __future__ import annotations

import argparse
import configparser
import csv
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA = "kitsu.firmware-reflashable-release.v2"
ARTIFACT_STATUS = "release-candidate-owner-reflashable"
RELEASE_CHANNEL = "candidate"
ENVIRONMENT = "heltec_wifi_lora_32_V3_reflashable"
DEVICE_CLASS = "heltec-wifi-lora-32-v3-esp32s3-8mb"
PARTITION_LAYOUT = "partitions_kitsu_8MB.csv"
PROFILE_MARKER = "-DKITSU_SECURITY_MODE_REFLASHABLE=1"
PLATFORM_PACKAGE = "espressif32@6.13.0"

LOCAL_ONLY_EXCLUDED_SOURCES = (
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

BOOTLOADER_OFFSET = 0x000000
PARTITIONS_OFFSET = 0x008000
OTA_DATA_OFFSET = 0x00E000
APP0_OFFSET = 0x010000
# The authorization-gated historical stable packager still imports APP_OFFSET
# while its pinned 0.11.1 release remains available. The v2 candidate itself
# uses APP0_OFFSET and intentionally does not write OTA_DATA_OFFSET.
APP_OFFSET = APP0_OFFSET
APP_SLOT_BYTES = 0x330000
OTA_JOURNAL_BYTES = 0x001000
APP_IMAGE_BYTES_MAX = APP_SLOT_BYTES - OTA_JOURNAL_BYTES
APP0_JOURNAL_OFFSET = 0x33F000
APP1_OFFSET = 0x340000
APP1_JOURNAL_OFFSET = 0x66F000
LEGACY_CONNECTIVITY_OFFSET = 0x7B0000
LEGACY_CONNECTIVITY_BYTES = 0x040000
FLASH_BYTES = 0x800000
PARTITION_BINARY_BYTES = 0x0C00
PARTITION_ENTRY_BYTES = 32
PARTITION_MAGIC = b"\xaa\x50"
PARTITION_MD5_MAGIC = b"\xeb\xeb"

VERSION_PATTERN = re.compile(r"[0-9A-Za-z][0-9A-Za-z._+-]{0,63}\Z")
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}\Z")
IMAGE_INFO_HASH = re.compile(
    r"Validation\s+hash:\s*([0-9a-f]{64})\s*\(valid\)", re.IGNORECASE
)
IMAGE_INFO_TOOL_VERSION = re.compile(r"esptool\.py\s+v([0-9]+(?:\.[0-9]+){2})", re.IGNORECASE)
EXPECTED_ESPTOOL_VERSION = "4.11.0"
IMAGE_INFO_FLASH_SIZE = re.compile(r"Flash size:\s*([^\r\n]+)", re.IGNORECASE)
IMAGE_INFO_FLASH_FREQ = re.compile(r"Flash freq:\s*([^\r\n]+)", re.IGNORECASE)
IMAGE_INFO_FLASH_MODE = re.compile(r"Flash mode:\s*([^\r\n]+)", re.IGNORECASE)
SDKCONFIG_DEFINE = re.compile(
    r"^\s*#\s*define\s+(CONFIG_[A-Za-z0-9_]+)(?:\s+(.+?))?\s*$"
)

OTA_JOURNAL_CLEAR_SHA256 = (
    "f47a8ec3e9aff2318d896942282ad4fe37d6391c82914f54a5da8a37de1300c6"
)
LEGACY_CONNECTIVITY_CLEAR_SHA256 = (
    "3b874d3ba46c638fc3094f8e92fb744ca974893873f8885f54e23760f9b6311b"
)

REQUIRED_SDKCONFIG = {
    "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE": "1",
    "CONFIG_APP_ROLLBACK_ENABLE": "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE",
}
FORBIDDEN_SDKCONFIG = (
    "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK",
    "CONFIG_SECURE_BOOT",
    "CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES",
    "CONFIG_SECURE_BOOT_V1_ENABLED",
    "CONFIG_SECURE_BOOT_V2_ENABLED",
    "CONFIG_SECURE_FLASH_ENC_ENABLED",
    "CONFIG_FLASH_ENCRYPTION_ENABLED",
    "CONFIG_FLASH_ENCRYPTION_MODE_DEVELOPMENT",
    "CONFIG_FLASH_ENCRYPTION_MODE_RELEASE",
    "CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT",
    "CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT",
)

PARTITION_TYPES = {"app": 0x00, "data": 0x01}
APP_SUBTYPES = {
    "factory": 0x00,
    **{f"ota_{slot}": 0x10 + slot for slot in range(16)},
    "test": 0x20,
}
DATA_SUBTYPES = {
    "ota": 0x00,
    "phy": 0x01,
    "nvs": 0x02,
    "coredump": 0x03,
    "nvs_keys": 0x04,
    "efuse": 0x05,
    "undefined": 0x06,
    "esphttpd": 0x80,
    "fat": 0x81,
    "spiffs": 0x82,
    "littlefs": 0x83,
}
PARTITION_FLAGS = {"encrypted": 0x01, "readonly": 0x02}

EXPECTED_LAYOUT = (
    ("nvs", 0x01, 0x02, 0x009000, 0x005000, 0),
    ("otadata", 0x01, 0x00, 0x00E000, 0x002000, 0),
    ("app0", 0x00, 0x10, 0x010000, 0x330000, 0),
    ("app1", 0x00, 0x11, 0x340000, 0x330000, 0),
    ("spiffs", 0x01, 0x82, 0x670000, 0x140000, 0),
    ("kitsu_conn", 0x01, 0x40, 0x7B0000, 0x040000, 0),
    ("coredump", 0x01, 0x03, 0x7F0000, 0x010000, 0),
)

FORBIDDEN_PROFILE_TOKENS = (
    "KITSU_PRODUCTION_PROFILE",
    "KITSU_CONNECTIVITY_DEVELOPMENT",
    "SECURE_BOOT",
    "FLASH_ENCRYPT",
    "SECURE_FLASH_ENC",
    "NVS_ENCRYPT",
    "EFUSE",
    "HMAC_UP",
    "XTS_AES",
    "DISABLE_ROM_DL",
    "SECURE_ROM_DL",
)

SUSPICIOUS_BUILD_FILE = re.compile(
    r"(?:^|[-_.])(?:efuse|espefuse|xts|hmac|secure[-_]?boot|signing|signature|"
    r"encrypted|recovery[-_]?key|rotation[-_]?key)(?:[-_.]|$)",
    re.IGNORECASE,
)
SENSITIVE_SUFFIXES = {".key", ".p12", ".pfx", ".jks", ".keystore"}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def scons_content_signature(path: Path) -> str:
    """Return the MD5 content signature used by this pinned SCons build."""

    digest = hashlib.md5()  # nosec B324 - compatibility with the SCons file format
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def require_version(value: str) -> str:
    if not VERSION_PATTERN.fullmatch(value):
        raise SystemExit(
            "firmware version must be 1..64 safe ASCII characters "
            "(letters, digits, dot, underscore, plus, or hyphen)"
        )
    return value


def require_file(path: Path, description: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        raise SystemExit(f"{description} is missing: {resolved}")
    return resolved


def require_build_file(build_root: Path, name: str, description: str) -> Path:
    path = require_file(build_root / name, description)
    try:
        path.relative_to(build_root)
    except ValueError as error:
        raise SystemExit(f"{description} escapes the build directory") from error
    if path.is_symlink():
        raise SystemExit(f"{description} must not be a symbolic link")
    return path


def tool_command(tool: Path) -> list[str]:
    return [sys.executable, str(tool)] if tool.suffix.lower() == ".py" else [str(tool)]


def parse_platformio_profile(project_root: Path) -> dict[str, Any]:
    platformio = require_file(project_root / "platformio.ini", "PlatformIO profile")
    parser = configparser.RawConfigParser(interpolation=None, strict=True)
    try:
        with platformio.open("r", encoding="utf-8") as stream:
            parser.read_file(stream)
    except (configparser.Error, UnicodeDecodeError, OSError) as error:
        raise SystemExit("platformio.ini is not valid UTF-8 PlatformIO INI") from error

    default_envs = parser.get("platformio", "default_envs", fallback="")
    normalized_defaults = [
        value.strip()
        for line in default_envs.splitlines()
        for value in line.split(",")
        if value.strip()
    ]
    if normalized_defaults != [ENVIRONMENT]:
        raise SystemExit(
            f"the only default PlatformIO environment must be {ENVIRONMENT}"
        )

    active_risky = [
        section
        for section in parser.sections()
        if section.lower().startswith("env:")
        and any(token in section.lower() for token in ("production", "secure", "efuse"))
    ]
    if active_risky:
        raise SystemExit(
            "withdrawn secure/eFuse PlatformIO environments remain selectable: "
            + ", ".join(active_risky)
        )

    section = f"env:{ENVIRONMENT}"
    if not parser.has_section(section):
        raise SystemExit(f"PlatformIO environment is missing: {section}")
    platform = parser.get(section, "platform", fallback="").strip().lower()
    framework = parser.get(section, "framework", fallback="").strip().lower()
    board = parser.get(section, "board", fallback="").strip()
    partitions = parser.get(section, "board_build.partitions", fallback="").strip()
    upload_protocol = parser.get(section, "upload_protocol", fallback="").strip().lower()
    if platform != PLATFORM_PACKAGE:
        raise SystemExit(f"reflashable firmware must pin {PLATFORM_PACKAGE}")
    if framework != "arduino":
        raise SystemExit("reflashable firmware must be an Arduino-only build")
    if board != "heltec_wifi_lora_32_V3":
        raise SystemExit("reflashable firmware targets the wrong board")
    if partitions != PARTITION_LAYOUT:
        raise SystemExit("reflashable firmware must use the unencrypted 8 MiB layout")
    if upload_protocol != "esptool":
        raise SystemExit("reflashable firmware must retain the esptool serial uploader")
    if parser.has_option(section, "extra_scripts"):
        raise SystemExit("reflashable firmware may not run signing/encryption build scripts")

    source_filter = [
        line.strip()
        for line in parser.get(section, "build_src_filter", fallback="").splitlines()
        if line.strip() and not line.lstrip().startswith((";", "#"))
    ]
    expected_source_filter = ["+<*>"] + [
        f"-<{source}>" for source in LOCAL_ONLY_EXCLUDED_SOURCES
    ]
    if source_filter != expected_source_filter:
        raise SystemExit(
            "reflashable build must use the exact reviewed local-only source filter"
        )

    flags = [
        line.strip()
        for line in parser.get(section, "build_flags", fallback="").splitlines()
        if line.strip() and not line.lstrip().startswith((";", "#"))
    ]
    if flags.count(PROFILE_MARKER) != 1:
        raise SystemExit(f"reflashable build must contain exactly one {PROFILE_MARKER}")
    flattened = "\n".join(flags).upper()
    found_forbidden = [token for token in FORBIDDEN_PROFILE_TOKENS if token in flattened]
    if found_forbidden:
        raise SystemExit(
            "reflashable build contains a forbidden security/profile token: "
            + ", ".join(found_forbidden)
        )

    if (project_root / "sdkconfig.defaults").exists():
        raise SystemExit(
            "generic sdkconfig.defaults remains active-looking; quarantine it before release"
        )
    production_sdkconfigs = sorted(project_root.glob("sdkconfig*production*"))
    if production_sdkconfigs:
        raise SystemExit(
            "production security sdkconfig remains active-looking: "
            + ", ".join(path.name for path in production_sdkconfigs)
        )

    return {
        "environment": ENVIRONMENT,
        "platform": PLATFORM_PACKAGE,
        "framework": "arduino",
        "board": board,
        "partition_table": partitions,
        "security_mode": "reflashable",
        "compile_marker": "KITSU_SECURITY_MODE_REFLASHABLE=1",
        "upload_protocol": upload_protocol,
        "local_only_excluded_sources": list(LOCAL_ONLY_EXCLUDED_SOURCES),
        "platformio_sha256": sha256(platformio),
    }


def validate_build_sdkconfig(build_root: Path, sdkconfig: Path) -> dict[str, Any]:
    """Bind the reviewed rollback settings to the exact PlatformIO build."""

    resolved = require_file(sdkconfig, "build SDK configuration")
    normalized = resolved.as_posix().lower()
    expected_suffix = "/tools/sdk/esp32s3/qio_qspi/include/sdkconfig.h"
    if not normalized.endswith(expected_suffix):
        raise SystemExit(
            "build SDK configuration must be the ESP32-S3 qio_qspi generated sdkconfig.h"
        )

    signatures = sorted(build_root.glob(".sconsign*.dblite"))
    if len(signatures) != 1 or not signatures[0].is_file():
        raise SystemExit("exactly one PlatformIO build signature is required")
    signature = signatures[0]
    signature_bytes = signature.read_bytes().replace(b"\\", b"/").lower()
    if normalized.encode("utf-8") not in signature_bytes:
        raise SystemExit(
            "build signature does not bind the supplied SDK configuration to this build"
        )
    sdkconfig_csig = scons_content_signature(resolved)
    if sdkconfig_csig.encode("ascii") not in signature_bytes:
        raise SystemExit(
            "build signature does not bind the current SDK configuration contents"
        )

    try:
        text = resolved.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError) as error:
        raise SystemExit("build SDK configuration is not valid UTF-8") from error
    definitions: dict[str, str] = {}
    for line in text.splitlines():
        match = SDKCONFIG_DEFINE.fullmatch(line)
        if not match:
            continue
        name = match.group(1)
        value = (match.group(2) or "1").strip()
        if name in definitions:
            raise SystemExit(f"build SDK configuration defines {name} more than once")
        definitions[name] = value

    for name, expected in REQUIRED_SDKCONFIG.items():
        if definitions.get(name) != expected:
            raise SystemExit(
                f"build SDK configuration must define {name} as {expected}"
            )
    enabled_forbidden = [name for name in FORBIDDEN_SDKCONFIG if name in definitions]
    if enabled_forbidden:
        raise SystemExit(
            "build SDK configuration enables forbidden anti-rollback or hardware security: "
            + ", ".join(enabled_forbidden)
        )

    return {
        "source": "framework-arduinoespressif32/tools/sdk/esp32s3/qio_qspi/include/sdkconfig.h",
        "sha256": sha256(resolved),
        "platformio_content_csig": sdkconfig_csig,
        "platformio_build_signature": signature.name,
        "platformio_build_signature_sha256": sha256(signature),
        "bootloader_app_rollback": True,
        "bootloader_app_anti_rollback": False,
        "secure_boot": False,
        "flash_encryption": False,
    }


def validate_firmware_version(
    project_root: Path, application: Path, requested: str
) -> dict[str, Any]:
    """Reject a candidate whose requested label differs from source or image."""

    source = require_file(project_root / "src" / "main.cpp", "firmware version source")
    try:
        source_text = source.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError) as error:
        raise SystemExit("firmware version source is not valid UTF-8") from error
    matches = re.findall(
        r'^\s*constexpr\s+char\s+FIRMWARE_VERSION\[\]\s*=\s*"([^"]+)";\s*$',
        source_text,
        flags=re.MULTILINE,
    )
    if len(matches) != 1:
        raise SystemExit("firmware version source must define FIRMWARE_VERSION exactly once")
    source_version = require_version(matches[0])
    if requested != source_version:
        raise SystemExit(
            f"requested firmware version {requested} does not match source version {source_version}"
        )

    marker = requested.encode("ascii") + b"\x00"
    marker_count = application.read_bytes().count(marker)
    if marker_count != 1:
        raise SystemExit(
            "application image must contain the exact NUL-terminated firmware version once"
        )
    return {
        "version": requested,
        "source": "src/main.cpp",
        "source_sha256": sha256(source),
        "image_marker_verified": True,
    }


def parse_number(value: str) -> int:
    try:
        return int(value.strip(), 0)
    except ValueError as error:
        raise SystemExit(f"invalid numeric partition value: {value}") from error


def partition_code(value: str, names: dict[str, int], description: str) -> int:
    normalized = value.strip().lower()
    if normalized in names:
        return names[normalized]
    number = parse_number(normalized)
    if not 0 <= number <= 0xFF:
        raise SystemExit(f"{description} is outside one byte: {value}")
    return number


def expected_partition_records(layout: Path) -> list[tuple[str, int, int, int, int, int]]:
    records: list[tuple[str, int, int, int, int, int]] = []
    labels: set[str] = set()
    with layout.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.reader(
            line for line in stream if not line.lstrip().startswith("#")
        )
        for row in reader:
            if not row or not any(cell.strip() for cell in row):
                continue
            if len(row) < 5:
                raise SystemExit(f"invalid partition CSV row: {row}")
            label = row[0].strip()
            try:
                encoded_label = label.encode("ascii")
            except UnicodeEncodeError as error:
                raise SystemExit(f"partition label is not ASCII: {label}") from error
            if not label or len(encoded_label) > 15 or label in labels:
                raise SystemExit(f"invalid or duplicate partition label: {label}")
            labels.add(label)
            type_code = partition_code(row[1], PARTITION_TYPES, "partition type")
            subtype_names = APP_SUBTYPES if type_code == 0x00 else DATA_SUBTYPES
            subtype_code = partition_code(row[2], subtype_names, "partition subtype")
            flags = 0
            for cell in row[5:]:
                for flag in cell.split(":"):
                    name = flag.strip().lower()
                    if not name:
                        continue
                    if name not in PARTITION_FLAGS:
                        raise SystemExit(f"unknown partition flag: {name}")
                    flags |= PARTITION_FLAGS[name]
            records.append(
                (
                    label,
                    type_code,
                    subtype_code,
                    parse_number(row[3]),
                    parse_number(row[4]),
                    flags,
                )
            )
    if tuple(records) != EXPECTED_LAYOUT:
        raise SystemExit(
            "reviewed reflashable partition CSV differs from the exact unencrypted layout"
        )
    return records


def validate_partition_table(binary: Path, layout: Path) -> dict[str, Any]:
    expected = expected_partition_records(layout)
    data = binary.read_bytes()
    if len(data) != PARTITION_BINARY_BYTES:
        raise SystemExit("partitions.bin must be exactly 0xC00 bytes")
    actual: list[tuple[str, int, int, int, int, int]] = []
    cursor = 0
    embedded_md5 = ""
    while cursor + PARTITION_ENTRY_BYTES <= len(data):
        entry = data[cursor : cursor + PARTITION_ENTRY_BYTES]
        if entry == b"\xff" * PARTITION_ENTRY_BYTES:
            break
        if entry[:2] == PARTITION_MD5_MAGIC:
            if entry[2:16] != b"\xff" * 14:
                raise SystemExit("partition table has a malformed MD5 marker")
            expected_md5 = hashlib.md5(data[:cursor]).digest()  # nosec B324
            if entry[16:] != expected_md5:
                raise SystemExit("partition table MD5 does not verify")
            embedded_md5 = expected_md5.hex()
            cursor += PARTITION_ENTRY_BYTES
            if any(byte != 0xFF for byte in data[cursor:]):
                raise SystemExit("partition table contains data after its MD5 record")
            break
        if entry[:2] != PARTITION_MAGIC:
            raise SystemExit(f"invalid partition magic at 0x{cursor:X}")
        _, type_code, subtype, offset, size, label_raw, flags = struct.unpack(
            "<2sBBII16sI", entry
        )
        if b"\x00" not in label_raw:
            raise SystemExit("partition label is not NUL-terminated")
        label_bytes, padding = label_raw.split(b"\x00", 1)
        if any(padding):
            raise SystemExit("partition label padding is nonzero")
        try:
            label = label_bytes.decode("ascii")
        except UnicodeDecodeError as error:
            raise SystemExit("partition label is not ASCII") from error
        if flags:
            raise SystemExit(
                f"reflashable partition {label} has forbidden flags 0x{flags:X}"
            )
        actual.append((label, type_code, subtype, offset, size, flags))
        cursor += PARTITION_ENTRY_BYTES
    if not embedded_md5:
        raise SystemExit("partition table is missing its MD5 record")
    if actual != expected:
        raise SystemExit("compiled partition table differs from the reviewed CSV")

    previous_end = 0
    for label, _, _, offset, size, _ in actual:
        if size <= 0 or offset < previous_end or offset + size > FLASH_BYTES:
            raise SystemExit(f"partition {label} overlaps or exceeds the 8 MiB flash")
        previous_end = offset + size
    return {
        "file": f"metadata/{layout.name}",
        "sha256": sha256(layout),
        "compiled_sha256": sha256(binary),
        "verified_against_reviewed_csv": True,
        "binary_bytes": len(data),
        "entry_count": len(actual),
        "embedded_md5": embedded_md5,
        "encrypted_partition_flags": False,
    }


def parse_plain_esp_image(path: Path, role: str) -> dict[str, Any]:
    data = path.read_bytes()
    if len(data) < 24 or data[0] != 0xE9:
        raise SystemExit(f"{role} is not a plaintext ESP image")
    segment_count = data[1]
    if not 1 <= segment_count <= 16:
        raise SystemExit(f"{role} has an invalid ESP segment count")
    cursor = 24
    checksum = 0xEF
    segments: list[dict[str, int]] = []
    for index in range(segment_count):
        if cursor + 8 > len(data):
            raise SystemExit(f"{role} has a truncated segment header")
        load_address, size = struct.unpack_from("<II", data, cursor)
        cursor += 8
        if size <= 0 or cursor + size > len(data):
            raise SystemExit(f"{role} has a truncated or empty segment")
        if load_address == 0:
            raise SystemExit(
                f"{role} contains a secure-padding segment and is not reflashable output"
            )
        segment = data[cursor : cursor + size]
        for byte in segment:
            checksum ^= byte
        segments.append(
            {"index": index, "load_address": load_address, "bytes": size}
        )
        cursor += size

    checksum_offset = ((cursor + 16) // 16) * 16 - 1
    if checksum_offset >= len(data):
        raise SystemExit(f"{role} is missing its ESP checksum")
    if any(data[cursor:checksum_offset]):
        raise SystemExit(f"{role} has nonzero pre-checksum padding")
    if data[checksum_offset] != checksum:
        raise SystemExit(f"{role} ESP checksum does not verify")
    hash_start = checksum_offset + 1
    hash_appended = data[23] == 1
    if not hash_appended:
        raise SystemExit(f"{role} lacks the normal appended validation hash")
    logical_end = hash_start + 32
    if logical_end > len(data):
        raise SystemExit(f"{role} has a truncated validation hash")
    calculated_hash = hashlib.sha256(data[:hash_start]).digest()
    if data[hash_start:logical_end] != calculated_hash:
        raise SystemExit(f"{role} validation hash does not verify")
    if len(data) != logical_end:
        raise SystemExit(
            f"{role} contains trailing secure-boot padding/signature data"
        )
    return {
        "segments": len(segments),
        "validation_sha256": calculated_hash.hex(),
        "exact_plaintext_image_length": True,
    }


def validate_with_esptool(tool: Path, image: Path, role: str) -> dict[str, str]:
    completed = subprocess.run(
        tool_command(tool) + ["image_info", "--version", "2", str(image)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=30,
    )
    output = completed.stdout + completed.stderr
    if completed.returncode != 0:
        raise SystemExit(f"esptool rejected the {role}; no release was committed")
    required = (
        "Detected image type: ESP32-S3",
        "Chip ID: 9 (ESP32-S3)",
    )
    if any(value not in output for value in required):
        raise SystemExit(f"esptool did not identify {role} as an ESP32-S3 image")
    if not re.search(r"Checksum:\s*0x?[0-9a-f]+\s*\(valid\)", output, re.IGNORECASE):
        raise SystemExit(f"esptool did not verify the {role} checksum")
    validation = IMAGE_INFO_HASH.search(output)
    tool_version = IMAGE_INFO_TOOL_VERSION.search(output)
    size = IMAGE_INFO_FLASH_SIZE.search(output)
    frequency = IMAGE_INFO_FLASH_FREQ.search(output)
    mode = IMAGE_INFO_FLASH_MODE.search(output)
    if not validation or not tool_version or not size or not frequency or not mode:
        raise SystemExit(f"esptool returned incomplete {role} image metadata")
    if tool_version.group(1) != EXPECTED_ESPTOOL_VERSION:
        raise SystemExit(
            f"release validation requires esptool {EXPECTED_ESPTOOL_VERSION}; "
            f"received {tool_version.group(1)}"
        )
    settings = {
        "esptool_version": tool_version.group(1),
        "flash_size": size.group(1).strip(),
        "flash_frequency": frequency.group(1).strip().lower(),
        "flash_mode": mode.group(1).strip().lower(),
        "validation_sha256": validation.group(1).lower(),
    }
    if settings["flash_size"].upper() != "8MB":
        raise SystemExit(f"{role} is not configured for 8 MiB flash")
    if settings["flash_frequency"] != "80m" or settings["flash_mode"] != "dio":
        raise SystemExit(f"{role} has unexpected flash mode/frequency metadata")
    return settings


def reject_sensitive_build_inputs(build_root: Path) -> None:
    for path in build_root.iterdir():
        if not path.is_file():
            continue
        name = path.name.lower()
        if path.suffix.lower() in SENSITIVE_SUFFIXES or SUSPICIOUS_BUILD_FILE.search(name):
            raise SystemExit(
                f"reflashable build directory contains a forbidden sensitive artifact: {path.name}"
            )


def artifact_record(
    path: Path,
    *,
    role: str,
    partition: str,
    offset: int,
    esp_validation: dict[str, Any] | None = None,
) -> dict[str, Any]:
    return {
        "file": f"images/{path.name}",
        "role": role,
        "partition": partition,
        "offset": offset,
        "offset_hex": f"0x{offset:06X}",
        "bytes": path.stat().st_size,
        "sha256": sha256(path),
        "secure_boot_signed": False,
        "encrypted": False,
        "esp_image_verified": esp_validation is not None,
        **({"esp_validation": esp_validation} if esp_validation else {}),
    }


def validate_flash_records(records: list[dict[str, Any]]) -> None:
    if len(records) != 7:
        raise SystemExit("candidate must contain exactly seven ordered flash records")

    expected = (
        ("bootloader", "bootloader", BOOTLOADER_OFFSET, "images/0x000000-bootloader.bin"),
        (
            "partition_table",
            "partition_table",
            PARTITIONS_OFFSET,
            "images/0x008000-partitions.bin",
        ),
        ("application_app0", "app0", APP0_OFFSET, "images/kitsu868-app.bin"),
        (
            "ota_journal_app0_clear",
            "app0_journal",
            APP0_JOURNAL_OFFSET,
            "images/kitsu868-ota-journal-clear.bin",
        ),
        ("application_app1", "app1", APP1_OFFSET, "images/kitsu868-app.bin"),
        (
            "ota_journal_app1_clear",
            "app1_journal",
            APP1_JOURNAL_OFFSET,
            "images/kitsu868-ota-journal-clear.bin",
        ),
        (
            "legacy_connectivity_clear",
            "retired_legacy_connectivity",
            LEGACY_CONNECTIVITY_OFFSET,
            "images/kitsu868-legacy-connectivity-clear.bin",
        ),
    )
    image_roles = {"bootloader", "application_app0", "application_app1"}
    base_keys = {
        "file",
        "role",
        "partition",
        "offset",
        "offset_hex",
        "bytes",
        "sha256",
        "secure_boot_signed",
        "encrypted",
        "esp_image_verified",
    }
    for record, (role, partition, offset, file) in zip(records, expected, strict=True):
        expected_keys = base_keys | ({"esp_validation"} if role in image_roles else set())
        if set(record) != expected_keys:
            raise SystemExit(f"{role} flash record fields differ from the reviewed contract")
        if (
            record["role"] != role
            or record["partition"] != partition
            or record["offset"] != offset
            or record["offset_hex"] != f"0x{offset:06X}"
            or record["file"] != file
        ):
            raise SystemExit(f"{role} flash record differs from the reviewed contract")
        if record["secure_boot_signed"] is not False or record["encrypted"] is not False:
            raise SystemExit(f"{role} must remain a plaintext unsigned flash record")
        if record["esp_image_verified"] is not (role in image_roles):
            raise SystemExit(f"{role} has an invalid ESP-image verification marker")
        if not isinstance(record["bytes"], int) or record["bytes"] <= 0:
            raise SystemExit(f"{role} has an invalid byte length")
        if not SHA256_PATTERN.fullmatch(record["sha256"]):
            raise SystemExit(f"{role} has an invalid SHA-256")

    bootloader, partitions, app0, journal0, app1, journal1, legacy = records
    if bootloader["bytes"] >= PARTITIONS_OFFSET:
        raise SystemExit("bootloader overlaps the partition table")
    if partitions["bytes"] != PARTITION_BINARY_BYTES:
        raise SystemExit("partition table length differs from the reviewed contract")
    if not 0 < app0["bytes"] <= APP_IMAGE_BYTES_MAX:
        raise SystemExit("application exceeds the image area before its private journal")
    if (
        app1["bytes"] != app0["bytes"]
        or app1["sha256"] != app0["sha256"]
        or app1["file"] != app0["file"]
        or app1["esp_validation"] != app0["esp_validation"]
    ):
        raise SystemExit("app1 must use the exact validated app0 application bytes")
    if (
        journal0["bytes"] != OTA_JOURNAL_BYTES
        or journal1["bytes"] != OTA_JOURNAL_BYTES
        or journal0["sha256"] != OTA_JOURNAL_CLEAR_SHA256
        or journal1["sha256"] != OTA_JOURNAL_CLEAR_SHA256
        or journal1["file"] != journal0["file"]
    ):
        raise SystemExit("both OTA journals must use the exact reviewed erased sector")
    if (
        legacy["bytes"] != LEGACY_CONNECTIVITY_BYTES
        or legacy["sha256"] != LEGACY_CONNECTIVITY_CLEAR_SHA256
    ):
        raise SystemExit(
            "legacy connectivity retirement must clear the exact isolated partition"
        )

    prior_end = 0
    for record in records:
        end = record["offset"] + record["bytes"]
        if record["offset"] < prior_end or end > FLASH_BYTES:
            raise SystemExit("candidate flash records overlap or exceed the 8 MiB flash")
        prior_end = end


def readback_plan(records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    plan: list[dict[str, Any]] = []
    for index, record in enumerate(records):
        output = f"readback-{index + 1:02d}-{record['role']}.bin"
        command = [
            "python",
            "-m",
            "esptool",
            "--chip",
            "esp32s3",
            "--before",
            "default_reset",
            "--after",
            "hard_reset",
            "read_flash",
            "--flash_size",
            "8MB",
            record["offset_hex"],
            f"0x{record['bytes']:X}",
            output,
        ]
        plan.append(
            {
                "role": record["role"],
                "offset": record["offset"],
                "bytes": record["bytes"],
                "expected_sha256": record["sha256"],
                "output": output,
                "command": command,
            }
        )
    return plan


def write_checksum_index(root: Path) -> None:
    files = sorted(
        path
        for path in root.rglob("*")
        if path.is_file() and path.name != "SHA256SUMS.txt"
    )
    lines = [f"{sha256(path)}  {path.relative_to(root).as_posix()}" for path in files]
    (root / "SHA256SUMS.txt").write_text("\n".join(lines) + "\n", encoding="ascii")


def flash_command(records: list[dict[str, Any]]) -> list[str]:
    command = [
        "python",
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "--before",
        "default_reset",
        "--after",
        "hard_reset",
        "write_flash",
        "--flash_mode",
        "dio",
        "--flash_freq",
        "80m",
        "--flash_size",
        "8MB",
        "--verify",
    ]
    for record in records:
        command.extend((record["offset_hex"], record["file"]))
    return command


def validate_serial_plan(
    records: list[dict[str, Any]],
    command: list[str],
    readbacks: list[dict[str, Any]],
) -> None:
    expected_command = [
        "python",
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "--before",
        "default_reset",
        "--after",
        "hard_reset",
        "write_flash",
        "--flash_mode",
        "dio",
        "--flash_freq",
        "80m",
        "--flash_size",
        "8MB",
        "--verify",
    ]
    for record in records:
        expected_command.extend((record["offset_hex"], record["file"]))
    if command != expected_command:
        raise SystemExit("serial write command differs from the exact seven-write plan")

    if len(readbacks) != len(records):
        raise SystemExit("serial candidate requires one readback for every flash record")
    keys = {"role", "offset", "bytes", "expected_sha256", "output", "command"}
    for index, (record, readback) in enumerate(
        zip(records, readbacks, strict=True), start=1
    ):
        output = f"readback-{index:02d}-{record['role']}.bin"
        expected = {
            "role": record["role"],
            "offset": record["offset"],
            "bytes": record["bytes"],
            "expected_sha256": record["sha256"],
            "output": output,
            "command": [
                "python",
                "-m",
                "esptool",
                "--chip",
                "esp32s3",
                "--before",
                "default_reset",
                "--after",
                "hard_reset",
                "read_flash",
                "--flash_size",
                "8MB",
                record["offset_hex"],
                f"0x{record['bytes']:X}",
                output,
            ],
        }
        if set(readback) != keys or readback != expected:
            raise SystemExit(f"{record['role']} readback differs from the exact plan")

    serialized = " ".join(command + [part for item in readbacks for part in item["command"]])
    if "erase_flash" in serialized.lower():
        raise SystemExit("serial candidate may not contain a whole-chip erase operation")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--esptool", type=Path, required=True)
    parser.add_argument("--sdkconfig", type=Path, required=True)
    parser.add_argument("--firmware-version", required=True)
    args = parser.parse_args()

    firmware_version = require_version(args.firmware_version)
    project_root = args.project_root.expanduser().resolve()
    if not project_root.is_dir():
        raise SystemExit(f"project root is missing: {project_root}")
    build_root = args.build_dir.expanduser().resolve()
    if not build_root.is_dir() or build_root.name != ENVIRONMENT:
        raise SystemExit(
            f"build directory must be the exact {ENVIRONMENT} environment output"
        )
    output_dir = args.output_dir.expanduser().resolve()
    if output_dir.exists() and not output_dir.is_dir():
        raise SystemExit("output path exists and is not a directory")
    if output_dir.exists() and any(output_dir.iterdir()):
        raise SystemExit("output directory must be absent or empty")
    if output_dir.is_symlink():
        raise SystemExit("output directory must not be a symbolic link")
    esptool = require_file(args.esptool, "esptool validator")

    profile = parse_platformio_profile(project_root)
    sdkconfig = require_file(args.sdkconfig, "build SDK configuration")
    rollback_configuration = validate_build_sdkconfig(build_root, sdkconfig)
    reject_sensitive_build_inputs(build_root)
    layout = require_file(project_root / PARTITION_LAYOUT, "partition layout")
    candidates = {
        "bootloader": require_build_file(build_root, "bootloader.bin", "bootloader"),
        "partitions": require_build_file(build_root, "partitions.bin", "partition table"),
        "application": require_build_file(build_root, "firmware.bin", "application"),
    }
    source_hashes = {
        role: sha256(path) for role, path in {**candidates, "layout": layout}.items()
    }
    if not 0 < candidates["bootloader"].stat().st_size < PARTITIONS_OFFSET:
        raise SystemExit("bootloader is empty or overlaps the partition table")
    if not 0 < candidates["application"].stat().st_size <= APP_IMAGE_BYTES_MAX:
        raise SystemExit("application is empty or overlaps the private OTA journal")

    partition_validation = validate_partition_table(candidates["partitions"], layout)
    boot_internal = parse_plain_esp_image(candidates["bootloader"], "bootloader")
    app_internal = parse_plain_esp_image(candidates["application"], "application")
    firmware_identity = validate_firmware_version(
        project_root, candidates["application"], firmware_version
    )
    boot_external = validate_with_esptool(esptool, candidates["bootloader"], "bootloader")
    app_external = validate_with_esptool(esptool, candidates["application"], "application")
    if boot_internal["validation_sha256"] != boot_external["validation_sha256"]:
        raise SystemExit("bootloader validation hash differs between independent checks")
    if app_internal["validation_sha256"] != app_external["validation_sha256"]:
        raise SystemExit("application validation hash differs between independent checks")
    flash_settings = {
        key: boot_external[key]
        for key in ("flash_size", "flash_frequency", "flash_mode")
    }
    if any(app_external[key] != value for key, value in flash_settings.items()):
        raise SystemExit("bootloader and application flash metadata differ")
    if profile["platformio_sha256"] != sha256(project_root / "platformio.ini"):
        raise SystemExit("platformio.ini changed during release validation")
    if any(sha256(path) != source_hashes[role] for role, path in {**candidates, "layout": layout}.items()):
        raise SystemExit("a release input changed during validation")
    if sha256(sdkconfig) != rollback_configuration["sha256"]:
        raise SystemExit("build SDK configuration changed during release validation")
    build_signature = build_root / rollback_configuration["platformio_build_signature"]
    if sha256(build_signature) != rollback_configuration["platformio_build_signature_sha256"]:
        raise SystemExit("PlatformIO build signature changed during release validation")
    if sha256(project_root / firmware_identity["source"]) != firmware_identity["source_sha256"]:
        raise SystemExit("firmware version source changed during release validation")

    output_dir.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(
        tempfile.mkdtemp(prefix=f".{output_dir.name}.staging-", dir=output_dir.parent)
    )
    committed = False
    try:
        images = staging / "images"
        metadata = staging / "metadata"
        images.mkdir()
        metadata.mkdir()

        names = {
            "bootloader": "0x000000-bootloader.bin",
            "partitions": "0x008000-partitions.bin",
            "application": "kitsu868-app.bin",
            "journal": "kitsu868-ota-journal-clear.bin",
            "legacy": "kitsu868-legacy-connectivity-clear.bin",
        }
        copied: dict[str, Path] = {}
        for role in ("bootloader", "partitions", "application"):
            destination = images / names[role]
            shutil.copyfile(candidates[role], destination)
            copied[role] = destination
        copied["journal"] = images / names["journal"]
        copied["journal"].write_bytes(b"\xff" * OTA_JOURNAL_BYTES)
        copied["legacy"] = images / names["legacy"]
        copied["legacy"].write_bytes(b"\xff" * LEGACY_CONNECTIVITY_BYTES)
        shutil.copyfile(layout, metadata / layout.name)
        if any(
            sha256(copied[role]) != source_hashes[role]
            for role in ("bootloader", "partitions", "application")
        ):
            raise SystemExit("a firmware image changed while being staged")
        if sha256(copied["journal"]) != OTA_JOURNAL_CLEAR_SHA256:
            raise SystemExit("staged OTA journal clear image differs from the reviewed bytes")
        if sha256(copied["legacy"]) != LEGACY_CONNECTIVITY_CLEAR_SHA256:
            raise SystemExit(
                "staged legacy connectivity clear image differs from the reviewed bytes"
            )
        if sha256(metadata / layout.name) != source_hashes["layout"]:
            raise SystemExit("the partition layout changed while being staged")

        records = [
            artifact_record(
                copied["bootloader"],
                role="bootloader",
                partition="bootloader",
                offset=BOOTLOADER_OFFSET,
                esp_validation={**boot_internal, **boot_external},
            ),
            artifact_record(
                copied["partitions"],
                role="partition_table",
                partition="partition_table",
                offset=PARTITIONS_OFFSET,
            ),
            artifact_record(
                copied["application"],
                role="application_app0",
                partition="app0",
                offset=APP0_OFFSET,
                esp_validation={**app_internal, **app_external},
            ),
            artifact_record(
                copied["journal"],
                role="ota_journal_app0_clear",
                partition="app0_journal",
                offset=APP0_JOURNAL_OFFSET,
            ),
            artifact_record(
                copied["application"],
                role="application_app1",
                partition="app1",
                offset=APP1_OFFSET,
                esp_validation={**app_internal, **app_external},
            ),
            artifact_record(
                copied["journal"],
                role="ota_journal_app1_clear",
                partition="app1_journal",
                offset=APP1_JOURNAL_OFFSET,
            ),
            artifact_record(
                copied["legacy"],
                role="legacy_connectivity_clear",
                partition="retired_legacy_connectivity",
                offset=LEGACY_CONNECTIVITY_OFFSET,
            ),
        ]
        validate_flash_records(records)
        command = flash_command(records)
        readbacks = readback_plan(records)
        validate_serial_plan(records, command, readbacks)
        command_text = " ".join(command)
        forbidden_command_tokens = ("espefuse", "burn_", "--encrypt", "sign_data")
        if any(token in command_text.lower() for token in forbidden_command_tokens):
            raise SystemExit("generated serial command contains a forbidden operation")

        readback_text = "\n\n".join(
            " ".join(item["command"])
            + f"\nExpected SHA-256: {item['expected_sha256']}"
            for item in readbacks
        )
        flashing = staging / "FLASHING.txt"
        flashing.write_text(
            "Kitsu local-only serial-bootstrap candidate\n"
            "===========================================\n\n"
            "This candidate contains plaintext, unsigned ESP32-S3 images. It performs "
            "exactly seven bounded writes, no whole-chip erase, and no hardware-fuse "
            "operation. The final write intentionally clears only the isolated retired "
            "legacy-connectivity partition.\n\n"
            "Install esptool 4.11.0, then run this write command from the bundle root:\n\n"
            + command_text
            + "\n\nThe write command's --verify check is mandatory. Then run every "
            "readback command below and compute each output file's SHA-256. Every digest "
            "must exactly match its expected value before accepting the bootstrap.\n\n"
            + readback_text
            + "\n\nDo not add erase_flash. The bounded plan preserves OTA selection data, "
            "companion state, companion packs, controller records, MeshCore state, "
            "coredump, and eFuses. On boot, firmware separately retires only the "
            "kitsu_lan_act NVS namespace. Physical access can replace firmware and "
            "inspect plaintext flash by design.\n",
            encoding="utf-8",
        )

        manifest = {
            "schema": SCHEMA,
            "created_at": datetime.now(timezone.utc)
            .isoformat()
            .replace("+00:00", "Z"),
            "artifact_status": ARTIFACT_STATUS,
            "firmware_version": firmware_version,
            "release_channel": RELEASE_CHANNEL,
            "device_class": DEVICE_CLASS,
            "checksum_index": "SHA256SUMS.txt",
            "build_profile": profile,
            "firmware_identity": firmware_identity,
            "rollback_configuration": rollback_configuration,
            "partition_layout": partition_validation,
            "security_profile": {
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
            },
            "product_contract": {
                "mode": "local_only",
                "owner_control": "authenticated_bluetooth",
                "firmware_update": "authenticated_bluetooth",
                "account_required": False,
                "remote_service_required": False,
            },
            "operations": {
                "erase_flash": False,
                "write_count": 7,
                "retire_legacy_connectivity": True,
                "runtime_retires_nvs_namespace": "kitsu_lan_act",
            },
            "preserves": {
                "ota_data": True,
                "nvs_except_kitsu_lan_act": True,
                "companion_pack": True,
                "controller_store": True,
                "meshcore_state": True,
                "coredump": True,
                "efuses": True,
            },
            "release_requirements": {
                "device_specific_secrets": False,
                "hardware_keys": False,
                "secure_boot_signing_key": False,
                "signing_stage": False,
                "efuse_operations": False,
            },
            "flash_artifacts": records,
            "serial_flash": {
                "tool": "esptool",
                "validated_with": f"esptool.py v{boot_external['esptool_version']}",
                "chip": "esp32s3",
                "before": "default_reset",
                "after": "hard_reset",
                **flash_settings,
                "write_count": 7,
                "write_verify": True,
                "readback_verify": True,
                "readback_algorithm": "sha256",
                "readback_plan": readbacks,
                "erase_required": False,
                "erase_flash": False,
                "command_file": flashing.name,
                "command": command,
            },
            "warnings": [
                {
                    "code": "PHYSICAL_ACCESS_CAN_REPLACE_FIRMWARE",
                    "severity": "high",
                    "message": "Physical access can replace firmware and inspect plaintext flash.",
                },
                {
                    "code": "NO_VERIFIED_BOOT_CHAIN",
                    "severity": "high",
                    "message": "The device intentionally does not enforce a signed boot chain.",
                },
                {
                    "code": "PLAINTEXT_FLASH_NOT_HARDWARE_PROTECTED",
                    "severity": "high",
                    "message": "Firmware and retained flash data are not protected by hardware flash encryption.",
                },
                {
                    "code": "SERIAL_RECOVERY_INTENTIONALLY_PRESERVED",
                    "severity": "info",
                    "message": "Ordinary serial erase and reflash recovery remains available.",
                },
                {
                    "code": "LEGACY_CONNECTIVITY_REGION_RETIRED",
                    "severity": "info",
                    "message": "The final bounded write intentionally clears the isolated retired connectivity region.",
                },
            ],
        }
        manifest_path = staging / "manifest.json"
        manifest_path.write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )
        write_checksum_index(staging)

        if output_dir.exists():
            output_dir.rmdir()
        os.replace(staging, output_dir)
        committed = True
    finally:
        if not committed:
            shutil.rmtree(staging, ignore_errors=True)

    print(f"Kitsu local-only serial candidate created: {output_dir / 'manifest.json'}")


if __name__ == "__main__":
    main()
