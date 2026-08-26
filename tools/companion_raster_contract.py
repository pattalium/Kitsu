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
