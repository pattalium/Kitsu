#!/usr/bin/env python3
"""Fail-closed raster contract for private companion artwork.

Image generation output is not release artwork. A release build is allowed
only when every action candidate uses the identity transform, a pre-frozen
native permission mask, explicit immutable identity/edit-target lineage, and
deterministic bounded composition. This module never cleans, repairs, shrinks,
fits, or guesses at damaged artwork: clipped sources, missing provenance,
invalid final debris, scale drift, and identity drift are build errors.

The public Cat, Dog, and Fox starters are intentionally outside this module.
Their already-released source sheets and packs must not enter the wild-creature
pipeline.
"""

from __future__ import annotations

import hashlib
import json
import math
from dataclasses import dataclass
from pathlib import Path

from PIL import Image

import build_default_packs as base


IDENTITY_LOCK_SCHEMA = "kitsu-wild-identity-lock-v1"
HIGH_RES_IDENTITY_LOCK_SCHEMA = "kitsu-wild-identity-lock-v2"
LEGACY_IMAGEGEN_IMPORT_LOCK_SCHEMA = "kitsu-wild-imagegen-import-lock-v4"
IMAGEGEN_IMPORT_LOCK_SCHEMA = "kitsu-wild-imagegen-import-lock-v5"
LEGACY_GENERATED_ACTION_SEMANTIC_SCHEMA = (
    "kitsu-wild-generated-action-semantic-locality-v3"
)
GENERATED_ACTION_SEMANTIC_SCHEMA = (
    "kitsu-wild-generated-action-semantic-locality-v4"
)
GENERATED_PHASE_PREAUTHORIZATION_SCHEMA = (
    "kitsu-wild-generated-phase-preauthorization-v2"
)
GENERATED_PREAUTHORIZATION_NATIVE_MASK_BASIS = (
    "frozen-storyboard-native-region-before-generation"
)
GENERATED_PREAUTHORIZATION_RECTANGLE_MIGRATION_BASIS = (
    "hash-pinned-pre-generation-native-rectangle-migration"
)
GENERATED_PREAUTHORIZATION_FREEZE_SOURCE_DIRECTORY = (
    "preauthorization/_frozen-source"
)
GENERATED_PREAUTHORIZATION_FREEZE_REGION_FIELD = (
    "roles[].preauthorized_role_pose_region"
)
GENERATED_PREAUTHORIZATION_ADDENDUM_FREEZE_REGION_FIELD = (
    "base_freeze.roles[].preauthorized_role_pose_region"
)
GENERATED_ROLE_REGISTRATION_SCHEMA = (
    "kitsu-wild-generated-role-registration-v1"
)
GENERATED_COMPOSITION_MODE = (
    "immutable-baseline-outside-mask-imported-candidate-inside-mask"
)
NATIVE_GRID_REFERENCE_SCHEMA = (
    "kitsu-wild-native-grid-conditioning-reference-v1"
)
NATIVE_GRID_REFERENCE_KIND = "read-only-native-grid-conditioning-reference"
NATIVE_GRID_REFERENCE_DIRECTORY = "native-grid-reference"
NATIVE_GRID_REFERENCE_DERIVATION = (
    "inverse-locked-output-offset-nearest-native-grid-v1"
)
P0_GENERATION_REFERENCE_MODE = "two-reference-identity-edit-v1"
TWO_REFERENCE_GENERATION_MODE = "two-reference-same-p0-star-v1"
THREE_REFERENCE_GENERATION_MODE = (
    "three-reference-same-p0-native-grid-star-v1"
)
NATIVE_REGION_MASK_SCHEMA = "kitsu-native-region-mask-64x80-v1"
NATIVE_REGION_MASK_ENCODING = "lsb0-row-major-hex"
PROTECTED_STARTERS = frozenset({"cat", "dog", "fox"})
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
SPECIES_ROLE_VISUAL_GATE_REASON_CODES = {
    ("axolotl", "idle", "required_8_connected_anchors"): (
        "AX_REQUIRED_LANDMARK_DISCONNECTED"
    ),
    ("axolotl", "idle", "phase_delta_locality"): (
        "AX_IDLE_GILL_PHASE_LOCALITY"
    ),
    ("axolotl", "idle", "loop_seam"): "AX_IDLE_LOOP_SEAM",
    ("axolotl", "blink", "required_8_connected_anchors"): (
        "AX_REQUIRED_LANDMARK_DISCONNECTED"
    ),
    ("axolotl", "blink", "pupil_only"): "AX_BLINK_PUPIL_ONLY",
    ("axolotl", "blink", "per_eye_occupancy"): (
        "AX_BLINK_PER_EYE_OCCUPANCY"
    ),
    ("axolotl", "pet", "required_8_connected_anchors"): (
        "AX_REQUIRED_LANDMARK_DISCONNECTED"
    ),
    ("axolotl", "pet", "localized_redraw"): "AX_PET_REDRAW_BUDGET",
    ("axolotl", "pet", "rigid_pupil_translation"): (
        "AX_PET_NONRIGID_PUPIL_TRANSFORM"
    ),
    ("axolotl", "pet", "gill_base_shared_transform"): (
        "AX_PET_GILL_BASE_DRIFT"
    ),
    ("rabbit", "idle", "phase_delta_locality"): (
        "RABBIT_IDLE_PHASE_RESIDUAL"
    ),
    ("rabbit", "idle", "split_nose_topology"): (
        "RABBIT_IDLE_SPLIT_NOSE_TOPOLOGY"
    ),
    ("rabbit", "idle", "cadence"): "RABBIT_IDLE_CADENCE",
    ("rabbit", "blink", "eye_sequence"): (
        "RABBIT_BLINK_EYE_MASS_SEQUENCE"
    ),
    ("rabbit", "listen", "skull_freeze"): (
        "RABBIT_LISTEN_SKULL_FREEZE"
    ),
    ("rabbit", "listen", "ear_base_freeze"): (
        "RABBIT_LISTEN_EAR_BASE_FREEZE"
    ),
    ("rabbit", "listen", "local_scale_ink"): (
        "RABBIT_LISTEN_LOCAL_SCALE_INK"
    ),
}
SPECIES_ROLE_VISUAL_GATE_EXECUTION_ORDER = (
    "required_8_connected_anchors",
    "phase_delta_locality",
    "loop_seam",
    "pupil_only",
    "per_eye_occupancy",
    "localized_redraw",
    "rigid_pupil_translation",
    "gill_base_shared_transform",
    "split_nose_topology",
    "cadence",
    "eye_sequence",
    "skull_freeze",
    "ear_base_freeze",
    "local_scale_ink",
)
SOURCE_THRESHOLD = 170
SOURCE_EDGE_GUARD = 1
MIN_SOURCE_INK = 80
MAX_SOURCE_COMPONENTS = 12
MAX_OUTPUT_COMPONENTS = 10
MIN_OUTPUT_COMPONENT_PIXELS = 2
MIN_PRIMARY_SOURCE_FRACTION = 0.55
MIN_CHANGED_PIXELS = 8
REQUIRED_FRAMES_PER_ROLE = 4

# Format-v2 artwork is authored at the exact OLED raster.  Nothing in the
# release path resizes it.  The firmware places this cell at logical (0, 16)
# on its 64x128 portrait framebuffer, so a subject ending at row 77 ends at
# screen row 93.  Rows 78..79 are deliberately blank, and the format-v2 Pet
# screen's next content starts at row 99.
HIGH_RES_FORMAT_VERSION = 2
HIGH_RES_FRAME_WIDTH = 64
HIGH_RES_FRAME_HEIGHT = 80
HIGH_RES_FRAME_BYTES = HIGH_RES_FRAME_WIDTH * HIGH_RES_FRAME_HEIGHT // 8
HIGH_RES_RENDER_X = 0
HIGH_RES_RENDER_Y = 16
HIGH_RES_BODY_AXIS_X = 32
HIGH_RES_SAFE_LEFT = 2
HIGH_RES_SAFE_TOP = 2
HIGH_RES_SAFE_RIGHT = 61
HIGH_RES_FLOOR_Y = 77
HIGH_RES_BOTTOM_GUARD_ROWS = tuple(range(78, 80))
HIGH_RES_PORTRAIT_WIDTH = 16
HIGH_RES_PORTRAIT_HEIGHT = 18
HIGH_RES_PORTRAIT_BYTES = (
    HIGH_RES_PORTRAIT_WIDTH * HIGH_RES_PORTRAIT_HEIGHT // 8
)
HIGH_RES_FRAME_FILES = tuple(f"{phase:02d}.png" for phase in range(4))
HIGH_RES_MIN_CHANGED_PIXELS = 16
HIGH_RES_MIN_PAIR_CHANGED_PIXELS = 4
HIGH_RES_MAX_COMPONENTS = 12
HIGH_RES_MIN_PRIMARY_FRACTION = 0.55
HIGH_RES_CENTER_TOLERANCE = 4.0
IMAGEGEN_OUTPUT_CANVAS = (HIGH_RES_FRAME_WIDTH, HIGH_RES_FRAME_HEIGHT)
IMAGEGEN_RESAMPLE_MODE = "box-area"
IMAGEGEN_LUMINANCE_MODE = "pillow-rgb-luma-over-white"
IMAGEGEN_ALPHA_BACKGROUND = (255, 255, 255)
IMAGEGEN_RECOMMENDED_SOURCE_CANVAS = (1122, 1402)
IMAGEGEN_RECOMMENDED_CROP_RECT = (1, 1, 1121, 1401)
IMAGEGEN_RECOMMENDED_OUTPUT_OFFSET = (-1, 27)
IMAGEGEN_RECOMMENDED_BLACK_COVERAGE_PER_MILLE = 120
IMAGEGEN_COVERAGE_STABILITY_PER_MILLE = 20
IMAGEGEN_MAX_AMBIGUOUS_SOURCE_FRACTION = 0.55
IMAGEGEN_MAX_THRESHOLD_SENSITIVE_FRACTION = 0.05
IMAGEGEN_ACTION_SHEET_LAYOUT_SCHEMA = "kitsu-imagegen-action-sheet-2x2-v1"
IMAGEGEN_ACTION_SHEET_SOURCE_CANVAS = (1122, 1402)
IMAGEGEN_ACTION_SHEET_PHASE_RECTS = (
    (0, 0, 560, 700),
    (562, 0, 1122, 700),
    (0, 702, 560, 1402),
    (562, 702, 1122, 1402),
)
IMAGEGEN_ACTION_SHEET_GUTTER_RECTS = (
    (560, 0, 562, 1402),
    (0, 700, 1122, 702),
)
IMAGEGEN_ACTION_SHEET_CELL_SAFE_GUARD_PIXELS = SOURCE_EDGE_GUARD + 1
GENERATED_MAX_OUT_OF_REGION_PIXELS = 0
GENERATED_MIN_MOTION_LANDMARK_PIXELS = 4
GENERATED_MIN_FROZEN_LANDMARK_PIXELS = 4
GENERATED_SOURCE_LAYOUTS = frozenset({"independent-frame"})
GENERATED_IDENTITY_BASELINE_ASSET_LAYOUT = "immutable-identity-baseline-copy"
GENERATED_ASSET_LAYOUTS = GENERATED_SOURCE_LAYOUTS | frozenset(
    {GENERATED_IDENTITY_BASELINE_ASSET_LAYOUT}
)
GENERATED_ROLE_OUTPUT_OFFSET_MAXIMUM = 4
GENERATED_ROLE_BASELINE_POLICY: dict[str, str] = {
    "idle": "identity-anchored",
    "blink": "identity-anchored",
    "pet": "immutable-role-phase-0",
    "sleep": "immutable-role-phase-0",
    "listen": "identity-anchored",
    "surprise": "immutable-role-phase-0",
    "play": "immutable-role-phase-0",
    "tired": "immutable-role-phase-0",
    "feed": "immutable-role-phase-0",
    "wake": "immutable-role-phase-0",
    "meet": "immutable-role-phase-0",
    "evolve": "immutable-role-phase-0",
}
GENERATED_ROLE_CONTACT_POLICY_DEFAULTS: dict[str, str] = {
    "idle": "planted-identity",
    "blink": "planted-identity",
    "pet": "planted-role-base",
    "sleep": "planted-role-base",
    "listen": "planted-identity",
    "surprise": "planted-role-base",
    "play": "bounded-approved-gait-lift",
    "tired": "planted-role-base",
    "feed": "planted-role-base",
    "wake": "bounded-approved-pose-change",
    "meet": "bounded-approved-gait-lift",
    "evolve": "bounded-approved-pose-change",
}
GENERATED_ROLE_CONTACT_POLICY_CAPABILITIES: dict[str, frozenset[str]] = {
    "idle": frozenset({"planted-identity"}),
    "blink": frozenset({"planted-identity"}),
    "pet": frozenset({"planted-role-base", "bounded-approved-gait-lift"}),
    "sleep": frozenset(
        {"planted-role-base", "bounded-approved-pose-change"}
    ),
    "listen": frozenset({"planted-identity"}),
    "surprise": frozenset(
        {"planted-role-base", "bounded-approved-pose-change"}
    ),
    "play": frozenset({"planted-role-base", "bounded-approved-gait-lift"}),
    "tired": frozenset(
        {"planted-role-base", "bounded-approved-pose-change"}
    ),
    "feed": frozenset({"planted-role-base", "bounded-approved-gait-lift"}),
    "wake": frozenset(
        {"planted-role-base", "bounded-approved-pose-change"}
    ),
    "meet": frozenset({"planted-role-base", "bounded-approved-gait-lift"}),
    "evolve": frozenset(
        {"planted-role-base", "bounded-approved-pose-change"}
    ),
}
GENERATED_CONTACT_POLICY_MAXIMUMS = {
    "planted-identity": 0,
    "planted-role-base": 0,
    "bounded-approved-gait-lift": 16,
    "bounded-approved-pose-change": 32,
}

# Direct-at-target art has no builder scale knob.  These are fail-closed
# plausibility gates for camera/identity pops, not permission to rescale a
# failing frame.  A failure goes back to artwork generation.
HIGH_RES_ROLE_SCALE_ENVELOPES: dict[str, tuple[float, float]] = {
    "idle": (0.90, 1.12),
    "blink": (0.88, 1.12),
    "pet": (0.78, 1.24),
    "sleep": (0.60, 1.22),
    "listen": (0.80, 1.20),
    "surprise": (0.76, 1.25),
    "play": (0.72, 1.28),
    "tired": (0.62, 1.22),
    "feed": (0.76, 1.22),
    "wake": (0.70, 1.24),
    "meet": (0.76, 1.24),
    "evolve": (0.78, 1.24),
}

HIGH_RES_ROLE_SCALE_POP_MAXIMUM: dict[str, float] = {
    "idle": 1.10,
    "blink": 1.10,
    "pet": 1.22,
    "sleep": 1.30,
    "listen": 1.16,
    "surprise": 1.22,
    "play": 1.28,
    "tired": 1.28,
    "feed": 1.20,
    "wake": 1.28,
    "meet": 1.22,
    "evolve": 1.22,
}

HIGH_RES_ROLE_TRANSITION_MAXIMUM: dict[str, float] = {
    "idle": 0.34,
    "blink": 0.34,
    "pet": 0.62,
    "sleep": 0.72,
    "listen": 0.48,
    "surprise": 0.66,
    "play": 0.76,
    "tired": 0.72,
    "feed": 0.62,
    "wake": 0.72,
    "meet": 0.64,
    "evolve": 0.70,
}

# These ranges describe apparent linear size relative to the approved identity
# master after normalizing for source-canvas dimensions.  They are deliberately
# broad enough for a pose, but narrow enough to reject a different camera scale.
# A frame outside an envelope is regenerated; the builder never rescales it.
ROLE_SCALE_ENVELOPES: dict[str, tuple[float, float]] = {
    "idle": (0.85, 1.15),
    "blink": (0.85, 1.15),
    "pet": (0.72, 1.28),
    "sleep": (0.60, 1.30),
    "listen": (0.75, 1.25),
    "surprise": (0.72, 1.30),
    "play": (0.68, 1.32),
    "tired": (0.65, 1.30),
    "feed": (0.72, 1.28),
    "wake": (0.68, 1.32),
    "meet": (0.72, 1.28),
    "evolve": (0.72, 1.30),
}

# Identity overlap is checked for every stored frame, not only Idle frame 0.
# Low-profile poses naturally overlap less, hence the role-specific floors.
ROLE_IDENTITY_JACCARD_MINIMUM: dict[str, float] = {
    "idle": 0.50,
    "blink": 0.48,
    "pet": 0.22,
    "sleep": 0.08,
    "listen": 0.30,
    "surprise": 0.20,
    "play": 0.14,
    "tired": 0.12,
    "feed": 0.20,
    "wake": 0.16,
    "meet": 0.18,
    "evolve": 0.16,
}


class RasterContractError(ValueError):
    """Artwork violates a condition that must never be auto-repaired."""


class SpeciesRoleVisualGateError(RasterContractError):
    """One configured visual gate rejected a final composited frame sequence."""

    def __init__(
        self,
        message: str,
        reason_code: str | tuple[str, ...] | list[str],
    ) -> None:
        super().__init__(message)
        raw_reason_codes = (
            (reason_code,)
            if isinstance(reason_code, str)
            else tuple(reason_code)
        )
        reason_codes = tuple(dict.fromkeys(raw_reason_codes))
        if not reason_codes:
            raise ValueError("species-role visual gate error requires a reason code")
        self.reason_codes = reason_codes
        self.reason_code = reason_codes[0]


@dataclass(frozen=True)
class IdentityLock:
    identity_key: str
    identity_sha256: str
    source_canvas: tuple[int, int]
    target_long_axis_pixels: int
    approved: bool


@dataclass(frozen=True)
class SourceMetrics:
    bounds: tuple[int, int, int, int]
    ink_pixels: int
    components: int
    smallest_component_pixels: int
    primary_fraction: float


@dataclass(frozen=True)
class IdentityMaster:
    lock: IdentityLock
    source_frame: base.SourceFrame
    source_metrics: SourceMetrics
    raster_scale: float
    output_mask: frozenset[tuple[int, int]]


@dataclass(frozen=True)
class RasterFrame:
    source_frame: base.SourceFrame
    source_metrics: SourceMetrics
    source_mask_sha256: str
    apparent_scale_ratio: float
    identity_jaccard: float
    output_mask: frozenset[tuple[int, int]]
    output_sha256: str


@dataclass(frozen=True)
class SpeciesRaster:
    identity: IdentityMaster
    frames: tuple[RasterFrame, ...]
    source_sha256: dict[str, str]
    fixed_action_scale: float


@dataclass(frozen=True)
class HighResIdentityLock:
    """Approval for one exact, already-canonical format-v2 identity frame."""

    identity_key: str
    identity_sha256: str
    frame_canvas: tuple[int, int]
    approved: bool


@dataclass(frozen=True)
class HighResFrame:
    """One exact 64x80 release frame; no derived raster is stored here."""

    path: Path
    role: str
    phase: int
    source_sha256: str
    mask: frozenset[tuple[int, int]]
    metrics: SourceMetrics
    apparent_scale_ratio: float
    identity_jaccard: float
    packed: bytes


@dataclass(frozen=True)
class HighResPortrait:
    path: Path
    source_sha256: str
    mask: frozenset[tuple[int, int]]
    packed: bytes


@dataclass(frozen=True)
class HighResSpeciesRaster:
    identity: HighResFrame
    portrait: HighResPortrait
    frames: tuple[HighResFrame, ...]
    source_sha256: dict[str, str]
    fixed_action_scale: float = 1.0
    generated_semantic_evidence: dict[str, dict[str, object]] | None = None
    generated_action_semantic_contract_sha256: str | None = None


@dataclass(frozen=True)
class SpeciesRoleVisualGateEntry:
    """One strictly parsed, species/role-specific ROI policy entry."""

    identity_key: str
    role: str
    identity_frame_sha256: str
    gates: dict[str, object]
    entry_sha256: str


@dataclass(frozen=True)
class SpeciesRoleVisualGatePolicy:
    """The canonical allow-listed visual gate policy and its file binding."""

    schema: str
    relative_path: str
    source_sha256: str
    source_bytes: bytes
    identity_frame_sha256: dict[str, str]
    entries: dict[tuple[str, str], SpeciesRoleVisualGateEntry]


@dataclass(frozen=True)
class ImageGenImportTransform:
    source_canvas: tuple[int, int]
    crop_rect: tuple[int, int, int, int]
    output_canvas: tuple[int, int]
    resample_mode: str
    luminance_mode: str
    black_coverage_threshold_per_mille: int
    alpha_background: tuple[int, int, int]
    output_offset: tuple[int, int]
    action_output_offset: tuple[int, int]


@dataclass(frozen=True)
class ImageGenImportLock:
    schema: str
    identity_key: str
    identity_source_sha256: str
    identity_frame_sha256: str
    transform_sha256: str
    transform: ImageGenImportTransform
    action_semantic_contract_sha256: str
    action_semantic_contract: GeneratedActionSemanticContract
    approved: bool


@dataclass(frozen=True)
class NativeRegionMaskLock:
    """One exact 64x80 region; set bits select coordinates, not artwork ink."""

    mask: frozenset[tuple[int, int]]
    packed_sha256: str


@dataclass(frozen=True)
class ImmutableIdentityReference:
    kind: str
    relative_path: str
    identity_key: str
    source_sha256: str
    frame_sha256: str


@dataclass(frozen=True)
class ImmutableEditTargetReference:
    kind: str
    relative_path: str
    identity_key: str
    role: str
    phase: int
    source_sha256: str
    registered_frame_sha256: str
    accepted_composited_frame_sha256: str


@dataclass(frozen=True)
class NativeGridConditioningReference:
    schema: str
    kind: str
    image_number: int
    read_only: bool
    edit_target: bool
    source_relative_path: str
    source_png_sha256: str
    grid_relative_path: str
    grid_png_sha256: str
    p0_packed_sha256: str
    roundtrip_packed_sha256: str
    transform_sha256: str
    role_registration_sha256: str
    derivation: str


@dataclass(frozen=True)
class FrozenPhasePreauthorizationReference:
    kind: str
    relative_path: str
    source_sha256: str
    storyboard_sha256: str
    allowed_change_region_sha256: str
    edit_target_kind: str


@dataclass(frozen=True)
class GeneratedPhaseAsset:
    layout: str
    relative_path: str
    source_sha256: str
    imported_candidate_frame_sha256: str
    registered_candidate_frame_sha256: str


@dataclass(frozen=True)
class GeneratedRoleRegistrationLock:
    schema: str
    derivation: str
    output_offset: tuple[int, int]
    p0_unregistered_floor_y: int


@dataclass(frozen=True)
class FrozenSemanticRegion:
    kind: str
    name: str
    region: NativeRegionMaskLock
    maximum_changed_pixels: int


@dataclass(frozen=True)
class GeneratedPhaseSemanticLock:
    phase: int
    semantic_baseline: str
    identity_reference: ImmutableIdentityReference
    edit_target_reference: ImmutableEditTargetReference
    preauthorization_reference: FrozenPhasePreauthorizationReference
    generated_asset: GeneratedPhaseAsset
    allowed_change_region: NativeRegionMaskLock
    composition_mode: str
    composition_baseline_frame_sha256: str
    composited_frame_sha256: str
    maximum_out_of_region_changed_pixels: int
    frozen_regions: tuple[FrozenSemanticRegion, ...]
    generation_reference_mode: str = TWO_REFERENCE_GENERATION_MODE
    native_grid_reference: NativeGridConditioningReference | None = None


@dataclass(frozen=True)
class MotionLandmarkLock:
    name: str
    region: NativeRegionMaskLock
    minimum_changed_pixels: int


@dataclass(frozen=True)
class RolePoseIdentityLandmarkLock:
    name: str
    identity_region: NativeRegionMaskLock
    role_pose_region: NativeRegionMaskLock
    minimum_ink_pixels: int
    minimum_ink_retention_per_mille: int
    maximum_component_count_delta: int


@dataclass(frozen=True)
class GeneratedRoleSemanticLock:
    role: str
    baseline_policy: str
    contact_policy: str
    role_registration: GeneratedRoleRegistrationLock
    role_registration_sha256: str
    role_pose_baseline_frame_sha256: str
    maximum_role_pose_component_count_delta: int
    maximum_contact_changed_pixels_per_phase: int
    role_pose_identity_landmarks: tuple[RolePoseIdentityLandmarkLock, ...]
    phases: tuple[GeneratedPhaseSemanticLock, ...]
    motion_landmarks: tuple[MotionLandmarkLock, ...]


@dataclass(frozen=True)
class GeneratedActionSemanticContract:
    schema: str
    roles: tuple[GeneratedRoleSemanticLock, ...]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def mask_sha256(
    mask: set[tuple[int, int]] | frozenset[tuple[int, int]], width: int, height: int
) -> str:
    payload = bytearray()
    payload.extend(width.to_bytes(4, "little"))
    payload.extend(height.to_bytes(4, "little"))
    for x, y in sorted(mask, key=lambda point: (point[1], point[0])):
        payload.extend(x.to_bytes(4, "little"))
        payload.extend(y.to_bytes(4, "little"))
    return hashlib.sha256(payload).hexdigest()


def binary_mask(image: Image.Image) -> set[tuple[int, int]]:
    """Threshold to a binary mask without deleting or repairing any pixels."""

    gray = image.convert("L")
    return {
        (x, y)
        for y in range(gray.height)
        for x in range(gray.width)
        if gray.getpixel((x, y)) < SOURCE_THRESHOLD
    }


def _outside_primary_box(
    component: set[tuple[int, int]], primary_bounds: tuple[int, int, int, int]
) -> bool:
    left, top, right, bottom = base.bounds(component)
    p_left, p_top, p_right, p_bottom = primary_bounds
    center_x = (left + right) / 2
    center_y = (top + bottom) / 2
    return not (p_left <= center_x <= p_right and p_top <= center_y <= p_bottom)


def validate_source_mask(
    mask: set[tuple[int, int]], width: int, height: int, label: str
) -> SourceMetrics:
    """Validate one source subject, preserving its mask byte-for-byte."""

    if len(mask) < MIN_SOURCE_INK:
        raise RasterContractError(
            f"{label}: missing/empty frame ({len(mask)} foreground pixels)"
        )

    touching_edges = sorted(
        edge
        for edge, found in (
            ("left", any(x <= SOURCE_EDGE_GUARD for x, _y in mask)),
            ("top", any(y <= SOURCE_EDGE_GUARD for _x, y in mask)),
            (
                "right",
                any(x >= width - 1 - SOURCE_EDGE_GUARD for x, _y in mask),
            ),
            (
                "bottom",
                any(y >= height - 1 - SOURCE_EDGE_GUARD for _x, y in mask),
            ),
        )
        if found
    )
    if touching_edges:
        raise RasterContractError(
            f"{label}: subject touches source-cell edge(s) {touching_edges}; "
            "the frame is clipped and will not be cropped or shrunk"
        )

    components = sorted(base.connected_components(mask), key=len, reverse=True)
    if len(components) > MAX_SOURCE_COMPONENTS:
        raise RasterContractError(
            f"{label}: fragmented/debris source has {len(components)} components "
            f"(maximum {MAX_SOURCE_COMPONENTS})"
        )
    primary = components[0]
    primary_fraction = len(primary) / len(mask)
    if primary_fraction < MIN_PRIMARY_SOURCE_FRACTION:
        raise RasterContractError(
            f"{label}: primary subject is only {primary_fraction:.3f} of source ink; "
            "frame is fragmented or contains a second subject"
        )

    primary_bounds = base.bounds(primary)
    debris_limit = max(12, math.ceil(len(primary) * 0.002))
    detached_debris = [
        len(component)
        for component in components[1:]
        if len(component) < debris_limit
        and _outside_primary_box(component, primary_bounds)
    ]
    if detached_debris:
        raise RasterContractError(
            f"{label}: detached debris components {detached_debris}; "
            "source pixels are never silently deleted"
        )

    return SourceMetrics(
        bounds=base.bounds(mask),
        ink_pixels=len(mask),
        components=len(components),
        smallest_component_pixels=min(map(len, components)),
        primary_fraction=primary_fraction,
    )


