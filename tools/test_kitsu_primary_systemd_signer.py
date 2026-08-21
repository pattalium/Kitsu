#!/usr/bin/env python3
"""Host fixtures for the root-only systemd primary signer."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SIGNER_PATH = (
    ROOT
    / "platform"
    / "ops"
    / "kitsu-host"
    / "scripts"
    / "sign-firmware-primary-once.py"
)


def load_signer():
    spec = importlib.util.spec_from_file_location("kitsu_primary_signer", SIGNER_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


signer = load_signer()


def run(command: list[str]) -> None:
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    if completed.returncode:
        raise AssertionError(
            f"fixture command failed ({completed.returncode}): "
            f"{completed.stdout}{completed.stderr}"
        )


def openssl_path() -> Path:
    resolved = shutil.which("openssl")
    if resolved:
        return Path(resolved)
    git_openssl = Path(r"C:\Program Files\Git\usr\bin\openssl.exe")
    if git_openssl.is_file():
        return git_openssl
    raise AssertionError("OpenSSL is required for the primary-signer fixture")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def key_pair(root: Path, name: str, openssl: Path) -> tuple[Path, Path]:
    private = root / f"{name}-private.pem"
    public = root / f"{name}-public.pem"
    run(
        [
            str(openssl),
            "genpkey",
            "-algorithm",
            "RSA",
            "-pkeyopt",
            "rsa_keygen_bits:3072",
            "-out",
            str(private),
        ]
    )
    run(
        [
            str(openssl),
            "pkey",
            "-in",
            str(private),
            "-pubout",
            "-out",
            str(public),
        ]
    )
    return private, public


def write_candidate(input_dir: Path, *, bootloader_bytes: int = 0x7000) -> dict:
    input_dir.mkdir()
    bootloader = input_dir / "bootloader.bin"
    application = input_dir / "firmware.bin"
    bootloader.write_bytes(bytes((index % 251 for index in range(bootloader_bytes))))
    application.write_bytes(bytes(((index * 3) % 251 for index in range(0x10000))))
    request = {
        "schema": signer.REQUEST_SCHEMA,
        "request_id": "01234567-89ab-4def-8123-456789abcdef",
        "device_id": "KT-FIXTURE-0001",
        "firmware_version": "fixture-only",
        "candidate_inputs": [
            {
                "role": "bootloader",
                "file": "bootloader.bin",
                "bytes": bootloader.stat().st_size,
                "sha256": sha256(bootloader),
            },
            {
                "role": "application",
                "file": "firmware.bin",
                "bytes": application.stat().st_size,
                "sha256": sha256(application),
            },
        ],
    }
    (input_dir / "request.json").write_text(
        json.dumps(request, indent=2) + "\n", encoding="utf-8"
    )
    return request


def configuration(
    root: Path, input_dir: Path, private: Path, public: Path, openssl: Path
):
    root.mkdir(parents=True, exist_ok=True)
    return signer.Configuration(
        input_dir=input_dir,
        output_dir=root / "output",
        public_key=public,
        credential=private,
        signer_ref="tpm2-systemd-cred:fixture/primary",
        require_root=False,
        require_systemd=False,
        openssl=openssl,
        fixture_credential_filesystem="ramfs",
        fixture_network_private=True,
    )


def expect_failure(call, expected: str, secret: str = "") -> None:
    try:
        call()
    except SystemExit as error:
        message = str(error)
        assert message == expected, message
        if secret:
            assert secret not in message
    else:
        raise AssertionError("fixture unexpectedly succeeded")


def test_success(openssl: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="kitsu-primary-signer-success-") as name:
        root = Path(name)
        private, public = key_pair(root, "primary", openssl)
        request = write_candidate(root / "input")
        config = configuration(root, root / "input", private, public, openssl)
        evidence_path = signer.sign(config)
        output = config.output_dir
        assert {path.name for path in output.iterdir()} == {
            "bootloader.sig",
            "application.sig",
            "evidence.json",
        }
        evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
        assert set(evidence) == {
            "schema",
            "created_at",
            "request",
            "signer",
            "public_identity",
            "signatures",
            "controls",
            "warnings",
        }
        assert evidence["schema"] == signer.EVIDENCE_SCHEMA
        assert evidence["request"]["candidate_inputs"] == request["candidate_inputs"]
        assert evidence["public_identity"]["credential_matches_pinned_public"] is True
        assert [record["bytes"] for record in evidence["signatures"]] == [384, 384]
        assert all(record["scheme"] == "RSA-PSS-SHA256" for record in evidence["signatures"])
        assert evidence["controls"]["credential_filesystem"] == "ramfs"
        assert evidence["controls"]["network_namespace_private"] is True
        assert evidence["signer"]["private_key_exported"] is False
        assert not any(
            b"PRIVATE KEY" in path.read_bytes()
            for path in output.iterdir()
            if path.is_file()
        )


def test_schema_hash_and_boundary_rejection(openssl: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="kitsu-primary-signer-reject-") as name:
        root = Path(name)
        private, public = key_pair(root, "primary", openssl)

        schema_input = root / "schema-input"
        request = write_candidate(schema_input)
        request["unexpected"] = True
        (schema_input / "request.json").write_text(json.dumps(request), encoding="utf-8")
        expect_failure(
            lambda: signer.sign(configuration(root / "schema", schema_input, private, public, openssl)),
            "signing request has an invalid schema",
        )

        hash_input = root / "hash-input"
        write_candidate(hash_input)
        (hash_input / "firmware.bin").write_bytes(b"X" + (hash_input / "firmware.bin").read_bytes()[1:])
        expect_failure(
            lambda: signer.sign(configuration(root / "hash", hash_input, private, public, openssl)),
            "application candidate does not match its request binding",
        )

        boundary_input = root / "boundary-input"
        write_candidate(boundary_input, bootloader_bytes=0x8000)
        expect_failure(
            lambda: signer.sign(configuration(root / "boundary", boundary_input, private, public, openssl)),
            "bootloader candidate violates the Secure Boot V2 boundary",
        )


def test_extra_symlink_and_identity_rejection(openssl: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="kitsu-primary-signer-files-") as name:
        root = Path(name)
        private, public = key_pair(root, "primary", openssl)
        _, wrong_public = key_pair(root, "other", openssl)

        extra_input = root / "extra-input"
        write_candidate(extra_input)
        (extra_input / "unexpected.txt").write_text("not allowed", encoding="utf-8")
        expect_failure(
            lambda: signer.sign(configuration(root / "extra", extra_input, private, public, openssl)),
            "signing input directory must contain exactly the request and two candidates",
        )

        symlink_input = root / "symlink-input"
        write_candidate(symlink_input)
        real_bootloader = root / "real-bootloader.bin"
        (symlink_input / "bootloader.bin").replace(real_bootloader)
        try:
            os.symlink(real_bootloader, symlink_input / "bootloader.bin")
        except OSError:
            # Windows without Developer Mode cannot create symlinks.  The same
            # case runs unconditionally in the Linux production-host suite.
            pass
        else:
            expect_failure(
                lambda: signer.sign(configuration(root / "symlink", symlink_input, private, public, openssl)),
                "bootloader candidate must be a regular non-symlink file",
            )

        identity_input = root / "identity-input"
        write_candidate(identity_input)
        sentinel = "DO_NOT_LEAK_PRIMARY_MATERIAL"
        expect_failure(
            lambda: signer.sign(
                configuration(root / "identity", identity_input, private, wrong_public, openssl)
            ),
            "systemd credential does not match the pinned primary public identity",
            secret=sentinel,
        )


def main() -> None:
    openssl = openssl_path()
    test_success(openssl)
    test_schema_hash_and_boundary_rejection(openssl)
    test_extra_symlink_and_identity_rejection(openssl)
    print("Kitsu primary systemd signer fixture tests passed")


if __name__ == "__main__":
    main()
