#!/usr/bin/env python3
"""Cross-tool fixture for the Windows owner-custody ``sign-cms`` path.

All recipient and signing identities are generated under a TemporaryDirectory
and removed when the fixture exits.  This test never uses production custody
paths, a serial port, device secrets, esptool, or espefuse.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import secrets
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.hazmat.primitives.ciphers import algorithms
from cryptography.hazmat.primitives.serialization.pkcs7 import (
    PKCS7EnvelopeBuilder,
    PKCS7Options,
)

import package_kitsu_production as packager
import test_package_kitsu_production as package_fixture


ROOT = Path(__file__).resolve().parents[1]
WORK_ROOT = ROOT.parent
HELPER_PROJECT = WORK_ROOT / "owner-custody-tools" / "OwnerCustodyTool.csproj"
HELPER_DLL = (
    WORK_ROOT
    / "owner-custody-tools"
    / "bin"
    / "Release"
    / "net8.0-windows"
    / "OwnerCustodyTool.dll"
)


def run(
    command: list[str], *, expect_success: bool = True
) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    if expect_success and completed.returncode != 0:
        raise AssertionError(
            f"command failed ({completed.returncode}):\n"
            f"{completed.stdout}\n{completed.stderr}"
        )
    if not expect_success and completed.returncode == 0:
        raise AssertionError("command unexpectedly succeeded")
    return completed


def helper(*arguments: object, expect_success: bool = True) -> subprocess.CompletedProcess[str]:
    return run(
        ["dotnet", str(HELPER_DLL), *(str(argument) for argument in arguments)],
        expect_success=expect_success,
    )


def request_document(candidate: Path) -> dict[str, object]:
    return {
        "schema": packager.OFFLINE_SIGNING_REQUEST_SCHEMA,
        "request_id": str(uuid.UUID("12345678-9abc-4def-8123-456789abcdef")),
        "role": "recovery",
        "device_id": "KT-CMS-FIXTURE",
        "firmware_version": "fixture-only",
        "signer_ref": "offline-escrow:kitsu-owner-firmware-recovery-v1",
        "candidate_inputs": [
            {
                "role": "bootloader",
                "file": "bootloader.bin",
                "bytes": (candidate / "bootloader.bin").stat().st_size,
                "sha256": packager.sha256(candidate / "bootloader.bin"),
            },
            {
                "role": "application",
                "file": "firmware.bin",
                "bytes": (candidate / "firmware.bin").stat().st_size,
                "sha256": packager.sha256(candidate / "firmware.bin"),
            },
        ],
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--espsecure", required=True, type=Path)
    args = parser.parse_args()
    espsecure = args.espsecure.resolve()
    assert espsecure.is_file()
    assert HELPER_PROJECT.is_file()
    run(["dotnet", "build", str(HELPER_PROJECT), "--configuration", "Release"])
    assert HELPER_DLL.is_file()

    with tempfile.TemporaryDirectory(prefix="kitsu-owner-cms-fixture-") as temp_name:
        temp = Path(temp_name)
        private_dir = temp / "private"
        public_dir = temp / "public"
        candidate = temp / "candidate"
        private_dir.mkdir()
        public_dir.mkdir()
        candidate.mkdir()
        (candidate / "bootloader.bin").write_bytes(secrets.token_bytes(0x7000))
        (candidate / "firmware.bin").write_bytes(secrets.token_bytes(0x10000))

        helper("generate", "firmware-recovery", private_dir, public_dir)
        recipient_certificate = x509.load_pem_x509_certificate(
            (public_dir / "firmware-recovery.recipient-cert.pem").read_bytes()
        )

        signing_key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        signing_private_pem = signing_key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
        signing_public = temp / "recovery-public.pem"
        signing_public.write_bytes(
            signing_key.public_key().public_bytes(
                serialization.Encoding.PEM,
                serialization.PublicFormat.SubjectPublicKeyInfo,
            )
        )
        cms_path = temp / "firmware-recovery-signing-key.p7m"
        cms_path.write_bytes(
            PKCS7EnvelopeBuilder()
            .set_data(signing_private_pem)
            .set_content_encryption_algorithm(algorithms.AES256)
            .add_recipient(recipient_certificate)
            .encrypt(serialization.Encoding.DER, [PKCS7Options.Binary])
        )

        request = request_document(candidate)
        request_path = temp / "request.json"
        request_path.write_text(json.dumps(request, indent=2) + "\n", encoding="utf-8")

        malformed_request = dict(request)
        malformed_request["unexpected"] = True
        malformed_path = temp / "malformed-request.json"
        malformed_path.write_text(
            json.dumps(malformed_request, indent=2) + "\n", encoding="utf-8"
        )
        rejected_output = temp / "rejected-extra-field"
        rejected = helper(
            "sign-cms",
            "firmware-recovery",
            private_dir,
            public_dir,
            cms_path,
            signing_public,
            malformed_path,
            candidate,
            rejected_output,
            expect_success=False,
        )
        assert "signing request has an invalid schema" in rejected.stderr
        assert not rejected_output.exists()

        mismatched_key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        mismatched_public = temp / "mismatched-public.pem"
        mismatched_public.write_bytes(
            mismatched_key.public_key().public_bytes(
                serialization.Encoding.PEM,
                serialization.PublicFormat.SubjectPublicKeyInfo,
            )
        )
        mismatch_output = temp / "rejected-key-mismatch"
        rejected = helper(
            "sign-cms",
            "firmware-recovery",
            private_dir,
            public_dir,
            cms_path,
            mismatched_public,
            request_path,
            candidate,
            mismatch_output,
            expect_success=False,
        )
        assert "cryptographic operation failed" in rejected.stderr
        assert not mismatch_output.exists()

        result = temp / "recovery-signatures"
        completed = helper(
            "sign-cms",
            "firmware-recovery",
            private_dir,
            public_dir,
            cms_path,
            signing_public,
            request_path,
            candidate,
            result,
        )
        assert "signature_count=2" in completed.stdout
        assert {item.name for item in result.iterdir()} == {
            "evidence.json",
            "bootloader.sig",
            "application.sig",
        }
        evidence = packager.validate_offline_signing_evidence(
            result / "evidence.json",
            role="recovery",
            public_key=signing_public,
            signer_ref="offline-escrow:kitsu-owner-firmware-recovery-v1",
            device_id="KT-CMS-FIXTURE",
            firmware_version="fixture-only",
            candidate_inputs=request["candidate_inputs"],  # type: ignore[arg-type]
        )
        assert evidence["document"]["controls"]["plaintext_private_key_file"] is False
        assert evidence["document"]["controls"]["private_buffers_zeroed"] is True
        for image_role, image_file in (
            ("bootloader", "bootloader.bin"),
            ("application", "firmware.bin"),
        ):
            signature = (result / f"{image_role}.sig").read_bytes()
            assert len(signature) == 384
            signing_key.public_key().verify(
                signature,
                (candidate / image_file).read_bytes(),
                padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32),
                hashes.SHA256(),
            )

        primary_key = temp / "primary.pem"
        package_fixture.run(
            package_fixture.tool_command(espsecure)
            + [
                "generate_signing_key",
                "--version",
                "2",
                "--scheme",
                "rsa3072",
                str(primary_key),
            ]
        )
        primary_public = temp / "primary-public.pem"
        package_fixture.extract_public_key(espsecure, primary_key, primary_public)
        primary_boot_sig = temp / "primary-bootloader.sig"
        primary_app_sig = temp / "primary-application.sig"
        package_fixture.make_precomputed_signature(
            espsecure,
            primary_key,
            candidate / "bootloader.bin",
            temp / "primary-bootloader-reference.bin",
            primary_boot_sig,
        )
        package_fixture.make_precomputed_signature(
            espsecure,
            primary_key,
            candidate / "firmware.bin",
            temp / "primary-application-reference.bin",
            primary_app_sig,
        )
        primary_evidence = package_fixture.make_systemd_credential_evidence(
            temp / "primary-evidence",
            public_key=primary_public,
            bootloader_signature=primary_boot_sig,
            application_signature=primary_app_sig,
            bootloader=candidate / "bootloader.bin",
            application=candidate / "firmware.bin",
            device_id="KT-CMS-FIXTURE",
            firmware_version="fixture-only",
            signer_ref="tpm2-systemd-cred:fixture-primary",
        )
        primary_stage = temp / "primary-stage"
        run(
            [
                sys.executable,
                str(ROOT / "tools" / "sign_kitsu_production_stage.py"),
                "--role",
                "primary",
                "--input-dir",
                str(candidate),
                "--output-dir",
                str(primary_stage),
                "--espsecure",
                str(espsecure),
                "--public-key",
                str(primary_public),
                "--bootloader-signature",
                str(primary_boot_sig),
                "--app-signature",
                str(primary_app_sig),
                "--signing-evidence",
                str(primary_evidence),
                "--signer-ref",
                "tpm2-systemd-cred:fixture-primary",
                "--device-id",
                "KT-CMS-FIXTURE",
                "--firmware-version",
                "fixture-only",
            ]
        )
        recovery_stage = temp / "recovery-stage"
        run(
            [
                sys.executable,
                str(ROOT / "tools" / "sign_kitsu_production_stage.py"),
                "--role",
                "recovery",
                "--input-dir",
                str(primary_stage),
                "--output-dir",
                str(recovery_stage),
                "--espsecure",
                str(espsecure),
                "--public-key",
                str(signing_public),
                "--bootloader-signature",
                str(result / "bootloader.sig"),
                "--app-signature",
                str(result / "application.sig"),
                "--signing-evidence",
                str(result / "evidence.json"),
                "--signer-ref",
                "offline-escrow:kitsu-owner-firmware-recovery-v1",
                "--device-id",
                "KT-CMS-FIXTURE",
                "--firmware-version",
                "fixture-only",
            ]
        )
        validated = packager.validate_signing_stage(
            recovery_stage,
            espsecure,
            expected_device_id="KT-CMS-FIXTURE",
            expected_firmware_version="fixture-only",
            expected_role="recovery",
        )
        assert validated["manifest"]["signature_blocks"] == 2
        recovery_key = validated["manifest"]["secure_boot_public_keys"][1]
        assert recovery_key["signing_mode"] == "external-precomputed"
        assert recovery_key["signing_evidence_file"] == "evidence/recovery/evidence.json"
        assert not any(
            b"PRIVATE KEY" in path.read_bytes()
            for path in recovery_stage.rglob("*")
            if path.is_file()
        )

        tampered = json.loads((result / "evidence.json").read_text(encoding="utf-8"))
        tampered["controls"]["plaintext_private_key_file"] = True
        (result / "evidence.json").write_text(
            json.dumps(tampered, indent=2) + "\n", encoding="utf-8"
        )
        try:
            packager.validate_offline_signing_evidence(
                result / "evidence.json",
                role="recovery",
                public_key=signing_public,
                signer_ref="offline-escrow:kitsu-owner-firmware-recovery-v1",
                device_id="KT-CMS-FIXTURE",
                firmware_version="fixture-only",
                candidate_inputs=request["candidate_inputs"],  # type: ignore[arg-type]
            )
        except SystemExit as error:
            assert "memory/atomic controls" in str(error)
        else:
            raise AssertionError("tampered offline evidence unexpectedly validated")

        # Avoid retaining an additional immutable fixture copy longer than needed.
        assert hashlib.sha256(signing_private_pem).digest()

    print("owner custody sign-cms cross-tool fixture: PASS")


if __name__ == "__main__":
    main()