def decode_frame_bytes(payload: bytes) -> set[tuple[int, int]]:
    if len(payload) != base.FRAME_BYTES:
        raise RasterContractError(
            f"stored frame has {len(payload)} bytes; expected {base.FRAME_BYTES}"
        )
    return {
        (x, y)
        for y in range(base.FRAME_HEIGHT)
        for x in range(base.FRAME_WIDTH)
        if payload[y * (base.FRAME_WIDTH // 8) + x // 8] & (1 << (x & 7))
    }


def validate_output_mask(mask: set[tuple[int, int]], label: str) -> None:
    left, top, right, bottom = base.bounds(mask)
    if (
        left < base.SAFE_LEFT
        or top < base.SAFE_TOP
        or right > base.SAFE_RIGHT
        or bottom != base.FLOOR_Y
    ):
        raise RasterContractError(
            f"{label}: bounds {(left, top, right, bottom)} violate fixed canvas "
            f"{(base.SAFE_LEFT, base.SAFE_TOP, base.SAFE_RIGHT, base.FLOOR_Y)}"
        )
    axis = base.median_x(mask)
    if not base.BODY_AXIS_X - 0.5 <= axis <= base.BODY_AXIS_X + 0.5:
        raise RasterContractError(
            f"{label}: body axis {axis} violates fixed center x={base.BODY_AXIS_X}"
        )

    components = base.connected_components(mask)
    if len(components) > MAX_OUTPUT_COMPONENTS:
        raise RasterContractError(
            f"{label}: downscaled frame has {len(components)} components "
            f"(maximum {MAX_OUTPUT_COMPONENTS})"
        )
    smallest = min(map(len, components))
    if smallest < MIN_OUTPUT_COMPONENT_PIXELS:
        raise RasterContractError(
            f"{label}: downscale produced a {smallest}-pixel component; "
            "detail/debris did not survive at release resolution"
        )

    encoded = base.frame_bytes(mask)
    if decode_frame_bytes(encoded) != mask:
        raise RasterContractError(f"{label}: 1-bit pack round-trip changed pixels")


def jaccard(
    left: set[tuple[int, int]] | frozenset[tuple[int, int]],
    right: set[tuple[int, int]] | frozenset[tuple[int, int]],
) -> float:
    union = left | right
    return len(left & right) / len(union) if union else 0.0


def rasterize_full_cell(frame: base.SourceFrame, scale: float) -> set[tuple[int, int]]:
    """Nearest-neighbour resize of the complete cell, followed by translation.

    The old pipeline cropped to a detected bounding box and then repeatedly
    changed scale until the subject fit.  Here the entire source cell is
    sampled at the identity-locked camera scale.  Only a rigid translation is
    applied afterward; no source pixel is cropped and no output pixel is
    clamped away.
    """

    source = Image.new("1", (frame.cell_width, frame.cell_height), 0)
    pixels = source.load()
    for x, y in frame.mask:
        pixels[x, y] = 1
    width = max(1, round(frame.cell_width * scale))
    height = max(1, round(frame.cell_height * scale))
    resized = source.resize((width, height), Image.Resampling.NEAREST)
    if resized.mode != "1":
        raise RasterContractError(
            f"{frame.species}/{frame.role.name}/{frame.phase}: resize is not 1-bit"
        )
    resized_mask = {
        (x, y)
        for y in range(height)
        for x in range(width)
        if resized.getpixel((x, y))
    }
    if not resized_mask:
        raise RasterContractError(
            f"{frame.species}/{frame.role.name}/{frame.phase}: resize erased subject"
        )

    local_axis = base.median_x(resized_mask)
    local_bottom = max(y for _x, y in resized_mask)
    origin_x = round(base.BODY_AXIS_X - local_axis)
    origin_y = base.FLOOR_Y - local_bottom
    return {(x + origin_x, y + origin_y) for x, y in resized_mask}


def _load_identity_master(
    path: Path, species: str, lock: IdentityLock
) -> IdentityMaster:
    if species in PROTECTED_STARTERS:
        raise RasterContractError(
            f"{species}: protected starter cannot enter the wild raster pipeline"
        )
    actual_hash = sha256_file(path)
    if actual_hash != lock.identity_sha256:
        raise RasterContractError(
            f"{species}: identity.png SHA-256 differs from the approved identity lock"
        )
    with Image.open(path) as image:
        if image.size != lock.source_canvas:
            raise RasterContractError(
                f"{species}: identity canvas {image.size} differs from locked "
                f"canvas {lock.source_canvas}"
            )
        mask = binary_mask(image)
        metrics = validate_source_mask(
            mask, image.width, image.height, f"{species}/identity"
        )
        frame = base.SourceFrame(
            species=species,
            role=base.ROLE_SPECS[0],
            phase=0,
            mask=mask,
            cell_width=image.width,
            cell_height=image.height,
        )

    left, top, right, bottom = metrics.bounds
    long_axis = max(right - left + 1, bottom - top + 1)
    raster_scale = lock.target_long_axis_pixels / long_axis
    output = rasterize_full_cell(frame, raster_scale)
    validate_output_mask(output, f"{species}/identity")
    return IdentityMaster(
        lock=lock,
        source_frame=frame,
        source_metrics=metrics,
        raster_scale=raster_scale,
        output_mask=frozenset(output),
    )


def _apparent_scale_ratio(
    frame: base.SourceFrame, identity: IdentityMaster
) -> float:
    frame_density = len(frame.mask) / (frame.cell_width * frame.cell_height)
    master = identity.source_frame
    identity_density = len(master.mask) / (master.cell_width * master.cell_height)
    return math.sqrt(frame_density / identity_density)


def _rasterize_action_frame(
    frame: base.SourceFrame,
    metrics: SourceMetrics,
    identity: IdentityMaster,
    fixed_scale: float,
) -> RasterFrame:
    label = f"{frame.species}/{frame.role.name}/{frame.phase}"
    apparent_scale = _apparent_scale_ratio(frame, identity)
    lower, upper = ROLE_SCALE_ENVELOPES[frame.role.name]
    if not lower <= apparent_scale <= upper:
        raise RasterContractError(
            f"{label}: apparent subject scale {apparent_scale:.3f} is outside "
            f"identity envelope [{lower:.3f}, {upper:.3f}]"
        )

    output = rasterize_full_cell(frame, fixed_scale)
    try:
        validate_output_mask(output, label)
    except (RasterContractError, ValueError) as error:
        raise RasterContractError(
            f"{label}: frame does not fit the fixed identity scale; rejected "
            f"instead of auto-shrinking or clipping the action ({error})"
        ) from error

    similarity = jaccard(identity.output_mask, output)
    minimum = ROLE_IDENTITY_JACCARD_MINIMUM[frame.role.name]
    if similarity < minimum:
        raise RasterContractError(
            f"{label}: identity overlap {similarity:.3f} is below role floor "
            f"{minimum:.3f}"
        )

    encoded = base.frame_bytes(output)
    return RasterFrame(
        source_frame=frame,
        source_metrics=metrics,
        source_mask_sha256=mask_sha256(
            frame.mask, frame.cell_width, frame.cell_height
        ),
        apparent_scale_ratio=apparent_scale,
        identity_jaccard=similarity,
        output_mask=frozenset(output),
        output_sha256=base.sha256_bytes(encoded),
    )


def validate_four_frame_role(role: base.RoleSpec, frames: list[RasterFrame]) -> None:
    label = f"{frames[0].source_frame.species}/{role.name}" if frames else role.name
    phases = [frame.source_frame.phase for frame in frames]
    if len(frames) != REQUIRED_FRAMES_PER_ROLE or phases != list(
        range(REQUIRED_FRAMES_PER_ROLE)
    ):
        raise RasterContractError(
            f"{label}: exact phases 0,1,2,3 are required; got {phases}"
        )
    source_hashes = [frame.source_mask_sha256 for frame in frames]
    if len(set(source_hashes)) != REQUIRED_FRAMES_PER_ROLE:
        raise RasterContractError(
            f"{label}: all four source frames must be present and distinct"
        )
    output_hashes = [frame.output_sha256 for frame in frames]
    if len(set(output_hashes)) != REQUIRED_FRAMES_PER_ROLE:
        raise RasterContractError(
            f"{label}: downscale collapsed one or more frames; all four stored "
            "frames must remain distinct"
        )
    masks = [set(frame.output_mask) for frame in frames]
    changed_pixels = len(set().union(*masks) - set.intersection(*masks))
    if changed_pixels < MIN_CHANGED_PIXELS:
        raise RasterContractError(
            f"{label}: animation changes only {changed_pixels} stored pixels"
        )


def load_species_raster(
    source_dir: Path,
    species: str,
    lock: IdentityLock,
    roles: tuple[base.RoleSpec, ...] = base.ROLE_SPECS,
) -> SpeciesRaster:
    """Load and validate a complete species without writing any output."""

    if species in PROTECTED_STARTERS:
        raise RasterContractError(
            f"{species}: protected starter cannot enter the wild raster pipeline"
        )
    if lock.identity_key != species or not lock.approved:
        raise RasterContractError(
            f"{species}: an explicit approved identity lock is required"
        )
    species_dir = source_dir / species
    expected = {"identity.png", *(f"{role.name}.png" for role in roles)}
    actual = {path.name for path in species_dir.glob("*.png")}
    if actual != expected:
        raise RasterContractError(
            f"{species}: source set must be exact; missing={sorted(expected - actual)} "
            f"unexpected={sorted(actual - expected)}"
        )

    identity_path = species_dir / "identity.png"
    identity = _load_identity_master(identity_path, species, lock)
    source_hashes = {"identity.png": sha256_file(identity_path)}
    action_scale_x = identity.raster_scale * identity.source_frame.cell_width
    action_scale_y = identity.raster_scale * identity.source_frame.cell_height
    all_frames: list[RasterFrame] = []

    for role in roles:
        path = species_dir / f"{role.name}.png"
        source_hashes[path.name] = sha256_file(path)
        role_frames: list[RasterFrame] = []
        with Image.open(path) as image:
            if image.size != lock.source_canvas:
                raise RasterContractError(
                    f"{species}/{role.name}: action canvas {image.size} differs "
                    f"from locked identity canvas {lock.source_canvas}"
                )
            if image.width != image.height or image.width % 2 or image.height % 2:
                raise RasterContractError(
                    f"{species}/{role.name}: action asset must be an even square 2x2"
                )
            cell_width = image.width // 2
            cell_height = image.height // 2
            scale_x = action_scale_x / cell_width
            scale_y = action_scale_y / cell_height
            if not math.isclose(scale_x, scale_y, rel_tol=1e-9, abs_tol=1e-12):
                raise RasterContractError(
                    f"{species}/{role.name}: source aspect changes identity scale"
                )
            fixed_scale = (scale_x + scale_y) / 2
            gray = image.convert("L")
            for phase in range(REQUIRED_FRAMES_PER_ROLE):
                column = phase % 2
                row = phase // 2
                crop = gray.crop(
                    (
                        column * cell_width,
                        row * cell_height,
                        (column + 1) * cell_width,
                        (row + 1) * cell_height,
                    )
                )
                mask = binary_mask(crop)
                label = f"{species}/{role.name}/{phase}"
                metrics = validate_source_mask(mask, cell_width, cell_height, label)
                source_frame = base.SourceFrame(
                    species=species,
                    role=role,
                    phase=phase,
                    mask=mask,
                    cell_width=cell_width,
                    cell_height=cell_height,
                )
                role_frames.append(
                    _rasterize_action_frame(
                        source_frame, metrics, identity, fixed_scale
                    )
                )
        validate_four_frame_role(role, role_frames)
        all_frames.extend(role_frames)

    fixed_action_scale = (
        identity.raster_scale
        * identity.source_frame.cell_width
        / (lock.source_canvas[0] // 2)
    )
    if len(all_frames) != len(roles) * REQUIRED_FRAMES_PER_ROLE:
        raise RasterContractError(
            f"{species}: expected {len(roles) * REQUIRED_FRAMES_PER_ROLE} frames, "
            f"got {len(all_frames)}"
        )
    return SpeciesRaster(
        identity=identity,
        frames=tuple(all_frames),
        source_sha256=source_hashes,
        fixed_action_scale=fixed_action_scale,
    )


def load_identity_locks(path: Path, selected: list[str]) -> dict[str, IdentityLock]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict) or set(payload) != {"schema", "identities"}:
        raise RasterContractError(
            "identity lock must contain exactly schema and identities"
        )
    if payload["schema"] != IDENTITY_LOCK_SCHEMA:
        raise RasterContractError(f"identity lock schema must be {IDENTITY_LOCK_SCHEMA}")
    records = payload["identities"]
    if not isinstance(records, list):
        raise RasterContractError("identity lock identities must be a list")
    locks: dict[str, IdentityLock] = {}
    for raw in records:
        if not isinstance(raw, dict) or set(raw) != {
            "approved",
            "identity_key",
            "identity_sha256",
            "source_canvas",
            "target_long_axis_pixels",
        }:
            raise RasterContractError("identity lock record has unexpected fields")
        identity_key = raw["identity_key"]
        digest = raw["identity_sha256"]
        canvas = raw["source_canvas"]
        target = raw["target_long_axis_pixels"]
        approved = raw["approved"]
        if (
            not isinstance(identity_key, str)
            or identity_key in locks
            or identity_key in PROTECTED_STARTERS
        ):
            raise RasterContractError("identity lock contains invalid/duplicate identity")
        if (
            not isinstance(digest, str)
            or len(digest) != 64
            or any(character not in "0123456789abcdef" for character in digest)
        ):
            raise RasterContractError(f"{identity_key}: invalid identity SHA-256")
        if (
            not isinstance(canvas, list)
            or len(canvas) != 2
            or any(not isinstance(value, int) or value < 512 for value in canvas)
        ):
            raise RasterContractError(f"{identity_key}: invalid locked source canvas")
        if not isinstance(target, int) or not 40 <= target <= 52:
            raise RasterContractError(
                f"{identity_key}: target_long_axis_pixels must be 40..52"
            )
        if approved is not True:
            raise RasterContractError(f"{identity_key}: identity lock is not approved")
        locks[identity_key] = IdentityLock(
            identity_key=identity_key,
            identity_sha256=digest,
            source_canvas=(canvas[0], canvas[1]),
            target_long_axis_pixels=target,
            approved=True,
        )
    if set(locks) != set(selected):
        raise RasterContractError(
            f"identity lock set differs from selected build: "
            f"missing={sorted(set(selected) - set(locks))} "
            f"unexpected={sorted(set(locks) - set(selected))}"
        )
    return locks


def high_res_frame_bytes(mask: set[tuple[int, int]]) -> bytes:
    """Pack one exact 64x80 mask as row-major XBM, LSB-first."""

    packed = bytearray(HIGH_RES_FRAME_BYTES)
    for x, y in mask:
        if not (0 <= x < HIGH_RES_FRAME_WIDTH and 0 <= y < HIGH_RES_FRAME_HEIGHT):
            raise RasterContractError(
                f"format-v2 frame contains out-of-canvas pixel {(x, y)}"
            )
        packed[y * (HIGH_RES_FRAME_WIDTH // 8) + x // 8] |= 1 << (x & 7)
    return bytes(packed)


def _is_lower_sha256(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(character in "0123456789abcdef" for character in value)
    )


def _require_lower_sha256(value: object, label: str) -> str:
    if not _is_lower_sha256(value):
        raise RasterContractError(f"{label}: invalid lowercase SHA-256")
    return value


def _require_semantic_name(value: object, label: str) -> str:
    if (
        not isinstance(value, str)
        or not value
        or len(value) > 64
        or value[0] not in "abcdefghijklmnopqrstuvwxyz"
        or any(
            character not in "abcdefghijklmnopqrstuvwxyz0123456789-"
            for character in value
        )
    ):
        raise RasterContractError(
            f"{label}: semantic name must be lowercase kebab-case"
        )
    return value


def _require_canonical_relative_path(value: object, label: str) -> str:
    if not isinstance(value, str):
        raise RasterContractError(f"{label}: relative path must be a string")
    relative = Path(value)
    if (
        relative.is_absolute()
        or value != relative.as_posix()
        or any(part in {"", ".", ".."} for part in relative.parts)
    ):
        raise RasterContractError(f"{label}: path is not canonical and relative")
    return value


def _canonical_json_sha256(value: object) -> str:
    payload = json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
        allow_nan=False,
    ).encode("ascii")
    return hashlib.sha256(payload).hexdigest()


def _require_exact_object(
    raw: object, expected: set[str], label: str
) -> dict[str, object]:
    if not isinstance(raw, dict) or set(raw) != expected:
        actual = set(raw) if isinstance(raw, dict) else set()
        raise RasterContractError(
            f"{label}: exact object is required; "
            f"missing={sorted(expected - actual)} "
            f"unexpected={sorted(actual - expected)}"
        )
    return raw


def _require_visual_int(
    value: object, label: str, minimum: int = 0, maximum: int = 5120
) -> int:
    if type(value) is not int or not minimum <= value <= maximum:
        raise RasterContractError(
            f"{label}: integer must be in {minimum}..{maximum}"
        )
    return value


def _require_visual_number(
    value: object, label: str, minimum: float = 0.0, maximum: float = 5120.0
) -> float:
    if (
        isinstance(value, bool)
        or not isinstance(value, (int, float))
        or not math.isfinite(float(value))
        or not minimum <= float(value) <= maximum
    ):
        raise RasterContractError(
            f"{label}: number must be finite and in {minimum}..{maximum}"
        )
    return float(value)


def _parse_visual_rect(raw: object, label: str) -> list[int]:
    if not isinstance(raw, list) or len(raw) != 4:
        raise RasterContractError(f"{label}: inclusive ROI must contain four integers")
    values = [
        _require_visual_int(value, f"{label}[{index}]", 0, 79)
        for index, value in enumerate(raw)
    ]
    left, top, right, bottom = values
    if (
        right >= HIGH_RES_FRAME_WIDTH
        or bottom >= HIGH_RES_FRAME_HEIGHT
        or left > right
        or top > bottom
    ):
        raise RasterContractError(f"{label}: inclusive ROI leaves the 64x80 canvas")
    return values


def _parse_visual_named_rects(
    raw: object, expected_names: set[str], label: str
) -> dict[str, object]:
    mapping = _require_exact_object(raw, expected_names, label)
    for name, rect in mapping.items():
        _parse_visual_rect(rect, f"{label}.{name}")
    return mapping


def _parse_visual_int_min_max(
    raw: object, label: str, maximum: int = 5120
) -> list[int]:
    if not isinstance(raw, list) or len(raw) != 2:
        raise RasterContractError(f"{label}: minimum/maximum pair is required")
    low = _require_visual_int(raw[0], f"{label}[0]", 0, maximum)
    high = _require_visual_int(raw[1], f"{label}[1]", 0, maximum)
    if low > high:
        raise RasterContractError(f"{label}: minimum exceeds maximum")
    return [low, high]


def _parse_visual_number_min_max(
    raw: object, label: str, maximum: float = 10.0
) -> list[float]:
    if not isinstance(raw, list) or len(raw) != 2:
        raise RasterContractError(f"{label}: minimum/maximum pair is required")
    low = _require_visual_number(raw[0], f"{label}[0]", 0.0, maximum)
    high = _require_visual_number(raw[1], f"{label}[1]", 0.0, maximum)
    if low > high:
        raise RasterContractError(f"{label}: minimum exceeds maximum")
    return [low, high]


_SPECIES_ROLE_VISUAL_GATE_KIND_SETS = {
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


def _parse_species_role_visual_gate(
    kind: str, raw: object, label: str
) -> dict[str, object]:
    if kind == "required_8_connected_anchors":
        gate = _require_exact_object(raw, {"anchors", "apply_to_phases"}, label)
        if gate["apply_to_phases"] != [0, 1, 2, 3]:
            raise RasterContractError(f"{label}: all four ordered phases are required")
        _parse_visual_named_rects(
            gate["anchors"], {"body", "upper_tail", "lower_tail"}, f"{label}.anchors"
        )
        return gate
    if kind == "phase_delta_locality":
        gate = _require_exact_object(raw, {"baseline_phase", "phases"}, label)
        if gate["baseline_phase"] != 0:
            raise RasterContractError(f"{label}: baseline phase must be P0")
        phases = gate["phases"]
        if not isinstance(phases, list) or len(phases) != 3:
            raise RasterContractError(f"{label}: exact P1/P2/P3 rules are required")
        for expected_phase, value in enumerate(phases, start=1):
            phase = _require_exact_object(
                value,
                {"allowed_rects", "maximum_outside_xor_pixels", "phase"},
                f"{label}.phases[{expected_phase - 1}]",
            )
            if phase["phase"] != expected_phase:
                raise RasterContractError(f"{label}: phase rules must be ordered P1/P2/P3")
            rects = phase["allowed_rects"]
            if not isinstance(rects, list) or not rects or len(rects) > 4:
                raise RasterContractError(f"{label}: one to four allowed ROIs are required")
            for index, rect in enumerate(rects):
                _parse_visual_rect(rect, f"{label}.phases[{expected_phase - 1}].allowed_rects[{index}]")
            _require_visual_int(
                phase["maximum_outside_xor_pixels"],
                f"{label}.phases[{expected_phase - 1}].maximum_outside_xor_pixels",
            )
        return gate
    if kind == "loop_seam":
        gate = _require_exact_object(
            raw, {"from_phase", "maximum_xor_pixels", "to_phase"}, label
        )
        if gate["from_phase"] != 3 or gate["to_phase"] != 0:
            raise RasterContractError(f"{label}: loop seam must measure P3 to P0")
        _require_visual_int(gate["maximum_xor_pixels"], f"{label}.maximum_xor_pixels")
        return gate
    if kind == "pupil_only":
        gate = _require_exact_object(
            raw, {"baseline_phase", "eye_rects", "maximum_outside_xor_pixels"}, label
        )
        if gate["baseline_phase"] != 0:
            raise RasterContractError(f"{label}: baseline phase must be P0")
        _parse_visual_named_rects(gate["eye_rects"], {"left", "right"}, f"{label}.eye_rects")
        _require_visual_int(
            gate["maximum_outside_xor_pixels"], f"{label}.maximum_outside_xor_pixels"
        )
        return gate
    if kind == "per_eye_occupancy":
        gate = _require_exact_object(raw, {"eye_rects", "phase_min_max_pixels"}, label)
        _parse_visual_named_rects(gate["eye_rects"], {"left", "right"}, f"{label}.eye_rects")
        ranges = gate["phase_min_max_pixels"]
        if not isinstance(ranges, list) or len(ranges) != 4:
            raise RasterContractError(f"{label}: four occupancy ranges are required")
        for phase, pair in enumerate(ranges):
            _parse_visual_int_min_max(pair, f"{label}.phase_min_max_pixels[{phase}]", 64)
        return gate
    if kind == "localized_redraw":
        gate = _require_exact_object(raw, {"maximum_adjacent_xor_pixels"}, label)
        _require_visual_int(
            gate["maximum_adjacent_xor_pixels"], f"{label}.maximum_adjacent_xor_pixels"
        )
        return gate
    if kind == "rigid_pupil_translation":
        gate = _require_exact_object(
            raw,
            {
                "component_bbox_max",
                "component_pixels_min_max",
                "maximum_inter_eye_translation_delta",
                "tracking_windows",
            },
            label,
        )
        _parse_visual_named_rects(
            gate["tracking_windows"], {"left", "right"}, f"{label}.tracking_windows"
        )
        _parse_visual_int_min_max(
            gate["component_pixels_min_max"], f"{label}.component_pixels_min_max", 64
        )
        bbox = gate["component_bbox_max"]
        if not isinstance(bbox, list) or len(bbox) != 2:
            raise RasterContractError(f"{label}: component bbox maximum must have two integers")
        for index, value in enumerate(bbox):
            _require_visual_int(value, f"{label}.component_bbox_max[{index}]", 1, 16)
        delta = gate["maximum_inter_eye_translation_delta"]
        if not isinstance(delta, list) or len(delta) != 2:
            raise RasterContractError(f"{label}: translation delta must have two numbers")
        for index, value in enumerate(delta):
            _require_visual_number(
                value, f"{label}.maximum_inter_eye_translation_delta[{index}]", 0.0, 8.0
            )
        if delta != [0, 0]:
            raise RasterContractError(
                f"{label}: both pupils must use one exact shared integer vector"
            )
        return gate
    if kind == "gill_base_shared_transform":
        gate = _require_exact_object(
            raw,
            {"maximum_template_xor_pixels_each", "template_phase", "template_rects"},
            label,
        )
        if gate["template_phase"] != 0:
            raise RasterContractError(f"{label}: gill templates must derive from P0")
        _parse_visual_named_rects(
            gate["template_rects"],
            {"dorsal_base", "middle_base", "ventral_base"},
            f"{label}.template_rects",
        )
        _require_visual_int(
            gate["maximum_template_xor_pixels_each"],
            f"{label}.maximum_template_xor_pixels_each",
        )
        return gate
    if kind == "split_nose_topology":
        gate = _require_exact_object(raw, {"ink_rois", "phase", "white_rois"}, label)
        if gate["phase"] != 1:
            raise RasterContractError(f"{label}: split-nose topology is frozen at P1")
        ink = _require_exact_object(
            gate["ink_rois"], {"bridge", "left_lobe", "right_lobe"}, f"{label}.ink_rois"
        )
        for name, value in ink.items():
            record = _require_exact_object(
                value, {"minimum_ink_pixels", "rect"}, f"{label}.ink_rois.{name}"
            )
            rect = _parse_visual_rect(record["rect"], f"{label}.ink_rois.{name}.rect")
            maximum = (rect[2] - rect[0] + 1) * (rect[3] - rect[1] + 1)
            _require_visual_int(
                record["minimum_ink_pixels"],
                f"{label}.ink_rois.{name}.minimum_ink_pixels",
                1,
                maximum,
            )
        white = _require_exact_object(gate["white_rois"], {"cleft"}, f"{label}.white_rois")
        for name, value in white.items():
            record = _require_exact_object(
                value, {"maximum_ink_pixels", "rect"}, f"{label}.white_rois.{name}"
            )
            rect = _parse_visual_rect(record["rect"], f"{label}.white_rois.{name}.rect")
            maximum = (rect[2] - rect[0] + 1) * (rect[3] - rect[1] + 1)
            _require_visual_int(
                record["maximum_ink_pixels"],
                f"{label}.white_rois.{name}.maximum_ink_pixels",
                0,
                maximum,
            )
        return gate
    if kind == "cadence":
        gate = _require_exact_object(raw, {"required_durations_ms"}, label)
        durations = gate["required_durations_ms"]
        if not isinstance(durations, list) or len(durations) != 4:
            raise RasterContractError(f"{label}: four cadence durations are required")
        for phase, value in enumerate(durations):
            _require_visual_int(value, f"{label}.required_durations_ms[{phase}]", 10, 10000)
        return gate
    if kind == "eye_sequence":
        gate = _require_exact_object(
            raw,
            {
                "closed_phase",
                "closed_phase_required_component_count",
                "closed_phase_required_height_pixels",
                "containment_rect",
                "eye_rect",
                "maximum_centroid_distance_from_p0",
                "phase_mass_ratio_min_max_from_p0",
            },
            label,
        )
        eye_rect = _parse_visual_rect(gate["eye_rect"], f"{label}.eye_rect")
        containment_rect = _parse_visual_rect(
            gate["containment_rect"], f"{label}.containment_rect"
        )
        if not (
            containment_rect[0] < eye_rect[0]
            and containment_rect[1] < eye_rect[1]
            and containment_rect[2] > eye_rect[2]
            and containment_rect[3] > eye_rect[3]
        ):
            raise RasterContractError(
                f"{label}: containment rect must guard every eye-ROI border"
            )
        ranges = gate["phase_mass_ratio_min_max_from_p0"]
        if not isinstance(ranges, list) or len(ranges) != 4:
            raise RasterContractError(f"{label}: four eye-mass ranges are required")
        for phase, pair in enumerate(ranges):
            _parse_visual_number_min_max(
                pair, f"{label}.phase_mass_ratio_min_max_from_p0[{phase}]", 2.0
            )
        _require_visual_int(gate["closed_phase"], f"{label}.closed_phase", 0, 3)
        _require_visual_int(
            gate["closed_phase_required_component_count"],
            f"{label}.closed_phase_required_component_count",
            1,
            8,
        )
        _require_visual_int(
            gate["closed_phase_required_height_pixels"],
            f"{label}.closed_phase_required_height_pixels",
            1,
            16,
        )
        _require_visual_number(
            gate["maximum_centroid_distance_from_p0"],
            f"{label}.maximum_centroid_distance_from_p0",
            0.0,
            16.0,
        )
        return gate
    if kind == "skull_freeze":
        gate = _require_exact_object(raw, {"baseline_phase", "maximum_xor_pixels", "rect"}, label)
        if gate["baseline_phase"] != 0:
            raise RasterContractError(f"{label}: skull baseline must be P0")
        _parse_visual_rect(gate["rect"], f"{label}.rect")
        _require_visual_int(gate["maximum_xor_pixels"], f"{label}.maximum_xor_pixels")
        return gate
    if kind == "ear_base_freeze":
        gate = _require_exact_object(
            raw, {"baseline_phase", "maximum_xor_pixels_each", "rects"}, label
        )
        if gate["baseline_phase"] != 0:
            raise RasterContractError(f"{label}: ear-base baseline must be P0")
        _parse_visual_named_rects(gate["rects"], {"left", "right"}, f"{label}.rects")
        _require_visual_int(
            gate["maximum_xor_pixels_each"], f"{label}.maximum_xor_pixels_each"
        )
        return gate
    if kind == "local_scale_ink":
        gate = _require_exact_object(
            raw,
            {
                "ink_ratio_min_max_from_p0",
                "maximum_left_top_bbox_expansion_pixels",
                "rect",
            },
            label,
        )
        _parse_visual_rect(gate["rect"], f"{label}.rect")
        _parse_visual_number_min_max(
            gate["ink_ratio_min_max_from_p0"], f"{label}.ink_ratio_min_max_from_p0", 2.0
        )
        _require_visual_int(
            gate["maximum_left_top_bbox_expansion_pixels"],
            f"{label}.maximum_left_top_bbox_expansion_pixels",
            0,
            8,
        )
        return gate
    raise RasterContractError(f"{label}: unsupported visual gate {kind!r}")


def _reject_duplicate_json_pairs(
    pairs: list[tuple[str, object]],
) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise RasterContractError(f"visual gate policy duplicates JSON key {key!r}")
        result[key] = value
    return result


def load_species_role_visual_gate_policy(
    path: Path,
    relative_path: str = SPECIES_ROLE_VISUAL_GATE_POLICY_RELATIVE_PATH,
) -> SpeciesRoleVisualGatePolicy:
    """Load the exact allow-listed ROI policy without inferring anatomy."""

    relative_path = _require_canonical_relative_path(relative_path, "visual gate policy")
    try:
        source_bytes = path.read_bytes()
        payload = json.loads(
            source_bytes.decode("utf-8"),
            object_pairs_hook=_reject_duplicate_json_pairs,
        )
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RasterContractError(f"cannot read visual gate policy: {error}") from error
    raw = _require_exact_object(
        payload,
        {"canvas", "connectivity", "entries", "identity_frame_sha256", "schema"},
        "visual gate policy",
    )
    if raw["schema"] != SPECIES_ROLE_VISUAL_GATE_POLICY_SCHEMA:
        raise RasterContractError("visual gate policy schema is unsupported")
    if raw["canvas"] != [HIGH_RES_FRAME_WIDTH, HIGH_RES_FRAME_HEIGHT]:
        raise RasterContractError("visual gate policy canvas must be exact 64x80")
    if raw["connectivity"] != 8:
        raise RasterContractError("visual gate policy must use eight-neighbor connectivity")
    identity_hashes = _require_exact_object(
        raw["identity_frame_sha256"], {"axolotl", "rabbit"}, "visual gate identity hashes"
    )
    parsed_identity_hashes = {
        identity: _require_lower_sha256(value, f"visual gate identity hashes/{identity}")
        for identity, value in identity_hashes.items()
    }
    entries = raw["entries"]
    if not isinstance(entries, list) or len(entries) != len(SPECIES_ROLE_VISUAL_GATE_KEYS):
        raise RasterContractError("visual gate policy must contain the exact six configured entries")
    parsed: dict[tuple[str, str], SpeciesRoleVisualGateEntry] = {}
    actual_order: list[tuple[str, str]] = []
    for index, value in enumerate(entries):
        entry = _require_exact_object(
            value, {"gates", "identity_key", "role"}, f"visual gate entries[{index}]"
        )
        identity_key = _require_semantic_name(
            entry["identity_key"], f"visual gate entries[{index}].identity_key"
        )
        role = _require_semantic_name(entry["role"], f"visual gate entries[{index}].role")
        key = (identity_key, role)
        if identity_key in PROTECTED_STARTERS or key == ("ferret", "blink"):
            raise RasterContractError(f"visual gate policy cannot configure protected {identity_key}/{role}")
        expected_kinds = _SPECIES_ROLE_VISUAL_GATE_KIND_SETS.get(key)
        if expected_kinds is None:
            raise RasterContractError(f"visual gate policy contains unapproved {identity_key}/{role}")
        gates = _require_exact_object(
            entry["gates"], expected_kinds, f"visual gate entries[{index}].gates"
        )
        for kind, gate in gates.items():
            _parse_species_role_visual_gate(
                kind, gate, f"visual gate entries[{index}].gates.{kind}"
            )
        if key in parsed:
            raise RasterContractError(f"visual gate policy duplicates {identity_key}/{role}")
        actual_order.append(key)
        parsed[key] = SpeciesRoleVisualGateEntry(
            identity_key=identity_key,
            role=role,
            identity_frame_sha256=parsed_identity_hashes[identity_key],
            gates=gates,
            entry_sha256=_canonical_json_sha256(entry),
        )
    if tuple(actual_order) != SPECIES_ROLE_VISUAL_GATE_KEYS:
        raise RasterContractError(
            "visual gate entries differ from the exact ordered allow-list"
        )
    return SpeciesRoleVisualGatePolicy(
        schema=SPECIES_ROLE_VISUAL_GATE_POLICY_SCHEMA,
        relative_path=relative_path,
        source_sha256=hashlib.sha256(source_bytes).hexdigest(),
        source_bytes=source_bytes,
        identity_frame_sha256=parsed_identity_hashes,
        entries=parsed,
    )


def species_role_visual_gate_policy_record(
    policy: SpeciesRoleVisualGatePolicy,
) -> dict[str, object]:
    _require_species_role_visual_gate_policy_integrity(policy)
    return {
        "configured_species_roles": [
            f"{identity_key}/{role}" for identity_key, role in policy.entries
        ],
        "identity_frame_sha256": dict(policy.identity_frame_sha256),
        "relative_path": policy.relative_path,
        "schema": policy.schema,
        "sha256": policy.source_sha256,
    }


def _require_species_role_visual_gate_policy_integrity(
    policy: SpeciesRoleVisualGatePolicy,
) -> None:
    """Detect mutation of the parsed policy before any ROI is consumed."""

    if (
        policy.schema != SPECIES_ROLE_VISUAL_GATE_POLICY_SCHEMA
        or policy.relative_path != SPECIES_ROLE_VISUAL_GATE_POLICY_RELATIVE_PATH
        or not _is_lower_sha256(policy.source_sha256)
        or not isinstance(policy.source_bytes, bytes)
        or hashlib.sha256(policy.source_bytes).hexdigest() != policy.source_sha256
        or set(policy.identity_frame_sha256) != {"axolotl", "rabbit"}
    ):
        raise RasterContractError("visual gate policy object drifted after parsing")
    if tuple(policy.entries) != SPECIES_ROLE_VISUAL_GATE_KEYS:
        raise RasterContractError("visual gate policy entry order drifted")
    for key, entry in policy.entries.items():
        if (
            key != (entry.identity_key, entry.role)
            or entry.identity_frame_sha256
            != policy.identity_frame_sha256.get(entry.identity_key)
            or not _is_lower_sha256(entry.identity_frame_sha256)
            or _canonical_json_sha256(
                {
                    "gates": entry.gates,
                    "identity_key": entry.identity_key,
                    "role": entry.role,
                }
            )
            != entry.entry_sha256
        ):
            raise RasterContractError(
                f"visual gate policy entry {key[0]}/{key[1]} drifted after parsing"
            )
        expected_kinds = _SPECIES_ROLE_VISUAL_GATE_KIND_SETS[key]
        if set(entry.gates) != expected_kinds:
            raise RasterContractError(
                f"visual gate policy entry {key[0]}/{key[1]} gate set drifted"
            )
        for kind, gate in entry.gates.items():
            _parse_species_role_visual_gate(
                kind,
                gate,
                f"visual gate policy entry {key[0]}/{key[1]}.{kind}",
            )
    expected_document = {
        "canvas": [HIGH_RES_FRAME_WIDTH, HIGH_RES_FRAME_HEIGHT],
        "connectivity": 8,
        "entries": [
            {
                "gates": entry.gates,
                "identity_key": entry.identity_key,
                "role": entry.role,
            }
            for entry in policy.entries.values()
        ],
        "identity_frame_sha256": dict(policy.identity_frame_sha256),
        "schema": policy.schema,
    }
    try:
        source_document = json.loads(
            policy.source_bytes.decode("utf-8"),
            object_pairs_hook=_reject_duplicate_json_pairs,
        )
    except (
        UnicodeDecodeError,
        json.JSONDecodeError,
        RasterContractError,
    ) as error:
        raise RasterContractError(
            "visual gate policy source bytes drifted after parsing"
        ) from error
    if source_document != expected_document:
        raise RasterContractError(
            "visual gate policy source document drifted after parsing"
        )


def native_region_mask_lock(
    mask: set[tuple[int, int]] | frozenset[tuple[int, int]],
) -> NativeRegionMaskLock:
    """Create a byte-exact region record without inferring a semantic region."""

    points = set(mask)
    packed = high_res_frame_bytes(points)
    return NativeRegionMaskLock(
        mask=frozenset(points),
        packed_sha256=hashlib.sha256(packed).hexdigest(),
    )


def native_region_mask_record(region: NativeRegionMaskLock) -> dict[str, object]:
    packed = high_res_frame_bytes(set(region.mask))
    actual_hash = hashlib.sha256(packed).hexdigest()
    if region.packed_sha256 != actual_hash:
        raise RasterContractError(
            "native semantic region mask differs from its pinned SHA-256"
        )
    return {
        "canvas": [HIGH_RES_FRAME_WIDTH, HIGH_RES_FRAME_HEIGHT],
        "encoding": NATIVE_REGION_MASK_ENCODING,
        "packed_hex": packed.hex(),
        "packed_sha256": actual_hash,
        "schema": NATIVE_REGION_MASK_SCHEMA,
    }


def _parse_native_region_mask(raw: object, label: str) -> NativeRegionMaskLock:
    expected = {
        "canvas",
        "encoding",
        "packed_hex",
        "packed_sha256",
        "schema",
    }
    if not isinstance(raw, dict) or set(raw) != expected:
        missing = sorted(expected - set(raw)) if isinstance(raw, dict) else sorted(expected)
        unexpected = sorted(set(raw) - expected) if isinstance(raw, dict) else []
        raise RasterContractError(
            f"{label}: exact native region mask is required; "
            f"missing={missing} unexpected={unexpected}"
        )
    if raw["schema"] != NATIVE_REGION_MASK_SCHEMA:
        raise RasterContractError(
            f"{label}: native region mask schema must be {NATIVE_REGION_MASK_SCHEMA}"
        )
    if raw["canvas"] != [HIGH_RES_FRAME_WIDTH, HIGH_RES_FRAME_HEIGHT]:
        raise RasterContractError(f"{label}: native region mask canvas drifted")
    if raw["encoding"] != NATIVE_REGION_MASK_ENCODING:
        raise RasterContractError(f"{label}: native region mask encoding drifted")
    packed_hex = raw["packed_hex"]
    if (
        not isinstance(packed_hex, str)
        or len(packed_hex) != HIGH_RES_FRAME_BYTES * 2
        or packed_hex != packed_hex.lower()
        or any(character not in "0123456789abcdef" for character in packed_hex)
    ):
        raise RasterContractError(f"{label}: malformed exact native mask bytes")
    packed = bytes.fromhex(packed_hex)
    expected_hash = _require_lower_sha256(
        raw["packed_sha256"], f"{label}/packed_sha256"
    )
    actual_hash = hashlib.sha256(packed).hexdigest()
    if actual_hash != expected_hash:
        raise RasterContractError(
            f"{label}: native semantic region mask SHA-256 drifted"
        )
    return NativeRegionMaskLock(
        mask=frozenset(decode_high_res_frame_bytes(packed)),
        packed_sha256=actual_hash,
    )


def _identity_reference_record(
    reference: ImmutableIdentityReference,
) -> dict[str, object]:
    return {
        "frame_sha256": reference.frame_sha256,
        "identity_key": reference.identity_key,
        "kind": reference.kind,
        "relative_path": reference.relative_path,
        "source_sha256": reference.source_sha256,
    }


def _edit_target_reference_record(
    reference: ImmutableEditTargetReference,
) -> dict[str, object]:
    return {
        "accepted_composited_frame_sha256": (
            reference.accepted_composited_frame_sha256
        ),
        "identity_key": reference.identity_key,
        "kind": reference.kind,
        "phase": reference.phase,
        "registered_frame_sha256": reference.registered_frame_sha256,
        "relative_path": reference.relative_path,
        "role": reference.role,
        "source_sha256": reference.source_sha256,
    }


def _native_grid_reference_record(
    reference: NativeGridConditioningReference,
) -> dict[str, object]:
    return {
        "derivation": reference.derivation,
        "edit_target": reference.edit_target,
        "grid_png_sha256": reference.grid_png_sha256,
        "grid_relative_path": reference.grid_relative_path,
        "image_number": reference.image_number,
        "kind": reference.kind,
        "p0_packed_sha256": reference.p0_packed_sha256,
        "read_only": reference.read_only,
        "role_registration_sha256": reference.role_registration_sha256,
        "roundtrip_packed_sha256": reference.roundtrip_packed_sha256,
        "schema": reference.schema,
        "source_png_sha256": reference.source_png_sha256,
        "source_relative_path": reference.source_relative_path,
        "transform_sha256": reference.transform_sha256,
    }


def _preauthorization_reference_record(
    reference: FrozenPhasePreauthorizationReference,
) -> dict[str, object]:
    return {
        "allowed_change_region_sha256": (
            reference.allowed_change_region_sha256
        ),
        "edit_target_kind": reference.edit_target_kind,
        "kind": reference.kind,
        "relative_path": reference.relative_path,
        "source_sha256": reference.source_sha256,
        "storyboard_sha256": reference.storyboard_sha256,
    }


def _generated_asset_record(asset: GeneratedPhaseAsset) -> dict[str, object]:
    return {
        "imported_candidate_frame_sha256": (
            asset.imported_candidate_frame_sha256
        ),
        "layout": asset.layout,
        "relative_path": asset.relative_path,
        "registered_candidate_frame_sha256": (
            asset.registered_candidate_frame_sha256
        ),
        "source_sha256": asset.source_sha256,
    }


def generated_role_registration_record(
    registration: GeneratedRoleRegistrationLock,
) -> dict[str, object]:
    return {
        "derivation": registration.derivation,
        "output_offset": list(registration.output_offset),
        "p0_unregistered_floor_y": registration.p0_unregistered_floor_y,
        "schema": registration.schema,
    }


def generated_role_registration_sha256(
    registration: GeneratedRoleRegistrationLock,
) -> str:
    payload = json.dumps(
        generated_role_registration_record(registration),
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    ).encode("ascii")
    return hashlib.sha256(payload).hexdigest()


def _frozen_region_record(region: FrozenSemanticRegion) -> dict[str, object]:
    return {
        "kind": region.kind,
        "maximum_changed_pixels": region.maximum_changed_pixels,
        "name": region.name,
        "region": native_region_mask_record(region.region),
    }


def _phase_semantic_record(
    phase: GeneratedPhaseSemanticLock,
    schema: str,
) -> dict[str, object]:
    record = {
        "allowed_change_region": native_region_mask_record(
            phase.allowed_change_region
        ),
        "frozen_regions": [
            _frozen_region_record(region) for region in phase.frozen_regions
        ],
        "composition_baseline_frame_sha256": (
            phase.composition_baseline_frame_sha256
        ),
        "composition_mode": phase.composition_mode,
        "composited_frame_sha256": phase.composited_frame_sha256,
        "edit_target_reference": _edit_target_reference_record(
            phase.edit_target_reference
        ),
        "generated_asset": _generated_asset_record(phase.generated_asset),
        "identity_reference": _identity_reference_record(
            phase.identity_reference
        ),
        "maximum_out_of_region_changed_pixels": (
            phase.maximum_out_of_region_changed_pixels
        ),
        "phase": phase.phase,
        "preauthorization_reference": _preauthorization_reference_record(
            phase.preauthorization_reference
        ),
        "semantic_baseline": phase.semantic_baseline,
    }
    if schema == GENERATED_ACTION_SEMANTIC_SCHEMA:
        record["generation_reference_mode"] = phase.generation_reference_mode
        record["native_grid_reference"] = (
            None
            if phase.native_grid_reference is None
            else _native_grid_reference_record(phase.native_grid_reference)
        )
    elif schema == LEGACY_GENERATED_ACTION_SEMANTIC_SCHEMA:
        expected_legacy_mode = (
            P0_GENERATION_REFERENCE_MODE
            if phase.phase == 0
            else TWO_REFERENCE_GENERATION_MODE
        )
        if (
            phase.generation_reference_mode != expected_legacy_mode
            or phase.native_grid_reference is not None
        ):
            raise RasterContractError(
                "legacy semantic-v3 can serialize only truthful two-reference "
                "lineage; Image 3 requires semantic-v4"
            )
    else:
        raise RasterContractError(
            f"unsupported generated action semantic schema {schema!r}"
        )
    return record


def _motion_landmark_record(landmark: MotionLandmarkLock) -> dict[str, object]:
    return {
        "minimum_changed_pixels": landmark.minimum_changed_pixels,
        "name": landmark.name,
        "region": native_region_mask_record(landmark.region),
    }


def _role_pose_identity_landmark_record(
    landmark: RolePoseIdentityLandmarkLock,
) -> dict[str, object]:
    return {
        "identity_region": native_region_mask_record(landmark.identity_region),
        "maximum_component_count_delta": landmark.maximum_component_count_delta,
        "minimum_ink_pixels": landmark.minimum_ink_pixels,
        "minimum_ink_retention_per_mille": (
            landmark.minimum_ink_retention_per_mille
        ),
        "name": landmark.name,
        "role_pose_region": native_region_mask_record(landmark.role_pose_region),
    }


def generated_action_semantic_contract_record(
    contract: GeneratedActionSemanticContract,
) -> dict[str, object]:
    return {
        "roles": [
            {
                "baseline_policy": role.baseline_policy,
                "contact_policy": role.contact_policy,
                "maximum_contact_changed_pixels_per_phase": (
                    role.maximum_contact_changed_pixels_per_phase
                ),
                "maximum_role_pose_component_count_delta": (
                    role.maximum_role_pose_component_count_delta
                ),
                "motion_landmarks": [
                    _motion_landmark_record(landmark)
                    for landmark in role.motion_landmarks
                ],
                "phases": [
                    _phase_semantic_record(phase, contract.schema)
                    for phase in role.phases
                ],
                "role": role.role,
                "role_registration": generated_role_registration_record(
                    role.role_registration
                ),
                "role_registration_sha256": role.role_registration_sha256,
                "role_pose_baseline_frame_sha256": (
                    role.role_pose_baseline_frame_sha256
                ),
                "role_pose_identity_landmarks": [
                    _role_pose_identity_landmark_record(landmark)
                    for landmark in role.role_pose_identity_landmarks
                ],
            }
            for role in contract.roles
        ],
        "schema": contract.schema,
    }


def generated_action_semantic_contract_sha256(
    contract: GeneratedActionSemanticContract,
) -> str:
    payload = json.dumps(
        generated_action_semantic_contract_record(contract),
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    ).encode("ascii")
    return hashlib.sha256(payload).hexdigest()


def _parse_identity_reference(
    raw: object,
    label: str,
    identity_key: str,
    identity_source_sha256: str,
    identity_frame_sha256: str,
) -> ImmutableIdentityReference:
    expected = {
        "frame_sha256",
        "identity_key",
        "kind",
        "relative_path",
        "source_sha256",
    }
    if not isinstance(raw, dict) or set(raw) != expected:
        raise RasterContractError(
            f"{label}: every phase must pin exactly one immutable identity reference"
        )
    if raw["kind"] != "immutable-approved-identity-source":
        raise RasterContractError(
            f"{label}: generated-phase chaining is forbidden; the only generation "
            "reference is the immutable approved identity source"
        )
    relative_path = _require_canonical_relative_path(
        raw["relative_path"], f"{label}/relative_path"
    )
    if relative_path != "identity.png":
        raise RasterContractError(
            f"{label}: generated-phase chaining is forbidden; reference path must "
            "be identity.png"
        )
    source_hash = _require_lower_sha256(
        raw["source_sha256"], f"{label}/source_sha256"
    )
    frame_hash = _require_lower_sha256(
        raw["frame_sha256"], f"{label}/frame_sha256"
    )
    if (
        raw["identity_key"] != identity_key
        or source_hash != identity_source_sha256
        or frame_hash != identity_frame_sha256
    ):
        raise RasterContractError(
            f"{label}: immutable identity reference differs from the approved lock"
        )
    return ImmutableIdentityReference(
        kind="immutable-approved-identity-source",
        relative_path="identity.png",
        identity_key=identity_key,
        source_sha256=source_hash,
        frame_sha256=frame_hash,
    )


def _parse_generated_asset(
    raw: object, label: str, role: str, phase: int
) -> GeneratedPhaseAsset:
    expected = {
        "imported_candidate_frame_sha256",
        "layout",
        "relative_path",
        "registered_candidate_frame_sha256",
        "source_sha256",
    }
    if not isinstance(raw, dict) or set(raw) != expected:
        raise RasterContractError(f"{label}: generated asset pin is missing or malformed")
    layout = raw["layout"]
    if layout not in GENERATED_ASSET_LAYOUTS:
        raise RasterContractError(f"{label}: unsupported generated source layout")
    relative_path = _require_canonical_relative_path(
        raw["relative_path"], f"{label}/relative_path"
    )
    expected_path = f"{role}/{phase:02d}.png"
    if relative_path != expected_path:
        raise RasterContractError(
            f"{label}: generated asset path must be {expected_path!r}; phase "
            "aliasing/chaining is forbidden"
        )
    return GeneratedPhaseAsset(
        layout=layout,
        relative_path=relative_path,
        source_sha256=_require_lower_sha256(
            raw["source_sha256"], f"{label}/source_sha256"
        ),
        imported_candidate_frame_sha256=_require_lower_sha256(
            raw["imported_candidate_frame_sha256"],
            f"{label}/imported_candidate_frame_sha256",
        ),
        registered_candidate_frame_sha256=_require_lower_sha256(
            raw["registered_candidate_frame_sha256"],
            f"{label}/registered_candidate_frame_sha256",
        ),
    )


def _expected_edit_target_kind(_baseline_policy: str, phase: int) -> str:
    if phase == 0:
        return "immutable-approved-identity-source"
    return "immutable-accepted-role-phase-0"


def _parse_edit_target_reference(
    raw: object,
    label: str,
    role: str,
    phase: int,
    baseline_policy: str,
    identity_key: str,
    identity_source_sha256: str,
    identity_frame_sha256: str,
) -> ImmutableEditTargetReference:
    expected = {
        "accepted_composited_frame_sha256",
        "identity_key",
        "kind",
        "phase",
        "registered_frame_sha256",
        "relative_path",
        "role",
        "source_sha256",
    }
    if not isinstance(raw, dict) or set(raw) != expected:
        raise RasterContractError(
            f"{label}: exact immutable edit-target reference is required"
        )
    expected_kind = _expected_edit_target_kind(baseline_policy, phase)
    if raw["kind"] != expected_kind:
        if phase == 0:
            raise RasterContractError(
                f"{label}: immutable role phase 0 must target the approved identity"
            )
        raise RasterContractError(
            f"{label}: phases 1..3 must target the same immutable role phase 0"
        )
    relative_path = _require_canonical_relative_path(
        raw["relative_path"], f"{label}/relative_path"
    )
    if expected_kind == "immutable-approved-identity-source":
        expected_path = "identity.png"
        expected_role = "identity"
        expected_phase = -1
        source_hash = identity_source_sha256
        registered_hash = identity_frame_sha256
        accepted_hash = identity_frame_sha256
    else:
        expected_path = f"{role}/00.png"
        expected_role = role
        expected_phase = 0
        source_hash = _require_lower_sha256(
            raw["source_sha256"], f"{label}/source_sha256"
        )
        registered_hash = _require_lower_sha256(
            raw["registered_frame_sha256"],
            f"{label}/registered_frame_sha256",
        )
        accepted_hash = _require_lower_sha256(
            raw["accepted_composited_frame_sha256"],
            f"{label}/accepted_composited_frame_sha256",
        )
    if (
        raw["identity_key"] != identity_key
        or relative_path != expected_path
        or raw["role"] != expected_role
        or raw["phase"] != expected_phase
    ):
        raise RasterContractError(
            f"{label}: edit target path/owner differs from its immutable base"
        )
    if expected_kind == "immutable-approved-identity-source" and (
        raw["source_sha256"] != source_hash
        or raw["registered_frame_sha256"] != registered_hash
        or raw["accepted_composited_frame_sha256"] != accepted_hash
    ):
        raise RasterContractError(
            f"{label}: identity edit target differs from the approved identity"
        )
    return ImmutableEditTargetReference(
        kind=expected_kind,
        relative_path=relative_path,
        identity_key=identity_key,
        role=expected_role,
        phase=expected_phase,
        source_sha256=source_hash,
        registered_frame_sha256=registered_hash,
        accepted_composited_frame_sha256=accepted_hash,
    )


def _parse_native_grid_reference(
    raw: object,
    label: str,
    role: str,
    phase: int,
    generation_reference_mode: str,
    transform_sha256: str,
    role_registration_sha256: str,
) -> NativeGridConditioningReference | None:
    if phase == 0:
        if generation_reference_mode != P0_GENERATION_REFERENCE_MODE or raw is not None:
            raise RasterContractError(
                f"{label}: role P0 must use exactly two identity/edit references; "
                "Image 3 is forbidden"
            )
        return None
    if generation_reference_mode == TWO_REFERENCE_GENERATION_MODE:
        if raw is not None:
            raise RasterContractError(
                f"{label}: two-reference phase cannot claim an Image 3"
            )
        return None
    if generation_reference_mode != THREE_REFERENCE_GENERATION_MODE:
        raise RasterContractError(
            f"{label}: unsupported generation-reference mode"
        )
    expected = {
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
    }
    if not isinstance(raw, dict) or set(raw) != expected:
        raise RasterContractError(
            f"{label}: three-reference phase requires one exact native-grid "
            "conditioning reference"
        )
    if (
        raw["schema"] != NATIVE_GRID_REFERENCE_SCHEMA
        or raw["kind"] != NATIVE_GRID_REFERENCE_KIND
        or raw["image_number"] != 3
        or raw["read_only"] is not True
        or raw["edit_target"] is not False
        or raw["derivation"] != NATIVE_GRID_REFERENCE_DERIVATION
    ):
        raise RasterContractError(
            f"{label}: Image 3 must be the read-only deterministic native-grid "
            "conditioning reference"
        )
    source_path = _require_canonical_relative_path(
        raw["source_relative_path"], f"{label}/source_relative_path"
    )
    grid_path = _require_canonical_relative_path(
        raw["grid_relative_path"], f"{label}/grid_relative_path"
    )
    if source_path != f"{role}/00.png" or grid_path != (
        f"{NATIVE_GRID_REFERENCE_DIRECTORY}/{role}/00.png"
    ):
        raise RasterContractError(
            f"{label}: Image 3 must derive from the same role P0 and cannot be "
            "phase-specific or chained"
        )
    reference_transform_hash = _require_lower_sha256(
        raw["transform_sha256"], f"{label}/transform_sha256"
    )
    reference_registration_hash = _require_lower_sha256(
        raw["role_registration_sha256"],
        f"{label}/role_registration_sha256",
    )
    if reference_transform_hash != transform_sha256:
        raise RasterContractError(
            f"{label}: Image 3 import transform differs from the approved lock"
        )
    if reference_registration_hash != role_registration_sha256:
        raise RasterContractError(
            f"{label}: Image 3 role registration differs from its role lock"
        )
    p0_hash = _require_lower_sha256(
        raw["p0_packed_sha256"], f"{label}/p0_packed_sha256"
    )
    roundtrip_hash = _require_lower_sha256(
        raw["roundtrip_packed_sha256"], f"{label}/roundtrip_packed_sha256"
    )
    if roundtrip_hash != p0_hash:
        raise RasterContractError(
            f"{label}: Image 3 BOX round trip must equal the exact accepted P0"
        )
    return NativeGridConditioningReference(
        schema=NATIVE_GRID_REFERENCE_SCHEMA,
        kind=NATIVE_GRID_REFERENCE_KIND,
        image_number=3,
        read_only=True,
        edit_target=False,
        source_relative_path=source_path,
        source_png_sha256=_require_lower_sha256(
            raw["source_png_sha256"], f"{label}/source_png_sha256"
        ),
        grid_relative_path=grid_path,
        grid_png_sha256=_require_lower_sha256(
            raw["grid_png_sha256"], f"{label}/grid_png_sha256"
        ),
        p0_packed_sha256=p0_hash,
        roundtrip_packed_sha256=roundtrip_hash,
        transform_sha256=reference_transform_hash,
        role_registration_sha256=reference_registration_hash,
        derivation=NATIVE_GRID_REFERENCE_DERIVATION,
    )


def _parse_preauthorization_reference(
    raw: object,
    label: str,
    role: str,
    phase: int,
    edit_target_kind: str,
    allowed_change_region_sha256: str,
) -> FrozenPhasePreauthorizationReference:
    expected = {
        "allowed_change_region_sha256",
        "edit_target_kind",
        "kind",
        "relative_path",
        "source_sha256",
        "storyboard_sha256",
    }
    if not isinstance(raw, dict) or set(raw) != expected:
        raise RasterContractError(
            f"{label}: immutable pre-generation mask reference is required"
        )
    if raw["kind"] != "immutable-pre-generation-phase-mask":
        raise RasterContractError(
            f"{label}: semantic mask must preexist generation and remain frozen"
        )
    relative_path = _require_canonical_relative_path(
        raw["relative_path"], f"{label}/relative_path"
    )
    expected_path = f"preauthorization/{role}/{phase:02d}.json"
    mask_hash = _require_lower_sha256(
        raw["allowed_change_region_sha256"],
        f"{label}/allowed_change_region_sha256",
    )
    if (
        relative_path != expected_path
        or mask_hash != allowed_change_region_sha256
        or raw["edit_target_kind"] != edit_target_kind
    ):
        raise RasterContractError(
            f"{label}: preauthorization path, target, or frozen mask drifted"
        )
    return FrozenPhasePreauthorizationReference(
        kind="immutable-pre-generation-phase-mask",
        relative_path=relative_path,
        source_sha256=_require_lower_sha256(
            raw["source_sha256"], f"{label}/source_sha256"
        ),
        storyboard_sha256=_require_lower_sha256(
            raw["storyboard_sha256"], f"{label}/storyboard_sha256"
        ),
        allowed_change_region_sha256=mask_hash,
        edit_target_kind=edit_target_kind,
    )


def _parse_frozen_region(raw: object, label: str) -> FrozenSemanticRegion:
    expected = {"kind", "maximum_changed_pixels", "name", "region"}
    if not isinstance(raw, dict) or set(raw) != expected:
        raise RasterContractError(f"{label}: exact frozen semantic region is required")
    kind = raw["kind"]
    if kind not in {"planted-contact", "protected-identity-landmark"}:
        raise RasterContractError(f"{label}: unsupported frozen region kind")
    if raw["maximum_changed_pixels"] != 0:
        raise RasterContractError(
            f"{label}: planted contacts and hard anatomy have zero pixel budget"
        )
    region = _parse_native_region_mask(raw["region"], f"{label}/region")
    minimum = (
        1
        if kind == "planted-contact"
        else GENERATED_MIN_FROZEN_LANDMARK_PIXELS
    )
    if len(region.mask) < minimum:
        raise RasterContractError(f"{label}: frozen region is empty or trivial")
    return FrozenSemanticRegion(
        kind=kind,
        name=_require_semantic_name(raw["name"], f"{label}/name"),
        region=region,
        maximum_changed_pixels=0,
    )


def _parse_phase_semantic(
    raw: object,
    label: str,
    role: str,
    phase: int,
    baseline_policy: str,
    identity_key: str,
    identity_source_sha256: str,
    identity_frame_sha256: str,
    semantic_schema: str,
    transform_sha256: str,
    role_registration_sha256: str,
) -> GeneratedPhaseSemanticLock:
    expected = {
        "allowed_change_region",
        "composition_baseline_frame_sha256",
        "composition_mode",
        "composited_frame_sha256",
        "edit_target_reference",
        "frozen_regions",
        "generated_asset",
        "identity_reference",
        "maximum_out_of_region_changed_pixels",
        "phase",
        "preauthorization_reference",
        "semantic_baseline",
    }
    if semantic_schema == GENERATED_ACTION_SEMANTIC_SCHEMA:
        expected |= {"generation_reference_mode", "native_grid_reference"}
    elif semantic_schema != LEGACY_GENERATED_ACTION_SEMANTIC_SCHEMA:
        raise RasterContractError(f"{label}: unsupported semantic schema")
    if not isinstance(raw, dict) or set(raw) != expected or raw.get("phase") != phase:
        raise RasterContractError(f"{label}: exact phase semantic lock is required")
    generation_reference_mode = (
        raw["generation_reference_mode"]
        if semantic_schema == GENERATED_ACTION_SEMANTIC_SCHEMA
        else (
            P0_GENERATION_REFERENCE_MODE
            if phase == 0
            else TWO_REFERENCE_GENERATION_MODE
        )
    )
    native_grid_reference = _parse_native_grid_reference(
        raw.get("native_grid_reference"),
        f"{label}/native-grid-reference",
        role,
        phase,
        generation_reference_mode,
        transform_sha256,
        role_registration_sha256,
    )
    allowed = _parse_native_region_mask(
        raw["allowed_change_region"], f"{label}/allowed-change"
    )
    if not allowed.mask:
        raise RasterContractError(f"{label}: allowed-change region cannot be empty")
    if any(y in HIGH_RES_BOTTOM_GUARD_ROWS for _x, y in allowed.mask):
        raise RasterContractError(
            f"{label}: allowed-change region enters blank format-v2 guard rows"
        )
    if raw["composition_mode"] != GENERATED_COMPOSITION_MODE:
        raise RasterContractError(
            f"{label}: generated frame must use deterministic bounded composition"
        )
    budget = raw["maximum_out_of_region_changed_pixels"]
    if budget != GENERATED_MAX_OUT_OF_REGION_PIXELS:
        raise RasterContractError(
            f"{label}: production out-of-region budget must be exactly zero"
        )
    expected_baseline = (
        (
            "approved-identity"
            if baseline_policy == "identity-anchored"
            else "approved-identity-pose-gate"
        )
        if phase == 0
        else "immutable-role-phase-0"
    )
    if raw["semantic_baseline"] != expected_baseline:
        raise RasterContractError(
            f"{label}: semantic baseline must be {expected_baseline}; role phase 0 "
            "is one immutable star base and phase chaining is forbidden"
        )
    frozen_raw = raw["frozen_regions"]
    if not isinstance(frozen_raw, list):
        raise RasterContractError(f"{label}: frozen regions must be a list")
    frozen = tuple(
        _parse_frozen_region(record, f"{label}/frozen/{index}")
        for index, record in enumerate(frozen_raw)
    )
    kinds = {region.kind for region in frozen}
    names = [region.name for region in frozen]
    if kinds != {"planted-contact", "protected-identity-landmark"}:
        raise RasterContractError(
            f"{label}: both planted-contact and protected-identity-landmark "
            "frozen masks are required"
        )
    if len(names) != len(set(names)):
        raise RasterContractError(f"{label}: frozen region names must be unique")
    edit_target = _parse_edit_target_reference(
        raw["edit_target_reference"],
        f"{label}/edit-target-reference",
        role,
        phase,
        baseline_policy,
        identity_key,
        identity_source_sha256,
        identity_frame_sha256,
    )
    generated_asset = _parse_generated_asset(
        raw["generated_asset"], f"{label}/generated-asset", role, phase
    )
    if (
        generated_asset.layout == GENERATED_IDENTITY_BASELINE_ASSET_LAYOUT
        and phase != 0
    ):
        raise RasterContractError(
            f"{label}: immutable identity baseline copy is allowed only for a "
            "role P0"
        )
    return GeneratedPhaseSemanticLock(
        phase=phase,
        semantic_baseline=expected_baseline,
        identity_reference=_parse_identity_reference(
            raw["identity_reference"],
            f"{label}/identity-reference",
            identity_key,
            identity_source_sha256,
            identity_frame_sha256,
        ),
        edit_target_reference=edit_target,
        preauthorization_reference=_parse_preauthorization_reference(
            raw["preauthorization_reference"],
            f"{label}/preauthorization-reference",
            role,
            phase,
            edit_target.kind,
            allowed.packed_sha256,
        ),
        generated_asset=generated_asset,
        allowed_change_region=allowed,
        composition_mode=GENERATED_COMPOSITION_MODE,
        composition_baseline_frame_sha256=_require_lower_sha256(
            raw["composition_baseline_frame_sha256"],
            f"{label}/composition_baseline_frame_sha256",
        ),
        composited_frame_sha256=_require_lower_sha256(
            raw["composited_frame_sha256"],
            f"{label}/composited_frame_sha256",
        ),
        maximum_out_of_region_changed_pixels=budget,
        frozen_regions=frozen,
        generation_reference_mode=generation_reference_mode,
        native_grid_reference=native_grid_reference,
    )


def _parse_motion_landmark(raw: object, label: str) -> MotionLandmarkLock:
    expected = {"minimum_changed_pixels", "name", "region"}
    if not isinstance(raw, dict) or set(raw) != expected:
        raise RasterContractError(f"{label}: exact motion landmark is required")
    region = _parse_native_region_mask(raw["region"], f"{label}/region")
    minimum = raw["minimum_changed_pixels"]
    if (
        not isinstance(minimum, int)
        or isinstance(minimum, bool)
        or minimum < GENERATED_MIN_MOTION_LANDMARK_PIXELS
        or minimum > len(region.mask)
    ):
        raise RasterContractError(
            f"{label}: motion changed-pixel minimum must be at least "
            f"{GENERATED_MIN_MOTION_LANDMARK_PIXELS} and fit its exact mask"
        )
    return MotionLandmarkLock(
        name=_require_semantic_name(raw["name"], f"{label}/name"),
        region=region,
        minimum_changed_pixels=minimum,
    )


def _parse_role_pose_identity_landmark(
    raw: object, label: str
) -> RolePoseIdentityLandmarkLock:
    expected = {
        "identity_region",
        "maximum_component_count_delta",
        "minimum_ink_pixels",
        "minimum_ink_retention_per_mille",
        "name",
        "role_pose_region",
    }
    if not isinstance(raw, dict) or set(raw) != expected:
        raise RasterContractError(
            f"{label}: exact identity-to-role-pose landmark is required"
        )
    identity_region = _parse_native_region_mask(
        raw["identity_region"], f"{label}/identity-region"
    )
    role_pose_region = _parse_native_region_mask(
        raw["role_pose_region"], f"{label}/role-pose-region"
    )
    if (
        len(identity_region.mask) < GENERATED_MIN_FROZEN_LANDMARK_PIXELS
        or len(role_pose_region.mask) < GENERATED_MIN_FROZEN_LANDMARK_PIXELS
    ):
        raise RasterContractError(f"{label}: pose landmark regions are trivial")
    minimum_ink = raw["minimum_ink_pixels"]
    if (
        not isinstance(minimum_ink, int)
        or isinstance(minimum_ink, bool)
        or minimum_ink < GENERATED_MIN_FROZEN_LANDMARK_PIXELS
        or minimum_ink > min(len(identity_region.mask), len(role_pose_region.mask))
    ):
        raise RasterContractError(f"{label}: invalid pose-landmark ink minimum")
    retention = raw["minimum_ink_retention_per_mille"]
    if (
        not isinstance(retention, int)
        or isinstance(retention, bool)
        or not 500 <= retention <= 1000
    ):
        raise RasterContractError(
            f"{label}: pose-landmark ink retention must be 500..1000 per mille"
        )
    component_delta = raw["maximum_component_count_delta"]
    if (
        not isinstance(component_delta, int)
        or isinstance(component_delta, bool)
        or not 0 <= component_delta <= 2
    ):
        raise RasterContractError(
            f"{label}: pose-landmark component delta must be 0..2"
        )
    return RolePoseIdentityLandmarkLock(
        name=_require_semantic_name(raw["name"], f"{label}/name"),
        identity_region=identity_region,
        role_pose_region=role_pose_region,
        minimum_ink_pixels=minimum_ink,
        minimum_ink_retention_per_mille=retention,
        maximum_component_count_delta=component_delta,
    )


def _parse_generated_role_registration(
    raw: object,
    raw_sha256: object,
    label: str,
    baseline_policy: str,
) -> tuple[GeneratedRoleRegistrationLock, str]:
    expected = {
        "derivation",
        "output_offset",
        "p0_unregistered_floor_y",
        "schema",
    }
    if not isinstance(raw, dict) or set(raw) != expected:
        raise RasterContractError(
            f"{label}: exact role-level output registration is required"
        )
    if raw["schema"] != GENERATED_ROLE_REGISTRATION_SCHEMA:
        raise RasterContractError(
            f"{label}: role registration schema must be "
            f"{GENERATED_ROLE_REGISTRATION_SCHEMA}"
        )
    offset = raw["output_offset"]
    floor = raw["p0_unregistered_floor_y"]
    if (
        not isinstance(offset, list)
        or len(offset) != 2
        or any(type(value) is not int for value in offset)
        or type(floor) is not int
    ):
        raise RasterContractError(f"{label}: malformed role output registration")
    registration = GeneratedRoleRegistrationLock(
        schema=GENERATED_ROLE_REGISTRATION_SCHEMA,
        derivation=raw["derivation"],
        output_offset=(offset[0], offset[1]),
        p0_unregistered_floor_y=floor,
    )
    if baseline_policy == "identity-anchored":
        if (
            registration.derivation != "identity-anchored-zero-offset"
            or registration.output_offset != (0, 0)
            or registration.p0_unregistered_floor_y != HIGH_RES_FLOOR_Y
        ):
            raise RasterContractError(
                f"{label}: identity-anchored role cannot drift from the identity "
                "transform/offset"
            )
    else:
        dx, dy = registration.output_offset
        if (
            registration.derivation
            != "role-p0-fixed-dx-explicit-dy-floor-derived"
            or abs(dx) > GENERATED_ROLE_OUTPUT_OFFSET_MAXIMUM
            or abs(dy) > GENERATED_ROLE_OUTPUT_OFFSET_MAXIMUM
            or not 0
            <= registration.p0_unregistered_floor_y
            < HIGH_RES_FRAME_HEIGHT
            or dy
            != HIGH_RES_FLOOR_Y - registration.p0_unregistered_floor_y
        ):
            raise RasterContractError(
                f"{label}: role-P0 offset must be one bounded role-level lock; "
                "dy is exactly floor-derived and no per-phase override is allowed"
            )
    expected_hash = _require_lower_sha256(raw_sha256, f"{label}/sha256")
    actual_hash = generated_role_registration_sha256(registration)
    if actual_hash != expected_hash:
        raise RasterContractError(f"{label}: role registration SHA-256 drifted")
    return registration, actual_hash


def _parse_generated_action_semantic_contract(
    raw: object,
    identity_key: str,
    identity_source_sha256: str,
    identity_frame_sha256: str,
    expected_schema: str,
    transform_sha256: str,
) -> GeneratedActionSemanticContract:
    expected = {"roles", "schema"}
    if not isinstance(raw, dict) or set(raw) != expected:
        raise RasterContractError(
            f"{identity_key}: generated action semantic contract is missing or malformed"
        )
    if raw["schema"] != expected_schema:
        raise RasterContractError(
            f"{identity_key}: generated action semantic schema must be "
            f"{expected_schema}"
        )
    records = raw["roles"]
    if not isinstance(records, list) or not records:
        raise RasterContractError(f"{identity_key}: semantic roles cannot be empty")
    canonical_role_order = {role.name: index for index, role in enumerate(base.ROLE_SPECS)}
    roles: list[GeneratedRoleSemanticLock] = []
    for raw_role in records:
        if not isinstance(raw_role, dict) or set(raw_role) != {
            "baseline_policy",
            "contact_policy",
            "maximum_contact_changed_pixels_per_phase",
            "maximum_role_pose_component_count_delta",
            "motion_landmarks",
            "phases",
            "role",
            "role_registration",
            "role_registration_sha256",
            "role_pose_baseline_frame_sha256",
            "role_pose_identity_landmarks",
        }:
            raise RasterContractError(
                f"{identity_key}: malformed generated role semantic lock"
            )
        role = raw_role["role"]
        if not isinstance(role, str) or role not in canonical_role_order:
            raise RasterContractError(f"{identity_key}: unknown semantic action role")
        baseline_policy = raw_role["baseline_policy"]
        expected_baseline_policy = GENERATED_ROLE_BASELINE_POLICY[role]
        if baseline_policy != expected_baseline_policy:
            raise RasterContractError(
                f"{identity_key}/{role}: baseline policy must be "
                f"{expected_baseline_policy}"
            )
        registration, registration_hash = _parse_generated_role_registration(
            raw_role["role_registration"],
            raw_role["role_registration_sha256"],
            f"{identity_key}/{role}/role-registration",
            baseline_policy,
        )
        contact_policy = raw_role["contact_policy"]
        if contact_policy not in GENERATED_ROLE_CONTACT_POLICY_CAPABILITIES[role]:
            raise RasterContractError(
                f"{identity_key}/{role}: contact policy {contact_policy!r} is not "
                "an authorized storyboard capability for this action"
            )
        maximum_contact_changes = raw_role[
            "maximum_contact_changed_pixels_per_phase"
        ]
        contact_ceiling = GENERATED_CONTACT_POLICY_MAXIMUMS[contact_policy]
        contact_floor = 0 if contact_ceiling == 0 else 1
        if (
            not isinstance(maximum_contact_changes, int)
            or isinstance(maximum_contact_changes, bool)
            or not contact_floor <= maximum_contact_changes <= contact_ceiling
        ):
            raise RasterContractError(
                f"{identity_key}/{role}: contact-change maximum for "
                f"{contact_policy} must be {contact_floor}..{contact_ceiling}"
            )
        baseline_frame_hash = _require_lower_sha256(
            raw_role["role_pose_baseline_frame_sha256"],
            f"{identity_key}/{role}/role_pose_baseline_frame_sha256",
        )
        maximum_role_component_delta = raw_role[
            "maximum_role_pose_component_count_delta"
        ]
        if (
            not isinstance(maximum_role_component_delta, int)
            or isinstance(maximum_role_component_delta, bool)
            or not 0 <= maximum_role_component_delta <= 2
        ):
            raise RasterContractError(
                f"{identity_key}/{role}: role-pose component delta must be 0..2"
            )
        pose_landmarks_raw = raw_role["role_pose_identity_landmarks"]
        if not isinstance(pose_landmarks_raw, list) or not pose_landmarks_raw:
            raise RasterContractError(
                f"{identity_key}/{role}: identity-to-role-pose landmark gates are "
                "required"
            )
        pose_landmarks = tuple(
            _parse_role_pose_identity_landmark(
                landmark, f"{identity_key}/{role}/pose-landmark/{index}"
            )
            for index, landmark in enumerate(pose_landmarks_raw)
        )
        pose_landmark_names = [landmark.name for landmark in pose_landmarks]
        if len(pose_landmark_names) != len(set(pose_landmark_names)):
            raise RasterContractError(
                f"{identity_key}/{role}: pose landmark names must be unique"
            )
        phases_raw = raw_role["phases"]
        if not isinstance(phases_raw, list) or len(phases_raw) != REQUIRED_FRAMES_PER_ROLE:
            raise RasterContractError(
                f"{identity_key}/{role}: all four phase semantic locks are required"
            )
        phases = tuple(
            _parse_phase_semantic(
                phase_raw,
                f"{identity_key}/{role}/{phase}",
                role,
                phase,
                baseline_policy,
                identity_key,
                identity_source_sha256,
                identity_frame_sha256,
                expected_schema,
                transform_sha256,
                registration_hash,
            )
            for phase, phase_raw in enumerate(phases_raw)
        )
        if baseline_frame_hash != phases[0].composited_frame_sha256:
            raise RasterContractError(
                f"{identity_key}/{role}: role-pose baseline must be the exact "
                "bounded-composite phase-0 frame"
            )
        p0_asset = phases[0].generated_asset
        three_reference_records = [
            phase_lock.native_grid_reference
            for phase_lock in phases[1:]
            if phase_lock.native_grid_reference is not None
        ]
        if three_reference_records and len(set(three_reference_records)) != 1:
            raise RasterContractError(
                f"{identity_key}/{role}: all Image 3 phases must reuse one exact "
                "immutable role-P0 native-grid reference"
            )
        for phase_lock in phases:
            expected_composition_baseline = (
                identity_frame_sha256
                if phase_lock.phase == 0
                else baseline_frame_hash
            )
            if (
                phase_lock.composition_baseline_frame_sha256
                != expected_composition_baseline
            ):
                raise RasterContractError(
                    f"{identity_key}/{role}/{phase_lock.phase}: composition "
                    "baseline hash drifted"
                )
            if phase_lock.phase > 0:
                target = phase_lock.edit_target_reference
                if (
                    target.source_sha256 != p0_asset.source_sha256
                    or target.registered_frame_sha256
                    != p0_asset.registered_candidate_frame_sha256
                    or target.accepted_composited_frame_sha256
                    != baseline_frame_hash
                ):
                    raise RasterContractError(
                        f"{identity_key}/{role}/{phase_lock.phase}: P1/P2/P3 "
                        "must independently reference the same accepted immutable P0"
                    )
                native_grid_reference = phase_lock.native_grid_reference
                if native_grid_reference is not None and (
                    registration.derivation != "identity-anchored-zero-offset"
                    or registration.output_offset != (0, 0)
                    or p0_asset.layout != GENERATED_IDENTITY_BASELINE_ASSET_LAYOUT
                    or p0_asset.source_sha256 != identity_source_sha256
                    or p0_asset.imported_candidate_frame_sha256
                    != identity_frame_sha256
                    or p0_asset.registered_candidate_frame_sha256
                    != identity_frame_sha256
                    or baseline_frame_hash != identity_frame_sha256
                    or native_grid_reference.source_png_sha256
                    != p0_asset.source_sha256
                    or native_grid_reference.p0_packed_sha256
                    != baseline_frame_hash
                    or native_grid_reference.roundtrip_packed_sha256
                    != baseline_frame_hash
                ):
                    raise RasterContractError(
                        f"{identity_key}/{role}/{phase_lock.phase}: Image 3 v1 "
                        "requires a zero-registration byte-exact identity-copy P0"
                    )
        landmarks_raw = raw_role["motion_landmarks"]
        if not isinstance(landmarks_raw, list) or not landmarks_raw:
            raise RasterContractError(
                f"{identity_key}/{role}: at least one role motion landmark is required"
            )
        landmarks = tuple(
            _parse_motion_landmark(
                landmark, f"{identity_key}/{role}/motion/{index}"
            )
            for index, landmark in enumerate(landmarks_raw)
        )
        names = [landmark.name for landmark in landmarks]
        if len(names) != len(set(names)):
            raise RasterContractError(
                f"{identity_key}/{role}: motion landmark names must be unique"
            )
        roles.append(
            GeneratedRoleSemanticLock(
                role=role,
                baseline_policy=baseline_policy,
                contact_policy=contact_policy,
                role_registration=registration,
                role_registration_sha256=registration_hash,
                role_pose_baseline_frame_sha256=baseline_frame_hash,
                maximum_role_pose_component_count_delta=(
                    maximum_role_component_delta
                ),
                maximum_contact_changed_pixels_per_phase=(
                    maximum_contact_changes
                ),
                role_pose_identity_landmarks=pose_landmarks,
                phases=phases,
                motion_landmarks=landmarks,
            )
        )
    names = [role.role for role in roles]
    if len(names) != len(set(names)) or names != sorted(
        names, key=canonical_role_order.__getitem__
    ):
        raise RasterContractError(
            f"{identity_key}: semantic roles must be unique and in canonical order"
        )
    return GeneratedActionSemanticContract(
        schema=expected_schema,
        roles=tuple(roles),
    )


def load_high_res_identity_locks(
    path: Path, selected: list[str]
) -> dict[str, HighResIdentityLock]:
    """Load exact-canvas approvals; legacy source-scale locks are forbidden."""

    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict) or set(payload) != {"schema", "identities"}:
        raise RasterContractError(
            "format-v2 identity lock must contain exactly schema and identities"
        )
    if payload["schema"] != HIGH_RES_IDENTITY_LOCK_SCHEMA:
        raise RasterContractError(
            f"format-v2 identity lock schema must be {HIGH_RES_IDENTITY_LOCK_SCHEMA}"
        )
    records = payload["identities"]
    if not isinstance(records, list):
        raise RasterContractError("format-v2 identity lock identities must be a list")

    locks: dict[str, HighResIdentityLock] = {}
    for raw in records:
        if not isinstance(raw, dict) or set(raw) != {
            "approved",
            "identity_key",
            "identity_sha256",
            "frame_canvas",
        }:
            raise RasterContractError(
                "format-v2 identity lock record has unexpected fields"
            )
        identity_key = raw["identity_key"]
        digest = raw["identity_sha256"]
        canvas = raw["frame_canvas"]
        approved = raw["approved"]
        if (
            not isinstance(identity_key, str)
            or identity_key in locks
            or identity_key in PROTECTED_STARTERS
        ):
            raise RasterContractError(
                "format-v2 identity lock contains invalid/duplicate identity"
            )
        if (
            not isinstance(digest, str)
            or len(digest) != 64
            or any(character not in "0123456789abcdef" for character in digest)
        ):
            raise RasterContractError(
                f"{identity_key}: invalid format-v2 identity SHA-256"
            )
        if canvas != [HIGH_RES_FRAME_WIDTH, HIGH_RES_FRAME_HEIGHT]:
            raise RasterContractError(
                f"{identity_key}: format-v2 frame_canvas must be "
                f"[{HIGH_RES_FRAME_WIDTH}, {HIGH_RES_FRAME_HEIGHT}]"
            )
        if approved is not True:
            raise RasterContractError(
                f"{identity_key}: format-v2 identity lock is not approved"
            )
        locks[identity_key] = HighResIdentityLock(
            identity_key=identity_key,
            identity_sha256=digest,
            frame_canvas=(HIGH_RES_FRAME_WIDTH, HIGH_RES_FRAME_HEIGHT),
            approved=True,
        )
    if set(locks) != set(selected):
        raise RasterContractError(
            "format-v2 identity lock set differs from selected build: "
            f"missing={sorted(set(selected) - set(locks))} "
            f"unexpected={sorted(set(locks) - set(selected))}"
        )
    return locks


def recommended_imagegen_import_transform(
    *, action_output_offset: tuple[int, int]
) -> ImageGenImportTransform:
    """Build the fixed full-canvas transform with an explicit reserved field.

    ``action_output_offset`` remains mandatory in canonical transform records
    so legacy values cannot be silently dropped or reinterpreted. ImageGen v4/v5
    independent-frame import never consumes it; new locks normally set it to
    the same value as ``output_offset``.
    """

    return ImageGenImportTransform(
        source_canvas=IMAGEGEN_RECOMMENDED_SOURCE_CANVAS,
        crop_rect=IMAGEGEN_RECOMMENDED_CROP_RECT,
        output_canvas=IMAGEGEN_OUTPUT_CANVAS,
        resample_mode=IMAGEGEN_RESAMPLE_MODE,
        luminance_mode=IMAGEGEN_LUMINANCE_MODE,
        black_coverage_threshold_per_mille=(
            IMAGEGEN_RECOMMENDED_BLACK_COVERAGE_PER_MILLE
        ),
        alpha_background=IMAGEGEN_ALPHA_BACKGROUND,
        output_offset=IMAGEGEN_RECOMMENDED_OUTPUT_OFFSET,
        action_output_offset=action_output_offset,
    )


def imagegen_import_transform_record(
    transform: ImageGenImportTransform,
) -> dict[str, object]:
    return {
        "action_output_offset": list(transform.action_output_offset),
        "alpha_background": list(transform.alpha_background),
        "black_coverage_threshold_per_mille": (
            transform.black_coverage_threshold_per_mille
        ),
        "crop_rect": list(transform.crop_rect),
        "luminance_mode": transform.luminance_mode,
        "output_canvas": list(transform.output_canvas),
        "output_offset": list(transform.output_offset),
        "resample_mode": transform.resample_mode,
        "source_canvas": list(transform.source_canvas),
    }


def imagegen_import_transform_sha256(transform: ImageGenImportTransform) -> str:
    payload = json.dumps(
        imagegen_import_transform_record(transform),
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    ).encode("ascii")
    return hashlib.sha256(payload).hexdigest()


def imagegen_action_sheet_layout_record() -> dict[str, object]:
    """Canonical four-phase layout for one—and only one—named action."""

    return {
        "cell_outer_safe_guard_pixels": (
            IMAGEGEN_ACTION_SHEET_CELL_SAFE_GUARD_PIXELS
        ),
        "cell_transform": "identity-pinned-box-area-coverage-fixed-offset",
        "fixed_cell_extraction": True,
        "gutter_rects": [list(rect) for rect in IMAGEGEN_ACTION_SHEET_GUTTER_RECTS],
        "per_cell_cleanup": False,
        "per_cell_crop": False,
        "per_cell_fit": False,
        "per_cell_offset": False,
        "per_cell_threshold": False,
        "phase_order": [0, 1, 2, 3],
        "phase_viewports": [
            list(rect) for rect in IMAGEGEN_ACTION_SHEET_PHASE_RECTS
        ],
        "schema": IMAGEGEN_ACTION_SHEET_LAYOUT_SCHEMA,
        "source_asset_per_action": True,
        "source_canvas": list(IMAGEGEN_ACTION_SHEET_SOURCE_CANVAS),
    }


def imagegen_action_sheet_layout_sha256() -> str:
    payload = json.dumps(
        imagegen_action_sheet_layout_record(),
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    ).encode("ascii")
    return hashlib.sha256(payload).hexdigest()


def validate_imagegen_import_transform(
    transform: ImageGenImportTransform, label: str
) -> None:
    if (
        transform.source_canvas[0] < 800
        or transform.source_canvas[1] < 1000
    ):
        raise RasterContractError(
            f"{label}: ImageGen source_canvas is too small for stable area import"
        )
    if transform.output_canvas != IMAGEGEN_OUTPUT_CANVAS:
        raise RasterContractError(
            f"{label}: ImageGen output_canvas must be {IMAGEGEN_OUTPUT_CANVAS}"
        )
    if transform.resample_mode != IMAGEGEN_RESAMPLE_MODE:
        raise RasterContractError(
            f"{label}: ImageGen resample_mode must be {IMAGEGEN_RESAMPLE_MODE}"
        )
    if transform.luminance_mode != IMAGEGEN_LUMINANCE_MODE:
        raise RasterContractError(
            f"{label}: ImageGen luminance_mode must be {IMAGEGEN_LUMINANCE_MODE}"
        )
    if transform.alpha_background != IMAGEGEN_ALPHA_BACKGROUND:
        raise RasterContractError(
            f"{label}: ImageGen alpha background must be opaque white"
        )
    if not 50 <= transform.black_coverage_threshold_per_mille <= 500:
        raise RasterContractError(
            f"{label}: black coverage threshold must be 50..500 per mille"
        )

    left, top, right, bottom = transform.crop_rect
    source_width, source_height = transform.source_canvas
    crop_width = right - left
    crop_height = bottom - top
    if not (
        0 <= left < right <= source_width
        and 0 <= top < bottom <= source_height
    ):
        raise RasterContractError(f"{label}: crop rectangle is outside source canvas")
    if crop_width * 5 != crop_height * 4:
        raise RasterContractError(
            f"{label}: crop rectangle must have exact centered 4:5 aspect"
        )
    if left + right != source_width or top + bottom != source_height:
        raise RasterContractError(
            f"{label}: crop rectangle must remain centered; per-frame offsets "
            "are forbidden"
        )
    if crop_width < 800 or crop_height < 1000:
        raise RasterContractError(
            f"{label}: crop viewport is too small for stable area downsampling"
        )
    if max(left, top, source_width - right, source_height - bottom) > 2:
        raise RasterContractError(
            f"{label}: ImageGen import may remove at most a two-pixel white "
            "source border; subject-box crops are forbidden"
        )
    for offset_name, offset in (
        ("identity output_offset", transform.output_offset),
        ("action_output_offset", transform.action_output_offset),
    ):
        if (
            not isinstance(offset, tuple)
            or len(offset) != 2
            or not isinstance(offset[0], int)
            or not isinstance(offset[1], int)
            or abs(offset[0]) >= HIGH_RES_FRAME_WIDTH
            or abs(offset[1]) >= HIGH_RES_FRAME_HEIGHT
        ):
            raise RasterContractError(f"{label}: invalid pinned {offset_name}")


def _parse_imagegen_import_transform(
    raw: object, label: str
) -> ImageGenImportTransform:
    expected_fields = {
        "action_output_offset",
        "alpha_background",
        "black_coverage_threshold_per_mille",
        "crop_rect",
        "luminance_mode",
        "output_canvas",
        "output_offset",
        "resample_mode",
        "source_canvas",
    }
    if not isinstance(raw, dict):
        raise RasterContractError(
            f"{label}: ImageGen transform must be an object"
        )
    if set(raw) != expected_fields:
        raise RasterContractError(
            f"{label}: corrected ImageGen transform requires the explicit "
            f"action_output_offset; missing={sorted(expected_fields - set(raw))} "
            f"unexpected={sorted(set(raw) - expected_fields)}"
        )
    source = raw["source_canvas"]
    crop = raw["crop_rect"]
    output = raw["output_canvas"]
    offset = raw["output_offset"]
    action_offset = raw["action_output_offset"]
    background = raw["alpha_background"]
    coverage = raw["black_coverage_threshold_per_mille"]
    if (
        not isinstance(source, list)
        or len(source) != 2
        or any(not isinstance(value, int) for value in source)
        or not isinstance(crop, list)
        or len(crop) != 4
        or any(not isinstance(value, int) for value in crop)
        or not isinstance(output, list)
        or len(output) != 2
        or any(not isinstance(value, int) for value in output)
        or not isinstance(background, list)
        or len(background) != 3
        or any(not isinstance(value, int) for value in background)
        or not isinstance(offset, list)
        or len(offset) != 2
        or any(not isinstance(value, int) for value in offset)
        or not isinstance(action_offset, list)
        or len(action_offset) != 2
        or any(not isinstance(value, int) for value in action_offset)
        or not isinstance(coverage, int)
        or not isinstance(raw["resample_mode"], str)
        or not isinstance(raw["luminance_mode"], str)
    ):
        raise RasterContractError(f"{label}: malformed ImageGen transform")
    transform = ImageGenImportTransform(
        source_canvas=(source[0], source[1]),
        crop_rect=(crop[0], crop[1], crop[2], crop[3]),
        output_canvas=(output[0], output[1]),
        resample_mode=raw["resample_mode"],
        luminance_mode=raw["luminance_mode"],
        black_coverage_threshold_per_mille=coverage,
        alpha_background=(background[0], background[1], background[2]),
        output_offset=(offset[0], offset[1]),
        action_output_offset=(action_offset[0], action_offset[1]),
    )
    validate_imagegen_import_transform(transform, label)
    return transform


def load_imagegen_import_locks(
    path: Path, selected: list[str]
) -> dict[str, ImageGenImportLock]:
    """Load identity-pinned, immutable ImageGen area-downsample transforms."""

    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict) or set(payload) != {"schema", "identities"}:
        raise RasterContractError(
            "ImageGen import lock must contain exactly schema and identities"
        )
    lock_schema = payload["schema"]
    if lock_schema not in {
        LEGACY_IMAGEGEN_IMPORT_LOCK_SCHEMA,
        IMAGEGEN_IMPORT_LOCK_SCHEMA,
    }:
        raise RasterContractError(
            "ImageGen import lock schema must be "
            f"{LEGACY_IMAGEGEN_IMPORT_LOCK_SCHEMA} or {IMAGEGEN_IMPORT_LOCK_SCHEMA}"
        )
    semantic_schema = (
        LEGACY_GENERATED_ACTION_SEMANTIC_SCHEMA
        if lock_schema == LEGACY_IMAGEGEN_IMPORT_LOCK_SCHEMA
        else GENERATED_ACTION_SEMANTIC_SCHEMA
    )
    records = payload["identities"]
    if not isinstance(records, list):
        raise RasterContractError("ImageGen import lock identities must be a list")

    locks: dict[str, ImageGenImportLock] = {}
    for raw in records:
        required_fields = {
            "action_semantic_contract",
            "action_semantic_contract_sha256",
            "approved",
            "identity_frame_sha256",
            "identity_key",
            "identity_source_sha256",
            "transform",
            "transform_sha256",
        }
        transform_fields = required_fields - {
            "action_semantic_contract",
            "action_semantic_contract_sha256",
        }
        if not isinstance(raw, dict) or not transform_fields <= set(raw):
            raise RasterContractError(
                "ImageGen import lock record has unexpected fields"
            )
        identity_key = raw["identity_key"]
        if (
            not isinstance(identity_key, str)
            or identity_key in locks
            or identity_key in PROTECTED_STARTERS
        ):
            raise RasterContractError(
                "ImageGen import lock contains invalid/duplicate identity"
            )
        digests = {
            "identity source": raw["identity_source_sha256"],
            "identity frame": raw["identity_frame_sha256"],
            "transform": raw["transform_sha256"],
        }
        for digest_label, digest in digests.items():
            if (
                not isinstance(digest, str)
                or len(digest) != 64
                or any(character not in "0123456789abcdef" for character in digest)
            ):
                raise RasterContractError(
                    f"{identity_key}: invalid {digest_label} SHA-256"
                )
        transform = _parse_imagegen_import_transform(
            raw["transform"], f"{identity_key}/transform"
        )
        actual_transform_hash = imagegen_import_transform_sha256(transform)
        if actual_transform_hash != raw["transform_sha256"]:
            raise RasterContractError(
                f"{identity_key}: ImageGen transform SHA-256 mismatch; even a "
                "one-pixel crop, scale, or offset change requires a new approval"
            )
        if set(raw) != required_fields:
            raise RasterContractError(
                f"{identity_key}: generated action semantic contract is mandatory; "
                f"missing={sorted(required_fields - set(raw))} "
                f"unexpected={sorted(set(raw) - required_fields)}"
            )
        action_contract_hash = _require_lower_sha256(
            raw["action_semantic_contract_sha256"],
            f"{identity_key}/action_semantic_contract_sha256",
        )
        action_contract = _parse_generated_action_semantic_contract(
            raw["action_semantic_contract"],
            identity_key,
            raw["identity_source_sha256"],
            raw["identity_frame_sha256"],
            semantic_schema,
            raw["transform_sha256"],
        )
        actual_action_contract_hash = generated_action_semantic_contract_sha256(
            action_contract
        )
        if actual_action_contract_hash != action_contract_hash:
            raise RasterContractError(
                f"{identity_key}: generated action semantic contract SHA-256 "
                "drifted; mask, lineage, or motion-policy changes require a new "
                "approval"
            )
        if raw["approved"] is not True:
            raise RasterContractError(
                f"{identity_key}: ImageGen import lock is not approved"
            )
        locks[identity_key] = ImageGenImportLock(
            schema=lock_schema,
            identity_key=identity_key,
            identity_source_sha256=raw["identity_source_sha256"],
            identity_frame_sha256=raw["identity_frame_sha256"],
            transform_sha256=raw["transform_sha256"],
            transform=transform,
            action_semantic_contract_sha256=action_contract_hash,
            action_semantic_contract=action_contract,
            approved=True,
        )
    if set(locks) != set(selected):
        raise RasterContractError(
            "ImageGen import lock set differs from selected build: "
            f"missing={sorted(set(selected) - set(locks))} "
            f"unexpected={sorted(set(locks) - set(selected))}"
        )
    return locks


def _coverage_is_ink(luminance: int, coverage_per_mille: int) -> bool:
    return (255 - luminance) * 1000 >= coverage_per_mille * 255


def _flat_image_values(image: Image.Image) -> list[int]:
    """Return flat pixel values across supported Pillow releases."""

    flattened = getattr(image, "get_flattened_data", None)
    return list(flattened() if flattened is not None else image.getdata())


def _import_imagegen_mask(
    path: Path,
    transform: ImageGenImportTransform,
    label: str,
    *,
    require_floor: bool = True,
    bounded_validation_region: set[tuple[int, int]] | None = None,
) -> set[tuple[int, int]]:
    """Apply the one pinned identity transform, without per-frame fitting.

    ``bounded_validation_region`` is only used for a v4/v5 generated candidate.
    It is expressed in the candidate's post-transform, pre-registration 64x80
    coordinates.  Coverage ambiguity outside that region is deliberately not
    release-relevant because bounded composition cannot copy those pixels into
    the final frame.  Canvas, source-edge, and output clipping checks remain
    global and fail closed.
    """

    validate_imagegen_import_transform(transform, label)
    with Image.open(path) as image:
        if image.size != transform.source_canvas:
            raise RasterContractError(
                f"{label}: ImageGen canvas {image.size} differs from locked "
                f"canvas {transform.source_canvas}; no per-frame scale is allowed"
            )
        if image.mode not in {"RGB", "RGBA"}:
            raise RasterContractError(
                f"{label}: ImageGen import expects RGB/RGBA, got {image.mode!r}"
            )
        rgba = image.convert("RGBA")
        background = Image.new(
            "RGBA", image.size, (*transform.alpha_background, 255)
        )
        composited = Image.alpha_composite(background, rgba).convert("RGB")
        gray = composited.convert("L")

    left, top, right, bottom = transform.crop_rect
    # The crop is the whole generated canvas minus at most a two-pixel border.
    # Requiring one additional white source pixel proves that even this tiny
    # identity-pinned border does not cut content.
    allowed_source_bounds = (
        left + 1,
        top + 1,
        right - 1,
        bottom - 1,
    )

    lookup = [
        255
        if _coverage_is_ink(
            value, transform.black_coverage_threshold_per_mille
        )
        else 0
        for value in range(256)
    ]
    source_ink = gray.point(lookup, mode="L")
    source_bounds = source_ink.getbbox()
    if source_bounds is None:
        raise RasterContractError(f"{label}: ImageGen source contains no subject ink")
    allowed_left, allowed_top, allowed_right, allowed_bottom = allowed_source_bounds
    if (
        source_bounds[0] < allowed_left
        or source_bounds[1] < allowed_top
        or source_bounds[2] > allowed_right
        or source_bounds[3] > allowed_bottom
    ):
        raise RasterContractError(
            f"{label}: source ink bounds {source_bounds} leave locked safe viewport "
            f"{allowed_source_bounds}; frame is clipped or would need per-frame fit"
        )

    crop = gray.crop(transform.crop_rect)
    if bounded_validation_region is None:
        relevant_source_values = _flat_image_values(crop)
    else:
        if any(
            x < 0
            or x >= HIGH_RES_FRAME_WIDTH
            or y < 0
            or y >= HIGH_RES_FRAME_HEIGHT
            for x, y in bounded_validation_region
        ):
            raise RasterContractError(
                f"{label}: bounded candidate validation region leaves 64x80 canvas"
            )
        offset_x, offset_y = transform.output_offset
        unshifted_relevance = Image.new("L", transform.output_canvas, 0)
        relevance_pixels = unshifted_relevance.load()
        for x, y in bounded_validation_region:
            unshifted_x = x - offset_x
            unshifted_y = y - offset_y
            if (
                0 <= unshifted_x < HIGH_RES_FRAME_WIDTH
                and 0 <= unshifted_y < HIGH_RES_FRAME_HEIGHT
            ):
                relevance_pixels[unshifted_x, unshifted_y] = 255
        source_relevance = unshifted_relevance.resize(
            crop.size, Image.Resampling.NEAREST
        )
        relevant_source_values = [
            value
            for value, relevant in zip(
                _flat_image_values(crop),
                _flat_image_values(source_relevance),
                strict=True,
            )
            if relevant
        ]
    nonwhite = sum(value < 250 for value in relevant_source_values)
    ambiguous = sum(17 <= value < 239 for value in relevant_source_values)
    ambiguous_fraction = ambiguous / nonwhite if nonwhite else 0.0
    if ambiguous_fraction > IMAGEGEN_MAX_AMBIGUOUS_SOURCE_FRACTION:
        raise RasterContractError(
            f"{label}: {ambiguous_fraction:.3f} of nonwhite source pixels are "
            "mid-tone/antialiased; regenerate clean one-bit-style artwork"
        )

    reduced = crop.resize(transform.output_canvas, Image.Resampling.BOX)
    reduced_values = list(reduced.tobytes())
    threshold = transform.black_coverage_threshold_per_mille
    unshifted_mask = {
        (index % HIGH_RES_FRAME_WIDTH, index // HIGH_RES_FRAME_WIDTH)
        for index, value in enumerate(reduced_values)
        if _coverage_is_ink(value, threshold)
    }
    offset_x, offset_y = transform.output_offset
    shifted_mask = {
        (x + offset_x, y + offset_y) for x, y in unshifted_mask
    }
    if any(
        x < 0
        or x >= HIGH_RES_FRAME_WIDTH
        or y < 0
        or y >= HIGH_RES_FRAME_HEIGHT
        for x, y in shifted_mask
    ):
        raise RasterContractError(
            f"{label}: the locked output offset {transform.output_offset} clips "
            "this action; per-frame fitting is forbidden"
        )
    mask = shifted_mask

    low_threshold = max(0, threshold - IMAGEGEN_COVERAGE_STABILITY_PER_MILLE)
    high_threshold = min(1000, threshold + IMAGEGEN_COVERAGE_STABILITY_PER_MILLE)
    low_mask = {
        (index % HIGH_RES_FRAME_WIDTH + offset_x,
         index // HIGH_RES_FRAME_WIDTH + offset_y)
        for index, value in enumerate(reduced_values)
        if _coverage_is_ink(value, low_threshold)
    }
    high_mask = {
        (index % HIGH_RES_FRAME_WIDTH + offset_x,
         index // HIGH_RES_FRAME_WIDTH + offset_y)
        for index, value in enumerate(reduced_values)
        if _coverage_is_ink(value, high_threshold)
    }
    sensitivity_region = (
        bounded_validation_region
        if bounded_validation_region is not None
        else {
            (x, y)
            for y in range(HIGH_RES_FRAME_HEIGHT)
            for x in range(HIGH_RES_FRAME_WIDTH)
        }
    )
    sensitive_pixels = len((low_mask ^ high_mask) & sensitivity_region)
    relevant_ink_pixels = len(mask & sensitivity_region)
    maximum_sensitive = max(
        4,
        math.ceil(
            relevant_ink_pixels * IMAGEGEN_MAX_THRESHOLD_SENSITIVE_FRACTION
        ),
    )
    if sensitive_pixels > maximum_sensitive:
        raise RasterContractError(
            f"{label}: {sensitive_pixels} output pixels are coverage-threshold "
            f"sensitive (maximum {maximum_sensitive}); regenerate cleaner art"
        )

    if bounded_validation_region is None:
        validate_high_res_mask(mask, label, require_floor=require_floor)
    else:
        _generated_candidate_metrics(mask, label)
    return mask


def _generated_candidate_metrics(
    mask: set[tuple[int, int]], label: str
) -> SourceMetrics:
    """Describe an imported candidate without treating discarded pixels as art.

    A v4/v5 candidate is provenance, not a release frame. Its complete binary
    reduction must remain nonempty, in-canvas, packable, and hashable, but
    semantic stage/debris/identity checks apply to the bounded composite that
    can actually ship.  This keeps off-mask ImageGen redraw noise visible in
    the candidate hash while preventing it from entering the final frame.
    """

    if not mask:
        raise RasterContractError(f"{label}: ImageGen candidate contains no ink")
    if any(
        x < 0
        or x >= HIGH_RES_FRAME_WIDTH
        or y < 0
        or y >= HIGH_RES_FRAME_HEIGHT
        for x, y in mask
    ):
        raise RasterContractError(f"{label}: ImageGen candidate leaves 64x80 canvas")
    components = sorted(base.connected_components(mask), key=len, reverse=True)
    packed = high_res_frame_bytes(mask)
    if decode_high_res_frame_bytes(packed) != mask:
        raise RasterContractError(f"{label}: candidate packing changed pixels")
    return SourceMetrics(
        bounds=base.bounds(mask),
        ink_pixels=len(mask),
        components=len(components),
        smallest_component_pixels=min(map(len, components)),
        primary_fraction=len(components[0]) / len(mask),
    )


def load_imagegen_import_frame(
    path: Path,
    role: str,
    phase: int,
    transform: ImageGenImportTransform,
    *,
    identity_mask: set[tuple[int, int]] | None = None,
    require_floor: bool = True,
    bounded_validation_region: set[tuple[int, int]] | None = None,
) -> HighResFrame:
    owner = path.parent.parent.name if path.parent.name == role else path.parent.name
    label = f"{owner}/{role}/{phase}"
    mask = _import_imagegen_mask(
        path,
        transform,
        label,
        require_floor=require_floor,
        bounded_validation_region=bounded_validation_region,
    )
    if bounded_validation_region is None:
        metrics = validate_high_res_mask(mask, label, require_floor=require_floor)
    else:
        metrics = _generated_candidate_metrics(mask, label)
    apparent_scale = 1.0
    similarity = 1.0
    if identity_mask is not None:
        apparent_scale = _high_res_apparent_scale(mask, identity_mask)
        lower, upper = HIGH_RES_ROLE_SCALE_ENVELOPES[role]
        if not lower <= apparent_scale <= upper:
            raise RasterContractError(
                f"{label}: apparent identity scale {apparent_scale:.3f} is "
                f"outside [{lower:.3f}, {upper:.3f}]; the pinned transform "
                "cannot auto-fit this frame"
            )
        similarity = jaccard(identity_mask, mask)
        minimum = ROLE_IDENTITY_JACCARD_MINIMUM[role]
        if similarity < minimum:
            raise RasterContractError(
                f"{label}: identity overlap {similarity:.3f} is below role "
                f"floor {minimum:.3f}"
            )
    packed = high_res_frame_bytes(mask)
    return HighResFrame(
        path=path,
        role=role,
        phase=phase,
        source_sha256=sha256_file(path),
        mask=frozenset(mask),
        metrics=metrics,
        apparent_scale_ratio=apparent_scale,
        identity_jaccard=similarity,
        packed=packed,
    )


def native_grid_reference_image(
    accepted_p0_mask: set[tuple[int, int]] | frozenset[tuple[int, int]],
    transform: ImageGenImportTransform,
    *,
    label: str,
) -> Image.Image:
    """Return the one exact RGB prompt projection of an accepted native P0."""

    validate_imagegen_import_transform(transform, f"{label}/transform")
    if (
        transform.source_canvas != IMAGEGEN_RECOMMENDED_SOURCE_CANVAS
        or transform.crop_rect != IMAGEGEN_RECOMMENDED_CROP_RECT
        or transform.output_canvas != IMAGEGEN_OUTPUT_CANVAS
    ):
        raise RasterContractError(
            f"{label}: native-grid Image 3 v1 requires the exact 1122x1402 "
            "canvas and 1120x1400 centered crop"
        )
    dx, dy = transform.output_offset
    unshifted = {(x - dx, y - dy) for x, y in accepted_p0_mask}
    outside = {
        (x, y)
        for x, y in unshifted
        if not (
            0 <= x < HIGH_RES_FRAME_WIDTH
            and 0 <= y < HIGH_RES_FRAME_HEIGHT
        )
    }
    if outside:
        first = min(outside, key=lambda point: (point[1], point[0]))
        raise RasterContractError(
            f"{label}: inverse locked output offset would discard accepted P0 "
            f"pixel {first}"
        )
    native = Image.new("1", IMAGEGEN_OUTPUT_CANVAS, 1)
    pixels = native.load()
    for x, y in unshifted:
        pixels[x, y] = 0
    left, top, right, bottom = transform.crop_rect
    enlarged = native.convert("RGB").resize(
        (right - left, bottom - top), Image.Resampling.NEAREST
    )
    reference = Image.new(
        "RGB", transform.source_canvas, transform.alpha_background
    )
    reference.paste(enlarged, (left, top))
    return reference


def _independent_box_axis_weights(
    source_length: int, output_length: int, output_index: int
) -> tuple[tuple[int, int], ...]:
    """Return exact source-pixel overlap numerators without Pillow resampling."""

    start = output_index * source_length
    end = (output_index + 1) * source_length
    first = start // output_length
    last = (end + output_length - 1) // output_length
    return tuple(
        (
            source_index,
            min(end, (source_index + 1) * output_length)
            - max(start, source_index * output_length),
        )
        for source_index in range(first, last)
        if min(end, (source_index + 1) * output_length)
        > max(start, source_index * output_length)
    )


def _independent_native_grid_box_mask(
    reference: Image.Image,
    transform: ImageGenImportTransform,
    coverage_per_mille: int,
    *,
    label: str,
) -> set[tuple[int, int]]:
    """Classify exact RGB black/white cells with rational BOX area weights."""

    crop = reference.crop(transform.crop_rect)
    crop_width, crop_height = crop.size
    source_pixels = crop.load()
    x_weights = tuple(
        _independent_box_axis_weights(
            crop_width, HIGH_RES_FRAME_WIDTH, output_x
        )
        for output_x in range(HIGH_RES_FRAME_WIDTH)
    )
    y_weights = tuple(
        _independent_box_axis_weights(
            crop_height, HIGH_RES_FRAME_HEIGHT, output_y
        )
        for output_y in range(HIGH_RES_FRAME_HEIGHT)
    )
    total_weight = crop_width * crop_height
    unshifted: set[tuple[int, int]] = set()
    for output_y, rows in enumerate(y_weights):
        for output_x, columns in enumerate(x_weights):
            black_weight = sum(
                x_weight * y_weight
                for source_y, y_weight in rows
                for source_x, x_weight in columns
                if source_pixels[source_x, source_y] == (0, 0, 0)
            )
            if black_weight * 1000 >= total_weight * coverage_per_mille:
                unshifted.add((output_x, output_y))
    dx, dy = transform.output_offset
    shifted = {(x + dx, y + dy) for x, y in unshifted}
    outside = {
        (x, y)
        for x, y in shifted
        if not (
            0 <= x < HIGH_RES_FRAME_WIDTH
            and 0 <= y < HIGH_RES_FRAME_HEIGHT
        )
    }
    if outside:
        first = min(outside, key=lambda point: (point[1], point[0]))
        raise RasterContractError(
            f"{label}: independent BOX proof clips output pixel {first}"
        )
    return shifted


def validate_native_grid_conditioning_reference(
    species_dir: Path,
    species: str,
    role: str,
    reference: NativeGridConditioningReference,
    transform: ImageGenImportTransform,
    accepted_p0: HighResFrame,
) -> dict[str, object]:
    """Authenticate one prompt-only Image 3 and two exact BOX round trips."""

    label = f"{species}/{role}/native-grid-reference"
    expected_p0_hash = hashlib.sha256(accepted_p0.packed).hexdigest()
    if (
        reference.p0_packed_sha256 != expected_p0_hash
        or reference.roundtrip_packed_sha256 != expected_p0_hash
        or reference.transform_sha256
        != imagegen_import_transform_sha256(transform)
    ):
        raise RasterContractError(
            f"{label}: P0 or transform provenance drifted"
        )
    path = species_dir / reference.grid_relative_path
    if not path.is_file() or sha256_file(path) != reference.grid_png_sha256:
        raise RasterContractError(
            f"{label}: native-grid PNG is missing or its SHA-256 drifted"
        )
    with Image.open(path) as opened:
        if (
            opened.format != "PNG"
            or opened.mode != "RGB"
            or opened.size != IMAGEGEN_RECOMMENDED_SOURCE_CANVAS
        ):
            raise RasterContractError(
                f"{label}: Image 3 must be an exact RGB 1122x1402 PNG"
            )
        candidate = opened.copy()
    colors = set(_flat_image_values(candidate))
    if not colors or not colors <= {(0, 0, 0), (255, 255, 255)}:
        raise RasterContractError(
            f"{label}: Image 3 must contain only opaque black and white pixels"
        )
    expected = native_grid_reference_image(
        accepted_p0.mask, transform, label=label
    )
    if candidate.tobytes() != expected.tobytes():
        raise RasterContractError(
            f"{label}: Image 3 is not the exact nearest native-grid projection; "
            "blur, shifts, fitting, and manual pixels are forbidden"
        )
    canonical_mask = _import_imagegen_mask(
        path, transform, f"{label}/canonical-BOX", require_floor=False
    )
    nominal = _independent_native_grid_box_mask(
        candidate,
        transform,
        transform.black_coverage_threshold_per_mille,
        label=f"{label}/independent-BOX",
    )
    low = _independent_native_grid_box_mask(
        candidate,
        transform,
        max(
            0,
            transform.black_coverage_threshold_per_mille
            - IMAGEGEN_COVERAGE_STABILITY_PER_MILLE,
        ),
        label=f"{label}/independent-BOX-low",
    )
    high = _independent_native_grid_box_mask(
        candidate,
        transform,
        min(
            1000,
            transform.black_coverage_threshold_per_mille
            + IMAGEGEN_COVERAGE_STABILITY_PER_MILLE,
        ),
        label=f"{label}/independent-BOX-high",
    )
    accepted_mask = set(accepted_p0.mask)
    if (
        canonical_mask != accepted_mask
        or nominal != accepted_mask
        or low != accepted_mask
        or high != accepted_mask
    ):
        raise RasterContractError(
            f"{label}: canonical or independent BOX round trip differs from "
            "the exact accepted P0"
        )
    canonical_hash = hashlib.sha256(
        high_res_frame_bytes(canonical_mask)
    ).hexdigest()
    independent_hash = hashlib.sha256(
        high_res_frame_bytes(nominal)
    ).hexdigest()
    if canonical_hash != expected_p0_hash or independent_hash != expected_p0_hash:
        raise RasterContractError(
            f"{label}: BOX round-trip packed hash differs from accepted P0"
        )
    return {
        "derivation": reference.derivation,
        "grid_png_sha256": reference.grid_png_sha256,
        "grid_relative_path": reference.grid_relative_path,
        "canonical_roundtrip_packed_sha256": canonical_hash,
        "canonical_independent_xor_pixels": len(canonical_mask ^ nominal),
        "independent_roundtrip_packed_sha256": independent_hash,
        "independent_threshold_sensitive_pixels_plus_minus_20": len(low ^ high),
        "p0_packed_sha256": expected_p0_hash,
        "transform_sha256": reference.transform_sha256,
    }


def register_generated_candidate(
    candidate: HighResFrame,
    registration: GeneratedRoleRegistrationLock,
    *,
    label: str,
) -> HighResFrame:
    """Apply one hash-pinned role offset without fitting any individual phase."""

    if registration.schema != GENERATED_ROLE_REGISTRATION_SCHEMA:
        raise RasterContractError(f"{label}: unsafe role registration schema")
    dx, dy = registration.output_offset
    if (
        abs(dx) > GENERATED_ROLE_OUTPUT_OFFSET_MAXIMUM
        or abs(dy) > GENERATED_ROLE_OUTPUT_OFFSET_MAXIMUM
    ):
        raise RasterContractError(f"{label}: role output offset is out of range")
    shifted = {(x + dx, y + dy) for x, y in candidate.mask}
    if any(
        x < 0
        or x >= HIGH_RES_FRAME_WIDTH
        or y < 0
        or y >= HIGH_RES_FRAME_HEIGHT
        for x, y in shifted
    ):
        raise RasterContractError(
            f"{label}: fixed role output offset clips the imported candidate"
        )
    metrics = _generated_candidate_metrics(shifted, label)
    packed = high_res_frame_bytes(shifted)
    return HighResFrame(
        path=candidate.path,
        role=candidate.role,
        phase=candidate.phase,
        source_sha256=candidate.source_sha256,
        mask=frozenset(shifted),
        metrics=metrics,
        apparent_scale_ratio=candidate.apparent_scale_ratio,
        identity_jaccard=candidate.identity_jaccard,
        packed=packed,
    )


def compose_bounded_generated_frame(
    candidate: HighResFrame,
    baseline: HighResFrame,
    allowed_change_region: NativeRegionMaskLock,
    identity_mask: set[tuple[int, int]],
    *,
    label: str,
) -> HighResFrame:
    """Compose candidate pixels only inside an immutable native mask.

    This is the release definition, not cleanup: baseline pixels are copied
    byte-exactly outside the preauthorized region, candidate pixels are copied
    byte-exactly inside it, and no crop, fit, threshold, morphology, or manual
    repair is performed.
    """

    _require_live_region_lock(allowed_change_region, f"{label}/allowed-change")
    allowed = set(allowed_change_region.mask)
    if not allowed:
        raise RasterContractError(f"{label}: bounded-composite mask is empty")
    baseline_mask = set(baseline.mask)
    candidate_mask = set(candidate.mask)
    final_mask = (baseline_mask - allowed) | (candidate_mask & allowed)
    if (baseline_mask ^ final_mask) - allowed:
        raise RasterContractError(
            f"{label}: bounded composition changed baseline pixels outside its mask"
        )
    if final_mask & allowed != candidate_mask & allowed:
        raise RasterContractError(
            f"{label}: bounded composition did not preserve candidate pixels inside mask"
        )
    metrics = validate_high_res_mask(final_mask, label)
    apparent_scale = _high_res_apparent_scale(final_mask, identity_mask)
    lower, upper = HIGH_RES_ROLE_SCALE_ENVELOPES[candidate.role]
    if not lower <= apparent_scale <= upper:
        raise RasterContractError(
            f"{label}: composed apparent identity scale {apparent_scale:.3f} is "
            f"outside [{lower:.3f}, {upper:.3f}]"
        )
    similarity = jaccard(identity_mask, final_mask)
    minimum = ROLE_IDENTITY_JACCARD_MINIMUM[candidate.role]
    if similarity < minimum:
        raise RasterContractError(
            f"{label}: composed identity overlap {similarity:.3f} is below role "
            f"floor {minimum:.3f}"
        )
    packed = high_res_frame_bytes(final_mask)
    return HighResFrame(
        path=candidate.path,
        role=candidate.role,
        phase=candidate.phase,
        source_sha256=candidate.source_sha256,
        mask=frozenset(final_mask),
        metrics=metrics,
        apparent_scale_ratio=apparent_scale,
        identity_jaccard=similarity,
        packed=packed,
    )


def _load_imagegen_action_sheet_luma(
    path: Path, transform: ImageGenImportTransform, label: str
) -> Image.Image:
    """Composite one action sheet without extracting or writing partial files."""

    validate_imagegen_import_transform(transform, f"{label}/transform")
    if transform.source_canvas != IMAGEGEN_ACTION_SHEET_SOURCE_CANVAS:
        raise RasterContractError(
            f"{label}: action-sheet identity source family must be exact "
            f"{IMAGEGEN_ACTION_SHEET_SOURCE_CANVAS}"
        )
    with Image.open(path) as image:
        if image.size != IMAGEGEN_ACTION_SHEET_SOURCE_CANVAS:
            raise RasterContractError(
                f"{label}: action sheet canvas {image.size} differs from exact "
                f"{IMAGEGEN_ACTION_SHEET_SOURCE_CANVAS}; per-action resize is forbidden"
            )
        if image.mode not in {"RGB", "RGBA"}:
            raise RasterContractError(
                f"{label}: action sheet expects RGB/RGBA, got {image.mode!r}"
            )
        rgba = image.convert("RGBA")
        background = Image.new(
            "RGBA", image.size, (*transform.alpha_background, 255)
        )
        return Image.alpha_composite(background, rgba).convert("RGB").convert("L")


def _require_blank_action_sheet_gutters(
    gray: Image.Image, transform: ImageGenImportTransform, label: str
) -> None:
    threshold = transform.black_coverage_threshold_per_mille
    for gutter in IMAGEGEN_ACTION_SHEET_GUTTER_RECTS:
        values = gray.crop(gutter).tobytes()
        if any(_coverage_is_ink(value, threshold) for value in values):
            raise RasterContractError(
                f"{label}: center gutter {gutter} contains ink; labels, dividers, "
                "grid lines, and cross-cell artwork are forbidden"
            )


def _load_imagegen_action_cell(
    path: Path,
    gray: Image.Image,
    species: str,
    role: str,
    phase: int,
    transform: ImageGenImportTransform,
    identity_mask: set[tuple[int, int]],
) -> HighResFrame:
    """Import one fixed cell; no subject detection affects its transform."""

    label = f"{species}/{role}/{phase}"
    rect = IMAGEGEN_ACTION_SHEET_PHASE_RECTS[phase]
    cell = gray.crop(rect)
    cell_width, cell_height = cell.size
    if (cell_width, cell_height) != (560, 700):
        raise RasterContractError(f"{label}: internal action viewport changed")

    threshold = transform.black_coverage_threshold_per_mille
    source_values = list(cell.tobytes())
    source_mask = {
        (index % cell_width, index // cell_width)
        for index, value in enumerate(source_values)
        if _coverage_is_ink(value, threshold)
    }
    # This raw-cell gate catches outer-edge clipping, labels, dividers, debris,
    # and a second comparable animal before any area sampling can hide them.
    validate_source_mask(source_mask, cell_width, cell_height, f"{label}/raw-cell")

    histogram = cell.histogram()
    nonwhite = sum(histogram[:250])
    ambiguous = sum(histogram[17:239])
    ambiguous_fraction = ambiguous / nonwhite if nonwhite else 0.0
    if ambiguous_fraction > IMAGEGEN_MAX_AMBIGUOUS_SOURCE_FRACTION:
        raise RasterContractError(
            f"{label}: {ambiguous_fraction:.3f} of nonwhite cell pixels are "
            "mid-tone/antialiased; regenerate the complete named action"
        )

    reduced = cell.resize(IMAGEGEN_OUTPUT_CANVAS, Image.Resampling.BOX)
    reduced_values = list(reduced.tobytes())
    unshifted_mask = {
        (index % HIGH_RES_FRAME_WIDTH, index // HIGH_RES_FRAME_WIDTH)
        for index, value in enumerate(reduced_values)
        if _coverage_is_ink(value, threshold)
    }
    # Action cells are half the identity viewport in each dimension and have
    # their own identity-stage placement.  This second offset is lock data,
    # never a value derived from a phase or role.
    offset_x, offset_y = transform.action_output_offset
    mask = {(x + offset_x, y + offset_y) for x, y in unshifted_mask}
    if any(
        x < 0
        or x >= HIGH_RES_FRAME_WIDTH
        or y < 0
        or y >= HIGH_RES_FRAME_HEIGHT
        for x, y in mask
    ):
        raise RasterContractError(
            f"{label}: the identity-locked action output offset "
            f"{transform.action_output_offset} clips this phase; the complete action "
            "is rejected and per-cell fitting is forbidden"
        )

    low_threshold = max(0, threshold - IMAGEGEN_COVERAGE_STABILITY_PER_MILLE)
    high_threshold = min(1000, threshold + IMAGEGEN_COVERAGE_STABILITY_PER_MILLE)
    low_mask = {
        index
        for index, value in enumerate(reduced_values)
        if _coverage_is_ink(value, low_threshold)
    }
    high_mask = {
        index
        for index, value in enumerate(reduced_values)
        if _coverage_is_ink(value, high_threshold)
    }
    sensitive_pixels = len(low_mask ^ high_mask)
    maximum_sensitive = max(
        4,
        math.ceil(
            len(unshifted_mask) * IMAGEGEN_MAX_THRESHOLD_SENSITIVE_FRACTION
        ),
    )
    if sensitive_pixels > maximum_sensitive:
        raise RasterContractError(
            f"{label}: {sensitive_pixels} target pixels are coverage-threshold "
            f"sensitive (maximum {maximum_sensitive}); regenerate the action"
        )

    metrics = validate_high_res_mask(mask, label)
    apparent_scale = _high_res_apparent_scale(mask, identity_mask)
    lower, upper = HIGH_RES_ROLE_SCALE_ENVELOPES[role]
    if not lower <= apparent_scale <= upper:
        raise RasterContractError(
            f"{label}: apparent identity scale {apparent_scale:.3f} is outside "
            f"[{lower:.3f}, {upper:.3f}]; one bad cell rejects the action"
        )
    similarity = jaccard(identity_mask, mask)
    minimum = ROLE_IDENTITY_JACCARD_MINIMUM[role]
    if similarity < minimum:
        raise RasterContractError(
            f"{label}: identity overlap {similarity:.3f} is below role floor "
            f"{minimum:.3f}; one bad cell rejects the action"
        )

    # The byte-exact sheet hash is kept in SpeciesRaster.source_sha256.  This
    # composited-region hash proves the four fixed viewports are independently
    # populated and lets duplicate-cell gates run before serialization.
    source_region_sha256 = hashlib.sha256(cell.tobytes()).hexdigest()
    packed = high_res_frame_bytes(mask)
    return HighResFrame(
        path=path,
        role=role,
        phase=phase,
        source_sha256=source_region_sha256,
        mask=frozenset(mask),
        metrics=metrics,
        apparent_scale_ratio=apparent_scale,
        identity_jaccard=similarity,
        packed=packed,
    )


def load_imagegen_action_sheet_frames(
    path: Path,
    species: str,
    role: base.RoleSpec,
    transform: ImageGenImportTransform,
    identity_mask: set[tuple[int, int]],
) -> tuple[HighResFrame, ...]:
    """Fail the complete named action before returning any phase."""

    label = f"{species}/{role.name}"
    gray = _load_imagegen_action_sheet_luma(path, transform, label)
    _require_blank_action_sheet_gutters(gray, transform, label)
    frames = [
        _load_imagegen_action_cell(
            path,
            gray,
            species,
            role.name,
            phase,
            transform,
            identity_mask,
        )
        for phase in range(REQUIRED_FRAMES_PER_ROLE)
    ]
    validate_high_res_four_frame_role(role, frames)
    return tuple(frames)


def decode_high_res_frame_bytes(payload: bytes) -> set[tuple[int, int]]:
    """Decode one format-v2 frame without interpreting or changing pixels."""

    if len(payload) != HIGH_RES_FRAME_BYTES:
        raise RasterContractError(
            f"format-v2 frame has {len(payload)} bytes; "
            f"expected {HIGH_RES_FRAME_BYTES}"
        )
    return {
        (x, y)
        for y in range(HIGH_RES_FRAME_HEIGHT)
        for x in range(HIGH_RES_FRAME_WIDTH)
        if payload[y * (HIGH_RES_FRAME_WIDTH // 8) + x // 8]
        & (1 << (x & 7))
    }


def _exact_one_bit_mask(
    path: Path, expected_size: tuple[int, int], label: str
) -> set[tuple[int, int]]:
    """Load a final PNG only when its pixels already are the release raster."""

    with Image.open(path) as image:
        if image.size != expected_size:
            raise RasterContractError(
                f"{label}: canonical PNG is {image.size}; expected {expected_size}; "
                "format-v2 never resizes, crops, or pads release art"
            )
        if image.mode != "1":
            raise RasterContractError(
                f"{label}: canonical PNG mode is {image.mode!r}; expected exact "
                "1-bit mode '1' (no threshold, alpha, grayscale, or palette conversion)"
            )
        return {
            (x, y)
            for y in range(image.height)
            for x in range(image.width)
            if image.getpixel((x, y)) == 0
        }


def validate_high_res_mask(
    mask: set[tuple[int, int]], label: str, *, require_floor: bool = True
) -> SourceMetrics:
    """Validate an exact 64x80 final frame without mutating its pixels."""

    metrics = validate_source_mask(
        mask, HIGH_RES_FRAME_WIDTH, HIGH_RES_FRAME_HEIGHT, label
    )
    left, top, right, bottom = metrics.bounds
    if (
        left < HIGH_RES_SAFE_LEFT
        or top < HIGH_RES_SAFE_TOP
        or right > HIGH_RES_SAFE_RIGHT
        or bottom > HIGH_RES_FLOOR_Y
    ):
        raise RasterContractError(
            f"{label}: bounds {metrics.bounds} violate format-v2 safe stage "
            f"{(HIGH_RES_SAFE_LEFT, HIGH_RES_SAFE_TOP, HIGH_RES_SAFE_RIGHT, HIGH_RES_FLOOR_Y)}"
        )
    if require_floor and bottom != HIGH_RES_FLOOR_Y:
        raise RasterContractError(
            f"{label}: floor is y={bottom}; exact format-v2 floor is "
            f"y={HIGH_RES_FLOOR_Y}; frame is rejected, never translated"
        )
    axis = base.median_x(mask)
    if not (
        HIGH_RES_BODY_AXIS_X - HIGH_RES_CENTER_TOLERANCE
        <= axis
        <= HIGH_RES_BODY_AXIS_X + HIGH_RES_CENTER_TOLERANCE
    ):
        raise RasterContractError(
            f"{label}: body axis {axis} is outside centered stage range "
            f"[{HIGH_RES_BODY_AXIS_X - HIGH_RES_CENTER_TOLERANCE}, "
            f"{HIGH_RES_BODY_AXIS_X + HIGH_RES_CENTER_TOLERANCE}]"
        )

    components = sorted(base.connected_components(mask), key=len, reverse=True)
    if len(components) > HIGH_RES_MAX_COMPONENTS:
        raise RasterContractError(
            f"{label}: final frame has {len(components)} components; "
            f"maximum is {HIGH_RES_MAX_COMPONENTS}"
        )
    primary_fraction = len(components[0]) / len(mask)
    if primary_fraction < HIGH_RES_MIN_PRIMARY_FRACTION:
        raise RasterContractError(
            f"{label}: primary subject is only {primary_fraction:.3f} of final ink"
        )
    primary_bounds = base.bounds(components[0])
    debris_limit = max(2, math.ceil(len(components[0]) * 0.002))
    detached_debris = [
        len(component)
        for component in components[1:]
        if len(component) <= debris_limit
        and _outside_primary_box(component, primary_bounds)
    ]
    if detached_debris:
        raise RasterContractError(
            f"{label}: detached final-raster debris components "
            f"{detached_debris}; format-v2 never deletes them"
        )

    packed = high_res_frame_bytes(mask)
    if decode_high_res_frame_bytes(packed) != mask:
        raise RasterContractError(
            f"{label}: 64x80 1-bit packing round-trip changed pixels"
        )
    if any(
        (x, y) in mask
        for y in HIGH_RES_BOTTOM_GUARD_ROWS
        for x in range(HIGH_RES_FRAME_WIDTH)
    ):
        raise RasterContractError(
            f"{label}: bottom guard rows {HIGH_RES_BOTTOM_GUARD_ROWS} must be blank"
        )
    return metrics


def _high_res_apparent_scale(
    mask: set[tuple[int, int]], identity_mask: set[tuple[int, int]]
) -> float:
    if not mask or not identity_mask:
        raise RasterContractError("cannot compare scale of an empty final frame")
    return math.sqrt(len(mask) / len(identity_mask))


def load_high_res_frame(
    path: Path,
    role: str,
    phase: int,
    *,
    identity_mask: set[tuple[int, int]] | None = None,
) -> HighResFrame:
    """Load one exact final frame.  The returned mask equals the PNG pixels."""

    owner = path.parent.parent.name if path.parent.name == role else path.parent.name
    label = f"{owner}/{role}/{phase}"
    mask = _exact_one_bit_mask(
        path, (HIGH_RES_FRAME_WIDTH, HIGH_RES_FRAME_HEIGHT), label
    )
    metrics = validate_high_res_mask(mask, label)
    apparent_scale = 1.0
    similarity = 1.0
    if identity_mask is not None:
        apparent_scale = _high_res_apparent_scale(mask, identity_mask)
        if role not in HIGH_RES_ROLE_SCALE_ENVELOPES:
            raise RasterContractError(f"{label}: unknown animation role")
        lower, upper = HIGH_RES_ROLE_SCALE_ENVELOPES[role]
        if not lower <= apparent_scale <= upper:
            raise RasterContractError(
                f"{label}: apparent identity scale {apparent_scale:.3f} is "
                f"outside [{lower:.3f}, {upper:.3f}]; regenerate at the fixed "
                "camera scale instead of resizing"
            )
        similarity = jaccard(identity_mask, mask)
        minimum = ROLE_IDENTITY_JACCARD_MINIMUM[role]
        if similarity < minimum:
            raise RasterContractError(
                f"{label}: identity overlap {similarity:.3f} is below role "
                f"floor {minimum:.3f}"
            )
    packed = high_res_frame_bytes(mask)
    return HighResFrame(
        path=path,
        role=role,
        phase=phase,
        source_sha256=sha256_file(path),
        mask=frozenset(mask),
        metrics=metrics,
        apparent_scale_ratio=apparent_scale,
        identity_jaccard=similarity,
        packed=packed,
    )


def validate_high_res_four_frame_role(
    role: base.RoleSpec, frames: list[HighResFrame]
) -> None:
    """Reject missing, duplicate, scale-popping, or incoherent phase sets."""

    if frames:
        # Independent frames live under <species>/<role>/, whereas all four
        # action-sheet frames deliberately point at <species>/<role>.png.
        # Keep diagnostics correct without inferring which loader to invoke.
        first_path = frames[0].path
        owner = (
            first_path.parent.name
            if all(frame.path == first_path for frame in frames)
            else first_path.parent.parent.name
        )
        label = f"{owner}/{role.name}"
    else:
        label = role.name
    phases = [frame.phase for frame in frames]
    if len(frames) != REQUIRED_FRAMES_PER_ROLE or phases != list(
        range(REQUIRED_FRAMES_PER_ROLE)
    ):
        raise RasterContractError(
            f"{label}: exact independent frames 00.png..03.png are required; "
            f"got phases {phases}"
        )
    hashes = [frame.source_sha256 for frame in frames]
    if len(set(hashes)) != REQUIRED_FRAMES_PER_ROLE:
        raise RasterContractError(
            f"{label}: all four exact canonical PNGs must be distinct"
        )
    packed_hashes = [hashlib.sha256(frame.packed).hexdigest() for frame in frames]
    if len(set(packed_hashes)) != REQUIRED_FRAMES_PER_ROLE:
        raise RasterContractError(
            f"{label}: area import collapsed one or more phases; all four "
            "native 64x80 frames must remain distinct"
        )
    masks = [set(frame.mask) for frame in frames]
    changed_pixels = len(set().union(*masks) - set.intersection(*masks))
    if changed_pixels < HIGH_RES_MIN_CHANGED_PIXELS:
        raise RasterContractError(
            f"{label}: animation changes only {changed_pixels} native pixels; "
            f"minimum is {HIGH_RES_MIN_CHANGED_PIXELS}"
        )

    for previous, current in zip(masks, masks[1:]):
        difference = len(previous ^ current)
        if difference < HIGH_RES_MIN_PAIR_CHANGED_PIXELS:
            raise RasterContractError(
                f"{label}: adjacent phases change only {difference} native pixels"
            )
        transition = difference / len(previous | current)
        maximum = HIGH_RES_ROLE_TRANSITION_MAXIMUM[role.name]
        if transition > maximum:
            raise RasterContractError(
                f"{label}: adjacent transition changes {transition:.3f} of the "
                f"subject (maximum {maximum:.3f}); likely identity/pose discontinuity"
            )

    scales = [frame.apparent_scale_ratio for frame in frames]
    scale_pop = max(scales) / min(scales)
    maximum_pop = HIGH_RES_ROLE_SCALE_POP_MAXIMUM[role.name]
    if scale_pop > maximum_pop:
        raise RasterContractError(
            f"{label}: within-action apparent scale pop is {scale_pop:.3f}; "
            f"maximum is {maximum_pop:.3f}"
        )


def _require_live_region_lock(region: NativeRegionMaskLock, label: str) -> None:
    try:
        native_region_mask_record(region)
    except RasterContractError as error:
        raise RasterContractError(f"{label}: {error}") from error


def _visual_rect_points(rect: list[int]) -> set[tuple[int, int]]:
    left, top, right, bottom = rect
    return {
        (x, y)
        for y in range(top, bottom + 1)
        for x in range(left, right + 1)
    }


def _visual_mask_in_rect(
    mask: set[tuple[int, int]], rect: list[int]
) -> set[tuple[int, int]]:
    left, top, right, bottom = rect
    return {
        (x, y)
        for x, y in mask
        if left <= x <= right and top <= y <= bottom
    }


def _visual_centroid(mask: set[tuple[int, int]]) -> tuple[float, float]:
    if not mask:
        raise RasterContractError("visual gate cannot measure an empty landmark")
    return (
        sum(x for x, _y in mask) / len(mask),
        sum(y for _x, y in mask) / len(mask),
    )


def _visual_shift_mask(
    mask: set[tuple[int, int]], dx: int, dy: int
) -> set[tuple[int, int]]:
    return {(x + dx, y + dy) for x, y in mask}


def _visual_shift_rect(rect: list[int], dx: int, dy: int) -> list[int]:
    left, top, right, bottom = rect
    shifted = [left + dx, top + dy, right + dx, bottom + dy]
    if (
        shifted[0] < 0
        or shifted[1] < 0
        or shifted[2] >= HIGH_RES_FRAME_WIDTH
        or shifted[3] >= HIGH_RES_FRAME_HEIGHT
    ):
        raise RasterContractError("visual gate translated ROI leaves the 64x80 canvas")
    return shifted


def _visual_trajectory_rect(
    source_rect: list[int], target_rect: list[int]
) -> list[int]:
    """Return the inclusive canvas region swept from source to target ROI."""

    return [
        min(source_rect[0], target_rect[0]),
        min(source_rect[1], target_rect[1]),
        max(source_rect[2], target_rect[2]),
        max(source_rect[3], target_rect[3]),
    ]


def _visual_mask_is_inside_rect_and_canvas(
    mask: set[tuple[int, int]], rect: list[int]
) -> bool:
    left, top, right, bottom = rect
    return all(
        0 <= x < HIGH_RES_FRAME_WIDTH
        and 0 <= y < HIGH_RES_FRAME_HEIGHT
        and left <= x <= right
        and top <= y <= bottom
        for x, y in mask
    )


def _visual_gate_error(
    identity_key: str,
    role: str,
    kind: str,
    detail: str,
    reason_code: str | None = None,
) -> SpeciesRoleVisualGateError:
    reason = reason_code or SPECIES_ROLE_VISUAL_GATE_REASON_CODES[
        (identity_key, role, kind)
    ]
    return SpeciesRoleVisualGateError(
        f"{identity_key}/{role}: species-role visual gate {reason} failed: {detail}",
        reason,
    )


def _visual_gate_failure(
    identity_key: str,
    role: str,
    kind: str,
    detail: str,
    reason_code: str | None = None,
) -> None:
    raise _visual_gate_error(identity_key, role, kind, detail, reason_code)


def _raise_accumulated_visual_gate_failures(
    identity_key: str,
    role: str,
    failures: list[SpeciesRoleVisualGateError],
) -> None:
    reason_codes = tuple(
        reason_code
        for failure in failures
        for reason_code in failure.reason_codes
    )
    raise SpeciesRoleVisualGateError(
        f"{identity_key}/{role}: species-role visual gates failed in deterministic "
        f"order: {'; '.join(str(failure) for failure in failures)}",
        reason_codes,
    )


def _visual_gate_reason_codes(
    identity_key: str, role: str, kind: str
) -> list[str]:
    if (identity_key, role, kind) == ("rabbit", "blink", "eye_sequence"):
        return [
            "RABBIT_BLINK_EYE_MASS_SEQUENCE",
            "RABBIT_BLINK_LID_GEOMETRY",
            "RABBIT_BLINK_EYE_CENTROID_DRIFT",
        ]
    return [SPECIES_ROLE_VISUAL_GATE_REASON_CODES[(identity_key, role, kind)]]


def _visual_detect_landmark_component(
    frame: set[tuple[int, int]],
    window: list[int],
    pixel_range: list[int],
    bbox_maximum: list[int],
) -> set[tuple[int, int]] | None:
    # Detect on the full canvas, then use the policy window only to select and
    # contain the landmark. This prevents a component that crosses the ROI
    # border from looking valid after cropping. A surrounding-anatomy
    # component may intersect the window only when its full component crosses
    # the window; every wholly contained component is landmark-significant.
    window_points = _visual_rect_points(window)
    contained: list[set[tuple[int, int]]] = []
    for component in base.connected_components(frame):
        if not component & window_points:
            continue
        if component <= window_points:
            contained.append(component)
    if len(contained) != 1:
        return None
    component = contained[0]
    left, top, right, bottom = base.bounds(component)
    width = right - left + 1
    height = bottom - top + 1
    if (
        not pixel_range[0] <= len(component) <= pixel_range[1]
        or width > bbox_maximum[0]
        or height > bbox_maximum[1]
    ):
        return None
    return component


def _run_visual_required_8_connected_anchors_gate(
    identity_key: str,
    role: str,
    frame_masks: list[set[tuple[int, int]]],
    gate: dict[str, object],
) -> dict[str, object]:
    phase_results: list[dict[str, object]] = []
    anchors = dict(gate["anchors"])
    for phase in gate["apply_to_phases"]:
        frame = frame_masks[phase]
        components = base.connected_components(frame)
        component_ids: dict[str, set[int]] = {}
        anchor_ink: dict[str, int] = {}
        for name, rect in anchors.items():
            selected = _visual_mask_in_rect(frame, rect)
            anchor_ink[name] = len(selected)
            component_ids[name] = {
                index
                for index, component in enumerate(components)
                if component & selected
            }
        common = set.intersection(*component_ids.values())
        exact_shared_component = (
            len(common) == 1
            and all(ids == common for ids in component_ids.values())
        )
        phase_results.append(
            {
                "anchor_component_count": {
                    name: len(ids) for name, ids in component_ids.items()
                },
                "anchor_ink_pixels": anchor_ink,
                "common_component_count": len(common),
                "phase": phase,
            }
        )
        if not all(anchor_ink.values()) or not exact_shared_component:
            _visual_gate_failure(
                identity_key,
                role,
                "required_8_connected_anchors",
                f"P{phase} every anchor ROI does not contain only the same "
                "single eight-connected foreground component",
            )
    return {"phases": phase_results}


def _run_visual_phase_delta_locality_gate(
    identity_key: str,
    role: str,
    frame_masks: list[set[tuple[int, int]]],
    gate: dict[str, object],
) -> dict[str, object]:
    baseline = frame_masks[gate["baseline_phase"]]
    phase_results: list[dict[str, object]] = []
    for phase_rule_value in gate["phases"]:
        phase_rule = dict(phase_rule_value)
        allowed: set[tuple[int, int]] = set()
        for rect in phase_rule["allowed_rects"]:
            allowed.update(_visual_rect_points(rect))
        phase = phase_rule["phase"]
        delta = baseline ^ frame_masks[phase]
        outside = delta - allowed
        phase_results.append(
            {
                "outside_xor_pixels": len(outside),
                "phase": phase,
                "total_xor_pixels": len(delta),
            }
        )
        if len(outside) > phase_rule["maximum_outside_xor_pixels"]:
            _visual_gate_failure(
                identity_key,
                role,
                "phase_delta_locality",
                f"P{phase} has {len(outside)} outside-mask XOR pixels",
            )
    return {"phases": phase_results}


def _run_visual_loop_seam_gate(
    identity_key: str,
    role: str,
    frame_masks: list[set[tuple[int, int]]],
    gate: dict[str, object],
) -> dict[str, object]:
    changed = len(
        frame_masks[gate["from_phase"]] ^ frame_masks[gate["to_phase"]]
    )
    if changed > gate["maximum_xor_pixels"]:
        _visual_gate_failure(
            identity_key,
            role,
            "loop_seam",
            f"P3-to-P0 XOR is {changed} pixels",
        )
    return {"xor_pixels": changed}


def _run_visual_pupil_only_gate(
    identity_key: str,
    role: str,
    frame_masks: list[set[tuple[int, int]]],
    gate: dict[str, object],
) -> dict[str, object]:
    allowed = set().union(
        *(
            _visual_rect_points(rect)
            for rect in dict(gate["eye_rects"]).values()
        )
    )
    baseline = frame_masks[gate["baseline_phase"]]
    outside = [len((frame ^ baseline) - allowed) for frame in frame_masks]
    if any(value > gate["maximum_outside_xor_pixels"] for value in outside):
        _visual_gate_failure(
            identity_key,
            role,
            "pupil_only",
            f"outside-pupil XOR sequence is {outside}",
        )
    return {"outside_xor_pixels": outside}


def _run_visual_per_eye_occupancy_gate(
    identity_key: str,
    role: str,
    frame_masks: list[set[tuple[int, int]]],
    gate: dict[str, object],
) -> dict[str, object]:
    counts: dict[str, list[int]] = {}
    for name, rect in dict(gate["eye_rects"]).items():
        rect_points = _visual_rect_points(rect)
        counts[name] = []
        for phase, frame in enumerate(frame_masks):
            components = [
                component
                for component in base.connected_components(frame)
                if component & rect_points
            ]
            if len(components) != 1 or not components[0] <= rect_points:
                _visual_gate_failure(
                    identity_key,
                    role,
                    "per_eye_occupancy",
                    f"P{phase} {name} eye is ambiguous or crosses its ROI border",
                )
            counts[name].append(len(components[0]))
    ranges = gate["phase_min_max_pixels"]
    if any(
        not ranges[phase][0] <= value <= ranges[phase][1]
        for values in counts.values()
        for phase, value in enumerate(values)
    ):
        _visual_gate_failure(
            identity_key,
            role,
            "per_eye_occupancy",
            f"per-eye occupancy sequence is {counts}",
        )
    return {"ink_pixels": counts}


def _run_visual_localized_redraw_gate(
    identity_key: str,
    role: str,
    frame_masks: list[set[tuple[int, int]]],
    gate: dict[str, object],
) -> dict[str, object]:
    adjacent = [
        len(frame_masks[phase - 1] ^ frame_masks[phase])
        for phase in range(1, REQUIRED_FRAMES_PER_ROLE)
    ]
    if any(value > gate["maximum_adjacent_xor_pixels"] for value in adjacent):
        _visual_gate_failure(
            identity_key,
            role,
            "localized_redraw",
            f"adjacent XOR sequence is {adjacent}",
        )
    return {"adjacent_xor_pixels": adjacent}


def _run_visual_rigid_pupil_translation_gate(
    identity_key: str,
    role: str,
    frame_masks: list[set[tuple[int, int]]],
    gate: dict[str, object],
) -> tuple[
    dict[str, object],
    list[tuple[int, int]],
]:
    detected: dict[str, list[set[tuple[int, int]]]] = {}
    for name, window in dict(gate["tracking_windows"]).items():
        detected[name] = []
        for phase, frame in enumerate(frame_masks):
            component = _visual_detect_landmark_component(
                frame,
                window,
                gate["component_pixels_min_max"],
                gate["component_bbox_max"],
            )
            if component is None:
                _visual_gate_failure(
                    identity_key,
                    role,
                    "rigid_pupil_translation",
                    f"P{phase} {name} pupil is missing or ambiguous",
                )
            detected[name].append(component)
    pupil_translations: dict[str, list[tuple[int, int]]] = {}
    for name, components in detected.items():
        base_component = components[0]
        base_left, base_top, _base_right, _base_bottom = base.bounds(
            base_component
        )
        window = dict(gate["tracking_windows"])[name]
        pupil_translations[name] = []
        for phase, component in enumerate(components):
            left, top, _right, _bottom = base.bounds(component)
            dx = left - base_left
            dy = top - base_top
            expected = _visual_shift_mask(base_component, dx, dy)
            if (
                not _visual_mask_is_inside_rect_and_canvas(expected, window)
                or component != expected
            ):
                _visual_gate_failure(
                    identity_key,
                    role,
                    "rigid_pupil_translation",
                    f"P{phase} {name} pupil is not an exact integer translation "
                    "of its P0 component",
                )
            pupil_translations[name].append((dx, dy))
    limits = gate["maximum_inter_eye_translation_delta"]
    translation_delta: list[list[int]] = []
    for phase in range(REQUIRED_FRAMES_PER_ROLE):
        delta_x = abs(
            pupil_translations["left"][phase][0]
            - pupil_translations["right"][phase][0]
        )
        delta_y = abs(
            pupil_translations["left"][phase][1]
            - pupil_translations["right"][phase][1]
        )
        translation_delta.append([delta_x, delta_y])
        if delta_x > limits[0] or delta_y > limits[1]:
            _visual_gate_failure(
                identity_key,
                role,
                "rigid_pupil_translation",
                f"P{phase} inter-eye translation delta is {[delta_x, delta_y]}",
            )
    return (
        {
            "inter_eye_translation_delta": translation_delta,
            "pupil_translation": {
                name: [[x, y] for x, y in values]
                for name, values in pupil_translations.items()
            },
        },
        list(pupil_translations["left"]),
    )


def _run_visual_gill_base_shared_transform_gate(
    identity_key: str,
    role: str,
    frame_masks: list[set[tuple[int, int]]],
    gate: dict[str, object],
    shared_pupil_translations: list[tuple[int, int]] | None,
) -> dict[str, object]:
    if shared_pupil_translations is None:
        raise RasterContractError(
            f"{identity_key}/{role}: gill gate dependency was not evaluated"
        )
    phase_results: list[dict[str, object]] = []
    for phase in range(REQUIRED_FRAMES_PER_ROLE):
        dx, dy = shared_pupil_translations[phase]
        template_xor: dict[str, int] = {}
        source_residue: dict[str, int] = {}
        trajectory_rects: dict[str, list[int]] = {}
        for name, rect in dict(gate["template_rects"]).items():
            template = _visual_mask_in_rect(
                frame_masks[gate["template_phase"]], rect
            )
            if not template:
                _visual_gate_failure(
                    identity_key,
                    role,
                    "gill_base_shared_transform",
                    f"P0 {name} template is empty",
                )
            expected = _visual_shift_mask(template, dx, dy)
            target_rect = _visual_shift_rect(rect, dx, dy)
            trajectory_rect = _visual_trajectory_rect(rect, target_rect)
            actual = _visual_mask_in_rect(frame_masks[phase], trajectory_rect)
            changed = len(expected ^ actual)
            template_xor[name] = changed
            source_only = _visual_rect_points(rect) - _visual_rect_points(target_rect)
            source_residue[name] = len((actual - expected) & source_only)
            trajectory_rects[name] = trajectory_rect
            if changed > gate["maximum_template_xor_pixels_each"]:
                _visual_gate_failure(
                    identity_key,
                    role,
                    "gill_base_shared_transform",
                    f"P{phase} {name} template XOR is {changed}",
                )
        phase_results.append(
            {
                "phase": phase,
                "source_residue_pixels": source_residue,
                "template_xor_pixels": template_xor,
                "trajectory_rects": trajectory_rects,
                "translation": [dx, dy],
            }
        )
    return {"phases": phase_results}


def _run_visual_split_nose_topology_gate(
    identity_key: str,
    role: str,
    frame_masks: list[set[tuple[int, int]]],
    gate: dict[str, object],
) -> dict[str, object]:
    frame = frame_masks[gate["phase"]]
    ink_counts: dict[str, int] = {}
    for name, roi_value in dict(gate["ink_rois"]).items():
        roi = dict(roi_value)
        count = len(_visual_mask_in_rect(frame, roi["rect"]))
        ink_counts[name] = count
        if count < roi["minimum_ink_pixels"]:
            _visual_gate_failure(
                identity_key,
                role,
                "split_nose_topology",
                f"{name} contains only {count} ink pixels",
            )
    white_counts: dict[str, int] = {}
    for name, roi_value in dict(gate["white_rois"]).items():
        roi = dict(roi_value)
        count = len(_visual_mask_in_rect(frame, roi["rect"]))
        white_counts[name] = count
        if count > roi["maximum_ink_pixels"]:
            _visual_gate_failure(
                identity_key,
                role,
                "split_nose_topology",
                f"{name} contains {count} ink pixels",
            )
    return {
        "ink_roi_pixels": ink_counts,
        "white_roi_ink_pixels": white_counts,
    }


def _run_visual_cadence_gate(
    identity_key: str,
    role: str,
    actual_durations: list[int],
    gate: dict[str, object],
) -> dict[str, object]:
    if actual_durations != gate["required_durations_ms"]:
        _visual_gate_failure(
            identity_key,
            role,
            "cadence",
            f"actual cadence is {actual_durations}",
        )
    return {"durations_ms": actual_durations}


def _run_visual_eye_sequence_gate(
    identity_key: str,
    role: str,
    frame_masks: list[set[tuple[int, int]]],
    gate: dict[str, object],
) -> dict[str, object]:
    eye_rect = gate["eye_rect"]
    eye_rect_points = _visual_rect_points(eye_rect)
    containment_rect = gate["containment_rect"]
    eyes = [
        _visual_mask_in_rect(frame, eye_rect)
        for frame in frame_masks
    ]
    if any(not eye for eye in eyes):
        _visual_gate_failure(
            identity_key, role, "eye_sequence", "one or more eye phases are empty"
        )
    masses = [len(eye) for eye in eyes]
    ratios = [mass / masses[0] for mass in masses]
    ranges = gate["phase_mass_ratio_min_max_from_p0"]
    failures: list[SpeciesRoleVisualGateError] = []
    containment_component_counts: list[int] = []
    outside_eye_rect_pixels: list[int] = []
    full_component_counts: list[int] = []
    full_component_contained: list[bool] = []
    for frame, eye in zip(frame_masks, eyes):
        containment_mask = _visual_mask_in_rect(frame, containment_rect)
        containment_component_counts.append(
            len(base.connected_components(containment_mask))
        )
        outside_eye_rect_pixels.append(
            len(containment_mask - eye_rect_points)
        )
        full_components = [
            component
            for component in base.connected_components(frame)
            if component & eye
        ]
        full_component_counts.append(len(full_components))
        full_component_contained.append(
            len(full_components) == 1
            and full_components[0] <= eye_rect_points
        )
    containment_failed = (
        any(count != 1 for count in containment_component_counts)
        or any(outside_eye_rect_pixels)
        or not all(full_component_contained)
    )
    if any(
        not ranges[phase][0] <= ratio <= ranges[phase][1]
        for phase, ratio in enumerate(ratios)
    ):
        failures.append(
            _visual_gate_error(
                identity_key,
                role,
                "eye_sequence",
                f"eye mass ratios are {ratios}",
            )
        )
    closed = eyes[gate["closed_phase"]]
    components = base.connected_components(closed)
    left, top, right, bottom = base.bounds(closed)
    height = bottom - top + 1
    closed_geometry_failed = (
        len(components) != gate["closed_phase_required_component_count"]
        or height != gate["closed_phase_required_height_pixels"]
    )
    if containment_failed or closed_geometry_failed:
        failures.append(
            _visual_gate_error(
                identity_key,
                role,
                "eye_sequence",
                "eye/lid containment or closed geometry failed; "
                f"containment components={containment_component_counts}, "
                f"outside pixels={outside_eye_rect_pixels}, "
                f"full components={full_component_counts}, closed lid has "
                f"{len(components)} components and height {height}",
                "RABBIT_BLINK_LID_GEOMETRY",
            )
        )
    base_center = _visual_centroid(eyes[0])
    distances = [
        math.dist(base_center, _visual_centroid(eye)) for eye in eyes
    ]
    if any(
        distance > gate["maximum_centroid_distance_from_p0"]
        for distance in distances
    ):
        failures.append(
            _visual_gate_error(
                identity_key,
                role,
                "eye_sequence",
                f"eye centroid distances are {distances}",
                "RABBIT_BLINK_EYE_CENTROID_DRIFT",
            )
        )
    if failures:
        _raise_accumulated_visual_gate_failures(identity_key, role, failures)
    return {
        "centroid_distance_from_p0": distances,
        "closed_component_count": len(components),
        "closed_height_pixels": height,
        "containment_component_count": containment_component_counts,
        "containment_outside_eye_rect_pixels": outside_eye_rect_pixels,
        "full_component_contained": full_component_contained,
        "full_component_count": full_component_counts,
        "ink_pixels": masses,
        "mass_ratio_from_p0": ratios,
    }


def _run_visual_skull_freeze_gate(
    identity_key: str,
    role: str,
    frame_masks: list[set[tuple[int, int]]],
    gate: dict[str, object],
) -> dict[str, object]:
    baseline = _visual_mask_in_rect(
        frame_masks[gate["baseline_phase"]], gate["rect"]
    )
    changed = [
        len(baseline ^ _visual_mask_in_rect(frame, gate["rect"]))
        for frame in frame_masks
    ]
    if any(value > gate["maximum_xor_pixels"] for value in changed):
        _visual_gate_failure(
            identity_key,
            role,
            "skull_freeze",
            f"skull XOR sequence is {changed}",
        )
    return {"xor_pixels": changed}


def _run_visual_ear_base_freeze_gate(
    identity_key: str,
    role: str,
    frame_masks: list[set[tuple[int, int]]],
    gate: dict[str, object],
) -> dict[str, object]:
    changed_by_ear: dict[str, list[int]] = {}
    for name, rect in dict(gate["rects"]).items():
        baseline = _visual_mask_in_rect(
            frame_masks[gate["baseline_phase"]], rect
        )
        changed = [
            len(baseline ^ _visual_mask_in_rect(frame, rect))
            for frame in frame_masks
        ]
        changed_by_ear[name] = changed
        if any(value > gate["maximum_xor_pixels_each"] for value in changed):
            _visual_gate_failure(
                identity_key,
                role,
                "ear_base_freeze",
                f"{name} ear-base XOR sequence is {changed}",
            )
    return {"xor_pixels": changed_by_ear}


def _run_visual_local_scale_ink_gate(
    identity_key: str,
    role: str,
    frame_masks: list[set[tuple[int, int]]],
    gate: dict[str, object],
) -> dict[str, object]:
    tracking_rect = gate["rect"]
    baseline_components = [
        component
        for component in base.connected_components(frame_masks[0])
        if _visual_mask_in_rect(component, tracking_rect)
    ]
    if not baseline_components:
        _visual_gate_failure(
            identity_key,
            role,
            "local_scale_ink",
            "P0 has no full-canvas component owning tracking-ROI ink",
        )
    baseline_seeds = [
        _visual_mask_in_rect(component, tracking_rect)
        for component in baseline_components
    ]
    seed_sizes = [len(seed) for seed in baseline_seeds]
    maximum_seed_size = max(seed_sizes)
    if seed_sizes.count(maximum_seed_size) != 1:
        _visual_gate_failure(
            identity_key,
            role,
            "local_scale_ink",
            "P0 primary full-canvas component seed is ambiguous",
        )
    primary_seed_index = seed_sizes.index(maximum_seed_size)
    primary_seed = baseline_seeds[primary_seed_index]

    tracked_components: list[set[tuple[int, int]]] = []
    roi_component_counts: list[int] = []
    for phase, frame in enumerate(frame_masks):
        roi_owners = [
            component
            for component in base.connected_components(frame)
            if _visual_mask_in_rect(component, tracking_rect)
        ]
        roi_component_counts.append(len(roi_owners))
        owner_seed_ids = [
            {
                index
                for index, seed in enumerate(baseline_seeds)
                if component & seed
            }
            for component in roi_owners
        ]
        seed_owner_counts = [
            sum(index in seed_ids for seed_ids in owner_seed_ids)
            for index in range(len(baseline_seeds))
        ]
        primary_owners = [
            component
            for component, seed_ids in zip(roi_owners, owner_seed_ids)
            if primary_seed_index in seed_ids
        ]
        if (
            any(not seed_ids for seed_ids in owner_seed_ids)
            or any(count != 1 for count in seed_owner_counts)
            or len(primary_owners) != 1
        ):
            _visual_gate_failure(
                identity_key,
                role,
                "local_scale_ink",
                f"P{phase} tracking-ROI components do not retain unambiguous "
                "P0-seed ownership",
            )
        tracked_components.append(primary_owners[0])

    tracking_rect_points = _visual_rect_points(tracking_rect)
    local_ink = [
        len(component & tracking_rect_points)
        for component in tracked_components
    ]
    base_ink = local_ink[0]
    ratios = [ink / base_ink for ink in local_ink]
    full_bounds = [
        list(base.bounds(component)) for component in tracked_components
    ]
    baseline_bounds = full_bounds[0]
    maximum_expansion = gate["maximum_left_top_bbox_expansion_pixels"]
    ratio_range = gate["ink_ratio_min_max_from_p0"]
    left_border, top_border, _right_border, _bottom_border = tracking_rect
    if any(
        not ratio_range[0] <= ratio <= ratio_range[1]
        for ratio in ratios
    ) or any(
        candidate[0] < baseline_bounds[0] - maximum_expansion
        or candidate[1] < baseline_bounds[1] - maximum_expansion
        or candidate[0] < left_border
        or candidate[1] < top_border
        for candidate in full_bounds
    ):
        _visual_gate_failure(
            identity_key,
            role,
            "local_scale_ink",
            f"full-canvas tracked ink ratios/bounds are {ratios}/{full_bounds}",
        )
    return {
        "full_canvas_bounds": full_bounds,
        "ink_ratio_from_p0": ratios,
        "local_ink_pixels": local_ink,
        "p0_primary_seed_pixels": len(primary_seed),
        "p0_roi_component_count": len(baseline_seeds),
        "roi_component_count": roi_component_counts,
    }


def _run_species_role_visual_gate_kind(
    kind: str,
    identity_key: str,
    role: str,
    frame_masks: list[set[tuple[int, int]]],
    actual_durations: list[int],
    gate: dict[str, object],
    shared_pupil_translations: list[tuple[int, int]] | None,
) -> tuple[
    dict[str, object],
    list[tuple[int, int]] | None,
]:
    if kind == "required_8_connected_anchors":
        measurements = _run_visual_required_8_connected_anchors_gate(
            identity_key, role, frame_masks, gate
        )
    elif kind == "phase_delta_locality":
        measurements = _run_visual_phase_delta_locality_gate(
            identity_key, role, frame_masks, gate
        )
    elif kind == "loop_seam":
        measurements = _run_visual_loop_seam_gate(
            identity_key, role, frame_masks, gate
        )
    elif kind == "pupil_only":
        measurements = _run_visual_pupil_only_gate(
            identity_key, role, frame_masks, gate
        )
    elif kind == "per_eye_occupancy":
        measurements = _run_visual_per_eye_occupancy_gate(
            identity_key, role, frame_masks, gate
        )
    elif kind == "localized_redraw":
        measurements = _run_visual_localized_redraw_gate(
            identity_key, role, frame_masks, gate
        )
    elif kind == "rigid_pupil_translation":
        return _run_visual_rigid_pupil_translation_gate(
            identity_key, role, frame_masks, gate
        )
    elif kind == "gill_base_shared_transform":
        measurements = _run_visual_gill_base_shared_transform_gate(
            identity_key,
            role,
            frame_masks,
            gate,
            shared_pupil_translations,
        )
    elif kind == "split_nose_topology":
        measurements = _run_visual_split_nose_topology_gate(
            identity_key, role, frame_masks, gate
        )
    elif kind == "cadence":
        measurements = _run_visual_cadence_gate(
            identity_key, role, actual_durations, gate
        )
    elif kind == "eye_sequence":
        measurements = _run_visual_eye_sequence_gate(
            identity_key, role, frame_masks, gate
        )
    elif kind == "skull_freeze":
        measurements = _run_visual_skull_freeze_gate(
            identity_key, role, frame_masks, gate
        )
    elif kind == "ear_base_freeze":
        measurements = _run_visual_ear_base_freeze_gate(
            identity_key, role, frame_masks, gate
        )
    elif kind == "local_scale_ink":
        measurements = _run_visual_local_scale_ink_gate(
            identity_key, role, frame_masks, gate
        )
    else:
        raise RasterContractError(
            f"{identity_key}/{role}: unsupported parsed visual gate {kind}"
        )
    return measurements, shared_pupil_translations


def species_role_visual_gate_evidence_sha256(
    evidence: dict[str, object],
) -> str:
    return _canonical_json_sha256(evidence)


def validate_species_role_visual_gates(
    policy: SpeciesRoleVisualGatePolicy,
    identity_key: str,
    identity_frame_sha256: str,
    role: base.RoleSpec,
    frames: list[HighResFrame] | tuple[HighResFrame, ...],
    durations_ms: tuple[int, int, int, int] | list[int] | None,
) -> dict[str, object] | None:
    """Apply only an explicitly configured species/role visual policy.

    The policy's identity-frame hash authenticates the coordinate basis before
    any ROI is interpreted. Unconfigured species/roles retain the pre-existing
    raster and semantic gates and receive no inferred anatomy policy.
    """

    _require_species_role_visual_gate_policy_integrity(policy)
    key = (identity_key, role.name)
    entry = policy.entries.get(key)
    if entry is None:
        return None
    if identity_key in PROTECTED_STARTERS or key == ("ferret", "blink"):
        raise RasterContractError(
            f"{identity_key}/{role.name}: protected companion entered visual gates"
        )
    actual_identity_hash = _require_lower_sha256(
        identity_frame_sha256,
        f"{identity_key}/{role.name}/visual-gate-identity-frame",
    )
    if actual_identity_hash != entry.identity_frame_sha256:
        raise RasterContractError(
            f"{identity_key}/{role.name}: visual gate identity-frame SHA-256 "
            "does not authenticate the policy coordinate basis"
        )
    if len(frames) != REQUIRED_FRAMES_PER_ROLE:
        raise RasterContractError(
            f"{identity_key}/{role.name}: visual gate requires exact P0..P3"
        )
    frame_masks: list[set[tuple[int, int]]] = []
    frame_hashes: list[str] = []
    for phase, frame in enumerate(frames):
        if frame.role != role.name or frame.phase != phase:
            raise RasterContractError(
                f"{identity_key}/{role.name}: visual gate frame order drifted"
            )
        mask = set(frame.mask)
        if high_res_frame_bytes(mask) != frame.packed:
            raise RasterContractError(
                f"{identity_key}/{role.name}/P{phase}: visual gate input packing drifted"
            )
        frame_masks.append(mask)
        frame_hashes.append(hashlib.sha256(frame.packed).hexdigest())
    if durations_ms is None:
        raise RasterContractError(
            f"{identity_key}/{role.name}: visual gate requires actual role cadence"
        )
    if (
        not isinstance(durations_ms, (tuple, list))
        or len(durations_ms) != REQUIRED_FRAMES_PER_ROLE
        or any(type(value) is not int or value <= 0 for value in durations_ms)
    ):
        raise RasterContractError(
            f"{identity_key}/{role.name}: visual gate cadence is malformed"
        )
    actual_durations = list(durations_ms)
    gate_results: dict[str, object] = {}
    shared_pupil_translations: list[tuple[int, int]] | None = None
    failures: list[SpeciesRoleVisualGateError] = []

    for kind in SPECIES_ROLE_VISUAL_GATE_EXECUTION_ORDER:
        if kind not in entry.gates:
            continue
        # Gill evidence is meaningful only after every pupil phase has
        # established an exact integer P0 translation. A pupil failure is
        # already diagnosed; inventing a downstream gill failure from an
        # unavailable transform would be false evidence.
        if (
            kind == "gill_base_shared_transform"
            and shared_pupil_translations is None
        ):
            continue
        gate_value = entry.gates[kind]
        gate = dict(gate_value)
        try:
            (
                measurements,
                shared_pupil_translations,
            ) = _run_species_role_visual_gate_kind(
                kind,
                identity_key,
                role.name,
                frame_masks,
                actual_durations,
                gate,
                shared_pupil_translations,
            )
        except SpeciesRoleVisualGateError as error:
            failures.append(error)
            continue
        gate_results[kind] = {
            "possible_failure_reason_codes": _visual_gate_reason_codes(
                identity_key, role.name, kind
            ),
            "measurements": measurements,
            "status": "pass",
        }

    if failures:
        _raise_accumulated_visual_gate_failures(
            identity_key, role.name, failures
        )

    return {
        "durations_ms": actual_durations,
        "frame_sha256": frame_hashes,
        "gate_results": gate_results,
        "identity_frame_sha256": actual_identity_hash,
        "identity_key": identity_key,
        "policy_entry_sha256": entry.entry_sha256,
        "policy_relative_path": policy.relative_path,
        "policy_schema": policy.schema,
        "policy_sha256": policy.source_sha256,
        "role": role.name,
        "schema": SPECIES_ROLE_VISUAL_GATE_EVIDENCE_SCHEMA,
        "status": "pass",
    }


def validate_generated_action_semantic_role(
    species: str,
    identity: HighResFrame,
    role: base.RoleSpec,
    frames: list[HighResFrame],
    semantic: GeneratedRoleSemanticLock,
    *,
    imported_candidates: list[HighResFrame],
    registered_candidates: list[HighResFrame],
    identity_source_sha256: str,
    identity_frame_sha256: str,
    source_layout: str,
    transform_sha256: str | None = None,
    semantic_schema: str = GENERATED_ACTION_SEMANTIC_SCHEMA,
) -> dict[str, object]:
    """Prove bounded composition, truthful reference lineage, and local motion."""

    label = f"{species}/{role.name}"
    if semantic_schema not in {
        LEGACY_GENERATED_ACTION_SEMANTIC_SCHEMA,
        GENERATED_ACTION_SEMANTIC_SCHEMA,
    }:
        raise RasterContractError(
            f"{label}: unsupported generated semantic evidence schema"
        )
    if semantic.role != role.name:
        raise RasterContractError(f"{label}: semantic action role drifted")
    expected_baseline_policy = GENERATED_ROLE_BASELINE_POLICY[role.name]
    if semantic.baseline_policy != expected_baseline_policy:
        raise RasterContractError(
            f"{label}: baseline policy must be {expected_baseline_policy}"
        )
    capabilities = GENERATED_ROLE_CONTACT_POLICY_CAPABILITIES[role.name]
    if semantic.contact_policy not in capabilities:
        raise RasterContractError(
            f"{label}: contact policy {semantic.contact_policy!r} is not an "
            "authorized storyboard capability for this role"
        )
    contact_ceiling = GENERATED_CONTACT_POLICY_MAXIMUMS[semantic.contact_policy]
    contact_floor = 0 if contact_ceiling == 0 else 1
    maximum_contact_changes = semantic.maximum_contact_changed_pixels_per_phase
    if (
        not isinstance(maximum_contact_changes, int)
        or isinstance(maximum_contact_changes, bool)
        or not contact_floor <= maximum_contact_changes <= contact_ceiling
    ):
        raise RasterContractError(
            f"{label}: contact-change bound does not match the explicit policy"
        )
    if source_layout not in GENERATED_SOURCE_LAYOUTS:
        raise RasterContractError(f"{label}: unsupported semantic source layout")
    if len(frames) != REQUIRED_FRAMES_PER_ROLE or [
        frame.phase for frame in frames
    ] != list(range(REQUIRED_FRAMES_PER_ROLE)):
        raise RasterContractError(f"{label}: semantic gate requires phases 0..3")
    for candidate_label, candidates in (
        ("imported candidates", imported_candidates),
        ("registered candidates", registered_candidates),
    ):
        if len(candidates) != REQUIRED_FRAMES_PER_ROLE or [
            frame.phase for frame in candidates
        ] != list(range(REQUIRED_FRAMES_PER_ROLE)):
            raise RasterContractError(
                f"{label}: semantic gate requires four exact {candidate_label}"
            )
    if len(semantic.phases) != REQUIRED_FRAMES_PER_ROLE or [
        phase.phase for phase in semantic.phases
    ] != list(range(REQUIRED_FRAMES_PER_ROLE)):
        raise RasterContractError(
            f"{label}: all four exact phase semantic locks are required"
        )
    if not semantic.motion_landmarks:
        raise RasterContractError(
            f"{label}: at least one role-specific motion landmark is required"
        )

    registration_hash = generated_role_registration_sha256(
        semantic.role_registration
    )
    if registration_hash != semantic.role_registration_sha256:
        raise RasterContractError(f"{label}: role registration hash drifted")
    if semantic.baseline_policy == "identity-anchored":
        if semantic.role_registration != GeneratedRoleRegistrationLock(
            schema=GENERATED_ROLE_REGISTRATION_SCHEMA,
            derivation="identity-anchored-zero-offset",
            output_offset=(0, 0),
            p0_unregistered_floor_y=HIGH_RES_FLOOR_Y,
        ):
            raise RasterContractError(
                f"{label}: identity-anchored role cannot use a role output offset"
            )
    else:
        actual_p0_floor = imported_candidates[0].metrics.bounds[3]
        if (
            semantic.role_registration.schema
            != GENERATED_ROLE_REGISTRATION_SCHEMA
            or semantic.role_registration.derivation
            != "role-p0-fixed-dx-explicit-dy-floor-derived"
            or actual_p0_floor
            != semantic.role_registration.p0_unregistered_floor_y
            or semantic.role_registration.output_offset[1]
            != HIGH_RES_FLOOR_Y - actual_p0_floor
        ):
            raise RasterContractError(
                f"{label}: role-P0 dy is not derived from its unregistered floor"
            )
    for imported, registered in zip(
        imported_candidates, registered_candidates, strict=True
    ):
        expected_registered = register_generated_candidate(
            imported,
            semantic.role_registration,
            label=f"{label}/{imported.phase}/role-registration",
        )
        if expected_registered.packed != registered.packed:
            raise RasterContractError(
                f"{label}/{imported.phase}: per-phase output-offset override "
                "or registered-candidate drift detected"
            )

    identity_mask = set(identity.mask)
    role_pose_mask = set(frames[0].mask)
    role_pose_hash = hashlib.sha256(frames[0].packed).hexdigest()
    if role_pose_hash != semantic.role_pose_baseline_frame_sha256:
        raise RasterContractError(
            f"{label}: immutable role phase-0 raster differs from its lock"
        )
    p0_asset = semantic.phases[0].generated_asset
    identity_baseline_copy = (
        p0_asset.layout == GENERATED_IDENTITY_BASELINE_ASSET_LAYOUT
    )
    if identity_baseline_copy and (
        semantic.role_registration.output_offset != (0, 0)
        or p0_asset.source_sha256 != identity_source_sha256
        or imported_candidates[0].source_sha256 != identity_source_sha256
        or hashlib.sha256(imported_candidates[0].packed).hexdigest()
        != identity_frame_sha256
        or hashlib.sha256(registered_candidates[0].packed).hexdigest()
        != identity_frame_sha256
        or role_pose_hash != identity_frame_sha256
    ):
        raise RasterContractError(
            f"{label}/0: no-call identity baseline is not an exact source, "
            "imported, registered, and final identity copy"
        )
    native_grid_references = [
        phase.native_grid_reference
        for phase in semantic.phases[1:]
        if phase.native_grid_reference is not None
    ]
    if native_grid_references and len(set(native_grid_references)) != 1:
        raise RasterContractError(
            f"{label}: all Image 3 phases must reuse one exact immutable "
            "role-P0 native-grid reference"
        )
    if (
        semantic_schema == LEGACY_GENERATED_ACTION_SEMANTIC_SCHEMA
        and native_grid_references
    ):
        raise RasterContractError(
            f"{label}: semantic-v3 cannot claim Image 3 or three-reference "
            "lineage"
        )
    if native_grid_references and (
        not identity_baseline_copy
        or semantic.role_registration.derivation
        != "identity-anchored-zero-offset"
        or semantic.role_registration.output_offset != (0, 0)
        or p0_asset.source_sha256 != identity_source_sha256
        or p0_asset.imported_candidate_frame_sha256 != identity_frame_sha256
        or p0_asset.registered_candidate_frame_sha256 != identity_frame_sha256
        or role_pose_hash != identity_frame_sha256
    ):
        raise RasterContractError(
            f"{label}: Image 3 v1 requires a zero-registration byte-exact "
            "identity-copy P0"
        )
    if abs(base.median_x(role_pose_mask) - base.median_x(identity_mask)) > 4.0:
        raise RasterContractError(
            f"{label}/0: role-level dx does not preserve identity root alignment"
        )

    lower_scale, upper_scale = HIGH_RES_ROLE_SCALE_ENVELOPES[role.name]
    if not lower_scale <= frames[0].apparent_scale_ratio <= upper_scale:
        raise RasterContractError(
            f"{label}/0: role-pose scale leaves the identity envelope"
        )
    minimum_overlap = ROLE_IDENTITY_JACCARD_MINIMUM[role.name]
    if frames[0].identity_jaccard < minimum_overlap:
        raise RasterContractError(
            f"{label}/0: role-pose identity overlap is below the role floor"
        )
    maximum_role_component_delta = (
        semantic.maximum_role_pose_component_count_delta
    )
    if (
        not isinstance(maximum_role_component_delta, int)
        or isinstance(maximum_role_component_delta, bool)
        or not 0 <= maximum_role_component_delta <= 2
    ):
        raise RasterContractError(
            f"{label}: role-pose component-count bound must be 0..2"
        )
    role_component_delta = abs(
        frames[0].metrics.components - identity.metrics.components
    )
    if role_component_delta > maximum_role_component_delta:
        raise RasterContractError(
            f"{label}/0: role-pose topology changes component count by "
            f"{role_component_delta} (maximum "
            f"{maximum_role_component_delta})"
        )

    if not semantic.role_pose_identity_landmarks:
        raise RasterContractError(
            f"{label}: identity-to-role-pose landmark gates are required"
        )
    pose_landmark_names: set[str] = set()
    pose_landmark_evidence: list[dict[str, object]] = []
    for landmark in semantic.role_pose_identity_landmarks:
        if landmark.name in pose_landmark_names:
            raise RasterContractError(
                f"{label}: role-pose landmark names must be unique"
            )
        pose_landmark_names.add(landmark.name)
        _require_live_region_lock(
            landmark.identity_region,
            f"{label}/pose-landmark/{landmark.name}/identity",
        )
        _require_live_region_lock(
            landmark.role_pose_region,
            f"{label}/pose-landmark/{landmark.name}/role-pose",
        )
        identity_landmark_region = set(landmark.identity_region.mask)
        identity_ink = identity_mask & identity_landmark_region
        role_pose_ink = role_pose_mask & (
            identity_landmark_region
            if identity_baseline_copy
            else set(landmark.role_pose_region.mask)
        )
        if (
            len(identity_ink) < landmark.minimum_ink_pixels
            or len(role_pose_ink) < landmark.minimum_ink_pixels
        ):
            raise RasterContractError(
                f"{label}/{landmark.name}: species marking/anatomy ink is missing "
                "from identity or role pose"
            )
        retention = math.floor(
            min(len(identity_ink), len(role_pose_ink))
            * 1000
            / max(len(identity_ink), len(role_pose_ink))
        )
        if retention < landmark.minimum_ink_retention_per_mille:
            raise RasterContractError(
                f"{label}/{landmark.name}: species marking/anatomy retention is "
                f"{retention} per mille (minimum "
                f"{landmark.minimum_ink_retention_per_mille})"
            )
        identity_components = len(base.connected_components(identity_ink))
        role_pose_components = len(base.connected_components(role_pose_ink))
        local_component_delta = abs(identity_components - role_pose_components)
        if local_component_delta > landmark.maximum_component_count_delta:
            raise RasterContractError(
                f"{label}/{landmark.name}: local marking/anatomy topology drifted"
            )
        pose_landmark_evidence.append(
            {
                "name": landmark.name,
                "identity_region_sha256": landmark.identity_region.packed_sha256,
                "role_pose_region_sha256": landmark.role_pose_region.packed_sha256,
                "comparison_mode": (
                    "byte-exact-identity-baseline-copy"
                    if identity_baseline_copy
                    else "identity-region-to-role-pose-region"
                ),
                "identity_ink_pixels": len(identity_ink),
                "role_pose_ink_pixels": len(role_pose_ink),
                "ink_retention_per_mille": retention,
                "component_count_delta": local_component_delta,
            }
        )

    phase_evidence: list[dict[str, object]] = []
    frame_masks: list[set[tuple[int, int]]] = []
    protected_signatures = [
        tuple(
            sorted(
                (frozen.name, frozen.region.packed_sha256)
                for frozen in phase.frozen_regions
                if frozen.kind == "protected-identity-landmark"
            )
        )
        for phase in semantic.phases
    ]
    if len(set(protected_signatures)) != 1:
        raise RasterContractError(
            f"{label}: protected role-baseline landmark masks drift between phases"
        )
    if semantic.contact_policy in {"planted-identity", "planted-role-base"}:
        contact_signatures = [
            tuple(
                sorted(
                    (frozen.name, frozen.region.packed_sha256)
                    for frozen in phase.frozen_regions
                    if frozen.kind == "planted-contact"
                )
            )
            for phase in semantic.phases
        ]
        if len(set(contact_signatures)) != 1:
            raise RasterContractError(
                f"{label}: planted contact masks drift between stationary phases"
            )
    for frame, imported_candidate, registered_candidate, phase_lock in zip(
        frames,
        imported_candidates,
        registered_candidates,
        semantic.phases,
        strict=True,
    ):
        phase_label = f"{label}/{frame.phase}"
        expected_reference_modes = (
            {P0_GENERATION_REFERENCE_MODE}
            if frame.phase == 0
            else {
                TWO_REFERENCE_GENERATION_MODE,
                THREE_REFERENCE_GENERATION_MODE,
            }
        )
        if phase_lock.generation_reference_mode not in expected_reference_modes:
            raise RasterContractError(
                f"{phase_label}: generation-reference mode is invalid for this "
                "phase"
            )
        native_grid_reference = phase_lock.native_grid_reference
        if frame.phase == 0 and native_grid_reference is not None:
            raise RasterContractError(
                f"{phase_label}: role P0 cannot use Image 3"
            )
        if (
            phase_lock.generation_reference_mode
            == TWO_REFERENCE_GENERATION_MODE
            and native_grid_reference is not None
        ):
            raise RasterContractError(
                f"{phase_label}: two-reference phase cannot claim Image 3"
            )
        if (
            phase_lock.generation_reference_mode
            == THREE_REFERENCE_GENERATION_MODE
            and native_grid_reference is None
        ):
            raise RasterContractError(
                f"{phase_label}: three-reference phase is missing Image 3"
            )
        reference = phase_lock.identity_reference
        if (
            reference.kind != "immutable-approved-identity-source"
            or reference.relative_path != "identity.png"
            or reference.identity_key != species
            or reference.source_sha256 != identity_source_sha256
            or reference.frame_sha256 != identity_frame_sha256
        ):
            raise RasterContractError(
                f"{phase_label}: generated-phase chaining is forbidden; every "
                "phase must independently reference the immutable approved identity"
            )

        expected_semantic_baseline = (
            (
                "approved-identity"
                if semantic.baseline_policy == "identity-anchored"
                else "approved-identity-pose-gate"
            )
            if frame.phase == 0
            else "immutable-role-phase-0"
        )
        if phase_lock.semantic_baseline != expected_semantic_baseline:
            raise RasterContractError(
                f"{phase_label}: semantic baseline drifted; phase 0 alone targets "
                "identity and every later phase independently targets the same "
                "accepted phase 0"
            )
        semantic_baseline = (
            identity_mask
            if expected_semantic_baseline
            in {"approved-identity", "approved-identity-pose-gate"}
            else role_pose_mask
        )
        frozen_baseline = (
            identity_mask
            if semantic.baseline_policy == "identity-anchored"
            else role_pose_mask
        )

        asset = phase_lock.generated_asset
        expected_path = f"{role.name}/{frame.phase:02d}.png"
        expected_source_hash = imported_candidate.source_sha256
        imported_candidate_hash = hashlib.sha256(
            imported_candidate.packed
        ).hexdigest()
        registered_candidate_hash = hashlib.sha256(
            registered_candidate.packed
        ).hexdigest()
        identity_baseline_copy = (
            asset.layout == GENERATED_IDENTITY_BASELINE_ASSET_LAYOUT
        )
        if identity_baseline_copy and (
            frame.phase != 0
            or semantic.role_registration.output_offset != (0, 0)
            or asset.source_sha256 != identity_source_sha256
            or imported_candidate_hash != identity_frame_sha256
            or registered_candidate_hash != identity_frame_sha256
            or hashlib.sha256(frame.packed).hexdigest()
            != identity_frame_sha256
        ):
            raise RasterContractError(
                f"{phase_label}: no-call identity baseline must be one exact "
                "byte-identical, zero-registration role P0 copy"
            )
        if (
            asset.layout
            not in {source_layout, GENERATED_IDENTITY_BASELINE_ASSET_LAYOUT}
            or asset.relative_path != expected_path
            or asset.source_sha256 != expected_source_hash
            or asset.imported_candidate_frame_sha256
            != imported_candidate_hash
            or asset.registered_candidate_frame_sha256
            != registered_candidate_hash
        ):
            raise RasterContractError(
                f"{phase_label}: generated source asset pin drifted or aliases "
                "another phase"
            )

        _require_live_region_lock(
            phase_lock.allowed_change_region,
            f"{phase_label}/allowed-change",
        )
        allowed = set(phase_lock.allowed_change_region.mask)
        if not allowed:
            raise RasterContractError(
                f"{phase_label}: allowed-change region cannot be empty"
            )
        if phase_lock.maximum_out_of_region_changed_pixels != 0:
            raise RasterContractError(
                f"{phase_label}: production out-of-region budget must be zero"
            )

        frame_mask = set(frame.mask)
        composition_baseline_frame = (
            identity
            if frame.phase == 0
            else frames[0]
        )
        composition_baseline = set(composition_baseline_frame.mask)
        composition_baseline_hash = hashlib.sha256(
            composition_baseline_frame.packed
        ).hexdigest()
        if (
            phase_lock.composition_mode != GENERATED_COMPOSITION_MODE
            or phase_lock.composition_baseline_frame_sha256
            != composition_baseline_hash
            or phase_lock.composited_frame_sha256
            != hashlib.sha256(frame.packed).hexdigest()
        ):
            raise RasterContractError(
                f"{phase_label}: bounded-composition provenance hash drifted"
            )
        expected_frame_mask = (
            composition_baseline - allowed
        ) | (set(registered_candidate.mask) & allowed)
        if frame_mask != expected_frame_mask:
            raise RasterContractError(
                f"{phase_label}: final frame is not the exact deterministic "
                "baseline-outside/candidate-inside composition"
            )
        composition_outside = (composition_baseline ^ frame_mask) - allowed
        if composition_outside:
            raise RasterContractError(
                f"{phase_label}: composed output differs outside its exact mask"
            )
        if frame_mask & allowed != set(registered_candidate.mask) & allowed:
            raise RasterContractError(
                f"{phase_label}: candidate motion inside the exact mask was changed"
            )
        discarded_candidate_outside = (
            composition_baseline ^ set(registered_candidate.mask)
        ) - allowed

        target = phase_lock.edit_target_reference
        expected_target_kind = _expected_edit_target_kind(
            semantic.baseline_policy, frame.phase
        )
        if target.kind != expected_target_kind:
            if frame.phase == 0:
                raise RasterContractError(
                    f"{phase_label}: immutable role phase 0 must target the "
                    "approved identity"
                )
            raise RasterContractError(
                f"{phase_label}: phases 1..3 must target the same immutable role "
                "phase 0"
            )
        if expected_target_kind == "immutable-approved-identity-source":
            if (
                target.identity_key != species
                or target.relative_path != "identity.png"
                or target.role != "identity"
                or target.phase != -1
                or target.source_sha256 != identity_source_sha256
                or target.registered_frame_sha256 != identity_frame_sha256
                or target.accepted_composited_frame_sha256 != identity_frame_sha256
            ):
                raise RasterContractError(
                    f"{phase_label}: identity edit target differs from its lock"
                )
        else:
            p0_asset = semantic.phases[0].generated_asset
            if (
                target.identity_key != species
                or target.relative_path != f"{role.name}/00.png"
                or target.role != role.name
                or target.phase != 0
                or target.source_sha256 != p0_asset.source_sha256
                or target.registered_frame_sha256
                != p0_asset.registered_candidate_frame_sha256
                or target.accepted_composited_frame_sha256
                != semantic.role_pose_baseline_frame_sha256
            ):
                raise RasterContractError(
                    f"{phase_label}: P1/P2/P3 do not share the same immutable P0"
                )
        if native_grid_reference is not None and (
            native_grid_reference.schema != NATIVE_GRID_REFERENCE_SCHEMA
            or native_grid_reference.kind != NATIVE_GRID_REFERENCE_KIND
            or native_grid_reference.image_number != 3
            or native_grid_reference.read_only is not True
            or native_grid_reference.edit_target is not False
            or native_grid_reference.source_relative_path
            != f"{role.name}/00.png"
            or native_grid_reference.source_png_sha256
            != semantic.phases[0].generated_asset.source_sha256
            or native_grid_reference.grid_relative_path
            != f"{NATIVE_GRID_REFERENCE_DIRECTORY}/{role.name}/00.png"
            or native_grid_reference.p0_packed_sha256 != role_pose_hash
            or native_grid_reference.roundtrip_packed_sha256 != role_pose_hash
            or native_grid_reference.role_registration_sha256
            != semantic.role_registration_sha256
            or native_grid_reference.transform_sha256 != transform_sha256
            or native_grid_reference.derivation
            != NATIVE_GRID_REFERENCE_DERIVATION
        ):
            raise RasterContractError(
                f"{phase_label}: Image 3 is not the exact read-only projection "
                "of this role's immutable accepted P0"
            )

        preauthorization = phase_lock.preauthorization_reference
        expected_preauthorization_path = (
            f"preauthorization/{role.name}/{frame.phase:02d}.json"
        )
        if (
            preauthorization.kind != "immutable-pre-generation-phase-mask"
            or preauthorization.relative_path != expected_preauthorization_path
            or preauthorization.allowed_change_region_sha256
            != phase_lock.allowed_change_region.packed_sha256
            or preauthorization.edit_target_kind != expected_target_kind
        ):
            raise RasterContractError(
                f"{phase_label}: preauthorization path, target, or frozen mask "
                "drifted"
            )
        _require_lower_sha256(
            preauthorization.source_sha256,
            f"{phase_label}/preauthorization/source_sha256",
        )
        _require_lower_sha256(
            preauthorization.storyboard_sha256,
            f"{phase_label}/preauthorization/storyboard_sha256",
        )

        delta = semantic_baseline ^ frame_mask
        outside = delta - allowed
        if outside:
            raise RasterContractError(
                f"{phase_label}: {len(outside)} changed pixels are outside the "
                "exact allowed-change region (maximum 0); scattered head/tail/paw "
                "edits are rejected"
            )

        frozen_kinds = {region.kind for region in phase_lock.frozen_regions}
        if frozen_kinds != {
            "planted-contact",
            "protected-identity-landmark",
        }:
            raise RasterContractError(
                f"{phase_label}: planted-contact and protected-identity-landmark "
                "frozen masks are both required"
            )
        frozen_names: set[str] = set()
        frozen_contact: set[tuple[int, int]] = set()
        frozen_delta = frozen_baseline ^ frame_mask
        for frozen in phase_lock.frozen_regions:
            if frozen.name in frozen_names:
                raise RasterContractError(
                    f"{phase_label}: frozen region names must be unique"
                )
            frozen_names.add(frozen.name)
            if (
                frozen.kind
                not in {"planted-contact", "protected-identity-landmark"}
                or frozen.maximum_changed_pixels != 0
            ):
                raise RasterContractError(
                    f"{phase_label}/{frozen.name}: floor contacts and hard "
                    "anatomy require a zero-pixel frozen budget"
                )
            _require_live_region_lock(
                frozen.region, f"{phase_label}/frozen/{frozen.name}"
            )
            region_mask = set(frozen.region.mask)
            if frozen.kind == "protected-identity-landmark" and (
                len(region_mask) < GENERATED_MIN_FROZEN_LANDMARK_PIXELS
                or not region_mask & frozen_baseline
            ):
                raise RasterContractError(
                    f"{phase_label}/{frozen.name}: protected role-baseline "
                    "landmark is empty, trivial, or does not cover anatomy"
                )
            if frozen.kind == "planted-contact":
                if not region_mask & frozen_baseline:
                    raise RasterContractError(
                        f"{phase_label}/{frozen.name}: planted-contact region "
                        "does not cover a baseline contact"
                    )
                frozen_contact.update(region_mask)
            changed = frozen_delta & region_mask
            if changed:
                raise RasterContractError(
                    f"{phase_label}/{frozen.name}: {len(changed)} pixels shimmer "
                    "inside a zero-tolerance frozen contact/anatomy region"
                )

        contact_baseline = (
            identity_mask
            if semantic.contact_policy == "planted-identity"
            else role_pose_mask
        )
        contact_floor = {
            point for point in contact_baseline if point[1] == HIGH_RES_FLOOR_Y
        }
        if not contact_floor:
            raise RasterContractError(f"{phase_label}: contact baseline has no floor")
        floor_changes = {
            point
            for point in contact_baseline ^ frame_mask
            if point[1] == HIGH_RES_FLOOR_Y
        }
        if semantic.contact_policy in {"planted-identity", "planted-role-base"}:
            if not contact_floor <= frozen_contact:
                raise RasterContractError(
                    f"{phase_label}: planted-contact mask does not freeze every "
                    "baseline floor contact"
                )
            if floor_changes:
                raise RasterContractError(
                    f"{phase_label}: planted paw/contact shimmer changes "
                    f"{len(floor_changes)} floor pixels"
                )
        else:
            # The explicit allowed-change mask identifies every floor contact
            # that the storyboard permits this phase to alter.  All remaining
            # baseline contacts must be represented in the exact zero-tolerance
            # frozen mask.  Phase 0 establishes the role baseline, so all of its
            # contacts are frozen even when its identity-to-pose delta includes
            # those coordinates.
            expected_frozen_contact = (
                contact_floor
                if frame.phase == 0
                else contact_floor - allowed
            )
            if not expected_frozen_contact <= frozen_contact:
                raise RasterContractError(
                    f"{phase_label}: bounded contact policy leaves "
                    f"{len(expected_frozen_contact - frozen_contact)} unapproved "
                    "baseline floor contacts outside its exact frozen mask"
                )
            if len(floor_changes) > semantic.maximum_contact_changed_pixels_per_phase:
                raise RasterContractError(
                    f"{phase_label}: explicit {semantic.contact_policy} changes "
                    f"{len(floor_changes)} floor pixels (maximum "
                    f"{semantic.maximum_contact_changed_pixels_per_phase})"
                )
            if not floor_changes <= allowed:
                raise RasterContractError(
                    f"{phase_label}: approved contact changes leave their exact "
                    "allowed-change mask"
                )

        frame_masks.append(frame_mask)
        phase_record: dict[str, object] = {
            "phase": frame.phase,
            "semantic_baseline": expected_semantic_baseline,
            "semantic_baseline_frame_sha256": (
                identity_frame_sha256
                if expected_semantic_baseline
                in {"approved-identity", "approved-identity-pose-gate"}
                else semantic.role_pose_baseline_frame_sha256
            ),
            "changed_from_semantic_baseline_pixels": len(delta),
            "outside_allowed_change_pixels": len(outside),
            "allowed_change_mask_sha256": (
                phase_lock.allowed_change_region.packed_sha256
            ),
            "preauthorization_source_sha256": (
                phase_lock.preauthorization_reference.source_sha256
            ),
            "preauthorization_storyboard_sha256": (
                phase_lock.preauthorization_reference.storyboard_sha256
            ),
            "frozen_region_mask_sha256": [
                frozen.region.packed_sha256
                for frozen in phase_lock.frozen_regions
            ],
            "identity_reference_source_sha256": reference.source_sha256,
            "edit_target_kind": target.kind,
            "edit_target_relative_path": target.relative_path,
            "edit_target_source_sha256": target.source_sha256,
            "edit_target_registered_frame_sha256": (
                target.registered_frame_sha256
            ),
            "edit_target_accepted_composited_frame_sha256": (
                target.accepted_composited_frame_sha256
            ),
            "generated_source_sha256": asset.source_sha256,
            "generated_asset_layout": asset.layout,
            "imported_candidate_frame_sha256": imported_candidate_hash,
            "registered_candidate_frame_sha256": registered_candidate_hash,
            "composition_mode": phase_lock.composition_mode,
            "composition_baseline_frame_sha256": composition_baseline_hash,
            "composited_frame_sha256": hashlib.sha256(frame.packed).hexdigest(),
            "discarded_candidate_outside_mask_pixels": len(
                discarded_candidate_outside
            ),
            "candidate_inside_mask_pixels": len(
                set(registered_candidate.mask) & allowed
            ),
            "contact_policy": semantic.contact_policy,
            "floor_contact_changed_pixels": len(floor_changes),
        }
        if semantic_schema == GENERATED_ACTION_SEMANTIC_SCHEMA:
            phase_record["generation_reference_mode"] = (
                phase_lock.generation_reference_mode
            )
            phase_record["native_grid_reference"] = (
                None
                if native_grid_reference is None
                else _native_grid_reference_record(native_grid_reference)
            )
        phase_evidence.append(phase_record)

    motion_union: set[tuple[int, int]] = set()
    landmark_evidence: list[dict[str, object]] = []
    landmark_names: set[str] = set()
    motion_baseline = (
        identity_mask
        if semantic.baseline_policy == "identity-anchored"
        else role_pose_mask
    )
    role_delta = set().union(*(motion_baseline ^ mask for mask in frame_masks))
    allowed_union = set().union(
        *(set(phase.allowed_change_region.mask) for phase in semantic.phases)
    )
    for landmark in semantic.motion_landmarks:
        if landmark.name in landmark_names:
            raise RasterContractError(f"{label}: motion landmark names must be unique")
        landmark_names.add(landmark.name)
        _require_live_region_lock(
            landmark.region, f"{label}/motion/{landmark.name}"
        )
        region = set(landmark.region.mask)
        if not region or not region <= allowed_union:
            raise RasterContractError(
                f"{label}/{landmark.name}: motion landmark must be a non-empty "
                "subset of the role's exact allowed-change regions"
            )
        minimum = landmark.minimum_changed_pixels
        if (
            not isinstance(minimum, int)
            or isinstance(minimum, bool)
            or minimum < GENERATED_MIN_MOTION_LANDMARK_PIXELS
            or minimum > len(region)
        ):
            raise RasterContractError(
                f"{label}/{landmark.name}: invalid meaningful-motion minimum"
            )
        changed = len(role_delta & region)
        if changed < minimum:
            raise RasterContractError(
                f"{label}/{landmark.name}: action changes only {changed} pixels "
                f"inside its role-specific motion landmark (minimum {minimum}); "
                "off-role noise cannot prove animation"
            )
        motion_union.update(region)
        landmark_evidence.append(
            {
                "name": landmark.name,
                "mask_sha256": landmark.region.packed_sha256,
                "changed_pixels": changed,
                "minimum_changed_pixels": minimum,
            }
        )

    motion_states = [frozenset(mask & motion_union) for mask in frame_masks]
    if len(set(motion_states)) != REQUIRED_FRAMES_PER_ROLE:
        raise RasterContractError(
            f"{label}: four unique frames are not four unique role-motion states; "
            "scattered off-role noise is rejected"
        )
    adjacent_motion_changed_pixels: list[int] = []
    for previous, current in zip(motion_states, motion_states[1:]):
        changed = len(previous ^ current)
        adjacent_motion_changed_pixels.append(changed)
        if changed < GENERATED_MIN_MOTION_LANDMARK_PIXELS:
            raise RasterContractError(
                f"{label}: adjacent phases change only {changed} pixels inside "
                "role-specific motion landmarks (minimum "
                f"{GENERATED_MIN_MOTION_LANDMARK_PIXELS}); off-role noise cannot "
                "hide one-pixel landmark shimmer"
            )

    return {
        "schema": semantic_schema,
        "role": role.name,
        "baseline_policy": semantic.baseline_policy,
        "contact_policy": semantic.contact_policy,
        "role_registration": generated_role_registration_record(
            semantic.role_registration
        ),
        "role_registration_sha256": semantic.role_registration_sha256,
        "maximum_contact_changed_pixels_per_phase": (
            semantic.maximum_contact_changed_pixels_per_phase
        ),
        "role_pose_baseline_frame_sha256": (
            semantic.role_pose_baseline_frame_sha256
        ),
        "role_pose_component_count_delta": role_component_delta,
        "role_pose_identity_landmarks": pose_landmark_evidence,
        "source_layout": source_layout,
        "motion_landmarks": landmark_evidence,
        "motion_state_sha256": [
            mask_sha256(state, HIGH_RES_FRAME_WIDTH, HIGH_RES_FRAME_HEIGHT)
            for state in motion_states
        ],
        "adjacent_motion_changed_pixels": adjacent_motion_changed_pixels,
        "phases": phase_evidence,
    }


def _require_generated_semantic_contract_roles(
    species: str,
    lock: ImageGenImportLock,
    roles: tuple[base.RoleSpec, ...],
) -> dict[str, GeneratedRoleSemanticLock]:
    contract = lock.action_semantic_contract
    expected_schema = (
        LEGACY_GENERATED_ACTION_SEMANTIC_SCHEMA
        if lock.schema == LEGACY_IMAGEGEN_IMPORT_LOCK_SCHEMA
        else GENERATED_ACTION_SEMANTIC_SCHEMA
    )
    if contract.schema != expected_schema:
        raise RasterContractError(
            f"{species}: unsafe or missing generated action semantic contract"
        )
    actual_hash = generated_action_semantic_contract_sha256(contract)
    if actual_hash != lock.action_semantic_contract_sha256:
        raise RasterContractError(
            f"{species}: generated action semantic contract hash drifted"
        )
    by_role = {record.role: record for record in contract.roles}
    expected = [role.name for role in roles]
    if len(by_role) != len(contract.roles) or list(by_role) != expected:
        raise RasterContractError(
            f"{species}: semantic action lock set/order differs from selected roles; "
            f"expected={expected} actual={list(by_role)}"
        )
    return by_role


def load_generated_phase_preauthorization(
    species_dir: Path,
    species: str,
    role: str,
    phase: int,
    phase_lock: GeneratedPhaseSemanticLock,
) -> dict[str, str]:
    """Verify and enumerate one frozen pre-generation mask provenance chain.

    The ordinary path authenticates an exact native mask authored before the
    generation call.  One deliberately narrow migration path is also accepted
    for role P0 only: a content-addressed legacy freeze may have declared an
    inclusive native rectangle before generation without serializing its 640
    mask bytes.  That rectangle is materialized mechanically from the
    authenticated freeze record and must equal the semantic lock byte-for-byte.
    """

    reference = phase_lock.preauthorization_reference
    expected_relative = f"preauthorization/{role}/{phase:02d}.json"
    if (
        reference.kind != "immutable-pre-generation-phase-mask"
        or reference.relative_path != expected_relative
        or reference.allowed_change_region_sha256
        != phase_lock.allowed_change_region.packed_sha256
        or reference.edit_target_kind != phase_lock.edit_target_reference.kind
    ):
        raise RasterContractError(
            f"{species}/{role}/{phase}: frozen preauthorization reference drifted"
        )
    path = species_dir / reference.relative_path
    if not path.is_file():
        raise RasterContractError(
            f"{species}/{role}/{phase}: pre-generation mask file is missing"
        )
    actual_source_hash = sha256_file(path)
    if actual_source_hash != reference.source_sha256:
        raise RasterContractError(
            f"{species}/{role}/{phase}: pre-generation mask file SHA-256 drifted"
        )
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RasterContractError(
            f"{species}/{role}/{phase}: cannot read frozen preauthorization: {error}"
        ) from error
    expected = {
        "allowed_change_region",
        "edit_target_kind",
        "frozen_before_generation",
        "identity_key",
        "mask_authoring_basis",
        "phase",
        "role",
        "schema",
        "storyboard_sha256",
    }
    if not isinstance(raw, dict):
        raise RasterContractError(
            f"{species}/{role}/{phase}: exact pre-generation mask record is required; "
            "dynamic/generated-frame-derived masks are forbidden"
        )
    authoring_basis = raw.get("mask_authoring_basis")
    if authoring_basis == GENERATED_PREAUTHORIZATION_NATIVE_MASK_BASIS:
        expected_fields = expected
    elif (
        authoring_basis
        == GENERATED_PREAUTHORIZATION_RECTANGLE_MIGRATION_BASIS
    ):
        expected_fields = expected | {"rectangle_migration"}
    else:
        expected_fields = expected
    if set(raw) != expected_fields:
        raise RasterContractError(
            f"{species}/{role}/{phase}: exact pre-generation mask record is required; "
            "dynamic/generated-frame-derived masks are forbidden"
        )
    if (
        raw["schema"] != GENERATED_PHASE_PREAUTHORIZATION_SCHEMA
        or raw["identity_key"] != species
        or raw["role"] != role
        or raw["phase"] != phase
        or raw["edit_target_kind"] != reference.edit_target_kind
        or raw["frozen_before_generation"] is not True
        or authoring_basis
        not in {
            GENERATED_PREAUTHORIZATION_NATIVE_MASK_BASIS,
            GENERATED_PREAUTHORIZATION_RECTANGLE_MIGRATION_BASIS,
        }
        or raw["storyboard_sha256"] != reference.storyboard_sha256
    ):
        raise RasterContractError(
            f"{species}/{role}/{phase}: mask is not an immutable storyboard "
            "preauthorization"
        )
    _require_lower_sha256(
        raw["storyboard_sha256"],
        f"{species}/{role}/{phase}/storyboard_sha256",
    )
    file_mask = _parse_native_region_mask(
        raw["allowed_change_region"],
        f"{species}/{role}/{phase}/preauthorized-mask",
    )
    if (
        file_mask.packed_sha256 != reference.allowed_change_region_sha256
        or file_mask != phase_lock.allowed_change_region
    ):
        raise RasterContractError(
            f"{species}/{role}/{phase}: dynamic or drifted allowed-change mask"
        )
    source_hashes = {reference.relative_path: actual_source_hash}
    if authoring_basis == GENERATED_PREAUTHORIZATION_RECTANGLE_MIGRATION_BASIS:
        source_hashes.update(
            _load_pre_generation_rectangle_migration(
                species_dir,
                species,
                role,
                phase,
                phase_lock,
                raw["rectangle_migration"],
                file_mask,
            )
        )
    return source_hashes


def _materialize_pre_generation_rectangle(
    rectangle: object, label: str
) -> NativeRegionMaskLock:
    """Materialize exactly one inclusive safe-stage rectangle declaration."""

    rectangle_fields = {
        "canvas",
        "guard_rows_78_79",
        "safe_x_inclusive",
        "safe_y_inclusive",
    }
    if not isinstance(rectangle, dict) or set(rectangle) != rectangle_fields:
        raise RasterContractError(
            f"{label}: exact frozen native rectangle declaration is required"
        )
    x_bounds = rectangle["safe_x_inclusive"]
    y_bounds = rectangle["safe_y_inclusive"]
    if (
        rectangle["canvas"] != [HIGH_RES_FRAME_WIDTH, HIGH_RES_FRAME_HEIGHT]
        or rectangle["guard_rows_78_79"] != "forbidden"
        or not isinstance(x_bounds, list)
        or len(x_bounds) != 2
        or any(not isinstance(value, int) for value in x_bounds)
        or not HIGH_RES_SAFE_LEFT <= x_bounds[0] <= x_bounds[1] <= HIGH_RES_SAFE_RIGHT
        or not isinstance(y_bounds, list)
        or len(y_bounds) != 2
        or any(not isinstance(value, int) for value in y_bounds)
        or not HIGH_RES_SAFE_TOP <= y_bounds[0] <= y_bounds[1] <= HIGH_RES_FLOOR_Y
    ):
        raise RasterContractError(
            f"{label}: frozen rectangle leaves the format-v2 safe stage"
        )
    return native_region_mask_lock(
        {
            (x, y)
            for y in range(y_bounds[0], y_bounds[1] + 1)
            for x in range(x_bounds[0], x_bounds[1] + 1)
        }
    )


def _read_content_addressed_freeze_record(
    species_dir: Path,
    relative_path: object,
    source_sha256: object,
    label: str,
) -> tuple[str, str, dict[str, object]]:
    """Read one exact freeze record from its SHA-named source-snapshot path."""

    source_hash = _require_lower_sha256(source_sha256, f"{label}/sha256")
    relative = _require_canonical_relative_path(
        relative_path, f"{label}/relative_path"
    )
    expected_relative = (
        f"{GENERATED_PREAUTHORIZATION_FREEZE_SOURCE_DIRECTORY}/"
        f"{source_hash}.json"
    )
    if relative != expected_relative:
        raise RasterContractError(
            f"{label}: freeze record must use its content-addressed provenance path"
        )
    path = species_dir / relative
    if not path.is_file():
        raise RasterContractError(f"{label}: hash-pinned freeze record is missing")
    actual_hash = sha256_file(path)
    if actual_hash != source_hash:
        raise RasterContractError(f"{label}: freeze record SHA-256 drifted")
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RasterContractError(
            f"{label}: cannot read hash-pinned freeze record: {error}"
        ) from error
    if not isinstance(raw, dict):
        raise RasterContractError(f"{label}: freeze record must be an object")
    return relative, actual_hash, raw


def _validate_direct_rectangle_freeze(
    freeze: dict[str, object],
    freeze_schema: object,
    identity_source_sha256: str,
    storyboard_sha256: str,
    label: str,
    *,
    role: str | None,
) -> NativeRegionMaskLock:
    """Validate the original direct rectangle freeze; optionally select a role."""

    expected_freeze_fields = {
        "fixed_transform",
        "identity_source_sha256",
        "method_note",
        "roles",
        "schema",
        "status",
        "storyboard_sha256",
    }
    if (
        set(freeze) != expected_freeze_fields
        or freeze.get("schema") != freeze_schema
        or freeze.get("status") != "frozen-before-p0-generation"
        or freeze.get("identity_source_sha256") != identity_source_sha256
        or freeze.get("storyboard_sha256") != storyboard_sha256
        or not isinstance(freeze.get("fixed_transform"), dict)
        or freeze["fixed_transform"].get("output_canvas")
        != [HIGH_RES_FRAME_WIDTH, HIGH_RES_FRAME_HEIGHT]
        or not isinstance(freeze.get("roles"), list)
        or not freeze["roles"]
    ):
        raise RasterContractError(
            f"{label}: legacy base freeze does not authenticate this identity, "
            "storyboard, and native canvas"
        )
    role_fields = {
        "generated_source_count_at_freeze",
        "imagegen_edit_target",
        "phase",
        "preauthorized_role_pose_region",
        "prompt_path",
        "prompt_sha256",
        "role",
    }
    records = freeze["roles"]
    if role is not None:
        records = [
            record
            for record in records
            if isinstance(record, dict)
            and record.get("role") == role
            and record.get("phase") == 0
        ]
        if len(records) != 1:
            raise RasterContractError(
                f"{label}: legacy freeze must contain one exact role-P0 record"
            )
    materialized: list[NativeRegionMaskLock] = []
    seen_roles: set[str] = set()
    for index, frozen_role in enumerate(records):
        record_label = f"{label}/roles/{index}"
        if (
            not isinstance(frozen_role, dict)
            or set(frozen_role) != role_fields
            or not isinstance(frozen_role.get("role"), str)
            or frozen_role["role"] in seen_roles
            or frozen_role["phase"] != 0
            or frozen_role["generated_source_count_at_freeze"] != 0
            or frozen_role["imagegen_edit_target"]
            != "immutable-approved-identity"
        ):
            raise RasterContractError(
                f"{record_label}: source was not frozen before role-P0 identity "
                "editing"
            )
        seen_roles.add(frozen_role["role"])
        _require_canonical_relative_path(
            frozen_role["prompt_path"], f"{record_label}/prompt_path"
        )
        _require_lower_sha256(
            frozen_role["prompt_sha256"], f"{record_label}/prompt_sha256"
        )
        materialized.append(
            _materialize_pre_generation_rectangle(
                frozen_role["preauthorized_role_pose_region"],
                f"{record_label}/rectangle",
            )
        )
    if not materialized or len(set(materialized)) != 1:
        raise RasterContractError(
            f"{label}: base freeze does not declare one invariant native rectangle"
        )
    return materialized[0]


def _load_pre_generation_rectangle_migration(
    species_dir: Path,
    species: str,
    role: str,
    phase: int,
    phase_lock: GeneratedPhaseSemanticLock,
    raw_migration: object,
    file_mask: NativeRegionMaskLock,
) -> dict[str, str]:
    """Authenticate a direct or addendum-chained legacy pre-call rectangle."""

    label = f"{species}/{role}/{phase}/rectangle-migration"
    expected = {
        "freeze_record_relative_path",
        "freeze_record_schema",
        "freeze_record_sha256",
        "freeze_region_field",
        "freeze_role",
        "freeze_phase",
    }
    chained = expected | {
        "base_freeze_record_relative_path",
        "base_freeze_record_schema",
        "base_freeze_record_sha256",
    }
    if (
        not isinstance(raw_migration, dict)
        or frozenset(raw_migration)
        not in {frozenset(expected), frozenset(chained)}
    ):
        raise RasterContractError(
            f"{label}: exact hash-pinned rectangle migration is required"
        )
    if (
        phase != 0
        or phase_lock.semantic_baseline != "approved-identity-pose-gate"
        or phase_lock.edit_target_reference.kind
        != "immutable-approved-identity-source"
    ):
        raise RasterContractError(
            f"{label}: rectangle migration is allowed only for role P0 targeting "
            "the immutable identity"
        )
    is_addendum = set(raw_migration) == chained
    freeze_schema = raw_migration["freeze_record_schema"]
    expected_schema_suffix = (
        "-role-p0-pre-generation-freeze-addendum-v1"
        if is_addendum
        else "-role-p0-pre-generation-freeze-v1"
    )
    expected_region_field = (
        GENERATED_PREAUTHORIZATION_ADDENDUM_FREEZE_REGION_FIELD
        if is_addendum
        else GENERATED_PREAUTHORIZATION_FREEZE_REGION_FIELD
    )
    if (
        not isinstance(freeze_schema, str)
        or not freeze_schema.startswith("kitsu-")
        or not freeze_schema.endswith(expected_schema_suffix)
    ):
        raise RasterContractError(f"{label}: unsupported legacy freeze schema")
    if (
        raw_migration["freeze_region_field"] != expected_region_field
        or raw_migration["freeze_role"] != role
        or raw_migration["freeze_phase"] != 0
    ):
        raise RasterContractError(
            f"{label}: freeze role, phase, or rectangle field drifted"
        )

    freeze_relative, freeze_hash, freeze = (
        _read_content_addressed_freeze_record(
            species_dir,
            raw_migration["freeze_record_relative_path"],
            raw_migration["freeze_record_sha256"],
            f"{label}/freeze-record",
        )
    )
    source_hashes = {freeze_relative: freeze_hash}
    if not is_addendum:
        materialized = _validate_direct_rectangle_freeze(
            freeze,
            freeze_schema,
            phase_lock.identity_reference.source_sha256,
            phase_lock.preauthorization_reference.storyboard_sha256,
            f"{label}/freeze-record",
            role=role,
        )
    else:
        addendum_fields = {
            "base_freeze_sha256",
            "bounded_role_registration_policy_pending_v4",
            "fixed_identity_transform",
            "identity_source_sha256",
            "imagegen_edit_target",
            "later_phase_method",
            "roles",
            "schema",
            "status",
            "storyboard_sha256",
        }
        policy = freeze.get("bounded_role_registration_policy_pending_v4")
        expected_policy = {
            "dx_must_pass_root_axis_and_safe_stage_gates": True,
            "dy_must_be_floor_derived": True,
            "maximum_delta_from_identity_offset_px": 4,
            "one_role_offset_reused_for_all_four_phases": True,
            "phase_specific_override": False,
            "scale_crop_threshold_change": False,
        }
        addendum_role_fields = {
            "prompt_path",
            "prompt_sha256",
            "raw_source_count_for_role_at_freeze",
            "role",
        }
        matches = (
            [
                record
                for record in freeze.get("roles", [])
                if isinstance(record, dict) and record.get("role") == role
            ]
            if isinstance(freeze.get("roles"), list)
            else []
        )
        base_hash = _require_lower_sha256(
            raw_migration["base_freeze_record_sha256"],
            f"{label}/base_freeze_record_sha256",
        )
        if (
            set(freeze) != addendum_fields
            or freeze.get("schema") != freeze_schema
            or freeze.get("status")
            != "frozen-before-listed-p0-generation"
            or freeze.get("identity_source_sha256")
            != phase_lock.identity_reference.source_sha256
            or freeze.get("storyboard_sha256")
            != phase_lock.preauthorization_reference.storyboard_sha256
            or freeze.get("base_freeze_sha256") != base_hash
            or freeze.get("imagegen_edit_target")
            != "immutable-approved-identity"
            or not isinstance(freeze.get("fixed_identity_transform"), dict)
            or freeze["fixed_identity_transform"].get("output_canvas")
            != [HIGH_RES_FRAME_WIDTH, HIGH_RES_FRAME_HEIGHT]
            or policy != expected_policy
            or len(matches) != 1
            or set(matches[0]) != addendum_role_fields
            or matches[0]["raw_source_count_for_role_at_freeze"] != 0
        ):
            raise RasterContractError(
                f"{label}: addendum was not frozen before this role-P0 identity "
                "edit under the bounded registration policy"
            )
        _require_canonical_relative_path(
            matches[0]["prompt_path"], f"{label}/addendum/prompt_path"
        )
        _require_lower_sha256(
            matches[0]["prompt_sha256"], f"{label}/addendum/prompt_sha256"
        )
        base_schema = raw_migration["base_freeze_record_schema"]
        if (
            not isinstance(base_schema, str)
            or not base_schema.startswith("kitsu-")
            or not base_schema.endswith(
                "-role-p0-pre-generation-freeze-v1"
            )
        ):
            raise RasterContractError(
                f"{label}: unsupported base legacy freeze schema"
            )
        base_relative, actual_base_hash, base_freeze = (
            _read_content_addressed_freeze_record(
                species_dir,
                raw_migration["base_freeze_record_relative_path"],
                base_hash,
                f"{label}/base-freeze-record",
            )
        )
        source_hashes[base_relative] = actual_base_hash
        materialized = _validate_direct_rectangle_freeze(
            base_freeze,
            base_schema,
            phase_lock.identity_reference.source_sha256,
            phase_lock.preauthorization_reference.storyboard_sha256,
            f"{label}/base-freeze-record",
            role=None,
        )
    if materialized != file_mask or materialized != phase_lock.allowed_change_region:
        raise RasterContractError(
            f"{label}: materialized pre-call rectangle differs from the exact "
            "allowed-change mask"
        )
    return source_hashes


def _high_res_portrait_bytes(mask: set[tuple[int, int]]) -> bytes:
    packed = bytearray(HIGH_RES_PORTRAIT_BYTES)
    for x, y in mask:
        if not (
            0 <= x < HIGH_RES_PORTRAIT_WIDTH
            and 0 <= y < HIGH_RES_PORTRAIT_HEIGHT
        ):
            raise RasterContractError(
                f"portrait contains out-of-canvas pixel {(x, y)}"
            )
        packed[y * 2 + x // 8] |= 1 << (x & 7)
    return bytes(packed)


def load_high_res_portrait(path: Path, species: str) -> HighResPortrait:
    """Load a separately authored portrait; never derive it by downscaling."""

    label = f"{species}/portrait"
    mask = _exact_one_bit_mask(
        path, (HIGH_RES_PORTRAIT_WIDTH, HIGH_RES_PORTRAIT_HEIGHT), label
    )
    if not 16 <= len(mask) <= 220:
        raise RasterContractError(
            f"{label}: implausible native ink density ({len(mask)} pixels)"
        )
    if any(
        x in (0, HIGH_RES_PORTRAIT_WIDTH - 1)
        or y in (0, HIGH_RES_PORTRAIT_HEIGHT - 1)
        for x, y in mask
    ):
        raise RasterContractError(
            f"{label}: portrait touches its edge; it is clipped"
        )
    components = sorted(base.connected_components(mask), key=len, reverse=True)
    if len(components) > MAX_OUTPUT_COMPONENTS:
        raise RasterContractError(
            f"{label}: portrait has {len(components)} fragmented components"
        )
    if len(components[0]) / len(mask) < HIGH_RES_MIN_PRIMARY_FRACTION:
        raise RasterContractError(
            f"{label}: portrait no longer has one dominant identity subject"
        )
    packed = _high_res_portrait_bytes(mask)
    round_trip = {
        (x, y)
        for y in range(HIGH_RES_PORTRAIT_HEIGHT)
        for x in range(HIGH_RES_PORTRAIT_WIDTH)
        if packed[y * 2 + x // 8] & (1 << (x & 7))
    }
    if round_trip != mask:
        raise RasterContractError(
            f"{label}: 16x18 portrait packing changed pixels"
        )
    return HighResPortrait(
        path=path,
        source_sha256=sha256_file(path),
        mask=frozenset(mask),
        packed=packed,
    )


def load_high_res_species(
    source_dir: Path,
    species: str,
    lock: HighResIdentityLock,
    roles: tuple[base.RoleSpec, ...] = base.ROLE_SPECS,
) -> HighResSpeciesRaster:
    """Validate one complete direct-at-target format-v2 source tree."""

    if species in PROTECTED_STARTERS:
        raise RasterContractError(
            f"{species}: protected legacy starter cannot enter format-v2 art tooling"
        )
    if (
        lock.identity_key != species
        or lock.frame_canvas != (HIGH_RES_FRAME_WIDTH, HIGH_RES_FRAME_HEIGHT)
        or not lock.approved
    ):
        raise RasterContractError(
            f"{species}: exact approved 64x80 identity lock is required"
        )

    species_dir = source_dir / species
    expected_top = {"identity.png", "portrait.png", *(role.name for role in roles)}
    actual_top = {path.name for path in species_dir.iterdir()}
    if actual_top != expected_top:
        raise RasterContractError(
            f"{species}: exact source tree required; "
            f"missing={sorted(expected_top - actual_top)} "
            f"unexpected={sorted(actual_top - expected_top)}"
        )
    if any(not (species_dir / role.name).is_dir() for role in roles):
        raise RasterContractError(f"{species}: every animation must be a directory")

    identity_path = species_dir / "identity.png"
    if sha256_file(identity_path) != lock.identity_sha256:
        raise RasterContractError(
            f"{species}: identity.png SHA-256 differs from approved v2 lock"
        )
    identity = load_high_res_frame(identity_path, "identity", -1)
    portrait = load_high_res_portrait(species_dir / "portrait.png", species)
    identity_mask = set(identity.mask)
    source_hashes = {
        "identity.png": identity.source_sha256,
        "portrait.png": portrait.source_sha256,
    }
    frames: list[HighResFrame] = []
    for role in roles:
        role_dir = species_dir / role.name
        actual_files = {path.name for path in role_dir.iterdir()}
        expected_files = set(HIGH_RES_FRAME_FILES)
        if actual_files != expected_files:
            raise RasterContractError(
                f"{species}/{role.name}: exact independent frame set required; "
                f"missing={sorted(expected_files - actual_files)} "
                f"unexpected={sorted(actual_files - expected_files)}"
            )
        role_frames: list[HighResFrame] = []
        for phase, filename in enumerate(HIGH_RES_FRAME_FILES):
            path = role_dir / filename
            if not path.is_file():
                raise RasterContractError(
                    f"{species}/{role.name}/{filename}: expected a regular PNG file"
                )
            frame = load_high_res_frame(
                path,
                role.name,
                phase,
                identity_mask=identity_mask,
            )
            source_hashes[f"{role.name}/{filename}"] = frame.source_sha256
            role_frames.append(frame)
        validate_high_res_four_frame_role(role, role_frames)
        frames.extend(role_frames)

    return HighResSpeciesRaster(
        identity=identity,
        portrait=portrait,
        frames=tuple(frames),
        source_sha256=source_hashes,
    )


def load_high_res_generated_species(
    source_dir: Path,
    species: str,
    lock: ImageGenImportLock,
    roles: tuple[base.RoleSpec, ...] = base.ROLE_SPECS,
) -> HighResSpeciesRaster:
    """Validate v4/v5 edits through deterministic bounded composition."""

    if species in PROTECTED_STARTERS:
        raise RasterContractError(
            f"{species}: protected legacy starter cannot enter ImageGen import"
        )
    if lock.identity_key != species or not lock.approved:
        raise RasterContractError(
            f"{species}: approved ImageGen import lock is required"
        )
    validate_imagegen_import_transform(lock.transform, f"{species}/transform")
    if imagegen_import_transform_sha256(lock.transform) != lock.transform_sha256:
        raise RasterContractError(
            f"{species}: in-memory ImageGen transform differs from locked hash"
        )
    semantic_by_role = _require_generated_semantic_contract_roles(
        species, lock, roles
    )
    native_grid_by_role: dict[str, NativeGridConditioningReference] = {}
    for role in roles:
        references = [
            phase.native_grid_reference
            for phase in semantic_by_role[role.name].phases[1:]
            if phase.native_grid_reference is not None
        ]
        if references:
            if len(set(references)) != 1:
                raise RasterContractError(
                    f"{species}/{role.name}: Image 3 phases do not share one "
                    "immutable native-grid reference"
                )
            native_grid_by_role[role.name] = references[0]

    species_dir = source_dir / species
    expected_top = {
        "identity.png",
        "portrait.png",
        "preauthorization",
        *(role.name for role in roles),
    }
    if native_grid_by_role:
        expected_top.add(NATIVE_GRID_REFERENCE_DIRECTORY)
    actual_top = {path.name for path in species_dir.iterdir()}
    if actual_top != expected_top:
        raise RasterContractError(
            f"{species}: exact generated source tree required; "
            f"missing={sorted(expected_top - actual_top)} "
            f"unexpected={sorted(actual_top - expected_top)}"
        )
    if any(not (species_dir / role.name).is_dir() for role in roles):
        raise RasterContractError(f"{species}: every animation must be a directory")
    if native_grid_by_role:
        native_grid_dir = species_dir / NATIVE_GRID_REFERENCE_DIRECTORY
        if not native_grid_dir.is_dir():
            raise RasterContractError(
                f"{species}: referenced native-grid Image 3 tree is missing"
            )
        actual_native_grid_roles = {
            path.name for path in native_grid_dir.iterdir()
        }
        if actual_native_grid_roles != set(native_grid_by_role) or any(
            not (native_grid_dir / role / "00.png").is_file()
            or {path.name for path in (native_grid_dir / role).iterdir()}
            != {"00.png"}
            for role in native_grid_by_role
        ):
            raise RasterContractError(
                f"{species}: native-grid Image 3 files differ from exact phase "
                "references; missing or orphan references are forbidden"
            )
    preauthorization_dir = species_dir / "preauthorization"
    if not preauthorization_dir.is_dir():
        raise RasterContractError(
            f"{species}: exact pre-generation mask tree is required"
        )
    actual_preauthorization_entries = {
        path.name for path in preauthorization_dir.iterdir()
    }
    expected_preauthorization_roles = {role.name for role in roles}
    permitted_preauthorization_entries = expected_preauthorization_roles | {
        "_frozen-source"
    }
    if (
        not expected_preauthorization_roles
        <= actual_preauthorization_entries
        or not actual_preauthorization_entries
        <= permitted_preauthorization_entries
        or any(
        not (preauthorization_dir / role.name).is_dir() for role in roles
        )
        or (
            "_frozen-source" in actual_preauthorization_entries
            and not (preauthorization_dir / "_frozen-source").is_dir()
        )
    ):
        raise RasterContractError(
            f"{species}: preauthorization role tree differs from selected roles"
        )
    for role in roles:
        actual_files = {
            path.name for path in (preauthorization_dir / role.name).iterdir()
        }
        expected_files = {f"{phase:02d}.json" for phase in range(4)}
        if actual_files != expected_files:
            raise RasterContractError(
                f"{species}/{role.name}: exact four pre-generation masks are "
                "required"
            )

    identity_path = species_dir / "identity.png"
    if sha256_file(identity_path) != lock.identity_source_sha256:
        raise RasterContractError(
            f"{species}: generated identity source SHA-256 differs from lock"
        )
    identity = load_imagegen_import_frame(
        identity_path,
        "identity",
        -1,
        lock.transform,
    )
    actual_identity_frame_hash = hashlib.sha256(identity.packed).hexdigest()
    if actual_identity_frame_hash != lock.identity_frame_sha256:
        raise RasterContractError(
            f"{species}: imported identity 64x80 SHA-256 differs from lock"
        )

    portrait = load_high_res_portrait(species_dir / "portrait.png", species)
    identity_mask = set(identity.mask)
    source_hashes = {
        "identity.png": identity.source_sha256,
        "portrait.png": portrait.source_sha256,
    }
    frames: list[HighResFrame] = []
    semantic_evidence: dict[str, dict[str, object]] = {}
    for role in roles:
        role_dir = species_dir / role.name
        actual_files = {path.name for path in role_dir.iterdir()}
        expected_files = set(HIGH_RES_FRAME_FILES)
        if actual_files != expected_files:
            raise RasterContractError(
                f"{species}/{role.name}: exact independent generated frame set "
                f"required; missing={sorted(expected_files - actual_files)} "
                f"unexpected={sorted(actual_files - expected_files)}"
            )
        semantic_role = semantic_by_role[role.name]
        imported_candidates: list[HighResFrame] = []
        for phase, filename in enumerate(HIGH_RES_FRAME_FILES):
            path = role_dir / filename
            if not path.is_file():
                raise RasterContractError(
                    f"{species}/{role.name}/{filename}: expected a regular PNG file"
                )
            phase_lock = semantic_role.phases[phase]
            # The frozen mask is authenticated before candidate pixels are
            # interpreted.  Only pixels capable of entering that mask are
            # relevant to candidate ambiguity/stability gates.
            preauthorization_hashes = load_generated_phase_preauthorization(
                species_dir,
                species,
                role.name,
                phase,
                phase_lock,
            )
            for relative_path, source_hash in preauthorization_hashes.items():
                existing_hash = source_hashes.get(relative_path)
                if existing_hash is not None and existing_hash != source_hash:
                    raise RasterContractError(
                        f"{species}/{role.name}/{phase}: shared preauthorization "
                        "provenance hash drifted"
                    )
                source_hashes[relative_path] = source_hash
            registration_dx, registration_dy = (
                semantic_role.role_registration.output_offset
            )
            candidate_relevance_region = {
                (x - registration_dx, y - registration_dy)
                for x, y in phase_lock.allowed_change_region.mask
                if 0 <= x - registration_dx < HIGH_RES_FRAME_WIDTH
                and 0 <= y - registration_dy < HIGH_RES_FRAME_HEIGHT
            }
            candidate = load_imagegen_import_frame(
                path,
                role.name,
                phase,
                lock.transform,
                require_floor=False,
                bounded_validation_region=candidate_relevance_region,
            )
            source_hashes[f"{role.name}/{filename}"] = candidate.source_sha256
            imported_candidates.append(candidate)
        registered_candidates = [
            register_generated_candidate(
                candidate,
                semantic_role.role_registration,
                label=f"{species}/{role.name}/{candidate.phase}/registered-candidate",
            )
            for candidate in imported_candidates
        ]
        role_frames: list[HighResFrame] = []
        for phase, registered_candidate in enumerate(registered_candidates):
            baseline = (
                identity
                if phase == 0
                else role_frames[0]
            )
            role_frames.append(
                compose_bounded_generated_frame(
                    registered_candidate,
                    baseline,
                    semantic_role.phases[phase].allowed_change_region,
                    identity_mask,
                    label=f"{species}/{role.name}/{phase}/bounded-composite",
                )
            )
        validate_high_res_four_frame_role(role, role_frames)
        role_evidence = validate_generated_action_semantic_role(
            species,
            identity,
            role,
            role_frames,
            semantic_role,
            imported_candidates=imported_candidates,
            registered_candidates=registered_candidates,
            identity_source_sha256=lock.identity_source_sha256,
            identity_frame_sha256=lock.identity_frame_sha256,
            source_layout="independent-frame",
            transform_sha256=lock.transform_sha256,
            semantic_schema=lock.action_semantic_contract.schema,
        )
        native_grid_reference = native_grid_by_role.get(role.name)
        if native_grid_reference is not None:
            reference_evidence = validate_native_grid_conditioning_reference(
                species_dir,
                species,
                role.name,
                native_grid_reference,
                lock.transform,
                role_frames[0],
            )
            source_hashes[native_grid_reference.grid_relative_path] = (
                native_grid_reference.grid_png_sha256
            )
            role_evidence["native_grid_reference_proof"] = reference_evidence
        semantic_evidence[role.name] = role_evidence
        frames.extend(role_frames)

    freeze_source_dir = preauthorization_dir / "_frozen-source"
    actual_freeze_sources = (
        {
            f"{GENERATED_PREAUTHORIZATION_FREEZE_SOURCE_DIRECTORY}/{path.name}"
            for path in freeze_source_dir.iterdir()
            if path.is_file()
        }
        if freeze_source_dir.is_dir()
        else set()
    )
    if freeze_source_dir.is_dir() and any(
        not path.is_file() for path in freeze_source_dir.iterdir()
    ):
        raise RasterContractError(
            f"{species}: frozen preauthorization provenance must be regular files"
        )
    referenced_freeze_sources = {
        relative_path
        for relative_path in source_hashes
        if relative_path.startswith(
            f"{GENERATED_PREAUTHORIZATION_FREEZE_SOURCE_DIRECTORY}/"
        )
    }
    if (
        actual_freeze_sources != referenced_freeze_sources
        or freeze_source_dir.is_dir() != bool(referenced_freeze_sources)
    ):
        raise RasterContractError(
            f"{species}: frozen preauthorization provenance set differs from "
            "the phase references"
        )

    crop_width = lock.transform.crop_rect[2] - lock.transform.crop_rect[0]
    fixed_scale = HIGH_RES_FRAME_WIDTH / crop_width
    return HighResSpeciesRaster(
        identity=identity,
        portrait=portrait,
        frames=tuple(frames),
        source_sha256=source_hashes,
        fixed_action_scale=fixed_scale,
        generated_semantic_evidence=semantic_evidence,
        generated_action_semantic_contract_sha256=(
            lock.action_semantic_contract_sha256
        ),
    )


def load_high_res_generated_action_sheet_species(
    source_dir: Path,
    species: str,
    lock: ImageGenImportLock,
    roles: tuple[base.RoleSpec, ...] = base.ROLE_SPECS,
) -> HighResSpeciesRaster:
    """Reject legacy sheets: v4/v5 requires one pinned raw per phase."""

    raise RasterContractError(
        f"{species}: ImageGen import v4/v5 forbids one-action sheets because they "
        "cannot prove independent star edits from one immutable role P0"
    )
