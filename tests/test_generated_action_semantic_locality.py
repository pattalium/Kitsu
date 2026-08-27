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


def make_semantic_role(
    identity: contract.HighResFrame,
    frames: list[contract.HighResFrame],
    *,
    allowed_regions: list[set[tuple[int, int]]] | None = None,
    motion_region: set[tuple[int, int]] | None = None,
) -> contract.GeneratedRoleSemanticLock:
    identity_mask = set(identity.mask)
    deltas = [identity_mask ^ set(frame.mask) for frame in frames]
    allowed = deltas if allowed_regions is None else allowed_regions
    if motion_region is None:
        motion_region = set().union(*deltas)
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
    phases = tuple(
        contract.GeneratedPhaseSemanticLock(
            phase=phase,
            semantic_baseline="approved-identity",
            identity_reference=reference,
            generated_asset=contract.GeneratedPhaseAsset(
                layout="independent-frame",
                relative_path=f"idle/{phase:02d}.png",
                source_sha256=frame.source_sha256,
                source_region_sha256=frame.source_sha256,
            ),
            allowed_change_region=contract.native_region_mask_lock(
                allowed[phase]
            ),
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
        for phase, frame in enumerate(frames)
    )
    return contract.GeneratedRoleSemanticLock(
        role="idle",
        baseline_policy="identity-anchored",
        contact_policy="planted-identity",
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
        phases=phases,
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
) -> dict[str, object]:
    return contract.validate_generated_action_semantic_role(
        "red_panda",
        identity,
        IDLE,
        frames,
        semantic,
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
    frozen_anatomy = contract.native_region_mask_lock(protected_region)
    phases: list[contract.GeneratedPhaseSemanticLock] = []
    for phase, frame in enumerate(frames):
        baseline = identity_mask if phase == 0 else role_pose
        delta = baseline ^ set(frame.mask)
        allowed = delta or motion_region
        phases.append(
            contract.GeneratedPhaseSemanticLock(
                phase=phase,
                semantic_baseline=(
                    "approved-identity-pose-gate"
                    if phase == 0
                    else "immutable-role-phase-0"
                ),
                identity_reference=reference,
                generated_asset=contract.GeneratedPhaseAsset(
                    layout="independent-frame",
                    relative_path=f"{role.name}/{phase:02d}.png",
                    source_sha256=frame.source_sha256,
                    source_region_sha256=frame.source_sha256,
                ),
                allowed_change_region=contract.native_region_mask_lock(allowed),
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
) -> dict[str, object]:
    return contract.validate_generated_action_semantic_role(
        species,
        identity,
        role,
        frames,
        semantic,
        identity_source_sha256=identity.source_sha256,
        identity_frame_sha256=hashlib.sha256(identity.packed).hexdigest(),
        source_layout="independent-frame",
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

    def test_ferret_style_scattered_head_tail_and_paw_edits_are_rejected(self) -> None:
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
        frames = make_frames(scattered, label="scattered")
        semantic = make_semantic_role(
            identity,
            frames,
            allowed_regions=intended,
            motion_region=set().union(*intended),
        )

        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"outside the exact allowed-change region.*scattered head/tail/paw",
        ):
            validate(identity, frames, semantic)

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
                r"schema must be kitsu-wild-imagegen-import-lock-v3",
            ):
                contract.load_imagegen_import_locks(path, ["red_panda"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
