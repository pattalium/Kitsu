#!/usr/bin/env python3
"""Run one isolated Kitsu Secure Boot V2 signing-custody stage.

Each invocation accepts exactly one role and at most one private-key source.
The primary role must use a TPM2/PKCS#11 HSM or externally precomputed
signatures from the reviewed systemd-credential one-shot signer; a plaintext
primary private key is rejected. Safe public material, immutable image hashes,
custody evidence, and the signature chain are handed to the next role.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import package_kitsu_production as packager


SIGNER_REF_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:/+-]{0,127}\Z")
ARCHIVED_REFERENCE_ONLY = True
WITHDRAWN_MESSAGE = (
    "KITSU_WITHDRAWN_NOT_AUTHORIZED: secure-boot signing custody was withdrawn "
    "by owner decision. Reflashable releases require no signing key. This "
    "implementation remains only as forensic reference."
)


def signing_command(
    tool: Path,
    source: Path,
    destination: Path,
    *,
    append: bool,
    public_key: Path,
    signing_key: Path | None,
    hsm_config: Path | None,
    signature: Path | None,
) -> list[str]:
    command = packager.espsecure_command(tool) + ["sign_data", "--version", "2"]
    if append:
        command.append("--append_signatures")
    if signing_key is not None:
        command.extend(("--keyfile", str(signing_key)))
    elif hsm_config is not None:
        command.extend(
            (
                "--hsm",
                "--hsm-config",
                str(hsm_config),
                "--pub-key",
                str(public_key),
            )
        )
    elif signature is not None:
        command.extend(("--pub-key", str(public_key), "--signature", str(signature)))
    else:  # pragma: no cover - guarded before command construction
        raise SystemExit("no signing source was selected")
    command.extend(("--output", str(destination), str(source)))
    return command


def candidate_binding(path: Path, role: str) -> dict[str, Any]:
    return {
        "file": path.name,
        "role": role,
        "bytes": path.stat().st_size,
        "sha256": packager.sha256(path),
    }


def signed_binding(
    path: Path, role: str, source_sha256: str, signature_blocks: int
) -> dict[str, Any]:
    return {
        "file": path.name,
        "role": role,
        "bytes": path.stat().st_size,
        "sha256": packager.sha256(path),
        "source_sha256": source_sha256,
        "secure_boot_v2_signature_blocks": signature_blocks,
    }


def main() -> None:
    if ARCHIVED_REFERENCE_ONLY:
        raise SystemExit(WITHDRAWN_MESSAGE)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--role", choices=packager.SIGNING_ROLES, required=True)
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--espsecure", type=Path, required=True)
    parser.add_argument("--public-key", type=Path, required=True)
    parser.add_argument("--signing-key", type=Path)
    parser.add_argument("--hsm-config", type=Path)
    parser.add_argument("--bootloader-signature", type=Path)
    parser.add_argument("--app-signature", type=Path)
    parser.add_argument("--signing-evidence", type=Path)
    parser.add_argument("--signer-ref", required=True)
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--firmware-version", required=True)
    args = parser.parse_args()

    role = args.role
    stage_index = packager.SIGNING_ROLES.index(role)
    stage_number = stage_index + 1
    prior_role = packager.SIGNING_ROLES[stage_index - 1] if stage_index else None
    device_id = packager.require_token(args.device_id, "device ID")
    firmware_version = packager.require_token(
        args.firmware_version, "firmware version"
    )
    if not SIGNER_REF_PATTERN.fullmatch(args.signer_ref):
        raise SystemExit("signer reference must be 1..128 safe ASCII characters")
    if role != "primary" and not args.signer_ref.startswith("offline-escrow:"):
        raise SystemExit("recovery/rotation signer reference must start with offline-escrow:")

    tool = packager.require_file(args.espsecure, "espsecure tool")
    public_key = packager.require_file(args.public_key, "public verification key")
    public_bytes = public_key.read_bytes()
    if b"PRIVATE KEY" in public_bytes or b"BEGIN PUBLIC KEY" not in public_bytes:
        raise SystemExit("--public-key must contain only a PEM public key")

    signing_key = (
        packager.require_file(args.signing_key, "single-role signing key")
        if args.signing_key
        else None
    )
    hsm_config = (
        packager.require_file(args.hsm_config, "PKCS#11 HSM configuration")
        if args.hsm_config
        else None
    )
    has_external_signatures = bool(
        args.bootloader_signature or args.app_signature
    )
    if has_external_signatures and not (
        args.bootloader_signature and args.app_signature
    ):
        raise SystemExit("both external image signatures are required")
    bootloader_signature = (
        packager.require_file(args.bootloader_signature, "bootloader signature")
        if args.bootloader_signature
        else None
    )
    app_signature = (
        packager.require_file(args.app_signature, "application signature")
        if args.app_signature
        else None
    )
    signing_evidence = (
        packager.require_file(args.signing_evidence, "primary signing evidence")
        if args.signing_evidence
        else None
    )
    selected_modes = sum(
        (signing_key is not None, hsm_config is not None, has_external_signatures)
    )
    if selected_modes != 1:
        raise SystemExit(
            "select exactly one signer: --signing-key, --hsm-config, or the "
            "two external signature files"
        )
    if role == "primary" and signing_key is not None:
        raise SystemExit("a plaintext primary signing key is forbidden")
    if role == "primary" and hsm_config is not None:
        if not args.signer_ref.startswith("tpm2-pkcs11:"):
            raise SystemExit("PKCS#11 primary signer reference must start with tpm2-pkcs11:")
        if signing_evidence is not None:
            raise SystemExit("PKCS#11 primary signing does not accept precomputed evidence")
    elif role == "primary" and has_external_signatures:
        if not args.signer_ref.startswith("tpm2-systemd-cred:"):
            raise SystemExit(
                "systemd-credential primary signer reference must start with "
                "tpm2-systemd-cred:"
            )
        if signing_evidence is None:
            raise SystemExit("systemd-credential primary signatures require evidence")
    elif role == "primary":  # pragma: no cover - guarded by signer selection
        raise SystemExit("unsupported primary signing mode")
    elif has_external_signatures and signing_evidence is None:
        raise SystemExit("offline precomputed signatures require signing evidence")
    elif not has_external_signatures and signing_evidence is not None:
        raise SystemExit("offline signing evidence requires external signatures")
    signing_mode = (
        "single-private-key"
        if signing_key is not None
        else "pkcs11-hsm"
        if hsm_config is not None
        else "systemd-credential-precomputed"
        if role == "primary"
        else "external-precomputed"
    )

    input_dir = args.input_dir.expanduser().resolve()
    if not input_dir.is_dir():
        raise SystemExit(f"input directory is missing: {input_dir}")
    output_dir = args.output_dir.expanduser().resolve()
    if output_dir.exists() and not output_dir.is_dir():
        raise SystemExit("output path exists and is not a directory")
    if output_dir.exists() and any(output_dir.iterdir()):
        raise SystemExit("output directory must be absent or empty")

    prior_stage: dict[str, Any] | None = None
    if role == "primary":
        unsigned_bootloader = packager.require_file(
            input_dir / "bootloader.bin", "unsigned bootloader"
        )
        unsigned_app = packager.require_file(
            input_dir / "firmware.bin", "unsigned application"
        )
        if (
            unsigned_bootloader.stat().st_size
            > packager.UNSIGNED_BOOTLOADER_MAX_BYTES
        ):
            raise SystemExit(
                "unsigned bootloader must leave one complete Secure Boot V2 "
                "signature sector before the partition table"
            )
        if unsigned_bootloader.stat().st_size % 0x1000 != 0:
            raise SystemExit("unsigned bootloader is not aligned to 4 KiB")
        if unsigned_app.stat().st_size % 0x10000 != 0:
            raise SystemExit("unsigned application lacks 64 KiB secure padding")
        if packager.signature_block_digests(tool, unsigned_bootloader):
            raise SystemExit("primary stage input bootloader is already signed")
        if packager.signature_block_digests(tool, unsigned_app):
            raise SystemExit("primary stage input application is already signed")
        source_images = {
            "bootloader": unsigned_bootloader,
            "application": unsigned_app,
        }
        candidate_inputs = [
            candidate_binding(unsigned_bootloader, "bootloader"),
            candidate_binding(unsigned_app, "application"),
        ]
        prior_keys: list[dict[str, Any]] = []
        prior_manifest_digest: str | None = None
        if signing_evidence is not None:
            packager.validate_primary_signing_evidence(
                signing_evidence,
                public_key=public_key,
                signer_ref=args.signer_ref,
                device_id=device_id,
                firmware_version=firmware_version,
                candidate_inputs=candidate_inputs,
            )
    else:
        prior_stage = packager.validate_signing_stage(
            input_dir,
            tool,
            expected_device_id=device_id,
            expected_firmware_version=firmware_version,
            expected_role=prior_role,
        )
        source_images = prior_stage["image_paths"]
        candidate_inputs = prior_stage["manifest"]["candidate_inputs"]
        prior_keys = prior_stage["manifest"]["secure_boot_public_keys"]
        prior_manifest_digest = packager.sha256(prior_stage["manifest_path"])
        if signing_evidence is not None:
            packager.validate_offline_signing_evidence(
                signing_evidence,
                role=role,
                public_key=public_key,
                signer_ref=args.signer_ref,
                device_id=device_id,
                firmware_version=firmware_version,
                candidate_inputs=candidate_inputs,
            )

    output_dir.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(
        tempfile.mkdtemp(
            prefix=f".{output_dir.name}.staging-", dir=str(output_dir.parent)
        )
    )
    committed = False
    try:
        public_dir = staging / "public-keys"
        digest_dir = staging / "digests"
        public_dir.mkdir()
        digest_dir.mkdir()

        key_records: list[dict[str, Any]] = []
        if prior_stage is not None:
            for prior_key in prior_keys:
                source_public = prior_stage["root"] / prior_key["public_key_file"]
                source_digest = prior_stage["root"] / prior_key["digest_file"]
                destination_public = staging / prior_key["public_key_file"]
                destination_digest = staging / prior_key["digest_file"]
                packager.copy_image(source_public, destination_public)
                packager.copy_image(source_digest, destination_digest)
                evidence_file = prior_key["signing_evidence_file"]
                if evidence_file is not None:
                    source_evidence = prior_stage["root"] / evidence_file
                    source_evidence_dir = source_evidence.parent
                    destination_evidence_dir = (staging / evidence_file).parent
                    destination_evidence_dir.mkdir(parents=True, exist_ok=True)
                    for evidence_item in source_evidence_dir.iterdir():
                        if evidence_item.is_file():
                            packager.copy_image(
                                evidence_item, destination_evidence_dir / evidence_item.name
                            )
                key_records.append(dict(prior_key))

        role_public = public_dir / f"{role}.pem"
        role_digest = digest_dir / f"secure-boot-digest{stage_index}.bin"
        packager.copy_image(public_key, role_public)
        digest = packager.public_key_digest(tool, role_public, role_digest)
        evidence_relative: str | None = None
        evidence_sha256: str | None = None
        if signing_evidence is not None:
            evidence_destination_dir = staging / "evidence" / role
            evidence_destination_dir.mkdir(parents=True)
            for evidence_item in signing_evidence.parent.iterdir():
                if evidence_item.is_file():
                    packager.copy_image(
                        evidence_item, evidence_destination_dir / evidence_item.name
                    )
            evidence_destination = evidence_destination_dir / "evidence.json"
            evidence_relative = evidence_destination.relative_to(staging).as_posix()
            evidence_sha256 = packager.sha256(evidence_destination)
        key_records.append(
            {
                "purpose": role,
                "sequence": stage_index,
                "efuse_block": packager.SIGNING_BLOCKS[stage_index],
                "efuse_purpose": packager.SIGNING_PURPOSES[stage_index],
                "public_key_file": role_public.relative_to(staging).as_posix(),
                "public_key_sha256": packager.sha256(role_public),
                "digest_file": role_digest.relative_to(staging).as_posix(),
                "sha256": digest,
                "signing_mode": signing_mode,
                "signer_ref": args.signer_ref,
                "signing_evidence_file": evidence_relative,
                "signing_evidence_sha256": evidence_sha256,
            }
        )
        if len({record["sha256"] for record in key_records}) != stage_number:
            raise SystemExit("the current Secure Boot public key is not unique")
        if len({record["signer_ref"] for record in key_records}) != stage_number:
            raise SystemExit("the current signer custody reference is not unique")

        signed_bootloader = staging / "bootloader-signed.bin"
        signed_app = staging / "app-signed.bin"
        for image_role, source, destination, signature in (
            (
                "bootloader",
                source_images["bootloader"],
                signed_bootloader,
                bootloader_signature,
            ),
            ("application", source_images["application"], signed_app, app_signature),
        ):
            packager.run(
                signing_command(
                    tool,
                    source,
                    destination,
                    append=stage_number > 1,
                    public_key=role_public,
                    signing_key=signing_key,
                    hsm_config=hsm_config,
                    signature=signature,
                ),
                f"isolated {role} {image_role} signing",
            )
            candidate_bytes = next(
                record["bytes"]
                for record in candidate_inputs
                if record["role"] == image_role
            )
            if destination.stat().st_size != (
                candidate_bytes + packager.SECURE_BOOT_V2_SIGNATURE_SECTOR_BYTES
            ):
                raise SystemExit(
                    "signed image must contain exactly one 4 KiB Secure Boot "
                    "V2 signature sector"
                )
            if (
                image_role == "bootloader"
                and destination.stat().st_size
                > packager.SIGNED_BOOTLOADER_MAX_BYTES
            ):
                raise SystemExit(
                    "signed bootloader exceeds the partition-table boundary"
                )
            if packager.signature_block_digests(tool, destination) != [
                record["sha256"] for record in key_records
            ]:
                raise SystemExit("new signature block is missing, reordered, or invalid")
            for key_record in key_records:
                packager.verify_signature(
                    tool, staging / key_record["public_key_file"], destination
                )

        source_hashes = {
            record["role"]: record["sha256"] for record in candidate_inputs
        }
        manifest = {
            "schema": packager.SIGNING_STAGE_SCHEMA,
            "created_at": datetime.now(timezone.utc)
            .isoformat()
            .replace("+00:00", "Z"),
            "role": role,
            "stage_number": stage_number,
            "signature_blocks": stage_number,
            "device_id": device_id,
            "firmware_version": firmware_version,
            "input": {
                "kind": "candidate" if stage_number == 1 else "prior_signing_stage",
                "prior_stage_manifest_sha256": prior_manifest_digest,
            },
            "candidate_inputs": candidate_inputs,
            "images": [
                signed_binding(
                    signed_bootloader,
                    "bootloader",
                    source_hashes["bootloader"],
                    stage_number,
                ),
                signed_binding(
                    signed_app,
                    "application",
                    source_hashes["application"],
                    stage_number,
                ),
            ],
            "secure_boot_public_keys": key_records,
            "custody": {
                "model": "sequential-isolated-one-signer-per-stage",
                "maximum_private_keys_in_process": 1,
                "private_key_material_in_stage": False,
                "role_isolated": True,
            },
            "checksum_index": "SHA256SUMS.txt",
        }
        manifest_path = staging / "signing-stage.json"
        manifest_path.write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )
        packager.write_checksum_index(staging)
        packager.validate_signing_stage(
            staging,
            tool,
            expected_device_id=device_id,
            expected_firmware_version=firmware_version,
            expected_role=role,
        )

        if output_dir.exists():
            output_dir.rmdir()
        os.replace(staging, output_dir)
        committed = True
    finally:
        if not committed:
            shutil.rmtree(staging, ignore_errors=True)

    print(f"Kitsu {role} signing stage created: {output_dir / 'signing-stage.json'}")


if __name__ == "__main__":
    main()
