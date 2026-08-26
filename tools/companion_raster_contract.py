#!/usr/bin/env python3
"""Fail-closed raster contract for private companion artwork.

Image generation output is not release artwork.  A release build is allowed
only when every action frame can be projected at one scale derived from an
explicitly approved identity master.  This module never cleans, repairs,
shrinks, crops, or otherwise guesses at damaged artwork: clipped subjects,
debris, missing frames, scale drift, and identity drift are build errors.

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
IMAGEGEN_IMPORT_LOCK_SCHEMA = "kitsu-wild-imagegen-import-lock-v2"
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
    approved: bool


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
    """Build the Ferret-E identity transform with an explicit action lock.

    There is intentionally no default action offset.  It can be approved only
    after complete action-sheet validation and must never be inherited from
    the differently scaled identity viewport.
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
        if not isinstance(raw, dict) or set(raw) != {
            "approved",
            "identity_frame_sha256",
            "identity_key",
            "identity_source_sha256",
            "transform",
            "transform_sha256",
        }:
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


def _import_imagegen_mask(
    path: Path, transform: ImageGenImportTransform, label: str
) -> set[tuple[int, int]]:
    """Apply the one pinned identity transform, without per-frame fitting."""

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
    histogram = crop.histogram()
    nonwhite = sum(histogram[:250])
    ambiguous = sum(histogram[17:239])
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
        4, math.ceil(len(unshifted_mask) * IMAGEGEN_MAX_THRESHOLD_SENSITIVE_FRACTION)
    )
    if sensitive_pixels > maximum_sensitive:
        raise RasterContractError(
            f"{label}: {sensitive_pixels} output pixels are coverage-threshold "
            f"sensitive (maximum {maximum_sensitive}); regenerate cleaner art"
        )

    validate_high_res_mask(mask, label)
    return mask


def load_imagegen_import_frame(
    path: Path,
    role: str,
    phase: int,
    transform: ImageGenImportTransform,
    *,
    identity_mask: set[tuple[int, int]] | None = None,
) -> HighResFrame:
    owner = path.parent.parent.name if path.parent.name == role else path.parent.name
    label = f"{owner}/{role}/{phase}"
    mask = _import_imagegen_mask(path, transform, label)
    metrics = validate_high_res_mask(mask, label)
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
    """Validate a complete ImageGen tree using one identity-pinned transform."""

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

    species_dir = source_dir / species
    expected_top = {"identity.png", "portrait.png", *(role.name for role in roles)}
    actual_top = {path.name for path in species_dir.iterdir()}
    if actual_top != expected_top:
        raise RasterContractError(
            f"{species}: exact generated source tree required; "
            f"missing={sorted(expected_top - actual_top)} "
            f"unexpected={sorted(actual_top - expected_top)}"
        )
    if any(not (species_dir / role.name).is_dir() for role in roles):
        raise RasterContractError(f"{species}: every animation must be a directory")

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
        role_frames: list[HighResFrame] = []
        for phase, filename in enumerate(HIGH_RES_FRAME_FILES):
            path = role_dir / filename
            if not path.is_file():
                raise RasterContractError(
                    f"{species}/{role.name}/{filename}: expected a regular PNG file"
                )
            frame = load_imagegen_import_frame(
                path,
                role.name,
                phase,
                lock.transform,
                identity_mask=identity_mask,
            )
            source_hashes[f"{role.name}/{filename}"] = frame.source_sha256
            role_frames.append(frame)
        validate_high_res_four_frame_role(role, role_frames)
        frames.extend(role_frames)

    crop_width = lock.transform.crop_rect[2] - lock.transform.crop_rect[0]
    fixed_scale = HIGH_RES_FRAME_WIDTH / crop_width
    return HighResSpeciesRaster(
        identity=identity,
        portrait=portrait,
        frames=tuple(frames),
        source_sha256=source_hashes,
        fixed_action_scale=fixed_scale,
    )


def load_high_res_generated_action_sheet_species(
    source_dir: Path,
    species: str,
    lock: ImageGenImportLock,
    roles: tuple[base.RoleSpec, ...] = base.ROLE_SPECS,
) -> HighResSpeciesRaster:
    """Validate one generated source file per action, with four fixed cells."""

    if species in PROTECTED_STARTERS:
        raise RasterContractError(
            f"{species}: protected legacy starter cannot enter action-sheet import"
        )
    if lock.identity_key != species or not lock.approved:
        raise RasterContractError(
            f"{species}: approved ImageGen identity lock is required"
        )
    validate_imagegen_import_transform(lock.transform, f"{species}/transform")
    if lock.transform.source_canvas != IMAGEGEN_ACTION_SHEET_SOURCE_CANVAS:
        raise RasterContractError(
            f"{species}: action-sheet path requires identity/source family "
            f"{IMAGEGEN_ACTION_SHEET_SOURCE_CANVAS}"
        )
    if imagegen_import_transform_sha256(lock.transform) != lock.transform_sha256:
        raise RasterContractError(
            f"{species}: in-memory ImageGen transform differs from locked hash"
        )

    species_dir = source_dir / species
    expected_top = {
        "identity.png",
        "portrait.png",
        *(f"{role.name}.png" for role in roles),
    }
    actual_top = {path.name for path in species_dir.iterdir()}
    if actual_top != expected_top:
        raise RasterContractError(
            f"{species}: exact one-action-per-file tree required; "
            f"missing={sorted(expected_top - actual_top)} "
            f"unexpected={sorted(actual_top - expected_top)}"
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
    if hashlib.sha256(identity.packed).hexdigest() != lock.identity_frame_sha256:
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
    for role in roles:
        path = species_dir / f"{role.name}.png"
        if not path.is_file():
            raise RasterContractError(
                f"{species}/{role.name}: exact action sheet is missing"
            )
        role_frames = load_imagegen_action_sheet_frames(
            path,
            species,
            role,
            lock.transform,
            identity_mask,
        )
        # Nothing is written or returned until every phase of every selected
        # role passes, so a late failure cannot leave a partial extraction.
        source_hashes[path.name] = sha256_file(path)
        frames.extend(role_frames)

    return HighResSpeciesRaster(
        identity=identity,
        portrait=portrait,
        frames=tuple(frames),
        source_sha256=source_hashes,
        fixed_action_scale=HIGH_RES_FRAME_WIDTH / 560,
    )
