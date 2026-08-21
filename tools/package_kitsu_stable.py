#!/usr/bin/env python3
"""Create the authorization-gated stable owner-reflashable firmware bundle.

The stable path is intentionally separate from the candidate packager. It
requires the exact retained physical-QA authorization and evidence digests,
revalidates the authorized application and partition table, and never copies
the private evidence document into public output.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import package_kitsu_reflashable as base


SCHEMA = "kitsu.firmware-owner-reflashable-release.v2"
ARTIFACT_STATUS = "available-owner-reflashable"
RELEASE_CHANNEL = "stable"
AUTHORIZATION_SCHEMA = "kitsu.firmware-publication-authorization.v1"

# These values are one intrinsic physical-acceptance identity. A version label
# is not transferable to another application or partition-table build.
EXPECTED_FIRMWARE_VERSION = "0.11.0"
EXPECTED_AUTHORIZATION_SHA256 = (
    "0321762cb41b9c65a9ecc8d0afd211cfa50f1bf8f27225c0c5764b7d9c729a7f"
)
EXPECTED_EVIDENCE_SHA256 = (
    "56dbbf1b92189e17dd809865c6a4453d2a20df1ffb8a9dce40571fd55a771f4c"
)
EXPECTED_APPLICATION_SHA256 = (
    "7196bb7b16d169a33b4dffc484ac3ea8af06369530e442c0373c47f78e91f5bd"
)
EXPECTED_PARTITIONS_SHA256 = (
    "f9b22e16fcfb701520dd6c7e0791582ececbbd44c317c8d519e3d6b2b9ce8b7a"
)

APP1_OFFSET = 0x340000
NVS_OFFSET = 0x009000
NVS_BYTES = 0x005000
PACK_OFFSET = 0x670000
PACK_BYTES = 0x140000
CONNECTIVITY_OFFSET = 0x7B0000
CONNECTIVITY_BYTES = 0x040000
COREDUMP_OFFSET = 0x7F0000
COREDUMP_BYTES = 0x010000
PUBLIC_PACK_SPECIES = ("cat", "fox", "dog")
PUBLIC_FIRMWARE_VERSION_PATTERN = re.compile(
    r"[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?"
)


def checked_json(path: Path, description: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SystemExit(f"{description} is not valid UTF-8 JSON") from error
    if not isinstance(value, dict):
        raise SystemExit(f"{description} must be a JSON object")
    return value


def validate_authorization(
    authorization_path: Path,
    evidence_path: Path,
    application: Path,
    partitions: Path,
) -> dict[str, str]:
    authorization_hash = base.sha256(authorization_path)
    evidence_hash = base.sha256(evidence_path)
    application_hash = base.sha256(application)
    partitions_hash = base.sha256(partitions)
    if authorization_hash != EXPECTED_AUTHORIZATION_SHA256:
        raise SystemExit("physical-QA authorization digest is not the reviewed digest")
    if evidence_hash != EXPECTED_EVIDENCE_SHA256:
        raise SystemExit("physical-QA evidence digest is not the retained reviewed digest")
    if application_hash != EXPECTED_APPLICATION_SHA256:
        raise SystemExit("application digest is not the physically accepted image")
    if partitions_hash != EXPECTED_PARTITIONS_SHA256:
        raise SystemExit("partition digest is not the physically accepted table")

    authorization = checked_json(authorization_path, "physical-QA authorization")
    required = {
        "schema",
        "decision",
        "accepted_at",
        "evidence_sha256",
        "application_sha256",
        "partition_table_sha256",
    }
    if set(authorization) != required:
        raise SystemExit("physical-QA authorization has unexpected or missing fields")
    if authorization["schema"] != AUTHORIZATION_SCHEMA:
        raise SystemExit("physical-QA authorization schema is unsupported")
    if authorization["decision"] != "PASS":
        raise SystemExit("physical-QA authorization decision is not PASS")
    if authorization["evidence_sha256"] != evidence_hash:
        raise SystemExit("authorization does not bind the retained physical-QA evidence")
    if authorization["application_sha256"] != application_hash:
        raise SystemExit("authorization does not bind this application")
    if authorization["partition_table_sha256"] != partitions_hash:
        raise SystemExit("authorization does not bind this partition table")
    if not isinstance(authorization["accepted_at"], str) or not authorization["accepted_at"]:
        raise SystemExit("physical-QA authorization has no acceptance timestamp")

    # Only non-sensitive hashes and the decision cross the public boundary.
    return {
        "decision": "PASS",
        "accepted_at": authorization["accepted_at"],
        "authorization_sha256": authorization_hash,
        "evidence_sha256": evidence_hash,
        "application_sha256": application_hash,
        "partition_table_sha256": partitions_hash,
    }


def flash_command(entries: list[tuple[int, str]]) -> list[str]:
    command = [
        "python",
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "--before",
        "default_reset",
        "--after",
        "hard_reset",
        "write_flash",
        "--flash_mode",
        "dio",
        "--flash_freq",
        "80m",
        "--flash_size",
        "8MB",
        "--verify",
    ]
    for offset, file_name in entries:
        command.extend((f"0x{offset:06X}", file_name))
    text = " ".join(command).lower()
    if any(token in text for token in ("erase_flash", "espefuse", "burn_", "--encrypt")):
        raise SystemExit("generated command contains a forbidden operation")
    return command


def installation_path(
    *,
    guide: str,
    writes: list[tuple[int, str, str]],
    resets: list[str],
    preserves: list[str],
) -> dict[str, Any]:
    return {
        "guide": guide,
        "writes": [
            {
                "offset": offset,
                "offset_hex": f"0x{offset:06X}",
                "file": file_name,
                "target": target,
            }
            for offset, file_name, target in writes
        ],
        "resets": resets,
        "preserves": preserves,
        "whole_flash_erase": False,
        "efuse_operations": False,
        "command": flash_command([(offset, file_name) for offset, file_name, _ in writes]),
    }


def artifact_record(
    path: Path,
    *,
    role: str,
    install_targets: list[tuple[str, int]],
    esp_validation: dict[str, Any] | None = None,
) -> dict[str, Any]:
    return {
        "file": f"images/{path.name}",
        "role": role,
        "bytes": path.stat().st_size,
        "sha256": base.sha256(path),
        "install_targets": [
            {"target": target, "offset": offset, "offset_hex": f"0x{offset:06X}"}
            for target, offset in install_targets
        ],
        "secure_boot_signed": False,
        "encrypted": False,
        "esp_image_verified": esp_validation is not None,
        **({"esp_validation": esp_validation} if esp_validation else {}),
    }


def command_text(command: list[str]) -> str:
    return " ".join(command)


def require_public_firmware_version(value: str) -> str:
    """Validate the same bounded public version grammar as the flash site."""
    if (
        not isinstance(value, str)
        or len(value) > 64
        or PUBLIC_FIRMWARE_VERSION_PATTERN.fullmatch(value) is None
    ):
        raise SystemExit(
            "firmware version must match the public SemVer grammar "
            "MAJOR.MINOR.PATCH with one optional -/+ suffix (maximum 64 characters)"
        )
    return value


def require_expected_firmware_version(value: str) -> str:
    """Require the version belonging to the fixed physically accepted images."""
    version = require_public_firmware_version(value)
    if version != EXPECTED_FIRMWARE_VERSION:
        raise SystemExit(
            "firmware version does not match the physically accepted stable build: "
            f"expected {EXPECTED_FIRMWARE_VERSION}"
        )
    return version


def public_pack_asset_names(firmware_version: str) -> tuple[str, ...]:
    """Return the public pack asset names for one validated firmware version."""
    version = require_public_firmware_version(firmware_version)
    return tuple(
        f"Kitsu868-v{version}-{species}.k868" for species in PUBLIC_PACK_SPECIES
    )


def installing_packs_guide(firmware_version: str) -> str:
    cat_asset, fox_asset, dog_asset = public_pack_asset_names(firmware_version)
    return (
        "# Installing a Cat, Fox, or Dog companion pack\n\n"
        "Download exactly one `.k868` release asset next to this extracted firmware folder.\n"
        f"The release filenames are `{cat_asset}`, `{fox_asset}`, "
        f"and `{dog_asset}`. A local rename is allowed because the installer validates "
        "the file contents rather than trusting its basename.\n\n"
        "Install Python 3, PlatformIO (for its local esptool), and connect the supported Heltec by USB.\n"
        "First validate without writing, replacing `COM3` and the pack path as needed:\n\n"
        f"```text\npython tools/install_pack.py ../{cat_asset} --port COM3 --dry-run\n```\n\n"
        "The validator must report `PACK_VALID`, a 64x64 frame canvas, and a write beginning at `0x670000`.\n"
        "Then install the same validated file:\n\n"
        f"```text\npython tools/install_pack.py ../{cat_asset} --port COM3\n```\n\n"
        "The installer writes only the 1.25 MiB companion slot. It does not erase the whole chip and preserves "
        "the bootloader, partition table, NVS, OTA apps, connectivity partition, coredump, and hardware-fuse state.\n"
        "Installing another pack replaces the current companion pack only.\n"
    )


def flashing_guide(firmware_version: str) -> str:
    version = require_public_firmware_version(firmware_version)
    return (
        f"# Kitsu owner-reflashable firmware {version}\n\n"
        "This stable bundle is for Heltec WiFi LoRa 32 V3/V3.2 boards with exactly 8 MiB flash.\n"
        "Choose one path: `UPGRADE_PRESERVE_DATA.txt`, `RECOVERY_PRESERVE_DATA.txt`, or `FACTORY_RESET.txt`.\n"
        "Install esptool with `python -m pip install esptool==4.11.0`, connect by USB, and run the chosen command "
        "from this extracted directory. All commands verify written bytes.\n\n"
        "The firmware is intentionally owner-reflashable: no secure-boot lock, flash-encryption lock, download-mode "
        "lock, or hardware-fuse write is performed. Stock firmware can be restored later.\n\n"
        "Verify every shipped file against `SHA256SUMS.txt` before flashing. See `INSTALLING_PACKS.md` after firmware installation.\n"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--esptool", type=Path, required=True)
    parser.add_argument("--firmware-version", required=True)
    parser.add_argument("--physical-qa-authorization", type=Path, required=True)
    parser.add_argument("--physical-qa-evidence", type=Path, required=True)
    args = parser.parse_args()

    firmware_version = require_expected_firmware_version(args.firmware_version)
    project_root = args.project_root.expanduser().resolve()
    build_root = args.build_dir.expanduser().resolve()
    output_dir = args.output_dir.expanduser().resolve()
    if not project_root.is_dir():
        raise SystemExit(f"project root is missing: {project_root}")
    if not build_root.is_dir() or build_root.name != base.ENVIRONMENT:
        raise SystemExit(f"build directory must be the exact {base.ENVIRONMENT} output")
    if output_dir.exists() and (not output_dir.is_dir() or any(output_dir.iterdir())):
        raise SystemExit("output directory must be absent or empty")
    if output_dir.is_symlink():
        raise SystemExit("output directory must not be a symbolic link")

    esptool = base.require_file(args.esptool, "esptool validator")
    authorization_path = base.require_file(
        args.physical_qa_authorization, "physical-QA authorization"
    )
    evidence_path = base.require_file(args.physical_qa_evidence, "physical-QA evidence")
    profile = base.parse_platformio_profile(project_root)
    base.reject_sensitive_build_inputs(build_root)
    layout = base.require_file(project_root / base.PARTITION_LAYOUT, "partition layout")
    bootloader = base.require_build_file(build_root, "bootloader.bin", "bootloader")
    partitions = base.require_build_file(build_root, "partitions.bin", "partition table")
    application = base.require_build_file(build_root, "firmware.bin", "application")
    installer_source = base.require_file(
        Path(__file__).resolve().with_name("install_pack.py"), "pack installer"
    )
    if not 0 < bootloader.stat().st_size < base.PARTITIONS_OFFSET:
        raise SystemExit("bootloader is empty or overlaps the partition table")
    if not 0 < application.stat().st_size <= base.APP_SLOT_BYTES:
        raise SystemExit("application is empty or exceeds an OTA application slot")

    input_hashes = {
        "bootloader": base.sha256(bootloader),
        "partitions": base.sha256(partitions),
        "application": base.sha256(application),
        "layout": base.sha256(layout),
        "installer": base.sha256(installer_source),
    }
    authorization = validate_authorization(
        authorization_path, evidence_path, application, partitions
    )
    partition_validation = base.validate_partition_table(partitions, layout)
    boot_internal = base.parse_plain_esp_image(bootloader, "bootloader")
    app_internal = base.parse_plain_esp_image(application, "application")
    boot_external = base.validate_with_esptool(esptool, bootloader, "bootloader")
    app_external = base.validate_with_esptool(esptool, application, "application")
    if boot_internal["validation_sha256"] != boot_external["validation_sha256"]:
        raise SystemExit("bootloader validation differs between independent checks")
    if app_internal["validation_sha256"] != app_external["validation_sha256"]:
        raise SystemExit("application validation differs between independent checks")
    for key in ("flash_size", "flash_frequency", "flash_mode"):
        if boot_external[key] != app_external[key]:
            raise SystemExit("bootloader and application flash metadata differ")
    if any(
        base.sha256(path) != input_hashes[name]
        for name, path in {
            "bootloader": bootloader,
            "partitions": partitions,
            "application": application,
            "layout": layout,
            "installer": installer_source,
        }.items()
    ):
        raise SystemExit("a release input changed during validation")

    output_dir.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(
        tempfile.mkdtemp(prefix=f".{output_dir.name}.staging-", dir=output_dir.parent)
    )
    committed = False
    try:
        images = staging / "images"
        metadata = staging / "metadata"
        tools = staging / "tools"
        images.mkdir()
        metadata.mkdir()
        tools.mkdir()

        files = {
            "bootloader": images / "0x000000-bootloader.bin",
            "partitions": images / "0x008000-partitions.bin",
            "nvs_empty": images / "0x009000-nvs-empty.bin",
            "ota_empty": images / "0x00E000-ota-data-empty.bin",
            "application": images / "0x010000-kitsu868.bin",
            "pack_empty": images / "0x670000-pack-slot-empty.bin",
            "connectivity_empty": images / "0x7B0000-connectivity-empty.bin",
            "coredump_empty": images / "0x7F0000-coredump-empty.bin",
        }
        shutil.copyfile(bootloader, files["bootloader"])
        shutil.copyfile(partitions, files["partitions"])
        shutil.copyfile(application, files["application"])
        files["nvs_empty"].write_bytes(b"\xff" * NVS_BYTES)
        files["ota_empty"].write_bytes(b"\xff" * 0x2000)
        files["pack_empty"].write_bytes(b"\xff" * PACK_BYTES)
        files["connectivity_empty"].write_bytes(b"\xff" * CONNECTIVITY_BYTES)
        files["coredump_empty"].write_bytes(b"\xff" * COREDUMP_BYTES)
        shutil.copyfile(layout, metadata / layout.name)
        shutil.copyfile(installer_source, tools / "install_pack.py")

        if base.sha256(files["bootloader"]) != input_hashes["bootloader"]:
            raise SystemExit("staged bootloader changed")
        if base.sha256(files["partitions"]) != input_hashes["partitions"]:
            raise SystemExit("staged partition table changed")
        if base.sha256(files["application"]) != input_hashes["application"]:
            raise SystemExit("staged application changed")
        if base.sha256(tools / "install_pack.py") != input_hashes["installer"]:
            raise SystemExit("staged pack installer changed")

        image_paths = {key: f"images/{path.name}" for key, path in files.items()}
        upgrade_writes = [
            (base.PARTITIONS_OFFSET, image_paths["partitions"], "partition_table"),
            (base.APP_OFFSET, image_paths["application"], "app0"),
            (APP1_OFFSET, image_paths["application"], "app1"),
        ]
        recovery_writes = [
            (base.BOOTLOADER_OFFSET, image_paths["bootloader"], "bootloader"),
            *upgrade_writes[:1],
            (base.OTA_DATA_OFFSET, image_paths["ota_empty"], "otadata"),
            *upgrade_writes[1:],
        ]
        factory_writes = [
            (base.BOOTLOADER_OFFSET, image_paths["bootloader"], "bootloader"),
            (base.PARTITIONS_OFFSET, image_paths["partitions"], "partition_table"),
            (NVS_OFFSET, image_paths["nvs_empty"], "nvs"),
            (base.OTA_DATA_OFFSET, image_paths["ota_empty"], "otadata"),
            (base.APP_OFFSET, image_paths["application"], "app0"),
            (APP1_OFFSET, image_paths["application"], "app1"),
            (PACK_OFFSET, image_paths["pack_empty"], "pack_slot"),
            (CONNECTIVITY_OFFSET, image_paths["connectivity_empty"], "connectivity"),
            (COREDUMP_OFFSET, image_paths["coredump_empty"], "coredump"),
        ]
        installation_paths = {
            "upgrade_preserve_data": installation_path(
                guide="UPGRADE_PRESERVE_DATA.txt",
                writes=upgrade_writes,
                resets=[],
                preserves=["nvs", "otadata", "pack_slot", "connectivity", "coredump"],
            ),
            "recovery_preserve_data": installation_path(
                guide="RECOVERY_PRESERVE_DATA.txt",
                writes=recovery_writes,
                resets=["otadata"],
                preserves=["nvs", "pack_slot", "connectivity", "coredump"],
            ),
            "factory_reset": installation_path(
                guide="FACTORY_RESET.txt",
                writes=factory_writes,
                resets=["nvs", "otadata", "pack_slot", "connectivity", "coredump"],
                preserves=["usb_uart_download_mode", "efuse_state"],
            ),
        }

        (staging / "UPGRADE_PRESERVE_DATA.txt").write_text(
            "Kitsu firmware upgrade - preserve owner data\n"
            "=============================================\n\n"
            "Supported hardware only: Heltec WiFi LoRa 32 V3 or V3.2 with 8 MiB flash.\n"
            "This writes the reviewed partition table and the same application to both OTA slots.\n"
            "It preserves NVS, OTA selection metadata, the companion pack, connectivity state, and coredump.\n\n"
            + command_text(installation_paths["upgrade_preserve_data"]["command"])
            + "\n\nNo erase command and no hardware-fuse operation is used.\n",
            encoding="utf-8",
        )
        (staging / "RECOVERY_PRESERVE_DATA.txt").write_text(
            "Kitsu serial recovery - preserve owner data\n"
            "===========================================\n\n"
            "Supported hardware only: Heltec WiFi LoRa 32 V3 or V3.2 with 8 MiB flash.\n"
            "This restores bootloader, partition table, OTA selection metadata, and both app slots.\n"
            "It preserves NVS, the companion pack, connectivity state, and coredump.\n\n"
            + command_text(installation_paths["recovery_preserve_data"]["command"])
            + "\n\nNo whole-flash erase and no hardware-fuse operation is used.\n",
            encoding="utf-8",
        )
        (staging / "FACTORY_RESET.txt").write_text(
            "Kitsu factory reset - intentionally clears owner data\n"
            "=====================================================\n\n"
            "Supported hardware only: Heltec WiFi LoRa 32 V3 or V3.2 with 8 MiB flash.\n"
            "This writes every Kitsu partition with reviewed firmware or an erased-state image.\n"
            "It clears NVS, OTA state, the companion slot, connectivity state, and coredump.\n"
            "Afterward, install exactly one Cat, Fox, or Dog pack using INSTALLING_PACKS.md.\n\n"
            + command_text(installation_paths["factory_reset"]["command"])
            + "\n\nNo whole-flash erase and no hardware-fuse operation is used. USB/UART reflashing remains available.\n",
            encoding="utf-8",
        )
        (staging / "INSTALLING_PACKS.md").write_text(
            installing_packs_guide(firmware_version), encoding="utf-8"
        )
        (staging / "FLASHING.md").write_text(
            flashing_guide(firmware_version), encoding="utf-8"
        )

        artifacts = [
            artifact_record(
                files["bootloader"], role="bootloader", install_targets=[("bootloader", 0)],
                esp_validation={**boot_internal, **boot_external},
            ),
            artifact_record(
                files["partitions"], role="partition_table",
                install_targets=[("partition_table", base.PARTITIONS_OFFSET)],
            ),
            artifact_record(
                files["nvs_empty"], role="erased_state",
                install_targets=[("nvs", NVS_OFFSET)],
            ),
            artifact_record(
                files["ota_empty"], role="erased_state",
                install_targets=[("otadata", base.OTA_DATA_OFFSET)],
            ),
            artifact_record(
                files["application"], role="application",
                install_targets=[("app0", base.APP_OFFSET), ("app1", APP1_OFFSET)],
                esp_validation={**app_internal, **app_external},
            ),
            artifact_record(
                files["pack_empty"], role="erased_state",
                install_targets=[("pack_slot", PACK_OFFSET)],
            ),
            artifact_record(
                files["connectivity_empty"], role="erased_state",
                install_targets=[("connectivity", CONNECTIVITY_OFFSET)],
            ),
            artifact_record(
                files["coredump_empty"], role="erased_state",
                install_targets=[("coredump", COREDUMP_OFFSET)],
            ),
        ]
        manifest = {
            "schema": SCHEMA,
            "created_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
            "artifact_status": ARTIFACT_STATUS,
            "firmware_version": firmware_version,
            "release_channel": RELEASE_CHANNEL,
            "supported_hardware": [
                {
                    "vendor": "Heltec",
                    "models": ["WiFi LoRa 32 V3", "WiFi LoRa 32 V3.2"],
                    "chip": "ESP32-S3",
                    "flash_bytes": base.FLASH_BYTES,
                    "flash_mib": 8,
                }
            ],
            "checksum_index": "SHA256SUMS.txt",
            "physical_qa_authorization": authorization,
            "build_profile": profile,
            "partition_layout": partition_validation,
            "security_profile": {
                "mode": "owner-reflashable",
                "secure_boot": False,
                "flash_encryption": False,
                "nvs_encryption": False,
                "hardware_root_protected": False,
                "firmware_images_encrypted": False,
                "application_layer_encryption": True,
                "efuse_writes": False,
                "efuse_locks": False,
                "uart_download_disabled": False,
                "usb_download_disabled": False,
                "serial_reflash_available": True,
                "stock_firmware_restore_available": True,
            },
            "network_security": {
                "tls": True,
                "mutual_tls": True,
                "authenticated_ble_enrollment": True,
                "oidc_owner_authentication": True,
            },
            "release_requirements": {
                "device_specific_secrets": False,
                "private_keys": False,
                "owner_credentials": False,
                "efuse_operations": False,
                "whole_flash_erase": False,
            },
            "flash_artifacts": artifacts,
            "installation_paths": installation_paths,
            "pack_installation": {
                "guide": "INSTALLING_PACKS.md",
                "tool": "tools/install_pack.py",
                "slot_offset": PACK_OFFSET,
                "slot_offset_hex": f"0x{PACK_OFFSET:06X}",
                "slot_bytes": PACK_BYTES,
                "allowed_display_names": ["CAT", "FOX", "DOG"],
                "whole_flash_erase": False,
                "efuse_operations": False,
                "preserves": [
                    "bootloader", "partition_table", "nvs", "otadata",
                    "app0", "app1", "connectivity", "coredump",
                ],
            },
            "warnings": [
                {
                    "code": "SUPPORTED_HARDWARE_ONLY",
                    "severity": "high",
                    "message": "Use only the listed Heltec V3/V3.2 8 MiB hardware.",
                },
                {
                    "code": "OWNER_REFLASHABLE_BY_DESIGN",
                    "severity": "info",
                    "message": "Physical access can replace firmware; USB/UART recovery remains available.",
                },
                {
                    "code": "FACTORY_PATH_CLEARS_OWNER_DATA",
                    "severity": "high",
                    "message": "The factory-reset path intentionally clears all Kitsu data partitions.",
                },
            ],
        }
        (staging / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        base.write_checksum_index(staging)

        # The authorization and evidence files are intentionally never copied.
        if any(path.name in {authorization_path.name, evidence_path.name} for path in staging.rglob("*")):
            raise SystemExit("private QA inputs crossed the public staging boundary")
        if output_dir.exists():
            output_dir.rmdir()
        os.replace(staging, output_dir)
        committed = True
    finally:
        if not committed:
            shutil.rmtree(staging, ignore_errors=True)

    print(f"KITSU_STABLE_RELEASE_CREATED {output_dir / 'manifest.json'}")


if __name__ == "__main__":
    main()
