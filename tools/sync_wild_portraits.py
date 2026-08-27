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


MANIFEST_SCHEMA = "kitsu-wild-pack-private-release-v6"
EXPECTED_CREATURES = 21
PORTRAIT_WIDTH = 16
PORTRAIT_HEIGHT = 18
PORTRAIT_BYTES = 36
PORTRAIT_STORAGE = "XBM least-significant-bit first, two bytes per row"
DIRECT_LOCK_SCHEMA = "kitsu-wild-identity-lock-v2"
IMAGEGEN_LOCK_SCHEMA = "kitsu-wild-imagegen-import-lock-v4"
GENERATED_ACTION_SEMANTIC_SCHEMA = (
    "kitsu-wild-generated-action-semantic-locality-v3"
)
GENERATED_PHASE_PREAUTHORIZATION_SCHEMA = (
    "kitsu-wild-generated-phase-preauthorization-v2"
)
GENERATED_ROLE_REGISTRATION_SCHEMA = (
    "kitsu-wild-generated-role-registration-v1"
)
GENERATED_COMPOSITION_MODE = (
    "immutable-baseline-outside-mask-imported-candidate-inside-mask"
)
GENERATED_ROLE_OUTPUT_OFFSET_MAXIMUM = 4
GENERATED_IDENTITY_BASELINE_ASSET_LAYOUT = "immutable-identity-baseline-copy"
DIRECT_RASTER_TRANSFORM = "none-direct-exact-target"
IMAGEGEN_RASTER_TRANSFORM = (
    "rgba-over-white-box-area-fixed-role-registration-bounded-native-composite-v1"
)
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
    ],
    **TRANSFORM_CONTROLS,
    "source_snapshots": True,
    "portrait_source": "independently-authored-exact-16x18",
    "portrait_resampling": "none",
}
REQUIRED_ANIMATION_CONTRACT_VALUES = {
    "generated_action_semantic_schema": GENERATED_ACTION_SEMANTIC_SCHEMA,
    "preauthorization_schema": GENERATED_PHASE_PREAUTHORIZATION_SCHEMA,
    "phase_mask_frozen_before_generation": True,
    "bounded_composition_mode": GENERATED_COMPOSITION_MODE,
    "role_registration_schema": GENERATED_ROLE_REGISTRATION_SCHEMA,
    "maximum_absolute_role_output_offset_pixels": (
        GENERATED_ROLE_OUTPUT_OFFSET_MAXIMUM
    ),
    "per_phase_registration_override": False,
    "immutable_identity_reference_per_generated_phase": True,
    "identity_reference_image_number": 1,
    "immutable_edit_target_reference_per_generated_phase": True,
    "edit_target_reference_image_number": 2,
    "generated_phase_chaining": False,
    "identity_anchored_later_phases_use_role_p0_edit_target": True,
    "role_phase_0_generation_target": "approved-identity",
    "role_phase_0_exact_identity_baseline_copy_without_generation": True,
    "identity_baseline_copy_requires_byte_exact_source_and_zero_registration": (
        True
    ),
    "role_phase_1_to_3_generation_target": (
        "same-immutable-accepted-role-phase-0"
    ),
    "role_phase_0_is_generation_reference_for_role_phases_1_to_3": True,
    "production_out_of_region_pixel_budget": 0,
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
        raise ValueError("manifest raster_contract is not the fail-closed v6 contract")
    animation_contract = require_mapping(
        manifest.get("animation_contract"), "animation_contract"
    )
    for field, expected in REQUIRED_ANIMATION_CONTRACT_VALUES.items():
        if animation_contract.get(field) != expected:
            raise ValueError(
                f"manifest animation_contract.{field} is not fail-closed v6"
            )
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
        scale_values = (
            pack.get("identity_raster_scale"),
            pack.get("action_cell_raster_scale"),
            pack.get("fixed_action_scale"),
        )
        if (
            any(not isinstance(value, (int, float)) for value in scale_values)
            or abs(scale_values[0] - identity_scale) > 1e-12
            or abs(scale_values[1] - identity_scale) > 1e-12
            or abs(scale_values[2] - identity_scale) > 1e-12
        ):
            raise ValueError(f"{identity_key} ImageGen fixed scale does not match its lock")
        if (
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


IDENTITY_ANCHORED_ROLES = frozenset({"idle", "blink", "listen"})


def require_generated_semantic_evidence(
    identity_key: str,
    role_record: dict[str, object],
    identity_lock: dict[str, object],
    source_hashes: dict[str, object],
) -> None:
    """Verify the v4 bounded-composite evidence consumed by portrait sync."""

    role = role_record.get("role")
    if not isinstance(role, str):
        raise ValueError(f"{identity_key} generated role is invalid")
    label = f"{identity_key}.{role}.semantic_locality"
    semantic = require_mapping(role_record.get("semantic_locality"), label)
    expected_baseline = (
        "identity-anchored"
        if role in IDENTITY_ANCHORED_ROLES
        else "immutable-role-phase-0"
    )
    if (
        semantic.get("schema") != GENERATED_ACTION_SEMANTIC_SCHEMA
        or semantic.get("role") != role
        or semantic.get("source_layout") != "independent-frame"
        or semantic.get("baseline_policy") != expected_baseline
    ):
        raise ValueError(f"{label} lacks exact v4 role lineage")

    registration = require_mapping(
        semantic.get("role_registration"), f"{label}.role_registration"
    )
    if set(registration) != {
        "derivation",
        "output_offset",
        "p0_unregistered_floor_y",
        "schema",
    } or registration.get("schema") != GENERATED_ROLE_REGISTRATION_SCHEMA:
        raise ValueError(f"{label} role registration is missing or malformed")
    canonical_registration = json.dumps(
        registration,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    ).encode("ascii")
    registration_hash = require_sha256(
        semantic.get("role_registration_sha256"),
        f"{label}.role_registration_sha256",
    )
    if hashlib.sha256(canonical_registration).hexdigest() != registration_hash:
        raise ValueError(f"{label} role registration hash drifted")
    offset = registration.get("output_offset")
    floor = registration.get("p0_unregistered_floor_y")
    if (
        not isinstance(offset, list)
        or len(offset) != 2
        or any(type(value) is not int for value in offset)
        or any(abs(value) > GENERATED_ROLE_OUTPUT_OFFSET_MAXIMUM for value in offset)
        or type(floor) is not int
    ):
        raise ValueError(f"{label} role registration is out of range")
    if expected_baseline == "identity-anchored":
        if registration != {
            "derivation": "identity-anchored-zero-offset",
            "output_offset": [0, 0],
            "p0_unregistered_floor_y": 77,
            "schema": GENERATED_ROLE_REGISTRATION_SCHEMA,
        }:
            raise ValueError(f"{label} identity-anchored registration drifted")
    elif (
        registration.get("derivation")
        != "role-p0-fixed-dx-explicit-dy-floor-derived"
        or offset[1] != 77 - floor
    ):
        raise ValueError(f"{label} role-P0 registration is not floor-derived")

    phases = require_list(semantic.get("phases"), f"{label}.phases")
    frame_hashes = require_list(
        role_record.get("frame_sha256"), f"{identity_key}.{role}.frame_sha256"
    )
    generated_source_hashes = require_list(
        role_record.get("source_sha256"), f"{identity_key}.{role}.source_sha256"
    )
    if len(phases) != 4 or len(frame_hashes) != 4 or len(generated_source_hashes) != 4:
        raise ValueError(f"{label} must pin exactly four independent phases")
    identity_source_hash = require_sha256(
        identity_lock.get("identity_source_sha256"),
        f"{identity_key}.identity_source_sha256",
    )
    identity_frame_hash = require_sha256(
        identity_lock.get("identity_frame_sha256"),
        f"{identity_key}.identity_frame_sha256",
    )
    p0 = require_mapping(phases[0], f"{label}.phases[0]")
    p0_source_hash = p0.get("generated_source_sha256")
    p0_registered_hash = p0.get("registered_candidate_frame_sha256")
    p0_final_hash = p0.get("composited_frame_sha256")
    if semantic.get("role_pose_baseline_frame_sha256") != p0_final_hash:
        raise ValueError(f"{label} immutable role P0 hash drifted")

    for phase_index, raw_phase in enumerate(phases):
        phase = require_mapping(raw_phase, f"{label}.phases[{phase_index}]")
        if phase.get("phase") != phase_index:
            raise ValueError(f"{label} phases are missing or out of order")
        for field in (
            "allowed_change_mask_sha256",
            "preauthorization_source_sha256",
            "preauthorization_storyboard_sha256",
            "identity_reference_source_sha256",
            "edit_target_source_sha256",
            "edit_target_registered_frame_sha256",
            "edit_target_accepted_composited_frame_sha256",
            "generated_source_sha256",
            "imported_candidate_frame_sha256",
            "registered_candidate_frame_sha256",
            "composition_baseline_frame_sha256",
            "composited_frame_sha256",
        ):
            require_sha256(phase.get(field), f"{label}.phases[{phase_index}].{field}")
        if (
            phase.get("composition_mode") != GENERATED_COMPOSITION_MODE
            or phase.get("outside_allowed_change_pixels") != 0
            or type(phase.get("discarded_candidate_outside_mask_pixels")) is not int
            or phase.get("discarded_candidate_outside_mask_pixels") < 0
            or "output_offset" in phase
        ):
            raise ValueError(f"{label}.phases[{phase_index}] is not a bounded composite")
        if (
            phase.get("composited_frame_sha256") != frame_hashes[phase_index]
            or phase.get("generated_source_sha256")
            != generated_source_hashes[phase_index]
            or source_hashes.get(f"{role}/{phase_index:02d}.png")
            != phase.get("generated_source_sha256")
            or phase.get("identity_reference_source_sha256")
            != identity_source_hash
            or source_hashes.get(
                f"preauthorization/{role}/{phase_index:02d}.json"
            )
            != phase.get("preauthorization_source_sha256")
        ):
            raise ValueError(f"{label}.phases[{phase_index}] provenance drifted")

        asset_layout = phase.get("generated_asset_layout")
        if asset_layout == GENERATED_IDENTITY_BASELINE_ASSET_LAYOUT:
            if (
                phase_index != 0
                or registration.get("output_offset") != [0, 0]
                or phase.get("generated_source_sha256")
                != identity_source_hash
                or phase.get("imported_candidate_frame_sha256")
                != identity_frame_hash
                or phase.get("registered_candidate_frame_sha256")
                != identity_frame_hash
                or phase.get("composited_frame_sha256")
                != identity_frame_hash
            ):
                raise ValueError(
                    f"{label}.phases[{phase_index}] no-call identity baseline "
                    "copy drifted"
                )
        elif asset_layout != "independent-frame":
            raise ValueError(
                f"{label}.phases[{phase_index}] generated asset layout drifted"
            )

        identity_target = phase_index == 0
        expected_baseline_hash = (
            identity_frame_hash if identity_target else p0_final_hash
        )
        if phase.get("composition_baseline_frame_sha256") != expected_baseline_hash:
            raise ValueError(f"{label}.phases[{phase_index}] baseline hash drifted")
        if identity_target:
            if (
                phase.get("edit_target_kind")
                != "immutable-approved-identity-source"
                or phase.get("edit_target_relative_path") != "identity.png"
                or phase.get("edit_target_source_sha256") != identity_source_hash
                or phase.get("edit_target_registered_frame_sha256")
                != identity_frame_hash
                or phase.get("edit_target_accepted_composited_frame_sha256")
                != identity_frame_hash
            ):
                raise ValueError(
                    f"{label}.phases[{phase_index}] must target immutable identity"
                )
        elif (
            phase.get("edit_target_kind")
            != "immutable-accepted-role-phase-0"
            or phase.get("edit_target_relative_path") != f"{role}/00.png"
            or phase.get("edit_target_source_sha256") != p0_source_hash
            or phase.get("edit_target_registered_frame_sha256")
            != p0_registered_hash
            or phase.get("edit_target_accepted_composited_frame_sha256")
            != p0_final_hash
        ):
            raise ValueError(
                f"{label}.phases[{phase_index}] does not target the same immutable P0"
            )


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
        pack_identity_lock = require_mapping(
            pack.get("identity_lock"), f"{identity_key}.identity_lock"
        )
        if pack_identity_lock.get("schema") != manifest.get("identity_lock_schema"):
            raise ValueError(f"{identity_key} lock schema differs from the manifest")
        source_hashes = require_mapping(
            pack.get("source_sha256"), f"{identity_key}.source_sha256"
        )

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
        for role in roles:
            role_record = require_mapping(role, f"{identity_key}.role")
            if any(
                field in role_record
                for field in (
                    "action_output_offset",
                    "action_source_sha256",
                    "source_region_sha256",
                )
            ):
                raise ValueError(
                    f"{identity_key}.{role_record.get('role')} cannot use or "
                    "override a legacy action-sheet source"
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
            for field in ("final_mask_sha256", "frame_sha256", "source_sha256"):
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
            if pack.get("raster_transform") == IMAGEGEN_RASTER_TRANSFORM:
                require_generated_semantic_evidence(
                    identity_key,
                    role_record,
                    pack_identity_lock,
                    source_hashes,
                )

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
