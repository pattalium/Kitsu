#!/usr/bin/env python3
"""Prepare or validate the minimal Kitsu 0.17.4 firmware publication stage.

Preparation consumes the final owner-reflashable package and the small COM3
publication authorization created only after physical acceptance. It binds the
flash-site release module to those exact bytes, runs the existing flash-site
check/build, and creates an unsigned stage containing only the static flasher,
five unique firmware binaries, update authority, and canonical manifest.

The same file is copied into the stage and used by the signing, upload, and
server-side atomic deployment paths. It never signs, uploads, or deploys.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


FIRMWARE_VERSION = "0.17.4"
RELEASE_ID = "kitsu-0.17.4-reflashable-1"
PACKAGE_SCHEMA = "kitsu.firmware-reflashable-release.v2"
UPDATE_SCHEMA = "kitsu.firmware-update.v2"
AUTHORIZATION_SCHEMA = "kitsu.firmware-publication-authorization.v2"
PLAN_SCHEMA = "kitsu.firmware-publication-plan.v1"
DEVICE_CLASS = "heltec-wifi-lora-32-v3-esp32s3-8mb"
PUBLIC_KEY_SHA256 = "711ad6b564e129cbd31b8edca52f4977c03daf0410490f62c6fba4484f65366c"
PUBLIC_KEY_SPKI_SHA256 = "df530766fbc4fc93e82cdbd354ebe4a17a453c83e9bb7fe2af30ca2d202494ab"

APP_SLOT_BYTES = 0x330000
OTA_JOURNAL_BYTES = 0x001000

FIXED_ARTIFACTS = {
    "bootloader": {
        "source": "images/0x000000-bootloader.bin",
        "public": "kitsu868-bootloader.bin",
        "offset": 0x000000,
        "bytes": 15104,
        "sha256": "1776e4dd896a69d0a5c2e79957b0e2a88aa4129b1381d6478683515a1f6af343",
        "acceptance": "bootloader_sha256",
    },
    "partition_table": {
        "source": "images/0x008000-partitions.bin",
        "public": "kitsu868-partitions.bin",
        "offset": 0x008000,
        "bytes": 3072,
        "sha256": "f9b22e16fcfb701520dd6c7e0791582ececbbd44c317c8d519e3d6b2b9ce8b7a",
        "acceptance": "partition_table_sha256",
    },
    "ota_journal_app0_clear": {
        "source": "images/kitsu868-ota-journal-clear.bin",
        "public": "kitsu868-ota-journal-clear.bin",
        "offset": 0x33F000,
        "bytes": OTA_JOURNAL_BYTES,
        "sha256": "f47a8ec3e9aff2318d896942282ad4fe37d6391c82914f54a5da8a37de1300c6",
        "acceptance": "ota_journal_clear_sha256",
    },
    "ota_journal_app1_clear": {
        "source": "images/kitsu868-ota-journal-clear.bin",
        "public": "kitsu868-ota-journal-clear.bin",
        "offset": 0x66F000,
        "bytes": OTA_JOURNAL_BYTES,
        "sha256": "f47a8ec3e9aff2318d896942282ad4fe37d6391c82914f54a5da8a37de1300c6",
        "acceptance": "ota_journal_clear_sha256",
    },
    "legacy_connectivity_clear": {
        "source": "images/kitsu868-legacy-connectivity-clear.bin",
        "public": "kitsu868-legacy-connectivity-clear.bin",
        "offset": 0x7B0000,
        "bytes": 0x040000,
        "sha256": "3b874d3ba46c638fc3094f8e92fb744ca974893873f8885f54e23760f9b6311b",
        "acceptance": "legacy_connectivity_clear_sha256",
    },
}

ROLE_ORDER = (
    "bootloader",
    "partition_table",
    "application_app0",
    "ota_journal_app0_clear",
    "application_app1",
    "ota_journal_app1_clear",
    "legacy_connectivity_clear",
)
ROLE_OFFSETS = {
    "application_app0": 0x010000,
    "application_app1": 0x340000,
}

SAFE_FLASH_SUFFIXES = {".js", ".css", ".pet"}
SAFE_TOKEN = re.compile(r"[0-9A-Za-z][0-9A-Za-z._-]{0,127}\Z")
SAFE_RELATIVE_PATH = re.compile(
    r"(?:[0-9A-Za-z._-]+/)*[0-9A-Za-z._-]+\Z"
)
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}\Z")
UTC_PATTERN = re.compile(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z\Z")
PUBLIC_STARTER_PACK_SHA256 = {
    "49b0758ab2fdba77bff543ac3235110190896d5ce7b3456770bb44f59c09f985",
    "47876efaa0f7fe4831906c94e9a3b2d5a74a267f1a6f981593525bff5476c051",
    "e67892d8515b3c6830c598fce74aa6a64074075679912d58df05df003623c38d",
}

AUTHORIZATION_KEYS = {
    "schema",
    "status",
    "evidence_sha256",
    "accepted_at",
    "bootloader_sha256",
    "application_sha256",
    "partition_table_sha256",
    "ota_journal_clear_sha256",
    "legacy_connectivity_clear_sha256",
}


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def canonical_bytes(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode(
        "utf-8"
    )


def require_object(value: object, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be an object")
    return value


def require_exact_keys(value: dict[str, Any], keys: set[str], label: str) -> None:
    actual = set(value)
    if actual != keys:
        raise ValueError(
            f"{label} fields differ: missing={sorted(keys - actual)} "
            f"extra={sorted(actual - keys)}"
        )


def require_sha256(value: object, label: str) -> str:
    if not isinstance(value, str) or not SHA256_PATTERN.fullmatch(value):
        raise ValueError(f"{label} must be lowercase SHA-256")
    return value


def require_utc(value: object, label: str) -> str:
    if not isinstance(value, str) or not UTC_PATTERN.fullmatch(value):
        raise ValueError(f"{label} must be whole-second UTC")
    try:
        datetime.fromisoformat(value.removesuffix("Z") + "+00:00")
    except ValueError as error:
        raise ValueError(f"{label} is not a valid UTC timestamp") from error
    return value


def read_json(path: Path, label: str) -> dict[str, Any]:
    try:
        return require_object(json.loads(path.read_text(encoding="utf-8")), label)
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read {label}: {error}") from error


def require_regular_file(path: Path, label: str) -> Path:
    resolved = path.resolve()
    if not resolved.is_file() or resolved.is_symlink():
        raise ValueError(f"{label} must be a regular non-symlink file: {resolved}")
    return resolved


def relative_files(root: Path) -> list[str]:
    output: list[str] = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"symlink is forbidden: {path}")
        mode = path.stat().st_mode
        if path.is_dir():
            continue
        if not stat.S_ISREG(mode):
            raise ValueError(f"special file is forbidden: {path}")
        output.append(path.relative_to(root).as_posix())
    return output


def file_record(root: Path, relative: str) -> dict[str, object]:
    path = require_regular_file(root / relative, relative)
    return {
        "path": relative,
        "bytes": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def validate_authorization_object(
    authorization: dict[str, Any], label: str
) -> dict[str, Any]:
    require_exact_keys(
        authorization,
        AUTHORIZATION_KEYS,
        label,
    )
    if authorization["schema"] != AUTHORIZATION_SCHEMA:
        raise ValueError(f"unsupported {label} schema")
    if authorization["status"] != "passed":
        raise ValueError(f"{label} has not passed")
    require_utc(authorization["accepted_at"], f"{label} accepted_at")
    for key in (
        "evidence_sha256",
        "bootloader_sha256",
        "application_sha256",
        "partition_table_sha256",
        "ota_journal_clear_sha256",
        "legacy_connectivity_clear_sha256",
    ):
        require_sha256(authorization[key], f"{label} {key}")
    return authorization


def validate_authorization(path: Path) -> dict[str, Any]:
    path = require_regular_file(path, "COM3 publication authorization")
    return validate_authorization_object(
        read_json(path, "COM3 publication authorization"),
        "COM3 publication authorization",
    )


def package_plan(
    package_dir: Path, authorization: dict[str, Any]
) -> tuple[list[dict[str, Any]], dict[str, Path], int, str]:
    package_manifest_path = require_regular_file(
        package_dir / "manifest.json", "package manifest"
    )
    package_manifest = read_json(package_manifest_path, "package manifest")
    if package_manifest.get("schema") != PACKAGE_SCHEMA:
        raise ValueError("package manifest schema is not owner-reflashable v2")
    if package_manifest.get("firmware_version") != FIRMWARE_VERSION:
        raise ValueError(f"package firmware version must be {FIRMWARE_VERSION}")
    if package_manifest.get("release_channel") != "candidate":
        raise ValueError("input package must still be a candidate")
    if package_manifest.get("artifact_status") != "release-candidate-owner-reflashable":
        raise ValueError("input package artifact status is not releasable")
    if package_manifest.get("device_class") != DEVICE_CLASS:
        raise ValueError("input package targets the wrong device class")
    firmware_identity = require_object(
        package_manifest.get("firmware_identity"), "firmware_identity"
    )
    if (
        firmware_identity.get("version") != FIRMWARE_VERSION
        or firmware_identity.get("image_marker_verified") is not True
    ):
        raise ValueError(f"input package does not contain a verified {FIRMWARE_VERSION} image marker")
    security = require_object(package_manifest.get("security_profile"), "security_profile")
    for key in ("secure_boot", "flash_encryption", "efuse_writes"):
        if security.get(key) is not False:
            raise ValueError(f"input package unexpectedly enables {key}")
    operations = require_object(package_manifest.get("operations"), "operations")
    if operations.get("erase_flash") is not False or operations.get("write_count") != 7:
        raise ValueError("input package is not the bounded seven-write plan")

    raw_artifacts = package_manifest.get("flash_artifacts")
    if not isinstance(raw_artifacts, list) or len(raw_artifacts) != 7:
        raise ValueError("package manifest must contain seven flash artifacts")
    artifacts = [require_object(value, f"flash_artifacts[{index}]") for index, value in enumerate(raw_artifacts)]
    if tuple(value.get("role") for value in artifacts) != ROLE_ORDER:
        raise ValueError("package flash roles or order changed")

    sources: dict[str, Path] = {}
    application_bytes = 0
    application_sha256 = ""
    for artifact in artifacts:
        role = str(artifact["role"])
        if artifact.get("encrypted") is not False or artifact.get("secure_boot_signed") is not False:
            raise ValueError(f"{role} is encrypted or secure-boot signed")
        if role in ("application_app0", "application_app1"):
            expected_source = "images/kitsu868-app.bin"
            expected_offset = ROLE_OFFSETS[role]
            if artifact.get("file") != expected_source or artifact.get("offset") != expected_offset:
                raise ValueError(f"{role} source or offset changed")
            if not isinstance(artifact.get("bytes"), int) or not 1 <= artifact["bytes"] <= APP_SLOT_BYTES - OTA_JOURNAL_BYTES:
                raise ValueError(f"{role} byte count does not fit its slot")
            digest = require_sha256(artifact.get("sha256"), f"{role} digest")
            if not application_sha256:
                application_bytes = artifact["bytes"]
                application_sha256 = digest
            elif artifact["bytes"] != application_bytes or digest != application_sha256:
                raise ValueError("app0 and app1 must reuse identical application bytes")
            source = require_regular_file(package_dir / expected_source, expected_source)
            sources["kitsu868-app.bin"] = source
        else:
            definition = FIXED_ARTIFACTS[role]
            if (
                artifact.get("file") != definition["source"]
                or artifact.get("offset") != definition["offset"]
                or artifact.get("bytes") != definition["bytes"]
                or artifact.get("sha256") != definition["sha256"]
            ):
                raise ValueError(f"{role} differs from the reviewed fixed artifact")
            source = require_regular_file(package_dir / str(definition["source"]), role)
            sources[str(definition["public"])] = source

    for public_name, source in sources.items():
        expected_bytes = application_bytes if public_name == "kitsu868-app.bin" else source.stat().st_size
        expected_hash = application_sha256 if public_name == "kitsu868-app.bin" else sha256_file(source)
        if source.stat().st_size != expected_bytes or sha256_file(source) != expected_hash:
            raise ValueError(f"packaged bytes changed after manifest creation: {public_name}")
    for definition in FIXED_ARTIFACTS.values():
        source = sources[str(definition["public"])]
        if source.stat().st_size != definition["bytes"] or sha256_file(source) != definition["sha256"]:
            raise ValueError(f"fixed artifact identity changed: {definition['public']}")
    if sources["kitsu868-ota-journal-clear.bin"].read_bytes() != b"\xff" * OTA_JOURNAL_BYTES:
        raise ValueError("OTA journal clear file is not all 0xff")
    if sources["kitsu868-legacy-connectivity-clear.bin"].read_bytes() != b"\xff" * 0x040000:
        raise ValueError("legacy connectivity clear file is not all 0xff")

    for definition in FIXED_ARTIFACTS.values():
        acceptance_key = str(definition["acceptance"])
        if authorization[acceptance_key] != definition["sha256"]:
            raise ValueError(f"COM3 authorization does not bind {acceptance_key}")
    if authorization["application_sha256"] != application_sha256:
        raise ValueError("COM3 authorization does not bind the packaged application")

    writes: list[dict[str, Any]] = []
    for artifact in artifacts:
        role = str(artifact["role"])
        public_name = (
            "kitsu868-app.bin"
            if role.startswith("application_")
            else str(FIXED_ARTIFACTS[role]["public"])
        )
        writes.append(
            {
                "role": role,
                "path": f"firmware/{RELEASE_ID}/{public_name}",
                "offset": artifact["offset"],
                "bytes": artifact["bytes"],
                "sha256": artifact["sha256"],
                "encrypted": False,
                "secure_boot_signed": False,
            }
        )
    return writes, sources, application_bytes, application_sha256


def replace_once(source: str, pattern: str, replacement: str, label: str) -> str:
    output, count = re.subn(pattern, replacement, source, count=1)
    if count != 1:
        raise ValueError(f"cannot update flash release field: {label}")
    return output


def bind_flash_release_module(
    path: Path,
    application_bytes: int,
    application_sha256: str,
) -> str:
    original = path.read_text(encoding="utf-8")
    values: dict[str, object] = {
        "bootloaderBytes": FIXED_ARTIFACTS["bootloader"]["bytes"],
        "bootloaderSha256": FIXED_ARTIFACTS["bootloader"]["sha256"],
        "partitionBytes": FIXED_ARTIFACTS["partition_table"]["bytes"],
        "partitionSha256": FIXED_ARTIFACTS["partition_table"]["sha256"],
        "otaJournalSha256": FIXED_ARTIFACTS["ota_journal_app0_clear"]["sha256"],
        "legacyConnectivitySha256": FIXED_ARTIFACTS["legacy_connectivity_clear"]["sha256"],
        "applicationBytes": application_bytes,
        "applicationSha256": application_sha256,
    }
    updated = original
    for field, value in values.items():
        if isinstance(value, int):
            updated = replace_once(
                updated,
                rf"({field}:\s*)\d+",
                rf"\g<1>{value}",
                field,
            )
        else:
            updated = replace_once(
                updated,
                rf'({field}:\s*)"[0-9a-f]{{64}}"',
                rf'\g<1>"{value}"',
                field,
            )
    if PUBLIC_KEY_SPKI_SHA256 not in updated:
        raise ValueError("flash release module uses an unexpected update authority")
    return updated


def atomic_text_write(path: Path, content: str) -> None:
    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        newline="\n",
        dir=path.parent,
        prefix=f".{path.name}.",
        suffix=".tmp",
        delete=False,
    ) as stream:
        stream.write(content)
        candidate = Path(stream.name)
    try:
        os.replace(candidate, path)
    except BaseException:
        candidate.unlink(missing_ok=True)
        raise


def validate_flash_tree(flash_root: Path, required_hashes: set[str]) -> list[dict[str, object]]:
    files = relative_files(flash_root)
    if "index.html" not in files:
        raise ValueError("flash build has no index.html")
    for relative in files:
        path = Path(relative)
        if relative == "index.html":
            continue
        if len(path.parts) != 2 or path.parts[0] != "assets" or path.suffix.lower() not in SAFE_FLASH_SUFFIXES:
            raise ValueError(f"flash build contains a non-public asset: {relative}")
    pet_files = [relative for relative in files if relative.endswith(".pet")]
    if len(pet_files) != 3:
        raise ValueError("flash build must contain exactly Cat, Fox, and Dog pet bundles")
    if {sha256_file(flash_root / relative) for relative in pet_files} != PUBLIC_STARTER_PACK_SHA256:
        raise ValueError("flash build contains a non-starter companion bundle")
    if any(relative.lower().endswith((".map", ".bin", ".k868", ".aab", ".idsig", ".jks", ".keystore", ".pem", ".key")) for relative in files):
        raise ValueError("flash build contains a forbidden release asset")
    bundled = b"\n".join(
        (flash_root / relative).read_bytes()
        for relative in files
        if relative.endswith(".js")
    )
    for digest in required_hashes | {PUBLIC_KEY_SPKI_SHA256}:
        if digest.encode("ascii") not in bundled:
            raise ValueError(f"flash bundle is not bound to digest {digest}")
    return [file_record(flash_root, relative) for relative in files]


def prepare(args: argparse.Namespace) -> int:
    project_root = args.project_root.resolve()
    package_dir = args.package_dir.resolve()
    stage = args.stage_dir.resolve()
    public_key = require_regular_file(args.public_key, "update public key")
    authorization = validate_authorization(args.acceptance.resolve())
    if not package_dir.is_dir() or package_dir.is_symlink():
        raise ValueError("package directory is missing or unsafe")
    if stage.exists() or stage.is_symlink():
        raise ValueError(f"publication stage already exists: {stage}")
    if sha256_file(public_key) != PUBLIC_KEY_SHA256:
        raise ValueError("update authority public-key file changed")

    writes, sources, application_bytes, application_sha256 = package_plan(
        package_dir, authorization
    )
    release_module = project_root / "platform/flash-site/src/release.js"
    original_release_source = release_module.read_text(encoding="utf-8")
    updated_release_source = bind_flash_release_module(
        release_module, application_bytes, application_sha256
    )
    atomic_text_write(release_module, updated_release_source)
    flash_root = project_root / "platform/flash-site"
    npm = shutil.which("npm.cmd") or shutil.which("npm")
    if npm is None:
        atomic_text_write(release_module, original_release_source)
        raise ValueError("npm is unavailable")
    try:
        subprocess.run([npm, "run", "check"], cwd=flash_root, check=True)
    except BaseException:
        atomic_text_write(release_module, original_release_source)
        raise

    published_at = datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    accepted_at = require_utc(authorization["accepted_at"], "authorization accepted_at")
    if published_at < accepted_at:
        raise ValueError("publication timestamp predates COM3 acceptance")
    stamp = published_at.replace("-", "").replace(":", "")
    flash_release_id = f"{stamp}-{FIRMWARE_VERSION}"

    manifest = {
        "schema": UPDATE_SCHEMA,
        "release_id": RELEASE_ID,
        "firmware_version": FIRMWARE_VERSION,
        "release_channel": "stable",
        "artifact_status": "available",
        "published_at": published_at,
        "device_class": DEVICE_CLASS,
        "chip": "esp32s3",
        "physical_acceptance": authorization,
        "writes": writes,
        "operations": {
            "erase_flash": False,
            "retire_legacy_connectivity": True,
            "retire_legacy_lan_action_state": True,
        },
        "preserves": {
            "ota_data": True,
            "companion_state": True,
            "companion_pack": True,
            "controller_store": True,
            "meshcore_state": True,
            "coredump": True,
        },
        "capabilities": {
            "full_chip_erase_available": True,
            "rollback_bootloader": True,
            "stock_meshcore_restore_available": True,
        },
        "security": {
            "mode": "reflashable",
            "efuse_writes": False,
            "secure_boot": False,
            "flash_encryption": False,
        },
        "flash": {
            "flash_mode": "dio",
            "flash_frequency": "80m",
            "flash_size": "8MB",
            "readback_verify": True,
        },
    }
    manifest_bytes = canonical_bytes(manifest)

    try:
        (stage / "update" / "firmware" / RELEASE_ID).mkdir(parents=True)
        shutil.copytree(flash_root / "dist", stage / "flash")
        shutil.copy2(public_key, stage / "update" / "update-ed25519-public.pem")
        update_firmware = stage / "update" / "firmware" / RELEASE_ID
        for public_name, source in sources.items():
            shutil.copy2(source, update_firmware / public_name)
        (stage / "update" / "latest.json").write_bytes(manifest_bytes)
        (stage / "tools").mkdir()
        validator_target = stage / "tools" / "validate-stage.py"
        shutil.copy2(Path(__file__).resolve(), validator_target)

        required_hashes = {
            str(write["sha256"])
            for write in writes
        }
        flash_files = validate_flash_tree(stage / "flash", required_hashes)
        update_payload_paths = [
            "update-ed25519-public.pem",
            *[f"firmware/{RELEASE_ID}/{name}" for name in sorted(sources)],
        ]
        update_payload_files = [
            file_record(stage / "update", relative) for relative in update_payload_paths
        ]
        plan = {
            "schema": PLAN_SCHEMA,
            "firmware_version": FIRMWARE_VERSION,
            "release_id": RELEASE_ID,
            "flash_release_id": flash_release_id,
            "manifest": {
                "bytes": len(manifest_bytes),
                "sha256": sha256_bytes(manifest_bytes),
            },
            "public_key_sha256": PUBLIC_KEY_SHA256,
            "validator_sha256": sha256_file(validator_target),
            "update_payload_files": update_payload_files,
            "flash_files": flash_files,
        }
        (stage / "publication-plan.json").write_bytes(canonical_bytes(plan))
        validate_stage(stage, require_signature=False, openssl=args.openssl)
    except BaseException:
        if stage.exists() and not stage.is_symlink():
            shutil.rmtree(stage)
        atomic_text_write(release_module, original_release_source)
        raise

    print(
        "KITSU_FIRMWARE_0174_STAGE_READY "
        f"stage={stage} release={RELEASE_ID} app_bytes={application_bytes} "
        f"app_sha256={application_sha256} manifest_bytes={len(manifest_bytes)} "
        f"manifest_sha256={sha256_bytes(manifest_bytes)}"
    )
    return 0


def validate_update_manifest(
    manifest: dict[str, Any],
) -> tuple[dict[str, tuple[int, str]], set[str]]:
    require_exact_keys(
        manifest,
        {
            "schema",
            "release_id",
            "firmware_version",
            "release_channel",
            "artifact_status",
            "published_at",
            "device_class",
            "chip",
            "physical_acceptance",
            "writes",
            "operations",
            "preserves",
            "capabilities",
            "security",
            "flash",
        },
        "latest.json",
    )
    if (
        manifest["schema"] != UPDATE_SCHEMA
        or manifest["release_id"] != RELEASE_ID
        or manifest["firmware_version"] != FIRMWARE_VERSION
        or manifest["release_channel"] != "stable"
        or manifest["artifact_status"] != "available"
        or manifest["device_class"] != DEVICE_CLASS
        or manifest["chip"] != "esp32s3"
    ):
        raise ValueError("latest.json release identity or channel changed")
    published_at = require_utc(manifest["published_at"], "latest.json published_at")
    authorization = validate_authorization_object(
        require_object(manifest["physical_acceptance"], "latest physical_acceptance"),
        "latest physical_acceptance",
    )
    if published_at < authorization["accepted_at"]:
        raise ValueError("latest.json predates physical acceptance")

    raw_writes = manifest["writes"]
    if not isinstance(raw_writes, list) or len(raw_writes) != 7:
        raise ValueError("latest.json must contain exactly seven writes")
    writes = [
        require_object(value, f"latest writes[{index}]")
        for index, value in enumerate(raw_writes)
    ]
    if tuple(write.get("role") for write in writes) != ROLE_ORDER:
        raise ValueError("latest.json write roles or order changed")

    payloads: dict[str, tuple[int, str]] = {}
    required_hashes: set[str] = set()
    application_size = 0
    application_digest = ""
    for index, write in enumerate(writes):
        label = f"latest writes[{index}]"
        require_exact_keys(
            write,
            {
                "role",
                "path",
                "offset",
                "bytes",
                "sha256",
                "encrypted",
                "secure_boot_signed",
            },
            label,
        )
        if write["encrypted"] is not False or write["secure_boot_signed"] is not False:
            raise ValueError(f"{label} enables a protected-flash mode")
        if type(write["offset"]) is not int or type(write["bytes"]) is not int:
            raise ValueError(f"{label} offset and byte count must be integers")
        role = str(write["role"])
        digest = require_sha256(write["sha256"], f"{label} digest")
        if role in ("application_app0", "application_app1"):
            expected_name = "kitsu868-app.bin"
            expected_offset = ROLE_OFFSETS[role]
            size = write["bytes"]
            if (
                type(size) is not int
                or not 1 <= size <= APP_SLOT_BYTES - OTA_JOURNAL_BYTES
                or write["offset"] != expected_offset
            ):
                raise ValueError(f"{label} does not fit its fixed application slot")
            if not application_digest:
                application_size = size
                application_digest = digest
            elif size != application_size or digest != application_digest:
                raise ValueError("latest app0 and app1 do not use identical bytes")
        else:
            definition = FIXED_ARTIFACTS[role]
            expected_name = str(definition["public"])
            size = int(definition["bytes"])
            if (
                write["offset"] != definition["offset"]
                or write["bytes"] != size
                or digest != definition["sha256"]
            ):
                raise ValueError(f"{label} differs from the reviewed fixed artifact")
        expected_path = f"firmware/{RELEASE_ID}/{expected_name}"
        if write["path"] != expected_path:
            raise ValueError(f"{label} leaves the fixed firmware release path")
        record = (size, digest)
        if expected_path in payloads and payloads[expected_path] != record:
            raise ValueError(f"conflicting repeated firmware payload: {expected_path}")
        payloads[expected_path] = record
        required_hashes.add(digest)

    if authorization["application_sha256"] != application_digest:
        raise ValueError("latest physical acceptance does not bind the application")
    for definition in FIXED_ARTIFACTS.values():
        if authorization[str(definition["acceptance"])] != definition["sha256"]:
            raise ValueError("latest physical acceptance does not bind fixed bytes")

    operations = require_object(manifest["operations"], "latest operations")
    require_exact_keys(
        operations,
        {
            "erase_flash",
            "retire_legacy_connectivity",
            "retire_legacy_lan_action_state",
        },
        "latest operations",
    )
    if (
        operations["erase_flash"] is not False
        or operations["retire_legacy_connectivity"] is not True
        or operations["retire_legacy_lan_action_state"] is not True
    ):
        raise ValueError("latest operations changed")

    preserves = require_object(manifest["preserves"], "latest preserves")
    preserve_keys = {
        "ota_data",
        "companion_state",
        "companion_pack",
        "controller_store",
        "meshcore_state",
        "coredump",
    }
    require_exact_keys(preserves, preserve_keys, "latest preserves")
    if any(preserves[key] is not True for key in preserve_keys):
        raise ValueError("latest preservation contract changed")

    capabilities = require_object(manifest["capabilities"], "latest capabilities")
    capability_keys = {
        "full_chip_erase_available",
        "rollback_bootloader",
        "stock_meshcore_restore_available",
    }
    require_exact_keys(capabilities, capability_keys, "latest capabilities")
    if any(capabilities[key] is not True for key in capability_keys):
        raise ValueError("latest recovery capabilities changed")

    security = require_object(manifest["security"], "latest security")
    require_exact_keys(
        security,
        {"mode", "efuse_writes", "secure_boot", "flash_encryption"},
        "latest security",
    )
    if (
        security["mode"] != "reflashable"
        or security["efuse_writes"] is not False
        or security["secure_boot"] is not False
        or security["flash_encryption"] is not False
    ):
        raise ValueError("latest security mode is not owner-reflashable")

    flash = require_object(manifest["flash"], "latest flash")
    require_exact_keys(
        flash,
        {"flash_mode", "flash_frequency", "flash_size", "readback_verify"},
        "latest flash",
    )
    if (
        flash["flash_mode"] != "dio"
        or flash["flash_frequency"] != "80m"
        or flash["flash_size"] != "8MB"
        or flash["readback_verify"] is not True
    ):
        raise ValueError("latest flash parameters changed")
    return payloads, required_hashes


def record_map(value: object, label: str) -> dict[str, tuple[int, str]]:
    if not isinstance(value, list):
        raise ValueError(f"{label} must be an array")
    output: dict[str, tuple[int, str]] = {}
    for index, raw in enumerate(value):
        record = require_object(raw, f"{label}[{index}]")
        require_exact_keys(record, {"path", "bytes", "sha256"}, f"{label}[{index}]")
        path = record["path"]
        size = record["bytes"]
        digest = record["sha256"]
        if (
            not isinstance(path, str)
            or not SAFE_RELATIVE_PATH.fullmatch(path)
            or path.startswith("/")
            or ".." in Path(path).parts
            or "\\" in path
        ):
            raise ValueError(f"{label}[{index}] path is unsafe")
        if not isinstance(size, int) or size < 1:
            raise ValueError(f"{label}[{index}] byte count is invalid")
        require_sha256(digest, f"{label}[{index}] digest")
        if path in output:
            raise ValueError(f"duplicate {label} path: {path}")
        output[path] = (size, digest)
    return output


def verify_records(root: Path, expected: dict[str, tuple[int, str]], label: str) -> None:
    actual = set(relative_files(root))
    if actual != set(expected):
        raise ValueError(
            f"{label} inventory differs: missing={sorted(set(expected) - actual)} "
            f"extra={sorted(actual - set(expected))}"
        )
    for relative, (size, digest) in expected.items():
        path = require_regular_file(root / relative, f"{label} {relative}")
        if path.stat().st_size != size or sha256_file(path) != digest:
            raise ValueError(f"{label} file identity changed: {relative}")


def validate_stage(stage: Path, require_signature: bool, openssl: str) -> dict[str, Any]:
    stage = stage.resolve()
    if not stage.is_dir() or stage.is_symlink():
        raise ValueError("publication stage is missing or unsafe")
    plan = read_json(stage / "publication-plan.json", "publication plan")
    require_exact_keys(
        plan,
        {
            "schema",
            "firmware_version",
            "release_id",
            "flash_release_id",
            "manifest",
            "public_key_sha256",
            "validator_sha256",
            "update_payload_files",
            "flash_files",
        },
        "publication plan",
    )
    if plan["schema"] != PLAN_SCHEMA or plan["firmware_version"] != FIRMWARE_VERSION or plan["release_id"] != RELEASE_ID:
        raise ValueError(f"publication plan is not the Kitsu {FIRMWARE_VERSION} stable release")
    if not isinstance(plan["flash_release_id"], str) or not SAFE_TOKEN.fullmatch(plan["flash_release_id"]):
        raise ValueError("flash release ID is unsafe")
    require_sha256(plan["public_key_sha256"], "plan public key digest")
    require_sha256(plan["validator_sha256"], "plan validator digest")
    if plan["public_key_sha256"] != PUBLIC_KEY_SHA256:
        raise ValueError("publication plan update authority changed")
    validator = require_regular_file(stage / "tools/validate-stage.py", "stage validator")
    if sha256_file(validator) != plan["validator_sha256"]:
        raise ValueError("stage validator changed")

    manifest_record = require_object(plan["manifest"], "plan manifest")
    require_exact_keys(manifest_record, {"bytes", "sha256"}, "plan manifest")
    if (
        type(manifest_record["bytes"]) is not int
        or not 2 <= manifest_record["bytes"] <= 65536
    ):
        raise ValueError("plan manifest byte count is invalid")
    require_sha256(manifest_record["sha256"], "plan manifest digest")
    manifest_path = require_regular_file(stage / "update/latest.json", "latest.json")
    if manifest_path.stat().st_size != manifest_record["bytes"] or sha256_file(manifest_path) != manifest_record["sha256"]:
        raise ValueError("canonical update manifest changed")
    manifest = read_json(manifest_path, "latest.json")
    if canonical_bytes(manifest) != manifest_path.read_bytes():
        raise ValueError("latest.json is not canonical sorted-key JSON plus LF")
    manifest_payloads, required_hashes = validate_update_manifest(manifest)

    update_expected = record_map(plan["update_payload_files"], "update payload files")
    public_key_path = require_regular_file(
        stage / "update/update-ed25519-public.pem", "update authority public key"
    )
    expected_payloads = {
        "update-ed25519-public.pem": (
            public_key_path.stat().st_size,
            PUBLIC_KEY_SHA256,
        ),
        **manifest_payloads,
    }
    if update_expected != expected_payloads:
        raise ValueError("publication plan does not contain the exact signed firmware payloads")
    update_expected["latest.json"] = (
        manifest_path.stat().st_size,
        sha256_file(manifest_path),
    )
    signature_path = stage / "update/latest.json.sig"
    if require_signature:
        signature_path = require_regular_file(signature_path, "latest.json.sig")
        if signature_path.stat().st_size != 64:
            raise ValueError("latest.json.sig must be exactly 64 bytes")
        update_expected["latest.json.sig"] = (64, sha256_file(signature_path))
    elif signature_path.exists() or signature_path.is_symlink():
        raise ValueError("unsigned validation refuses an unexpected signature")
    verify_records(stage / "update", update_expected, "update tree")

    flash_expected = record_map(plan["flash_files"], "flash files")
    verify_records(stage / "flash", flash_expected, "flash tree")
    if any(path.lower().endswith((".map", ".bin", ".k868", ".aab", ".idsig", ".jks", ".keystore", ".pem", ".key")) for path in flash_expected):
        raise ValueError("flash tree includes a forbidden public asset")
    pet_paths = [path for path in flash_expected if path.endswith(".pet")]
    if len(pet_paths) != 3:
        raise ValueError("flash tree must contain exactly three starter .pet bundles")
    pet_hashes = {flash_expected[path][1] for path in pet_paths}
    if pet_hashes != PUBLIC_STARTER_PACK_SHA256:
        raise ValueError("flash tree contains a non-starter companion bundle")
    bundled_javascript = b"\n".join(
        (stage / "flash" / path).read_bytes()
        for path in flash_expected
        if path.endswith(".js")
    )
    for digest in required_hashes | {PUBLIC_KEY_SPKI_SHA256}:
        if digest.encode("ascii") not in bundled_javascript:
            raise ValueError(f"flash tree is not bound to reviewed digest {digest}")

    stage_expected = {
        "publication-plan.json",
        "tools/validate-stage.py",
        *[f"flash/{path}" for path in flash_expected],
        *[f"update/{path}" for path in update_expected],
    }
    actual_stage = set(relative_files(stage))
    if actual_stage != stage_expected:
        raise ValueError(
            f"stage inventory differs: missing={sorted(stage_expected - actual_stage)} "
            f"extra={sorted(actual_stage - stage_expected)}"
        )

    if require_signature:
        public_key = stage / "update/update-ed25519-public.pem"
        completed = subprocess.run(
            [
                openssl,
                "pkeyutl",
                "-verify",
                "-pubin",
                "-inkey",
                str(public_key),
                "-rawin",
                "-in",
                str(manifest_path),
                "-sigfile",
                str(signature_path),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            check=False,
        )
        if completed.returncode != 0:
            raise ValueError("latest.json Ed25519 signature is invalid")
    return plan


def validate_command(args: argparse.Namespace) -> int:
    plan = validate_stage(
        args.stage.resolve(),
        require_signature=args.require_signature,
        openssl=args.openssl,
    )
    print(
        "KITSU_FIRMWARE_0174_STAGE_VALID "
        f"release={plan['release_id']} signed={str(args.require_signature).lower()} "
        f"manifest_sha256={plan['manifest']['sha256']}"
    )
    return 0


def parser() -> argparse.ArgumentParser:
    root = Path(__file__).resolve().parents[1]
    command = argparse.ArgumentParser()
    subcommands = command.add_subparsers(dest="command", required=True)

    prepare_parser = subcommands.add_parser("prepare")
    prepare_parser.add_argument("--package-dir", type=Path, required=True)
    prepare_parser.add_argument("--acceptance", type=Path, required=True)
    prepare_parser.add_argument("--public-key", type=Path, required=True)
    prepare_parser.add_argument("--stage-dir", type=Path, required=True)
    prepare_parser.add_argument("--project-root", type=Path, default=root)
    prepare_parser.add_argument("--openssl", default="openssl")
    prepare_parser.set_defaults(handler=prepare)

    validate_parser = subcommands.add_parser("validate-stage")
    validate_parser.add_argument("--stage", type=Path, required=True)
    validate_parser.add_argument("--require-signature", action="store_true")
    validate_parser.add_argument("--openssl", default="openssl")
    validate_parser.set_defaults(handler=validate_command)
    return command


def main() -> int:
    args = parser().parse_args()
    try:
        return args.handler(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"firmware publication preparation failed: {error}") from error


if __name__ == "__main__":
    raise SystemExit(main())
