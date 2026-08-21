#!/usr/bin/env python3
"""Assemble an offline, device-bound Kitsu production firmware bundle.

The tool never receives a Secure Boot private key. It accepts only a completed
three-role signing stage, verifies every signature/public-key digest and the
immutable candidate hashes, creates address-bound AES-XTS-256 recovery images,
validates the compiled partition table byte-for-byte against the reviewed CSV
semantics, and commits the bundle atomically. It never opens a serial port and
never invokes esptool or espefuse.
"""

from __future__ import annotations

import argparse
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
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA = "kitsu.firmware-production-bundle.v1"
ARCHIVED_REFERENCE_ONLY = True
WITHDRAWN_MESSAGE = (
    "KITSU_WITHDRAWN_NOT_AUTHORIZED: the secure-boot/eFuse production ceremony "
    "was withdrawn by owner decision. Use tools/package_kitsu_reflashable.py. "
    "This implementation remains only as forensic reference."
)
SIGNING_STAGE_SCHEMA = "kitsu.secure-boot-signing-stage.v1"
PRIMARY_SIGNING_EVIDENCE_SCHEMA = "kitsu.primary-precomputed-signatures.v1"
PRIMARY_SIGNING_REQUEST_SCHEMA = "kitsu.primary-signing-request.v1"
OFFLINE_SIGNING_EVIDENCE_SCHEMA = "kitsu.offline-precomputed-signatures.v1"
OFFLINE_SIGNING_REQUEST_SCHEMA = "kitsu.offline-signing-request.v1"
SIGNING_ROLES = ("primary", "recovery", "rotation")
SIGNING_BLOCKS = ("BLOCK_KEY0", "BLOCK_KEY4", "BLOCK_KEY5")
SIGNING_PURPOSES = (
    "SECURE_BOOT_DIGEST0",
    "SECURE_BOOT_DIGEST1",
    "SECURE_BOOT_DIGEST2",
)
BOOTLOADER_OFFSET = 0x000000
PARTITIONS_OFFSET = 0x008000
SECURE_BOOT_V2_SIGNATURE_SECTOR_BYTES = 0x001000
UNSIGNED_BOOTLOADER_MAX_BYTES = (
    PARTITIONS_OFFSET - SECURE_BOOT_V2_SIGNATURE_SECTOR_BYTES
)
SIGNED_BOOTLOADER_MAX_BYTES = PARTITIONS_OFFSET
OTA_DATA_OFFSET = 0x00E000
APP0_OFFSET = 0x010000
APP1_OFFSET = 0x340000
LEGACY_NVS_OFFSET = 0x009000
PACK_OFFSET = 0x670000
CONNECTION_OFFSET = 0x7B0000
PARTITION_BINARY_BYTES = 0x0C00
PARTITION_ENTRY_BYTES = 32
PARTITION_MAGIC = b"\xaa\x50"
PARTITION_MD5_MAGIC = b"\xeb\xeb"
TOKEN_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]{0,63}\Z")
HEX_SHA256_PATTERN = re.compile(r"[0-9A-F]{64}\Z")
SIGNATURE_DIGEST_PATTERN = re.compile(
    r"Public key digest for block (\d+): "
    r"((?:[0-9a-fA-F]{2} ){31}[0-9a-fA-F]{2})",
)
SIGNATURE_TYPE_PATTERN = re.compile(
    r"Signature block (\d+) is valid \(([A-Za-z0-9_-]+)\)\."
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


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def require_file(path: Path, description: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        raise SystemExit(f"{description} is missing: {resolved}")
    return resolved


def require_token(value: str, description: str) -> str:
    if not isinstance(value, str) or not TOKEN_PATTERN.fullmatch(value):
        raise SystemExit(
            f"{description} must be 1..64 safe ASCII characters "
            "(letters, digits, dot, underscore, plus, or hyphen)"
        )
    return value


def run_capture(command: list[str], operation: str) -> str:
    # Capture output because espsecure diagnostics can include private-key
    # custody paths. A reviewed operator may rerun a failed command locally.
    completed = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise SystemExit(
            f"espsecure {operation} failed with exit code "
            f"{completed.returncode}; no bundle was committed"
        )
    return completed.stdout + completed.stderr


def run(command: list[str], operation: str) -> None:
    run_capture(command, operation)


def espsecure_command(tool: Path) -> list[str]:
    return [sys.executable, str(tool)] if tool.suffix.lower() == ".py" else [str(tool)]


def public_key_digest(tool: Path, key: Path, output: Path) -> str:
    run(
        espsecure_command(tool)
        + [
            "digest_sbv2_public_key",
            "--keyfile",
            str(key),
            "--output",
            str(output),
        ],
        "public-key digest",
    )
    if output.stat().st_size != 32:
        raise SystemExit("espsecure produced an invalid Secure Boot V2 digest")
    return output.read_bytes().hex().upper()


def signature_block_digests(tool: Path, image: Path) -> list[str]:
    output = run_capture(
        espsecure_command(tool) + ["signature_info_v2", str(image)],
        "signature-block inspection",
    )
    types = SIGNATURE_TYPE_PATTERN.findall(output)
    records = SIGNATURE_DIGEST_PATTERN.findall(output)
    if len(types) != len(records):
        raise SystemExit(
            "espsecure signature-block report is internally inconsistent: "
            + output.strip()
        )
    digests: list[str] = []
    for expected_index, ((type_index, scheme), (index, encoded_digest)) in enumerate(
        zip(types, records)
    ):
        if int(type_index) != expected_index or int(index) != expected_index:
            raise SystemExit("Secure Boot V2 signature blocks are not contiguous")
        if scheme != "RSA":
            raise SystemExit("production Secure Boot signatures must use RSA-3072")
        digest = encoded_digest.replace(" ", "").upper()
        if not HEX_SHA256_PATTERN.fullmatch(digest):
            raise SystemExit("espsecure reported an invalid public-key digest")
        digests.append(digest)
    return digests


def verify_signature(tool: Path, public_key: Path, image: Path) -> None:
    run(
        espsecure_command(tool)
        + [
            "verify_signature",
            "--version",
            "2",
            "--keyfile",
            str(public_key),
            str(image),
        ],
        "signature verification",
    )


def encrypt_and_verify(
    tool: Path, key: Path, plaintext: Path, address: int, output: Path
) -> None:
    run(
        espsecure_command(tool)
        + [
            "encrypt_flash_data",
            "--aes_xts",
            "--keyfile",
            str(key),
            "--address",
            hex(address),
            "--output",
            str(output),
            str(plaintext),
        ],
        "AES-XTS encryption",
    )
    with tempfile.TemporaryDirectory(prefix="kitsu-production-verify-") as temp:
        recovered = Path(temp) / "recovered.bin"
        run(
            espsecure_command(tool)
            + [
                "decrypt_flash_data",
                "--aes_xts",
                "--keyfile",
                str(key),
                "--address",
                hex(address),
                "--output",
                str(recovered),
                str(output),
            ],
            "AES-XTS readback verification",
        )
        if sha256(recovered) != sha256(plaintext):
            raise SystemExit(f"encrypted-image readback mismatch at {hex(address)}")


def parse_number(value: str) -> int:
    return int(value.strip(), 0)


def parse_partition_code(value: str, names: dict[str, int], description: str) -> int:
    normalized = value.strip().lower()
    if normalized in names:
        return names[normalized]
    try:
        code = parse_number(normalized)
    except ValueError as error:
        raise SystemExit(f"unknown {description}: {value}") from error
    if not 0 <= code <= 0xFF:
        raise SystemExit(f"{description} is outside one byte: {value}")
    return code


def expected_partition_records(layout: Path) -> list[dict[str, Any]]:
    expected: list[dict[str, Any]] = []
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
            type_code = parse_partition_code(
                row[1], PARTITION_TYPES, "partition type"
            )
            subtype_names = APP_SUBTYPES if type_code == 0x00 else DATA_SUBTYPES
            subtype_code = parse_partition_code(
                row[2], subtype_names, "partition subtype"
            )
            flags = 0
            for cell in row[5:]:
                for name in cell.split(":"):
                    normalized = name.strip().lower()
                    if not normalized:
                        continue
                    if normalized not in PARTITION_FLAGS:
                        raise SystemExit(f"unknown partition flag: {normalized}")
                    flags |= PARTITION_FLAGS[normalized]
            expected.append(
                {
                    "label": label,
                    "type": type_code,
                    "subtype": subtype_code,
                    "offset": parse_number(row[3]),
                    "size": parse_number(row[4]),
                    "flags": flags,
                }
            )
    return expected


def validate_partition_table(binary: Path, layout: Path) -> dict[str, Any]:
    data = binary.read_bytes()
    if len(data) != PARTITION_BINARY_BYTES:
        raise SystemExit(
            f"partitions.bin must be exactly 0x{PARTITION_BINARY_BYTES:X} bytes"
        )
    actual: list[dict[str, Any]] = []
    cursor = 0
    table_md5 = ""
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
            table_md5 = expected_md5.hex().upper()
            cursor += PARTITION_ENTRY_BYTES
            if any(value != 0xFF for value in data[cursor:]):
                raise SystemExit("partition table contains data after its MD5 record")
            break
        if entry[:2] != PARTITION_MAGIC:
            raise SystemExit(f"invalid partition-table magic at 0x{cursor:X}")
        _, type_code, subtype, offset, size, label_raw, flags = struct.unpack(
            "<2sBBII16sI", entry
        )
        if b"\x00" not in label_raw:
            raise SystemExit(f"unterminated partition label at 0x{cursor:X}")
        label_bytes, padding = label_raw.split(b"\x00", 1)
        if any(padding):
            raise SystemExit(f"nonzero partition-label padding at 0x{cursor:X}")
        try:
            label = label_bytes.decode("ascii")
        except UnicodeDecodeError as error:
            raise SystemExit(f"non-ASCII partition label at 0x{cursor:X}") from error
        if flags & ~0x03:
            raise SystemExit(f"unknown partition flags for {label}: 0x{flags:X}")
        actual.append(
            {
                "label": label,
                "type": type_code,
                "subtype": subtype,
                "offset": offset,
                "size": size,
                "flags": flags,
            }
        )
        cursor += PARTITION_ENTRY_BYTES
    if not table_md5:
        raise SystemExit("partition table is missing its verified MD5 record")
    expected = expected_partition_records(layout)
    if actual != expected:
        raise SystemExit(
            "compiled partition table does not exactly match the reviewed "
            f"layout; expected={expected!r}, actual={actual!r}"
        )
    return {
        "verified_against_reviewed_csv": True,
        "binary_bytes": len(data),
        "entry_count": len(actual),
        "embedded_md5": table_md5,
    }


def image_record(
    path: Path,
    role: str,
    partition: str,
    offset: int,
    *,
    signed: bool,
    encrypted: bool,
    source: Path | None = None,
    signature_blocks: int = 0,
) -> dict[str, Any]:
    record: dict[str, Any] = {
        "file": path.name,
        "role": role,
        "partition": partition,
        "offset": offset,
        "offset_hex": f"0x{offset:06X}",
        "bytes": path.stat().st_size,
        "sha256": sha256(path),
        "secure_boot_v2_signed": signed,
        "secure_boot_v2_signature_blocks": signature_blocks if signed else 0,
        "flash_encrypted": encrypted,
    }
    if source:
        record["source_sha256"] = sha256(source)
    return record


def candidate_record(path: Path, role: str) -> dict[str, Any]:
    return {
        "file": path.name,
        "role": role,
        "bytes": path.stat().st_size,
        "sha256": sha256(path),
    }


def copy_image(source: Path, destination: Path) -> None:
    shutil.copyfile(source, destination)


def write_checksum_index(root: Path) -> None:
    files = sorted(
        path for path in root.rglob("*") if path.is_file() and path.name != "SHA256SUMS.txt"
    )
    lines = [f"{sha256(path)}  {path.relative_to(root).as_posix()}" for path in files]
    (root / "SHA256SUMS.txt").write_text("\n".join(lines) + "\n", encoding="ascii")


def require_exact_keys(
    value: object, expected: set[str], description: str
) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != expected:
        raise SystemExit(f"{description} has an invalid schema")
    return value


def require_sha256(value: object, description: str) -> str:
    if not isinstance(value, str) or not HEX_SHA256_PATTERN.fullmatch(value):
        raise SystemExit(f"{description} is not an uppercase SHA-256 value")
    return value


def stage_file(root: Path, value: object, description: str) -> Path:
    if (
        not isinstance(value, str)
        or not value
        or "\\" in value
        or value.startswith("/")
        or any(part in ("", ".", "..") for part in value.split("/"))
    ):
        raise SystemExit(f"{description} is not a canonical relative path")
    resolved = (root / Path(*value.split("/"))).resolve()
    try:
        resolved.relative_to(root)
    except ValueError as error:
        raise SystemExit(f"{description} escapes the signing stage") from error
    return require_file(resolved, description)


def validate_primary_signing_evidence(
    evidence_path: Path,
    *,
    public_key: Path,
    signer_ref: str,
    device_id: str,
    firmware_version: str,
    candidate_inputs: list[dict[str, Any]],
    evidence_root: Path | None = None,
) -> dict[str, Any]:
    """Validate the root-only systemd-credential signer's public evidence.

    The evidence directory contains only the two precomputed RSA-PSS
    signatures and the JSON evidence record.  It never contains key material.
    """

    path = require_file(evidence_path, "primary precomputed-signing evidence")
    root = evidence_root.resolve() if evidence_root is not None else path.parent.resolve()
    try:
        path.resolve().relative_to(root)
    except ValueError as error:
        raise SystemExit("primary signing evidence escapes its custody directory") from error
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError, OSError) as error:
        raise SystemExit("primary signing evidence is not valid UTF-8 JSON") from error
    evidence = require_exact_keys(
        value,
        {
            "schema",
            "created_at",
            "request",
            "signer",
            "public_identity",
            "signatures",
            "controls",
            "warnings",
        },
        "primary signing evidence",
    )
    if evidence["schema"] != PRIMARY_SIGNING_EVIDENCE_SCHEMA:
        raise SystemExit("primary signing evidence schema is unsupported")
    if not isinstance(evidence["created_at"], str) or not evidence["created_at"].endswith("Z"):
        raise SystemExit("primary signing evidence timestamp is invalid")

    request = require_exact_keys(
        evidence["request"],
        {"schema", "request_id", "device_id", "firmware_version", "candidate_inputs"},
        "primary signing evidence request",
    )
    if request["schema"] != PRIMARY_SIGNING_REQUEST_SCHEMA:
        raise SystemExit("primary signing evidence request schema is unsupported")
    try:
        request_id = uuid.UUID(request["request_id"])
    except (ValueError, TypeError, AttributeError) as error:
        raise SystemExit("primary signing evidence request ID is invalid") from error
    if request_id.int == 0 or str(request_id) != request["request_id"]:
        raise SystemExit("primary signing evidence request ID is not canonical")
    if request["device_id"] != device_id or request["firmware_version"] != firmware_version:
        raise SystemExit("primary signing evidence is bound to another release")
    if request["candidate_inputs"] != candidate_inputs:
        raise SystemExit("primary signing evidence candidate binding differs")

    signer = require_exact_keys(
        evidence["signer"],
        {
            "signer_ref",
            "host",
            "credential_name",
            "delivery",
            "private_key_exported",
            "plaintext_persisted",
        },
        "primary signing evidence signer",
    )
    if (
        signer["signer_ref"] != signer_ref
        or not signer_ref.startswith("tpm2-systemd-cred:")
        or signer["credential_name"] != "firmware-signing-primary-key"
        or signer["delivery"] != "systemd-LoadCredentialEncrypted"
        or signer["private_key_exported"] is not False
        or signer["plaintext_persisted"] is not False
    ):
        raise SystemExit("primary signing evidence signer claim is invalid")
    require_token(signer["host"], "primary signer host")

    identity = require_exact_keys(
        evidence["public_identity"],
        {
            "algorithm",
            "bits",
            "public_key_sha256",
            "spki_sha256",
            "credential_matches_pinned_public",
        },
        "primary signing public identity",
    )
    if (
        identity["algorithm"] != "RSA"
        or identity["bits"] != 3072
        or identity["credential_matches_pinned_public"] is not True
        or identity["public_key_sha256"] != sha256(public_key)
    ):
        raise SystemExit("primary signing evidence public identity is invalid")
    require_sha256(identity["spki_sha256"], "primary public SPKI checksum")
    require_sha256(identity["public_key_sha256"], "primary public-key checksum")

    signatures = evidence["signatures"]
    if not isinstance(signatures, list) or len(signatures) != 2:
        raise SystemExit("primary signing evidence must contain exactly two signatures")
    signature_paths: dict[str, Path] = {}
    for record_value, candidate in zip(signatures, candidate_inputs):
        record = require_exact_keys(
            record_value,
            {
                "role",
                "file",
                "bytes",
                "sha256",
                "input_file",
                "input_sha256",
                "scheme",
                "mgf1",
                "salt_length",
            },
            "primary precomputed signature",
        )
        role = candidate["role"]
        expected_file = f"{role}.sig"
        if (
            record["role"] != role
            or record["file"] != expected_file
            or record["bytes"] != 384
            or record["input_file"] != candidate["file"]
            or record["input_sha256"] != candidate["sha256"]
            or record["scheme"] != "RSA-PSS-SHA256"
            or record["mgf1"] != "SHA256"
            or record["salt_length"] != 32
        ):
            raise SystemExit("primary precomputed signature contract is invalid")
        signature = stage_file(root, record["file"], "primary precomputed signature")
        if signature.parent != path.parent or signature.stat().st_size != 384:
            raise SystemExit("primary precomputed signature location or size is invalid")
        if sha256(signature) != require_sha256(record["sha256"], "signature checksum"):
            raise SystemExit("primary precomputed signature checksum mismatch")
        signature_paths[role] = signature

    controls = require_exact_keys(
        evidence["controls"],
        {
            "effective_uid",
            "credential_filesystem",
            "network_namespace_private",
            "coredumps_disabled",
            "output_atomic",
            "credential_cleanup",
        },
        "primary signing controls",
    )
    if controls != {
        "effective_uid": 0,
        "credential_filesystem": controls["credential_filesystem"],
        "network_namespace_private": True,
        "coredumps_disabled": True,
        "output_atomic": True,
        "credential_cleanup": "systemd-managed-after-oneshot",
    } or controls["credential_filesystem"] not in ("ramfs", "tmpfs"):
        raise SystemExit("primary signing evidence lacks required one-shot controls")
    if evidence["warnings"] != [
        "SYSTEMD_CREDENTIAL_DECRYPTED_IN_RAM_FOR_ONE_SHOT",
        "NO_HARDWARE_NONEXPORTABLE_SIGNING_CLAIM",
        "NO_FLASH_OR_EFUSE_ACTION",
    ]:
        raise SystemExit("primary signing evidence warning set is invalid")
    actual_files = {
        item.name for item in path.parent.iterdir() if item.is_file()
    }
    if actual_files != {"evidence.json", "bootloader.sig", "application.sig"}:
        raise SystemExit("primary signing evidence directory contains unexpected files")
    return {"document": evidence, "path": path, "signature_paths": signature_paths}


def validate_offline_signing_evidence(
    evidence_path: Path,
    *,
    role: str,
    public_key: Path,
    signer_ref: str,
    device_id: str,
    firmware_version: str,
    candidate_inputs: list[dict[str, Any]],
    evidence_root: Path | None = None,
) -> dict[str, Any]:
    """Validate one owner-isolated DPAPI/CMS offline signing result."""

    if role not in ("recovery", "rotation"):
        raise SystemExit("offline signing evidence role is invalid")
    path = require_file(evidence_path, "offline precomputed-signing evidence")
    root = evidence_root.resolve() if evidence_root is not None else path.parent.resolve()
    try:
        path.resolve().relative_to(root)
    except ValueError as error:
        raise SystemExit("offline signing evidence escapes its custody directory") from error
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError, OSError) as error:
        raise SystemExit("offline signing evidence is not valid UTF-8 JSON") from error
    evidence = require_exact_keys(
        value,
        {
            "schema",
            "created_at",
            "request",
            "custody",
            "signing_identity",
            "signatures",
            "controls",
            "warnings",
        },
        "offline signing evidence",
    )
    if evidence["schema"] != OFFLINE_SIGNING_EVIDENCE_SCHEMA:
        raise SystemExit("offline signing evidence schema is unsupported")
    if not isinstance(evidence["created_at"], str) or not evidence["created_at"].endswith("Z"):
        raise SystemExit("offline signing evidence timestamp is invalid")

    request = require_exact_keys(
        evidence["request"],
        {
            "schema",
            "request_id",
            "role",
            "device_id",
            "firmware_version",
            "signer_ref",
            "candidate_inputs",
        },
        "offline signing evidence request",
    )
    if request["schema"] != OFFLINE_SIGNING_REQUEST_SCHEMA:
        raise SystemExit("offline signing evidence request schema is unsupported")
    try:
        request_id = uuid.UUID(request["request_id"])
    except (ValueError, TypeError, AttributeError) as error:
        raise SystemExit("offline signing evidence request ID is invalid") from error
    if request_id.int == 0 or str(request_id) != request["request_id"]:
        raise SystemExit("offline signing evidence request ID is not canonical")
    if (
        request["role"] != role
        or request["device_id"] != device_id
        or request["firmware_version"] != firmware_version
        or request["signer_ref"] != signer_ref
        or request["candidate_inputs"] != candidate_inputs
    ):
        raise SystemExit("offline signing evidence is bound to another release or role")

    custody_id = f"kitsu-owner-firmware-{role}-v1"
    custody = require_exact_keys(
        evidence["custody"],
        {
            "role",
            "custody_id",
            "signer_ref",
            "recipient_certificate_file",
            "recipient_certificate_sha256",
            "dpapi_scope",
            "process_role_isolated",
        },
        "offline signing custody record",
    )
    if custody != {
        "role": role,
        "custody_id": custody_id,
        "signer_ref": signer_ref,
        "recipient_certificate_file": f"firmware-{role}.recipient-cert.pem",
        "recipient_certificate_sha256": custody["recipient_certificate_sha256"],
        "dpapi_scope": "CurrentUser",
        "process_role_isolated": True,
    } or signer_ref != f"offline-escrow:{custody_id}":
        raise SystemExit("offline signing custody claim is invalid")
    require_sha256(
        custody["recipient_certificate_sha256"],
        "offline recipient-certificate checksum",
    )

    identity = require_exact_keys(
        evidence["signing_identity"],
        {
            "algorithm",
            "bits",
            "public_key_file",
            "public_key_sha256",
            "spki_sha256",
            "private_key_matches_public",
        },
        "offline signing public identity",
    )
    if (
        identity["algorithm"] != "RSA"
        or identity["bits"] != 3072
        or identity["public_key_file"] != f"{role}.pem"
        or identity["public_key_sha256"] != sha256(public_key)
        or identity["private_key_matches_public"] is not True
    ):
        raise SystemExit("offline signing public identity is invalid")
    require_sha256(identity["public_key_sha256"], "offline public-key checksum")
    require_sha256(identity["spki_sha256"], "offline public SPKI checksum")

    signatures = evidence["signatures"]
    if not isinstance(signatures, list) or len(signatures) != 2:
        raise SystemExit("offline signing evidence must contain exactly two signatures")
    signature_paths: dict[str, Path] = {}
    for record_value, candidate in zip(signatures, candidate_inputs):
        record = require_exact_keys(
            record_value,
            {
                "role",
                "file",
                "bytes",
                "sha256",
                "input_file",
                "input_sha256",
                "scheme",
                "mgf1",
                "salt_length",
            },
            "offline precomputed signature",
        )
        image_role = candidate["role"]
        expected_file = f"{image_role}.sig"
        if (
            record["role"] != image_role
            or record["file"] != expected_file
            or record["bytes"] != 384
            or record["input_file"] != candidate["file"]
            or record["input_sha256"] != candidate["sha256"]
            or record["scheme"] != "RSA-PSS-SHA256"
            or record["mgf1"] != "SHA256"
            or record["salt_length"] != 32
        ):
            raise SystemExit("offline precomputed signature contract is invalid")
        signature = stage_file(root, record["file"], "offline precomputed signature")
        if signature.parent != path.parent or signature.stat().st_size != 384:
            raise SystemExit("offline precomputed signature location or size is invalid")
        if sha256(signature) != require_sha256(record["sha256"], "signature checksum"):
            raise SystemExit("offline precomputed signature checksum mismatch")
        signature_paths[image_role] = signature

    controls = require_exact_keys(
        evidence["controls"],
        {
            "maximum_private_keys_in_process",
            "cms_content_encryption",
            "dpapi_unsealed_in_memory",
            "signing_key_plaintext_in_memory",
            "plaintext_private_key_file",
            "plaintext_temporary_file",
            "output_atomic",
            "private_buffers_zeroed",
        },
        "offline signing controls",
    )
    if controls != {
        "maximum_private_keys_in_process": 1,
        "cms_content_encryption": "AES-256-CBC",
        "dpapi_unsealed_in_memory": True,
        "signing_key_plaintext_in_memory": True,
        "plaintext_private_key_file": False,
        "plaintext_temporary_file": False,
        "output_atomic": True,
        "private_buffers_zeroed": True,
    }:
        raise SystemExit("offline signing evidence lacks required memory/atomic controls")
    if evidence["warnings"] != [
        "OWNER_DPAPI_CURRENTUSER_BOUND",
        "NO_PLAINTEXT_SIGNING_KEY_FILE",
        "NO_FLASH_OR_EFUSE_ACTION",
    ]:
        raise SystemExit("offline signing evidence warning set is invalid")
    actual_files = {item.name for item in path.parent.iterdir() if item.is_file()}
    if actual_files != {"evidence.json", "bootloader.sig", "application.sig"}:
        raise SystemExit("offline signing evidence directory contains unexpected files")
    return {"document": evidence, "path": path, "signature_paths": signature_paths}


