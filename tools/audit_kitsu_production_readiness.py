#!/usr/bin/env python3
"""Create a deterministic, non-secret KTDEAD production-readiness audit."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any

import package_kitsu_production as packager


SCHEMA = "kitsu.production-readiness.v1"
ARCHIVED_REFERENCE_ONLY = True
WITHDRAWN_MESSAGE = (
    "KITSU_WITHDRAWN_NOT_AUTHORIZED: device-bound eFuse production readiness "
    "is not applicable after the owner-selected reflashable profile. This "
    "implementation remains only as forensic reference."
)
PREFLIGHT_SCHEMA = "kitsu.device-production-preflight.v1"
EXPECTED_DEVICE = "KTDEAD"
EXPECTED_VERSION = "0.10.0"
EXPECTED_BACKUP_BYTES = 0x800000
PRESERVED_REGIONS = (
    ("nvs_legacy", 0x009000, 0x005000),
    ("spiffs", 0x670000, 0x140000),
    ("kitsu_conn", 0x7B0000, 0x040000),
)
REPURPOSED_REGIONS = (("former_coredump_tail", 0x7F0000, 0x010000),)
FINAL_MANIFEST_FIELDS = (
    "schema",
    "created_at",
    "artifact_status",
    "device_id",
    "device_class",
    "firmware_version",
    "checksum_index",
    "candidate_inputs",
    "partition_layout",
    "security_profile",
    "signing_ceremony",
    "key_block_allocation",
    "secure_boot_public_key_digests",
    "device_secret_fingerprints",
    "first_boot_plaintext_images",
    "post_provision_encrypted_images",
    "preserved_regions",
    "runtime_generated_regions",
    "warnings",
)
FINAL_WARNING_SEVERITIES = {
    "IRREVERSIBLE_EFUSE_CHECKPOINT": "critical",
    "SPLIT_SIGNING_CUSTODY_REQUIRED": "critical",
    "DEVICE_BOUND_XTS": "critical",
    "KEY_UNIQUENESS_EXTERNAL": "critical",
    "NVS_KEYS_RUNTIME_GENERATED": "high",
    "DEVELOPMENT_CREDENTIALS_INVALIDATED": "high",
    "PRESERVE_PLAINTEXT_REGIONS": "critical",
    "NO_DIRECT_FLASH": "high",
}


def sha256(path: Path) -> str:
    return packager.sha256(path)


def exact_keys(value: object, expected: set[str], description: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != expected:
        raise SystemExit(f"{description} has an invalid schema")
    return value


def file_record(path: Path, role: str) -> dict[str, Any]:
    file = packager.require_file(path, role)
    return {
        "role": role,
        "file": file.name,
        "bytes": file.stat().st_size,
        "sha256": sha256(file),
    }


def region_records(backup: Path, specs: tuple[tuple[str, int, int], ...]) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    with backup.open("rb") as stream:
        for name, offset, size in specs:
            stream.seek(offset)
            payload = stream.read(size)
            if len(payload) != size:
                raise SystemExit(f"backup is truncated in {name}")
            records.append(
                {
                    "region": name,
                    "offset": offset,
                    "offset_hex": f"0x{offset:06X}",
                    "bytes": size,
                    "sha256": hashlib.sha256(payload).hexdigest().upper(),
                }
            )
    return records


def validate_preflight(directory: Path) -> dict[str, Any]:
    manifest_path = packager.require_file(directory / "preflight.json", "device preflight")
    try:
        value = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError, OSError) as error:
        raise SystemExit("device preflight is not valid UTF-8 JSON") from error
    manifest = exact_keys(
        value,
        {
            "schema",
            "captured_at_local",
            "device_uid",
            "serial_port",
            "chip",
            "embedded_flash_bytes",
            "crystal_mhz",
            "mac",
            "security",
            "backup",
            "writes_performed",
            "efuses_changed",
        },
        "device preflight",
    )
    if (
        manifest["schema"] != PREFLIGHT_SCHEMA
        or manifest["device_uid"] != EXPECTED_DEVICE
        or manifest["serial_port"] != "COM3"
        or manifest["chip"] != "ESP32-S3 QFN56 revision 0.2"
        or manifest["embedded_flash_bytes"] != EXPECTED_BACKUP_BYTES
        or manifest["crystal_mhz"] != 40
        or manifest["writes_performed"] is not False
        or manifest["efuses_changed"] is not False
    ):
        raise SystemExit("device preflight identity or no-write assertion differs")
    security = exact_keys(
        manifest["security"],
        {"flags", "secure_boot", "flash_encryption", "spi_boot_crypt_count", "key_blocks"},
        "device preflight security",
    )
    if (
        security["flags"] != "0x00000000"
        or security["secure_boot"] is not False
        or security["flash_encryption"] is not False
        or security["spi_boot_crypt_count"] != "0x0"
        or security["key_blocks"]
        != {f"BLOCK_KEY{index}": "USER/EMPTY" for index in range(6)}
    ):
        raise SystemExit("device preflight security state is not pristine")
    backup_record = exact_keys(
        manifest["backup"],
        {"file", "offset", "bytes", "sha256", "read_only", "verified_size"},
        "device preflight backup",
    )
    backup = packager.require_file(directory / backup_record["file"], "full-flash backup")
    if (
        backup_record["offset"] != "0x00000000"
        or backup_record["bytes"] != EXPECTED_BACKUP_BYTES
        or backup.stat().st_size != EXPECTED_BACKUP_BYTES
        or backup_record["read_only"] is not True
        or backup_record["verified_size"] is not True
        or backup_record["sha256"].upper() != sha256(backup)
    ):
        raise SystemExit("full-flash backup does not match the preflight")
    return {
        "manifest_file": manifest_path.name,
        "manifest_sha256": sha256(manifest_path),
        "captured_at_local": manifest["captured_at_local"],
        "device_uid": manifest["device_uid"],
        "serial_port": manifest["serial_port"],
        "chip": manifest["chip"],
        "mac": manifest["mac"],
        "backup_file": backup.name,
        "backup_bytes": backup.stat().st_size,
        "backup_sha256": sha256(backup),
        "security_state": "secure-boot-off_flash-encryption-off_key0-5-empty",
        "preserved_region_baselines": region_records(backup, PRESERVED_REGIONS),
        "deliberately_repurposed_region_baselines": region_records(
            backup, REPURPOSED_REGIONS
        ),
    }


def main() -> None:
    if ARCHIVED_REFERENCE_ONLY:
        raise SystemExit(WITHDRAWN_MESSAGE)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate-dir", required=True, type=Path)
    parser.add_argument("--primary-stage-dir", required=True, type=Path)
    parser.add_argument("--preflight-dir", required=True, type=Path)
    parser.add_argument("--espsecure", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    candidate = args.candidate_dir.resolve()
    tool = packager.require_file(args.espsecure, "espsecure tool")
    bootloader = packager.require_file(candidate / "bootloader.bin", "unsigned bootloader")
    partitions = packager.require_file(candidate / "partitions.bin", "partition table")
    ota = packager.require_file(candidate / "ota_data_initial.bin", "OTA metadata")
    application = packager.require_file(candidate / "firmware.bin", "unsigned application")
    if bootloader.stat().st_size > packager.UNSIGNED_BOOTLOADER_MAX_BYTES:
        raise SystemExit("candidate bootloader crosses the signing boundary")
    if bootloader.stat().st_size % 0x1000 or application.stat().st_size % 0x10000:
        raise SystemExit("candidate secure-padding alignment is invalid")
    if packager.signature_block_digests(tool, bootloader) or packager.signature_block_digests(
        tool, application
    ):
        raise SystemExit("readiness candidate must remain unsigned")
    partition_validation = packager.validate_partition_table(
        partitions,
        Path(__file__).resolve().parents[1] / "partitions_kitsu_production_8MB.csv",
    )
    primary = packager.validate_signing_stage(
        args.primary_stage_dir,
        tool,
        expected_device_id=EXPECTED_DEVICE,
        expected_firmware_version=EXPECTED_VERSION,
        expected_role="primary",
    )
    primary_manifest = primary["manifest"]
    primary_key = primary_manifest["secure_boot_public_keys"][0]
    if (
        primary_key["signing_mode"] != "systemd-credential-precomputed"
        or primary_key["signer_ref"]
        != "tpm2-systemd-cred:kitsu-host/firmware-signing-primary-key"
    ):
        raise SystemExit("primary stage does not carry the reviewed custody identity")

    preflight = validate_preflight(args.preflight_dir.resolve())
    document = {
        "schema": SCHEMA,
        "artifact_status": "nondeployable-awaiting-owner-custody-and-device-secrets",
        "device_id": EXPECTED_DEVICE,
        "firmware_version": EXPECTED_VERSION,
        "candidate_inputs": [
            file_record(bootloader, "bootloader"),
            file_record(partitions, "partition_table"),
            file_record(ota, "ota_data"),
            file_record(application, "application"),
        ],
        "partition_layout": partition_validation,
        "primary_signing_stage": {
            "schema": primary_manifest["schema"],
            "role": primary_manifest["role"],
            "stage_number": primary_manifest["stage_number"],
            "signature_blocks": primary_manifest["signature_blocks"],
            "manifest_sha256": sha256(primary["manifest_path"]),
            "checksum_index_sha256": sha256(primary["root"] / "SHA256SUMS.txt"),
            "signed_bootloader_bytes": primary["image_paths"]["bootloader"].stat().st_size,
            "signed_bootloader_sha256": sha256(primary["image_paths"]["bootloader"]),
            "signed_application_bytes": primary["image_paths"]["application"].stat().st_size,
            "signed_application_sha256": sha256(primary["image_paths"]["application"]),
            "public_key_digest": primary_key["sha256"],
            "signing_mode": primary_key["signing_mode"],
            "signer_ref": primary_key["signer_ref"],
            "evidence_sha256": primary_key["signing_evidence_sha256"],
        },
        "device_preflight": preflight,
        "remaining_authorized_inputs": [
            {
                "role": "recovery",
                "requirement": "owner-approved distinct RSA-3072 CMS recipient identity and one-role signing/escrow ceremony",
                "private_material_allowed_in_readiness_pack": False,
            },
            {
                "role": "rotation",
                "requirement": "owner-approved distinct RSA-3072 CMS recipient identity and separate one-role signing/escrow ceremony",
                "private_material_allowed_in_readiness_pack": False,
            },
            {
                "role": "device_xts",
                "requirement": "unique 64-byte random ESP32-S3 AES-XTS-256 key generated only after approval",
                "private_material_allowed_in_readiness_pack": False,
            },
            {
                "role": "device_hmac_up",
                "requirement": "unique 32-byte random HMAC_UP key generated only after approval",
                "private_material_allowed_in_readiness_pack": False,
            },
        ],
        "final_bundle_contract": {
            "schema": packager.SCHEMA,
            "exact_top_level_fields": list(FINAL_MANIFEST_FIELDS),
            "warning_severities": FINAL_WARNING_SEVERITIES,
            "expected_signature_blocks": 3,
            "expected_signed_bootloader_max_bytes": packager.SIGNED_BOOTLOADER_MAX_BYTES,
            "expected_partition_table_offset": packager.PARTITIONS_OFFSET,
        },
        "approval_gate": {
            "required": True,
            "text": (
                "I authorize creation of two durable, distinct DPAPI-protected "
                "RSA-3072 owner custody identities (firmware-recovery and "
                "firmware-rotation), their public recipient certificates, two "
                "separate one-role Secure Boot escrow/signing ceremonies, and "
                "unique KTDEAD 64-byte XTS-AES-256 and 32-byte HMAC_UP device "
                "secrets. Do not flash COM3 or burn eFuses yet."
            ),
        },
        "prohibited_actions_at_readiness_stage": [
            "open-or-write-COM3",
            "invoke-esptool-or-espefuse-against-hardware",
            "generate-XTS-or-HMAC-device-secrets",
            "generate-owner-custody-private-identities",
            "flash-images",
            "burn-efuses",
            "release-BOOT-GPIO0-for-first-production-boot",
        ],
    }
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        raise SystemExit("readiness audit output already exists")
    output.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"Kitsu deterministic production-readiness audit created: {output}")


if __name__ == "__main__":
    main()
