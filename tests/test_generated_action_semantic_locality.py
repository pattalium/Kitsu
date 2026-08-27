#!/usr/bin/env python3
"""Hostile tests for generated-action semantic locality and provenance."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import build_default_packs as base  # noqa: E402
import companion_raster_contract as contract  # noqa: E402


IDLE = base.ROLE_SPECS[0]
PET = base.ROLE_SPECS[2]
SLEEP = base.ROLE_SPECS[3]
SURPRISE = base.ROLE_SPECS[5]


def digest(label: str) -> str:
    return hashlib.sha256(label.encode("ascii")).hexdigest()


def red_panda_identity_mask() -> set[tuple[int, int]]:
    """Compact native identity with head, torso, tail, and two planted paws."""

    mask = {
        (x, y)
        for y in range(24, 71)
        for x in range(18, 46)
        if y >= 40 or 20 <= x <= 43
    }
    mask.update(
        (x, y)
        for y in range(52, 61)
        for x in range(44, 55)
    )
    mask.update(
        (x, y)
        for left, right in ((20, 25), (38, 43))
        for y in range(71, 78)
        for x in range(left, right + 1)
    )
    return mask


def localized_belly_edits() -> list[set[tuple[int, int]]]:
    return [
        {
            (27 + phase * 3 + dx, 55 + dy)
            for dx in range(2)
            for dy in range(2)
        }
        for phase in range(4)
    ]


def metrics(mask: set[tuple[int, int]]) -> contract.SourceMetrics:
    components = base.connected_components(mask)
    return contract.SourceMetrics(
        bounds=base.bounds(mask),
        ink_pixels=len(mask),
        components=len(components),
        smallest_component_pixels=min(map(len, components)),
        primary_fraction=max(map(len, components)) / len(mask),
    )


def make_frame(
    mask: set[tuple[int, int]], phase: int, *, label: str
) -> contract.HighResFrame:
    packed = contract.high_res_frame_bytes(mask)
    return contract.HighResFrame(
        path=Path("red_panda") / "idle" / f"{phase:02d}.png",
        role="idle",
        phase=phase,
        source_sha256=digest(f"{label}-source-{phase}"),
        mask=frozenset(mask),
        metrics=metrics(mask),
        apparent_scale_ratio=1.0,
        identity_jaccard=0.99,
        packed=packed,
    )


def make_identity() -> contract.HighResFrame:
    mask = red_panda_identity_mask()
    packed = contract.high_res_frame_bytes(mask)
    return contract.HighResFrame(
        path=Path("red_panda") / "identity.png",
        role="identity",
        phase=-1,
        source_sha256=digest("red-panda-identity-source"),
        mask=frozenset(mask),
        metrics=metrics(mask),
        apparent_scale_ratio=1.0,
        identity_jaccard=1.0,
        packed=packed,
    )


def make_frames(
    edits: list[set[tuple[int, int]]], *, label: str = "localized"
) -> list[contract.HighResFrame]:
    identity = red_panda_identity_mask()
    return [
        make_frame(identity - phase_edits, phase, label=label)
        for phase, phase_edits in enumerate(edits)
    ]


def frame_sha256(frame: contract.HighResFrame) -> str:
    return hashlib.sha256(frame.packed).hexdigest()


def identity_edit_target(
    species: str, identity: contract.HighResFrame
) -> contract.ImmutableEditTargetReference:
    packed_sha256 = frame_sha256(identity)
    return contract.ImmutableEditTargetReference(
        kind="immutable-approved-identity-source",
        relative_path="identity.png",
        identity_key=species,
        role="identity",
        phase=-1,
        source_sha256=identity.source_sha256,
        registered_frame_sha256=packed_sha256,
        accepted_composited_frame_sha256=packed_sha256,
    )


def phase_preauthorization(
    role: str,
    phase: int,
    allowed: contract.NativeRegionMaskLock,
    edit_target_kind: str,
) -> contract.FrozenPhasePreauthorizationReference:
    return contract.FrozenPhasePreauthorizationReference(
        kind="immutable-pre-generation-phase-mask",
        relative_path=f"preauthorization/{role}/{phase:02d}.json",
        source_sha256=digest(f"{role}-{phase}-preauthorization"),
        storyboard_sha256=digest(f"{role}-frozen-storyboard"),
        allowed_change_region_sha256=allowed.packed_sha256,
        edit_target_kind=edit_target_kind,
    )


def make_semantic_role(
    identity: contract.HighResFrame,
    frames: list[contract.HighResFrame],
    *,
    allowed_regions: list[set[tuple[int, int]]] | None = None,
    motion_region: set[tuple[int, int]] | None = None,
    imported_candidates: list[contract.HighResFrame] | None = None,
) -> contract.GeneratedRoleSemanticLock:
    identity_mask = set(identity.mask)
    deltas = [identity_mask ^ set(frame.mask) for frame in frames]
    allowed = deltas if allowed_regions is None else allowed_regions
    if motion_region is None:
        motion_region = set().union(*deltas)
    candidates = frames if imported_candidates is None else imported_candidates
    identity_frame_sha256 = hashlib.sha256(identity.packed).hexdigest()
    reference = contract.ImmutableIdentityReference(
        kind="immutable-approved-identity-source",
        relative_path="identity.png",
        identity_key="red_panda",
        source_sha256=identity.source_sha256,
        frame_sha256=identity_frame_sha256,
    )
    planted = contract.native_region_mask_lock(
        {(x, contract.HIGH_RES_FLOOR_Y) for x in range(contract.HIGH_RES_FRAME_WIDTH)}
    )
    protected_head = contract.native_region_mask_lock(
        {(x, y) for y in range(28, 36) for x in range(24, 40)}
    )
    target = identity_edit_target("red_panda", identity)
    phases: list[contract.GeneratedPhaseSemanticLock] = []
    for phase, (frame, candidate) in enumerate(
        zip(frames, candidates, strict=True)
    ):
        allowed_lock = contract.native_region_mask_lock(allowed[phase])
        phases.append(
            contract.GeneratedPhaseSemanticLock(
                phase=phase,
                semantic_baseline="approved-identity",
                identity_reference=reference,
                edit_target_reference=target,
                preauthorization_reference=phase_preauthorization(
                    "idle", phase, allowed_lock, target.kind
                ),
                generated_asset=contract.GeneratedPhaseAsset(
                    layout="independent-frame",
                    relative_path=f"idle/{phase:02d}.png",
                    source_sha256=candidate.source_sha256,
                    imported_candidate_frame_sha256=frame_sha256(candidate),
                    registered_candidate_frame_sha256=frame_sha256(candidate),
                ),
                allowed_change_region=allowed_lock,
                composition_mode=contract.GENERATED_COMPOSITION_MODE,
                composition_baseline_frame_sha256=identity_frame_sha256,
                composited_frame_sha256=frame_sha256(frame),
                maximum_out_of_region_changed_pixels=0,
                frozen_regions=(
                    contract.FrozenSemanticRegion(
                        kind="planted-contact",
                        name="both-planted-paws",
                        region=planted,
                        maximum_changed_pixels=0,
                    ),
                    contract.FrozenSemanticRegion(
                        kind="protected-identity-landmark",
                        name="face-and-ear-landmark",
                        region=protected_head,
                        maximum_changed_pixels=0,
                    ),
                ),
            )
        )
    registration = contract.GeneratedRoleRegistrationLock(
        schema=contract.GENERATED_ROLE_REGISTRATION_SCHEMA,
        derivation="identity-anchored-zero-offset",
        output_offset=(0, 0),
        p0_unregistered_floor_y=contract.HIGH_RES_FLOOR_Y,
    )
    return contract.GeneratedRoleSemanticLock(
        role="idle",
        baseline_policy="identity-anchored",
        contact_policy="planted-identity",
        role_registration=registration,
        role_registration_sha256=(
            contract.generated_role_registration_sha256(registration)
        ),
        role_pose_baseline_frame_sha256=hashlib.sha256(
            frames[0].packed
        ).hexdigest(),
        maximum_role_pose_component_count_delta=2,
        maximum_contact_changed_pixels_per_phase=0,
        role_pose_identity_landmarks=(
            contract.RolePoseIdentityLandmarkLock(
                name="face-marking",
                identity_region=protected_head,
                role_pose_region=protected_head,
                minimum_ink_pixels=4,
                minimum_ink_retention_per_mille=800,
                maximum_component_count_delta=1,
            ),
        ),
        phases=tuple(phases),
        motion_landmarks=(
            contract.MotionLandmarkLock(
                name="localized-belly-breath",
                region=contract.native_region_mask_lock(motion_region),
                minimum_changed_pixels=4,
            ),
        ),
    )


def validate(
    identity: contract.HighResFrame,
    frames: list[contract.HighResFrame],
    semantic: contract.GeneratedRoleSemanticLock,
    *,
    imported_candidates: list[contract.HighResFrame] | None = None,
) -> dict[str, object]:
    candidates = frames if imported_candidates is None else imported_candidates
    return contract.validate_generated_action_semantic_role(
        "red_panda",
        identity,
        IDLE,
        frames,
        semantic,
        imported_candidates=candidates,
        registered_candidates=candidates,
        identity_source_sha256=identity.source_sha256,
        identity_frame_sha256=hashlib.sha256(identity.packed).hexdigest(),
        source_layout="independent-frame",
    )


def import_lock_record(
    semantic: contract.GeneratedRoleSemanticLock,
    identity: contract.HighResFrame,
) -> dict[str, object]:
    semantic_contract = contract.GeneratedActionSemanticContract(
        schema=contract.GENERATED_ACTION_SEMANTIC_SCHEMA,
        roles=(semantic,),
    )
    transform = contract.recommended_imagegen_import_transform(
        action_output_offset=(-1, 30)
    )
    return {
        "action_semantic_contract": (
            contract.generated_action_semantic_contract_record(semantic_contract)
        ),
        "action_semantic_contract_sha256": (
            contract.generated_action_semantic_contract_sha256(semantic_contract)
        ),
        "approved": True,
        "identity_frame_sha256": hashlib.sha256(identity.packed).hexdigest(),
        "identity_key": "red_panda",
        "identity_source_sha256": identity.source_sha256,
        "transform": contract.imagegen_import_transform_record(transform),
        "transform_sha256": contract.imagegen_import_transform_sha256(transform),
    }


def make_role_frame(
    species: str,
    role: base.RoleSpec,
    identity: contract.HighResFrame,
    mask: set[tuple[int, int]],
    phase: int,
    *,
    label: str,
) -> contract.HighResFrame:
    return contract.HighResFrame(
        path=Path(species) / role.name / f"{phase:02d}.png",
        role=role.name,
        phase=phase,
        source_sha256=digest(f"{species}-{role.name}-{label}-{phase}"),
        mask=frozenset(mask),
        metrics=metrics(mask),
        apparent_scale_ratio=(len(mask) / len(identity.mask)) ** 0.5,
        identity_jaccard=contract.jaccard(set(identity.mask), mask),
        packed=contract.high_res_frame_bytes(mask),
    )


def make_role_base_semantic(
    species: str,
    role: base.RoleSpec,
    identity: contract.HighResFrame,
    frames: list[contract.HighResFrame],
    *,
    contact_policy: str,
    maximum_contact_changes: int,
    motion_region: set[tuple[int, int]],
    protected_region: set[tuple[int, int]],
    frozen_contact_regions: list[set[tuple[int, int]]],
    identity_pose_landmark: set[tuple[int, int]],
    role_pose_landmark: set[tuple[int, int]],
    imported_candidates: list[contract.HighResFrame] | None = None,
    registration: contract.GeneratedRoleRegistrationLock | None = None,
) -> contract.GeneratedRoleSemanticLock:
    identity_mask = set(identity.mask)
    role_pose = set(frames[0].mask)
    identity_frame_sha256 = hashlib.sha256(identity.packed).hexdigest()
    reference = contract.ImmutableIdentityReference(
        kind="immutable-approved-identity-source",
        relative_path="identity.png",
        identity_key=species,
        source_sha256=identity.source_sha256,
        frame_sha256=identity_frame_sha256,
    )
    candidates = frames if imported_candidates is None else imported_candidates
    if registration is None:
        registration = contract.GeneratedRoleRegistrationLock(
            schema=contract.GENERATED_ROLE_REGISTRATION_SCHEMA,
            derivation="role-p0-fixed-dx-explicit-dy-floor-derived",
            output_offset=(0, 0),
            p0_unregistered_floor_y=contract.HIGH_RES_FLOOR_Y,
        )
    registered_candidates = [
        contract.register_generated_candidate(
            candidate,
            registration,
            label=f"{species}/{role.name}/{phase}/test-registration",
        )
        for phase, candidate in enumerate(candidates)
    ]
    frozen_anatomy = contract.native_region_mask_lock(protected_region)
    phases: list[contract.GeneratedPhaseSemanticLock] = []
    for phase, (frame, candidate, registered_candidate) in enumerate(
        zip(frames, candidates, registered_candidates, strict=True)
    ):
        baseline = identity_mask if phase == 0 else role_pose
        delta = baseline ^ set(frame.mask)
        allowed = delta or motion_region
        allowed_lock = contract.native_region_mask_lock(allowed)
        if phase == 0:
            edit_target = identity_edit_target(species, identity)
            composition_baseline_hash = identity_frame_sha256
        else:
            edit_target = contract.ImmutableEditTargetReference(
                kind="immutable-accepted-role-phase-0",
                relative_path=f"{role.name}/00.png",
                identity_key=species,
                role=role.name,
                phase=0,
                source_sha256=candidates[0].source_sha256,
                registered_frame_sha256=frame_sha256(
                    registered_candidates[0]
                ),
                accepted_composited_frame_sha256=frame_sha256(frames[0]),
            )
            composition_baseline_hash = frame_sha256(frames[0])
        phases.append(
            contract.GeneratedPhaseSemanticLock(
                phase=phase,
                semantic_baseline=(
                    "approved-identity-pose-gate"
                    if phase == 0
                    else "immutable-role-phase-0"
                ),
                identity_reference=reference,
                edit_target_reference=edit_target,
                preauthorization_reference=phase_preauthorization(
                    role.name, phase, allowed_lock, edit_target.kind
                ),
                generated_asset=contract.GeneratedPhaseAsset(
                    layout="independent-frame",
                    relative_path=f"{role.name}/{phase:02d}.png",
                    source_sha256=candidate.source_sha256,
                    imported_candidate_frame_sha256=frame_sha256(candidate),
                    registered_candidate_frame_sha256=frame_sha256(
                        registered_candidate
                    ),
                ),
                allowed_change_region=allowed_lock,
                composition_mode=contract.GENERATED_COMPOSITION_MODE,
                composition_baseline_frame_sha256=composition_baseline_hash,
                composited_frame_sha256=frame_sha256(frame),
                maximum_out_of_region_changed_pixels=0,
                frozen_regions=(
                    contract.FrozenSemanticRegion(
                        kind="planted-contact",
                        name="stable-role-contact",
                        region=contract.native_region_mask_lock(
                            frozen_contact_regions[phase]
                        ),
                        maximum_changed_pixels=0,
                    ),
                    contract.FrozenSemanticRegion(
                        kind="protected-identity-landmark",
                        name="stable-role-anatomy",
                        region=frozen_anatomy,
                        maximum_changed_pixels=0,
                    ),
                ),
            )
        )
    return contract.GeneratedRoleSemanticLock(
        role=role.name,
        baseline_policy="immutable-role-phase-0",
        contact_policy=contact_policy,
        role_registration=registration,
        role_registration_sha256=(
            contract.generated_role_registration_sha256(registration)
        ),
        role_pose_baseline_frame_sha256=hashlib.sha256(frames[0].packed).hexdigest(),
        maximum_role_pose_component_count_delta=2,
        maximum_contact_changed_pixels_per_phase=maximum_contact_changes,
        role_pose_identity_landmarks=(
            contract.RolePoseIdentityLandmarkLock(
                name="species-face-marking",
                identity_region=contract.native_region_mask_lock(
                    identity_pose_landmark
                ),
                role_pose_region=contract.native_region_mask_lock(
                    role_pose_landmark
                ),
                minimum_ink_pixels=4,
                minimum_ink_retention_per_mille=700,
                maximum_component_count_delta=1,
            ),
        ),
        phases=tuple(phases),
        motion_landmarks=(
            contract.MotionLandmarkLock(
                name="storyboard-motion",
                region=contract.native_region_mask_lock(motion_region),
                minimum_changed_pixels=4,
            ),
        ),
    )


def validate_role_base(
    species: str,
    identity: contract.HighResFrame,
    role: base.RoleSpec,
    frames: list[contract.HighResFrame],
    semantic: contract.GeneratedRoleSemanticLock,
    *,
    imported_candidates: list[contract.HighResFrame] | None = None,
    registered_candidates: list[contract.HighResFrame] | None = None,
) -> dict[str, object]:
    imported = frames if imported_candidates is None else imported_candidates
    registered = frames if registered_candidates is None else registered_candidates
    return contract.validate_generated_action_semantic_role(
        species,
        identity,
        role,
        frames,
        semantic,
        imported_candidates=imported,
        registered_candidates=registered,
        identity_source_sha256=identity.source_sha256,
        identity_frame_sha256=hashlib.sha256(identity.packed).hexdigest(),
        source_layout="independent-frame",
    )


def simple_role_base_fixture(
    role: base.RoleSpec = SLEEP,
    *,
    imported_candidates: list[contract.HighResFrame] | None = None,
    registration: contract.GeneratedRoleRegistrationLock | None = None,
) -> tuple[
    contract.HighResFrame,
    list[contract.HighResFrame],
    contract.GeneratedRoleSemanticLock,
]:
    identity = make_identity()
    edits = localized_belly_edits()
    frames = [
        make_role_frame(
            "red_panda",
            role,
            identity,
            set(identity.mask) - phase_edits,
            phase,
            label="simple-role-base",
        )
        for phase, phase_edits in enumerate(edits)
    ]
    protected = {(x, y) for y in range(28, 36) for x in range(24, 40)}
    semantic = make_role_base_semantic(
        "red_panda",
        role,
        identity,
        frames,
        contact_policy="planted-role-base",
        maximum_contact_changes=0,
        motion_region=set().union(*edits),
        protected_region=protected,
        frozen_contact_regions=[
            {
                (x, contract.HIGH_RES_FLOOR_Y)
                for x in range(contract.HIGH_RES_FRAME_WIDTH)
            }
            for _phase in range(4)
        ],
        identity_pose_landmark=protected,
        role_pose_landmark=protected,
        imported_candidates=imported_candidates,
        registration=registration,
    )
    return identity, frames, semantic


def identity_baseline_role_fixture() -> tuple[
    contract.HighResFrame,
    list[contract.HighResFrame],
    contract.GeneratedRoleSemanticLock,
]:
    """Role-base fixture whose no-call P0 is an exact identity source copy."""

    identity = make_identity()
    edits = localized_belly_edits()
    masks = [
        set(identity.mask),
        set(identity.mask) - edits[0],
        set(identity.mask) - edits[1],
        set(identity.mask) - edits[2],
    ]
    frames = [
        make_role_frame(
            "red_panda",
            PET,
            identity,
            mask,
            phase,
            label="identity-baseline-role",
        )
        for phase, mask in enumerate(masks)
    ]
    frames[0] = replace(
        frames[0], source_sha256=identity.source_sha256
    )
    protected = {(x, y) for y in range(28, 36) for x in range(24, 40)}
    semantic = make_role_base_semantic(
        "red_panda",
        PET,
        identity,
        frames,
        contact_policy="planted-role-base",
        maximum_contact_changes=0,
        motion_region=set().union(*edits),
        protected_region=protected,
        frozen_contact_regions=[
            {
                (x, contract.HIGH_RES_FLOOR_Y)
                for x in range(contract.HIGH_RES_FRAME_WIDTH)
            }
            for _phase in range(4)
        ],
        identity_pose_landmark=protected,
        role_pose_landmark=protected,
    )
    phases = list(semantic.phases)
    phases[0] = replace(
        phases[0],
        generated_asset=replace(
            phases[0].generated_asset,
            layout=contract.GENERATED_IDENTITY_BASELINE_ASSET_LAYOUT,
        ),
    )
    return identity, frames, replace(semantic, phases=tuple(phases))


def write_rectangle_migration_preauthorization(
    species_dir: Path,
    role: str,
    phase_lock: contract.GeneratedPhaseSemanticLock,
    *,
    x_bounds: tuple[int, int] = (2, 61),
    y_bounds: tuple[int, int] = (2, 77),
) -> tuple[contract.GeneratedPhaseSemanticLock, dict[str, str], Path]:
    """Write a hash-pinned legacy freeze plus its deterministic v4 wrapper."""

    rectangle = contract.native_region_mask_lock(
        {
            (x, y)
            for y in range(y_bounds[0], y_bounds[1] + 1)
            for x in range(x_bounds[0], x_bounds[1] + 1)
        }
    )
    freeze_schema = "kitsu-red-panda-role-p0-pre-generation-freeze-v1"
    freeze_record = {
        "schema": freeze_schema,
        "status": "frozen-before-p0-generation",
        "identity_source_sha256": (
            phase_lock.identity_reference.source_sha256
        ),
        "storyboard_sha256": (
            phase_lock.preauthorization_reference.storyboard_sha256
        ),
        "fixed_transform": {
            "output_canvas": [64, 80],
        },
        "method_note": "P1-P3 use one accepted immutable role P0 star base.",
        "roles": [
            {
                "role": role,
                "phase": 0,
                "prompt_path": f"prompts/{role}/00.txt",
                "prompt_sha256": digest(f"{role}-p0-prompt"),
                "imagegen_edit_target": "immutable-approved-identity",
                "preauthorized_role_pose_region": {
                    "canvas": [64, 80],
                    "safe_x_inclusive": list(x_bounds),
                    "safe_y_inclusive": list(y_bounds),
                    "guard_rows_78_79": "forbidden",
                },
                "generated_source_count_at_freeze": 0,
            }
        ],
    }
    freeze_bytes = (
        json.dumps(freeze_record, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")
    freeze_hash = hashlib.sha256(freeze_bytes).hexdigest()
    freeze_relative = (
        f"preauthorization/_frozen-source/{freeze_hash}.json"
    )
    freeze_path = species_dir / freeze_relative
    freeze_path.parent.mkdir(parents=True, exist_ok=True)
    freeze_path.write_bytes(freeze_bytes)

    preauthorization_record = {
        "allowed_change_region": contract.native_region_mask_record(rectangle),
        "edit_target_kind": phase_lock.edit_target_reference.kind,
        "frozen_before_generation": True,
        "identity_key": "red_panda",
        "mask_authoring_basis": (
            contract.GENERATED_PREAUTHORIZATION_RECTANGLE_MIGRATION_BASIS
        ),
        "phase": phase_lock.phase,
        "rectangle_migration": {
            "freeze_record_relative_path": freeze_relative,
            "freeze_record_schema": freeze_schema,
            "freeze_record_sha256": freeze_hash,
            "freeze_region_field": (
                contract.GENERATED_PREAUTHORIZATION_FREEZE_REGION_FIELD
            ),
            "freeze_role": role,
            "freeze_phase": 0,
        },
        "role": role,
        "schema": contract.GENERATED_PHASE_PREAUTHORIZATION_SCHEMA,
        "storyboard_sha256": (
            phase_lock.preauthorization_reference.storyboard_sha256
        ),
    }
    preauthorization_path = (
        species_dir / phase_lock.preauthorization_reference.relative_path
    )
    preauthorization_path.parent.mkdir(parents=True, exist_ok=True)
    preauthorization_path.write_text(
        json.dumps(preauthorization_record, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    updated = replace(
        phase_lock,
        allowed_change_region=rectangle,
        preauthorization_reference=replace(
            phase_lock.preauthorization_reference,
            source_sha256=contract.sha256_file(preauthorization_path),
            allowed_change_region_sha256=rectangle.packed_sha256,
        ),
    )
    return (
        updated,
        {
            phase_lock.preauthorization_reference.relative_path: (
                contract.sha256_file(preauthorization_path)
            ),
            freeze_relative: freeze_hash,
        },
        freeze_path,
    )


def rewrite_rectangle_migration_as_addendum_chain(
    species_dir: Path,
    role: str,
    phase_lock: contract.GeneratedPhaseSemanticLock,
    base_freeze_path: Path,
) -> tuple[contract.GeneratedPhaseSemanticLock, dict[str, str], Path]:
    """Replace a direct wrapper with the exact chained-addendum provenance."""

    base_hash = contract.sha256_file(base_freeze_path)
    base_schema = json.loads(
        base_freeze_path.read_text(encoding="utf-8")
    )["schema"]
    addendum_schema = (
        "kitsu-red-panda-role-p0-pre-generation-freeze-addendum-v1"
    )
    addendum = {
        "schema": addendum_schema,
        "status": "frozen-before-listed-p0-generation",
        "base_freeze_sha256": base_hash,
        "identity_source_sha256": (
            phase_lock.identity_reference.source_sha256
        ),
        "storyboard_sha256": (
            phase_lock.preauthorization_reference.storyboard_sha256
        ),
        "fixed_identity_transform": {"output_canvas": [64, 80]},
        "bounded_role_registration_policy_pending_v4": {
            "maximum_delta_from_identity_offset_px": 4,
            "one_role_offset_reused_for_all_four_phases": True,
            "phase_specific_override": False,
            "scale_crop_threshold_change": False,
            "dy_must_be_floor_derived": True,
            "dx_must_pass_root_axis_and_safe_stage_gates": True,
        },
        "roles": [
            {
                "role": role,
                "prompt_path": f"prompts/{role}/00.txt",
                "prompt_sha256": digest(f"{role}-addendum-prompt"),
                "raw_source_count_for_role_at_freeze": 0,
            }
        ],
        "imagegen_edit_target": "immutable-approved-identity",
        "later_phase_method": "P1/P2/P3 independently edit immutable P0.",
    }
    addendum_bytes = (
        json.dumps(addendum, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")
    addendum_hash = hashlib.sha256(addendum_bytes).hexdigest()
    addendum_relative = (
        f"preauthorization/_frozen-source/{addendum_hash}.json"
    )
    addendum_path = species_dir / addendum_relative
    addendum_path.write_bytes(addendum_bytes)

    preauthorization_path = (
        species_dir / phase_lock.preauthorization_reference.relative_path
    )
    preauthorization = json.loads(
        preauthorization_path.read_text(encoding="utf-8")
    )
    preauthorization["rectangle_migration"] = {
        "base_freeze_record_relative_path": (
            f"preauthorization/_frozen-source/{base_hash}.json"
        ),
        "base_freeze_record_schema": base_schema,
        "base_freeze_record_sha256": base_hash,
        "freeze_record_relative_path": addendum_relative,
        "freeze_record_schema": addendum_schema,
        "freeze_record_sha256": addendum_hash,
        "freeze_region_field": (
            contract.GENERATED_PREAUTHORIZATION_ADDENDUM_FREEZE_REGION_FIELD
        ),
        "freeze_role": role,
        "freeze_phase": 0,
    }
    preauthorization_path.write_text(
        json.dumps(preauthorization, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    updated = replace(
        phase_lock,
        preauthorization_reference=replace(
            phase_lock.preauthorization_reference,
            source_sha256=contract.sha256_file(preauthorization_path),
        ),
    )
    return (
        updated,
        {
            phase_lock.preauthorization_reference.relative_path: (
                contract.sha256_file(preauthorization_path)
            ),
            f"preauthorization/_frozen-source/{base_hash}.json": base_hash,
            addendum_relative: addendum_hash,
        },
        addendum_path,
    )


class GeneratedActionSemanticLocalityTests(unittest.TestCase):
    def test_valid_red_panda_localized_native_action_passes(self) -> None:
        identity = make_identity()
        frames = make_frames(localized_belly_edits())
        contract.validate_high_res_four_frame_role(IDLE, frames)
        semantic = make_semantic_role(identity, frames)

        evidence = validate(identity, frames, semantic)

        self.assertEqual(evidence["role"], "idle")
        self.assertEqual(len(evidence["phases"]), 4)
        self.assertEqual(len(set(evidence["motion_state_sha256"])), 4)
        self.assertEqual(
            evidence["motion_landmarks"][0]["changed_pixels"], 16
        )

    def test_role_p1_to_p3_share_one_immutable_p0_star_target(self) -> None:
        identity, frames, semantic = simple_role_base_fixture()

        evidence = validate_role_base(
            "red_panda", identity, SLEEP, frames, semantic
        )

        star_phases = evidence["phases"][1:]
        self.assertEqual(
            {phase["edit_target_relative_path"] for phase in star_phases},
            {"sleep/00.png"},
        )
        self.assertEqual(
            len(
                {
                    phase["edit_target_source_sha256"]
                    for phase in star_phases
                }
            ),
            1,
        )
        self.assertEqual(
            len(
                {
                    phase["edit_target_accepted_composited_frame_sha256"]
                    for phase in star_phases
                }
            ),
            1,
        )

        phases = list(semantic.phases)
        p3 = phases[3]
        phases[2] = replace(
            phases[2],
            edit_target_reference=replace(
                phases[2].edit_target_reference,
                relative_path="sleep/03.png",
                phase=3,
                source_sha256=p3.generated_asset.source_sha256,
                registered_frame_sha256=(
                    p3.generated_asset.registered_candidate_frame_sha256
                ),
                accepted_composited_frame_sha256=p3.composited_frame_sha256,
            ),
        )
        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"P1/P2/P3 do not share the same immutable P0",
        ):
            validate_role_base(
                "red_panda",
                identity,
                SLEEP,
                frames,
                replace(semantic, phases=tuple(phases)),
            )

    def test_role_p0_may_be_one_exact_no_call_identity_baseline_copy(self) -> None:
        identity, frames, semantic = identity_baseline_role_fixture()

        evidence = validate_role_base(
            "red_panda", identity, PET, frames, semantic
        )

        self.assertEqual(
            evidence["phases"][0]["generated_asset_layout"],
            contract.GENERATED_IDENTITY_BASELINE_ASSET_LAYOUT,
        )
        self.assertEqual(
            evidence["phases"][0]["generated_source_sha256"],
            identity.source_sha256,
        )
        self.assertEqual(
            evidence["phases"][0]["composited_frame_sha256"],
            hashlib.sha256(identity.packed).hexdigest(),
        )

        record = import_lock_record(semantic, identity)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "imagegen-lock.json"
            path.write_text(
                json.dumps(
                    {
                        "schema": contract.IMAGEGEN_IMPORT_LOCK_SCHEMA,
                        "identities": [record],
                    }
                ),
                encoding="utf-8",
            )
            loaded = contract.load_imagegen_import_locks(
                path, ["red_panda"]
            )
        self.assertEqual(
            loaded["red_panda"]
            .action_semantic_contract.roles[0]
            .phases[0]
            .generated_asset.layout,
            contract.GENERATED_IDENTITY_BASELINE_ASSET_LAYOUT,
        )

    def test_identity_baseline_copy_cannot_alias_a_later_phase(self) -> None:
        identity, frames, semantic = identity_baseline_role_fixture()
        phases = list(semantic.phases)
        phases[1] = replace(
            phases[1],
            generated_asset=replace(
                phases[1].generated_asset,
                layout=contract.GENERATED_IDENTITY_BASELINE_ASSET_LAYOUT,
            ),
        )
        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"no-call identity baseline must be one exact.*role P0 copy",
        ):
            validate_role_base(
                "red_panda",
                identity,
                PET,
                frames,
                replace(semantic, phases=tuple(phases)),
            )

    def test_edit_target_role_rules_fail_closed(self) -> None:
        identity = make_identity()
        idle_frames = make_frames(localized_belly_edits())
        idle_semantic = make_semantic_role(identity, idle_frames)
        idle_phases = list(idle_semantic.phases)
        idle_phases[1] = replace(
            idle_phases[1],
            edit_target_reference=contract.ImmutableEditTargetReference(
                kind="immutable-accepted-role-phase-0",
                relative_path="idle/00.png",
                identity_key="red_panda",
                role="idle",
                phase=0,
                source_sha256=idle_phases[0].generated_asset.source_sha256,
                registered_frame_sha256=(
                    idle_phases[0].generated_asset.registered_candidate_frame_sha256
                ),
                accepted_composited_frame_sha256=(
                    idle_phases[0].composited_frame_sha256
                ),
            ),
        )
        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"identity-anchored role cannot use a role-P0 edit target",
        ):
            validate(
                identity,
                idle_frames,
                replace(idle_semantic, phases=tuple(idle_phases)),
            )

        identity, role_frames, role_semantic = simple_role_base_fixture()
        role_phases = list(role_semantic.phases)
        role_phases[0] = replace(
            role_phases[0],
            edit_target_reference=replace(
                role_phases[1].edit_target_reference,
            ),
        )
        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"immutable role phase 0 must target the approved identity",
        ):
            validate_role_base(
                "red_panda",
                identity,
                SLEEP,
                role_frames,
                replace(role_semantic, phases=tuple(role_phases)),
            )

    def test_composition_baseline_and_preauthorization_hash_drift_fail(self) -> None:
        identity, frames, semantic = simple_role_base_fixture()
        phases = list(semantic.phases)
        phases[2] = replace(
            phases[2], composition_baseline_frame_sha256=digest("wrong-baseline")
        )
        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"bounded-composition provenance hash drifted",
        ):
            validate_role_base(
                "red_panda",
                identity,
                SLEEP,
                frames,
                replace(semantic, phases=tuple(phases)),
            )

        phases = list(semantic.phases)
        phases[2] = replace(
            phases[2],
            preauthorization_reference=replace(
                phases[2].preauthorization_reference,
                allowed_change_region_sha256=digest("wrong-frozen-mask"),
            ),
        )
        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"preauthorization path, target, or frozen mask drifted",
        ):
            validate_role_base(
                "red_panda",
                identity,
                SLEEP,
                frames,
                replace(semantic, phases=tuple(phases)),
            )

    def test_dynamic_preauthorization_record_is_rejected(self) -> None:
        identity = make_identity()
        frames = make_frames(localized_belly_edits())
        phase_lock = make_semantic_role(identity, frames).phases[0]
        raw = {
            "allowed_change_region": contract.native_region_mask_record(
                phase_lock.allowed_change_region
            ),
            "edit_target_kind": phase_lock.edit_target_reference.kind,
            "frozen_before_generation": True,
            "identity_key": "red_panda",
            "mask_authoring_basis": (
                "frozen-storyboard-native-region-before-generation"
            ),
            "phase": 0,
            "role": "idle",
            "schema": contract.GENERATED_PHASE_PREAUTHORIZATION_SCHEMA,
            "storyboard_sha256": (
                phase_lock.preauthorization_reference.storyboard_sha256
            ),
            "derived_from_generated_candidate_sha256": digest("candidate"),
        }
        with tempfile.TemporaryDirectory() as temporary:
            species_dir = Path(temporary) / "red_panda"
            path = species_dir / "preauthorization" / "idle" / "00.json"
            path.parent.mkdir(parents=True)
            path.write_text(json.dumps(raw), encoding="utf-8")
            phase_lock = replace(
                phase_lock,
                preauthorization_reference=replace(
                    phase_lock.preauthorization_reference,
                    source_sha256=contract.sha256_file(path),
                ),
            )
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"dynamic/generated-frame-derived masks are forbidden",
            ):
                contract.load_generated_phase_preauthorization(
                    species_dir, "red_panda", "idle", 0, phase_lock
                )

    def test_hash_pinned_pre_call_p0_rectangle_migrates_exactly(self) -> None:
        _identity, _frames, semantic = simple_role_base_fixture()
        with tempfile.TemporaryDirectory() as temporary:
            species_dir = Path(temporary) / "red_panda"
            phase_lock, expected_hashes, _freeze_path = (
                write_rectangle_migration_preauthorization(
                    species_dir, "sleep", semantic.phases[0]
                )
            )

            source_hashes = contract.load_generated_phase_preauthorization(
                species_dir, "red_panda", "sleep", 0, phase_lock
            )

        self.assertEqual(source_hashes, expected_hashes)
        self.assertEqual(len(phase_lock.allowed_change_region.mask), 60 * 76)
        self.assertFalse(
            any(
                y in contract.HIGH_RES_BOTTOM_GUARD_ROWS
                for _x, y in phase_lock.allowed_change_region.mask
            )
        )

    def test_hash_pinned_addendum_chain_inherits_only_base_rectangle(self) -> None:
        _identity, _frames, semantic = simple_role_base_fixture()
        with tempfile.TemporaryDirectory() as temporary:
            species_dir = Path(temporary) / "red_panda"
            phase_lock, _direct_hashes, base_freeze_path = (
                write_rectangle_migration_preauthorization(
                    species_dir, "sleep", semantic.phases[0]
                )
            )
            phase_lock, expected_hashes, addendum_path = (
                rewrite_rectangle_migration_as_addendum_chain(
                    species_dir, "sleep", phase_lock, base_freeze_path
                )
            )

            source_hashes = contract.load_generated_phase_preauthorization(
                species_dir, "red_panda", "sleep", 0, phase_lock
            )
            self.assertEqual(source_hashes, expected_hashes)

            addendum = json.loads(addendum_path.read_text(encoding="utf-8"))
            addendum["roles"][0]["raw_source_count_for_role_at_freeze"] = 1
            addendum_path.write_text(
                json.dumps(addendum, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
                newline="\n",
            )
            migrated = json.loads(
                (
                    species_dir
                    / phase_lock.preauthorization_reference.relative_path
                ).read_text(encoding="utf-8")
            )["rectangle_migration"]
            migrated["freeze_record_sha256"] = contract.sha256_file(
                addendum_path
            )
            new_relative = (
                f"preauthorization/_frozen-source/"
                f"{migrated['freeze_record_sha256']}.json"
            )
            new_path = species_dir / new_relative
            addendum_path.replace(new_path)
            migrated["freeze_record_relative_path"] = new_relative
            preauthorization_path = (
                species_dir
                / phase_lock.preauthorization_reference.relative_path
            )
            preauthorization = json.loads(
                preauthorization_path.read_text(encoding="utf-8")
            )
            preauthorization["rectangle_migration"] = migrated
            preauthorization_path.write_text(
                json.dumps(preauthorization, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
                newline="\n",
            )
            phase_lock = replace(
                phase_lock,
                preauthorization_reference=replace(
                    phase_lock.preauthorization_reference,
                    source_sha256=contract.sha256_file(preauthorization_path),
                ),
            )
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"addendum was not frozen before this role-P0 identity edit",
            ):
                contract.load_generated_phase_preauthorization(
                    species_dir, "red_panda", "sleep", 0, phase_lock
                )

    def test_rectangle_migration_freeze_or_materialized_mask_drift_fails(self) -> None:
        _identity, _frames, semantic = simple_role_base_fixture()
        with tempfile.TemporaryDirectory() as temporary:
            species_dir = Path(temporary) / "red_panda"
            phase_lock, _expected_hashes, freeze_path = (
                write_rectangle_migration_preauthorization(
                    species_dir, "sleep", semantic.phases[0]
                )
            )
            freeze_path.write_bytes(freeze_path.read_bytes() + b"\n")
            with self.assertRaisesRegex(
                contract.RasterContractError, r"freeze record SHA-256 drifted"
            ):
                contract.load_generated_phase_preauthorization(
                    species_dir, "red_panda", "sleep", 0, phase_lock
                )

        with tempfile.TemporaryDirectory() as temporary:
            species_dir = Path(temporary) / "red_panda"
            phase_lock, _expected_hashes, _freeze_path = (
                write_rectangle_migration_preauthorization(
                    species_dir, "sleep", semantic.phases[0]
                )
            )
            smaller = contract.native_region_mask_lock(
                set(phase_lock.allowed_change_region.mask) - {(2, 2)}
            )
            preauthorization_path = (
                species_dir
                / phase_lock.preauthorization_reference.relative_path
            )
            raw = json.loads(preauthorization_path.read_text(encoding="utf-8"))
            raw["allowed_change_region"] = contract.native_region_mask_record(
                smaller
            )
            preauthorization_path.write_text(
                json.dumps(raw, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
                newline="\n",
            )
            phase_lock = replace(
                phase_lock,
                allowed_change_region=smaller,
                preauthorization_reference=replace(
                    phase_lock.preauthorization_reference,
                    source_sha256=contract.sha256_file(preauthorization_path),
                    allowed_change_region_sha256=smaller.packed_sha256,
                ),
            )
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"materialized pre-call rectangle differs",
            ):
                contract.load_generated_phase_preauthorization(
                    species_dir, "red_panda", "sleep", 0, phase_lock
                )

    def test_rectangle_migration_is_forbidden_for_role_p1_to_p3(self) -> None:
        _identity, _frames, semantic = simple_role_base_fixture()
        with tempfile.TemporaryDirectory() as temporary:
            species_dir = Path(temporary) / "red_panda"
            phase_lock, _expected_hashes, _freeze_path = (
                write_rectangle_migration_preauthorization(
                    species_dir, "sleep", semantic.phases[1]
                )
            )
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"allowed only for role P0 targeting the immutable identity",
            ):
                contract.load_generated_phase_preauthorization(
                    species_dir, "red_panda", "sleep", 1, phase_lock
                )

    def test_nonzero_role_registration_is_reused_without_phase_fitting(self) -> None:
        identity, final_frames, _semantic = simple_role_base_fixture()
        imported = [
            make_role_frame(
                "red_panda",
                SLEEP,
                identity,
                {(x, y - 2) for x, y in frame.mask},
                phase,
                label="unregistered-high-p0",
            )
            for phase, frame in enumerate(final_frames)
        ]
        registration = contract.GeneratedRoleRegistrationLock(
            schema=contract.GENERATED_ROLE_REGISTRATION_SCHEMA,
            derivation="role-p0-fixed-dx-explicit-dy-floor-derived",
            output_offset=(0, 2),
            p0_unregistered_floor_y=75,
        )
        identity, frames, semantic = simple_role_base_fixture(
            imported_candidates=imported,
            registration=registration,
        )
        registered = [
            contract.register_generated_candidate(
                candidate,
                registration,
                label=f"red_panda/sleep/{phase}/accepted-registration",
            )
            for phase, candidate in enumerate(imported)
        ]

        evidence = validate_role_base(
            "red_panda",
            identity,
            SLEEP,
            frames,
            semantic,
            imported_candidates=imported,
            registered_candidates=registered,
        )
        self.assertEqual(evidence["role_registration"]["output_offset"], [0, 2])
        self.assertTrue(
            all(
                phase["registered_candidate_frame_sha256"]
                == frame_sha256(registered[index])
                for index, phase in enumerate(evidence["phases"])
            )
        )

        overridden = list(registered)
        overridden[2] = replace(
            registered[2],
            mask=frozenset({(x + 1, y) for x, y in registered[2].mask}),
            packed=contract.high_res_frame_bytes(
                {(x + 1, y) for x, y in registered[2].mask}
            ),
        )
        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"per-phase output-offset override.*detected",
        ):
            validate_role_base(
                "red_panda",
                identity,
                SLEEP,
                frames,
                semantic,
                imported_candidates=imported,
                registered_candidates=overridden,
            )

    def test_registration_drift_range_floor_and_clipping_are_rejected(self) -> None:
        identity = make_identity()
        frames = make_frames(localized_belly_edits())
        semantic = make_semantic_role(identity, frames)
        drifted_registration = replace(
            semantic.role_registration, output_offset=(1, 0)
        )
        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"identity-anchored role cannot use a role output offset",
        ):
            validate(
                identity,
                frames,
                replace(
                    semantic,
                    role_registration=drifted_registration,
                    role_registration_sha256=(
                        contract.generated_role_registration_sha256(
                            drifted_registration
                        )
                    ),
                ),
            )

        identity, role_frames, role_semantic = simple_role_base_fixture()
        wrong_floor = replace(
            role_semantic.role_registration,
            p0_unregistered_floor_y=76,
        )
        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"role-P0 dy is not derived from its unregistered floor",
        ):
            validate_role_base(
                "red_panda",
                identity,
                SLEEP,
                role_frames,
                replace(
                    role_semantic,
                    role_registration=wrong_floor,
                    role_registration_sha256=(
                        contract.generated_role_registration_sha256(wrong_floor)
                    ),
                ),
            )

        with self.assertRaisesRegex(
            contract.RasterContractError, r"role output offset is out of range"
        ):
            contract.register_generated_candidate(
                frames[0],
                contract.GeneratedRoleRegistrationLock(
                    schema=contract.GENERATED_ROLE_REGISTRATION_SCHEMA,
                    derivation="role-p0-fixed-dx-explicit-dy-floor-derived",
                    output_offset=(5, 0),
                    p0_unregistered_floor_y=77,
                ),
                label="red_panda/idle/out-of-range",
            )

        clipping_candidate = make_frame(
            set(frames[0].mask) | {(63, 40)}, 0, label="edge-candidate"
        )
        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"fixed role output offset clips the imported candidate",
        ):
            contract.register_generated_candidate(
                clipping_candidate,
                contract.GeneratedRoleRegistrationLock(
                    schema=contract.GENERATED_ROLE_REGISTRATION_SCHEMA,
                    derivation="role-p0-fixed-dx-explicit-dy-floor-derived",
                    output_offset=(1, 0),
                    p0_unregistered_floor_y=77,
                ),
                label="red_panda/idle/clipping",
            )

    def test_sleep_role_pose_may_lie_down_but_cannot_shimmer(self) -> None:
        identity = make_identity()
        lying_pose = {
            (x, y)
            for y in range(57, 73)
            for x in range(10, 53)
        }
        lying_pose.update(
            (x, y) for y in range(45, 62) for x in range(12, 27)
        )
        lying_pose.update(
            (x, y) for y in range(60, 69) for x in range(45, 58)
        )
        lying_pose.update(
            (x, y) for y in range(73, 78) for x in range(18, 46)
        )
        breathing = [
            {
                (29 + phase * 3 + dx, 64 + dy)
                for dx in range(2)
                for dy in range(2)
            }
            for phase in range(4)
        ]
        frames = [
            make_role_frame(
                "red_panda",
                SLEEP,
                identity,
                lying_pose - edits,
                phase,
                label="lying-breath",
            )
            for phase, edits in enumerate(breathing)
        ]
        motion = set().union(*breathing)
        protected = {(x, y) for y in range(50, 56) for x in range(15, 23)}
        semantic = make_role_base_semantic(
            "red_panda",
            SLEEP,
            identity,
            frames,
            contact_policy="planted-role-base",
            maximum_contact_changes=0,
            motion_region=motion,
            protected_region=protected,
            frozen_contact_regions=[
                {
                    (x, contract.HIGH_RES_FLOOR_Y)
                    for x in range(contract.HIGH_RES_FRAME_WIDTH)
                }
                for _phase in range(4)
            ],
            identity_pose_landmark={
                (x, y) for y in range(28, 34) for x in range(24, 32)
            },
            role_pose_landmark=protected,
        )

        evidence = validate_role_base(
            "red_panda", identity, SLEEP, frames, semantic
        )
        self.assertEqual(evidence["baseline_policy"], "immutable-role-phase-0")
        self.assertEqual(evidence["contact_policy"], "planted-role-base")
        self.assertGreater(
            evidence["phases"][0]["changed_from_semantic_baseline_pixels"],
            100,
        )

        shimmer_frames = list(frames)
        shimmer_mask = set(shimmer_frames[2].mask) - {(16, 51)}
        shimmer_frames[2] = make_role_frame(
            "red_panda",
            SLEEP,
            identity,
            shimmer_mask,
            2,
            label="lying-shimmer",
        )
        shimmer_semantic = make_role_base_semantic(
            "red_panda",
            SLEEP,
            identity,
            shimmer_frames,
            contact_policy="planted-role-base",
            maximum_contact_changes=0,
            motion_region=motion | {(16, 51)},
            protected_region=protected,
            frozen_contact_regions=[
                {
                    (x, contract.HIGH_RES_FLOOR_Y)
                    for x in range(contract.HIGH_RES_FRAME_WIDTH)
                }
                for _phase in range(4)
            ],
            identity_pose_landmark={
                (x, y) for y in range(28, 34) for x in range(24, 32)
            },
            role_pose_landmark=protected,
        )
        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"pixels shimmer inside a zero-tolerance frozen contact/anatomy region",
        ):
            validate_role_base(
                "red_panda", identity, SLEEP, shimmer_frames, shimmer_semantic
            )

    def test_explicit_turtle_surprise_retract_is_bounded_not_arbitrary(self) -> None:
        identity = replace(
            make_identity(),
            path=Path("turtle") / "identity.png",
            source_sha256=digest("turtle-identity-source"),
        )
        identity_mask = set(identity.mask)
        left_retract = {(x, contract.HIGH_RES_FLOOR_Y) for x in range(20, 24)}
        right_retract = {(x, contract.HIGH_RES_FLOOR_Y) for x in range(40, 44)}
        retracts = [set(), left_retract, right_retract, left_retract | right_retract]
        identity_contacts = {
            point
            for point in identity_mask
            if point[1] == contract.HIGH_RES_FLOOR_Y
        }
        frames = [
            make_role_frame(
                "turtle",
                SURPRISE,
                identity,
                identity_mask - edits,
                phase,
                label="retract",
            )
            for phase, edits in enumerate(retracts)
        ]
        protected = {(x, y) for y in range(28, 36) for x in range(24, 40)}
        semantic = make_role_base_semantic(
            "turtle",
            SURPRISE,
            identity,
            frames,
            contact_policy="bounded-approved-pose-change",
            maximum_contact_changes=8,
            motion_region=left_retract | right_retract,
            protected_region=protected,
            frozen_contact_regions=[
                identity_contacts - edits for edits in retracts
            ],
            identity_pose_landmark=protected,
            role_pose_landmark=protected,
        )

        evidence = validate_role_base(
            "turtle", identity, SURPRISE, frames, semantic
        )
        self.assertEqual(
            evidence["contact_policy"], "bounded-approved-pose-change"
        )
        self.assertEqual(
            evidence["phases"][3]["floor_contact_changed_pixels"], 8
        )

        incomplete_phases = list(semantic.phases)
        incomplete_frozen = list(incomplete_phases[1].frozen_regions)
        incomplete_frozen[0] = replace(
            incomplete_frozen[0],
            region=contract.native_region_mask_lock(
                {(24, contract.HIGH_RES_FLOOR_Y)}
            ),
        )
        incomplete_phases[1] = replace(
            incomplete_phases[1], frozen_regions=tuple(incomplete_frozen)
        )
        incomplete_semantic = replace(
            semantic, phases=tuple(incomplete_phases)
        )
        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"bounded contact policy leaves .* outside its exact frozen mask",
        ):
            validate_role_base(
                "turtle", identity, SURPRISE, frames, incomplete_semantic
            )

        arbitrary = {
            (x, contract.HIGH_RES_FLOOR_Y)
            for x in (*range(20, 24), *range(38, 44))
        }
        bad_retracts = [set(), left_retract, right_retract, arbitrary]
        bad_frames = [
            make_role_frame(
                "turtle",
                SURPRISE,
                identity,
                identity_mask - edits,
                phase,
                label="arbitrary-floor-churn",
            )
            for phase, edits in enumerate(bad_retracts)
        ]
        bad_semantic = make_role_base_semantic(
            "turtle",
            SURPRISE,
            identity,
            bad_frames,
            contact_policy="bounded-approved-pose-change",
            maximum_contact_changes=8,
            motion_region=arbitrary,
            protected_region=protected,
            frozen_contact_regions=[
                identity_contacts - edits for edits in bad_retracts
            ],
            identity_pose_landmark=protected,
            role_pose_landmark=protected,
        )
        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"bounded-approved-pose-change changes 10 floor pixels \(maximum 8\)",
        ):
            validate_role_base(
                "turtle", identity, SURPRISE, bad_frames, bad_semantic
            )

    def test_bounded_composite_discards_scattered_redraw_but_keeps_motion(self) -> None:
        identity = make_identity()
        intended = localized_belly_edits()
        scattered = [
            edits
            | {
                (22 + phase, 30),
                (48 + phase, 55),
                (21 + phase, 73),
            }
            for phase, edits in enumerate(intended)
        ]
        candidates = make_frames(scattered, label="scattered-candidate")
        frames = make_frames(intended, label="bounded-final")
        semantic = make_semantic_role(
            identity,
            frames,
            allowed_regions=intended,
            motion_region=set().union(*intended),
            imported_candidates=candidates,
        )

        evidence = validate(
            identity,
            frames,
            semantic,
            imported_candidates=candidates,
        )

        for phase, phase_evidence in enumerate(evidence["phases"]):
            self.assertEqual(
                set(frames[phase].mask) & intended[phase],
                set(candidates[phase].mask) & intended[phase],
            )
            self.assertEqual(
                (set(identity.mask) ^ set(frames[phase].mask)) - intended[phase],
                set(),
            )
            self.assertEqual(
                phase_evidence["discarded_candidate_outside_mask_pixels"],
                3,
            )

    def test_nonzero_out_of_region_budget_is_never_a_production_default(self) -> None:
        identity = make_identity()
        frames = make_frames(localized_belly_edits())
        semantic = make_semantic_role(identity, frames)
        phases = list(semantic.phases)
        phases[0] = replace(
            phases[0], maximum_out_of_region_changed_pixels=1
        )
        semantic = replace(semantic, phases=tuple(phases))

        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"production out-of-region budget must be zero",
        ):
            validate(identity, frames, semantic)

    def test_planted_paw_mutation_is_zero_tolerance_even_when_allowed(self) -> None:
        identity = make_identity()
        edits = localized_belly_edits()
        edits[2] = edits[2] | {(21, contract.HIGH_RES_FLOOR_Y)}
        frames = make_frames(edits, label="paw-mutation")
        semantic = make_semantic_role(identity, frames, allowed_regions=edits)

        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"zero-tolerance frozen contact/anatomy|planted paw/contact mutated",
        ):
            validate(identity, frames, semantic)

    def test_generated_f1_cannot_be_the_f2_generation_reference(self) -> None:
        identity = make_identity()
        frames = make_frames(localized_belly_edits())
        semantic = make_semantic_role(identity, frames)
        phases = list(semantic.phases)
        phases[2] = replace(
            phases[2],
            identity_reference=contract.ImmutableIdentityReference(
                kind="generated-phase",
                relative_path="idle/01.png",
                identity_key="red_panda",
                source_sha256=frames[1].source_sha256,
                frame_sha256=hashlib.sha256(frames[1].packed).hexdigest(),
            ),
        )
        semantic = replace(semantic, phases=tuple(phases))

        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"generated-phase chaining is forbidden",
        ):
            validate(identity, frames, semantic)

    def test_four_unique_noise_frames_do_not_prove_role_motion(self) -> None:
        identity = make_identity()
        noise = [
            {(22 + phase, 38), (48 + phase, 55), (21 + phase, 73), (42, 44 + phase)}
            for phase in range(4)
        ]
        frames = make_frames(noise, label="noise-only")
        intended_motion = set().union(*localized_belly_edits())
        allowed = [phase_noise | intended_motion for phase_noise in noise]
        semantic = make_semantic_role(
            identity,
            frames,
            allowed_regions=allowed,
            motion_region=intended_motion,
        )

        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"off-role noise cannot prove animation|not four unique role-motion states",
        ):
            validate(identity, frames, semantic)

    def test_off_role_noise_cannot_hide_one_pixel_landmark_shimmer(self) -> None:
        identity = make_identity()
        motion_points = [(27 + index, 55) for index in range(4)]
        motion_edits = [
            set(motion_points[:count]) for count in (0, 1, 2, 4)
        ]
        noise_edits = [
            {
                (20 + phase * 5 + dx, 45 + dy)
                for dx in range(2)
                for dy in range(2)
            }
            for phase in range(4)
        ]
        edits = [
            motion | noise
            for motion, noise in zip(motion_edits, noise_edits, strict=True)
        ]
        frames = make_frames(edits, label="noise-hides-shimmer")
        contract.validate_high_res_four_frame_role(IDLE, frames)
        semantic = make_semantic_role(
            identity,
            frames,
            motion_region=set(motion_points),
        )

        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"adjacent phases change only .*off-role noise cannot hide",
        ):
            validate(identity, frames, semantic)

    def test_unauthorized_contact_policy_is_rejected_for_idle(self) -> None:
        identity = make_identity()
        frames = make_frames(localized_belly_edits())
        semantic = make_semantic_role(identity, frames)
        semantic = replace(
            semantic,
            contact_policy="bounded-approved-gait-lift",
            maximum_contact_changed_pixels_per_phase=4,
        )

        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"contact policy .* is not an authorized storyboard capability",
        ):
            validate(identity, frames, semantic)

    def test_missing_and_hash_drifted_masks_fail_closed_at_lock_load(self) -> None:
        identity = make_identity()
        frames = make_frames(localized_belly_edits())
        semantic = make_semantic_role(identity, frames)
        record = import_lock_record(semantic, identity)
        contract_record = record["action_semantic_contract"]

        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "imagegen-lock.json"
            missing = json.loads(json.dumps(record))
            del missing["action_semantic_contract"]["roles"][0]["phases"][0][
                "allowed_change_region"
            ]
            path.write_text(
                json.dumps(
                    {
                        "schema": contract.IMAGEGEN_IMPORT_LOCK_SCHEMA,
                        "identities": [missing],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                contract.RasterContractError, r"exact phase semantic lock is required"
            ):
                contract.load_imagegen_import_locks(path, ["red_panda"])

            drifted = json.loads(json.dumps(record))
            mask = drifted["action_semantic_contract"]["roles"][0]["phases"][0][
                "allowed_change_region"
            ]
            replacement = "1" if mask["packed_hex"][0] == "0" else "0"
            mask["packed_hex"] = replacement + mask["packed_hex"][1:]
            path.write_text(
                json.dumps(
                    {
                        "schema": contract.IMAGEGEN_IMPORT_LOCK_SCHEMA,
                        "identities": [drifted],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"native semantic region mask SHA-256 drifted",
            ):
                contract.load_imagegen_import_locks(path, ["red_panda"])

        self.assertEqual(
            contract_record["schema"], contract.GENERATED_ACTION_SEMANTIC_SCHEMA
        )

    def test_older_v2_generated_lock_is_unsafe_and_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "old-imagegen-lock.json"
            path.write_text(
                json.dumps(
                    {
                        "schema": "kitsu-wild-imagegen-import-lock-v2",
                        "identities": [],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"schema must be kitsu-wild-imagegen-import-lock-v4",
            ):
                contract.load_imagegen_import_locks(path, ["red_panda"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
