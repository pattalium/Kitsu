#!/usr/bin/env python3
"""Independently audit a generated Kitsu stable firmware directory."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

import package_kitsu_stable as stable


EXPECTED_FILES = {
    "FACTORY_RESET.txt",
    "FLASHING.md",
    "INSTALLING_PACKS.md",
    "RECOVERY_PRESERVE_DATA.txt",
    "SHA256SUMS.txt",
    "UPGRADE_PRESERVE_DATA.txt",
    "manifest.json",
    "images/0x000000-bootloader.bin",
    "images/0x008000-partitions.bin",
    "images/0x009000-nvs-empty.bin",
    "images/0x00E000-ota-data-empty.bin",
    "images/0x010000-kitsu868.bin",
    "images/0x670000-pack-slot-empty.bin",
    "images/0x7B0000-connectivity-empty.bin",
    "images/0x7F0000-coredump-empty.bin",
    "metadata/partitions_kitsu_8MB.csv",
    "tools/install_pack.py",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def audited_firmware_version(
    manifest: dict[str, object], expected_firmware_version: str | None = None
) -> str:
    version = manifest.get("firmware_version")
    require(isinstance(version, str), "firmware version is not a string")
    try:
        version = stable.require_public_firmware_version(version)
    except SystemExit as error:
        raise AssertionError("firmware version is invalid") from error
    require(
        version == stable.EXPECTED_FIRMWARE_VERSION,
        "wrong firmware version for the physically accepted stable build",
    )
    if expected_firmware_version is not None:
        try:
            expected_firmware_version = stable.require_public_firmware_version(
                expected_firmware_version
            )
        except SystemExit as error:
            raise AssertionError("expected firmware version is invalid") from error
        require(version == expected_firmware_version, "wrong firmware version")
    return version


def audit(
    root: Path, *, expected_firmware_version: str | None = None
) -> dict[str, object]:
    require(root.is_dir() and not root.is_symlink(), "bundle root is invalid")
    actual_files = {
        path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file()
    }
    require(actual_files == EXPECTED_FILES, f"unexpected file set: {sorted(actual_files ^ EXPECTED_FILES)}")
    require(not any(path.is_symlink() for path in root.rglob("*")), "bundle contains symlinks")

    checksum_lines = (root / "SHA256SUMS.txt").read_text(encoding="ascii").splitlines()
    indexed: dict[str, str] = {}
    for line in checksum_lines:
        digest, relative = line.split("  ", 1)
        require(re.fullmatch(r"[0-9a-f]{64}", digest) is not None, "bad checksum syntax")
        require(relative not in indexed, "duplicate checksum entry")
        indexed[relative] = digest
        require(sha256(root / Path(*relative.split("/"))) == digest, f"hash mismatch: {relative}")
    require(set(indexed) == EXPECTED_FILES - {"SHA256SUMS.txt"}, "checksum index is incomplete")

    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    require(manifest["schema"] == stable.SCHEMA, "bad stable schema")
    require(manifest["artifact_status"] == stable.ARTIFACT_STATUS, "not available")
    require(manifest["release_channel"] == stable.RELEASE_CHANNEL, "not stable")
    firmware_version = audited_firmware_version(manifest, expected_firmware_version)
    require(manifest["supported_hardware"] == [
        {
            "vendor": "Heltec",
            "models": ["WiFi LoRa 32 V3", "WiFi LoRa 32 V3.2"],
            "chip": "ESP32-S3",
            "flash_bytes": 0x800000,
            "flash_mib": 8,
        }
    ], "hardware scope is not exact")
    authorization = manifest["physical_qa_authorization"]
    require(authorization["decision"] == "PASS", "QA decision is not PASS")
    require(authorization["authorization_sha256"] == stable.EXPECTED_AUTHORIZATION_SHA256, "bad auth hash")
    require(authorization["evidence_sha256"] == stable.EXPECTED_EVIDENCE_SHA256, "bad evidence hash")
    require(authorization["application_sha256"] == stable.EXPECTED_APPLICATION_SHA256, "bad app authorization")
    require(authorization["partition_table_sha256"] == stable.EXPECTED_PARTITIONS_SHA256, "bad table authorization")
    require(sha256(root / "images/0x010000-kitsu868.bin") == stable.EXPECTED_APPLICATION_SHA256, "wrong app")
    require(sha256(root / "images/0x008000-partitions.bin") == stable.EXPECTED_PARTITIONS_SHA256, "wrong table")

    empty_contract = {
        "images/0x009000-nvs-empty.bin": stable.NVS_BYTES,
        "images/0x00E000-ota-data-empty.bin": 0x2000,
        "images/0x670000-pack-slot-empty.bin": stable.PACK_BYTES,
        "images/0x7B0000-connectivity-empty.bin": stable.CONNECTIVITY_BYTES,
        "images/0x7F0000-coredump-empty.bin": stable.COREDUMP_BYTES,
    }
    for relative, length in empty_contract.items():
        data = (root / relative).read_bytes()
        require(len(data) == length and data == b"\xff" * length, f"bad erased-state image: {relative}")

    paths = manifest["installation_paths"]
    require(set(paths) == {"upgrade_preserve_data", "recovery_preserve_data", "factory_reset"}, "bad install paths")
    expected_offsets = {
        "upgrade_preserve_data": [0x8000, 0x10000, 0x340000],
        "recovery_preserve_data": [0, 0x8000, 0xE000, 0x10000, 0x340000],
        "factory_reset": [0, 0x8000, 0x9000, 0xE000, 0x10000, 0x340000, 0x670000, 0x7B0000, 0x7F0000],
    }
    for name, path in paths.items():
        require([entry["offset"] for entry in path["writes"]] == expected_offsets[name], f"bad {name} writes")
        require(path["whole_flash_erase"] is False and path["efuse_operations"] is False, f"unsafe {name}")
        command = " ".join(path["command"]).lower()
        require("write_flash" in command and "--verify" in command, f"unverified {name}")
        require(all(token not in command for token in ("erase_flash", "espefuse", "burn_", "--encrypt")), f"unsafe command {name}")

    pack_install = manifest["pack_installation"]
    require(pack_install["slot_offset"] == 0x670000 and pack_install["slot_bytes"] == 0x140000, "bad pack slot")
    require(pack_install["allowed_display_names"] == ["CAT", "FOX", "DOG"], "bad pack allow-list")
    guide = (root / "INSTALLING_PACKS.md").read_text(encoding="utf-8")
    for name in stable.public_pack_asset_names(firmware_version):
        require(name in guide, f"pack guide omits {name}")
    flashing = (root / "FLASHING.md").read_text(encoding="utf-8")
    require(
        f"# Kitsu owner-reflashable firmware {firmware_version}" in flashing,
        "flashing guide has the wrong firmware version",
    )
    require("--dry-run" in guide and "0x670000" in guide, "pack guide is not actionable")

    animal = "".join(chr(value) for value in (102, 111, 120))
    person = "".join(chr(value) for value in (103, 105, 114, 108))
    blocked = (f"{animal}-{person}", f"{animal}_{person}", f"{animal} {person}")
    for path in root.rglob("*"):
        relative = path.relative_to(root).as_posix().lower()
        require(not any(token in relative for token in blocked), "non-public label in filename")
        if path.is_file() and path.suffix.lower() in {".md", ".txt", ".json", ".py", ".csv"}:
            lowered = path.read_text(encoding="utf-8", errors="ignore").lower()
            require(not any(token in lowered for token in blocked), f"non-public label in {relative}")

    return {
        "schema": manifest["schema"],
        "status": manifest["artifact_status"],
        "channel": manifest["release_channel"],
        "firmware_version": firmware_version,
        "file_count": len(actual_files),
        "application_sha256": stable.EXPECTED_APPLICATION_SHA256,
        "partition_table_sha256": stable.EXPECTED_PARTITIONS_SHA256,
        "bundle_manifest_sha256": sha256(root / "manifest.json"),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument(
        "--expected-firmware-version",
        help="optionally repeat the exact intrinsically accepted stable version",
    )
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args()
    try:
        result = audit(
            args.bundle.resolve(),
            expected_firmware_version=args.expected_firmware_version,
        )
    except (AssertionError, OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"KITSU_STABLE_BUNDLE_AUDIT_FAIL {error}", file=sys.stderr)
        return 1
    if args.json_out:
        args.json_out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("KITSU_STABLE_BUNDLE_AUDIT_PASS " + " ".join(f"{key}={value}" for key, value in result.items()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
