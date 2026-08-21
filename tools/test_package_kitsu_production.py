#!/usr/bin/env python3
"""End-to-end offline test for the production bundle packager."""

from __future__ import annotations

import argparse
import hashlib
import json
import secrets
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

import package_kitsu_production as packager


ROOT = Path(__file__).resolve().parents[1]


def run(command: list[str], expect_success: bool = True) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    if expect_success and completed.returncode != 0:
        raise AssertionError(
            f"command failed ({completed.returncode}):\n{completed.stdout}\n{completed.stderr}"
        )
    if not expect_success and completed.returncode == 0:
        raise AssertionError("command unexpectedly succeeded")
    return completed


def tool_command(espsecure: Path) -> list[str]:
    return [sys.executable, str(espsecure)] if espsecure.suffix.lower() == ".py" else [str(espsecure)]


def encoded_partition_table(records: list[dict[str, object]]) -> bytes:
    entries = b""
    for record in records:
        label = (str(record["label"]).encode("ascii") + b"\0").ljust(16, b"\0")
        entries += struct.pack(
            "<2sBBII16sI",
            packager.PARTITION_MAGIC,
            int(record["type"]),
            int(record["subtype"]),
            int(record["offset"]),
            int(record["size"]),
            label,
            int(record["flags"]),
        )
    md5_record = (
        packager.PARTITION_MD5_MAGIC
        + b"\xff" * 14
        + hashlib.md5(entries).digest()  # nosec B324 -- ESP table format
    )
    return (entries + md5_record).ljust(packager.PARTITION_BINARY_BYTES, b"\xff")


def verify_checksum_index(bundle: Path) -> None:
    index = bundle / "SHA256SUMS.txt"
    entries = index.read_text(encoding="ascii").splitlines()
    assert entries
    for entry in entries:
        digest, relative = entry.split("  ", 1)
        path = bundle / relative
        assert path.is_file(), relative
        assert packager.sha256(path) == digest, relative


def extract_public_key(espsecure: Path, private_key: Path, public_key: Path) -> None:
    run(
        tool_command(espsecure)
        + [
            "extract_public_key",
            "--version",
            "2",
            "--keyfile",
            str(private_key),
            str(public_key),
        ]
    )


def make_precomputed_signature(
    espsecure: Path,
    private_key: Path,
    candidate: Path,
    signed_image: Path,
    signature_file: Path,
) -> None:
    run(
        tool_command(espsecure)
        + [
            "sign_data",
            "--version",
            "2",
            "--keyfile",
            str(private_key),
            "--output",
            str(signed_image),
            str(candidate),
        ]
    )
    signed = signed_image.read_bytes()
    assert len(signed) == candidate.stat().st_size + 0x1000
    signature_block = signed[-0x1000 : -0x1000 + 1216]
    # RSA signature bytes occupy 812..1195 in little-endian hardware order;
    # espsecure's external-signature input uses the original big-endian form.
    signature = signature_block[812:1196][::-1]
    assert len(signature) == 384
    signature_file.write_bytes(signature)


