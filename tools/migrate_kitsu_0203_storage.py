#!/usr/bin/env python3
"""Audit or execute Kitsu's one-time 0.20.3 table-last migration.

This workflow deliberately does not share PlatformIO's ordinary upload path.
It preserves the legacy 20 KiB NVS prefix and every byte at 0x670000..0x7fffff,
creates two exact 3 MiB OTA slots, and commits the new partition table only
after every destructive range has been read back and verified.  A failure after
the NVS-extension erase intentionally leaves the device in the ROM loader;
the independently verified full-flash backup is the recovery authority.

No command in this tool performs a chip erase.  ``audit`` is entirely offline.
``migrate`` and ``restore`` require explicit confirmation strings and never
reset the board on a failed gate.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

try:
    from kitsu_firmware_identity import parse_identity
except ModuleNotFoundError:
    from tools.kitsu_firmware_identity import parse_identity


SCHEMA = "kitsu.storage-migration-0.20.3.v1"
FLASH_BYTES = 0x800000
SECTOR_BYTES = 0x1000
PARTITION_TABLE_OFFSET = 0x008000
PARTITION_TABLE_BYTES = 0x000C00
PARTITION_TABLE_SECTOR_BYTES = 0x001000
NVS_OFFSET = 0x009000
LEGACY_NVS_BYTES = 0x005000
NVS_EXTENSION_OFFSET = 0x00E000
NVS_EXTENSION_BYTES = 0x03B000
OTA_DATA_OFFSET = 0x049000
OTA_DATA_BYTES = 0x002000
LEGACY_APP0_OFFSET = 0x010000
LOWER_GAP_OFFSET = 0x04B000
LOWER_GAP_BYTES = 0x005000
APP0_OFFSET = 0x050000
APP1_OFFSET = 0x350000
APP_SLOT_BYTES = 0x300000
OTA_JOURNAL_BYTES = 0x001000
MAX_IMAGE_BYTES = APP_SLOT_BYTES - OTA_JOURNAL_BYTES
UPPER_GAP_OFFSET = 0x650000
UPPER_GAP_BYTES = 0x020000
PRESERVED_UPPER_OFFSET = 0x670000
PRESERVED_UPPER_BYTES = FLASH_BYTES - PRESERVED_UPPER_OFFSET
KITSU_CONN_OFFSET = 0x7B0000
ESP32S3_CHIP_ID = 9
ESP_IMAGE_CHECKSUM_SEED = 0xEF
EXPECTED_ESPTOOL_VERSION = "4.11.0"
EXPECTED_ESPTOOL_PYTHON_TREE_SHA256 = (
    "aa4aa5a3bfeef8d050efd262cf5a9b78d68abd7c2437b307e4379d47919eb83a"
)
BOOT_APP0_SHA256 = "f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0"
MIGRATION_CONFIRMATION = "preserve-nvs-table-last-0.20.3"
RESTORE_CONFIRMATION_PREFIX = "restore-full-8mb:"
SOURCE_FIRMWARE_VERSION = "0.20.2"
TARGET_FIRMWARE_VERSION = "0.20.3"
NVS_ORACLE_SCHEMA = "kitsu.nvs-idf-4.4.7-expansion-oracle.v1"
NVS_ORACLE_IDF_ARCHIVE_SHA256 = (
    "ecb1124730742772364c2b7417dbfa7f55652407b0ab0c0899a1380b0598252b"
)
NVS_ORACLE_GCC_SHA256 = (
    "b5f1b773a7c733738352000c92a077dc5852a1a2fc6d836b1e411be1e9ec5f88"
)
NVS_ORACLE_GXX_SHA256 = (
    "e6718f7e0c7d057c3ff77b550c603da9bc4030e3ede3c053705acce1293dbe4d"
)
MAC_PATTERN = re.compile(r"[0-9a-f]{2}(?::[0-9a-f]{2}){5}\Z")
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}\Z")
NVS_ORACLE_OUTPUT_PATTERN = re.compile(
    r"KITSU_NVS_IDF447_ORACLE_OK total=([0-9]+) used=([0-9]+) "
    r"free=([0-9]+) namespaces=([0-9]+) iterable=([0-9]+) "
    r"reads=([0-9]+) writes=([0-9]+) erases=([0-9]+)\Z"
)
NVS_ORACLE_RUNNER_PATTERN = re.compile(
    r"KITSU_NVS_IDF447_RUNNER_OK runner_sha256=([0-9a-f]{64}) "
    r"harness_sha256=([0-9a-f]{64}) archive_sha256=([0-9a-f]{64}) "
    r"expanded_sha256=([0-9a-f]{64}) binary_sha256=([0-9a-f]{64}) "
    r"build_log_sha256=([0-9a-f]{64}) "
    r"gcc=([0-9]+\.[0-9]+\.[0-9]+) gcc_sha256=([0-9a-f]{64}) "
    r"gxx=([0-9]+\.[0-9]+\.[0-9]+) gxx_sha256=([0-9a-f]{64}) "
    r"builds=([0-9]+) deterministic=(true|false)\Z"
)

WINDOWS_FULL_CONTROL = 0x001F01FF
WINDOWS_PRIVATE_ACL_SET_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
$payload = [Console]::In.ReadToEnd() | ConvertFrom-Json
$path = [string]$payload.path
$sid = [System.Security.Principal.SecurityIdentifier]::new([string]$payload.sid)
$rights = [System.Security.AccessControl.FileSystemRights]::FullControl
$allow = [System.Security.AccessControl.AccessControlType]::Allow
if ([bool]$payload.directory) {
  $acl = [System.Security.AccessControl.DirectorySecurity]::new()
  $acl.SetAccessRuleProtection($true, $false)
  $inheritance = [System.Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
                 [System.Security.AccessControl.InheritanceFlags]::ObjectInherit
  $rule = [System.Security.AccessControl.FileSystemAccessRule]::new(
      $sid, $rights, $inheritance,
      [System.Security.AccessControl.PropagationFlags]::None, $allow)
  [void]$acl.AddAccessRule($rule)
  [System.IO.Directory]::SetAccessControl($path, $acl)
} else {
  $acl = [System.Security.AccessControl.FileSecurity]::new()
  $acl.SetAccessRuleProtection($true, $false)
  $rule = [System.Security.AccessControl.FileSystemAccessRule]::new(
      $sid, $rights, $allow)
  [void]$acl.AddAccessRule($rule)
  [System.IO.File]::SetAccessControl($path, $acl)
}
"""
WINDOWS_PRIVATE_ACL_QUERY_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
$payload = [Console]::In.ReadToEnd() | ConvertFrom-Json
$path = [string]$payload.path
if ([bool]$payload.directory) {
  $acl = [System.IO.Directory]::GetAccessControl($path)
} else {
  $acl = [System.IO.File]::GetAccessControl($path)
}
$rules = @()
foreach ($rule in $acl.GetAccessRules(
    $true, $true, [System.Security.Principal.SecurityIdentifier])) {
  $rules += [ordered]@{
    sid = [string]$rule.IdentityReference.Value
    type = [string]$rule.AccessControlType
    rights = [int64]$rule.FileSystemRights
    inherited = [bool]$rule.IsInherited
    inheritance = [string]$rule.InheritanceFlags
    propagation = [string]$rule.PropagationFlags
  }
}
[ordered]@{
  protected = [bool]$acl.AreAccessRulesProtected
  rules = $rules
} | ConvertTo-Json -Compress -Depth 5
"""

PARTITION_MAGIC = b"\xaa\x50"
PARTITION_MD5_MAGIC = b"\xeb\xeb"

OLD_LAYOUT = (
    ("nvs", 0x01, 0x02, 0x009000, 0x005000, 0),
    ("otadata", 0x01, 0x00, 0x00E000, 0x002000, 0),
    ("app0", 0x00, 0x10, 0x010000, 0x330000, 0),
    ("app1", 0x00, 0x11, 0x340000, 0x330000, 0),
    ("spiffs", 0x01, 0x82, 0x670000, 0x140000, 0),
    ("kitsu_conn", 0x01, 0x40, 0x7B0000, 0x040000, 0),
    ("coredump", 0x01, 0x03, 0x7F0000, 0x010000, 0),
)

NEW_LAYOUT = (
    ("nvs", 0x01, 0x02, 0x009000, 0x040000, 0),
    ("otadata", 0x01, 0x00, 0x049000, 0x002000, 0),
    ("app0", 0x00, 0x10, 0x050000, 0x300000, 0),
    ("app1", 0x00, 0x11, 0x350000, 0x300000, 0),
    ("spiffs", 0x01, 0x82, 0x670000, 0x140000, 0),
    ("kitsu_conn", 0x01, 0x40, 0x7B0000, 0x040000, 0),
    ("coredump", 0x01, 0x03, 0x7F0000, 0x010000, 0),
)


@dataclass(frozen=True)
class MigrationOperation:
    name: str
    kind: str
    offset: int
    size: int


OPERATIONS = (
    MigrationOperation("stage_app1_erase", "erase", APP1_OFFSET, APP_SLOT_BYTES),
    MigrationOperation("stage_app1_write", "write", APP1_OFFSET, 0),
    MigrationOperation("verify_app1_stage", "verify", APP1_OFFSET, APP_SLOT_BYTES),
    MigrationOperation(
        "expand_nvs_blank_extension", "erase", NVS_EXTENSION_OFFSET,
        NVS_EXTENSION_BYTES,
    ),
    MigrationOperation(
        "verify_nvs_extension", "verify", NVS_EXTENSION_OFFSET,
        NVS_EXTENSION_BYTES,
    ),
    MigrationOperation("move_otadata_blank", "erase", OTA_DATA_OFFSET, OTA_DATA_BYTES),
    MigrationOperation("move_otadata_write", "write", OTA_DATA_OFFSET, OTA_DATA_BYTES),
    MigrationOperation("verify_moved_otadata", "verify", OTA_DATA_OFFSET, OTA_DATA_BYTES),
    MigrationOperation("blank_lower_gap", "erase", LOWER_GAP_OFFSET, LOWER_GAP_BYTES),
    MigrationOperation("verify_lower_gap", "verify", LOWER_GAP_OFFSET, LOWER_GAP_BYTES),
    MigrationOperation("stage_app0_erase", "erase", APP0_OFFSET, APP_SLOT_BYTES),
    MigrationOperation("stage_app0_write", "write", APP0_OFFSET, 0),
    MigrationOperation("verify_app0_stage", "verify", APP0_OFFSET, APP_SLOT_BYTES),
    MigrationOperation("blank_upper_gap", "erase", UPPER_GAP_OFFSET, UPPER_GAP_BYTES),
    MigrationOperation("verify_upper_gap", "verify", UPPER_GAP_OFFSET, UPPER_GAP_BYTES),
    MigrationOperation("verify_legacy_nvs_prefix", "verify", NVS_OFFSET,
                       LEGACY_NVS_BYTES),
    MigrationOperation("verify_nvs_extension_final", "verify", NVS_EXTENSION_OFFSET,
                       NVS_EXTENSION_BYTES),
    MigrationOperation("verify_moved_otadata_final", "verify", OTA_DATA_OFFSET,
                       OTA_DATA_BYTES),
    MigrationOperation("verify_lower_gap_final", "verify", LOWER_GAP_OFFSET,
                       LOWER_GAP_BYTES),
    MigrationOperation("verify_app0_final", "verify", APP0_OFFSET, APP_SLOT_BYTES),
    MigrationOperation("verify_app1_final", "verify", APP1_OFFSET, APP_SLOT_BYTES),
    MigrationOperation("verify_upper_gap_final", "verify", UPPER_GAP_OFFSET,
                       UPPER_GAP_BYTES),
    MigrationOperation("verify_boot_prefix_precommit", "verify", 0,
                       PARTITION_TABLE_OFFSET),
    MigrationOperation("verify_preserved_upper", "verify", PRESERVED_UPPER_OFFSET,
                       PRESERVED_UPPER_BYTES),
    MigrationOperation("verify_legacy_table_precommit", "verify",
                       PARTITION_TABLE_OFFSET, PARTITION_TABLE_SECTOR_BYTES),
    MigrationOperation(
        "partition_table_commit_last", "write", PARTITION_TABLE_OFFSET,
        PARTITION_TABLE_BYTES,
    ),
    MigrationOperation("verify_partition_table_commit", "verify", PARTITION_TABLE_OFFSET,
                       PARTITION_TABLE_SECTOR_BYTES),
    MigrationOperation("verify_complete_flash", "verify", 0, FLASH_BYTES),
    MigrationOperation("reset_after_verified_commit", "reset", 0, 0),
)


def materialized_operations(application_bytes: int) -> list[dict[str, int | str]]:
    if application_bytes < 1 or application_bytes > MAX_IMAGE_BYTES:
        raise ValueError("operation plan application size is invalid")
    result: list[dict[str, int | str]] = []
    for operation in OPERATIONS:
        size = operation.size
        if operation.name in {"stage_app0_write", "stage_app1_write"}:
            size = application_bytes
        result.append({
            "name": operation.name,
            "kind": operation.kind,
            "offset": operation.offset,
            "size": size,
        })
    return result


def sha256_bytes(value: bytes | bytearray | memoryview) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def normalized_mac(value: str) -> str:
    result = value.strip().lower().replace("-", ":")
    if not MAC_PATTERN.fullmatch(result):
        raise ValueError("expected device MAC is invalid")
    return result


def normalized_sha256(value: str, description: str) -> str:
    result = value.strip().lower()
    if not SHA256_PATTERN.fullmatch(result):
        raise ValueError(f"{description} SHA-256 is invalid")
    return result


def require_file(path: Path, description: str, exact_bytes: int | None = None) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file() or resolved.is_symlink():
        raise ValueError(f"{description} is missing or not a regular file: {resolved}")
    if exact_bytes is not None and resolved.stat().st_size != exact_bytes:
        raise ValueError(f"{description} must be exactly 0x{exact_bytes:x} bytes")
    return resolved


def partition_table(entries: Sequence[tuple[str, int, int, int, int, int]]) -> bytes:
    raw = bytearray()
    for label, type_code, subtype, offset, size, flags in entries:
        encoded = label.encode("ascii") + b"\x00"
        if len(encoded) > 16:
            raise ValueError("partition label is too long")
        encoded += b"\x00" * (16 - len(encoded))
        raw.extend(struct.pack(
            "<2sBBII16sI", PARTITION_MAGIC, type_code, subtype,
            offset, size, encoded, flags,
        ))
    raw.extend(PARTITION_MD5_MAGIC + b"\xff" * 14 + hashlib.md5(raw).digest())  # nosec B324 - ESP partition format
    raw.extend(b"\xff" * (PARTITION_TABLE_BYTES - len(raw)))
    return bytes(raw)


def parse_partition_table(
    value: bytes, expected: Sequence[tuple[str, int, int, int, int, int]],
) -> None:
    if len(value) != PARTITION_TABLE_BYTES:
        raise ValueError("partition table has the wrong byte length")
    entries: list[tuple[str, int, int, int, int, int]] = []
    cursor = 0
    digest_seen = False
    while cursor + 32 <= len(value):
        block = value[cursor:cursor + 32]
        if block[:2] == PARTITION_MAGIC:
            magic, type_code, subtype, offset, size, raw_label, flags = struct.unpack(
                "<2sBBII16sI", block
            )
            del magic
            label_bytes = raw_label.split(b"\x00", 1)[0]
            try:
                label = label_bytes.decode("ascii")
            except UnicodeDecodeError as error:
                raise ValueError("partition label is not ASCII") from error
            entries.append((label, type_code, subtype, offset, size, flags))
            cursor += 32
            continue
        if block[:2] == PARTITION_MD5_MAGIC:
            if block[2:16] != b"\xff" * 14:
                raise ValueError("partition MD5 marker is malformed")
            actual = hashlib.md5(value[:cursor]).digest()  # nosec B324 - ESP format
            if block[16:] != actual:
                raise ValueError("partition table MD5 does not match")
            digest_seen = True
            cursor += 32
            break
        raise ValueError("partition table terminated without its MD5 record")
    if not digest_seen or value[cursor:] != b"\xff" * (len(value) - cursor):
        raise ValueError("partition table has non-FF trailing bytes")
    if tuple(entries) != tuple(expected):
        raise ValueError("partition table does not match the exact reviewed layout")


def validate_esp32s3_application(image: bytes) -> None:
    if len(image) < 24 or len(image) > MAX_IMAGE_BYTES:
        raise ValueError("application byte length is outside the 0x2ff000 boundary")
    if image[0] != 0xE9 or image[1] < 1 or image[1] > 16:
        raise ValueError("application ESP image header is invalid")
    if struct.unpack_from("<H", image, 12)[0] != ESP32S3_CHIP_ID:
        raise ValueError("application is not an ESP32-S3 image")
    if image[23] != 1:
        raise ValueError("application has no appended validation digest")
    cursor = 24
    checksum = ESP_IMAGE_CHECKSUM_SEED
    for _ in range(image[1]):
        if cursor + 8 > len(image):
            raise ValueError("application segment header is truncated")
        load_address, data_bytes = struct.unpack_from("<II", image, cursor)
        cursor += 8
        if (data_bytes < 1 or data_bytes % 4 != 0 or
                load_address + data_bytes > 0x1_0000_0000 or
                cursor + data_bytes > len(image)):
            raise ValueError("application segment range is invalid")
        for byte in image[cursor:cursor + data_bytes]:
            checksum ^= byte
        cursor += data_bytes
    checksum_offset = cursor + (15 - (cursor % 16))
    digest_offset = checksum_offset + 1
    if digest_offset + 32 != len(image):
        raise ValueError("application has truncated or trailing bytes")
    if image[checksum_offset] != checksum:
        raise ValueError("application checksum does not match")
    if image[digest_offset:] != hashlib.sha256(image[:digest_offset]).digest():
        raise ValueError("application appended digest does not match")


def application_at(flash: bytes, offset: int, maximum: int) -> bytes:
    if offset < 0 or maximum < 24 or offset + maximum > len(flash):
        raise ValueError("application extraction range is invalid")
    view = flash[offset:offset + maximum]
    if view[0] != 0xE9 or view[1] < 1 or view[1] > 16:
        raise ValueError("legacy app0 is not a valid ESP image")
    cursor = 24
    for _ in range(view[1]):
        if cursor + 8 > len(view):
            raise ValueError("legacy app0 segment header is truncated")
        _, data_bytes = struct.unpack_from("<II", view, cursor)
        cursor += 8
        if data_bytes < 1 or data_bytes % 4 or cursor + data_bytes > len(view):
            raise ValueError("legacy app0 segment is invalid")
        cursor += data_bytes
    checksum_offset = cursor + (15 - (cursor % 16))
    end = checksum_offset + 1 + 32
    if end > len(view):
        raise ValueError("legacy app0 digest is truncated")
    image = view[:end]
    validate_esp32s3_application(image)
    return image


def ota_select_slot(otadata: bytes) -> int:
    if len(otadata) != OTA_DATA_BYTES:
        raise ValueError("legacy OTA-data partition has the wrong byte length")
    candidates: list[int] = []
    invalid_records = 0
    for page in range(2):
        entry = otadata[page * SECTOR_BYTES:page * SECTOR_BYTES + 32]
        sequence = struct.unpack_from("<I", entry, 0)[0]
        state = struct.unpack_from("<I", entry, 24)[0]
        stored_crc = struct.unpack_from("<I", entry, 28)[0]
        actual_crc = zlib.crc32(entry[:4], 0xFFFFFFFF) & 0xFFFFFFFF
        invalid = sequence == 0xFFFFFFFF or state in (3, 4)
        if invalid:
            invalid_records += 1
        if not invalid and stored_crc == actual_crc:
            candidates.append(sequence)
    if not candidates:
        # This mirrors pinned bootloader_common_ota_select_invalid(): with no
        # factory partition and both records erased/INVALID/ABORTED, the
        # bootloader starts its fallback scan at ota_0.
        if invalid_records == 2:
            return 0
        raise ValueError("legacy OTA-data has no valid selection record")
    sequence = max(candidates)
    return ((sequence - 1) & 0xFFFFFFFF) % 2


def validate_boot_app0(value: bytes) -> None:
    if len(value) != OTA_DATA_BYTES:
        raise ValueError("boot_app0 helper must be exactly 8192 bytes")
    if sha256_bytes(value) != BOOT_APP0_SHA256:
        raise ValueError("boot_app0 helper SHA-256 is not the pinned framework artifact")
    if ota_select_slot(value) != 0:
        raise ValueError("boot_app0 helper does not select app0")


def validate_legacy_nvs_prefix(prefix: bytes) -> dict[str, int]:
    if len(prefix) != LEGACY_NVS_BYTES:
        raise ValueError("legacy NVS prefix has the wrong byte length")
    blank_pages = 0
    valid_version_pages = 0
    namespace_entries = 0
    valid_states = {0xFFFFFFFE, 0xFFFFFFFC, 0xFFFFFFF8}
    for page in range(LEGACY_NVS_BYTES // SECTOR_BYTES):
        data = prefix[page * SECTOR_BYTES:(page + 1) * SECTOR_BYTES]
        if data == b"\xff" * SECTOR_BYTES:
            blank_pages += 1
            continue
        state = struct.unpack_from("<I", data, 0)[0]
        if state not in valid_states:
            raise ValueError("legacy NVS page state is not active/full/freeing")
        if data[8] != 0xFE or data[9:28] != b"\xff" * 19:
            raise ValueError("legacy NVS page header version/reserved bytes are invalid")
        stored_header_crc = struct.unpack_from("<I", data, 28)[0]
        actual_header_crc = zlib.crc32(data[4:28], 0xFFFFFFFF) & 0xFFFFFFFF
        if stored_header_crc != actual_header_crc:
            raise ValueError("legacy NVS page header CRC does not match")
        valid_version_pages += 1
        entry_states: list[int] = []
        for index in range(126):
            bit = index * 2
            entry_states.append((data[32 + bit // 8] >> (bit % 8)) & 0x03)
        if any(state == 0x01 for state in entry_states):
            raise ValueError("legacy NVS entry-state bitmap contains ILLEGAL state")
        index = 0
        while index < 126:
            state = entry_states[index]
            entry = data[64 + index * 32:96 + index * 32]
            if state == 0x03:
                if entry != b"\xff" * 32:
                    raise ValueError("legacy NVS EMPTY entry contains programmed bytes")
                index += 1
                continue
            if state == 0x00:
                index += 1
                continue
            # Only bitmap-WRITTEN entries are items. Validate the same header
            # CRC and span bounds used by pinned IDF 4.4.7 Page::load; never
            # infer an item merely because programmed bytes happen to exist.
            span = entry[2]
            if span < 1 or index + span > 126:
                raise ValueError("legacy NVS WRITTEN item span is invalid")
            crc = zlib.crc32(entry[:4], 0xFFFFFFFF) & 0xFFFFFFFF
            crc = zlib.crc32(entry[8:], crc) & 0xFFFFFFFF
            if struct.unpack_from("<I", entry, 4)[0] != crc:
                raise ValueError("legacy NVS WRITTEN item CRC does not match")
            if any(entry_states[child] != 0x02
                   for child in range(index, index + span)):
                raise ValueError("legacy NVS WRITTEN item span bitmap is incomplete")
            key_field = entry[8:24]
            key = key_field.split(b"\x00", 1)[0]
            if (not key or b"\x00" not in key_field or
                    not all(0x20 <= value <= 0x7E for value in key)):
                raise ValueError("legacy NVS WRITTEN item key is invalid")
            if entry[0] == 0 and entry[1] == 0x01 and span == 1:
                namespace_entries += 1
            index += span
    if valid_version_pages == 0 or namespace_entries == 0:
        raise ValueError("legacy NVS must contain valid pages and a namespace entry")
    return {
        "blank_pages": blank_pages,
        "version_fe_pages": valid_version_pages,
        "namespace_entries": namespace_entries,
    }


def validate_expanded_nvs_mount_contract(value: bytes) -> dict[str, int]:
    if len(value) != LEGACY_NVS_BYTES + NVS_EXTENSION_BYTES:
        raise ValueError("expanded NVS image must be exactly 0x40000 bytes")
    prefix = validate_legacy_nvs_prefix(value[:LEGACY_NVS_BYTES])
    extension = value[LEGACY_NVS_BYTES:]
    if extension != b"\xff" * NVS_EXTENSION_BYTES:
        raise ValueError("expanded NVS extension contains non-FF bytes")
    # This mirrors the pinned IDF 4.4.7 Page::load/PageManager::load decision
    # relevant to migration: CRC-valid version-0xFE pages are sequence-sorted;
    # each all-FF 4 KiB sector is UNINITIALIZED and enters the free-page list.
    extension_pages = NVS_EXTENSION_BYTES // SECTOR_BYTES
    free_pages = prefix["blank_pages"] + extension_pages
    if free_pages == 0:
        raise ValueError("expanded NVS has no page available to PageManager")
    return {
        **prefix,
        "extension_ff_pages": extension_pages,
        "mount_free_pages": free_pages,
        "total_pages": (LEGACY_NVS_BYTES + NVS_EXTENSION_BYTES) // SECTOR_BYTES,
    }


def expected_slot(application: bytes) -> bytes:
    return application + b"\xff" * (APP_SLOT_BYTES - len(application))


def table_sector(table: bytes) -> bytes:
    return table + b"\xff" * (PARTITION_TABLE_SECTOR_BYTES - len(table))


def verify_final_image(
    original: bytes, migrated: bytes, table: bytes, application: bytes,
    boot_app0: bytes,
) -> None:
    if len(migrated) != FLASH_BYTES:
        raise ValueError("simulated flash length changed")
    if migrated[NVS_OFFSET:NVS_EXTENSION_OFFSET] != original[NVS_OFFSET:NVS_EXTENSION_OFFSET]:
        raise ValueError("legacy NVS prefix changed")
    if migrated[NVS_EXTENSION_OFFSET:OTA_DATA_OFFSET] != b"\xff" * NVS_EXTENSION_BYTES:
        raise ValueError("expanded NVS extension is not all FF")
    if migrated[OTA_DATA_OFFSET:OTA_DATA_OFFSET + OTA_DATA_BYTES] != boot_app0:
        raise ValueError("moved OTA-data partition is not exact boot_app0")
    if migrated[LOWER_GAP_OFFSET:APP0_OFFSET] != b"\xff" * LOWER_GAP_BYTES:
        raise ValueError("lower alignment gap is not blank")
    slot = expected_slot(application)
    if migrated[APP0_OFFSET:APP0_OFFSET + APP_SLOT_BYTES] != slot:
        raise ValueError("app0 slot does not match the reviewed image and FF tail")
    if migrated[APP1_OFFSET:APP1_OFFSET + APP_SLOT_BYTES] != slot:
        raise ValueError("app1 slot does not match the reviewed image and FF tail")
    if migrated[UPPER_GAP_OFFSET:PRESERVED_UPPER_OFFSET] != b"\xff" * UPPER_GAP_BYTES:
        raise ValueError("upper alignment gap is not blank")
    if migrated[PRESERVED_UPPER_OFFSET:] != original[PRESERVED_UPPER_OFFSET:]:
        raise ValueError("SPIFFS/connectivity/coredump bytes changed")
    if migrated[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + PARTITION_TABLE_SECTOR_BYTES] != table_sector(table):
        raise ValueError("partition-table commit sector does not match")


def simulate_migration(
    backup: bytes, table: bytes, application: bytes, boot_app0: bytes,
) -> tuple[bytes, dict]:
    if len(backup) != FLASH_BYTES:
        raise ValueError("full-flash backup must be exactly 8 MiB")
    parse_partition_table(
        backup[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + PARTITION_TABLE_BYTES],
        OLD_LAYOUT,
    )
    parse_partition_table(table, NEW_LAYOUT)
    validate_esp32s3_application(application)
    validate_boot_app0(boot_app0)
    nvs = validate_expanded_nvs_mount_contract(
        backup[NVS_OFFSET:NVS_EXTENSION_OFFSET] + b"\xff" * NVS_EXTENSION_BYTES
    )
    legacy_application = application_at(backup, 0x010000, 0x32F000)
    if SOURCE_FIRMWARE_VERSION.encode("ascii") not in legacy_application:
        raise ValueError("legacy app0 is not the required 0.20.2 source image")
    identity = parse_identity(application)
    if identity["firmware_version"] != TARGET_FIRMWARE_VERSION:
        raise ValueError("candidate application identity is not the required 0.20.3")
    selected_slot = ota_select_slot(backup[0x00E000:0x010000])
    if selected_slot != 0:
        raise ValueError("legacy OTA-data must select the validated app0 slot")

    migrated = bytearray(backup)
    # Destructive staging follows the actual executor order.  The partition
    # table remains byte-identical to the legacy table until the final commit.
    migrated[APP1_OFFSET:APP1_OFFSET + APP_SLOT_BYTES] = b"\xff" * APP_SLOT_BYTES
    migrated[APP1_OFFSET:APP1_OFFSET + len(application)] = application
    if migrated[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + PARTITION_TABLE_BYTES] != backup[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + PARTITION_TABLE_BYTES]:
        raise ValueError("partition table changed before final commit")

    migrated[NVS_EXTENSION_OFFSET:OTA_DATA_OFFSET] = b"\xff" * NVS_EXTENSION_BYTES
    migrated[OTA_DATA_OFFSET:OTA_DATA_OFFSET + OTA_DATA_BYTES] = boot_app0
    migrated[LOWER_GAP_OFFSET:APP0_OFFSET] = b"\xff" * LOWER_GAP_BYTES
    migrated[APP0_OFFSET:APP0_OFFSET + APP_SLOT_BYTES] = b"\xff" * APP_SLOT_BYTES
    migrated[APP0_OFFSET:APP0_OFFSET + len(application)] = application
    migrated[UPPER_GAP_OFFSET:PRESERVED_UPPER_OFFSET] = b"\xff" * UPPER_GAP_BYTES

    if migrated[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + PARTITION_TABLE_BYTES] != backup[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + PARTITION_TABLE_BYTES]:
        raise ValueError("partition table changed before precommit verification")
    if migrated[NVS_OFFSET:NVS_EXTENSION_OFFSET] != backup[NVS_OFFSET:NVS_EXTENSION_OFFSET]:
        raise ValueError("NVS prefix changed before commit")
    if migrated[PRESERVED_UPPER_OFFSET:] != backup[PRESERVED_UPPER_OFFSET:]:
        raise ValueError("upper preserved region changed before commit")

    migrated[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + PARTITION_TABLE_SECTOR_BYTES] = b"\xff" * PARTITION_TABLE_SECTOR_BYTES
    migrated[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + len(table)] = table
    verify_final_image(backup, migrated, table, application, boot_app0)

    operations = materialized_operations(len(application))
    evidence = {
        "schema": SCHEMA,
        "source_firmware_version": SOURCE_FIRMWARE_VERSION,
        "firmware_version": TARGET_FIRMWARE_VERSION,
        "flash_bytes": FLASH_BYTES,
        "backup_sha256": sha256_bytes(backup),
        "legacy_partition_table_sha256": sha256_bytes(
            backup[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + PARTITION_TABLE_BYTES]
        ),
        "new_partition_table_sha256": sha256_bytes(table),
        "application_bytes": len(application),
        "application_sha256": sha256_bytes(application),
        "application_identity": identity,
        "legacy_app0_bytes": len(legacy_application),
        "legacy_app0_sha256": sha256_bytes(legacy_application),
        "legacy_selected_slot": selected_slot,
        "boot_app0_bytes": len(boot_app0),
        "boot_app0_sha256": sha256_bytes(boot_app0),
        "legacy_nvs_prefix_sha256": sha256_bytes(
            backup[NVS_OFFSET:NVS_EXTENSION_OFFSET]
        ),
        "legacy_nvs": nvs,
        "preserved_upper_sha256": sha256_bytes(backup[PRESERVED_UPPER_OFFSET:]),
        "simulated_flash_sha256": sha256_bytes(migrated),
        "operations": operations,
        "partition_table_commit_index": next(
            index for index, operation in enumerate(operations)
            if operation["name"] == "partition_table_commit_last"
        ),
        "partition_table_commit_is_last_write": True,
        "no_chip_erase": True,
        "reset_only_after_verified_commit": True,
        "serial_tooling": {
            "esptool_version": EXPECTED_ESPTOOL_VERSION,
            "esptool_python_tree_sha256": EXPECTED_ESPTOOL_PYTHON_TREE_SHA256,
            "write_flash_settings": "mode=keep,freq=keep,size=keep",
            "final_operation": "read_mac_then_global_hard_reset_once",
            "run_command_forbidden": True,
        },
        "interruption_recovery": {
            "before_nvs_extension_erase": "legacy_app0_or_full_backup",
            "after_nvs_extension_erase_before_commit": "rom_loader_full_backup",
            "during_partition_table_commit": "rom_loader_full_backup",
            "after_verified_commit": "new_app0_with_new_app1_fallback",
        },
    }
    return bytes(migrated), evidence


def esptool_python_tree_sha256(esptool: Path) -> tuple[int, str]:
    launcher = esptool.expanduser().resolve()
    package_root = launcher.parent
    module_root = package_root / "esptool"
    sources = [launcher, *sorted(
        module_root.rglob("*.py"),
        key=lambda item: item.relative_to(package_root).as_posix(),
    )]
    tree = hashlib.sha256()
    for source in sources:
        if not source.is_file() or source.is_symlink():
            raise ValueError("pinned esptool source tree is incomplete")
        relative = source.relative_to(package_root).as_posix().encode("utf-8")
        tree.update(relative)
        tree.update(b"\0")
        tree.update(bytes.fromhex(sha256_file(source)))
    return len(sources), tree.hexdigest()


class EsptoolSession:
    def __init__(self, python: Path, esptool: Path, port: str, baud: int):
        self.python = require_file(python, "Python interpreter")
        self.esptool = require_file(esptool, "esptool.py")
        self.port = port
        self.baud = baud
        self.connected = False

    def validate_version(self) -> None:
        source_count, tree_sha256 = esptool_python_tree_sha256(self.esptool)
        if source_count != 29:
            raise ValueError("migration requires the exact pinned esptool source tree")
        if tree_sha256 != EXPECTED_ESPTOOL_PYTHON_TREE_SHA256:
            raise ValueError("migration esptool source tree SHA-256 is not pinned")
        completed = subprocess.run(
            [str(self.python), str(self.esptool), "version"],
            check=False, capture_output=True, text=True,
        )
        text = completed.stdout + completed.stderr
        if completed.returncode != 0 or f"v{EXPECTED_ESPTOOL_VERSION}" not in text:
            raise ValueError(f"migration requires esptool.py v{EXPECTED_ESPTOOL_VERSION}")

    def _run(
        self, command: Sequence[str], *, final_reset: bool = False,
        capture: bool = False,
    ) -> str:
        invocation = self.invocation(command, final_reset=final_reset)
        completed = subprocess.run(
            invocation, check=False, capture_output=capture, text=capture
        )
        if completed.returncode != 0:
            raise RuntimeError(
                "esptool gate failed; leave the device in the ROM loader and "
                "use the verified full-flash backup"
            )
        self.connected = True
        if capture:
            return (completed.stdout or "") + (completed.stderr or "")
        return ""

    def invocation(
        self, command: Sequence[str], *, final_reset: bool = False,
    ) -> list[str]:
        before = "no_reset" if self.connected else "default_reset"
        after = "hard_reset" if final_reset else "no_reset"
        return [
            str(self.python), str(self.esptool), "--chip", "esp32s3",
            "--port", self.port, "--baud", str(self.baud),
            "--before", before, "--after", after,
            *command,
        ]

    def probe(self, expected_mac: str) -> None:
        expected = normalized_mac(expected_mac)
        mac_output = self._run(["read_mac"], capture=True).lower().replace("-", ":")
        found = re.findall(r"[0-9a-f]{2}(?::[0-9a-f]{2}){5}", mac_output)
        if expected not in found:
            raise RuntimeError("connected ESP32-S3 MAC does not match the frozen target")
        flash_output = self._run(["flash_id"], capture=True).lower()
        if not re.search(r"detected flash size:\s*8mb\b", flash_output):
            raise RuntimeError("connected ESP32-S3 is not reporting exactly 8 MiB flash")

    def read(self, offset: int, size: int, output: Path) -> bytes:
        if output.exists():
            output.unlink()
        self._run(["read_flash", hex(offset), hex(size), str(output)])
        value = output.read_bytes()
        if len(value) != size:
            raise RuntimeError("esptool readback has the wrong byte length")
        return value

    def erase(self, offset: int, size: int) -> None:
        if offset % SECTOR_BYTES or size % SECTOR_BYTES:
            raise ValueError("erase range is not sector aligned")
        self._run(["erase_region", hex(offset), hex(size)])

    def write(self, offset: int, path: Path) -> None:
        self._run([
            # Every ceremony image is already byte-reviewed and the connected
            # target was independently probed as 8 MiB. `keep` on all three
            # header settings is mandatory, especially for restore offset 0:
            # esptool must not rewrite the recovery backup's bootloader header.
            "write_flash", "--flash_mode", "keep", "--flash_freq", "keep",
            "--flash_size", "keep", "--verify",
            hex(offset), str(path),
        ])

    def verify(self, offset: int, path: Path) -> None:
        self._run(["verify_flash", hex(offset), str(path)])

    def finish(self) -> None:
        if not self.connected:
            raise RuntimeError("cannot reset a migration session that is not held")
        # Pinned esptool 4.11's read_mac command only reads eFuse-backed
        # identity registers. Its global --after hard_reset handler then calls
        # esp.hard_reset() exactly once. Do not use the `run` command: it calls
        # esp.run()/flash_finish first and would transiently launch the app
        # before the global hard reset.
        self._run(["read_mac"], final_reset=True)
        self.connected = False


def require_private_output(path: Path, description: str) -> Path:
    resolved = path.expanduser().resolve()
    project = Path(__file__).resolve().parents[1]
    try:
        resolved.relative_to(project)
    except ValueError:
        pass
    else:
        raise ValueError(f"{description} must remain outside the source repository")
    resolved.parent.mkdir(parents=True, exist_ok=True)
    return resolved


def path_within(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def validate_backup_leaf_location(
    path: Path, description: str, *, project_root: Path | None = None,
    profile_root: Path | None = None,
) -> Path:
    """Require a dedicated backup leaf outside broad/sensitive directories.

    The parent of ``path`` is the directory whose ACL the capture command will
    replace.  It must therefore be a purpose-created leaf, never a volume root,
    user profile, source tree, or an ancestor/descendant of either protected
    tree.  Optional roots exist only to make the pure location policy directly
    testable; production callers always use the actual resolved roots.
    """
    expanded = path.expanduser()
    if expanded.name in {"", ".", ".."} or ":" in expanded.name:
        raise ValueError(f"{description} must name a regular backup file")
    resolved = expanded.resolve()
    leaf = resolved.parent
    if not resolved.anchor:
        raise ValueError(f"{description} must use an absolute local path")
    if os.name == "nt":
        drive, _ = os.path.splitdrive(str(resolved))
        if not re.fullmatch(r"[A-Za-z]:", drive):
            raise ValueError(f"{description} must use a local Windows volume")
    volume_root = Path(resolved.anchor).resolve()
    project = (project_root or Path(__file__).resolve().parents[1]).resolve()
    profile = (profile_root or Path(os.environ.get("USERPROFILE", Path.home()))).resolve()
    if leaf == volume_root:
        raise ValueError(f"{description} parent must not be a volume root")
    if (path_within(leaf, project) or path_within(project, leaf)):
        raise ValueError(
            f"{description} leaf must remain outside the source repository "
            "and its ancestors"
        )
    if (path_within(leaf, profile) or path_within(profile, leaf)):
        raise ValueError(
            f"{description} leaf must remain outside the user profile and "
            "its ancestors"
        )
    return resolved


def require_private_file(
    path: Path, description: str, exact_bytes: int | None = None,
) -> Path:
    resolved = require_file(path, description, exact_bytes)
    project = Path(__file__).resolve().parents[1]
    try:
        resolved.relative_to(project)
    except ValueError:
        return resolved
    raise ValueError(f"{description} must remain outside the source repository")


def fsync_parent(path: Path) -> None:
    if os.name == "nt":
        raise RuntimeError(
            "Windows directory durability requires a write-through atomic move"
        )
    try:
        descriptor = os.open(path.parent, os.O_RDONLY)
    except OSError as error:
        raise RuntimeError("cannot open the private output directory for fsync") from error
    try:
        os.fsync(descriptor)
    except OSError as error:
        raise RuntimeError("cannot fsync the private output directory") from error
    finally:
        os.close(descriptor)


def windows_durable_move(source: Path, destination: Path, replace: bool) -> None:
    import ctypes
    from ctypes import wintypes

    movefile_replace_existing = 0x00000001
    movefile_write_through = 0x00000008
    flags = movefile_write_through
    if replace:
        flags |= movefile_replace_existing
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    move = kernel32.MoveFileExW
    move.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD]
    move.restype = wintypes.BOOL
    if not move(str(source), str(destination), flags):
        error = ctypes.get_last_error()
        raise OSError(
            error,
            "MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH) failed"
            if replace else "MoveFileExW(WRITE_THROUGH) failed",
            str(destination),
        )


def durable_publish(source: Path, destination: Path, replace: bool) -> None:
    if source.parent != destination.parent:
        raise ValueError("durable private publish must remain in one directory")
    if os.name == "nt":
        windows_durable_move(source, destination, replace)
        return
    if replace:
        os.replace(source, destination)
    else:
        # link(2) is the portable fail-if-destination-exists publication gate;
        # unlinking the temporary name leaves the same inode at destination.
        os.link(source, destination)
        os.unlink(source)
    fsync_parent(destination)


def write_private_atomic(
    path: Path, value: bytes, description: str, *, replace: bool,
) -> Path:
    temporary = path.with_name(path.name + ".tmp")
    if temporary.exists():
        raise ValueError(f"stale {description} temporary file exists")
    if not replace and path.exists():
        raise ValueError(f"{description} output already exists")
    descriptor = os.open(
        temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600
    )
    try:
        with os.fdopen(descriptor, "wb", closefd=False) as stream:
            stream.write(value)
            stream.flush()
            os.fsync(stream.fileno())
    finally:
        os.close(descriptor)
    # Apply and verify the protected ACL while the bytes still have only a
    # private temporary name. The final directory entry is then published with
    # a write-through rename; no post-reset status depends on a lazy rename.
    harden_private_file(temporary)
    if temporary.read_bytes() != value:
        raise RuntimeError(f"temporary {description} failed readback")
    durable_publish(temporary, path, replace)
    inspect_private_acl(path, False)
    if path.read_bytes() != value:
        raise RuntimeError(f"persisted {description} failed readback")
    return path


def run_windows_acl_script(script: str, payload: dict) -> str:
    powershell = windows_system_directory() / "WindowsPowerShell" / "v1.0" / "powershell.exe"
    if not powershell.is_file() or powershell.is_symlink():
        raise RuntimeError("trusted Windows PowerShell executable is unavailable")
    completed = subprocess.run(
        [
            str(powershell), "-NoLogo", "-NoProfile", "-NonInteractive",
            "-ExecutionPolicy", "Bypass", "-Command", script,
        ],
        input=json.dumps(payload, sort_keys=True, separators=(",", ":")),
        check=False, capture_output=True, text=True,
    )
    if completed.returncode != 0:
        raise RuntimeError("cannot apply or inspect the private Windows DACL")
    return completed.stdout.strip()


def validate_windows_acl_snapshot(
    snapshot: dict, expected_sid: str, directory: bool,
) -> dict[str, object]:
    if snapshot.get("protected") is not True:
        raise RuntimeError("private Windows DACL still inherits access rules")
    rules = snapshot.get("rules")
    if isinstance(rules, dict):
        rules = [rules]
    if not isinstance(rules, list) or len(rules) != 1:
        raise RuntimeError("private Windows DACL has unexpected principals")
    rule = rules[0]
    if not isinstance(rule, dict):
        raise RuntimeError("private Windows DACL rule is malformed")
    if (str(rule.get("sid", "")) != expected_sid or
            str(rule.get("type", "")) != "Allow" or
            rule.get("inherited") is not False or
            int(rule.get("rights", -1)) != WINDOWS_FULL_CONTROL or
            str(rule.get("propagation", "")) != "None"):
        raise RuntimeError("private Windows DACL is not owner-only full control")
    inheritance = str(rule.get("inheritance", ""))
    inheritance_flags = {
        item.strip() for item in inheritance.split(",") if item.strip()
    }
    expected_inheritance = (
        {"ContainerInherit", "ObjectInherit"} if directory else {"None"}
    )
    if inheritance_flags != expected_inheritance:
        raise RuntimeError("private Windows DACL inheritance scope is wrong")
    return {
        "kind": "windows-protected-dacl",
        "protected": True,
        "rule_count": 1,
        "principal_sid_sha256": sha256_bytes(expected_sid.encode("ascii")),
        "full_control": True,
        "child_inheritance": directory,
    }


def inspect_private_acl(path: Path, directory: bool) -> dict[str, object]:
    resolved = path.expanduser().resolve()
    if directory:
        if not resolved.is_dir() or resolved.is_symlink():
            raise ValueError("private backup parent must be a regular directory")
    elif not resolved.is_file() or resolved.is_symlink():
        raise ValueError("private backup/evidence must be a regular file")
    if os.name != "nt":
        expected_mode = 0o700 if directory else 0o600
        actual_mode = resolved.stat().st_mode & 0o777
        if actual_mode != expected_mode:
            raise RuntimeError("private path has group/other permissions")
        principal = f"uid:{os.getuid()}"
        return {
            "kind": "posix-mode",
            "protected": True,
            "mode": f"{expected_mode:04o}",
            "rule_count": 1,
            "principal_sid_sha256": sha256_bytes(principal.encode("ascii")),
            "full_control": True,
            "child_inheritance": directory,
        }
    sid = current_windows_sid()
    raw = run_windows_acl_script(
        WINDOWS_PRIVATE_ACL_QUERY_SCRIPT,
        {"path": str(resolved), "directory": directory},
    )
    try:
        snapshot = json.loads(raw)
    except json.JSONDecodeError as error:
        raise RuntimeError("private Windows DACL query was malformed") from error
    if not isinstance(snapshot, dict):
        raise RuntimeError("private Windows DACL query was not an object")
    return validate_windows_acl_snapshot(snapshot, sid, directory)


def set_windows_private_acl(path: Path, directory: bool) -> None:
    sid = current_windows_sid()
    run_windows_acl_script(
        WINDOWS_PRIVATE_ACL_SET_SCRIPT,
        {"path": str(path.expanduser().resolve()), "sid": sid,
         "directory": directory},
    )


def harden_private_file(path: Path) -> dict[str, object]:
    if os.name != "nt":
        os.chmod(path, 0o600)
    else:
        set_windows_private_acl(path, False)
    return inspect_private_acl(path, False)


def current_windows_sid() -> str:
    whoami = windows_system_directory() / "whoami.exe"
    if not whoami.is_file() or whoami.is_symlink():
        raise RuntimeError("trusted Windows identity executable is unavailable")
    identity = subprocess.run(
        [str(whoami), "/user", "/fo", "csv", "/nh"],
        check=False, capture_output=True, text=True,
    )
    if identity.returncode != 0:
        raise RuntimeError("cannot resolve the current Windows security identity")
    rows = list(csv.reader(identity.stdout.splitlines()))
    if len(rows) != 1 or len(rows[0]) != 2 or not rows[0][1].startswith("S-1-"):
        raise RuntimeError("current Windows SID output is malformed")
    return rows[0][1]


def windows_system_directory() -> Path:
    if os.name != "nt":
        raise RuntimeError("Windows system directory requested on another platform")
    import ctypes

    buffer = ctypes.create_unicode_buffer(32768)
    length = ctypes.windll.kernel32.GetSystemDirectoryW(buffer, len(buffer))  # type: ignore[attr-defined]
    if length == 0 or length >= len(buffer):
        raise RuntimeError("cannot resolve the Windows system directory")
    resolved = Path(buffer.value).resolve()
    if not resolved.is_dir() or resolved.is_symlink():
        raise RuntimeError("Windows system directory is unavailable")
    return resolved


def harden_private_directory(path: Path) -> dict[str, object]:
    resolved = path.expanduser().resolve()
    if not resolved.is_dir() or resolved.is_symlink():
        raise ValueError("private backup parent must be a regular directory")
    if os.name != "nt":
        os.chmod(resolved, 0o700)
    else:
        set_windows_private_acl(resolved, True)
    return inspect_private_acl(resolved, True)


def backup_volume_identity(path: Path) -> dict[str, int | str]:
    resolved = path.expanduser().resolve()
    probe = resolved
    while not probe.exists() and probe.parent != probe:
        probe = probe.parent
    if not probe.exists():
        raise ValueError("backup path has no existing volume ancestor")
    stat_device = int(probe.stat().st_dev)
    if os.name != "nt":
        return {
            "kind": "posix-device",
            "st_dev": stat_device,
        }
    import ctypes

    drive, _ = os.path.splitdrive(str(resolved))
    if not drive:
        raise ValueError("Windows backup path has no local volume root")
    root = drive.upper() + "\\"
    serial = ctypes.c_uint32()
    maximum_component = ctypes.c_uint32()
    flags = ctypes.c_uint32()
    filesystem = ctypes.create_unicode_buffer(64)
    if not ctypes.windll.kernel32.GetVolumeInformationW(  # type: ignore[attr-defined]
        root, None, 0, ctypes.byref(serial), ctypes.byref(maximum_component),
        ctypes.byref(flags), filesystem, len(filesystem),
    ):
        raise RuntimeError("cannot resolve Windows backup volume identity")
    return {
        "kind": "windows-volume",
        "root": root,
        "serial": f"{serial.value:08x}",
        "filesystem": filesystem.value,
        "st_dev": stat_device,
    }


def validate_backup_volume_pair(primary: Path, redundant: Path) -> tuple[dict, dict]:
    primary_volume = backup_volume_identity(primary)
    redundant_volume = backup_volume_identity(redundant)
    if primary_volume["st_dev"] == redundant_volume["st_dev"]:
        raise ValueError("recovery backups must be on distinct storage devices")
    if os.name == "nt":
        if (str(primary_volume.get("filesystem", "")).upper() != "NTFS" or
                str(redundant_volume.get("filesystem", "")).upper() != "NTFS"):
            raise ValueError("recovery backup volumes must provide NTFS DACLs")
        if (primary_volume["serial"] == redundant_volume["serial"] or
                primary_volume["root"] == redundant_volume["root"]):
            raise ValueError("recovery backups must use distinct Windows volumes")
    return primary_volume, redundant_volume


def create_hardened_backup_leaf(output: Path, description: str) -> None:
    raw_parent = output.expanduser().parent
    if raw_parent.is_symlink():
        raise ValueError(f"{description} leaf must not be a symlink")
    leaf = output.parent
    if leaf.exists():
        raise ValueError(f"{description} dedicated leaf already exists")
    base = leaf.parent
    if not base.is_dir() or base.is_symlink():
        raise ValueError(
            f"{description} dedicated leaf requires an existing regular base"
        )
    leaf.mkdir(mode=0o700)
    harden_private_directory(leaf)
    if any(leaf.iterdir()):
        raise RuntimeError(f"{description} dedicated leaf is not empty")


def prepare_capture_backup_pair(
    primary: Path, redundant: Path,
) -> tuple[Path, Path]:
    primary_path = validate_backup_leaf_location(
        primary, "primary recovery backup"
    )
    redundant_path = validate_backup_leaf_location(
        redundant, "redundant recovery backup"
    )
    if primary_path == redundant_path or primary_path.parent == redundant_path.parent:
        raise ValueError("recovery backups require distinct dedicated leaf directories")
    validate_backup_volume_pair(primary_path, redundant_path)
    # Check both destinations before creating either leaf, so an unsafe or
    # pre-existing redundant destination cannot cause even an empty partial
    # ceremony directory to appear on the primary volume.
    for output, description in (
        (primary_path, "primary recovery backup"),
        (redundant_path, "redundant recovery backup"),
    ):
        raw_parent = Path(output).expanduser().parent
        if raw_parent.is_symlink():
            raise ValueError(f"{description} leaf must not be a symlink")
        leaf = output.parent
        if leaf.exists():
            raise ValueError(f"{description} dedicated leaf already exists")
        base = leaf.parent
        if not base.is_dir() or base.is_symlink():
            raise ValueError(
                f"{description} dedicated leaf requires an existing regular base"
            )
    create_hardened_backup_leaf(primary_path, "primary recovery backup")
    create_hardened_backup_leaf(redundant_path, "redundant recovery backup")
    return primary_path, redundant_path


def private_exclusive_bytes(path: Path, value: bytes, description: str) -> Path:
    resolved = require_private_output(path, description)
    return write_private_atomic(
        resolved, value, description, replace=False
    )


def private_staged_bytes(path: Path, value: bytes, description: str) -> Path:
    resolved = require_private_output(path, description)
    if not resolved.exists():
        return private_exclusive_bytes(resolved, value, description)
    if not resolved.is_file() or resolved.read_bytes() != value:
        raise ValueError(f"existing {description} differs from validated bytes")
    return resolved


def exclusive_json(path: Path, value: dict) -> None:
    path = require_private_output(path, "migration evidence")
    if path.exists():
        raise ValueError("migration evidence output already exists")
    payload = (
        json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n"
    ).encode("utf-8")
    write_private_atomic(
        path, payload, "migration evidence", replace=False
    )


def canonical_json_bytes(value: dict) -> bytes:
    return (
        json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n"
    ).encode("utf-8")


def replace_private_json(
    path: Path, value: dict, *, expected_current: Sequence[dict] | None = None,
) -> None:
    resolved = require_private_output(path, "migration result")
    if not resolved.is_file():
        raise ValueError("migration result reservation is missing")
    inspect_private_acl(resolved, False)
    if expected_current is not None and resolved.read_bytes() not in {
        canonical_json_bytes(candidate) for candidate in expected_current
    }:
        raise ValueError("migration result state changed outside this transaction")
    payload = canonical_json_bytes(value)
    write_private_atomic(
        resolved, payload, "migration result", replace=True
    )


def migration_result(binding: dict, status: str) -> dict:
    if status == "prepared":
        return {
            "schema": "kitsu.storage-migration-0.20.3.result-preflight.v1",
            "status": status,
            **binding,
        }
    if status != "flash_verified":
        raise ValueError("migration result status is invalid")
    return {
        "schema": "kitsu.storage-migration-0.20.3.result.v1",
        "status": status,
        **binding,
        "partition_table_committed_last": True,
        "full_readback_before_reset": True,
    }


def preflight_private_json_output(path: Path, binding: dict) -> tuple[Path, str]:
    resolved = require_private_output(path, "migration result")
    temporary = resolved.with_name(resolved.name + ".tmp")
    if temporary.exists():
        raise ValueError("stale migration result temporary file exists")
    prepared = migration_result(binding, "prepared")
    payload = canonical_json_bytes(prepared)
    if resolved.exists():
        if not resolved.is_file():
            raise ValueError("migration result output already exists")
        inspect_private_acl(resolved, False)
        existing = resolved.read_bytes()
        for status in ("prepared", "flash_verified"):
            if existing == canonical_json_bytes(migration_result(binding, status)):
                return resolved, status
        raise ValueError("migration result output already exists")
    # Reserve and fsync the exact destination before the first flash mutation.
    # If a post-commit result replacement fails, resume accepts this same
    # bound reservation, verifies the full current image, and retries it.
    exclusive_json(resolved, prepared)
    return resolved, "prepared"


def restore_result(binding: dict, status: str) -> dict:
    if status not in {"prepared", "flash_verified"}:
        raise ValueError("restore result status is invalid")
    return {
        "schema": "kitsu.storage-migration-0.20.3.restore-result.v1",
        "status": status,
        **binding,
        "partition_table_restored_last": status == "flash_verified",
        "full_readback_before_reset": status == "flash_verified",
    }


def preflight_restore_result(path: Path, binding: dict) -> tuple[Path, str]:
    resolved = require_private_output(path, "restore result")
    temporary = resolved.with_name(resolved.name + ".tmp")
    if temporary.exists():
        raise ValueError("stale restore result temporary file exists")
    if resolved.exists():
        if not resolved.is_file():
            raise ValueError("restore result output already exists")
        inspect_private_acl(resolved, False)
        existing = resolved.read_bytes()
        for status in ("prepared", "flash_verified"):
            if existing == canonical_json_bytes(restore_result(binding, status)):
                return resolved, status
        raise ValueError("restore result output already exists")
    exclusive_json(resolved, restore_result(binding, "prepared"))
    return resolved, "prepared"


def load_canonical_json(path: Path) -> dict:
    source = require_file(path, "frozen migration manifest")
    raw = source.read_bytes()
    try:
        text = raw.decode("utf-8")
        pairs: list[tuple[str, object]] = []

        def unique(values: list[tuple[str, object]]) -> dict:
            keys = [key for key, _ in values]
            if len(keys) != len(set(keys)):
                raise ValueError("migration manifest contains duplicate keys")
            return dict(values)

        value = json.loads(text, object_pairs_hook=unique)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError("migration manifest is not canonical JSON") from error
    if not isinstance(value, dict):
        raise ValueError("migration manifest must be an object")
    canonical = json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n"
    if text != canonical:
        raise ValueError("migration manifest bytes are not canonical")
    return value


def nvs_oracle_source_path() -> Path:
    return Path(__file__).resolve().with_name(
        "test_kitsu_nvs_expansion_oracle.cpp"
    )


def nvs_oracle_runner_path() -> Path:
    return Path(__file__).resolve().with_name(
        "run_kitsu_nvs_expansion_oracle.sh"
    )


def parse_nvs_oracle_result_line(value: str) -> dict[str, int]:
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as error:
        raise ValueError("NVS oracle output is not ASCII") from error
    if b"\r" in encoded or b"\n" in encoded:
        raise ValueError("NVS oracle result must be exactly one line")
    match = NVS_ORACLE_OUTPUT_PATTERN.fullmatch(value)
    if not match:
        raise ValueError("NVS oracle output is not the exact success record")
    keys = (
        "total_entries", "used_entries", "free_entries", "namespace_count",
        "iterable_entries", "read_operations", "write_operations",
        "erase_operations",
    )
    result = dict(zip(keys, (int(item, 10) for item in match.groups())))
    if result["total_entries"] != 8064:
        raise ValueError("NVS oracle reported the wrong expanded entry capacity")
    if (result["used_entries"] + result["free_entries"] !=
            result["total_entries"]):
        raise ValueError("NVS oracle entry accounting is inconsistent")
    if result["free_entries"] < 4096:
        raise ValueError("NVS oracle did not prove the required free headroom")
    if result["namespace_count"] < 6 or result["iterable_entries"] < 17:
        raise ValueError("NVS oracle did not enumerate the required frozen state")
    if result["read_operations"] < 1:
        raise ValueError("NVS oracle did not exercise the pinned storage reader")
    if result["write_operations"] != 0 or result["erase_operations"] != 0:
        raise ValueError("NVS oracle initialization was not read-only")
    return result


def parse_nvs_oracle_output(value: str) -> tuple[dict[str, object], dict[str, int]]:
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as error:
        raise ValueError("NVS oracle evidence output is not ASCII") from error
    if encoded.endswith(b"\n"):
        encoded = encoded[:-1]
    if b"\r" in encoded or encoded.count(b"\n") != 1:
        raise ValueError("NVS oracle evidence must contain exactly two LF lines")
    runner_line_raw, result_line_raw = encoded.split(b"\n", 1)
    runner_line = runner_line_raw.decode("ascii")
    result_line = result_line_raw.decode("ascii")
    match = NVS_ORACLE_RUNNER_PATTERN.fullmatch(runner_line)
    if not match:
        raise ValueError("NVS oracle runner output is not the exact success record")
    (runner_sha, harness_sha, archive_sha, expanded_sha, binary_sha,
     build_log_sha, gcc_version, gcc_sha, gxx_version, gxx_sha,
     builds_raw, deterministic_raw) = match.groups()
    runner = {
        "runner_sha256": runner_sha,
        "harness_sha256": harness_sha,
        "archive_sha256": archive_sha,
        "expanded_sha256": expanded_sha,
        "binary_sha256": binary_sha,
        "build_log_sha256": build_log_sha,
        "gcc_version": gcc_version,
        "gcc_sha256": gcc_sha,
        "gxx_version": gxx_version,
        "gxx_sha256": gxx_sha,
        "builds": int(builds_raw, 10),
        "deterministic": deterministic_raw == "true",
        "runner_output": runner_line,
    }
    if (runner["gcc_version"] != "15.2.0" or
            runner["gxx_version"] != "15.2.0" or
            runner["gcc_sha256"] != NVS_ORACLE_GCC_SHA256 or
            runner["gxx_sha256"] != NVS_ORACLE_GXX_SHA256 or
            runner["builds"] != 2 or runner["deterministic"] is not True):
        raise ValueError("NVS oracle runner did not use the reviewed deterministic build")
    return runner, parse_nvs_oracle_result_line(result_line)


def build_nvs_oracle_record(
    legacy_prefix: bytes, oracle_output: str,
    idf_source_archive_sha256: str, oracle_source_sha256: str,
    runner_source_sha256: str,
) -> dict:
    validate_legacy_nvs_prefix(legacy_prefix)
    expanded = legacy_prefix + b"\xff" * NVS_EXTENSION_BYTES
    validate_expanded_nvs_mount_contract(expanded)
    archive_sha = normalized_sha256(
        idf_source_archive_sha256, "pinned IDF source archive"
    )
    if archive_sha != NVS_ORACLE_IDF_ARCHIVE_SHA256:
        raise ValueError("NVS oracle did not use the pinned IDF 4.4.7 source archive")
    source_sha = normalized_sha256(
        oracle_source_sha256, "NVS oracle source"
    )
    runner_source_sha = normalized_sha256(
        runner_source_sha256, "NVS oracle runner source"
    )
    runner, stats = parse_nvs_oracle_output(oracle_output)
    if (runner["runner_sha256"] != runner_source_sha or
            runner["harness_sha256"] != source_sha or
            runner["archive_sha256"] != archive_sha or
            runner["expanded_sha256"] != sha256_bytes(expanded)):
        raise ValueError("NVS oracle runner output does not bind the reviewed inputs")
    return {
        "schema": NVS_ORACLE_SCHEMA,
        "idf_version": "4.4.7",
        "idf_source_archive_sha256": archive_sha,
        "runner_source_sha256": runner_source_sha,
        "oracle_source_sha256": source_sha,
        "legacy_nvs_prefix_bytes": len(legacy_prefix),
        "legacy_nvs_prefix_sha256": sha256_bytes(legacy_prefix),
        "expanded_nvs_bytes": len(expanded),
        "expanded_nvs_sha256": sha256_bytes(expanded),
        "runner": runner,
        "oracle_output": oracle_output.rstrip("\n").split("\n", 1)[1],
        "result": {
            **stats,
            "byte_identical_after_init": True,
            "critical_items_present": True,
        },
    }


def validate_nvs_oracle_evidence(
    path: Path, expected_sha256: str, legacy_prefix: bytes,
) -> dict:
    evidence_path = require_file(path, "pinned-IDF NVS oracle evidence")
    actual_sha = sha256_file(evidence_path)
    if actual_sha != normalized_sha256(
        expected_sha256, "pinned-IDF NVS oracle evidence"
    ):
        raise ValueError("pinned-IDF NVS oracle evidence SHA-256 is not reviewed")
    record = load_canonical_json(evidence_path)
    source_path = require_file(
        nvs_oracle_source_path(), "pinned-IDF NVS oracle source"
    )
    runner_path = require_file(
        nvs_oracle_runner_path(), "pinned-IDF NVS oracle runner"
    )
    output = record.get("oracle_output")
    runner = record.get("runner")
    if (not isinstance(output, str) or not isinstance(runner, dict) or
            not isinstance(runner.get("runner_output"), str)):
        raise ValueError("pinned-IDF NVS oracle evidence has no exact output records")
    combined_output = runner["runner_output"] + "\n" + output
    expected = build_nvs_oracle_record(
        legacy_prefix, combined_output, NVS_ORACLE_IDF_ARCHIVE_SHA256,
        sha256_file(source_path), sha256_file(runner_path),
    )
    if record != expected:
        raise ValueError("pinned-IDF NVS oracle evidence does not bind this backup")
    return {
        "evidence_sha256": actual_sha,
        **record,
    }


def oracle_record_command(args: argparse.Namespace) -> int:
    prefix_path = require_file(
        args.legacy_nvs, "captured legacy NVS prefix", LEGACY_NVS_BYTES
    )
    archive_path = require_file(
        args.idf_source_archive, "pinned IDF source archive"
    )
    output_path = require_file(args.oracle_output, "pinned-IDF oracle output")
    binary_path = require_file(args.oracle_binary, "pinned-IDF oracle binary")
    build_log_a_path = require_file(
        args.oracle_build_log_a, "pinned-IDF oracle build A log"
    )
    build_log_b_path = require_file(
        args.oracle_build_log_b, "pinned-IDF oracle build B log"
    )
    try:
        oracle_output = output_path.read_text(encoding="ascii")
    except UnicodeDecodeError as error:
        raise ValueError("pinned-IDF oracle output is not ASCII") from error
    source_path = require_file(
        nvs_oracle_source_path(), "pinned-IDF NVS oracle source"
    )
    runner_path = require_file(
        nvs_oracle_runner_path(), "pinned-IDF NVS oracle runner"
    )
    record = build_nvs_oracle_record(
        prefix_path.read_bytes(), oracle_output, sha256_file(archive_path),
        sha256_file(source_path), sha256_file(runner_path),
    )
    if record["runner"]["binary_sha256"] != sha256_file(binary_path):
        raise ValueError("retained NVS oracle binary SHA-256 does not match output")
    if build_log_a_path.read_bytes() != build_log_b_path.read_bytes():
        raise ValueError("deterministic NVS oracle build logs differ")
    if (record["runner"]["build_log_sha256"] !=
            sha256_file(build_log_a_path)):
        raise ValueError("retained NVS oracle build log SHA-256 does not match output")
    exclusive_json(args.evidence, record)
    print(
        "KITSU_NVS_ORACLE_EVIDENCE_OK "
        f"sha256={sha256_file(args.evidence.expanduser().resolve())} "
        f"expanded_nvs_sha256={record['expanded_nvs_sha256']}"
    )
    return 0


def bound_evidence(
    backup: bytes, table: bytes, application: bytes, boot_app0: bytes,
    nvs_oracle: dict,
    target_mac: str, expected_source_app_sha256: str,
    expected_application_sha256: str, expected_source_app_bytes: int,
    expected_application_bytes: int,
) -> dict:
    _, evidence = simulate_migration(backup, table, application, boot_app0)
    source_hash = normalized_sha256(
        expected_source_app_sha256, "source app0"
    )
    candidate_hash = normalized_sha256(
        expected_application_sha256, "candidate application"
    )
    if evidence["legacy_app0_sha256"] != source_hash:
        raise ValueError("backup-selected source app0 SHA-256 is not the reviewed input")
    if evidence["application_sha256"] != candidate_hash:
        raise ValueError("candidate application SHA-256 is not the reviewed input")
    if (expected_source_app_bytes < 1 or
            evidence["legacy_app0_bytes"] != expected_source_app_bytes):
        raise ValueError("backup-selected source app0 size is not the reviewed input")
    if (expected_application_bytes < 1 or
            evidence["application_bytes"] != expected_application_bytes):
        raise ValueError("candidate application size is not the reviewed input")
    evidence["target_mac"] = normalized_mac(target_mac)
    evidence["reviewed_source_app0_sha256"] = source_hash
    evidence["reviewed_application_sha256"] = candidate_hash
    evidence["reviewed_source_app0_bytes"] = expected_source_app_bytes
    evidence["reviewed_application_bytes"] = expected_application_bytes
    evidence["redundant_backup_verified"] = True
    if not isinstance(nvs_oracle, dict) or nvs_oracle.get("schema") != NVS_ORACLE_SCHEMA:
        raise ValueError("pinned-IDF NVS oracle binding is missing")
    if nvs_oracle.get("legacy_nvs_prefix_sha256") != evidence[
        "legacy_nvs_prefix_sha256"
    ]:
        raise ValueError("pinned-IDF NVS oracle does not bind the backup prefix")
    evidence["pinned_idf_nvs_oracle"] = nvs_oracle
    return evidence


def validate_reviewed_source_application(
    source_app: bytes, expected_sha256: str, expected_bytes: int,
) -> str:
    """Bind the captured legacy app to the independently reviewed artifact."""
    reviewed_hash = normalized_sha256(expected_sha256, "source app0")
    if expected_bytes < 1 or len(source_app) != expected_bytes:
        raise ValueError("captured source app0 size is not the reviewed input")
    actual_hash = sha256_bytes(source_app)
    if actual_hash != reviewed_hash:
        raise ValueError("captured source app0 SHA-256 is not the reviewed input")
    # This is a diagnostic consistency check only. The exact byte count and
    # SHA-256 above, not a version substring, are the source authority.
    if SOURCE_FIRMWARE_VERSION.encode("ascii") not in source_app:
        raise ValueError("reviewed source app0 lacks the expected 0.20.2 diagnostic")
    return actual_hash


def backup_authority(primary: Path, redundant: Path) -> dict:
    primary_path = validate_backup_leaf_location(
        primary, "primary full-flash backup"
    )
    redundant_path = validate_backup_leaf_location(
        redundant, "redundant full-flash backup"
    )
    if primary_path.parent == redundant_path.parent:
        raise ValueError("recovery backups must use distinct private directories")
    if (not primary_path.is_file() or primary_path.is_symlink() or
            not redundant_path.is_file() or redundant_path.is_symlink()):
        raise ValueError("recovery backup authority must be regular files")
    if (os.path.samefile(primary_path, redundant_path) or
            (primary_path.stat().st_dev, primary_path.stat().st_ino) ==
            (redundant_path.stat().st_dev, redundant_path.stat().st_ino)):
        raise ValueError("primary and redundant backups must not be hardlinks")
    for backup_path, description in (
        (primary_path, "primary full-flash backup"),
        (redundant_path, "redundant full-flash backup"),
    ):
        entries = list(backup_path.parent.iterdir())
        if (len(entries) != 1 or entries[0].is_symlink() or
                entries[0].resolve() != backup_path):
            raise ValueError(
                f"{description} dedicated leaf contains unexpected files"
            )
    primary_volume, redundant_volume = validate_backup_volume_pair(
        primary_path, redundant_path
    )
    primary_directory_acl = inspect_private_acl(primary_path.parent, True)
    redundant_directory_acl = inspect_private_acl(redundant_path.parent, True)
    primary_file_acl = inspect_private_acl(primary_path, False)
    redundant_file_acl = inspect_private_acl(redundant_path, False)
    principal_hashes = {
        str(item["principal_sid_sha256"])
        for item in (
            primary_directory_acl, redundant_directory_acl,
            primary_file_acl, redundant_file_acl,
        )
    }
    if len(principal_hashes) != 1:
        raise RuntimeError("recovery backup ACL principals do not match")
    return {
        "backup_distinct_file_ids": True,
        "backup_distinct_directories": True,
        "backup_distinct_volumes": True,
        "backup_acl_private": True,
        "backup_acl_principal_sid_sha256": principal_hashes.pop(),
        "backup_primary_directory_acl": primary_directory_acl,
        "backup_primary_file_acl": primary_file_acl,
        "backup_redundant_directory_acl": redundant_directory_acl,
        "backup_redundant_file_acl": redundant_file_acl,
        "backup_primary_volume": primary_volume,
        "backup_redundant_volume": redundant_volume,
    }


def add_backup_authority_evidence(
    evidence: dict, primary: Path, redundant: Path,
) -> None:
    authority = backup_authority(primary, redundant)
    evidence.update(authority)


def verified_backup_pair(primary: Path, redundant: Path) -> tuple[Path, bytes]:
    for raw, description in (
        (primary.expanduser(), "primary full-flash backup"),
        (redundant.expanduser(), "redundant full-flash backup"),
    ):
        if raw.is_symlink() or raw.parent.is_symlink():
            raise ValueError(f"{description} authority must not use symlinks")
    primary_path = require_private_file(
        primary, "primary full-flash backup", FLASH_BYTES
    )
    redundant_path = require_private_file(
        redundant, "redundant full-flash backup", FLASH_BYTES
    )
    # Capture already created and protected the dedicated leaves before any
    # device read. Auditing/restoring validates those exact DACLs without
    # mutating a possibly unsafe existing directory.
    backup_authority(primary_path, redundant_path)
    primary_bytes = primary_path.read_bytes()
    if redundant_path.read_bytes() != primary_bytes:
        raise ValueError("primary and redundant full-flash backups differ")
    return primary_path, primary_bytes


def audit_command(args: argparse.Namespace) -> int:
    backup_path, backup = verified_backup_pair(args.backup, args.backup_copy)
    table_path = require_file(args.partitions, "new partition table", PARTITION_TABLE_BYTES)
    app_path = require_file(args.application, "0.20.3 application")
    boot_app0_path = require_file(args.otadata, "boot_app0 helper", OTA_DATA_BYTES)
    nvs_oracle = validate_nvs_oracle_evidence(
        args.nvs_oracle_evidence,
        args.expected_nvs_oracle_evidence_sha256,
        backup[NVS_OFFSET:NVS_EXTENSION_OFFSET],
    )
    evidence = bound_evidence(
        backup, table_path.read_bytes(), app_path.read_bytes(),
        boot_app0_path.read_bytes(), nvs_oracle, args.expected_mac,
        args.expected_source_app_sha256, args.expected_application_sha256,
        args.expected_source_app_bytes, args.expected_application_bytes,
    )
    add_backup_authority_evidence(evidence, backup_path, args.backup_copy)
    if args.evidence:
        exclusive_json(args.evidence.expanduser().resolve(), evidence)
    print(json.dumps(evidence, sort_keys=True, separators=(",", ":")))
    return 0


def capture_command(args: argparse.Namespace) -> int:
    # Validate both locations and protect two brand-new dedicated leaves before
    # entering the ROM loader or reading a single sensitive flash byte.
    backup_path, backup_copy_path = prepare_capture_backup_pair(
        args.backup, args.backup_copy
    )
    session = EsptoolSession(args.python, args.esptool, args.port, args.baud)
    session.validate_version()
    session.probe(args.expected_mac)
    with tempfile.TemporaryDirectory(prefix="kitsu-0203-capture-") as raw:
        temp = Path(raw)
        first = session.read(0, FLASH_BYTES, temp / "backup-first.bin")
        second = session.read(0, FLASH_BYTES, temp / "backup-second.bin")
        assert_bytes(second, first, "independent full-flash backups")
        parse_partition_table(
            first[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + PARTITION_TABLE_BYTES],
            OLD_LAYOUT,
        )
        validate_legacy_nvs_prefix(first[NVS_OFFSET:NVS_EXTENSION_OFFSET])
        source_app = application_at(first, 0x010000, 0x32F000)
        source_app_sha256 = validate_reviewed_source_application(
            source_app, args.expected_source_app_sha256,
            args.expected_source_app_bytes,
        )
        if ota_select_slot(first[0x00E000:0x010000]) != 0:
            raise ValueError("captured legacy OTA-data does not select app0")
        private_exclusive_bytes(backup_path, first, "primary recovery backup")
        private_exclusive_bytes(
            backup_copy_path, first, "redundant recovery backup"
        )
        if (sha256_file(backup_path) != sha256_file(backup_copy_path) or
                backup_path.read_bytes() != backup_copy_path.read_bytes()):
            raise RuntimeError("redundant recovery backups do not match")
        verified_backup_pair(backup_path, backup_copy_path)
        authority = backup_authority(backup_path, backup_copy_path)
        # Re-probe and re-read after both durable backup authorities exist.
        # EsptoolSession remains connected with --before/--after no_reset, so
        # this is affirmative evidence that the target is still held in the
        # ROM loader and that its commit sector has not drifted.
        session.probe(args.expected_mac)
        held_table = session.read(
            PARTITION_TABLE_OFFSET, PARTITION_TABLE_SECTOR_BYTES,
            temp / "loader-held-table.bin",
        )
        assert_bytes(
            held_table,
            first[PARTITION_TABLE_OFFSET:
                  PARTITION_TABLE_OFFSET + PARTITION_TABLE_SECTOR_BYTES],
            "post-backup loader-held partition table",
        )
    print(
        "KITSU_CAPTURE_OK "
        f"target_mac={normalized_mac(args.expected_mac)} "
        f"backup_sha256={sha256_bytes(first)} "
        f"source_app_sha256={source_app_sha256} "
        f"held_table_sha256={sha256_bytes(held_table)} "
        f"primary_volume={authority['backup_primary_volume']['root'] if os.name == 'nt' else authority['backup_primary_volume']['st_dev']} "
        f"redundant_volume={authority['backup_redundant_volume']['root'] if os.name == 'nt' else authority['backup_redundant_volume']['st_dev']} "
        "held_in_loader=true"
    )
    return 0


def assert_bytes(actual: bytes, expected: bytes, description: str) -> None:
    if actual != expected:
        raise RuntimeError(f"readback mismatch: {description}")


def migration_inputs(args: argparse.Namespace) -> tuple[
    Path, bytes, Path, bytes, Path, bytes, Path, bytes, dict, str,
]:
    backup_path, backup = verified_backup_pair(args.backup, args.backup_copy)
    table_path = require_file(args.partitions, "new partition table", PARTITION_TABLE_BYTES)
    app_path = require_file(args.application, "0.20.3 application")
    boot_app0_path = require_file(args.otadata, "boot_app0 helper", OTA_DATA_BYTES)
    table = table_path.read_bytes()
    application = app_path.read_bytes()
    boot_app0 = boot_app0_path.read_bytes()
    nvs_oracle = validate_nvs_oracle_evidence(
        args.nvs_oracle_evidence,
        args.expected_nvs_oracle_evidence_sha256,
        backup[NVS_OFFSET:NVS_EXTENSION_OFFSET],
    )
    expected = bound_evidence(
        backup, table, application, boot_app0, nvs_oracle, args.expected_mac,
        args.expected_source_app_sha256, args.expected_application_sha256,
        args.expected_source_app_bytes, args.expected_application_bytes,
    )
    add_backup_authority_evidence(expected, backup_path, args.backup_copy)
    manifest_path = require_file(args.manifest, "frozen migration manifest")
    manifest_sha256 = sha256_file(manifest_path)
    if manifest_sha256 != normalized_sha256(
        args.expected_manifest_sha256, "migration manifest"
    ):
        raise ValueError("frozen migration manifest SHA-256 is not reviewed")
    manifest = load_canonical_json(manifest_path)
    if manifest != expected:
        raise ValueError("frozen migration manifest does not bind the exact inputs")
    return (
        backup_path, backup, table_path, table, app_path, application,
        boot_app0_path, boot_app0, expected, manifest_sha256,
    )


def result_binding(evidence: dict, manifest_sha256: str) -> dict:
    return {
        "target_mac": evidence["target_mac"],
        "backup_sha256": evidence["backup_sha256"],
        "manifest_sha256": manifest_sha256,
        "final_flash_sha256": evidence["simulated_flash_sha256"],
    }


def finalize_verified_migration(
    session: EsptoolSession, result_path: Path, binding: dict, status: str,
) -> None:
    prepared = migration_result(binding, "prepared")
    verified = migration_result(binding, "flash_verified")
    if status == "prepared":
        replace_private_json(
            result_path, verified, expected_current=[prepared]
        )
        status = "flash_verified"
    elif status != "flash_verified":
        raise ValueError("migration result cannot enter the reset phase")
    # The terminal record means flash-verified, independent of reset ACK.  It
    # is durable before the sole run command, so there is no post-reset write
    # window in which a legitimate boot can mutate NVS and strand resume.
    session.finish()


def verify_runtime_immutable_ranges(
    session: EsptoolSession, temp: Path, expected: bytes,
    ranges: Sequence[tuple[int, int, str]],
) -> None:
    """Revalidate boot/layout/application bytes after a reset ACK was lost.

    A durable ``flash_verified`` result was written only after a complete
    pre-reset readback.  If the reset then succeeded but its host ACK was lost,
    the running firmware may legitimately update NVS, OTA metadata,
    ``kitsu_conn``, or coredump state before the operator returns to ROM mode.
    Those runtime-owned ranges must not strand recovery.  Everything listed
    here is immutable under normal runtime and remains byte-exact.
    """
    for offset, size, name in ranges:
        if offset < 0 or size <= 0 or offset + size > FLASH_BYTES:
            raise ValueError("runtime immutable range is invalid")
        actual = session.read(
            offset, size, temp / f"runtime-immutable-{name}.bin"
        )
        assert_bytes(
            actual, expected[offset:offset + size],
            f"runtime immutable {name}",
        )


def resume_verified_migration(
    session: EsptoolSession, temp: Path, expected: bytes,
    result_path: Path, binding: dict, status: str,
) -> None:
    if status != "flash_verified":
        raise ValueError("runtime migration resume requires flash_verified")
    verify_runtime_immutable_ranges(
        session, temp, expected,
        (
            (0, NVS_OFFSET, "new-boot-and-table"),
            (
                LOWER_GAP_OFFSET, KITSU_CONN_OFFSET - LOWER_GAP_OFFSET,
                "new-gaps-apps-and-spiffs",
            ),
        ),
    )
    finalize_verified_migration(
        session, result_path, binding, status
    )


def execute_stages(
    session: EsptoolSession, temp: Path, original: bytes, table: bytes,
    application: bytes, boot_app0: bytes, result_path: Path, evidence: dict,
    manifest_sha256: str,
) -> None:
    # Stage immutable private copies of every input passed to esptool. The
    # public/source paths were already read and validated; they are never used
    # again after this point, eliminating artifact TOCTOU at the table commit.
    table_stage = private_staged_bytes(
        temp / "partitions-reviewed.bin", table, "staged partition table"
    )
    app_stage = private_staged_bytes(
        temp / "firmware-reviewed.bin", application, "staged application"
    )
    boot_app0_stage = private_staged_bytes(
        temp / "boot-app0-reviewed.bin", boot_app0, "staged OTA helper"
    )
    slot = expected_slot(application)
    ff_extension = b"\xff" * NVS_EXTENSION_BYTES
    ff_lower = b"\xff" * LOWER_GAP_BYTES
    ff_upper = b"\xff" * UPPER_GAP_BYTES

    session.erase(APP1_OFFSET, APP_SLOT_BYTES)
    session.write(APP1_OFFSET, app_stage)
    assert_bytes(session.read(APP1_OFFSET, APP_SLOT_BYTES, temp / "app1.bin"), slot, "app1")

    session.erase(NVS_EXTENSION_OFFSET, NVS_EXTENSION_BYTES)
    assert_bytes(session.read(NVS_EXTENSION_OFFSET, NVS_EXTENSION_BYTES,
                              temp / "nvs-extension.bin"), ff_extension, "NVS extension")
    session.erase(OTA_DATA_OFFSET, OTA_DATA_BYTES)
    session.write(OTA_DATA_OFFSET, boot_app0_stage)
    assert_bytes(session.read(OTA_DATA_OFFSET, OTA_DATA_BYTES,
                              temp / "otadata.bin"), boot_app0, "OTA data")
    session.erase(LOWER_GAP_OFFSET, LOWER_GAP_BYTES)
    assert_bytes(session.read(LOWER_GAP_OFFSET, LOWER_GAP_BYTES,
                              temp / "lower-gap.bin"), ff_lower, "lower gap")

    session.erase(APP0_OFFSET, APP_SLOT_BYTES)
    session.write(APP0_OFFSET, app_stage)
    assert_bytes(session.read(APP0_OFFSET, APP_SLOT_BYTES, temp / "app0.bin"), slot, "app0")
    session.erase(UPPER_GAP_OFFSET, UPPER_GAP_BYTES)
    assert_bytes(session.read(UPPER_GAP_OFFSET, UPPER_GAP_BYTES,
                              temp / "upper-gap.bin"), ff_upper, "upper gap")

    # Complete precommit re-read. No partition-table write can happen unless
    # every preserve/blank/app gate succeeds in this same loader session.
    assert_bytes(
        session.read(NVS_OFFSET, LEGACY_NVS_BYTES, temp / "nvs-prefix.bin"),
        original[NVS_OFFSET:NVS_EXTENSION_OFFSET], "legacy NVS prefix",
    )
    assert_bytes(session.read(NVS_EXTENSION_OFFSET, NVS_EXTENSION_BYTES,
                              temp / "nvs-extension-final.bin"),
                 ff_extension, "NVS extension final")
    assert_bytes(session.read(OTA_DATA_OFFSET, OTA_DATA_BYTES,
                              temp / "otadata-final.bin"),
                 boot_app0, "OTA data final")
    assert_bytes(session.read(LOWER_GAP_OFFSET, LOWER_GAP_BYTES,
                              temp / "lower-gap-final.bin"),
                 ff_lower, "lower gap final")
    assert_bytes(session.read(APP0_OFFSET, APP_SLOT_BYTES,
                              temp / "app0-final.bin"), slot, "app0 final")
    assert_bytes(session.read(APP1_OFFSET, APP_SLOT_BYTES,
                              temp / "app1-final.bin"), slot, "app1 final")
    assert_bytes(session.read(UPPER_GAP_OFFSET, UPPER_GAP_BYTES,
                              temp / "upper-gap-final.bin"),
                 ff_upper, "upper gap final")
    assert_bytes(
        session.read(0, PARTITION_TABLE_OFFSET,
                     temp / "boot-prefix-precommit.bin"),
        original[:PARTITION_TABLE_OFFSET], "bootloader prefix before commit",
    )
    assert_bytes(
        session.read(PRESERVED_UPPER_OFFSET, PRESERVED_UPPER_BYTES,
                     temp / "upper-preserved.bin"),
        original[PRESERVED_UPPER_OFFSET:], "SPIFFS/connectivity/coredump",
    )
    assert_bytes(
        session.read(PARTITION_TABLE_OFFSET, PARTITION_TABLE_SECTOR_BYTES,
                     temp / "legacy-table-precommit.bin"),
        original[PARTITION_TABLE_OFFSET:
                 PARTITION_TABLE_OFFSET + PARTITION_TABLE_SECTOR_BYTES],
        "legacy partition table before commit",
    )

    # Irreversible layout commit: deliberately the final write.
    session.write(PARTITION_TABLE_OFFSET, table_stage)
    assert_bytes(
        session.read(PARTITION_TABLE_OFFSET, PARTITION_TABLE_SECTOR_BYTES,
                     temp / "new-table.bin"),
        table_sector(table), "new partition-table commit",
    )
    expected_final, _ = simulate_migration(original, table, application, boot_app0)
    final = session.read(0, FLASH_BYTES, temp / "full-final.bin")
    assert_bytes(final, expected_final, "complete post-commit flash image")
    binding = result_binding(evidence, manifest_sha256)
    if sha256_bytes(final) != binding["final_flash_sha256"]:
        raise RuntimeError("final flash readback differs from bound result")
    finalize_verified_migration(
        session, result_path, binding, "prepared"
    )


def migrate_command(args: argparse.Namespace) -> int:
    if args.confirm != MIGRATION_CONFIRMATION:
        raise ValueError(f"--confirm must equal {MIGRATION_CONFIRMATION!r}")
    (
        _, original, table_path, table, app_path, application,
        boot_app0_path, boot_app0, evidence, manifest_sha256,
    ) = migration_inputs(args)
    binding = result_binding(evidence, manifest_sha256)
    result_path, result_status = preflight_private_json_output(
        args.result, binding
    )
    if result_status != "prepared":
        raise ValueError("migrate requires a fresh prepared result; use resume")

    session = EsptoolSession(args.python, args.esptool, args.port, args.baud)
    session.validate_version()
    session.probe(args.expected_mac)
    with tempfile.TemporaryDirectory(prefix="kitsu-0203-migration-") as raw:
        temp = Path(raw)
        current = session.read(0, FLASH_BYTES, temp / "current-before-migration.bin")
        assert_bytes(current, original, "live flash versus frozen original backup")
        execute_stages(
            session, temp, original, table, application, boot_app0,
            result_path, evidence,
            manifest_sha256,
        )

    print(f"KITSU_MIGRATION_OK backup_sha256={evidence['backup_sha256']}")
    return 0


def classify_resume_table(current_sector: bytes, original: bytes, table: bytes) -> str:
    old = original[PARTITION_TABLE_OFFSET:
                   PARTITION_TABLE_OFFSET + PARTITION_TABLE_SECTOR_BYTES]
    if current_sector == old:
        return "legacy"
    if current_sector == table_sector(table):
        return "current"
    return "partial"


def resume_command(args: argparse.Namespace) -> int:
    if args.confirm != MIGRATION_CONFIRMATION:
        raise ValueError(f"--confirm must equal {MIGRATION_CONFIRMATION!r}")
    (
        _, original, table_path, table, app_path, application,
        boot_app0_path, boot_app0, evidence, manifest_sha256,
    ) = migration_inputs(args)
    binding = result_binding(evidence, manifest_sha256)
    result_path, result_status = preflight_private_json_output(
        args.result, binding
    )
    session = EsptoolSession(args.python, args.esptool, args.port, args.baud)
    session.validate_version()
    session.probe(args.expected_mac)
    with tempfile.TemporaryDirectory(prefix="kitsu-0203-resume-") as raw:
        temp = Path(raw)
        sector = session.read(
            PARTITION_TABLE_OFFSET, PARTITION_TABLE_SECTOR_BYTES,
            temp / "resume-table.bin",
        )
        state = classify_resume_table(sector, original, table)
        if state == "partial":
            raise RuntimeError(
                "partition-table sector is partial/unknown; restore the frozen backup"
            )
        if state == "current":
            expected, _ = simulate_migration(original, table, application, boot_app0)
            if result_status == "prepared":
                current = session.read(
                    0, FLASH_BYTES, temp / "resume-current-prepared.bin"
                )
                assert_bytes(current, expected, "already-committed migration")
                if sha256_bytes(current) != binding["final_flash_sha256"]:
                    raise RuntimeError("committed flash does not match bound result")
                finalize_verified_migration(
                    session, result_path, binding, result_status
                )
            else:
                resume_verified_migration(
                    session, temp, expected, result_path, binding,
                    result_status,
                )
        else:
            # Under the old table, re-running every erase/write is idempotent.
            # First prove the two protected regions still match the original
            # backup so a transitional backup can never replace authority.
            if result_status != "prepared":
                raise RuntimeError(
                    "verified/complete result cannot accompany the legacy table"
                )
            assert_bytes(
                session.read(0, PARTITION_TABLE_OFFSET,
                             temp / "resume-boot-prefix.bin"),
                original[:PARTITION_TABLE_OFFSET], "resume bootloader prefix",
            )
            assert_bytes(
                session.read(NVS_OFFSET, LEGACY_NVS_BYTES,
                             temp / "resume-nvs-prefix.bin"),
                original[NVS_OFFSET:NVS_EXTENSION_OFFSET], "resume NVS prefix",
            )
            assert_bytes(
                session.read(PRESERVED_UPPER_OFFSET, PRESERVED_UPPER_BYTES,
                             temp / "resume-upper.bin"),
                original[PRESERVED_UPPER_OFFSET:], "resume upper preserved region",
            )
            execute_stages(
                session, temp, original, table, application, boot_app0,
                result_path, evidence,
                manifest_sha256,
            )
    print(f"KITSU_RESUME_OK backup_sha256={evidence['backup_sha256']}")
    return 0


def finalize_verified_restore(
    session: EsptoolSession, result_path: Path, binding: dict, status: str,
) -> None:
    prepared = restore_result(binding, "prepared")
    verified = restore_result(binding, "flash_verified")
    if status == "prepared":
        replace_private_json(
            result_path, verified, expected_current=[prepared]
        )
        status = "flash_verified"
    elif status != "flash_verified":
        raise ValueError("restore result cannot enter the reset phase")
    session.finish()


def resume_verified_restore(
    session: EsptoolSession, temp: Path, expected: bytes,
    result_path: Path, binding: dict, status: str,
) -> None:
    if status != "flash_verified":
        raise ValueError("runtime restore resume requires flash_verified")
    verify_runtime_immutable_ranges(
        session, temp, expected,
        (
            (0, NVS_OFFSET, "legacy-boot-and-table"),
            (
                LEGACY_APP0_OFFSET, KITSU_CONN_OFFSET - LEGACY_APP0_OFFSET,
                "legacy-apps-and-spiffs",
            ),
        ),
    )
    finalize_verified_restore(
        session, result_path, binding, status
    )


def execute_restore(
    session: EsptoolSession, temp: Path, backup: bytes, result_path: Path,
    binding: dict,
) -> None:
    boot_path = temp / "restore-boot-before-table.bin"
    data_path = temp / "restore-data-after-table.bin"
    table_path = temp / "restore-old-table-last.bin"
    private_staged_bytes(
        boot_path, backup[:PARTITION_TABLE_OFFSET], "staged restore boot prefix"
    )
    private_staged_bytes(
        data_path, backup[NVS_OFFSET:], "staged restore data region"
    )
    private_staged_bytes(
        table_path, backup[PARTITION_TABLE_OFFSET:NVS_OFFSET],
        "staged restore partition-table sector",
    )

    # Invalidate the currently valid new table before restoring any legacy
    # interior. A power loss from this point remains a ROM-loader recovery
    # state; it can never boot the new layout against partially restored old
    # NVS/app bytes.
    session.erase(PARTITION_TABLE_OFFSET, PARTITION_TABLE_SECTOR_BYTES)
    assert_bytes(
        session.read(PARTITION_TABLE_OFFSET, PARTITION_TABLE_SECTOR_BYTES,
                     temp / "restore-table-invalidated.bin"),
        b"\xff" * PARTITION_TABLE_SECTOR_BYTES,
        "invalidated partition-table sector",
    )

    # Restore all non-table ranges while the partition table stays invalid.
    session.write(NVS_OFFSET, data_path)
    assert_bytes(
        session.read(NVS_OFFSET, FLASH_BYTES - NVS_OFFSET,
                     temp / "restore-data-readback.bin"),
        backup[NVS_OFFSET:], "restored NVS/apps/upper data",
    )
    session.write(0, boot_path)
    assert_bytes(
        session.read(0, PARTITION_TABLE_OFFSET,
                     temp / "restore-boot-readback.bin"),
        backup[:PARTITION_TABLE_OFFSET], "restored bootloader prefix",
    )

    # Old partition-table sector is the final restore write.
    session.write(PARTITION_TABLE_OFFSET, table_path)
    assert_bytes(
        session.read(PARTITION_TABLE_OFFSET, PARTITION_TABLE_SECTOR_BYTES,
                     temp / "restore-table-readback.bin"),
        backup[PARTITION_TABLE_OFFSET:NVS_OFFSET], "restored old table last",
    )
    restored = session.read(0, FLASH_BYTES, temp / "restore-readback.bin")
    if restored != backup:
        raise RuntimeError("full-flash restore readback does not match recovery authority")
    if sha256_bytes(restored) != binding["final_flash_sha256"]:
        raise RuntimeError("restored flash does not match bound result")
    finalize_verified_restore(
        session, result_path, binding, "prepared"
    )


def validate_restore_authority(
    backup: bytes, manifest_path: Path, expected_manifest_sha256: str,
    expected_mac: str, expected_source_app_sha256: str,
    expected_source_app_bytes: int,
) -> str:
    manifest_file = require_file(manifest_path, "frozen migration manifest")
    manifest_sha = sha256_file(manifest_file)
    if manifest_sha != normalized_sha256(
        expected_manifest_sha256, "migration manifest"
    ):
        raise ValueError("restore manifest SHA-256 is not the frozen authority")
    manifest = load_canonical_json(manifest_file)
    source_app = application_at(backup, 0x010000, 0x32F000)
    source_hash = validate_reviewed_source_application(
        source_app, expected_source_app_sha256, expected_source_app_bytes,
    )
    restore_binding = {
        "schema": SCHEMA,
        "source_firmware_version": SOURCE_FIRMWARE_VERSION,
        "backup_sha256": sha256_bytes(backup),
        "target_mac": normalized_mac(expected_mac),
        "legacy_partition_table_sha256": sha256_bytes(
            backup[PARTITION_TABLE_OFFSET:
                   PARTITION_TABLE_OFFSET + PARTITION_TABLE_BYTES]
        ),
        "legacy_app0_sha256": source_hash,
        "legacy_app0_bytes": len(source_app),
        "reviewed_source_app0_sha256": source_hash,
        "reviewed_source_app0_bytes": expected_source_app_bytes,
        "redundant_backup_verified": True,
    }
    for key, expected in restore_binding.items():
        if manifest.get(key) != expected:
            raise ValueError(
                f"frozen migration manifest does not bind restore field {key}"
            )
    return manifest_sha


def restore_command(args: argparse.Namespace) -> int:
    backup_path, backup = verified_backup_pair(args.backup, args.backup_copy)
    actual_sha = sha256_file(backup_path)
    expected_confirmation = RESTORE_CONFIRMATION_PREFIX + actual_sha
    if args.confirm != expected_confirmation:
        raise ValueError("restore confirmation must bind the exact backup SHA-256")
    parse_partition_table(
        backup[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + PARTITION_TABLE_BYTES],
        OLD_LAYOUT,
    )
    validate_legacy_nvs_prefix(backup[NVS_OFFSET:NVS_EXTENSION_OFFSET])
    manifest_sha = validate_restore_authority(
        backup, args.manifest, args.expected_manifest_sha256,
        args.expected_mac, args.expected_source_app_sha256,
        args.expected_source_app_bytes,
    )
    binding = {
        "target_mac": normalized_mac(args.expected_mac),
        "backup_sha256": actual_sha,
        "manifest_sha256": manifest_sha,
        "final_flash_sha256": actual_sha,
    }
    result_path, result_status = preflight_restore_result(args.result, binding)
    session = EsptoolSession(args.python, args.esptool, args.port, args.baud)
    session.validate_version()
    session.probe(args.expected_mac)
    with tempfile.TemporaryDirectory(prefix="kitsu-0203-restore-") as raw:
        temp = Path(raw)
        if result_status == "prepared":
            execute_restore(
                session, temp, backup, result_path, binding
            )
        else:
            resume_verified_restore(
                session, temp, backup, result_path, binding, result_status
            )
    print(f"KITSU_RESTORE_OK backup_sha256={actual_sha}")
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)

    oracle = commands.add_parser(
        "oracle-record",
        help="bind a pinned-IDF 4.4.7 host-mount result to a captured NVS prefix",
    )
    oracle.add_argument("--legacy-nvs", type=Path, required=True)
    oracle.add_argument("--idf-source-archive", type=Path, required=True)
    oracle.add_argument("--oracle-output", type=Path, required=True)
    oracle.add_argument("--oracle-binary", type=Path, required=True)
    oracle.add_argument("--oracle-build-log-a", type=Path, required=True)
    oracle.add_argument("--oracle-build-log-b", type=Path, required=True)
    oracle.add_argument("--evidence", type=Path, required=True)
    oracle.set_defaults(callback=oracle_record_command)

    audit = commands.add_parser("audit", help="simulate and prove the migration offline")
    audit.add_argument("--backup", type=Path, required=True)
    audit.add_argument("--backup-copy", type=Path, required=True)
    audit.add_argument("--partitions", type=Path, required=True)
    audit.add_argument("--application", type=Path, required=True)
    audit.add_argument("--otadata", type=Path, required=True)
    audit.add_argument("--expected-mac", required=True)
    audit.add_argument("--expected-source-app-sha256", required=True)
    audit.add_argument("--expected-application-sha256", required=True)
    audit.add_argument("--expected-source-app-bytes", type=int, required=True)
    audit.add_argument("--expected-application-bytes", type=int, required=True)
    audit.add_argument("--nvs-oracle-evidence", type=Path, required=True)
    audit.add_argument(
        "--expected-nvs-oracle-evidence-sha256", required=True
    )
    audit.add_argument("--evidence", type=Path)
    audit.set_defaults(callback=audit_command)

    def serial_arguments(command: argparse.ArgumentParser) -> None:
        command.add_argument("--port", required=True)
        command.add_argument("--baud", type=int, default=460800)
        command.add_argument("--python", type=Path, required=True)
        command.add_argument("--esptool", type=Path, required=True)
        command.add_argument("--expected-mac", required=True)

    capture = commands.add_parser(
        "capture", help="read and verify the original flash twice without writing"
    )
    serial_arguments(capture)
    capture.add_argument("--backup", type=Path, required=True)
    capture.add_argument("--backup-copy", type=Path, required=True)
    capture.add_argument("--expected-source-app-sha256", required=True)
    capture.add_argument("--expected-source-app-bytes", type=int, required=True)
    capture.set_defaults(callback=capture_command)

    def migration_arguments(command: argparse.ArgumentParser) -> None:
        serial_arguments(command)
        command.add_argument("--partitions", type=Path, required=True)
        command.add_argument("--application", type=Path, required=True)
        command.add_argument("--otadata", type=Path, required=True)
        command.add_argument("--backup", type=Path, required=True)
        command.add_argument("--backup-copy", type=Path, required=True)
        command.add_argument("--manifest", type=Path, required=True)
        command.add_argument("--result", type=Path, required=True)
        command.add_argument("--confirm", required=True)
        command.add_argument("--expected-source-app-sha256", required=True)
        command.add_argument("--expected-application-sha256", required=True)
        command.add_argument("--expected-manifest-sha256", required=True)
        command.add_argument("--expected-source-app-bytes", type=int, required=True)
        command.add_argument("--expected-application-bytes", type=int, required=True)
        command.add_argument("--nvs-oracle-evidence", type=Path, required=True)
        command.add_argument(
            "--expected-nvs-oracle-evidence-sha256", required=True
        )

    migrate = commands.add_parser("migrate", help="execute the table-last serial migration")
    migration_arguments(migrate)
    migrate.set_defaults(callback=migrate_command)

    resume = commands.add_parser(
        "resume", help="resume from the original backup without recapturing state"
    )
    migration_arguments(resume)
    resume.set_defaults(callback=resume_command)

    restore = commands.add_parser("restore", help="restore an exact 8 MiB recovery backup")
    serial_arguments(restore)
    restore.add_argument("--backup", type=Path, required=True)
    restore.add_argument("--backup-copy", type=Path, required=True)
    restore.add_argument("--manifest", type=Path, required=True)
    restore.add_argument("--expected-manifest-sha256", required=True)
    restore.add_argument("--expected-source-app-sha256", required=True)
    restore.add_argument("--expected-source-app-bytes", type=int, required=True)
    restore.add_argument("--result", type=Path, required=True)
    restore.add_argument("--confirm", required=True)
    restore.set_defaults(callback=restore_command)
    return result


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        return int(args.callback(args))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"KITSU_MIGRATION_ERROR {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
