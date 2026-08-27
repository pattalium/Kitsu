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
IMAGEGEN_IMPORT_LOCK_SCHEMA = "kitsu-wild-imagegen-import-lock-v4"
GENERATED_ACTION_SEMANTIC_SCHEMA = (
    "kitsu-wild-generated-action-semantic-locality-v2"
)
GENERATED_PHASE_PREAUTHORIZATION_SCHEMA = (
    "kitsu-wild-generated-phase-preauthorization-v1"
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
NATIVE_REGION_MASK_SCHEMA = "kitsu-native-region-mask-64x80-v1"
NATIVE_REGION_MASK_ENCODING = "lsb0-row-major-hex"
PROTECTED_STARTERS = frozenset({"cat", "dog", "fox"})
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
) -> dict[str, object]:
    return {
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
                "phases": [_phase_semantic_record(phase) for phase in role.phases],
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


def _expected_edit_target_kind(baseline_policy: str, phase: int) -> str:
    if baseline_policy == "identity-anchored" or phase == 0:
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
        if baseline_policy == "identity-anchored":
            raise RasterContractError(
                f"{label}: identity-anchored role cannot use a role-P0 edit target"
            )
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
    if not isinstance(raw, dict) or set(raw) != expected or raw.get("phase") != phase:
        raise RasterContractError(f"{label}: exact phase semantic lock is required")
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
        "approved-identity"
        if baseline_policy == "identity-anchored"
        else (
            "approved-identity-pose-gate"
            if phase == 0
            else "immutable-role-phase-0"
        )
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
        and not (baseline_policy == "immutable-role-phase-0" and phase == 0)
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
) -> GeneratedActionSemanticContract:
    expected = {"roles", "schema"}
    if not isinstance(raw, dict) or set(raw) != expected:
        raise RasterContractError(
            f"{identity_key}: generated action semantic contract is missing or malformed"
        )
    if raw["schema"] != GENERATED_ACTION_SEMANTIC_SCHEMA:
        raise RasterContractError(
            f"{identity_key}: generated action semantic schema must be "
            f"{GENERATED_ACTION_SEMANTIC_SCHEMA}"
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
            )
            for phase, phase_raw in enumerate(phases_raw)
        )
        if baseline_frame_hash != phases[0].composited_frame_sha256:
            raise RasterContractError(
                f"{identity_key}/{role}: role-pose baseline must be the exact "
                "bounded-composite phase-0 frame"
            )
        p0_asset = phases[0].generated_asset
        for phase_lock in phases:
            expected_composition_baseline = (
                identity_frame_sha256
                if baseline_policy == "identity-anchored" or phase_lock.phase == 0
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
            if (
                baseline_policy == "immutable-role-phase-0"
                and phase_lock.phase > 0
            ):
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
        schema=GENERATED_ACTION_SEMANTIC_SCHEMA,
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
    so legacy values cannot be silently dropped or reinterpreted. ImageGen v4
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
    if payload["schema"] != IMAGEGEN_IMPORT_LOCK_SCHEMA:
        raise RasterContractError(
            f"ImageGen import lock schema must be {IMAGEGEN_IMPORT_LOCK_SCHEMA}"
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

    ``bounded_validation_region`` is only used for a v4 generated candidate.
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

    A v4 candidate is provenance, not a release frame.  Its complete binary
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
) -> dict[str, object]:
    """Prove bounded composition, two-reference lineage, and local motion."""

    label = f"{species}/{role.name}"
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
        identity_ink = identity_mask & set(landmark.identity_region.mask)
        role_pose_ink = role_pose_mask & set(landmark.role_pose_region.mask)
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
            "approved-identity"
            if semantic.baseline_policy == "identity-anchored"
            else (
                "approved-identity-pose-gate"
                if frame.phase == 0
                else "immutable-role-phase-0"
            )
        )
        if phase_lock.semantic_baseline != expected_semantic_baseline:
            raise RasterContractError(
                f"{phase_label}: semantic baseline drifted; phase 0 is never a "
                "generation reference"
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
            semantic.baseline_policy != "immutable-role-phase-0"
            or frame.phase != 0
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
            if semantic.baseline_policy == "identity-anchored" or frame.phase == 0
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
            if semantic.baseline_policy == "identity-anchored":
                raise RasterContractError(
                    f"{phase_label}: identity-anchored role cannot use a role-P0 "
                    "edit target"
                )
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
        phase_evidence.append(
            {
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
                "composited_frame_sha256": hashlib.sha256(
                    frame.packed
                ).hexdigest(),
                "discarded_candidate_outside_mask_pixels": len(
                    discarded_candidate_outside
                ),
                "candidate_inside_mask_pixels": len(
                    set(registered_candidate.mask) & allowed
                ),
                "contact_policy": semantic.contact_policy,
                "floor_contact_changed_pixels": len(floor_changes),
            }
        )

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
        "schema": GENERATED_ACTION_SEMANTIC_SCHEMA,
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
    if contract.schema != GENERATED_ACTION_SEMANTIC_SCHEMA:
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
    """Validate v4 independent edits through deterministic bounded composition."""

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

    species_dir = source_dir / species
    expected_top = {
        "identity.png",
        "portrait.png",
        "preauthorization",
        *(role.name for role in roles),
    }
    actual_top = {path.name for path in species_dir.iterdir()}
    if actual_top != expected_top:
        raise RasterContractError(
            f"{species}: exact generated source tree required; "
            f"missing={sorted(expected_top - actual_top)} "
            f"unexpected={sorted(actual_top - expected_top)}"
        )
    if any(not (species_dir / role.name).is_dir() for role in roles):
        raise RasterContractError(f"{species}: every animation must be a directory")
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
                if semantic_role.baseline_policy == "identity-anchored" or phase == 0
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
        semantic_evidence[role.name] = validate_generated_action_semantic_role(
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
        )
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
    """Reject legacy sheets: v4 requires one independently pinned raw per phase."""

    raise RasterContractError(
        f"{species}: ImageGen import v4 forbids one-action sheets because they "
        "cannot prove independent star edits from one immutable role P0"
    )