def validate_checksum_index(root: Path, expected: set[str]) -> None:
    index = require_file(root / "SHA256SUMS.txt", "signing-stage checksum index")
    records: dict[str, str] = {}
    try:
        lines = index.read_text(encoding="ascii").splitlines()
    except (UnicodeDecodeError, OSError) as error:
        raise SystemExit("signing-stage checksum index is not canonical ASCII") from error
    for line in lines:
        parts = line.split("  ", 1)
        if len(parts) != 2:
            raise SystemExit("signing-stage checksum index has a malformed record")
        digest, relative = parts
        require_sha256(digest, "signing-stage checksum")
        path = stage_file(root, relative, "signing-stage checksummed file")
        if relative in records:
            raise SystemExit("signing-stage checksum index has a duplicate file")
        if sha256(path) != digest:
            raise SystemExit(f"signing-stage checksum mismatch: {relative}")
        records[relative] = digest
    if set(records) != expected:
        raise SystemExit("signing-stage checksum index does not cover the exact stage")


def validate_signing_stage(
    stage_dir: Path,
    tool: Path,
    *,
    expected_device_id: str | None = None,
    expected_firmware_version: str | None = None,
    expected_role: str | None = None,
) -> dict[str, Any]:
    root = stage_dir.expanduser().resolve()
    if not root.is_dir():
        raise SystemExit(f"signing stage directory is missing: {root}")
    manifest_path = require_file(root / "signing-stage.json", "signing-stage manifest")
    try:
        manifest_value = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError, OSError) as error:
        raise SystemExit("signing-stage manifest is not valid UTF-8 JSON") from error
    manifest = require_exact_keys(
        manifest_value,
        {
            "schema",
            "created_at",
            "role",
            "stage_number",
            "signature_blocks",
            "device_id",
            "firmware_version",
            "input",
            "candidate_inputs",
            "images",
            "secure_boot_public_keys",
            "custody",
            "checksum_index",
        },
        "signing-stage manifest",
    )
    if manifest["schema"] != SIGNING_STAGE_SCHEMA:
        raise SystemExit("signing-stage manifest schema is not supported")
    if manifest["checksum_index"] != "SHA256SUMS.txt":
        raise SystemExit("signing-stage checksum index name is not canonical")
    role = manifest["role"]
    if role not in SIGNING_ROLES:
        raise SystemExit("signing-stage role is invalid")
    stage_number = SIGNING_ROLES.index(role) + 1
    if (
        manifest["stage_number"] != stage_number
        or manifest["signature_blocks"] != stage_number
    ):
        raise SystemExit("signing-stage sequence does not match its role")
    if expected_role is not None and role != expected_role:
        raise SystemExit(f"signing stage must be the {expected_role} stage")
    device_id = require_token(manifest["device_id"], "signing-stage device ID")
    version = require_token(
        manifest["firmware_version"], "signing-stage firmware version"
    )
    if expected_device_id is not None and device_id != expected_device_id:
        raise SystemExit("signing-stage device ID does not match the bundle")
    if expected_firmware_version is not None and version != expected_firmware_version:
        raise SystemExit("signing-stage firmware version does not match the bundle")

    input_record = require_exact_keys(
        manifest["input"], {"kind", "prior_stage_manifest_sha256"}, "stage input"
    )
    if stage_number == 1:
        if input_record != {"kind": "candidate", "prior_stage_manifest_sha256": None}:
            raise SystemExit("primary signing stage has an invalid input record")
    else:
        if input_record["kind"] != "prior_signing_stage":
            raise SystemExit("appended signing stage lacks a prior-stage input")
        require_sha256(
            input_record["prior_stage_manifest_sha256"],
            "prior signing-stage manifest checksum",
        )

    candidates = manifest["candidate_inputs"]
    if not isinstance(candidates, list) or len(candidates) != 2:
        raise SystemExit("signing stage must bind exactly two unsigned candidates")
    expected_candidates = (("bootloader", "bootloader.bin"), ("application", "firmware.bin"))
    candidate_hashes: dict[str, str] = {}
    candidate_sizes: dict[str, int] = {}
    for record_value, (expected_candidate_role, expected_file) in zip(
        candidates, expected_candidates
    ):
        record = require_exact_keys(
            record_value, {"file", "role", "bytes", "sha256"}, "candidate record"
        )
        if record["role"] != expected_candidate_role or record["file"] != expected_file:
            raise SystemExit("signing-stage candidate order or identity is invalid")
        if not isinstance(record["bytes"], int) or record["bytes"] <= 0:
            raise SystemExit("signing-stage candidate size is invalid")
        candidate_sizes[expected_candidate_role] = record["bytes"]
        candidate_hashes[expected_candidate_role] = require_sha256(
            record["sha256"], "candidate checksum"
        )
    if (
        candidate_sizes["bootloader"] > UNSIGNED_BOOTLOADER_MAX_BYTES
        or candidate_sizes["bootloader"] % SECURE_BOOT_V2_SIGNATURE_SECTOR_BYTES
        != 0
    ):
        raise SystemExit(
            "unsigned bootloader must be 4 KiB aligned and leave one complete "
            "Secure Boot V2 signature sector before the partition table"
        )
    if candidate_sizes["application"] % 0x10000 != 0:
        raise SystemExit("unsigned application lacks 64 KiB secure padding")

    images_value = manifest["images"]
    if not isinstance(images_value, list) or len(images_value) != 2:
        raise SystemExit("signing stage must contain exactly two signed images")
    expected_images = (
        ("bootloader", "bootloader-signed.bin"),
        ("application", "app-signed.bin"),
    )
    image_paths: dict[str, Path] = {}
    expected_files = {"signing-stage.json"}
    for record_value, (expected_image_role, expected_file) in zip(
        images_value, expected_images
    ):
        record = require_exact_keys(
            record_value,
            {
                "file",
                "role",
                "bytes",
                "sha256",
                "source_sha256",
                "secure_boot_v2_signature_blocks",
            },
            "signed image record",
        )
        if record["role"] != expected_image_role or record["file"] != expected_file:
            raise SystemExit("signing-stage image order or identity is invalid")
        path = stage_file(root, record["file"], "signed image")
        if record["bytes"] != path.stat().st_size or record["sha256"] != sha256(path):
            raise SystemExit("signing-stage image size or checksum does not match")
        if record["source_sha256"] != candidate_hashes[expected_image_role]:
            raise SystemExit("signed image is not bound to its unsigned candidate")
        if (
            expected_image_role == "bootloader"
            and record["bytes"] > SIGNED_BOOTLOADER_MAX_BYTES
        ):
            raise SystemExit("signed bootloader exceeds the partition-table boundary")
        expected_signed_bytes = (
            candidate_sizes[expected_image_role]
            + SECURE_BOOT_V2_SIGNATURE_SECTOR_BYTES
        )
        if record["bytes"] != expected_signed_bytes:
            raise SystemExit(
                "signed image must contain exactly one 4 KiB Secure Boot V2 "
                "signature sector"
            )
        if record["secure_boot_v2_signature_blocks"] != stage_number:
            raise SystemExit("signed image declares an invalid signature-block count")
        image_paths[expected_image_role] = path
        expected_files.add(record["file"])

    key_values = manifest["secure_boot_public_keys"]
    if not isinstance(key_values, list) or len(key_values) != stage_number:
        raise SystemExit("signing stage has an invalid public-key chain length")
    key_paths: list[Path] = []
    expected_digests: list[str] = []
    for index, record_value in enumerate(key_values):
        record = require_exact_keys(
            record_value,
            {
                "purpose",
                "sequence",
                "efuse_block",
                "efuse_purpose",
                "public_key_file",
                "public_key_sha256",
                "digest_file",
                "sha256",
                "signing_mode",
                "signer_ref",
                "signing_evidence_file",
                "signing_evidence_sha256",
            },
            "signing public-key record",
        )
        expected_public_file = f"public-keys/{SIGNING_ROLES[index]}.pem"
        expected_digest_file = f"digests/secure-boot-digest{index}.bin"
        if (
            record["purpose"] != SIGNING_ROLES[index]
            or record["sequence"] != index
            or record["efuse_block"] != SIGNING_BLOCKS[index]
            or record["efuse_purpose"] != SIGNING_PURPOSES[index]
            or record["public_key_file"] != expected_public_file
            or record["digest_file"] != expected_digest_file
        ):
            raise SystemExit("signing public-key order or allocation is invalid")
        if record["signing_mode"] not in (
            "pkcs11-hsm",
            "systemd-credential-precomputed",
            "external-precomputed",
            "single-private-key",
        ):
            raise SystemExit("signing mode is invalid")
        if not isinstance(record["signer_ref"], str) or not record["signer_ref"]:
            raise SystemExit("signer reference is missing")
        if index == 0:
            if record["signing_mode"] == "single-private-key":
                raise SystemExit("primary signing key must remain TPM/HSM protected")
            if record["signing_mode"] == "pkcs11-hsm":
                if not record["signer_ref"].startswith("tpm2-pkcs11:"):
                    raise SystemExit("PKCS#11 primary signer lacks its TPM2 reference")
                if record["signing_evidence_file"] is not None or record["signing_evidence_sha256"] is not None:
                    raise SystemExit("PKCS#11 primary signer has unexpected evidence")
            elif record["signing_mode"] == "systemd-credential-precomputed":
                if not record["signer_ref"].startswith("tpm2-systemd-cred:"):
                    raise SystemExit("systemd-credential primary signer lacks its reference")
                if not isinstance(record["signing_evidence_file"], str):
                    raise SystemExit("systemd-credential primary signer lacks evidence")
            else:
                raise SystemExit("primary signing mode is not production-safe")
        else:
            if not record["signer_ref"].startswith("offline-escrow:"):
                raise SystemExit("recovery/rotation signer lacks an offline escrow reference")
            if record["signing_mode"] == "external-precomputed":
                if not isinstance(record["signing_evidence_file"], str):
                    raise SystemExit("offline precomputed signer lacks evidence")
            elif (
                record["signing_evidence_file"] is not None
                or record["signing_evidence_sha256"] is not None
            ):
                raise SystemExit("non-precomputed offline signer has unexpected evidence")
        public_key = stage_file(root, record["public_key_file"], "public verification key")
        public_bytes = public_key.read_bytes()
        if b"PRIVATE KEY" in public_bytes or b"BEGIN PUBLIC KEY" not in public_bytes:
            raise SystemExit("signing stage contains a private or non-PEM verification key")
        if sha256(public_key) != require_sha256(
            record["public_key_sha256"], "public-key checksum"
        ):
            raise SystemExit("public verification-key checksum mismatch")
        digest_file = stage_file(root, record["digest_file"], "public-key digest")
        if digest_file.stat().st_size != 32:
            raise SystemExit("Secure Boot public-key digest is not 32 bytes")
        digest = require_sha256(record["sha256"], "Secure Boot public-key digest")
        if digest_file.read_bytes().hex().upper() != digest:
            raise SystemExit("Secure Boot public-key digest file mismatch")
        with tempfile.TemporaryDirectory(prefix="kitsu-stage-digest-verify-") as temp:
            computed_file = Path(temp) / "digest.bin"
            computed = public_key_digest(tool, public_key, computed_file)
        if computed != digest:
            raise SystemExit("Secure Boot public-key digest does not match its key")
        key_paths.append(public_key)
        expected_digests.append(digest)
        expected_files.update((record["public_key_file"], record["digest_file"]))
        if index == 0 and record["signing_mode"] == "systemd-credential-precomputed":
            evidence = stage_file(
                root, record["signing_evidence_file"], "primary signing evidence"
            )
            if sha256(evidence) != require_sha256(
                record["signing_evidence_sha256"], "primary signing evidence checksum"
            ):
                raise SystemExit("primary signing evidence checksum mismatch")
            evidence_result = validate_primary_signing_evidence(
                evidence,
                public_key=public_key,
                signer_ref=record["signer_ref"],
                device_id=device_id,
                firmware_version=version,
                candidate_inputs=candidates,
                evidence_root=evidence.parent,
            )
            expected_files.add(record["signing_evidence_file"])
            expected_files.update(
                path.relative_to(root).as_posix()
                for path in evidence_result["signature_paths"].values()
            )
        elif index > 0 and record["signing_mode"] == "external-precomputed":
            evidence = stage_file(
                root, record["signing_evidence_file"], "offline signing evidence"
            )
            if sha256(evidence) != require_sha256(
                record["signing_evidence_sha256"], "offline signing evidence checksum"
            ):
                raise SystemExit("offline signing evidence checksum mismatch")
            evidence_result = validate_offline_signing_evidence(
                evidence,
                role=SIGNING_ROLES[index],
                public_key=public_key,
                signer_ref=record["signer_ref"],
                device_id=device_id,
                firmware_version=version,
                candidate_inputs=candidates,
                evidence_root=evidence.parent,
            )
            expected_files.add(record["signing_evidence_file"])
            expected_files.update(
                path.relative_to(root).as_posix()
                for path in evidence_result["signature_paths"].values()
            )
    if len(set(expected_digests)) != stage_number:
        raise SystemExit("signing stage reuses a Secure Boot public key")
    signer_refs = [record["signer_ref"] for record in key_values]
    if len(set(signer_refs)) != stage_number:
        raise SystemExit("signing stage reuses a signer custody reference")

    custody = require_exact_keys(
        manifest["custody"],
        {
            "model",
            "maximum_private_keys_in_process",
            "private_key_material_in_stage",
            "role_isolated",
        },
        "signing custody record",
    )
    if custody != {
        "model": "sequential-isolated-one-signer-per-stage",
        "maximum_private_keys_in_process": 1,
        "private_key_material_in_stage": False,
        "role_isolated": True,
    }:
        raise SystemExit("signing-stage custody record is not production-safe")

    for image in image_paths.values():
        if signature_block_digests(tool, image) != expected_digests:
            raise SystemExit("Secure Boot signature-block order or digest is invalid")
        for public_key in key_paths:
            verify_signature(tool, public_key, image)

    validate_checksum_index(root, expected_files)
    actual_files = {
        path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file()
    }
    if actual_files != expected_files | {"SHA256SUMS.txt"}:
        raise SystemExit("signing stage contains an unmanifested file")
    return {
        "root": root,
        "manifest": manifest,
        "manifest_path": manifest_path,
        "image_paths": image_paths,
        "key_paths": key_paths,
    }


