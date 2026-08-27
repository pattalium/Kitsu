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


LEGACY_MANIFEST_SCHEMA = "kitsu-wild-pack-private-release-v6"
MANIFEST_SCHEMA = "kitsu-wild-pack-private-release-v7"
VISUAL_GATE_MANIFEST_SCHEMA = "kitsu-wild-pack-private-release-v8"
EXPECTED_CREATURES = 21
PORTRAIT_WIDTH = 16
PORTRAIT_HEIGHT = 18
PORTRAIT_BYTES = 36
PORTRAIT_STORAGE = "XBM least-significant-bit first, two bytes per row"
DIRECT_LOCK_SCHEMA = "kitsu-wild-identity-lock-v2"
LEGACY_IMAGEGEN_LOCK_SCHEMA = "kitsu-wild-imagegen-import-lock-v4"
IMAGEGEN_LOCK_SCHEMA = "kitsu-wild-imagegen-import-lock-v5"
LEGACY_GENERATED_ACTION_SEMANTIC_SCHEMA = (
    "kitsu-wild-generated-action-semantic-locality-v3"
)
GENERATED_ACTION_SEMANTIC_SCHEMA = (
    "kitsu-wild-generated-action-semantic-locality-v4"
)
NATIVE_GRID_REFERENCE_SCHEMA = (
    "kitsu-wild-native-grid-conditioning-reference-v1"
)
NATIVE_GRID_REFERENCE_KIND = "read-only-native-grid-conditioning-reference"
NATIVE_GRID_REFERENCE_DERIVATION = (
    "inverse-locked-output-offset-nearest-native-grid-v1"
)
P0_GENERATION_REFERENCE_MODE = "two-reference-identity-edit-v1"
TWO_REFERENCE_GENERATION_MODE = "two-reference-same-p0-star-v1"
THREE_REFERENCE_GENERATION_MODE = (
    "three-reference-same-p0-native-grid-star-v1"
)
SPECIES_ROLE_VISUAL_GATE_POLICY_SCHEMA = (
    "kitsu-species-role-visual-gates-v1"
)
SPECIES_ROLE_VISUAL_GATE_EVIDENCE_SCHEMA = (
    "kitsu-species-role-visual-gate-evidence-v1"
)
SPECIES_ROLE_VISUAL_GATE_POLICY_RELATIVE_PATH = (
    "assets/companion-sources/species-role-visual-gates-v1.json"
)
SPECIES_ROLE_VISUAL_GATE_KEYS = (
    ("axolotl", "idle"),
    ("axolotl", "blink"),
    ("axolotl", "pet"),
    ("rabbit", "idle"),
    ("rabbit", "blink"),
    ("rabbit", "listen"),
)
SPECIES_ROLE_VISUAL_GATE_IDENTITY_FRAME_SHA256 = {
    "axolotl": "4a46febde51aa1612632b5a2251a8f23eff02bb99e52597a48d073c2b373ac41",
    "rabbit": "8392e03d7b99b633ead2185d579c7f860e483eb46299d7d642890cf8059616e4",
}
SPECIES_ROLE_VISUAL_GATE_ENTRY_SHA256 = {
    ("axolotl", "idle"): (
        "cbccb33f1196708f6439fb1c630e8c59989bbe5beb360826c3d7ffa7a552b357"
    ),
    ("axolotl", "blink"): (
        "1bfb1e5003cd8f652b5462e033e91634b42cea23926f1fd6d08517aef387580f"
    ),
    ("axolotl", "pet"): (
        "abaa54c620357d05d86a63c0d6f70cb2edd9fa3ec387f7439f6226ac37ccabea"
    ),
    ("rabbit", "idle"): (
        "f55167effe20e2de5c75f67ce0f8b00b7072e3e62967c167b18c17cd3b50848b"
    ),
    ("rabbit", "blink"): (
        "e1c2c7d9c400393b58f790f1e39243e561a484efd58d4fd324a8732fef55ce36"
    ),
    ("rabbit", "listen"): (
        "94ba9d8a9c453025647a4f93d65adfae90e340bb18ef4ef2bf1e2fbff86544b6"
    ),
}
SPECIES_ROLE_VISUAL_GATE_KIND_SETS = {
    ("axolotl", "idle"): {
        "required_8_connected_anchors",
        "phase_delta_locality",
        "loop_seam",
    },
    ("axolotl", "blink"): {
        "required_8_connected_anchors",
        "pupil_only",
        "per_eye_occupancy",
    },
    ("axolotl", "pet"): {
        "required_8_connected_anchors",
        "localized_redraw",
        "rigid_pupil_translation",
        "gill_base_shared_transform",
    },
    ("rabbit", "idle"): {
        "phase_delta_locality",
        "split_nose_topology",
        "cadence",
    },
    ("rabbit", "blink"): {"eye_sequence"},
    ("rabbit", "listen"): {
        "skull_freeze",
        "ear_base_freeze",
        "local_scale_ink",
    },
}
SPECIES_ROLE_VISUAL_GATE_REASON_CODES = {
    ("axolotl", "idle", "required_8_connected_anchors"): [
        "AX_REQUIRED_LANDMARK_DISCONNECTED"
    ],
    ("axolotl", "idle", "phase_delta_locality"): [
        "AX_IDLE_GILL_PHASE_LOCALITY"
    ],
    ("axolotl", "idle", "loop_seam"): ["AX_IDLE_LOOP_SEAM"],
    ("axolotl", "blink", "required_8_connected_anchors"): [
        "AX_REQUIRED_LANDMARK_DISCONNECTED"
    ],
    ("axolotl", "blink", "pupil_only"): ["AX_BLINK_PUPIL_ONLY"],
    ("axolotl", "blink", "per_eye_occupancy"): [
        "AX_BLINK_PER_EYE_OCCUPANCY"
    ],
    ("axolotl", "pet", "required_8_connected_anchors"): [
        "AX_REQUIRED_LANDMARK_DISCONNECTED"
    ],
    ("axolotl", "pet", "localized_redraw"): ["AX_PET_REDRAW_BUDGET"],
    ("axolotl", "pet", "rigid_pupil_translation"): [
        "AX_PET_NONRIGID_PUPIL_TRANSFORM"
    ],
    ("axolotl", "pet", "gill_base_shared_transform"): [
        "AX_PET_GILL_BASE_DRIFT"
    ],
    ("rabbit", "idle", "phase_delta_locality"): [
        "RABBIT_IDLE_PHASE_RESIDUAL"
    ],
    ("rabbit", "idle", "split_nose_topology"): [
        "RABBIT_IDLE_SPLIT_NOSE_TOPOLOGY"
    ],
    ("rabbit", "idle", "cadence"): ["RABBIT_IDLE_CADENCE"],
    ("rabbit", "blink", "eye_sequence"): [
        "RABBIT_BLINK_EYE_MASS_SEQUENCE",
        "RABBIT_BLINK_LID_GEOMETRY",
        "RABBIT_BLINK_EYE_CENTROID_DRIFT",
    ],
    ("rabbit", "listen", "skull_freeze"): [
        "RABBIT_LISTEN_SKULL_FREEZE"
    ],
    ("rabbit", "listen", "ear_base_freeze"): [
        "RABBIT_LISTEN_EAR_BASE_FREEZE"
    ],
    ("rabbit", "listen", "local_scale_ink"): [
        "RABBIT_LISTEN_LOCAL_SCALE_INK"
    ],
}
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
LEGACY_REQUIRED_ANIMATION_CONTRACT_VALUES = {
    "generated_action_semantic_schema": (
        LEGACY_GENERATED_ACTION_SEMANTIC_SCHEMA
    ),
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
REQUIRED_ANIMATION_CONTRACT_VALUES = {
    **{
        key: value
        for key, value in LEGACY_REQUIRED_ANIMATION_CONTRACT_VALUES.items()
        if key != "generated_action_semantic_schema"
    },
    "generated_action_semantic_schema": GENERATED_ACTION_SEMANTIC_SCHEMA,
    "generated_action_semantic_schemas": [
        LEGACY_GENERATED_ACTION_SEMANTIC_SCHEMA,
        GENERATED_ACTION_SEMANTIC_SCHEMA,
    ],
    "generation_reference_modes": [
        P0_GENERATION_REFERENCE_MODE,
        TWO_REFERENCE_GENERATION_MODE,
        THREE_REFERENCE_GENERATION_MODE,
    ],
    "native_grid_reference_schema": NATIVE_GRID_REFERENCE_SCHEMA,
    "native_grid_reference_image_number": 3,
    "native_grid_reference_prompt_only": True,
    "native_grid_reference_phase_0": False,
    "native_grid_reference_per_phase_truthful": True,
    "native_grid_reference_same_role_p0_for_all_uses": True,
    "native_grid_reference_zero_registration_exact_copy_only": True,
    "native_grid_reference_derivation": NATIVE_GRID_REFERENCE_DERIVATION,
}
VISUAL_GATE_REQUIRED_ANIMATION_CONTRACT_VALUES = {
    **REQUIRED_ANIMATION_CONTRACT_VALUES,
    "species_role_visual_gate_policy_schema": (
        SPECIES_ROLE_VISUAL_GATE_POLICY_SCHEMA
    ),
    "species_role_visual_gate_evidence_schema": (
        SPECIES_ROLE_VISUAL_GATE_EVIDENCE_SCHEMA
    ),
    "species_role_visual_gate_policy_relative_path": (
        SPECIES_ROLE_VISUAL_GATE_POLICY_RELATIVE_PATH
    ),
    "species_role_visual_gate_connectivity": 8,
    "species_role_visual_gate_allowlisted_only": True,
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


@dataclass(frozen=True)
class SpeciesRoleVisualGatePolicy:
    """Independently authenticated policy provenance used by sync."""

    source_sha256: str
    identity_frame_sha256: dict[str, str]
    entries: dict[tuple[str, str], dict[str, object]]
    entry_sha256: dict[tuple[str, str], str]
    provenance_record: dict[str, object]


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


def canonical_json_sha256(value: object) -> str:
    try:
        payload = json.dumps(
            value,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
            allow_nan=False,
        ).encode("ascii")
    except (TypeError, ValueError, UnicodeEncodeError) as error:
        raise ValueError(f"value is not canonical ASCII JSON: {error}") from error
    return hashlib.sha256(payload).hexdigest()


def reject_duplicate_json_pairs(
    pairs: list[tuple[str, object]],
) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"visual gate policy duplicates JSON key {key!r}")
        result[key] = value
    return result


def require_exact_keys(
    value: object, expected: set[str], label: str
) -> dict[str, object]:
    record = require_mapping(value, label)
    if set(record) != expected:
        raise ValueError(
            f"{label} must contain exact fields; "
            f"missing={sorted(expected - set(record))} "
            f"unexpected={sorted(set(record) - expected)}"
        )
    return record


def load_species_role_visual_gate_policy(
    project_root: Path,
) -> SpeciesRoleVisualGatePolicy:
    """Authenticate the repository policy independently of build evidence."""

    root = project_root.resolve()
    policy_path = (root / SPECIES_ROLE_VISUAL_GATE_POLICY_RELATIVE_PATH).resolve()
    if root != policy_path and root not in policy_path.parents:
        raise ValueError("visual gate policy path escapes project root")
    try:
        source_bytes = policy_path.read_bytes()
        payload = json.loads(
            source_bytes.decode("utf-8"),
            object_pairs_hook=reject_duplicate_json_pairs,
        )
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read canonical visual gate policy: {error}") from error
    policy = require_exact_keys(
        payload,
        {"canvas", "connectivity", "entries", "identity_frame_sha256", "schema"},
        "visual gate policy",
    )
    if (
        policy.get("schema") != SPECIES_ROLE_VISUAL_GATE_POLICY_SCHEMA
        or policy.get("canvas") != [64, 80]
        or policy.get("connectivity") != 8
    ):
        raise ValueError("canonical visual gate policy header drifted")
    identity_hashes = require_exact_keys(
        policy.get("identity_frame_sha256"),
        {"axolotl", "rabbit"},
        "visual gate policy.identity_frame_sha256",
    )
    if identity_hashes != SPECIES_ROLE_VISUAL_GATE_IDENTITY_FRAME_SHA256:
        raise ValueError("canonical visual gate identity coordinate basis drifted")

    raw_entries = require_list(policy.get("entries"), "visual gate policy.entries")
    if len(raw_entries) != len(SPECIES_ROLE_VISUAL_GATE_KEYS):
        raise ValueError("visual gate policy must contain exactly six entries")
    entries: dict[tuple[str, str], dict[str, object]] = {}
    entry_hashes: dict[tuple[str, str], str] = {}
    for index, expected_key in enumerate(SPECIES_ROLE_VISUAL_GATE_KEYS):
        entry = require_exact_keys(
            raw_entries[index],
            {"gates", "identity_key", "role"},
            f"visual gate policy.entries[{index}]",
        )
        actual_key = (entry.get("identity_key"), entry.get("role"))
        if actual_key != expected_key:
            raise ValueError(
                "visual gate policy entries differ from the exact ordered allow-list"
            )
        gates = require_mapping(
            entry.get("gates"),
            f"visual gate policy.entries[{index}].gates",
        )
        if set(gates) != SPECIES_ROLE_VISUAL_GATE_KIND_SETS[expected_key] or any(
            not isinstance(gate, dict) for gate in gates.values()
        ):
            raise ValueError(
                f"visual gate policy {expected_key[0]}/{expected_key[1]} gate set drifted"
            )
        entry_hash = canonical_json_sha256(entry)
        if entry_hash != SPECIES_ROLE_VISUAL_GATE_ENTRY_SHA256[expected_key]:
            raise ValueError(
                f"visual gate policy {expected_key[0]}/{expected_key[1]} content drifted"
            )
        entries[expected_key] = entry
        entry_hashes[expected_key] = entry_hash

    source_sha256 = hashlib.sha256(source_bytes).hexdigest()
    provenance_record = {
        "configured_species_roles": [
            f"{identity_key}/{role}"
            for identity_key, role in SPECIES_ROLE_VISUAL_GATE_KEYS
        ],
        "identity_frame_sha256": dict(identity_hashes),
        "relative_path": SPECIES_ROLE_VISUAL_GATE_POLICY_RELATIVE_PATH,
        "schema": SPECIES_ROLE_VISUAL_GATE_POLICY_SCHEMA,
        "sha256": source_sha256,
    }
    return SpeciesRoleVisualGatePolicy(
        source_sha256=source_sha256,
        identity_frame_sha256=dict(identity_hashes),
        entries=entries,
        entry_sha256=entry_hashes,
        provenance_record=provenance_record,
    )


def require_fail_closed_manifest(
    manifest: dict[str, object],
    project_root: Path | None = None,
) -> SpeciesRoleVisualGatePolicy | None:
    manifest_schema = manifest.get("schema")
    if manifest_schema not in {
        LEGACY_MANIFEST_SCHEMA,
        MANIFEST_SCHEMA,
        VISUAL_GATE_MANIFEST_SCHEMA,
    }:
        raise ValueError(
            "manifest schema must be one of "
            f"{LEGACY_MANIFEST_SCHEMA}, {MANIFEST_SCHEMA}, or "
            f"{VISUAL_GATE_MANIFEST_SCHEMA}"
        )
    if manifest.get("complete_roster") is not True:
        raise ValueError("manifest must declare complete_roster=true")
    if manifest.get("non_destructive_build") is not True:
        raise ValueError("manifest must declare non_destructive_build=true")
    raster_contract = require_mapping(
        manifest.get("raster_contract"), "raster_contract"
    )
    if raster_contract != EXPECTED_RASTER_CONTRACT:
        raise ValueError(
            "manifest raster_contract is not the fail-closed v6/v7/v8 contract"
        )
    animation_contract = require_mapping(
        manifest.get("animation_contract"), "animation_contract"
    )
    if manifest_schema == LEGACY_MANIFEST_SCHEMA:
        required_animation_values = LEGACY_REQUIRED_ANIMATION_CONTRACT_VALUES
    elif manifest_schema == MANIFEST_SCHEMA:
        required_animation_values = REQUIRED_ANIMATION_CONTRACT_VALUES
    else:
        required_animation_values = VISUAL_GATE_REQUIRED_ANIMATION_CONTRACT_VALUES
    for field, expected in required_animation_values.items():
        if animation_contract.get(field) != expected:
            raise ValueError(
                f"manifest animation_contract.{field} is not fail-closed for "
                f"{manifest_schema}"
            )
    if manifest_schema in {LEGACY_MANIFEST_SCHEMA, MANIFEST_SCHEMA}:
        reserved_animation_fields = (
            set(VISUAL_GATE_REQUIRED_ANIMATION_CONTRACT_VALUES)
            - set(REQUIRED_ANIMATION_CONTRACT_VALUES)
        )
        if "species_role_visual_gate_policy" in manifest or any(
            field in animation_contract for field in reserved_animation_fields
        ):
            raise ValueError(
                f"{manifest_schema} cannot contain reserved v8 visual-gate fields"
            )
    allowed_lock_schemas = {DIRECT_LOCK_SCHEMA, LEGACY_IMAGEGEN_LOCK_SCHEMA}
    if manifest_schema in {MANIFEST_SCHEMA, VISUAL_GATE_MANIFEST_SCHEMA}:
        allowed_lock_schemas.add(IMAGEGEN_LOCK_SCHEMA)
    if manifest.get("identity_lock_schema") not in allowed_lock_schemas:
        raise ValueError("manifest uses an unsupported or legacy identity lock")
    if manifest_schema != VISUAL_GATE_MANIFEST_SCHEMA:
        return None

    policy = load_species_role_visual_gate_policy(
        Path(__file__).resolve().parents[1]
        if project_root is None
        else project_root
    )
    manifest_policy = require_exact_keys(
        manifest.get("species_role_visual_gate_policy"),
        {
            "configured_species_roles",
            "identity_frame_sha256",
            "relative_path",
            "schema",
            "sha256",
        },
        "species_role_visual_gate_policy",
    )
    if manifest_policy != policy.provenance_record:
        raise ValueError(
            "manifest visual gate policy provenance differs from the canonical file"
        )
    return policy


def require_sha256(value: object, label: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
        raise ValueError(f"{label} must be a lowercase SHA-256")
    return value


def require_species_role_visual_gate_evidence(
    manifest_schema: object,
    identity_key: str,
    role_record: dict[str, object],
    identity_lock: dict[str, object],
    policy: SpeciesRoleVisualGatePolicy | None,
) -> None:
    """Verify the v8 handoff without replaying private raster pixels."""

    role = role_record.get("role")
    label = f"{identity_key}.{role}.species_role_visual_gate"
    has_evidence = "species_role_visual_gate" in role_record
    has_evidence_hash = "species_role_visual_gate_sha256" in role_record
    if manifest_schema != VISUAL_GATE_MANIFEST_SCHEMA:
        if has_evidence or has_evidence_hash:
            raise ValueError(
                f"{identity_key}.{role}: v6/v7 cannot contain reserved v8 visual evidence"
            )
        return
    if policy is None:
        raise ValueError("v8 visual gate policy was not authenticated")

    key = (identity_key, role)
    entry = policy.entries.get(key)
    if entry is None:
        if has_evidence or has_evidence_hash:
            raise ValueError(
                f"{identity_key}.{role}: visual evidence is forbidden for an unconfigured role"
            )
        return
    if not has_evidence or not has_evidence_hash:
        raise ValueError(f"{label} and its SHA-256 are required")

    evidence = require_exact_keys(
        role_record.get("species_role_visual_gate"),
        {
            "durations_ms",
            "frame_sha256",
            "gate_results",
            "identity_frame_sha256",
            "identity_key",
            "policy_entry_sha256",
            "policy_relative_path",
            "policy_schema",
            "policy_sha256",
            "role",
            "schema",
            "status",
        },
        label,
    )
    evidence_hash = require_sha256(
        role_record.get("species_role_visual_gate_sha256"),
        f"{label}_sha256",
    )
    if canonical_json_sha256(evidence) != evidence_hash:
        raise ValueError(f"{label} SHA-256 does not match its canonical record")

    identity_frame_hash = require_sha256(
        identity_lock.get("identity_frame_sha256"),
        f"{identity_key}.identity_lock.identity_frame_sha256",
    )
    if (
        evidence.get("schema") != SPECIES_ROLE_VISUAL_GATE_EVIDENCE_SCHEMA
        or evidence.get("status") != "pass"
        or evidence.get("identity_key") != identity_key
        or evidence.get("role") != role
        or evidence.get("policy_schema")
        != SPECIES_ROLE_VISUAL_GATE_POLICY_SCHEMA
        or evidence.get("policy_relative_path")
        != SPECIES_ROLE_VISUAL_GATE_POLICY_RELATIVE_PATH
        or evidence.get("policy_sha256") != policy.source_sha256
        or evidence.get("policy_entry_sha256") != policy.entry_sha256[key]
        or evidence.get("identity_frame_sha256")
        != policy.identity_frame_sha256[identity_key]
        or evidence.get("identity_frame_sha256") != identity_frame_hash
    ):
        raise ValueError(f"{label} provenance or identity binding drifted")

    role_hashes = require_list(
        role_record.get("frame_sha256"),
        f"{identity_key}.{role}.frame_sha256",
    )
    evidence_hashes = require_list(
        evidence.get("frame_sha256"), f"{label}.frame_sha256"
    )
    if len(evidence_hashes) != 4:
        raise ValueError(f"{label}.frame_sha256 must bind exact P0..P3")
    for phase, digest in enumerate(evidence_hashes):
        require_sha256(digest, f"{label}.frame_sha256[{phase}]")
    if evidence_hashes != role_hashes:
        raise ValueError(f"{label} frame hashes differ from the containing role")

    role_durations = require_list(
        role_record.get("durations_ms"), f"{identity_key}.{role}.durations_ms"
    )
    evidence_durations = require_list(
        evidence.get("durations_ms"), f"{label}.durations_ms"
    )
    for durations_label, durations in (
        (f"{identity_key}.{role}.durations_ms", role_durations),
        (f"{label}.durations_ms", evidence_durations),
    ):
        if len(durations) != 4 or any(
            type(value) is not int or value <= 0 for value in durations
        ):
            raise ValueError(f"{durations_label} must contain four positive integers")
    if evidence_durations != role_durations:
        raise ValueError(f"{label} cadence differs from the containing role")
    gates = require_mapping(entry.get("gates"), f"{label}.policy_entry.gates")
    cadence = gates.get("cadence")
    if cadence is not None:
        required_durations = require_mapping(
            cadence, f"{label}.policy_entry.gates.cadence"
        ).get("required_durations_ms")
        if evidence_durations != required_durations:
            raise ValueError(f"{label} cadence differs from the canonical RoleSpec")

    gate_results = require_mapping(
        evidence.get("gate_results"), f"{label}.gate_results"
    )
    if set(gate_results) != set(gates):
        raise ValueError(f"{label} gate result set differs from the policy entry")
    for kind in gates:
        result = require_exact_keys(
            gate_results.get(kind),
            {"measurements", "possible_failure_reason_codes", "status"},
            f"{label}.gate_results.{kind}",
        )
        if (
            result.get("status") != "pass"
            or result.get("possible_failure_reason_codes")
            != SPECIES_ROLE_VISUAL_GATE_REASON_CODES[(identity_key, role, kind)]
        ):
            raise ValueError(f"{label}.{kind} is not a canonical passing result")
        require_mapping(result.get("measurements"), f"{label}.{kind}.measurements")


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
        } or lock_schema not in {
            LEGACY_IMAGEGEN_LOCK_SCHEMA,
            IMAGEGEN_LOCK_SCHEMA,
        }:
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
        expected_semantic_schema = (
            LEGACY_GENERATED_ACTION_SEMANTIC_SCHEMA
            if lock_schema == LEGACY_IMAGEGEN_LOCK_SCHEMA
            else GENERATED_ACTION_SEMANTIC_SCHEMA
        )
        if (
            set(semantic_contract) != {"roles", "schema"}
            or semantic_contract.get("schema") != expected_semantic_schema
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
    """Verify v4/v5 bounded-composite evidence consumed by portrait sync."""

    role = role_record.get("role")
    if not isinstance(role, str):
        raise ValueError(f"{identity_key} generated role is invalid")
    label = f"{identity_key}.{role}.semantic_locality"
    semantic = require_mapping(role_record.get("semantic_locality"), label)
    lock_schema = identity_lock.get("schema")
    semantic_schema = (
        LEGACY_GENERATED_ACTION_SEMANTIC_SCHEMA
        if lock_schema == LEGACY_IMAGEGEN_LOCK_SCHEMA
        else GENERATED_ACTION_SEMANTIC_SCHEMA
    )
    expected_baseline = (
        "identity-anchored"
        if role in IDENTITY_ANCHORED_ROLES
        else "immutable-role-phase-0"
    )
    if (
        semantic.get("schema") != semantic_schema
        or semantic.get("role") != role
        or semantic.get("source_layout") != "independent-frame"
        or semantic.get("baseline_policy") != expected_baseline
    ):
        raise ValueError(f"{label} lacks exact v4/v5 role lineage")

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
    native_grid_records: list[dict[str, object]] = []

    for phase_index, raw_phase in enumerate(phases):
        phase = require_mapping(raw_phase, f"{label}.phases[{phase_index}]")
        if phase.get("phase") != phase_index:
            raise ValueError(f"{label} phases are missing or out of order")
        mode_present = "generation_reference_mode" in phase
        native_grid_present = "native_grid_reference" in phase
        if lock_schema == LEGACY_IMAGEGEN_LOCK_SCHEMA:
            expected_mode = (
                P0_GENERATION_REFERENCE_MODE
                if phase_index == 0
                else TWO_REFERENCE_GENERATION_MODE
            )
            if mode_present or native_grid_present:
                raise ValueError(
                    f"{label}.phases[{phase_index}] must retain the exact audited "
                    "semantic-v3 evidence shape without v5-only reference fields"
                )
            generation_reference_mode = expected_mode
            native_grid = None
        else:
            if not mode_present or not native_grid_present:
                raise ValueError(
                    f"{label}.phases[{phase_index}] lacks explicit v5 reference "
                    "provenance"
                )
            generation_reference_mode = phase.get("generation_reference_mode")
            native_grid = phase.get("native_grid_reference")
            allowed_modes = (
                {P0_GENERATION_REFERENCE_MODE}
                if phase_index == 0
                else {
                    TWO_REFERENCE_GENERATION_MODE,
                    THREE_REFERENCE_GENERATION_MODE,
                }
            )
            if generation_reference_mode not in allowed_modes:
                raise ValueError(
                    f"{label}.phases[{phase_index}] reference mode is invalid"
                )
            if (
                generation_reference_mode == THREE_REFERENCE_GENERATION_MODE
            ) != (native_grid is not None):
                raise ValueError(
                    f"{label}.phases[{phase_index}] Image 3 presence contradicts "
                    "its reference mode"
                )
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
        if native_grid is not None:
            grid = require_mapping(
                native_grid,
                f"{label}.phases[{phase_index}].native_grid_reference",
            )
            if set(grid) != {
                "derivation",
                "edit_target",
                "grid_png_sha256",
                "grid_relative_path",
                "image_number",
                "kind",
                "p0_packed_sha256",
                "read_only",
                "role_registration_sha256",
                "roundtrip_packed_sha256",
                "schema",
                "source_png_sha256",
                "source_relative_path",
                "transform_sha256",
            }:
                raise ValueError(
                    f"{label}.phases[{phase_index}] Image 3 record is malformed"
                )
            grid_hash = require_sha256(
                grid.get("grid_png_sha256"),
                f"{label}.phases[{phase_index}].grid_png_sha256",
            )
            if (
                grid.get("schema") != NATIVE_GRID_REFERENCE_SCHEMA
                or grid.get("kind") != NATIVE_GRID_REFERENCE_KIND
                or grid.get("image_number") != 3
                or grid.get("read_only") is not True
                or grid.get("edit_target") is not False
                or grid.get("derivation") != NATIVE_GRID_REFERENCE_DERIVATION
                or grid.get("source_relative_path") != f"{role}/00.png"
                or grid.get("source_png_sha256") != p0_source_hash
                or grid.get("grid_relative_path")
                != f"native-grid-reference/{role}/00.png"
                or grid.get("p0_packed_sha256") != p0_final_hash
                or grid.get("roundtrip_packed_sha256") != p0_final_hash
                or grid.get("transform_sha256")
                != identity_lock.get("transform_sha256")
                or grid.get("role_registration_sha256")
                != semantic.get("role_registration_sha256")
                or source_hashes.get(str(grid.get("grid_relative_path")))
                != grid_hash
            ):
                raise ValueError(
                    f"{label}.phases[{phase_index}] Image 3 does not bind the "
                    "same immutable exact P0"
                )
            native_grid_records.append(grid)

    if native_grid_records:
        canonical_records = {
            json.dumps(record, sort_keys=True, separators=(",", ":"))
            for record in native_grid_records
        }
        if len(canonical_records) != 1:
            raise ValueError(
                f"{label} three-reference phases do not reuse one exact Image 3"
            )
        if (
            expected_baseline != "identity-anchored"
            or registration.get("derivation")
            != "identity-anchored-zero-offset"
            or registration.get("output_offset") != [0, 0]
            or p0.get("generated_asset_layout")
            != GENERATED_IDENTITY_BASELINE_ASSET_LAYOUT
            or p0_source_hash != identity_source_hash
            or p0.get("imported_candidate_frame_sha256") != identity_frame_hash
            or p0_registered_hash != identity_frame_hash
            or p0_final_hash != identity_frame_hash
        ):
            raise ValueError(
                f"{label} Image 3 v1 is not based on a zero-registration exact-copy P0"
            )
        grid_transform = require_imagegen_transform(
            identity_lock.get("transform"), identity_key
        )
        if (
            grid_transform.get("source_canvas") != [1122, 1402]
            or grid_transform.get("crop_rect") != [1, 1, 1121, 1401]
            or grid_transform.get("output_canvas") != [64, 80]
        ):
            raise ValueError(
                f"{label} Image 3 v1 requires the exact 1122x1402 source, "
                "1120x1400 centered crop, and 64x80 output transform"
            )
        proof = require_mapping(
            semantic.get("native_grid_reference_proof"),
            f"{label}.native_grid_reference_proof",
        )
        reference = native_grid_records[0]
        if (
            proof.get("derivation") != NATIVE_GRID_REFERENCE_DERIVATION
            or proof.get("grid_relative_path")
            != reference.get("grid_relative_path")
            or proof.get("grid_png_sha256") != reference.get("grid_png_sha256")
            or proof.get("p0_packed_sha256") != p0_final_hash
            or proof.get("canonical_roundtrip_packed_sha256") != p0_final_hash
            or proof.get("canonical_independent_xor_pixels") != 0
            or proof.get("independent_roundtrip_packed_sha256") != p0_final_hash
            or proof.get("independent_threshold_sensitive_pixels_plus_minus_20")
            != 0
            or proof.get("transform_sha256")
            != identity_lock.get("transform_sha256")
        ):
            raise ValueError(f"{label} Image 3 BOX proof drifted")
    elif "native_grid_reference_proof" in semantic:
        raise ValueError(f"{label} contains orphan Image 3 proof evidence")


def load_records(
    manifest_path: Path,
    *,
    project_root: Path | None = None,
) -> dict[str, PortraitRecord]:
    try:
        manifest = require_mapping(
            json.loads(manifest_path.read_text(encoding="utf-8")), "manifest"
        )
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read private manifest: {error}") from error

    visual_gate_policy = require_fail_closed_manifest(
        manifest,
        project_root=project_root,
    )

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
        expected_role_semantic_schema = (
            LEGACY_GENERATED_ACTION_SEMANTIC_SCHEMA
            if pack_identity_lock.get("schema") == LEGACY_IMAGEGEN_LOCK_SCHEMA
            else GENERATED_ACTION_SEMANTIC_SCHEMA
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
                    != expected_role_semantic_schema
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
            require_species_role_visual_gate_evidence(
                manifest.get("schema"),
                identity_key,
                role_record,
                pack_identity_lock,
                visual_gate_policy,
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

    records = load_records(manifest_path, project_root=project_root)
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
