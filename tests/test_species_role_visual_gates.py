#!/usr/bin/env python3
"""Focused hostile tests for species/role-specific native visual gates.

All artwork in this module is synthetic.  The fixtures are exact 64x80
``HighResFrame`` values whose packed bytes are derived directly from their
masks; no private production art, paths, or hashes enter the canonical tests.
"""

from __future__ import annotations

import copy
import hashlib
import json
import sys
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import build_default_packs as base  # noqa: E402
import build_wild_packs as wild  # noqa: E402
import companion_raster_contract as contract  # noqa: E402
import sync_wild_portraits as portrait_sync  # noqa: E402


POLICY_PATH = ROOT / contract.SPECIES_ROLE_VISUAL_GATE_POLICY_RELATIVE_PATH
ROLES = {role.name: role for role in base.ROLE_SPECS}


def rectangle(left: int, top: int, right: int, bottom: int) -> set[tuple[int, int]]:
    return {
        (x, y)
        for y in range(top, bottom + 1)
        for x in range(left, right + 1)
    }


def first_points(
    points: set[tuple[int, int]], count: int
) -> set[tuple[int, int]]:
    ordered = sorted(points, key=lambda point: (point[1], point[0]))
    if len(ordered) < count:
        raise AssertionError(f"fixture requested {count} of {len(ordered)} points")
    return set(ordered[:count])


def path8(
    start: tuple[int, int], end: tuple[int, int]
) -> set[tuple[int, int]]:
    """Return a minimal path that deliberately uses diagonal adjacency."""

    x, y = start
    target_x, target_y = end
    points = {(x, y)}
    while (x, y) != (target_x, target_y):
        if x < target_x:
            x += 1
        elif x > target_x:
            x -= 1
        if y < target_y:
            y += 1
        elif y > target_y:
            y -= 1
        points.add((x, y))
    return points


AX_TAIL_BREAK = (40, 58)


def ax_tail_body() -> set[tuple[int, int]]:
    # Body -> lower tail begins with diagonal-only links; lower -> upper tail
    # also uses diagonal links.  Four-neighbor code would split this control.
    return path8((30, 60), (48, 58)) | path8((48, 58), (54, 46))


def source_metrics(mask: set[tuple[int, int]]) -> contract.SourceMetrics:
    components = base.connected_components(mask)
    return contract.SourceMetrics(
        bounds=base.bounds(mask),
        ink_pixels=len(mask),
        components=len(components),
        smallest_component_pixels=min(len(component) for component in components),
        primary_fraction=max(len(component) for component in components) / len(mask),
    )


def make_frame(
    identity_key: str,
    role_name: str,
    phase: int,
    mask: set[tuple[int, int]],
) -> contract.HighResFrame:
    packed = contract.high_res_frame_bytes(mask)
    source_sha256 = hashlib.sha256(
        f"{identity_key}/{role_name}/{phase}/".encode("ascii") + packed
    ).hexdigest()
    return contract.HighResFrame(
        path=Path(identity_key) / role_name / f"{phase:02d}.png",
        role=role_name,
        phase=phase,
        source_sha256=source_sha256,
        mask=frozenset(mask),
        metrics=source_metrics(mask),
        apparent_scale_ratio=1.0,
        identity_jaccard=1.0,
        packed=packed,
    )


def make_frames(
    identity_key: str,
    role_name: str,
    masks: list[set[tuple[int, int]]],
) -> list[contract.HighResFrame]:
    if len(masks) != 4:
        raise AssertionError("visual fixture must contain P0..P3")
    return [
        make_frame(identity_key, role_name, phase, set(mask))
        for phase, mask in enumerate(masks)
    ]


def replace_masks(
    identity_key: str,
    role_name: str,
    frames: list[contract.HighResFrame],
    masks: list[set[tuple[int, int]]],
) -> list[contract.HighResFrame]:
    if len(frames) != len(masks):
        raise AssertionError("replacement phase count drifted")
    return make_frames(identity_key, role_name, masks)


def ax_idle_frames(*, p3_gill_pixels: int = 8) -> list[contract.HighResFrame]:
    body = ax_tail_body()
    gill = rectangle(2, 34, 18, 61)
    masks = [
        body,
        body | first_points(gill, 4),
        body | first_points(gill, 6),
        body | first_points(gill, p3_gill_pixels),
    ]
    return make_frames("axolotl", "idle", masks)


LEFT_BLINK_RECT = rectangle(12, 60, 14, 64)
RIGHT_BLINK_RECT = rectangle(23, 61, 25, 65)


def ax_blink_frames(
    *, p3_left: int = 8, p3_right: int = 8
) -> list[contract.HighResFrame]:
    body = ax_tail_body()
    counts = ((10, 10), (9, 9), (2, 2), (p3_left, p3_right))
    masks = [
        body
        | first_points(LEFT_BLINK_RECT, left_count)
        | first_points(RIGHT_BLINK_RECT, right_count)
        for left_count, right_count in counts
    ]
    return make_frames("axolotl", "blink", masks)


LEFT_PUPIL = rectangle(10, 60, 12, 62)
RIGHT_PUPIL = rectangle(22, 61, 24, 63)
AX_DORSAL_GILL = {(14, 46), (14, 47), (15, 47)}
AX_MIDDLE_GILL = {(10, 52), (11, 52), (11, 53)}
AX_VENTRAL_GILL = {(8, 57), (8, 58), (8, 59)}


def ring4(left: int, top: int) -> set[tuple[int, int]]:
    return {
        (x, y)
        for y in range(top, top + 4)
        for x in range(left, left + 4)
        if x in {left, left + 3} or y in {top, top + 3}
    }


def same_centroid_ring_redraw(
    ring: set[tuple[int, int]], left: int, top: int
) -> set[tuple[int, int]]:
    # Removed and added pairs have the same summed coordinates. Pixel count,
    # 4x4 bounds, and centroid stay fixed while the component shape changes.
    return (
        ring - {(left + 1, top), (left + 2, top + 3)}
    ) | {(left + 1, top + 1), (left + 2, top + 2)}


def ax_pet_mask() -> set[tuple[int, int]]:
    return (
        ax_tail_body()
        | LEFT_PUPIL
        | RIGHT_PUPIL
        | AX_DORSAL_GILL
        | AX_MIDDLE_GILL
        | AX_VENTRAL_GILL
    )


def ax_pet_frames() -> list[contract.HighResFrame]:
    base_mask = ax_pet_mask()
    return make_frames("axolotl", "pet", [base_mask] * 4)


def ax_pet_unique_frames() -> list[contract.HighResFrame]:
    """Builder-positive Pet frames with one shared rigid transform per phase."""

    translations = ((0, 0), (1, 0), (0, 1), (1, 1))
    moving_landmarks = (
        LEFT_PUPIL
        | RIGHT_PUPIL
        | AX_DORSAL_GILL
        | AX_MIDDLE_GILL
        | AX_VENTRAL_GILL
    )
    masks = [
        ax_tail_body()
        | {(x + dx, y + dy) for x, y in moving_landmarks}
        for dx, dy in translations
    ]
    return make_frames("axolotl", "pet", masks)


RABBIT_NOSE = {
    (13, 53),
    (16, 53),
    (13, 54),
    (14, 54),
    (15, 54),
    (16, 54),
}


def rabbit_idle_frames() -> list[contract.HighResFrame]:
    base_mask = RABBIT_NOSE | {(30, 70)}
    return make_frames("rabbit", "idle", [base_mask] * 4)


def rabbit_blink_masks() -> list[set[tuple[int, int]]]:
    return [
        rectangle(20, 47, 24, 50),  # 20 pixels; centroid (22, 48.5)
        rectangle(20, 48, 24, 49),  # 10 pixels; 0.5 mass ratio
        rectangle(20, 49, 24, 49),  # 5-pixel, one-row closed lid
        rectangle(20, 47, 23, 50),  # 16 pixels; 0.8 recovery ratio
    ]


def rabbit_blink_frames() -> list[contract.HighResFrame]:
    return make_frames("rabbit", "blink", rabbit_blink_masks())


def rabbit_listen_frames() -> list[contract.HighResFrame]:
    base_mask = rectangle(10, 20, 40, 60)
    return make_frames("rabbit", "listen", [base_mask] * 4)


def positive_fixture(
    key: tuple[str, str]
) -> tuple[list[contract.HighResFrame], tuple[int, int, int, int]]:
    identity_key, role_name = key
    if key == ("axolotl", "idle"):
        frames = ax_idle_frames()
    elif key == ("axolotl", "blink"):
        frames = ax_blink_frames()
    elif key == ("axolotl", "pet"):
        frames = ax_pet_frames()
    elif key == ("rabbit", "idle"):
        frames = rabbit_idle_frames()
    elif key == ("rabbit", "blink"):
        frames = rabbit_blink_frames()
    elif key == ("rabbit", "listen"):
        frames = rabbit_listen_frames()
    else:
        raise AssertionError(f"unhandled positive fixture {identity_key}/{role_name}")
    return frames, ROLES[role_name].durations_ms


class SpeciesRoleVisualGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.policy = contract.load_species_role_visual_gate_policy(POLICY_PATH)

    def load_payload(
        self, payload: dict[str, object]
    ) -> contract.SpeciesRoleVisualGatePolicy:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / "species-role-visual-gates-v1.json"
        path.write_text(
            json.dumps(payload, indent=2) + "\n",
            encoding="utf-8",
            newline="\n",
        )
        return contract.load_species_role_visual_gate_policy(path)

    def load_raw(self, raw: str) -> contract.SpeciesRoleVisualGatePolicy:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / "species-role-visual-gates-v1.json"
        path.write_text(raw, encoding="utf-8", newline="\n")
        return contract.load_species_role_visual_gate_policy(path)

    def validate(
        self,
        identity_key: str,
        role_name: str,
        frames: list[contract.HighResFrame],
        durations_ms: tuple[int, int, int, int] | list[int] | None = None,
        *,
        policy: contract.SpeciesRoleVisualGatePolicy | None = None,
        identity_frame_sha256: str | None = None,
    ) -> dict[str, object] | None:
        selected_policy = self.policy if policy is None else policy
        identity_hash = (
            selected_policy.identity_frame_sha256[identity_key]
            if identity_frame_sha256 is None
            else identity_frame_sha256
        )
        actual_durations = (
            ROLES[role_name].durations_ms
            if durations_ms is None
            else durations_ms
        )
        return contract.validate_species_role_visual_gates(
            selected_policy,
            identity_key,
            identity_hash,
            ROLES[role_name],
            frames,
            actual_durations,
        )

    def assert_reason(
        self,
        expected: str,
        identity_key: str,
        role_name: str,
        frames: list[contract.HighResFrame],
        durations_ms: tuple[int, int, int, int] | list[int] | None = None,
    ) -> contract.SpeciesRoleVisualGateError:
        actual_durations = (
            ROLES[role_name].durations_ms
            if durations_ms is None
            else durations_ms
        )
        with self.assertRaises(contract.SpeciesRoleVisualGateError) as raised:
            contract.validate_species_role_visual_gates(
                self.policy,
                identity_key,
                self.policy.identity_frame_sha256[identity_key],
                ROLES[role_name],
                frames,
                actual_durations,
            )
        self.assertEqual(raised.exception.reason_code, expected)
        self.assertEqual(raised.exception.reason_codes, (expected,))
        self.assertIn(expected, str(raised.exception))
        return raised.exception

    def assert_reasons(
        self,
        expected: tuple[str, ...],
        identity_key: str,
        role_name: str,
        frames: list[contract.HighResFrame],
        durations_ms: tuple[int, int, int, int] | list[int] | None = None,
    ) -> contract.SpeciesRoleVisualGateError:
        if not expected:
            raise AssertionError("aggregate hostile must expect at least one reason")
        actual_durations = (
            ROLES[role_name].durations_ms
            if durations_ms is None
            else durations_ms
        )
        with self.assertRaises(contract.SpeciesRoleVisualGateError) as raised:
            contract.validate_species_role_visual_gates(
                self.policy,
                identity_key,
                self.policy.identity_frame_sha256[identity_key],
                ROLES[role_name],
                frames,
                actual_durations,
            )
        self.assertEqual(raised.exception.reason_codes, expected)
        self.assertEqual(raised.exception.reason_code, expected[0])
        message_offsets = [str(raised.exception).index(code) for code in expected]
        self.assertEqual(message_offsets, sorted(message_offsets))
        return raised.exception

    def canonical_payload(self) -> dict[str, object]:
        return json.loads(POLICY_PATH.read_text(encoding="utf-8"))

    def sync_policy(self) -> portrait_sync.SpeciesRoleVisualGatePolicy:
        return portrait_sync.load_species_role_visual_gate_policy(ROOT)

    def minimal_manifest_header(
        self,
        schema: str = portrait_sync.VISUAL_GATE_MANIFEST_SCHEMA,
        *,
        policy: portrait_sync.SpeciesRoleVisualGatePolicy | None = None,
    ) -> dict[str, object]:
        if schema == portrait_sync.LEGACY_MANIFEST_SCHEMA:
            animation_contract = copy.deepcopy(
                portrait_sync.LEGACY_REQUIRED_ANIMATION_CONTRACT_VALUES
            )
        elif schema == portrait_sync.MANIFEST_SCHEMA:
            animation_contract = copy.deepcopy(
                portrait_sync.REQUIRED_ANIMATION_CONTRACT_VALUES
            )
        elif schema == portrait_sync.VISUAL_GATE_MANIFEST_SCHEMA:
            animation_contract = copy.deepcopy(
                portrait_sync.VISUAL_GATE_REQUIRED_ANIMATION_CONTRACT_VALUES
            )
        else:
            raise AssertionError(f"unsupported test manifest schema {schema}")
        manifest: dict[str, object] = {
            "animation_contract": animation_contract,
            "complete_roster": True,
            "identity_lock_schema": portrait_sync.DIRECT_LOCK_SCHEMA,
            "non_destructive_build": True,
            "raster_contract": copy.deepcopy(
                portrait_sync.EXPECTED_RASTER_CONTRACT
            ),
            "schema": schema,
        }
        if schema == portrait_sync.VISUAL_GATE_MANIFEST_SCHEMA:
            selected_policy = self.sync_policy() if policy is None else policy
            manifest["species_role_visual_gate_policy"] = copy.deepcopy(
                selected_policy.provenance_record
            )
        return manifest

    def synthetic_role_handoff(
        self, key: tuple[str, str]
    ) -> tuple[dict[str, object], dict[str, object], dict[str, object]]:
        frames, durations = positive_fixture(key)
        evidence = self.validate(key[0], key[1], frames, durations)
        if evidence is None:
            raise AssertionError(f"configured fixture {key!r} produced no evidence")
        role_record: dict[str, object] = {
            "durations_ms": list(evidence["durations_ms"]),
            "frame_sha256": list(evidence["frame_sha256"]),
            "role": key[1],
            "species_role_visual_gate": copy.deepcopy(evidence),
            "species_role_visual_gate_sha256": (
                contract.species_role_visual_gate_evidence_sha256(evidence)
            ),
        }
        identity_lock: dict[str, object] = {
            "identity_frame_sha256": evidence["identity_frame_sha256"]
        }
        return role_record, identity_lock, evidence

    def rehash_role_evidence(self, role_record: dict[str, object]) -> None:
        evidence = role_record.get("species_role_visual_gate")
        if not isinstance(evidence, dict):
            raise AssertionError("test role record does not contain evidence")
        role_record["species_role_visual_gate_sha256"] = (
            portrait_sync.canonical_json_sha256(evidence)
        )

    def synthetic_axolotl_builder_fixture(
        self,
    ) -> tuple[
        contract.SpeciesRoleVisualGatePolicy,
        contract.HighResIdentityLock,
        contract.HighResSpeciesRaster,
    ]:
        identity = make_frame(
            "axolotl",
            "identity",
            0,
            rectangle(24, 20, 39, 65),
        )
        identity_frame_sha256 = hashlib.sha256(identity.packed).hexdigest()
        payload = self.canonical_payload()
        payload["identity_frame_sha256"]["axolotl"] = identity_frame_sha256
        policy = self.load_payload(payload)

        frames: list[contract.HighResFrame] = []
        for role in base.ROLE_SPECS:
            if role.name == "idle":
                role_frames = ax_idle_frames()
            elif role.name == "blink":
                role_frames = ax_blink_frames()
            elif role.name == "pet":
                role_frames = ax_pet_unique_frames()
            else:
                body = rectangle(20, 20, 30, 30)
                role_frames = make_frames(
                    "axolotl",
                    role.name,
                    [
                        body | {(31, 20 + phase)}
                        for phase in range(4)
                    ],
                )
            if len({frame.packed for frame in role_frames}) != 4:
                raise AssertionError(
                    f"builder fixture {role.name} frames are not unique"
                )
            frames.extend(role_frames)
        if len(frames) != len(base.ROLE_SPECS) * 4:
            raise AssertionError("builder fixture does not contain exact 48 frames")

        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        portrait_path = Path(temporary.name) / "axolotl-portrait.png"
        portrait_source = b"synthetic-16x18-portrait-source"
        portrait_path.write_bytes(portrait_source)
        portrait = contract.HighResPortrait(
            path=portrait_path,
            source_sha256=hashlib.sha256(portrait_source).hexdigest(),
            mask=frozenset(rectangle(4, 3, 11, 14)),
            packed=bytes(range(wild.PORTRAIT_BYTES)),
        )
        raster = contract.HighResSpeciesRaster(
            identity=identity,
            portrait=portrait,
            frames=tuple(frames),
            source_sha256={
                "identity.png": identity.source_sha256,
                "portrait.png": portrait.source_sha256,
                **{
                    frame.path.as_posix(): frame.source_sha256
                    for frame in frames
                },
            },
            fixed_action_scale=1.0,
        )
        identity_lock = contract.HighResIdentityLock(
            identity_key="axolotl",
            identity_sha256=identity.source_sha256,
            frame_canvas=(64, 80),
            approved=True,
        )
        self.assertEqual(
            policy.identity_frame_sha256["axolotl"],
            identity_frame_sha256,
        )
        return policy, identity_lock, raster

    def test_all_six_synthetic_positive_roles_pass_exact_packing(self) -> None:
        for key in contract.SPECIES_ROLE_VISUAL_GATE_KEYS:
            with self.subTest(role_key="/".join(key)):
                frames, durations = positive_fixture(key)
                evidence = self.validate(key[0], key[1], frames, durations)
                self.assertIsNotNone(evidence)
                assert evidence is not None
                self.assertEqual(evidence["status"], "pass")
                self.assertEqual(evidence["identity_key"], key[0])
                self.assertEqual(evidence["role"], key[1])
                self.assertEqual(
                    set(evidence["gate_results"]),
                    set(self.policy.entries[key].gates),
                )
                self.assertEqual(
                    evidence["frame_sha256"],
                    [hashlib.sha256(frame.packed).hexdigest() for frame in frames],
                )
                self.assertTrue(
                    all(
                        contract.high_res_frame_bytes(set(frame.mask)) == frame.packed
                        for frame in frames
                    )
                )

    def test_ax_tail_control_uses_eight_neighbors_and_gap_is_rejected_for_each_role(self) -> None:
        control = ax_tail_body()
        self.assertIn(AX_TAIL_BREAK, control)
        self.assertEqual(len(base.connected_components(control)), 1)
        broken_control = control - {AX_TAIL_BREAK}
        self.assertGreater(len(base.connected_components(broken_control)), 1)

        for role_name in ("idle", "blink", "pet"):
            with self.subTest(role=role_name):
                frames, _durations = positive_fixture(("axolotl", role_name))
                broken_masks = [set(frame.mask) - {AX_TAIL_BREAK} for frame in frames]
                broken = replace_masks("axolotl", role_name, frames, broken_masks)
                self.assert_reason(
                    "AX_REQUIRED_LANDMARK_DISCONNECTED",
                    "axolotl",
                    role_name,
                    broken,
                )

    def test_ax_anchor_rejects_detached_upper_tail_island_for_each_role(self) -> None:
        detached_island = {(59, 36), (60, 36), (60, 37)}
        for role_name in ("idle", "blink", "pet"):
            with self.subTest(role=role_name):
                frames, _durations = positive_fixture(("axolotl", role_name))
                island_masks = [
                    set(frame.mask) | detached_island for frame in frames
                ]
                self.assert_reason(
                    "AX_REQUIRED_LANDMARK_DISCONNECTED",
                    "axolotl",
                    role_name,
                    replace_masks(
                        "axolotl", role_name, frames, island_masks
                    ),
                )

    def test_axolotl_idle_rejects_phase_escape_and_loop_seam(self) -> None:
        frames = ax_idle_frames()
        escaped_masks = [set(frame.mask) for frame in frames]
        escaped_masks[1].update({(x, 0) for x in range(5)})
        escaped = replace_masks("axolotl", "idle", frames, escaped_masks)
        self.assert_reason(
            "AX_IDLE_GILL_PHASE_LOCALITY", "axolotl", "idle", escaped
        )

        seam = ax_idle_frames(p3_gill_pixels=17)
        self.assert_reason("AX_IDLE_LOOP_SEAM", "axolotl", "idle", seam)

    def test_axolotl_blink_rejects_non_pupil_edit_and_bad_per_eye_recovery(self) -> None:
        frames = ax_blink_frames()
        outside_masks = [set(frame.mask) for frame in frames]
        outside_masks[1].add((0, 0))
        outside = replace_masks("axolotl", "blink", frames, outside_masks)
        self.assert_reason("AX_BLINK_PUPIL_ONLY", "axolotl", "blink", outside)

        bad_recovery = ax_blink_frames(p3_left=2, p3_right=2)
        self.assert_reason(
            "AX_BLINK_PER_EYE_OCCUPANCY",
            "axolotl",
            "blink",
            bad_recovery,
        )

    def test_axolotl_blink_rejects_full_component_boundary_escape(self) -> None:
        cases = {
            "left-border": {(11, 60)},
            # x=25 keeps P2 within its allowed three-pixel occupancy while
            # x=26 crosses the right ROI border on the same component.
            "right-border": {(25, 61), (26, 61)},
        }
        for label, escaped_pixels in cases.items():
            with self.subTest(boundary=label):
                frames = ax_blink_frames()
                masks = [
                    set(frame.mask) | escaped_pixels for frame in frames
                ]
                self.assert_reason(
                    "AX_BLINK_PER_EYE_OCCUPANCY",
                    "axolotl",
                    "blink",
                    replace_masks("axolotl", "blink", frames, masks),
                )

    def test_axolotl_pet_rejects_redraw_nonrigid_pupils_and_gill_drift(self) -> None:
        frames = ax_pet_frames()

        redraw_masks = [set(frame.mask) for frame in frames]
        redraw_masks[1].update(rectangle(40, 2, 62, 8))  # exactly 161 pixels
        redraw = replace_masks("axolotl", "pet", frames, redraw_masks)
        self.assert_reason("AX_PET_REDRAW_BUDGET", "axolotl", "pet", redraw)

        pupil_masks = [set(frame.mask) for frame in frames]
        pupil_masks[1].difference_update(LEFT_PUPIL)
        pupil_masks[1].update({(x + 2, y) for x, y in LEFT_PUPIL})
        nonrigid = replace_masks("axolotl", "pet", frames, pupil_masks)
        self.assert_reason(
            "AX_PET_NONRIGID_PUPIL_TRANSFORM",
            "axolotl",
            "pet",
            nonrigid,
        )

        gill_masks = [set(frame.mask) for frame in frames]
        gill_masks[1].difference_update(AX_DORSAL_GILL)
        gill_masks[1].update({(15, 46), (16, 46), (16, 47)})
        gill_drift = replace_masks("axolotl", "pet", frames, gill_masks)
        self.assert_reason("AX_PET_GILL_BASE_DRIFT", "axolotl", "pet", gill_drift)

    def test_axolotl_pet_requires_exact_integer_p0_pupil_masks(self) -> None:
        positive = ax_pet_unique_frames()
        evidence = self.validate("axolotl", "pet", positive)
        assert evidence is not None
        pupil_measurements = evidence["gate_results"][
            "rigid_pupil_translation"
        ]["measurements"]
        expected_translations = [[0, 0], [1, 0], [0, 1], [1, 1]]
        self.assertEqual(
            pupil_measurements["pupil_translation"]["left"],
            expected_translations,
        )
        self.assertEqual(
            pupil_measurements["pupil_translation"]["right"],
            expected_translations,
        )

        left_ring = ring4(10, 58)
        right_ring = ring4(22, 59)
        ring_base = (
            ax_tail_body()
            | left_ring
            | right_ring
            | AX_DORSAL_GILL
            | AX_MIDDLE_GILL
            | AX_VENTRAL_GILL
        )
        ring_redraw = (
            ax_tail_body()
            | same_centroid_ring_redraw(left_ring, 10, 58)
            | same_centroid_ring_redraw(right_ring, 22, 59)
            | AX_DORSAL_GILL
            | AX_MIDDLE_GILL
            | AX_VENTRAL_GILL
        )
        self.assert_reason(
            "AX_PET_NONRIGID_PUPIL_TRANSFORM",
            "axolotl",
            "pet",
            make_frames(
                "axolotl", "pet", [ring_base, ring_redraw, ring_base, ring_base]
            ),
        )

        fractional = ax_pet_frames()
        fractional_masks = [set(frame.mask) for frame in fractional]
        fractional_masks[1].difference_update(LEFT_PUPIL | RIGHT_PUPIL)
        fractional_masks[1].update(
            (LEFT_PUPIL - {(10, 60)})
            | {(13, 63)}
            | (RIGHT_PUPIL - {(22, 61)})
            | {(25, 64)}
        )
        fractional_frames = replace_masks(
            "axolotl", "pet", fractional, fractional_masks
        )
        self.assert_reason(
            "AX_PET_NONRIGID_PUPIL_TRANSFORM",
            "axolotl",
            "pet",
            fractional_frames,
        )

        delta_one = ax_pet_frames()
        delta_one_masks = [set(frame.mask) for frame in delta_one]
        delta_one_masks[1].difference_update(LEFT_PUPIL)
        delta_one_masks[1].update({(x + 1, y) for x, y in LEFT_PUPIL})
        self.assert_reason(
            "AX_PET_NONRIGID_PUPIL_TRANSFORM",
            "axolotl",
            "pet",
            replace_masks("axolotl", "pet", delta_one, delta_one_masks),
        )

        combined_masks = [set(frame.mask) for frame in fractional_frames]
        combined_masks[1].update(AX_VENTRAL_GILL)
        self.assert_reason(
            "AX_PET_NONRIGID_PUPIL_TRANSFORM",
            "axolotl",
            "pet",
            replace_masks("axolotl", "pet", fractional_frames, combined_masks),
        )

    def test_axolotl_pet_rejects_pupil_window_boundary_escape(self) -> None:
        cases = {
            "left-border": {(9, 62), (8, 62)},
            "right-border": {(25, 63), (26, 63), (27, 63)},
            "contained-tiny-speck": {(14, 66)},
            "contained-oversized-residue": rectangle(10, 64, 14, 66),
        }
        for label, escaped_pixels in cases.items():
            with self.subTest(boundary=label):
                frames = ax_pet_frames()
                masks = [
                    set(frame.mask) | escaped_pixels for frame in frames
                ]
                self.assert_reason(
                    "AX_PET_NONRIGID_PUPIL_TRANSFORM",
                    "axolotl",
                    "pet",
                    replace_masks("axolotl", "pet", frames, masks),
                )

    def test_axolotl_pet_gill_trajectory_counts_source_ghosts_and_tolerance(self) -> None:
        positive = ax_pet_unique_frames()

        tolerated_masks = [set(frame.mask) for frame in positive]
        tolerated_masks[1].update({(8, 57), (8, 58)})
        tolerated = replace_masks(
            "axolotl", "pet", positive, tolerated_masks
        )
        evidence = self.validate("axolotl", "pet", tolerated)
        assert evidence is not None
        ventral_phase = evidence["gate_results"][
            "gill_base_shared_transform"
        ]["measurements"]["phases"][1]
        self.assertEqual(ventral_phase["template_xor_pixels"]["ventral_base"], 2)
        self.assertEqual(ventral_phase["source_residue_pixels"]["ventral_base"], 2)

        ghost_masks = [set(frame.mask) for frame in positive]
        ghost_masks[1].update(AX_VENTRAL_GILL)
        self.assert_reason(
            "AX_PET_GILL_BASE_DRIFT",
            "axolotl",
            "pet",
            replace_masks("axolotl", "pet", positive, ghost_masks),
        )

    def test_rabbit_idle_rejects_residual_split_nose_and_wrong_cadence(self) -> None:
        frames = rabbit_idle_frames()

        residual_masks = [set(frame.mask) for frame in frames]
        residual_masks[2].add((0, 0))
        residual = replace_masks("rabbit", "idle", frames, residual_masks)
        self.assert_reason(
            "RABBIT_IDLE_PHASE_RESIDUAL", "rabbit", "idle", residual
        )

        nose_masks = [set(frame.mask) for frame in frames]
        nose_masks[1].add((14, 53))
        filled_cleft = replace_masks("rabbit", "idle", frames, nose_masks)
        self.assert_reason(
            "RABBIT_IDLE_SPLIT_NOSE_TOPOLOGY",
            "rabbit",
            "idle",
            filled_cleft,
        )

        self.assert_reason(
            "RABBIT_IDLE_CADENCE",
            "rabbit",
            "idle",
            frames,
            [220, 220, 220, 220],
        )

    def test_rabbit_idle_missing_actual_cadence_fails_before_gate_evaluation(self) -> None:
        frames = rabbit_idle_frames()
        with self.assertRaisesRegex(
            contract.RasterContractError, r"requires actual role cadence"
        ):
            contract.validate_species_role_visual_gates(
                self.policy,
                "rabbit",
                self.policy.identity_frame_sha256["rabbit"],
                ROLES["idle"],
                frames,
                None,
            )

    def test_rabbit_blink_rejects_mass_lid_geometry_and_centroid_drift(self) -> None:
        mass_masks = rabbit_blink_masks()
        mass_masks[3] = rectangle(20, 47, 24, 50)
        mass = make_frames("rabbit", "blink", mass_masks)
        self.assert_reason(
            "RABBIT_BLINK_EYE_MASS_SEQUENCE", "rabbit", "blink", mass
        )

        lid_masks = rabbit_blink_masks()
        lid_masks[2] = {(20, 49), (21, 49), (22, 49), (23, 49), (24, 50)}
        lid = make_frames("rabbit", "blink", lid_masks)
        self.assert_reason("RABBIT_BLINK_LID_GEOMETRY", "rabbit", "blink", lid)

        centroid_masks = rabbit_blink_masks()
        centroid_masks[3] = {(x + 3, y) for x, y in centroid_masks[3]}
        centroid = make_frames("rabbit", "blink", centroid_masks)
        self.assert_reason(
            "RABBIT_BLINK_EYE_CENTROID_DRIFT", "rabbit", "blink", centroid
        )

    def test_rabbit_blink_rejects_every_lid_containment_escape(self) -> None:
        top_masks = [
            rectangle(20, 43, 24, 46),
            rectangle(20, 44, 24, 45),
            rectangle(20, 43, 24, 43) | rectangle(20, 42, 24, 42),
            rectangle(20, 43, 23, 46),
        ]
        bottom_masks = [
            rectangle(20, 52, 24, 55),
            rectangle(20, 53, 24, 54),
            rectangle(20, 55, 24, 55) | rectangle(20, 56, 24, 56),
            rectangle(20, 52, 23, 55),
        ]
        split_masks = rabbit_blink_masks()
        split_masks[2] |= {(17, 49), (17, 50)}
        for label, masks in {
            "top": top_masks,
            "bottom": bottom_masks,
            "component-split": split_masks,
        }.items():
            with self.subTest(escape=label):
                self.assert_reason(
                    "RABBIT_BLINK_LID_GEOMETRY",
                    "rabbit",
                    "blink",
                    make_frames("rabbit", "blink", masks),
                )

    def test_rabbit_blink_combined_reasons_follow_catalog_order(self) -> None:
        masks = rabbit_blink_masks()
        masks[2] |= {(17, 49), (17, 50)}
        masks[3] = rectangle(23, 47, 27, 50)
        self.assert_reasons(
            (
                "RABBIT_BLINK_EYE_MASS_SEQUENCE",
                "RABBIT_BLINK_LID_GEOMETRY",
                "RABBIT_BLINK_EYE_CENTROID_DRIFT",
            ),
            "rabbit",
            "blink",
            make_frames("rabbit", "blink", masks),
        )

    def test_rabbit_listen_rejects_skull_ear_base_and_local_scale_bounds(self) -> None:
        frames = rabbit_listen_frames()
        base_mask = set(frames[0].mask)

        skull_masks = [set(frame.mask) for frame in frames]
        skull_points = first_points(
            base_mask & rectangle(8, 43, 35, 60), 41
        )
        skull_masks[1].difference_update(skull_points)
        skull = replace_masks("rabbit", "listen", frames, skull_masks)
        self.assert_reason(
            "RABBIT_LISTEN_SKULL_FREEZE", "rabbit", "listen", skull
        )

        ear_masks = [set(frame.mask) for frame in frames]
        ear_masks[1].difference_update({(x, 29) for x in range(13, 18)})
        ear = replace_masks("rabbit", "listen", frames, ear_masks)
        self.assert_reason(
            "RABBIT_LISTEN_EAR_BASE_FREEZE", "rabbit", "listen", ear
        )

        local_masks = [set(frame.mask) for frame in frames]
        local_masks[1].add((9, 20))
        local = replace_masks("rabbit", "listen", frames, local_masks)
        self.assert_reason(
            "RABBIT_LISTEN_LOCAL_SCALE_INK", "rabbit", "listen", local
        )

    def test_rabbit_listen_tracks_full_component_across_left_top_roi_borders(self) -> None:
        base_mask = rectangle(10, 20, 40, 60)
        cases = {
            "x=6": (
                base_mask | {(7, 20), (8, 20), (9, 20)},
                {(6, 20)},
            ),
            "y=5": (
                base_mask | {(10, y) for y in range(6, 20)},
                {(10, 5)},
            ),
        }
        for label, (baseline, escaped) in cases.items():
            with self.subTest(boundary=label):
                masks = [set(baseline) for _phase in range(4)]
                masks[1].update(escaped)
                self.assert_reason(
                    "RABBIT_LISTEN_LOCAL_SCALE_INK",
                    "rabbit",
                    "listen",
                    make_frames("rabbit", "listen", masks),
                )

        in_window = base_mask | {(7, 20), (8, 20), (9, 20)}
        evidence = self.validate(
            "rabbit",
            "listen",
            make_frames("rabbit", "listen", [in_window] * 4),
        )
        self.assertIsNotNone(evidence)

        legitimate_secondary = in_window | rectangle(42, 10, 43, 11)
        evidence = self.validate(
            "rabbit",
            "listen",
            make_frames(
                "rabbit", "listen", [legitimate_secondary] * 4
            ),
        )
        self.assertIsNotNone(evidence)

    def test_rabbit_listen_local_ratio_cannot_be_diluted_by_outside_body_ink(self) -> None:
        # Only ten baseline pixels of this large connected component are in
        # the policy ROI; the 663-pixel body below it is deliberately outside.
        # Five/six/seven local pixels are therefore 50/60/70% local growth,
        # even though each whole-component ratio remains close to 1.0.
        baseline = rectangle(10, 65, 60, 77) | {
            (10, y) for y in range(55, 65)
        }
        masks = [
            baseline,
            baseline | {(x, 60) for x in range(11, 16)},
            baseline | {(x, 60) for x in range(11, 17)},
            baseline | {(x, 60) for x in range(11, 18)},
        ]
        self.assert_reason(
            "RABBIT_LISTEN_LOCAL_SCALE_INK",
            "rabbit",
            "listen",
            make_frames("rabbit", "listen", masks),
        )

        ring = (
            rectangle(10, 20, 40, 20)
            | rectangle(10, 60, 40, 60)
            | rectangle(10, 20, 10, 60)
            | rectangle(40, 20, 40, 60)
        )
        detached_masks = [set(ring) for _phase in range(4)]
        detached_masks[1].update({(x, 25) for x in range(15, 20)})
        self.assert_reason(
            "RABBIT_LISTEN_LOCAL_SCALE_INK",
            "rabbit",
            "listen",
            make_frames("rabbit", "listen", detached_masks),
        )

    def test_multi_gate_hostiles_report_complete_deterministic_reason_tuples(self) -> None:
        ax_idle = ax_idle_frames(p3_gill_pixels=17)
        ax_idle_masks = [set(frame.mask) for frame in ax_idle]
        ax_idle_masks[1].update({(x, 0) for x in range(5)})
        self.assert_reasons(
            ("AX_IDLE_GILL_PHASE_LOCALITY", "AX_IDLE_LOOP_SEAM"),
            "axolotl",
            "idle",
            replace_masks("axolotl", "idle", ax_idle, ax_idle_masks),
        )

        ax_blink = ax_blink_frames(p3_left=2, p3_right=2)
        ax_blink_masks = [set(frame.mask) for frame in ax_blink]
        ax_blink_masks[1].add((0, 0))
        self.assert_reasons(
            ("AX_BLINK_PUPIL_ONLY", "AX_BLINK_PER_EYE_OCCUPANCY"),
            "axolotl",
            "blink",
            replace_masks("axolotl", "blink", ax_blink, ax_blink_masks),
        )

        ax_pet = ax_pet_frames()
        ax_pet_masks = [set(frame.mask) for frame in ax_pet]
        ax_pet_masks[1].update(rectangle(40, 2, 62, 8))
        ax_pet_masks[1].difference_update(LEFT_PUPIL | AX_DORSAL_GILL)
        ax_pet_masks[1].update({(x + 2, y) for x, y in LEFT_PUPIL})
        ax_pet_masks[1].update({(15, 46), (16, 46), (16, 47)})
        self.assert_reasons(
            (
                "AX_PET_REDRAW_BUDGET",
                "AX_PET_NONRIGID_PUPIL_TRANSFORM",
            ),
            "axolotl",
            "pet",
            replace_masks("axolotl", "pet", ax_pet, ax_pet_masks),
        )

        rabbit_idle = rabbit_idle_frames()
        rabbit_idle_masks = [set(frame.mask) for frame in rabbit_idle]
        rabbit_idle_masks[2].add((0, 0))
        self.assert_reasons(
            ("RABBIT_IDLE_PHASE_RESIDUAL", "RABBIT_IDLE_CADENCE"),
            "rabbit",
            "idle",
            replace_masks("rabbit", "idle", rabbit_idle, rabbit_idle_masks),
            [220, 220, 220, 220],
        )

        rabbit_blink_masks_combined = rabbit_blink_masks()
        rabbit_blink_masks_combined[2] = {
            (20, 49),
            (21, 49),
            (22, 49),
            (23, 49),
            (24, 50),
        }
        rabbit_blink_masks_combined[3] = rectangle(20, 47, 24, 50)
        self.assert_reasons(
            (
                "RABBIT_BLINK_EYE_MASS_SEQUENCE",
                "RABBIT_BLINK_LID_GEOMETRY",
            ),
            "rabbit",
            "blink",
            make_frames("rabbit", "blink", rabbit_blink_masks_combined),
        )

        rabbit_listen = rabbit_listen_frames()
        rabbit_listen_masks = [set(frame.mask) for frame in rabbit_listen]
        listen_base = set(rabbit_listen[0].mask)
        rabbit_listen_masks[1].difference_update(
            first_points(listen_base & rectangle(8, 43, 35, 60), 41)
        )
        rabbit_listen_masks[1].difference_update(
            {(x, 29) for x in range(13, 18)}
        )
        rabbit_listen_masks[1].add((9, 20))
        self.assert_reasons(
            (
                "RABBIT_LISTEN_SKULL_FREEZE",
                "RABBIT_LISTEN_EAR_BASE_FREEZE",
                "RABBIT_LISTEN_LOCAL_SCALE_INK",
            ),
            "rabbit",
            "listen",
            replace_masks(
                "rabbit", "listen", rabbit_listen, rabbit_listen_masks
            ),
        )

    def test_unconfigured_ferret_blink_is_an_exact_noop(self) -> None:
        masks = [
            rectangle(20 + phase, 30, 25 + phase, 40)
            for phase in range(4)
        ]
        frames = make_frames("ferret", "blink", masks)
        before = [frame.packed for frame in frames]
        result = contract.validate_species_role_visual_gates(
            self.policy,
            "ferret",
            "0" * 64,
            ROLES["blink"],
            frames,
            None,
        )
        self.assertIsNone(result)
        self.assertEqual([frame.packed for frame in frames], before)
        self.assertNotIn(("ferret", "blink"), self.policy.entries)

    def test_identity_hash_drift_is_rejected_before_roi_failure(self) -> None:
        frames = ax_idle_frames()
        hostile_masks = [set(frame.mask) for frame in frames]
        hostile_masks[1].update({(x, 0) for x in range(5)})
        hostile = replace_masks("axolotl", "idle", frames, hostile_masks)
        with self.assertRaises(contract.RasterContractError) as raised:
            contract.validate_species_role_visual_gates(
                self.policy,
                "axolotl",
                "0" * 64,
                ROLES["idle"],
                hostile,
                ROLES["idle"].durations_ms,
            )
        self.assertNotIsInstance(
            raised.exception, contract.SpeciesRoleVisualGateError
        )
        self.assertRegex(str(raised.exception), r"identity-frame SHA-256")

    def test_policy_rejects_unknown_protected_duplicate_and_reordered_scope(self) -> None:
        for identity_key, role_name, pattern in (
            ("cat", "idle", r"cannot configure protected cat/idle"),
            ("dog", "idle", r"cannot configure protected dog/idle"),
            ("fox", "idle", r"cannot configure protected fox/idle"),
            ("ferret", "blink", r"cannot configure protected ferret/blink"),
            ("red-panda", "idle", r"contains unapproved red-panda/idle"),
        ):
            with self.subTest(identity_key=identity_key, role=role_name):
                payload = self.canonical_payload()
                payload["entries"][0]["identity_key"] = identity_key
                payload["entries"][0]["role"] = role_name
                with self.assertRaisesRegex(contract.RasterContractError, pattern):
                    self.load_payload(payload)

        duplicate = self.canonical_payload()
        duplicate["entries"][1] = copy.deepcopy(duplicate["entries"][0])
        with self.assertRaisesRegex(contract.RasterContractError, r"duplicates axolotl/idle"):
            self.load_payload(duplicate)

        missing = self.canonical_payload()
        missing["entries"].pop()
        with self.assertRaisesRegex(contract.RasterContractError, r"exact six configured entries"):
            self.load_payload(missing)

        reordered = self.canonical_payload()
        reordered["entries"][0], reordered["entries"][1] = (
            reordered["entries"][1],
            reordered["entries"][0],
        )
        with self.assertRaisesRegex(contract.RasterContractError, r"exact ordered allow-list"):
            self.load_payload(reordered)

    def test_policy_rejects_duplicate_json_keys_and_malformed_fields(self) -> None:
        with self.assertRaisesRegex(contract.RasterContractError, r"duplicates JSON key 'schema'"):
            self.load_raw('{"schema":"first","schema":"second"}')

        malformed_payloads: list[tuple[str, dict[str, object], str]] = []

        unknown = self.canonical_payload()
        unknown["unexpected"] = True
        malformed_payloads.append(("unknown-field", unknown, r"unexpected=\['unexpected'\]"))

        connectivity = self.canonical_payload()
        connectivity["connectivity"] = 4
        malformed_payloads.append(("four-neighbor", connectivity, r"eight-neighbor connectivity"))

        roi = self.canonical_payload()
        roi["entries"][0]["gates"]["required_8_connected_anchors"]["anchors"]["body"] = [30, 60, 64, 71]
        malformed_payloads.append(("out-of-canvas-roi", roi, r"leaves the 64x80 canvas"))

        boolean_budget = self.canonical_payload()
        boolean_budget["entries"][0]["gates"]["loop_seam"]["maximum_xor_pixels"] = True
        malformed_payloads.append(("boolean-budget", boolean_budget, r"integer must be"))

        nonshared_pupils = self.canonical_payload()
        nonshared_pupils["entries"][2]["gates"]["rigid_pupil_translation"][
            "maximum_inter_eye_translation_delta"
        ] = [1, 0]
        malformed_payloads.append(
            (
                "nonshared-pupil-policy",
                nonshared_pupils,
                r"one exact shared integer vector",
            )
        )

        unguarded_eye = self.canonical_payload()
        unguarded_eye["entries"][4]["gates"]["eye_sequence"][
            "containment_rect"
        ] = [18, 43, 28, 55]
        malformed_payloads.append(
            (
                "unguarded-eye-roi",
                unguarded_eye,
                r"must guard every eye-ROI border",
            )
        )

        for name, payload, pattern in malformed_payloads:
            with self.subTest(case=name):
                with self.assertRaisesRegex(contract.RasterContractError, pattern):
                    self.load_payload(payload)

    def test_loaded_policy_rejects_nested_mutation_and_source_sha_substitution(self) -> None:
        mutated = contract.load_species_role_visual_gate_policy(POLICY_PATH)
        mutated.entries[("axolotl", "idle")].gates["loop_seam"][
            "maximum_xor_pixels"
        ] += 1
        with self.assertRaisesRegex(contract.RasterContractError, r"entry axolotl/idle drifted"):
            contract.species_role_visual_gate_policy_record(mutated)

        substituted = replace(
            contract.load_species_role_visual_gate_policy(POLICY_PATH),
            source_sha256="0" * 64,
        )
        with self.assertRaisesRegex(contract.RasterContractError, r"policy object drifted"):
            contract.species_role_visual_gate_policy_record(substituted)

        bogus_source = b"{}"
        substituted_source = replace(
            contract.load_species_role_visual_gate_policy(POLICY_PATH),
            source_bytes=bogus_source,
            source_sha256=hashlib.sha256(bogus_source).hexdigest(),
        )
        with self.assertRaisesRegex(
            contract.RasterContractError, r"source document drifted"
        ):
            contract.species_role_visual_gate_policy_record(
                substituted_source
            )

    def test_json_gate_key_reorder_preserves_semantics_but_changes_raw_provenance(self) -> None:
        canonical = contract.load_species_role_visual_gate_policy(POLICY_PATH)
        payload = self.canonical_payload()
        pet_record = next(
            record
            for record in payload["entries"]
            if record["identity_key"] == "axolotl" and record["role"] == "pet"
        )
        gate_items = list(pet_record["gates"].items())
        pet_record["gates"] = dict(reversed(gate_items))
        reordered = self.load_payload(payload)

        key = ("axolotl", "pet")
        self.assertEqual(
            canonical.entries[key].entry_sha256,
            reordered.entries[key].entry_sha256,
        )
        self.assertNotEqual(canonical.source_sha256, reordered.source_sha256)

        frames = ax_pet_frames()
        canonical_evidence = self.validate(
            "axolotl", "pet", frames, policy=canonical
        )
        reordered_evidence = self.validate(
            "axolotl", "pet", frames, policy=reordered
        )
        assert canonical_evidence is not None and reordered_evidence is not None
        self.assertEqual(
            canonical_evidence["gate_results"],
            reordered_evidence["gate_results"],
        )
        self.assertEqual(
            list(canonical_evidence["gate_results"]),
            list(reordered_evidence["gate_results"]),
        )
        self.assertEqual(
            canonical_evidence["policy_entry_sha256"],
            reordered_evidence["policy_entry_sha256"],
        )
        self.assertNotEqual(
            canonical_evidence["policy_sha256"],
            reordered_evidence["policy_sha256"],
        )
        self.assertNotEqual(
            contract.species_role_visual_gate_evidence_sha256(canonical_evidence),
            contract.species_role_visual_gate_evidence_sha256(reordered_evidence),
        )

    def test_sync_constants_match_core_policy_and_normalized_reason_catalogs(self) -> None:
        sync_policy = self.sync_policy()
        self.assertEqual(
            contract.SPECIES_ROLE_VISUAL_GATE_POLICY_SCHEMA,
            portrait_sync.SPECIES_ROLE_VISUAL_GATE_POLICY_SCHEMA,
        )
        self.assertEqual(
            contract.SPECIES_ROLE_VISUAL_GATE_EVIDENCE_SCHEMA,
            portrait_sync.SPECIES_ROLE_VISUAL_GATE_EVIDENCE_SCHEMA,
        )
        self.assertEqual(
            contract.SPECIES_ROLE_VISUAL_GATE_POLICY_RELATIVE_PATH,
            portrait_sync.SPECIES_ROLE_VISUAL_GATE_POLICY_RELATIVE_PATH,
        )
        self.assertEqual(
            contract.SPECIES_ROLE_VISUAL_GATE_KEYS,
            portrait_sync.SPECIES_ROLE_VISUAL_GATE_KEYS,
        )

        core_gate_kinds = {
            key: set(entry.gates) for key, entry in self.policy.entries.items()
        }
        self.assertEqual(
            core_gate_kinds,
            portrait_sync.SPECIES_ROLE_VISUAL_GATE_KIND_SETS,
        )
        self.assertEqual(
            self.policy.identity_frame_sha256,
            portrait_sync.SPECIES_ROLE_VISUAL_GATE_IDENTITY_FRAME_SHA256,
        )
        self.assertEqual(
            {
                key: entry.entry_sha256
                for key, entry in self.policy.entries.items()
            },
            portrait_sync.SPECIES_ROLE_VISUAL_GATE_ENTRY_SHA256,
        )
        self.assertEqual(
            self.policy.identity_frame_sha256,
            sync_policy.identity_frame_sha256,
        )
        self.assertEqual(
            {
                key: entry.entry_sha256
                for key, entry in self.policy.entries.items()
            },
            sync_policy.entry_sha256,
        )

        normalized_core_reasons: dict[tuple[str, str, str], list[str]] = {}
        for key in contract.SPECIES_ROLE_VISUAL_GATE_KEYS:
            frames, durations = positive_fixture(key)
            evidence = self.validate(key[0], key[1], frames, durations)
            assert evidence is not None
            for kind, result in evidence["gate_results"].items():
                normalized_core_reasons[(key[0], key[1], kind)] = list(
                    result["possible_failure_reason_codes"]
                )
        self.assertEqual(
            normalized_core_reasons,
            portrait_sync.SPECIES_ROLE_VISUAL_GATE_REASON_CODES,
        )

    def test_sync_loads_canonical_policy_and_accepts_minimal_v8_header(self) -> None:
        sync_policy = self.sync_policy()
        self.assertEqual(sync_policy.source_sha256, self.policy.source_sha256)
        self.assertEqual(
            sync_policy.provenance_record,
            contract.species_role_visual_gate_policy_record(self.policy),
        )
        self.assertEqual(
            tuple(sync_policy.entries),
            portrait_sync.SPECIES_ROLE_VISUAL_GATE_KEYS,
        )

        manifest = self.minimal_manifest_header(policy=sync_policy)
        self.assertEqual(
            set(manifest),
            {
                "animation_contract",
                "complete_roster",
                "identity_lock_schema",
                "non_destructive_build",
                "raster_contract",
                "schema",
                "species_role_visual_gate_policy",
            },
        )
        authenticated = portrait_sync.require_fail_closed_manifest(
            manifest, ROOT
        )
        self.assertIsNotNone(authenticated)
        assert authenticated is not None
        self.assertEqual(
            authenticated.provenance_record,
            sync_policy.provenance_record,
        )

    def test_v8_manifest_rejects_missing_or_drifted_policy(self) -> None:
        missing_record = self.minimal_manifest_header()
        missing_record.pop("species_role_visual_gate_policy")
        with self.assertRaisesRegex(
            ValueError, r"species_role_visual_gate_policy must be an object"
        ):
            portrait_sync.require_fail_closed_manifest(missing_record, ROOT)

        drifted_record = self.minimal_manifest_header()
        drifted_record["species_role_visual_gate_policy"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(
            ValueError, r"policy provenance differs from the canonical file"
        ):
            portrait_sync.require_fail_closed_manifest(drifted_record, ROOT)

        with tempfile.TemporaryDirectory() as temporary:
            empty_root = Path(temporary)
            with self.assertRaisesRegex(
                ValueError, r"cannot read canonical visual gate policy"
            ):
                portrait_sync.require_fail_closed_manifest(
                    self.minimal_manifest_header(), empty_root
                )

        with tempfile.TemporaryDirectory() as temporary:
            drifted_root = Path(temporary)
            policy_path = (
                drifted_root
                / portrait_sync.SPECIES_ROLE_VISUAL_GATE_POLICY_RELATIVE_PATH
            )
            policy_path.parent.mkdir(parents=True)
            payload = self.canonical_payload()
            payload["entries"][0]["gates"]["loop_seam"][
                "maximum_xor_pixels"
            ] += 1
            policy_path.write_text(
                json.dumps(payload, indent=2) + "\n",
                encoding="utf-8",
                newline="\n",
            )
            with self.assertRaisesRegex(
                ValueError, r"axolotl/idle content drifted"
            ):
                portrait_sync.require_fail_closed_manifest(
                    self.minimal_manifest_header(), drifted_root
                )

    def test_v6_and_v7_manifests_reject_reserved_v8_fields(self) -> None:
        for schema in (
            portrait_sync.LEGACY_MANIFEST_SCHEMA,
            portrait_sync.MANIFEST_SCHEMA,
        ):
            with self.subTest(schema=schema, location="top-level"):
                top_level = self.minimal_manifest_header(schema)
                top_level["species_role_visual_gate_policy"] = (
                    self.sync_policy().provenance_record
                )
                with self.assertRaisesRegex(
                    ValueError, r"cannot contain reserved v8 visual-gate fields"
                ):
                    portrait_sync.require_fail_closed_manifest(top_level, ROOT)

            with self.subTest(schema=schema, location="animation-contract"):
                animation = self.minimal_manifest_header(schema)
                animation["animation_contract"][
                    "species_role_visual_gate_policy_schema"
                ] = portrait_sync.SPECIES_ROLE_VISUAL_GATE_POLICY_SCHEMA
                with self.assertRaisesRegex(
                    ValueError, r"cannot contain reserved v8 visual-gate fields"
                ):
                    portrait_sync.require_fail_closed_manifest(animation, ROOT)

    def test_sync_accepts_synthetic_evidence_for_all_configured_roles(self) -> None:
        sync_policy = self.sync_policy()
        for key in portrait_sync.SPECIES_ROLE_VISUAL_GATE_KEYS:
            with self.subTest(role_key="/".join(key)):
                role_record, identity_lock, evidence = (
                    self.synthetic_role_handoff(key)
                )
                self.assertEqual(
                    contract.species_role_visual_gate_evidence_sha256(evidence),
                    portrait_sync.canonical_json_sha256(evidence),
                )
                portrait_sync.require_species_role_visual_gate_evidence(
                    portrait_sync.VISUAL_GATE_MANIFEST_SCHEMA,
                    key[0],
                    role_record,
                    identity_lock,
                    sync_policy,
                )

    def test_sync_rejects_every_configured_evidence_binding_drift(self) -> None:
        sync_policy = self.sync_policy()
        schema = portrait_sync.VISUAL_GATE_MANIFEST_SCHEMA
        key = ("axolotl", "idle")

        for missing_field in (
            "species_role_visual_gate",
            "species_role_visual_gate_sha256",
        ):
            with self.subTest(drift="missing", field=missing_field):
                role_record, identity_lock, _evidence = (
                    self.synthetic_role_handoff(key)
                )
                role_record.pop(missing_field)
                with self.assertRaisesRegex(
                    ValueError, r"and its SHA-256 are required"
                ):
                    portrait_sync.require_species_role_visual_gate_evidence(
                        schema, key[0], role_record, identity_lock, sync_policy
                    )

        role_record, identity_lock, _evidence = self.synthetic_role_handoff(key)
        role_record["species_role_visual_gate_sha256"] = "0" * 64
        with self.assertRaisesRegex(
            ValueError, r"SHA-256 does not match its canonical record"
        ):
            portrait_sync.require_species_role_visual_gate_evidence(
                schema, key[0], role_record, identity_lock, sync_policy
            )

        role_record, identity_lock, _evidence = self.synthetic_role_handoff(key)
        role_record["species_role_visual_gate"]["policy_entry_sha256"] = "0" * 64
        self.rehash_role_evidence(role_record)
        with self.assertRaisesRegex(
            ValueError, r"provenance or identity binding drifted"
        ):
            portrait_sync.require_species_role_visual_gate_evidence(
                schema, key[0], role_record, identity_lock, sync_policy
            )

        for source in ("evidence", "identity-lock"):
            with self.subTest(drift="identity", source=source):
                role_record, identity_lock, _evidence = (
                    self.synthetic_role_handoff(key)
                )
                if source == "evidence":
                    role_record["species_role_visual_gate"][
                        "identity_frame_sha256"
                    ] = "0" * 64
                    self.rehash_role_evidence(role_record)
                else:
                    identity_lock["identity_frame_sha256"] = "0" * 64
                with self.assertRaisesRegex(
                    ValueError, r"provenance or identity binding drifted"
                ):
                    portrait_sync.require_species_role_visual_gate_evidence(
                        schema, key[0], role_record, identity_lock, sync_policy
                    )

        role_record, identity_lock, _evidence = self.synthetic_role_handoff(key)
        role_record["frame_sha256"][0] = "0" * 64
        with self.assertRaisesRegex(
            ValueError, r"frame hashes differ from the containing role"
        ):
            portrait_sync.require_species_role_visual_gate_evidence(
                schema, key[0], role_record, identity_lock, sync_policy
            )

        role_record, identity_lock, _evidence = self.synthetic_role_handoff(key)
        role_record["durations_ms"][0] += 1
        with self.assertRaisesRegex(
            ValueError, r"cadence differs from the containing role"
        ):
            portrait_sync.require_species_role_visual_gate_evidence(
                schema, key[0], role_record, identity_lock, sync_policy
            )

        rabbit_key = ("rabbit", "idle")
        role_record, identity_lock, _evidence = self.synthetic_role_handoff(
            rabbit_key
        )
        role_record["durations_ms"][0] += 1
        role_record["species_role_visual_gate"]["durations_ms"][0] += 1
        self.rehash_role_evidence(role_record)
        with self.assertRaisesRegex(
            ValueError, r"cadence differs from the canonical RoleSpec"
        ):
            portrait_sync.require_species_role_visual_gate_evidence(
                schema,
                rabbit_key[0],
                role_record,
                identity_lock,
                sync_policy,
            )

        role_record, identity_lock, _evidence = self.synthetic_role_handoff(key)
        role_record["species_role_visual_gate"]["gate_results"].pop(
            "loop_seam"
        )
        self.rehash_role_evidence(role_record)
        with self.assertRaisesRegex(
            ValueError, r"gate result set differs from the policy entry"
        ):
            portrait_sync.require_species_role_visual_gate_evidence(
                schema, key[0], role_record, identity_lock, sync_policy
            )

        role_record, identity_lock, _evidence = self.synthetic_role_handoff(key)
        role_record["species_role_visual_gate"]["status"] = "fail"
        self.rehash_role_evidence(role_record)
        with self.assertRaisesRegex(
            ValueError, r"provenance or identity binding drifted"
        ):
            portrait_sync.require_species_role_visual_gate_evidence(
                schema, key[0], role_record, identity_lock, sync_policy
            )

        role_record, identity_lock, _evidence = self.synthetic_role_handoff(key)
        role_record["species_role_visual_gate"]["gate_results"]["loop_seam"][
            "status"
        ] = "fail"
        self.rehash_role_evidence(role_record)
        with self.assertRaisesRegex(
            ValueError, r"loop_seam is not a canonical passing result"
        ):
            portrait_sync.require_species_role_visual_gate_evidence(
                schema, key[0], role_record, identity_lock, sync_policy
            )

        blink_key = ("rabbit", "blink")
        role_record, identity_lock, _evidence = self.synthetic_role_handoff(
            blink_key
        )
        role_record["species_role_visual_gate"]["gate_results"]["eye_sequence"][
            "possible_failure_reason_codes"
        ] = ["RABBIT_BLINK_EYE_MASS_SEQUENCE"]
        self.rehash_role_evidence(role_record)
        with self.assertRaisesRegex(
            ValueError, r"eye_sequence is not a canonical passing result"
        ):
            portrait_sync.require_species_role_visual_gate_evidence(
                schema,
                blink_key[0],
                role_record,
                identity_lock,
                sync_policy,
            )

        role_record, identity_lock, _evidence = self.synthetic_role_handoff(key)
        with self.assertRaisesRegex(
            ValueError, r"v8 visual gate policy was not authenticated"
        ):
            portrait_sync.require_species_role_visual_gate_evidence(
                schema, key[0], role_record, identity_lock, None
            )

    def test_unconfigured_ferret_and_old_manifests_reject_reserved_evidence(self) -> None:
        sync_policy = self.sync_policy()
        ferret_record: dict[str, object] = {"role": "blink"}
        portrait_sync.require_species_role_visual_gate_evidence(
            portrait_sync.VISUAL_GATE_MANIFEST_SCHEMA,
            "ferret",
            ferret_record,
            {"identity_frame_sha256": "0" * 64},
            sync_policy,
        )
        self.assertNotIn("species_role_visual_gate", ferret_record)
        self.assertNotIn("species_role_visual_gate_sha256", ferret_record)

        configured_record, _identity_lock, _evidence = (
            self.synthetic_role_handoff(("axolotl", "blink"))
        )
        forbidden_ferret = copy.deepcopy(configured_record)
        forbidden_ferret["role"] = "blink"
        with self.assertRaisesRegex(
            ValueError, r"visual evidence is forbidden for an unconfigured role"
        ):
            portrait_sync.require_species_role_visual_gate_evidence(
                portrait_sync.VISUAL_GATE_MANIFEST_SCHEMA,
                "ferret",
                forbidden_ferret,
                {"identity_frame_sha256": "0" * 64},
                sync_policy,
            )

        role_record, identity_lock, _evidence = self.synthetic_role_handoff(
            ("axolotl", "idle")
        )
        for legacy_schema in (
            portrait_sync.LEGACY_MANIFEST_SCHEMA,
            portrait_sync.MANIFEST_SCHEMA,
        ):
            with self.subTest(schema=legacy_schema):
                with self.assertRaisesRegex(
                    ValueError, r"v6/v7 cannot contain reserved v8 visual evidence"
                ):
                    portrait_sync.require_species_role_visual_gate_evidence(
                        legacy_schema,
                        "axolotl",
                        copy.deepcopy(role_record),
                        copy.deepcopy(identity_lock),
                        sync_policy,
                    )

    def test_build_species_attaches_only_configured_axolotl_evidence(self) -> None:
        policy, identity_lock, raster = (
            self.synthetic_axolotl_builder_fixture()
        )
        identity_frame_sha256 = hashlib.sha256(
            raster.identity.packed
        ).hexdigest()

        def write_synthetic_contact(
            _masks: object,
            path: Path,
            _width: int,
            _height: int,
        ) -> None:
            path.write_bytes(b"synthetic-contact-sheet")

        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            source_dir = temporary_root / "source"
            private_output = temporary_root / "private"
            with (
                mock.patch.object(
                    wild.raster_contract,
                    "load_high_res_species",
                    return_value=raster,
                ) as load_raster,
                mock.patch.object(
                    wild.base,
                    "write_role_gif",
                ) as write_role_gif,
                mock.patch.object(
                    wild.base,
                    "write_contact_sheet",
                    side_effect=write_synthetic_contact,
                ) as write_contact_sheet,
            ):
                result = wild.build_species(
                    source_dir,
                    private_output,
                    "axolotl",
                    identity_lock,
                    "direct-exact-target",
                    policy,
                )

            load_raster.assert_called_once_with(
                source_dir, "axolotl", identity_lock
            )
            self.assertEqual(write_role_gif.call_count, len(base.ROLE_SPECS))
            write_contact_sheet.assert_called_once()
            self.assertTrue((private_output / "axolotl.k868").is_file())

        self.assertEqual(
            result["identity_lock"]["identity_frame_sha256"],
            identity_frame_sha256,
        )
        records = {record["role"]: record for record in result["roles"]}
        self.assertEqual(set(records), {role.name for role in base.ROLE_SPECS})
        configured_roles = {"idle", "blink", "pet"}
        self.assertEqual(
            {
                role_name
                for role_name, record in records.items()
                if "species_role_visual_gate" in record
                or "species_role_visual_gate_sha256" in record
            },
            configured_roles,
        )

        frames_by_role = {
            role.name: list(raster.frames[index * 4 : index * 4 + 4])
            for index, role in enumerate(base.ROLE_SPECS)
        }
        for role_name, record in records.items():
            with self.subTest(role=role_name):
                if role_name not in configured_roles:
                    self.assertNotIn("species_role_visual_gate", record)
                    self.assertNotIn(
                        "species_role_visual_gate_sha256", record
                    )
                    continue

                evidence = record["species_role_visual_gate"]
                evidence_sha256 = record[
                    "species_role_visual_gate_sha256"
                ]
                expected_frame_sha256 = [
                    hashlib.sha256(frame.packed).hexdigest()
                    for frame in frames_by_role[role_name]
                ]
                self.assertEqual(
                    evidence_sha256,
                    contract.species_role_visual_gate_evidence_sha256(
                        evidence
                    ),
                )
                self.assertEqual(
                    record["frame_sha256"], expected_frame_sha256
                )
                self.assertEqual(
                    evidence["frame_sha256"], expected_frame_sha256
                )
                self.assertEqual(
                    evidence["durations_ms"], record["durations_ms"]
                )
                self.assertEqual(
                    evidence["durations_ms"],
                    list(ROLES[role_name].durations_ms),
                )
                self.assertEqual(evidence["identity_key"], "axolotl")
                self.assertEqual(evidence["role"], role_name)
                self.assertEqual(
                    evidence["identity_frame_sha256"],
                    identity_frame_sha256,
                )
                self.assertEqual(
                    evidence["identity_frame_sha256"],
                    policy.identity_frame_sha256["axolotl"],
                )
                self.assertEqual(
                    evidence["policy_schema"], policy.schema
                )
                self.assertEqual(
                    evidence["policy_relative_path"],
                    policy.relative_path,
                )
                self.assertEqual(
                    evidence["policy_sha256"], policy.source_sha256
                )
                self.assertEqual(
                    evidence["policy_entry_sha256"],
                    policy.entries[
                        ("axolotl", role_name)
                    ].entry_sha256,
                )
                self.assertEqual(
                    set(evidence["gate_results"]),
                    set(policy.entries[("axolotl", role_name)].gates),
                )

    def test_build_species_rejects_configured_hostile_before_any_output(self) -> None:
        policy, identity_lock, raster = (
            self.synthetic_axolotl_builder_fixture()
        )
        hostile_masks = [
            set(frame.mask) for frame in raster.frames[:4]
        ]
        hostile_masks[1].update({(x, 0) for x in range(5)})
        hostile_idle = replace_masks(
            "axolotl",
            "idle",
            list(raster.frames[:4]),
            hostile_masks,
        )
        hostile_frames = list(raster.frames)
        hostile_frames[:4] = hostile_idle
        hostile_raster = replace(raster, frames=tuple(hostile_frames))

        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            source_dir = temporary_root / "source"
            private_output = temporary_root / "private"
            with (
                mock.patch.object(
                    wild.raster_contract,
                    "load_high_res_species",
                    return_value=hostile_raster,
                ),
                mock.patch.object(
                    wild.base, "write_role_gif"
                ) as write_role_gif,
                mock.patch.object(
                    wild.base, "write_contact_sheet"
                ) as write_contact_sheet,
                mock.patch.object(
                    wild, "build_wild_pack"
                ) as build_wild_pack,
            ):
                with self.assertRaises(
                    contract.SpeciesRoleVisualGateError
                ) as raised:
                    wild.build_species(
                        source_dir,
                        private_output,
                        "axolotl",
                        identity_lock,
                        "direct-exact-target",
                        policy,
                    )

            self.assertEqual(
                raised.exception.reason_codes,
                ("AX_IDLE_GILL_PHASE_LOCALITY",),
            )
            write_role_gif.assert_not_called()
            write_contact_sheet.assert_not_called()
            build_wild_pack.assert_not_called()
            self.assertFalse(
                (private_output / "axolotl.k868").exists()
            )


if __name__ == "__main__":
    unittest.main()