def make_systemd_credential_evidence(
    evidence_dir: Path,
    *,
    public_key: Path,
    bootloader_signature: Path,
    application_signature: Path,
    bootloader: Path,
    application: Path,
    device_id: str,
    firmware_version: str,
    signer_ref: str,
) -> Path:
    evidence_dir.mkdir()
    signatures = []
    candidate_inputs = []
    for role, candidate, source_signature in (
        ("bootloader", bootloader, bootloader_signature),
        ("application", application, application_signature),
    ):
        destination = evidence_dir / f"{role}.sig"
        shutil.copyfile(source_signature, destination)
        candidate_inputs.append(
            {
                "file": candidate.name,
                "role": role,
                "bytes": candidate.stat().st_size,
                "sha256": packager.sha256(candidate),
            }
        )
        signatures.append(
            {
                "role": role,
                "file": destination.name,
                "bytes": destination.stat().st_size,
                "sha256": packager.sha256(destination),
                "input_file": candidate.name,
                "input_sha256": packager.sha256(candidate),
                "scheme": "RSA-PSS-SHA256",
                "mgf1": "SHA256",
                "salt_length": 32,
            }
        )
    evidence = {
        "schema": packager.PRIMARY_SIGNING_EVIDENCE_SCHEMA,
        "created_at": "2026-08-18T00:00:00Z",
        "request": {
            "schema": packager.PRIMARY_SIGNING_REQUEST_SCHEMA,
            "request_id": str(uuid.UUID("01234567-89ab-4def-8123-456789abcdef")),
            "device_id": device_id,
            "firmware_version": firmware_version,
            "candidate_inputs": candidate_inputs,
        },
        "signer": {
            "signer_ref": signer_ref,
            "host": "fixture-host",
            "credential_name": "firmware-signing-primary-key",
            "delivery": "systemd-LoadCredentialEncrypted",
            "private_key_exported": False,
            "plaintext_persisted": False,
        },
        "public_identity": {
            "algorithm": "RSA",
            "bits": 3072,
            "public_key_sha256": packager.sha256(public_key),
            "spki_sha256": hashlib.sha256(public_key.read_bytes()).hexdigest().upper(),
            "credential_matches_pinned_public": True,
        },
        "signatures": signatures,
        "controls": {
            "effective_uid": 0,
            "credential_filesystem": "ramfs",
            "network_namespace_private": True,
            "coredumps_disabled": True,
            "output_atomic": True,
            "credential_cleanup": "systemd-managed-after-oneshot",
        },
        "warnings": [
            "SYSTEMD_CREDENTIAL_DECRYPTED_IN_RAM_FOR_ONE_SHOT",
            "NO_HARDWARE_NONEXPORTABLE_SIGNING_CLAIM",
            "NO_FLASH_OR_EFUSE_ACTION",
        ],
    }
    path = evidence_dir / "evidence.json"
    path.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    return path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--espsecure", required=True, type=Path)
    parser.add_argument("--espefuse", required=True, type=Path)
    args = parser.parse_args()
    espsecure = args.espsecure.resolve()
    espefuse = args.espefuse.resolve()
    assert espsecure.is_file()
    assert espefuse.is_file()

    with tempfile.TemporaryDirectory(prefix="kitsu-packager-e2e-") as temp_name:
        temp = Path(temp_name)
        candidate = temp / "candidate"
        candidate.mkdir()
        records = packager.expected_partition_records(
            ROOT / "partitions_kitsu_production_8MB.csv"
        )
        # Exercise the exact accepted boundary: a 0x7000 candidate receives
        # one 0x1000 SBv2 signature sector containing all three blocks and
        # ends exactly at the 0x8000 partition-table boundary.
        (candidate / "bootloader.bin").write_bytes(secrets.token_bytes(0x7000))
        (candidate / "partitions.bin").write_bytes(encoded_partition_table(records))
        (candidate / "ota_data_initial.bin").write_bytes(b"\xff" * 0x2000)
        (candidate / "firmware.bin").write_bytes(secrets.token_bytes(0x10000))

        signing_keys: list[Path] = []
        public_keys: list[Path] = []
        for name in ("primary", "recovery", "rotation"):
            key = temp / f"{name}.pem"
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
            signing_keys.append(key)
            public_key = temp / f"{name}-public.pem"
            extract_public_key(espsecure, key, public_key)
            public_keys.append(public_key)

        primary_bootloader_signature = temp / "primary-bootloader.sig"
        primary_app_signature = temp / "primary-app.sig"
        make_precomputed_signature(
            espsecure,
            signing_keys[0],
            candidate / "bootloader.bin",
            temp / "primary-bootloader-reference.bin",
            primary_bootloader_signature,
        )
        make_precomputed_signature(
            espsecure,
            signing_keys[0],
            candidate / "firmware.bin",
            temp / "primary-app-reference.bin",
            primary_app_signature,
        )
        primary_signer_ref = "tpm2-systemd-cred:test-fixture-primary"
        primary_evidence = make_systemd_credential_evidence(
            temp / "primary-evidence",
            public_key=public_keys[0],
            bootloader_signature=primary_bootloader_signature,
            application_signature=primary_app_signature,
            bootloader=candidate / "bootloader.bin",
            application=candidate / "firmware.bin",
            device_id="KT-TEST-0001",
            firmware_version="test-only",
            signer_ref=primary_signer_ref,
        )

        stages: list[Path] = []
        for index, role in enumerate(packager.SIGNING_ROLES):
            stage = temp / f"stage-{index + 1}-{role}"
            stage_command = [
                sys.executable,
                str(ROOT / "tools" / "sign_kitsu_production_stage.py"),
                "--role",
                role,
                "--input-dir",
                str(candidate if index == 0 else stages[index - 1]),
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
                    else f"offline-escrow:test-fixture-{role}"
                ),
                "--device-id",
                "KT-TEST-0001",
                "--firmware-version",
                "test-only",
            ]
            if index == 0:
                stage_command.extend(
                    (
                        "--bootloader-signature",
                        str(primary_bootloader_signature),
                        "--app-signature",
                        str(primary_app_signature),
                        "--signing-evidence",
                        str(primary_evidence),
                    )
                )
            else:
                stage_command.extend(("--signing-key", str(signing_keys[index])))
            run(stage_command)
            stage_manifest = json.loads(
                (stage / "signing-stage.json").read_text(encoding="utf-8")
            )
            assert stage_manifest["schema"] == packager.SIGNING_STAGE_SCHEMA
            assert stage_manifest["role"] == role
            assert stage_manifest["signature_blocks"] == index + 1
            assert (stage / "bootloader-signed.bin").stat().st_size == 0x8000
            assert (stage / "app-signed.bin").stat().st_size == 0x11000
            assert stage_manifest["custody"]["maximum_private_keys_in_process"] == 1
            assert stage_manifest["custody"]["private_key_material_in_stage"] is False
            assert not any(
                b"PRIVATE KEY" in path.read_bytes()
                for path in stage.rglob("*")
                if path.is_file()
            )
            verify_checksum_index(stage)
            stages.append(stage)

        # Production tooling must reject a plaintext primary private key.
        rejected_primary = temp / "rejected-primary-stage"
        rejected_primary_command = [
            sys.executable,
            str(ROOT / "tools" / "sign_kitsu_production_stage.py"),
            "--role",
            "primary",
            "--input-dir",
            str(candidate),
            "--output-dir",
            str(rejected_primary),
            "--espsecure",
            str(espsecure),
            "--public-key",
            str(public_keys[0]),
            "--signing-key",
            str(signing_keys[0]),
            "--signer-ref",
            "tpm2-pkcs11:test-fixture-primary",
            "--device-id",
            "KT-TEST-0001",
            "--firmware-version",
            "test-only",
        ]
        rejected = run(rejected_primary_command, expect_success=False)
        assert "plaintext primary signing key is forbidden" in (
            rejected.stdout + rejected.stderr
        )
        assert not rejected_primary.exists()

        # The isolated primary stage must refuse an unsigned bootloader that
        # would push its signature sector across the partition-table boundary.
        oversized_candidate = temp / "oversized-candidate"
        shutil.copytree(candidate, oversized_candidate)
        (oversized_candidate / "bootloader.bin").write_bytes(
            secrets.token_bytes(0x8000)
        )
        oversized_stage = temp / "rejected-oversized-stage"
        oversized_stage_command = [
            sys.executable,
            str(ROOT / "tools" / "sign_kitsu_production_stage.py"),
            "--role",
            "primary",
            "--input-dir",
            str(oversized_candidate),
            "--output-dir",
            str(oversized_stage),
            "--espsecure",
            str(espsecure),
            "--public-key",
            str(public_keys[0]),
            "--bootloader-signature",
            str(primary_bootloader_signature),
            "--app-signature",
            str(primary_app_signature),
            "--signing-evidence",
            str(primary_evidence),
            "--signer-ref",
            primary_signer_ref,
            "--device-id",
            "KT-TEST-0001",
            "--firmware-version",
            "test-only",
        ]
        rejected = run(oversized_stage_command, expect_success=False)
        assert "must leave one complete Secure Boot V2 signature sector" in (
            rejected.stdout + rejected.stderr
        )
        assert not oversized_stage.exists()
        xts_key = temp / "device-xts.bin"
        hmac_key = temp / "device-hmac.bin"
        xts_bytes = secrets.token_bytes(64)
        hmac_bytes = secrets.token_bytes(32)
        xts_key.write_bytes(xts_bytes)
        hmac_key.write_bytes(hmac_bytes)

        output = temp / "bundle"
        command = [
            sys.executable,
            str(ROOT / "tools" / "package_kitsu_production.py"),
            "--candidate-dir",
            str(candidate),
            "--signing-stage-dir",
            str(stages[-1]),
            "--output-dir",
            str(output),
            "--espsecure",
            str(espsecure),
            "--xts-key",
            str(xts_key),
            "--hmac-key",
            str(hmac_key),
            "--device-id",
            "KT-TEST-0001",
            "--firmware-version",
            "test-only",
        ]
        run(command)

        # The final assembler independently rejects the same unsigned
        # overflow before accepting or inspecting a signing ceremony.
        oversized_bundle = temp / "rejected-oversized-bundle"
        oversized_bundle_command = list(command)
        oversized_bundle_command[oversized_bundle_command.index(str(candidate))] = str(
            oversized_candidate
        )
        oversized_bundle_command[
            oversized_bundle_command.index(str(output))
        ] = str(oversized_bundle)
        rejected = run(oversized_bundle_command, expect_success=False)
        assert "must leave one complete Secure Boot V2 signature sector" in (
            rejected.stdout + rejected.stderr
        )
        assert not oversized_bundle.exists()

        # A signed-stage manifest cannot smuggle a bootloader beyond 0x8000,
        # even if its file checksum, byte count, and checksum index agree.
        oversized_signed_stage = temp / "rejected-oversized-signed-stage"
        shutil.copytree(stages[-1], oversized_signed_stage)
        oversized_signed_bootloader = (
            oversized_signed_stage / "bootloader-signed.bin"
        )
        oversized_signed_bootloader.write_bytes(
            oversized_signed_bootloader.read_bytes() + b"\xff" * 0x1000
        )
        oversized_manifest_path = oversized_signed_stage / "signing-stage.json"
        oversized_manifest = json.loads(
            oversized_manifest_path.read_text(encoding="utf-8")
        )
        oversized_manifest["images"][0]["bytes"] = (
            oversized_signed_bootloader.stat().st_size
        )
        oversized_manifest["images"][0]["sha256"] = packager.sha256(
            oversized_signed_bootloader
        )
        oversized_manifest_path.write_text(
            json.dumps(oversized_manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        packager.write_checksum_index(oversized_signed_stage)
        oversized_signed_bundle = temp / "rejected-oversized-signed-bundle"
        oversized_signed_command = list(command)
        oversized_signed_command[
            oversized_signed_command.index(str(stages[-1]))
        ] = str(oversized_signed_stage)
        oversized_signed_command[
            oversized_signed_command.index(str(output))
        ] = str(oversized_signed_bundle)
        rejected = run(oversized_signed_command, expect_success=False)
        assert "signed bootloader exceeds the partition-table boundary" in (
            rejected.stdout + rejected.stderr
        )
        assert not oversized_signed_bundle.exists()

        manifest_path = output / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        assert manifest["schema"] == packager.SCHEMA
        assert manifest["artifact_status"] == (
            "cryptographically_verified-awaiting-reviewed-provisioning"
        )
        assert manifest["partition_layout"]["verified_against_reviewed_csv"] is True
        assert manifest["partition_layout"]["entry_count"] == 9
        assert manifest["security_profile"]["signature_blocks"] == 3
        assert next(
            record
            for record in manifest["first_boot_plaintext_images"]
            if record["role"] == "bootloader"
        )["bytes"] == 0x8000
        assert manifest["signing_ceremony"] == {
            "schema": packager.SIGNING_STAGE_SCHEMA,
            "custody_model": "sequential-isolated-one-signer-per-stage",
            "final_stage_role": "rotation",
            "final_stage_manifest_sha256": packager.sha256(
                stages[-1] / "signing-stage.json"
            ),
            "final_stage_checksum_index_sha256": packager.sha256(
                stages[-1] / "SHA256SUMS.txt"
            ),
            "private_signing_keys_received_by_packager": False,
            "role_order": list(packager.SIGNING_ROLES),
        }
        assert len(manifest["secure_boot_public_key_digests"]) == 3
        assert len(
            {entry["sha256"] for entry in manifest["secure_boot_public_key_digests"]}
        ) == 3
        assert manifest["device_secret_fingerprints"]["xts_aes_256_key_sha256"] == (
            hashlib.sha256(xts_bytes).hexdigest().upper()
        )
        assert manifest["device_secret_fingerprints"]["hmac_up_key_sha256"] == (
            hashlib.sha256(hmac_bytes).hexdigest().upper()
        )
        assert len(manifest["first_boot_plaintext_images"]) == 5
        assert len(manifest["post_provision_encrypted_images"]) == 5
        assert all(
            record["secure_boot_v2_signature_blocks"] == 3
            for record in manifest["first_boot_plaintext_images"]
            if record["secure_boot_v2_signed"]
        )
        assert {warning["code"] for warning in manifest["warnings"]} == {
            "IRREVERSIBLE_EFUSE_CHECKPOINT",
            "SPLIT_SIGNING_CUSTODY_REQUIRED",
            "DEVICE_BOUND_XTS",
            "KEY_UNIQUENESS_EXTERNAL",
            "NVS_KEYS_RUNTIME_GENERATED",
            "DEVELOPMENT_CREDENTIALS_INVALIDATED",
            "PRESERVE_PLAINTEXT_REGIONS",
            "NO_DIRECT_FLASH",
        }
        serialized = manifest_path.read_text(encoding="utf-8")
        assert str(temp) not in serialized
        assert not any(
            b"PRIVATE KEY" in path.read_bytes()
            for path in output.rglob("*")
            if path.is_file()
        )
        verify_checksum_index(output)

        # Validate the exact one-confirmation six-block provisioning command
        # against espefuse's ESP32-S3 virtual eFuse model. Private RSA keys are
        # deliberately absent: the bundle's reviewed public-key digests burn
        # directly into their assigned readable digest blocks.
        virtual_efuse = temp / "virtual-esp32s3-efuse.bin"
        digest_files = [
            output / "metadata" / f"secure-boot-digest{index}.bin"
            for index in range(3)
        ]
        run(
            [
                sys.executable,
                str(espefuse),
                "--chip",
                "esp32s3",
                "--virt",
                "--path-efuse-file",
                str(virtual_efuse),
                "--do-not-confirm",
                "burn_key",
                "BLOCK_KEY0",
                str(digest_files[0]),
                "SECURE_BOOT_DIGEST0",
                "BLOCK_KEY1",
                str(xts_key),
                "XTS_AES_256_KEY",
                "BLOCK_KEY3",
                str(hmac_key),
                "HMAC_UP",
                "BLOCK_KEY4",
                str(digest_files[1]),
                "SECURE_BOOT_DIGEST1",
                "BLOCK_KEY5",
                str(digest_files[2]),
                "SECURE_BOOT_DIGEST2",
            ]
        )
        summary = run(
            [
                sys.executable,
                str(espefuse),
                "--chip",
                "esp32s3",
                "--virt",
                "--path-efuse-file",
                str(virtual_efuse),
                "summary",
            ]
        ).stdout
        for purpose in (
            "SECURE_BOOT_DIGEST0",
            "XTS_AES_256_KEY_1",
            "XTS_AES_256_KEY_2",
            "HMAC_UP",
            "SECURE_BOOT_DIGEST1",
            "SECURE_BOOT_DIGEST2",
        ):
            assert purpose in summary, purpose

        # A semantically altered table with a valid embedded MD5 must fail
        # before any partial output bundle is committed.
        wrong_records = [dict(record) for record in records]
        wrong_records[3]["offset"] = int(wrong_records[3]["offset"]) + 0x1000
        (candidate / "partitions.bin").write_bytes(encoded_partition_table(wrong_records))
        rejected_output = temp / "rejected-bundle"
        rejected_command = list(command)
        output_index = rejected_command.index("--output-dir") + 1
        rejected_command[output_index] = str(rejected_output)
        failure = run(rejected_command, expect_success=False)
        assert "does not exactly match" in (failure.stdout + failure.stderr)
        assert not rejected_output.exists()

    print("Kitsu production packager end-to-end tests passed")


if __name__ == "__main__":
    main()