def main() -> None:
    if ARCHIVED_REFERENCE_ONLY:
        raise SystemExit(WITHDRAWN_MESSAGE)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate-dir", type=Path, required=True)
    parser.add_argument("--signing-stage-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--espsecure", type=Path, required=True)
    parser.add_argument("--xts-key", type=Path, required=True)
    parser.add_argument("--hmac-key", type=Path, required=True)
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--firmware-version", required=True)
    parser.add_argument(
        "--connection-image",
        type=Path,
        help="Optional exact 0x40000-byte plaintext kitsu_conn backup region",
    )
    args = parser.parse_args()

    device_id = require_token(args.device_id, "device ID")
    firmware_version = require_token(args.firmware_version, "firmware version")
    candidate_dir = args.candidate_dir.expanduser().resolve()
    if not candidate_dir.is_dir():
        raise SystemExit(f"candidate directory is missing: {candidate_dir}")
    output_dir = args.output_dir.expanduser().resolve()
    if output_dir.exists() and not output_dir.is_dir():
        raise SystemExit("output path exists and is not a directory")
    if output_dir.exists() and any(output_dir.iterdir()):
        raise SystemExit("output directory must be absent or empty")

    tool = require_file(args.espsecure, "espsecure tool")
    xts_key = require_file(args.xts_key, "per-device XTS key")
    xts_bytes = xts_key.read_bytes()
    if len(xts_bytes) != 64:
        raise SystemExit("ESP32-S3 AES-XTS-256 key must be exactly 64 bytes")
    if xts_bytes[:32] == xts_bytes[32:] or xts_bytes in (
        b"\x00" * 64,
        b"\xff" * 64,
    ):
        raise SystemExit("XTS key failed the catastrophic-key sanity check")
    hmac_key = require_file(args.hmac_key, "per-device HMAC_UP key")
    hmac_bytes = hmac_key.read_bytes()
    if len(hmac_bytes) != 32:
        raise SystemExit("ESP32-S3 HMAC_UP key must be exactly 32 bytes")
    if hmac_bytes in (b"\x00" * 32, b"\xff" * 32):
        raise SystemExit("HMAC_UP key failed the catastrophic-key sanity check")

    candidates = {
        "bootloader": require_file(candidate_dir / "bootloader.bin", "bootloader"),
        "partitions": require_file(
            candidate_dir / "partitions.bin", "partition table"
        ),
        "ota": require_file(
            candidate_dir / "ota_data_initial.bin", "initial OTA data"
        ),
        "app": require_file(candidate_dir / "firmware.bin", "application"),
    }
    if candidates["bootloader"].stat().st_size > UNSIGNED_BOOTLOADER_MAX_BYTES:
        raise SystemExit(
            "unsigned bootloader must leave one complete Secure Boot V2 "
            "signature sector before the partition table"
        )
    if candidates["bootloader"].stat().st_size % 0x1000 != 0:
        raise SystemExit(
            "unsigned bootloader is not the required 4 KiB remote-signing candidate"
        )
    if candidates["ota"].stat().st_size != 0x2000:
        raise SystemExit("ota_data_initial.bin must be exactly 0x2000 bytes")
    if candidates["app"].stat().st_size % 0x10000 != 0:
        raise SystemExit(
            "unsigned application lacks Secure Boot V2 64 KiB secure padding"
        )
    connection: Path | None = None
    if args.connection_image:
        connection = require_file(args.connection_image, "kitsu_conn migration image")
        if connection.stat().st_size != 0x40000:
            raise SystemExit("kitsu_conn migration image must be exactly 0x40000 bytes")

    layout_file = Path(__file__).resolve().parents[1] / "partitions_kitsu_production_8MB.csv"
    partition_validation = validate_partition_table(candidates["partitions"], layout_file)
    signing_stage = validate_signing_stage(
        args.signing_stage_dir,
        tool,
        expected_device_id=device_id,
        expected_firmware_version=firmware_version,
        expected_role="rotation",
    )
    stage_manifest = signing_stage["manifest"]
    for record, candidate_role in zip(
        stage_manifest["candidate_inputs"], ("bootloader", "app")
    ):
        candidate = candidates[candidate_role]
        if record["bytes"] != candidate.stat().st_size or record["sha256"] != sha256(
            candidate
        ):
            raise SystemExit(
                "final signing stage is not bound to the supplied unsigned candidates"
            )

    output_dir.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(
        tempfile.mkdtemp(
            prefix=f".{output_dir.name}.staging-", dir=str(output_dir.parent)
        )
    )
    committed = False
    try:
        first_boot = staging / "first-boot-plaintext"
        recovery = staging / "post-provision-encrypted"
        metadata = staging / "metadata"
        first_boot.mkdir()
        recovery.mkdir()
        metadata.mkdir()
        copy_image(layout_file, metadata / layout_file.name)

        key_digests: list[dict[str, Any]] = []
        for index, stage_key in enumerate(
            stage_manifest["secure_boot_public_keys"]
        ):
            digest_file = metadata / f"secure-boot-digest{index}.bin"
            public_key_file = metadata / f"secure-boot-{SIGNING_ROLES[index]}-public.pem"
            copy_image(
                signing_stage["root"] / stage_key["digest_file"], digest_file
            )
            copy_image(
                signing_stage["root"] / stage_key["public_key_file"], public_key_file
            )
            key_digests.append(
                {
                    "purpose": SIGNING_ROLES[index],
                    "efuse_block": SIGNING_BLOCKS[index],
                    "efuse_purpose": SIGNING_PURPOSES[index],
                    "sha256": stage_key["sha256"],
                    "file": digest_file.name,
                    "public_key_file": public_key_file.name,
                    "public_key_sha256": sha256(public_key_file),
                    "signing_mode": stage_key["signing_mode"],
                    "signer_ref": stage_key["signer_ref"],
                }
            )
        if len({entry["sha256"] for entry in key_digests}) != 3:
            raise SystemExit("primary, recovery, and rotation public keys must differ")

        signature_blocks = len(SIGNING_ROLES)
        signed_bootloader = first_boot / "0x000000-bootloader-signed.bin"
        signed_app = first_boot / "0x010000-app0-signed.bin"
        copy_image(signing_stage["image_paths"]["bootloader"], signed_bootloader)
        copy_image(signing_stage["image_paths"]["application"], signed_app)
        if signed_bootloader.stat().st_size > SIGNED_BOOTLOADER_MAX_BYTES:
            raise SystemExit("signed bootloader exceeds the partition-table boundary")
        if signed_bootloader.stat().st_size != (
            candidates["bootloader"].stat().st_size
            + SECURE_BOOT_V2_SIGNATURE_SECTOR_BYTES
        ):
            raise SystemExit(
                "signed bootloader must contain exactly one 4 KiB Secure Boot "
                "V2 signature sector"
            )
        if signed_app.stat().st_size > 0x330000:
            raise SystemExit("signed application exceeds the 0x330000-byte OTA slot")

        first_partition = first_boot / "0x008000-partitions.bin"
        first_ota = first_boot / "0x00E000-ota-data-initial.bin"
        first_app1 = first_boot / "0x340000-app1-signed.bin"
        copy_image(candidates["partitions"], first_partition)
        copy_image(candidates["ota"], first_ota)
        copy_image(signed_app, first_app1)

        encrypted_specs = [
            (
                signed_bootloader,
                recovery / "0x000000-bootloader-signed-encrypted.bin",
                BOOTLOADER_OFFSET,
                "bootloader",
                "bootloader",
                True,
            ),
            (
                first_partition,
                recovery / "0x008000-partitions-encrypted.bin",
                PARTITIONS_OFFSET,
                "partition_table",
                "partition_table",
                False,
            ),
            (
                first_ota,
                recovery / "0x00E000-ota-data-encrypted.bin",
                OTA_DATA_OFFSET,
                "ota_data",
                "otadata",
                False,
            ),
            (
                signed_app,
                recovery / "0x010000-app0-signed-encrypted.bin",
                APP0_OFFSET,
                "application",
                "app0",
                True,
            ),
            (
                first_app1,
                recovery / "0x340000-app1-signed-encrypted.bin",
                APP1_OFFSET,
                "recovery_application",
                "app1",
                True,
            ),
        ]
        recovery_records: list[dict[str, Any]] = []
        for source, destination, offset, role, partition, is_signed in encrypted_specs:
            encrypt_and_verify(tool, xts_key, source, offset, destination)
            recovery_records.append(
                image_record(
                    destination,
                    role,
                    partition,
                    offset,
                    signed=is_signed,
                    encrypted=True,
                    source=source,
                    signature_blocks=signature_blocks if is_signed else 0,
                )
            )

        if connection:
            encrypted_connection = recovery / "0x7B0000-kitsu-conn-encrypted.bin"
            encrypt_and_verify(
                tool, xts_key, connection, CONNECTION_OFFSET, encrypted_connection
            )
            recovery_records.append(
                image_record(
                    encrypted_connection,
                    "connectivity_migration",
                    "kitsu_conn",
                    CONNECTION_OFFSET,
                    signed=False,
                    encrypted=True,
                    source=connection,
                )
            )

        first_records = [
            image_record(
                signed_bootloader,
                "bootloader",
                "bootloader",
                BOOTLOADER_OFFSET,
                signed=True,
                encrypted=False,
                source=candidates["bootloader"],
                signature_blocks=signature_blocks,
            ),
            image_record(
                first_partition,
                "partition_table",
                "partition_table",
                PARTITIONS_OFFSET,
                signed=False,
                encrypted=False,
                source=candidates["partitions"],
            ),
            image_record(
                first_ota,
                "ota_data",
                "otadata",
                OTA_DATA_OFFSET,
                signed=False,
                encrypted=False,
                source=candidates["ota"],
            ),
            image_record(
                signed_app,
                "application",
                "app0",
                APP0_OFFSET,
                signed=True,
                encrypted=False,
                source=candidates["app"],
                signature_blocks=signature_blocks,
            ),
            image_record(
                first_app1,
                "recovery_application",
                "app1",
                APP1_OFFSET,
                signed=True,
                encrypted=False,
                source=candidates["app"],
                signature_blocks=signature_blocks,
            ),
        ]

        manifest = {
            "schema": SCHEMA,
            "created_at": datetime.now(timezone.utc)
            .isoformat()
            .replace("+00:00", "Z"),
            "artifact_status": "cryptographically_verified-awaiting-reviewed-provisioning",
            "device_id": device_id,
            "device_class": "heltec-wifi-lora-32-v3-esp32s3-8mb",
            "firmware_version": firmware_version,
            "checksum_index": "SHA256SUMS.txt",
            "candidate_inputs": [
                candidate_record(candidates["bootloader"], "bootloader"),
                candidate_record(candidates["partitions"], "partition_table"),
                candidate_record(candidates["ota"], "ota_data"),
                candidate_record(candidates["app"], "application"),
            ],
            "partition_layout": {
                "file": f"metadata/{layout_file.name}",
                "sha256": sha256(layout_file),
                **partition_validation,
            },
            "security_profile": {
                "secure_boot": "v2-rsa3072",
                "signature_blocks": signature_blocks,
                "remote_signing_candidate_alignment_verified": True,
                "flash_encryption": "aes-xts-256-release",
                "nvs_encryption": "aes-xts-with-flash-encrypted-nvs-keys",
                "rom_download_mode": "secure",
                "ota_rollback": True,
                "anti_rollback_efuse": False,
            },
            "signing_ceremony": {
                "schema": SIGNING_STAGE_SCHEMA,
                "custody_model": "sequential-isolated-one-signer-per-stage",
                "final_stage_role": stage_manifest["role"],
                "final_stage_manifest_sha256": sha256(
                    signing_stage["manifest_path"]
                ),
                "final_stage_checksum_index_sha256": sha256(
                    signing_stage["root"] / "SHA256SUMS.txt"
                ),
                "private_signing_keys_received_by_packager": False,
                "role_order": list(SIGNING_ROLES),
            },
            "key_block_allocation": {
                "BLOCK_KEY0": "SECURE_BOOT_DIGEST0-primary",
                "BLOCK_KEY1": "XTS_AES_256_KEY_1",
                "BLOCK_KEY2": "XTS_AES_256_KEY_2",
                "BLOCK_KEY3": "HMAC_UP",
                "BLOCK_KEY4": "SECURE_BOOT_DIGEST1-recovery",
                "BLOCK_KEY5": "SECURE_BOOT_DIGEST2-rotation",
            },
            "secure_boot_public_key_digests": key_digests,
            "device_secret_fingerprints": {
                "xts_aes_256_key_sha256": hashlib.sha256(xts_bytes)
                .hexdigest()
                .upper(),
                "hmac_up_key_sha256": hashlib.sha256(hmac_bytes)
                .hexdigest()
                .upper(),
                "secret_bytes_in_manifest": False,
            },
            "first_boot_plaintext_images": first_records,
            "post_provision_encrypted_images": recovery_records,
            "preserved_regions": [
                {
                    "partition": "nvs_legacy",
                    "offset": LEGACY_NVS_OFFSET,
                    "offset_hex": "0x009000",
                    "action": "preserve-plaintext-source-opened-nvs-readonly",
                },
                {
                    "partition": "spiffs",
                    "offset": PACK_OFFSET,
                    "offset_hex": "0x670000",
                    "action": "preserve-plaintext-validated-raw-companion-pack",
                },
            ],
            "runtime_generated_regions": [
                {
                    "partition": "nvs_keys",
                    "offset": 0x7FB000,
                    "offset_hex": "0x7FB000",
                    "note": "generated by encrypted NVS initialization on first secure boot",
                },
                {
                    "partition": "nvs",
                    "offset": 0x7F0000,
                    "offset_hex": "0x7F0000",
                    "note": "encrypted destination populated by one-time readback-verified migration",
                },
            ],
            "warnings": [
                {
                    "code": "IRREVERSIBLE_EFUSE_CHECKPOINT",
                    "severity": "critical",
                    "message": (
                        "Do not reset into the production bootloader until every "
                        "image hash, key purpose, backup, and recovery key has been "
                        "independently verified."
                    ),
                },
                {
                    "code": "SPLIT_SIGNING_CUSTODY_REQUIRED",
                    "severity": "critical",
                    "message": (
                        "Primary, recovery, and rotation signing must remain "
                        "separate one-role ceremonies; no machine or process may "
                        "receive more than one private signing key."
                    ),
                },
                {
                    "code": "DEVICE_BOUND_XTS",
                    "severity": "critical",
                    "message": (
                        "Encrypted images are address-bound and valid only for the "
                        "device whose unique 64-byte XTS key produced this bundle."
                    ),
                },
                {
                    "code": "KEY_UNIQUENESS_EXTERNAL",
                    "severity": "critical",
                    "message": (
                        "The ceremony must verify both device-secret fingerprints "
                        "are unique in the external provisioning registry."
                    ),
                },
                {
                    "code": "NVS_KEYS_RUNTIME_GENERATED",
                    "severity": "high",
                    "message": (
                        "Take a fresh encrypted full-flash backup after the first "
                        "verified secure boot and migration."
                    ),
                },
                {
                    "code": "DEVELOPMENT_CREDENTIALS_INVALIDATED",
                    "severity": "high",
                    "message": (
                        "The migration deliberately excludes development security "
                        "roots, controller records, gateway enrollment, and replay "
                        "journals; Wi-Fi/gateway setup and owner enrollment must be "
                        "performed again over authenticated BLE."
                    ),
                },
                {
                    "code": "PRESERVE_PLAINTEXT_REGIONS",
                    "severity": "critical",
                    "message": (
                        "Never erase or offline-encrypt nvs_legacy at 0x009000 or "
                        "the companion pack at 0x670000 during migration."
                    ),
                },
                {
                    "code": "NO_DIRECT_FLASH",
                    "severity": "high",
                    "message": (
                        "This tool never invokes esptool or espefuse; provisioning "
                        "remains a separately reviewed ceremony."
                    ),
                },
            ],
        }
        manifest_path = staging / "manifest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        write_checksum_index(staging)

        if output_dir.exists():
            output_dir.rmdir()  # It was proven empty before staging began.
        os.replace(staging, output_dir)
        committed = True
    finally:
        if not committed:
            shutil.rmtree(staging, ignore_errors=True)

    print(f"Kitsu production bundle created: {output_dir / 'manifest.json'}")


if __name__ == "__main__":
    main()
