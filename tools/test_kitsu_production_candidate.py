#!/usr/bin/env python3
"""Cryptographically audit exact production candidates with ephemeral keys.

This test never opens a serial port, creates a deployable device bundle, or
uses production custody material. It passes the exact build outputs through
three isolated Secure Boot V2 fixture stages and proves the resulting image
boundaries before deleting every temporary key and signature.
"""

from __future__ import annotations

import argparse
import sys
import tempfile
from pathlib import Path

import package_kitsu_production as packager
from test_package_kitsu_production import (
    extract_public_key,
    make_precomputed_signature,
    make_systemd_credential_evidence,
    run,
    tool_command,
)


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate-dir", required=True, type=Path)
    parser.add_argument("--espsecure", required=True, type=Path)
    args = parser.parse_args()

    candidate = args.candidate_dir.resolve()
    espsecure = args.espsecure.resolve()
    bootloader = packager.require_file(candidate / "bootloader.bin", "bootloader")
    application = packager.require_file(candidate / "firmware.bin", "application")
    partitions = packager.require_file(candidate / "partitions.bin", "partition table")
    packager.require_file(candidate / "ota_data_initial.bin", "OTA metadata")
    if bootloader.stat().st_size > packager.UNSIGNED_BOOTLOADER_MAX_BYTES:
        raise SystemExit("candidate bootloader exceeds the unsigned boundary")
    if bootloader.stat().st_size % 0x1000 != 0:
        raise SystemExit("candidate bootloader is not 4 KiB aligned")
    if application.stat().st_size % 0x10000 != 0:
        raise SystemExit("candidate application lacks 64 KiB secure padding")
    if packager.signature_block_digests(espsecure, bootloader):
        raise SystemExit("candidate bootloader is unexpectedly signed")
    if packager.signature_block_digests(espsecure, application):
        raise SystemExit("candidate application is unexpectedly signed")
    partition_validation = packager.validate_partition_table(
        partitions, ROOT / "partitions_kitsu_production_8MB.csv"
    )

    with tempfile.TemporaryDirectory(prefix="kitsu-exact-candidate-audit-") as name:
        temp = Path(name)
        keys: list[Path] = []
        public_keys: list[Path] = []
        for role in packager.SIGNING_ROLES:
            key = temp / f"ephemeral-{role}.pem"
            run(
                tool_command(espsecure)
                + [
                    "generate_signing_key",
                    "--version",
                    "2",
                    "--scheme",
                    "rsa3072",
                    str(key),
                ]
            )
            public_key = temp / f"ephemeral-{role}-public.pem"
            extract_public_key(espsecure, key, public_key)
            keys.append(key)
            public_keys.append(public_key)

        boot_signature = temp / "primary-bootloader.sig"
        app_signature = temp / "primary-application.sig"
        make_precomputed_signature(
            espsecure,
            keys[0],
            bootloader,
            temp / "primary-bootloader-reference.bin",
            boot_signature,
        )
        make_precomputed_signature(
            espsecure,
            keys[0],
            application,
            temp / "primary-application-reference.bin",
            app_signature,
        )
        primary_signer_ref = "tpm2-systemd-cred:ephemeral-candidate-audit"
        primary_evidence = make_systemd_credential_evidence(
            temp / "primary-evidence",
            public_key=public_keys[0],
            bootloader_signature=boot_signature,
            application_signature=app_signature,
            bootloader=bootloader,
            application=application,
            device_id="KT-EPHEMERAL-AUDIT",
            firmware_version="candidate-audit-only",
            signer_ref=primary_signer_ref,
        )

        stages: list[Path] = []
        for index, role in enumerate(packager.SIGNING_ROLES):
            stage = temp / f"stage-{index + 1}-{role}"
            command = [
                sys.executable,
                str(ROOT / "tools" / "sign_kitsu_production_stage.py"),
                "--role",
                role,
                "--input-dir",
                str(candidate if index == 0 else stages[-1]),
                "--output-dir",
                str(stage),
                "--espsecure",
                str(espsecure),
                "--public-key",
                str(public_keys[index]),
                "--signer-ref",
                (
                    primary_signer_ref
                    if index == 0
                    else f"offline-escrow:ephemeral-{role}-audit"
                ),
                "--device-id",
                "KT-EPHEMERAL-AUDIT",
                "--firmware-version",
                "candidate-audit-only",
            ]
            if index == 0:
                command.extend(
                    (
                        "--bootloader-signature",
                        str(boot_signature),
                        "--app-signature",
                        str(app_signature),
                        "--signing-evidence",
                        str(primary_evidence),
                    )
                )
            else:
                command.extend(("--signing-key", str(keys[index])))
            run(command)
            stages.append(stage)

        final = packager.validate_signing_stage(
            stages[-1],
            espsecure,
            expected_device_id="KT-EPHEMERAL-AUDIT",
            expected_firmware_version="candidate-audit-only",
            expected_role="rotation",
        )
        signed_bootloader = final["image_paths"]["bootloader"]
        signed_application = final["image_paths"]["application"]
        signed_bootloader_bytes = signed_bootloader.stat().st_size
        signed_application_bytes = signed_application.stat().st_size
        if signed_bootloader_bytes != (
            bootloader.stat().st_size
            + packager.SECURE_BOOT_V2_SIGNATURE_SECTOR_BYTES
        ):
            raise SystemExit("bootloader did not gain exactly one signature sector")
        if signed_application_bytes != (
            application.stat().st_size
            + packager.SECURE_BOOT_V2_SIGNATURE_SECTOR_BYTES
        ):
            raise SystemExit("application did not gain exactly one signature sector")
        digests = packager.signature_block_digests(espsecure, signed_bootloader)
        if len(digests) != 3:
            raise SystemExit("signed bootloader does not contain three valid blocks")
        if signed_bootloader_bytes > packager.SIGNED_BOOTLOADER_MAX_BYTES:
            raise SystemExit("signed bootloader crosses the partition table")

        print(
            "exact_candidate_audit",
            {
                "bootloader_unsigned_bytes": bootloader.stat().st_size,
                "bootloader_unsigned_sha256": packager.sha256(bootloader),
                "signature_sector_bytes": packager.SECURE_BOOT_V2_SIGNATURE_SECTOR_BYTES,
                "signature_blocks": len(digests),
                "bootloader_signed_bytes": signed_bootloader_bytes,
                "bootloader_signed_end": f"0x{signed_bootloader_bytes:06X}",
                "partition_table_offset": f"0x{packager.PARTITIONS_OFFSET:06X}",
                "partition_table_gap_bytes": (
                    packager.PARTITIONS_OFFSET - signed_bootloader_bytes
                ),
                "application_unsigned_bytes": application.stat().st_size,
                "application_unsigned_sha256": packager.sha256(application),
                "application_signed_bytes": signed_application_bytes,
                "partition_entry_count": partition_validation["entry_count"],
                "partition_embedded_md5": partition_validation["embedded_md5"],
                "ephemeral_material_deleted_on_exit": True,
            },
        )


if __name__ == "__main__":
    main()
