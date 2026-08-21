#!/usr/bin/env python3
"""Positive and tamper tests for the authorization-gated stable packager."""

from __future__ import annotations

import hashlib
import json
import tempfile
from pathlib import Path

import package_kitsu_stable as stable


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expect_failure(function, text: str) -> None:
    try:
        function()
    except SystemExit as error:
        assert text in str(error), (text, error)
    else:
        raise AssertionError("operation unexpectedly passed")


def test_pinned_release_inputs() -> None:
    assert stable.EXPECTED_FIRMWARE_VERSION == "0.11.0"
    assert stable.EXPECTED_AUTHORIZATION_SHA256 == (
        "0321762cb41b9c65a9ecc8d0afd211cfa50f1bf8f27225c0c5764b7d9c729a7f"
    )
    assert stable.EXPECTED_EVIDENCE_SHA256 == (
        "56dbbf1b92189e17dd809865c6a4453d2a20df1ffb8a9dce40571fd55a771f4c"
    )
    assert stable.EXPECTED_APPLICATION_SHA256 == (
        "7196bb7b16d169a33b4dffc484ac3ea8af06369530e442c0373c47f78e91f5bd"
    )
    assert stable.EXPECTED_PARTITIONS_SHA256 == (
        "f9b22e16fcfb701520dd6c7e0791582ececbbd44c317c8d519e3d6b2b9ce8b7a"
    )
    assert stable.ARTIFACT_STATUS == "available-owner-reflashable"
    assert stable.RELEASE_CHANNEL == "stable"


def test_authorization_positive_and_tamper_guards() -> None:
    with tempfile.TemporaryDirectory(prefix="kitsu-stable-auth-") as temp:
        root = Path(temp)
        evidence = root / "evidence.json"
        app = root / "firmware.bin"
        partitions = root / "partitions.bin"
        authorization = root / "authorization.json"
        evidence.write_text('{"physical":"accepted"}\n', encoding="utf-8")
        app.write_bytes(b"accepted application fixture")
        partitions.write_bytes(b"accepted partition fixture")
        authorization.write_text(
            json.dumps(
                {
                    "schema": stable.AUTHORIZATION_SCHEMA,
                    "decision": "PASS",
                    "accepted_at": "2026-08-18T00:00:00Z",
                    "evidence_sha256": digest(evidence),
                    "application_sha256": digest(app),
                    "partition_table_sha256": digest(partitions),
                },
                separators=(",", ":"),
            ),
            encoding="utf-8",
        )

        original = (
            stable.EXPECTED_AUTHORIZATION_SHA256,
            stable.EXPECTED_EVIDENCE_SHA256,
            stable.EXPECTED_APPLICATION_SHA256,
            stable.EXPECTED_PARTITIONS_SHA256,
        )
        stable.EXPECTED_AUTHORIZATION_SHA256 = digest(authorization)
        stable.EXPECTED_EVIDENCE_SHA256 = digest(evidence)
        stable.EXPECTED_APPLICATION_SHA256 = digest(app)
        stable.EXPECTED_PARTITIONS_SHA256 = digest(partitions)
        try:
            public = stable.validate_authorization(
                authorization, evidence, app, partitions
            )
            assert set(public) == {
                "decision",
                "accepted_at",
                "authorization_sha256",
                "evidence_sha256",
                "application_sha256",
                "partition_table_sha256",
            }
            assert public["decision"] == "PASS"

            evidence.write_bytes(evidence.read_bytes() + b"tamper")
            expect_failure(
                lambda: stable.validate_authorization(
                    authorization, evidence, app, partitions
                ),
                "evidence digest",
            )
            evidence.write_text('{"physical":"accepted"}\n', encoding="utf-8")
            app.write_bytes(app.read_bytes() + b"tamper")
            expect_failure(
                lambda: stable.validate_authorization(
                    authorization, evidence, app, partitions
                ),
                "application digest",
            )
        finally:
            (
                stable.EXPECTED_AUTHORIZATION_SHA256,
                stable.EXPECTED_EVIDENCE_SHA256,
                stable.EXPECTED_APPLICATION_SHA256,
                stable.EXPECTED_PARTITIONS_SHA256,
            ) = original


def test_installation_path_contracts() -> None:
    app = "images/0x010000-kitsu868.bin"
    upgrade = stable.installation_path(
        guide="UPGRADE_PRESERVE_DATA.txt",
        writes=[
            (0x8000, "images/0x008000-partitions.bin", "partition_table"),
            (0x10000, app, "app0"),
            (0x340000, app, "app1"),
        ],
        resets=[],
        preserves=["nvs", "otadata", "pack_slot", "connectivity"],
    )
    assert [entry["offset"] for entry in upgrade["writes"]] == [
        0x8000,
        0x10000,
        0x340000,
    ]
    assert upgrade["writes"][1]["file"] == upgrade["writes"][2]["file"] == app
    assert upgrade["whole_flash_erase"] is False
    assert upgrade["efuse_operations"] is False
    command = " ".join(upgrade["command"]).lower()
    assert "write_flash" in command and "--verify" in command
    assert all(token not in command for token in ("erase_flash", "espefuse", "burn_", "--encrypt"))


def test_factory_empty_image_contract() -> None:
    expected = {
        "nvs": stable.NVS_BYTES,
        "otadata": 0x2000,
        "pack_slot": stable.PACK_BYTES,
        "connectivity": stable.CONNECTIVITY_BYTES,
        "coredump": stable.COREDUMP_BYTES,
    }
    with tempfile.TemporaryDirectory(prefix="kitsu-stable-empty-") as temp:
        root = Path(temp)
        for name, length in expected.items():
            path = root / f"{name}.bin"
            path.write_bytes(b"\xff" * length)
            assert path.stat().st_size == length
            assert digest(path) == hashlib.sha256(b"\xff" * length).hexdigest()


def main() -> None:
    test_pinned_release_inputs()
    test_authorization_positive_and_tamper_guards()
    test_installation_path_contracts()
    test_factory_empty_image_contract()
    print("KITSU_STABLE_PACKAGER_TEST_PASS authorization=positive+tamper paths=3 empty-images=5")


if __name__ == "__main__":
    main()
