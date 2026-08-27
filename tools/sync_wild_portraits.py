#!/usr/bin/env python3
"""Synchronize final wild-pack portraits into firmware and Android sources.

The input must be the complete private manifest produced by
``build_wild_packs.py``.  This tool copies only each pack's exact 16x18,
36-byte portrait representation; it never reads or publishes pack bytes or
animation sources.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import tempfile
from dataclasses import dataclass
from pathlib import Path


MANIFEST_SCHEMA = "kitsu-wild-pack-private-release-v4"
EXPECTED_CREATURES = 21
PORTRAIT_WIDTH = 16
PORTRAIT_HEIGHT = 18
PORTRAIT_BYTES = 36
PORTRAIT_STORAGE = "XBM least-significant-bit first, two bytes per row"
DIRECT_LOCK_SCHEMA = "kitsu-wild-identity-lock-v2"
IMAGEGEN_LOCK_SCHEMA = "kitsu-wild-imagegen-import-lock-v3"
GENERATED_ACTION_SEMANTIC_SCHEMA = (
    "kitsu-wild-generated-action-semantic-locality-v1"
)
DIRECT_RASTER_TRANSFORM = "none-direct-exact-target"
IMAGEGEN_RASTER_TRANSFORM = (
    "rgba-over-white-box-area-black-coverage-then-fixed-offset"
)
IMAGEGEN_ACTION_SHEET_RASTER_TRANSFORM = (
    "rgba-over-white-fixed-action-sheet-cells-box-area-black-coverage-"
    "then-fixed-offset"
)
EXPECTED_ACTION_SHEET_LAYOUT = {
    "cell_outer_safe_guard_pixels": 2,
    "cell_transform": "identity-pinned-box-area-coverage-fixed-offset",
    "fixed_cell_extraction": True,
    "gutter_rects": [[560, 0, 562, 1402], [0, 700, 1122, 702]],
    "per_cell_cleanup": False,
    "per_cell_crop": False,
    "per_cell_fit": False,
    "per_cell_offset": False,
    "per_cell_threshold": False,
    "phase_order": [0, 1, 2, 3],
    "phase_viewports": [
        [0, 0, 560, 700],
        [562, 0, 1122, 700],
        [0, 702, 560, 1402],
        [562, 702, 1122, 1402],
    ],
    "schema": "kitsu-imagegen-action-sheet-2x2-v1",
    "source_asset_per_action": True,
    "source_canvas": [1122, 1402],
}
TRANSFORM_CONTROLS = {
    "auto_fit": False,
    "crop_by_subject": False,
    "cleanup": False,
    "per_frame_translation": False,
    "per_frame_threshold": False,
}
EXPECTED_RASTER_CONTRACT = {
    "allowed_pack_transforms": [
        DIRECT_RASTER_TRANSFORM,
        IMAGEGEN_RASTER_TRANSFORM,
        IMAGEGEN_ACTION_SHEET_RASTER_TRANSFORM,
    ],
    **TRANSFORM_CONTROLS,
    "source_snapshots": True,
    "portrait_source": "independently-authored-exact-16x18",
    "portrait_resampling": "none",
}

FIRMWARE_RELATIVE_PATH = Path("src/wild_creature_catalog.cpp")
ANDROID_RELATIVE_PATH = Path(
    "platform/mobile/android/app/src/main/java/ptl/kitsu/app/ui/"
    "NearbyKitsuPresentation.kt"
)

CATALOG_ENTRY_RE = re.compile(
    r"\{\s*0x(?P<pack_id>[0-9a-fA-F]{8})UL,\s*"
    r'"(?P<name>[A-Za-z][A-Za-z ]+)",\s*'
    r"signal::Rarity::[A-Za-z]+,\s*"
    r"Portrait::(?P<portrait>[A-Za-z][A-Za-z0-9]*),\s*true\s*\}",
    re.MULTILINE,
)


@dataclass(frozen=True)
class PortraitRecord:
    identity_key: str
    pack_id: str
    display_name: str
    bitmap: bytes
    bitmap_base64: str


def parse_args() -> argparse.Namespace:
    project_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description=(
            "Sync the complete private wild-pack portrait manifest into the "
            "firmware and Android static portrait catalogs."
        )
    )
    parser.add_argument("manifest", type=Path, help="complete private manifest")
    parser.add_argument(
        "--project-root",
        type=Path,
        default=project_root,
        help="public source checkout (defaults to this script's repository)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="validate and report drift without writing either target",
    )
    return parser.parse_args()


def require_mapping(value: object, label: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be an object")
    return value


def require_list(value: object, label: str) -> list[object]:
    if not isinstance(value, list):
        raise ValueError(f"{label} must be an array")
    return value


def require_fail_closed_manifest(manifest: dict[str, object]) -> None:
    if manifest.get("schema") != MANIFEST_SCHEMA:
        raise ValueError(f"manifest schema must be {MANIFEST_SCHEMA}")
    if manifest.get("complete_roster") is not True:
        raise ValueError("manifest must declare complete_roster=true")
    if manifest.get("non_destructive_build") is not True:
        raise ValueError("manifest must declare non_destructive_build=true")
    raster_contract = require_mapping(
        manifest.get("raster_contract"), "raster_contract"
    )
    if raster_contract != EXPECTED_RASTER_CONTRACT:
        raise ValueError("manifest raster_contract is not the fail-closed v4 contract")
    if manifest.get("identity_lock_schema") not in {
        DIRECT_LOCK_SCHEMA,
        IMAGEGEN_LOCK_SCHEMA,
    }:
        raise ValueError("manifest uses an unsupported or legacy identity lock")


def require_sha256(value: object, label: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
        raise ValueError(f"{label} must be a lowercase SHA-256")
    return value


def require_imagegen_transform(raw: object, identity_key: str) -> dict[str, object]:
    transform = require_mapping(raw, f"{identity_key}.identity_lock.transform")
    if set(transform) != {
        "action_output_offset",
        "alpha_background",
        "black_coverage_threshold_per_mille",
        "crop_rect",
        "luminance_mode",
        "output_canvas",
        "output_offset",
        "resample_mode",
        "source_canvas",
    }:
        raise ValueError(f"{identity_key} ImageGen transform has unexpected fields")
    source = transform.get("source_canvas")
    crop = transform.get("crop_rect")
    offset = transform.get("output_offset")
    action_offset = transform.get("action_output_offset")
    threshold = transform.get("black_coverage_threshold_per_mille")
    if (
        not isinstance(source, list)
        or len(source) != 2
        or any(not isinstance(value, int) for value in source)
        or source[0] < 800
        or source[1] < 1000
        or not isinstance(crop, list)
        or len(crop) != 4
        or any(not isinstance(value, int) for value in crop)
        or not isinstance(offset, list)
        or len(offset) != 2
        or any(type(value) is not int for value in offset)
        or not isinstance(action_offset, list)
        or len(action_offset) != 2
        or any(type(value) is not int for value in action_offset)
        or not isinstance(threshold, int)
        or not 50 <= threshold <= 500
        or transform.get("alpha_background") != [255, 255, 255]
        or transform.get("luminance_mode") != "pillow-rgb-luma-over-white"
        or transform.get("output_canvas") != [64, 80]
        or transform.get("resample_mode") != "box-area"
    ):
        raise ValueError(f"{identity_key} ImageGen transform is not canonical")
    left, top, right, bottom = crop
    source_width, source_height = source
    crop_width = right - left
    crop_height = bottom - top
    if (
        not (0 <= left < right <= source_width)
        or not (0 <= top < bottom <= source_height)
        or crop_width * 5 != crop_height * 4
        or left + right != source_width
        or top + bottom != source_height
        or crop_width < 800
        or crop_height < 1000
        or max(left, top, source_width - right, source_height - bottom) > 2
        or abs(offset[0]) >= 64
        or abs(offset[1]) >= 80
        or abs(action_offset[0]) >= 64
        or abs(action_offset[1]) >= 80
        or action_offset == offset
    ):
        raise ValueError(f"{identity_key} ImageGen transform changes the fixed viewport")
    return transform


def require_pack_raster_provenance(
    pack: dict[str, object], identity_key: str
) -> None:
    transform_kind = pack.get("raster_transform")
    if transform_kind not in {
        DIRECT_RASTER_TRANSFORM,
        IMAGEGEN_RASTER_TRANSFORM,
        IMAGEGEN_ACTION_SHEET_RASTER_TRANSFORM,
    }:
        raise ValueError(f"{identity_key} was not built by the fail-closed raster path")
    if pack.get("transform_controls") != TRANSFORM_CONTROLS:
        raise ValueError(f"{identity_key} permits a destructive per-frame transform")
    if (
        pack.get("format_version") != 2
        or pack.get("frame_canvas") != [64, 80]
        or pack.get("pack_bytes") != 31_120
    ):
        raise ValueError(f"{identity_key} is not an exact 64x80 K868PK1 v2 pack")

    identity_lock = require_mapping(
        pack.get("identity_lock"), f"{identity_key}.identity_lock"
    )
    lock_schema = identity_lock.get("schema")
    if transform_kind == DIRECT_RASTER_TRANSFORM:
        if set(identity_lock) != {
            "frame_canvas",
            "identity_frame_sha256",
            "identity_sha256",
            "schema",
        } or lock_schema != DIRECT_LOCK_SCHEMA:
            raise ValueError(f"{identity_key} direct-target identity lock is invalid")
        if identity_lock.get("frame_canvas") != [64, 80]:
            raise ValueError(f"{identity_key} direct-target lock has a wrong canvas")
        require_sha256(identity_lock.get("identity_sha256"), f"{identity_key}.identity_sha256")
        require_sha256(
            identity_lock.get("identity_frame_sha256"),
            f"{identity_key}.identity_frame_sha256",
        )
        if (
            pack.get("source_kind") != "direct-exact-target"
            or pack.get("fixed_action_scale") != 1.0
            or pack.get("identity_raster_scale") != 1.0
            or pack.get("action_cell_raster_scale") != 1.0
            or "action_source_layout" in pack
            or "action_source_layout_sha256" in pack
            or "action_output_offset" in pack
        ):
            raise ValueError(f"{identity_key} direct-target build changed scale")
    else:
        if set(identity_lock) != {
            "action_semantic_contract",
            "action_semantic_contract_sha256",
            "identity_frame_sha256",
            "identity_source_sha256",
            "schema",
            "transform",
            "transform_sha256",
        } or lock_schema != IMAGEGEN_LOCK_SCHEMA:
            raise ValueError(f"{identity_key} ImageGen identity lock is invalid")
        require_sha256(
            identity_lock.get("identity_source_sha256"),
            f"{identity_key}.identity_source_sha256",
        )
        require_sha256(
            identity_lock.get("identity_frame_sha256"),
            f"{identity_key}.identity_frame_sha256",
        )
        transform = require_imagegen_transform(identity_lock.get("transform"), identity_key)
        transform_sha256 = require_sha256(
            identity_lock.get("transform_sha256"),
            f"{identity_key}.transform_sha256",
        )
        canonical = json.dumps(
            transform,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
        ).encode("ascii")
        if hashlib.sha256(canonical).hexdigest() != transform_sha256:
            raise ValueError(f"{identity_key} ImageGen transform hash does not match")
        semantic_contract = require_mapping(
            identity_lock.get("action_semantic_contract"),
            f"{identity_key}.action_semantic_contract",
        )
        if (
            set(semantic_contract) != {"roles", "schema"}
            or semantic_contract.get("schema") != GENERATED_ACTION_SEMANTIC_SCHEMA
        ):
            raise ValueError(
                f"{identity_key} generated semantic contract is missing or unsafe"
            )
        semantic_roles = require_list(
            semantic_contract.get("roles"),
            f"{identity_key}.action_semantic_contract.roles",
        )
        if len(semantic_roles) != 12:
            raise ValueError(
                f"{identity_key} generated semantic contract must cover all roles"
            )
        semantic_hash = require_sha256(
            identity_lock.get("action_semantic_contract_sha256"),
            f"{identity_key}.action_semantic_contract_sha256",
        )
        canonical_semantics = json.dumps(
            semantic_contract,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
        ).encode("ascii")
        if hashlib.sha256(canonical_semantics).hexdigest() != semantic_hash:
            raise ValueError(
                f"{identity_key} generated semantic contract hash does not match"
            )
        crop = transform["crop_rect"]
        identity_scale = 64 / (crop[2] - crop[0])
        action_sheet = transform_kind == IMAGEGEN_ACTION_SHEET_RASTER_TRANSFORM
        action_scale = 64 / 560 if action_sheet else identity_scale
        scale_values = (
            pack.get("identity_raster_scale"),
            pack.get("action_cell_raster_scale"),
            pack.get("fixed_action_scale"),
        )
        if (
            any(not isinstance(value, (int, float)) for value in scale_values)
            or abs(scale_values[0] - identity_scale) > 1e-12
            or abs(scale_values[1] - action_scale) > 1e-12
            or abs(scale_values[2] - action_scale) > 1e-12
        ):
            raise ValueError(f"{identity_key} ImageGen fixed scale does not match its lock")
        if action_sheet:
            if pack.get("source_kind") != "imagegen-one-action-sheets":
                raise ValueError(f"{identity_key} action-sheet source kind is invalid")
            layout = require_mapping(
                pack.get("action_source_layout"),
                f"{identity_key}.action_source_layout",
            )
            if layout != EXPECTED_ACTION_SHEET_LAYOUT:
                raise ValueError(f"{identity_key} action-sheet layout is not canonical")
            layout_sha256 = require_sha256(
                pack.get("action_source_layout_sha256"),
                f"{identity_key}.action_source_layout_sha256",
            )
            canonical_layout = json.dumps(
                layout,
                sort_keys=True,
                separators=(",", ":"),
                ensure_ascii=True,
            ).encode("ascii")
            if hashlib.sha256(canonical_layout).hexdigest() != layout_sha256:
                raise ValueError(f"{identity_key} action-sheet layout hash does not match")
            action_output_offset = pack.get("action_output_offset")
            if action_output_offset != transform["action_output_offset"]:
                raise ValueError(
                    f"{identity_key} action-sheet output offset differs from its "
                    "hash-bound identity lock"
                )
        elif (
            pack.get("source_kind") != "imagegen-locked-import"
            or "action_source_layout" in pack
            or "action_source_layout_sha256" in pack
            or "action_output_offset" in pack
        ):
            raise ValueError(f"{identity_key} independent-frame layout is invalid")

    source_snapshot = require_mapping(
        pack.get("source_snapshot"), f"{identity_key}.source_snapshot"
    )
    snapshot_hashes = require_mapping(
        source_snapshot.get("byte_exact_sha256"),
        f"{identity_key}.source_snapshot.byte_exact_sha256",
    )
    if snapshot_hashes != pack.get("source_sha256"):
        raise ValueError(f"{identity_key} source snapshot hashes do not match inputs")


def load_records(manifest_path: Path) -> dict[str, PortraitRecord]:
    try:
        manifest = require_mapping(
            json.loads(manifest_path.read_text(encoding="utf-8")), "manifest"
        )
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read private manifest: {error}") from error

    require_fail_closed_manifest(manifest)

    identity_keys = require_list(manifest.get("identity_keys"), "identity_keys")
    packs = require_list(manifest.get("packs"), "packs")
    if len(identity_keys) != EXPECTED_CREATURES or len(packs) != EXPECTED_CREATURES:
        raise ValueError(
            f"manifest must contain exactly {EXPECTED_CREATURES} identities and packs"
        )
    if len(set(identity_keys)) != EXPECTED_CREATURES:
        raise ValueError("manifest identity_keys must be unique")

    records: dict[str, PortraitRecord] = {}
    pack_identity_keys: set[str] = set()
    display_names: set[str] = set()
    for index, raw_pack in enumerate(packs):
        pack = require_mapping(raw_pack, f"packs[{index}]")
        identity_key = pack.get("identity_key")
        display_name = pack.get("display_name")
        pack_id = pack.get("pack_id")
        if not isinstance(identity_key, str) or not re.fullmatch(
            r"[a-z][a-z0-9_]*", identity_key
        ):
            raise ValueError(f"packs[{index}] has invalid identity_key")
        owner_private_key = "fox" + "_girl"
        owner_private_name = "Fox" + " Girl"
        if identity_key == owner_private_key or display_name == owner_private_name:
            raise ValueError("owner-private companion must remain absent")
        if not isinstance(display_name, str) or not re.fullmatch(
            r"[A-Za-z][A-Za-z ]+", display_name
        ):
            raise ValueError(f"packs[{index}] has invalid display_name")
        if not isinstance(pack_id, str) or not re.fullmatch(
            r"[0-9A-F]{8}", pack_id
        ):
            raise ValueError(f"packs[{index}] has invalid canonical pack_id")
        if pack.get("format") != "K868PK1":
            raise ValueError(f"{identity_key} is not an ordinary K868PK1 pack")
        if (
            pack.get("stored_frames") != 48
            or pack.get("clips") != 12
            or pack.get("steps") != 48
        ):
            raise ValueError(f"{identity_key} does not contain the complete 48/12 pack")
        require_pack_raster_provenance(pack, identity_key)
        if require_mapping(
            pack.get("identity_lock"), f"{identity_key}.identity_lock"
        ).get("schema") != manifest.get("identity_lock_schema"):
            raise ValueError(f"{identity_key} lock schema differs from the manifest")

        roles = require_list(pack.get("roles"), f"{identity_key}.roles")
        expected_roles = list((
            "idle",
            "blink",
            "pet",
            "sleep",
            "listen",
            "surprise",
            "play",
            "tired",
            "feed",
            "wake",
            "meet",
            "evolve",
        ))
        if [
            require_mapping(role, f"{identity_key}.roles").get("role")
            for role in roles
        ] != expected_roles:
            raise ValueError(f"{identity_key} roles are missing or out of order")
        action_sheet_pack = (
            pack.get("raster_transform")
            == IMAGEGEN_ACTION_SHEET_RASTER_TRANSFORM
        )
        action_source_hashes: list[str] = []
        for role in roles:
            role_record = require_mapping(role, f"{identity_key}.role")
            if "action_output_offset" in role_record:
                raise ValueError(
                    f"{identity_key}.{role_record.get('role')} cannot override the "
                    "single hash-bound action-sheet output offset"
                )
            if role_record.get("unique_frames") != 4:
                raise ValueError(f"{identity_key} has a collapsed animation role")
            if pack.get("raster_transform") != DIRECT_RASTER_TRANSFORM:
                semantic_locality = require_mapping(
                    role_record.get("semantic_locality"),
                    f"{identity_key}.{role_record.get('role')}.semantic_locality",
                )
                if (
                    semantic_locality.get("schema")
                    != GENERATED_ACTION_SEMANTIC_SCHEMA
                    or semantic_locality.get("role") != role_record.get("role")
                    or len(
                        require_list(
                            semantic_locality.get("phases"),
                            f"{identity_key}.{role_record.get('role')}.semantic_phases",
                        )
                    )
                    != 4
                    or not require_list(
                        semantic_locality.get("motion_landmarks"),
                        f"{identity_key}.{role_record.get('role')}.motion_landmarks",
                    )
                ):
                    raise ValueError(
                        f"{identity_key}.{role_record.get('role')} lacks semantic "
                        "locality evidence"
                    )
            phase_hash_fields = ["final_mask_sha256", "frame_sha256"]
            if action_sheet_pack:
                action_source_sha256 = require_sha256(
                    role_record.get("action_source_sha256"),
                    f"{identity_key}.{role_record.get('role')}.action_source_sha256",
                )
                action_source_hashes.append(action_source_sha256)
                source_hashes = require_mapping(
                    pack.get("source_sha256"), f"{identity_key}.source_sha256"
                )
                if source_hashes.get(f"{role_record.get('role')}.png") != action_source_sha256:
                    raise ValueError(
                        f"{identity_key}.{role_record.get('role')} action source "
                        "hash differs from its snapshot manifest"
                    )
                phase_hash_fields.append("source_region_sha256")
                if "source_sha256" in role_record:
                    raise ValueError(
                        f"{identity_key}.{role_record.get('role')} action sheet "
                        "cannot claim independent source files"
                    )
            else:
                phase_hash_fields.append("source_sha256")
                if (
                    "action_source_sha256" in role_record
                    or "source_region_sha256" in role_record
                ):
                    raise ValueError(
                        f"{identity_key}.{role_record.get('role')} independent "
                        "layout cannot claim an action sheet"
                    )

            for field in phase_hash_fields:
                hashes = require_list(
                    role_record.get(field), f"{identity_key}.{role_record.get('role')}.{field}"
                )
                if len(hashes) != 4:
                    raise ValueError(
                        f"{identity_key}.{role_record.get('role')}.{field} "
                        "must pin four independent frames"
                    )
                for frame_index, digest in enumerate(hashes):
                    require_sha256(
                        digest,
                        f"{identity_key}.{role_record.get('role')}.{field}[{frame_index}]",
                    )
                if len(set(hashes)) != 4:
                    raise ValueError(
                        f"{identity_key}.{role_record.get('role')}.{field} "
                        "must pin four independent frames"
                    )
        if action_sheet_pack and len(set(action_source_hashes)) != len(expected_roles):
            raise ValueError(f"{identity_key} reuses one action sheet for multiple roles")

        portrait = require_mapping(pack.get("portrait"), f"{identity_key}.portrait")
        if (
            portrait.get("width") != PORTRAIT_WIDTH
            or portrait.get("height") != PORTRAIT_HEIGHT
            or portrait.get("bytes") != PORTRAIT_BYTES
            or portrait.get("storage") != PORTRAIT_STORAGE
            or portrait.get("source") != "independently-authored-exact-16x18"
            or portrait.get("resampling") != "none"
        ):
            raise ValueError(
                f"{identity_key} portrait contract is not identity-locked 16x18 XBM"
            )
        portrait_source_sha256 = require_sha256(
            portrait.get("source_sha256"), f"{identity_key}.portrait.source_sha256"
        )
        if (
            portrait.get("png_sha256") != portrait_source_sha256
            or require_mapping(
                pack.get("source_sha256"), f"{identity_key}.source_sha256"
            ).get("portrait.png")
            != portrait_source_sha256
        ):
            raise ValueError(f"{identity_key} portrait is not a byte-exact source copy")
        bitmap_hex = portrait.get("bitmap_hex")
        bitmap_base64 = portrait.get("bitmap_base64")
        if not isinstance(bitmap_hex, str) or not re.fullmatch(
            rf"[0-9a-f]{{{PORTRAIT_BYTES * 2}}}", bitmap_hex
        ):
            raise ValueError(f"{identity_key} has invalid portrait bitmap_hex")
        if not isinstance(bitmap_base64, str):
            raise ValueError(f"{identity_key} has invalid portrait bitmap_base64")
        bitmap = bytes.fromhex(bitmap_hex)
        try:
            decoded = base64.b64decode(bitmap_base64, validate=True)
        except ValueError as error:
            raise ValueError(
                f"{identity_key} portrait bitmap_base64 is malformed"
            ) from error
        if decoded != bitmap:
            raise ValueError(
                f"{identity_key} portrait hex/Base64 values describe different bytes"
            )
        if pack_id in records:
            raise ValueError(f"duplicate pack_id in manifest: {pack_id}")
        if display_name in display_names:
            raise ValueError(f"duplicate display_name in manifest: {display_name}")

        pack_identity_keys.add(identity_key)
        display_names.add(display_name)
        records[pack_id] = PortraitRecord(
            identity_key=identity_key,
            pack_id=pack_id,
            display_name=display_name,
            bitmap=bitmap,
            bitmap_base64=bitmap_base64,
        )

    if pack_identity_keys != set(identity_keys):
        raise ValueError("manifest identity_keys do not match its packs")
    return records


def parse_firmware_catalog(
    source: str,
) -> dict[str, tuple[str, str]]:
    block_match = re.search(
        r"constexpr Creature kCreatures\[\]\s*=\s*\{(?P<body>.*?)\n\};",
        source,
        re.DOTALL,
    )
    if block_match is None:
        raise ValueError("cannot locate firmware wild-creature catalog")

    entries: dict[str, tuple[str, str]] = {}
    for match in CATALOG_ENTRY_RE.finditer(block_match.group("body")):
        pack_id = match.group("pack_id").upper()
        if pack_id in entries:
            raise ValueError(f"duplicate firmware pack_id: {pack_id}")
        entries[pack_id] = (match.group("name"), match.group("portrait"))
    if len(entries) != EXPECTED_CREATURES:
        raise ValueError(
            f"firmware catalog must contain exactly {EXPECTED_CREATURES} entries"
        )
    owner_private_name = "Fox" + " Girl"
    if any(name == owner_private_name for name, _ in entries.values()):
        raise ValueError("owner-private companion must remain absent from the firmware catalog")
    return entries


def require_matching_roster(
    records: dict[str, PortraitRecord],
    catalog: dict[str, tuple[str, str]],
) -> None:
    if set(records) != set(catalog):
        missing = sorted(set(catalog) - set(records))
        extra = sorted(set(records) - set(catalog))
        raise ValueError(f"manifest/catalog pack IDs differ: missing={missing} extra={extra}")
    for pack_id, record in records.items():
        expected_name = catalog[pack_id][0]
        if record.display_name != expected_name:
            raise ValueError(
                f"pack {pack_id} name mismatch: manifest={record.display_name!r} "
                f"firmware={expected_name!r}"
            )


def format_firmware_array(name: str, bitmap: bytes) -> str:
    rows = []
    for offset in range(0, len(bitmap), 10):
        chunk = bitmap[offset : offset + 10]
        rows.append("    " + ", ".join(f"0x{value:02x}" for value in chunk) + ",")
    return (
        f"constexpr uint8_t {name}[kPortraitBytes] = {{\n"
        + "\n".join(rows)
        + "\n};"
    )


def render_firmware(
    source: str,
    records: dict[str, PortraitRecord],
    catalog: dict[str, tuple[str, str]],
) -> str:
    rendered = source
    for pack_id, record in records.items():
        portrait_enum = catalog[pack_id][1]
        array_name = f"k{portrait_enum}Portrait"
        pattern = re.compile(
            rf"constexpr uint8_t {re.escape(array_name)}\[kPortraitBytes\]"
            rf"\s*=\s*\{{.*?\n\}};",
            re.DOTALL,
        )
        rendered, replacements = pattern.subn(
            format_firmware_array(array_name, record.bitmap), rendered
        )
        if replacements != 1:
            raise ValueError(
                f"expected one firmware portrait array {array_name}; found {replacements}"
            )
    return rendered


def render_android(
    source: str,
    records: dict[str, PortraitRecord],
) -> str:
    rendered = source
    discovered = {
        match.group("pack_id").upper(): match.group("name")
        for match in re.finditer(
            r"0x(?P<pack_id>[0-9A-Fa-f]{8})L\s+to\s+portrait\(\s*"
            r'"(?P<name>[A-Za-z][A-Za-z ]+)",\s*16,\s*18,',
            source,
        )
    }
    if discovered != {key: value.display_name for key, value in records.items()}:
        raise ValueError("Android 16x18 portrait roster differs from the manifest")

    for pack_id, record in records.items():
        pattern = re.compile(
            rf"(?m)^(?P<indent>[ \t]*)0x{pack_id}L\s+to\s+portrait\(\s*"
            rf'"{re.escape(record.display_name)}",\s*16,\s*18,\s*'
            rf'"[A-Za-z0-9+/=]+"\s*\),\s*$'
        )

        def replacement(match: re.Match[str]) -> str:
            return (
                f'{match.group("indent")}0x{pack_id}L to portrait('
                f'"{record.display_name}", 16, 18, "{record.bitmap_base64}"),'
            )

        rendered, replacements = pattern.subn(replacement, rendered)
        if replacements != 1:
            raise ValueError(
                f"expected one Android portrait entry for {pack_id}; found {replacements}"
            )
    return rendered


def atomic_write(path: Path, content: str) -> None:
    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        newline="\n",
        dir=path.parent,
        prefix=f".{path.name}.",
        suffix=".tmp",
        delete=False,
    ) as temporary:
        temporary.write(content)
        temporary_path = Path(temporary.name)
    try:
        os.replace(temporary_path, path)
    except BaseException:
        temporary_path.unlink(missing_ok=True)
        raise


def main() -> int:
    args = parse_args()
    project_root = args.project_root.resolve()
    manifest_path = args.manifest.resolve()
    firmware_path = project_root / FIRMWARE_RELATIVE_PATH
    android_path = project_root / ANDROID_RELATIVE_PATH

    records = load_records(manifest_path)
    firmware_source = firmware_path.read_text(encoding="utf-8")
    android_source = android_path.read_text(encoding="utf-8")
    catalog = parse_firmware_catalog(firmware_source)
    require_matching_roster(records, catalog)

    rendered_firmware = render_firmware(firmware_source, records, catalog)
    rendered_android = render_android(android_source, records)
    changed = [
        label
        for label, before, after in (
            ("firmware", firmware_source, rendered_firmware),
            ("android", android_source, rendered_android),
        )
        if before != after
    ]

    if args.check:
        print(
            "WILD_PORTRAIT_SYNC_CHECK "
            f"records={len(records)} drift={','.join(changed) if changed else 'none'}"
        )
        return 1 if changed else 0

    if firmware_source != rendered_firmware:
        atomic_write(firmware_path, rendered_firmware)
    if android_source != rendered_android:
        atomic_write(android_path, rendered_android)
    print(
        "WILD_PORTRAIT_SYNCED "
        f"records={len(records)} changed={','.join(changed) if changed else 'none'}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
