#!/usr/bin/env python3
"""Build the public Cat, Fox, and Dog K868 companion packs.

The source artwork is nine equal-grid monochrome sheets under
``assets/companion-sources``.  This builder is deterministic: it cleans
isolated source artifacts, applies one scale per four-frame role, fixes the perceived
body axis at x=32 and the floor at y=61, serializes the K868PK1 format, and
emits machine-readable geometry and animation evidence.

Only the three public default species are accepted.  The release allow-list is
deliberately closed rather than inferred from directory contents.
"""

from __future__ import annotations

import argparse
import binascii
import hashlib
import json
import math
import struct
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from PIL import Image


FRAME_WIDTH = 64
FRAME_HEIGHT = 64
FRAME_BYTES = FRAME_WIDTH * FRAME_HEIGHT // 8
BODY_AXIS_X = 32
FLOOR_Y = 61
SAFE_LEFT = 2
SAFE_RIGHT = 61
SAFE_TOP = 2
PACK_MAGIC = b"K868PK1\0"
PACK_VERSION = 1
PACK_HEADER_BYTES = 64
PACK_CLIP_BYTES = 12
PACK_STEP_BYTES = 4
PACK_PARTITION_BYTES = 0x140000
PACK_REVISION = 2

MODE_HOLD = 0
MODE_ONCE = 1
MODE_LOOP = 2
MODE_PINGPONG = 3


@dataclass(frozen=True)
class RoleSpec:
    role: int
    name: str
    sheet: str
    row: int
    mode: int
    durations_ms: tuple[int, int, int, int]


ROLE_SPECS = (
    RoleSpec(0, "idle", "core", 0, MODE_LOOP, (700, 550, 700, 550)),
    RoleSpec(1, "blink", "core", 1, MODE_ONCE, (600, 140, 180, 300)),
    RoleSpec(2, "pet", "core", 2, MODE_ONCE, (280, 320, 450, 500)),
    RoleSpec(3, "sleep", "life", 1, MODE_ONCE, (350, 450, 800, 1200)),
    RoleSpec(4, "listen", "life", 3, MODE_LOOP, (500, 450, 500, 650)),
    RoleSpec(5, "surprise", "social", 0, MODE_ONCE, (250, 220, 350, 450)),
    RoleSpec(6, "play", "core", 3, MODE_PINGPONG, (260, 240, 260, 320)),
    RoleSpec(7, "tired", "social", 3, MODE_ONCE, (650, 700, 900, 1000)),
    RoleSpec(8, "feed", "life", 0, MODE_ONCE, (300, 250, 350, 500)),
    RoleSpec(9, "wake", "life", 2, MODE_ONCE, (600, 400, 350, 450)),
    RoleSpec(10, "meet", "social", 1, MODE_ONCE, (300, 350, 420, 500)),
    RoleSpec(11, "evolve", "social", 2, MODE_ONCE, (350, 450, 600, 700)),
)

PUBLIC_SPECIES = {
    "cat": "CAT",
    "fox": "FOX",
    "dog": "DOG",
}


@dataclass
class SourceFrame:
    species: str
    role: RoleSpec
    phase: int
    mask: set[tuple[int, int]]
    cell_width: int
    cell_height: int


class CanvasFitError(ValueError):
    """A raster needs a smaller scale after integer pixel quantization."""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def connected_components(mask: set[tuple[int, int]]) -> list[set[tuple[int, int]]]:
    unseen = set(mask)
    components: list[set[tuple[int, int]]] = []
    while unseen:
        origin = unseen.pop()
        component = {origin}
        queue = deque([origin])
        while queue:
            x, y = queue.popleft()
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if not dx and not dy:
                        continue
                    point = (x + dx, y + dy)
                    if point in unseen:
                        unseen.remove(point)
                        component.add(point)
                        queue.append(point)
        components.append(component)
    return components


def bounds(mask: Iterable[tuple[int, int]]) -> tuple[int, int, int, int]:
    points = list(mask)
    if not points:
        raise ValueError("empty source frame")
    xs = [point[0] for point in points]
    ys = [point[1] for point in points]
    return min(xs), min(ys), max(xs), max(ys)


def clean_mask(mask: set[tuple[int, int]]) -> set[tuple[int, int]]:
    """Remove isolated generation marks without touching interior details."""

    components = sorted(connected_components(mask), key=len, reverse=True)
    if not components:
        raise ValueError("source cell contains no dark pixels")
    primary = components[0]
    left, top, right, bottom = bounds(primary)
    primary_area = len(primary)
    retained = set(primary)

    for component in components[1:]:
        c_left, c_top, c_right, c_bottom = bounds(component)
        center_x = (c_left + c_right) / 2
        center_y = (c_top + c_bottom) / 2
        inside_primary_box = (
            left <= center_x <= right and top <= center_y <= bottom
        )
        substantial = len(component) >= max(12, math.ceil(primary_area * 0.035))
        near_primary = not (
            c_right < left - 4
            or c_left > right + 4
            or c_bottom < top - 4
            or c_top > bottom + 4
        )
        if inside_primary_box or (substantial and near_primary):
            retained.update(component)

    if len(retained) < 80:
        raise ValueError("source cleanup left too little companion artwork")
    return retained


def load_source_frames(source_dir: Path, species: str) -> tuple[list[SourceFrame], dict[str, str]]:
    source_hashes: dict[str, str] = {}
    sheet_masks: dict[str, list[list[set[tuple[int, int]]]]] = {}
    cell_sizes: dict[str, tuple[int, int]] = {}

    for sheet_name in ("core", "life", "social"):
        path = source_dir / f"{species}-{sheet_name}.png"
        if not path.is_file():
            raise FileNotFoundError(f"missing public source sheet: {path}")
        source_hashes[path.name] = sha256_file(path)
        with Image.open(path) as image:
            gray = image.convert("L")
            width, height = gray.size
            if width < 512 or height < 512:
                raise ValueError(f"{path}: source sheet is too small: {width}x{height}")
            rows: list[list[set[tuple[int, int]]]] = []
            for row in range(4):
                row_masks: list[set[tuple[int, int]]] = []
                top = round(row * height / 4)
                bottom = round((row + 1) * height / 4)
                for column in range(4):
                    left = round(column * width / 4)
                    right = round((column + 1) * width / 4)
                    crop = gray.crop((left, top, right, bottom))
                    raw = {
                        (x, y)
                        for y in range(crop.height)
                        for x in range(crop.width)
                        if crop.getpixel((x, y)) < 170
                    }
                    row_masks.append(clean_mask(raw))
                    cell_sizes[sheet_name] = (crop.width, crop.height)
                rows.append(row_masks)
            sheet_masks[sheet_name] = rows

    frames: list[SourceFrame] = []
    for role in ROLE_SPECS:
        cell_width, cell_height = cell_sizes[role.sheet]
        for phase in range(4):
            frames.append(
                SourceFrame(
                    species=species,
                    role=role,
                    phase=phase,
                    mask=sheet_masks[role.sheet][role.row][phase],
                    cell_width=cell_width,
                    cell_height=cell_height,
                )
            )
    return frames, source_hashes


def median_x(mask: set[tuple[int, int]]) -> float:
    ordered = sorted(x for x, unused_y in mask)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return float(ordered[middle])
    return (ordered[middle - 1] + ordered[middle]) / 2


def allowed_scale(frame: SourceFrame) -> float:
    left, top, right, bottom = bounds(frame.mask)
    anchor = median_x(frame.mask)
    left_span = max(anchor - left, 1)
    right_span = max(right - anchor, 1)
    height = bottom - top + 1
    return min(
        (BODY_AXIS_X - SAFE_LEFT) / left_span,
        (SAFE_RIGHT - BODY_AXIS_X) / right_span,
        (FLOOR_Y - SAFE_TOP) / height,
    )


def rasterize(frame: SourceFrame, scale: float) -> set[tuple[int, int]]:
    left, top, right, bottom = bounds(frame.mask)
    source_width = right - left + 1
    source_height = bottom - top + 1
    crop = Image.new("1", (source_width, source_height), 0)
    pixels = crop.load()
    for x, y in frame.mask:
        pixels[x - left, y - top] = 1

    width = max(1, round(source_width * scale))
    height = max(1, round(source_height * scale))
    resized = crop.resize((width, height), Image.Resampling.NEAREST)
    resized_mask = {
        (x, y)
        for y in range(height)
        for x in range(width)
        if resized.getpixel((x, y))
    }
    if not resized_mask:
        raise ValueError(f"{frame.species}/{frame.role.name}/{frame.phase}: empty raster")

    local_left, _, local_right, _ = bounds(resized_mask)
    local_axis = median_x(resized_mask)
    local_bottom = max(y for unused_x, y in resized_mask)
    desired_origin_x = round(BODY_AXIS_X - local_axis)
    minimum_origin_x = SAFE_LEFT - local_left
    maximum_origin_x = SAFE_RIGHT - local_right
    if minimum_origin_x > maximum_origin_x:
        raise CanvasFitError(
            f"{frame.species}/{frame.role.name}/{frame.phase}: resized raster "
            "cannot fit the safe horizontal canvas"
        )
    origin_x = min(
        max(desired_origin_x, minimum_origin_x),
        maximum_origin_x,
    )
    origin_y = FLOOR_Y - local_bottom
    output = {(x + origin_x, y + origin_y) for x, y in resized_mask}
    out_left, out_top, out_right, out_bottom = bounds(output)
    out_axis = median_x(output)
    if (
        out_left < SAFE_LEFT
        or out_right > SAFE_RIGHT
        or out_top < SAFE_TOP
        or out_bottom != FLOOR_Y
        or not BODY_AXIS_X - 0.5 <= out_axis <= BODY_AXIS_X + 0.5
    ):
        raise CanvasFitError(
            f"{frame.species}/{frame.role.name}/{frame.phase}: aligned bounds "
            f"{(out_left, out_top, out_right, out_bottom)} with body axis "
            f"{out_axis} violate safe canvas"
        )
    return output


def frame_bytes(mask: set[tuple[int, int]]) -> bytes:
    packed = bytearray(FRAME_BYTES)
    for x, y in mask:
        packed[y * (FRAME_WIDTH // 8) + x // 8] |= 1 << (x & 7)
    return bytes(packed)


def build_pack(species: str, display_name: str, frames: list[bytes]) -> bytes:
    if len(frames) != 48:
        raise ValueError(f"{species}: expected 48 frames, got {len(frames)}")
    clips = bytearray()
    steps = bytearray()
    for clip_index, role in enumerate(ROLE_SPECS):
        first_step = clip_index * 4
        clips += struct.pack(
            "<BBBBIHH", role.role, 0, role.mode, 1, first_step, 4, 0
        )
        for phase, duration_ms in enumerate(role.durations_ms):
            steps += struct.pack("<HH", first_step + phase, duration_ms)

    payload = bytes(clips) + bytes(steps) + b"".join(frames)
    total_bytes = PACK_HEADER_BYTES + len(payload)
    if total_bytes > PACK_PARTITION_BYTES:
        raise ValueError(f"{species}: pack exceeds the asset partition")
    pack_id = binascii.crc32(f"kitsu868:{species}".encode("ascii")) & 0xFFFFFFFF
    if pack_id == 0:
        raise ValueError(f"{species}: generated zero pack ID")
    name = display_name.encode("ascii")
    if not 1 <= len(name) <= 15:
        raise ValueError(f"{species}: display name must use 1-15 ASCII bytes")

    header = bytearray(PACK_HEADER_BYTES)
    header[:8] = PACK_MAGIC
    struct.pack_into(
        "<HHIIIIIHHHHII16s",
        header,
        8,
        PACK_VERSION,
        PACK_HEADER_BYTES,
        total_bytes,
        binascii.crc32(payload) & 0xFFFFFFFF,
        0,
        pack_id,
        PACK_REVISION,
        FRAME_WIDTH,
        FRAME_HEIGHT,
        len(frames),
        len(ROLE_SPECS),
        len(ROLE_SPECS) * 4,
        0,
        name.ljust(16, b"\0"),
    )
    header_for_crc = bytearray(header[8:PACK_HEADER_BYTES])
    header_for_crc[0x14 - 8 : 0x18 - 8] = b"\0\0\0\0"
    struct.pack_into(
        "<I", header, 0x14, binascii.crc32(header_for_crc) & 0xFFFFFFFF
    )
    return bytes(header) + payload


def mask_image(mask: set[tuple[int, int]]) -> Image.Image:
    image = Image.new("1", (FRAME_WIDTH, FRAME_HEIGHT), 1)
    pixels = image.load()
    for x, y in mask:
        pixels[x, y] = 0
    return image


def write_contact_sheet(frames: list[set[tuple[int, int]]], path: Path) -> None:
    canvas = Image.new("1", (FRAME_WIDTH * 4, FRAME_HEIGHT * 12), 1)
    for index, mask in enumerate(frames):
        role = index // 4
        phase = index % 4
        canvas.paste(mask_image(mask), (phase * FRAME_WIDTH, role * FRAME_HEIGHT))
    canvas.resize((1024, 3072), Image.Resampling.NEAREST).save(path)


def write_role_gif(
    masks: list[set[tuple[int, int]]], role: RoleSpec, path: Path
) -> None:
    frames = [
        mask_image(mask).resize((256, 256), Image.Resampling.NEAREST).convert("P")
        for mask in masks
    ]
    sequence = frames
    durations = list(role.durations_ms)
    if role.mode == MODE_PINGPONG:
        sequence = frames + frames[-2:0:-1]
        durations += list(role.durations_ms[-2:0:-1])
    frames[0].save(
        path,
        save_all=True,
        append_images=sequence[1:],
        duration=durations,
        loop=0,
        optimize=False,
        disposal=2,
    )


def build_species(
    source_dir: Path, pack_dir: Path, evidence_dir: Path, species: str
) -> dict[str, object]:
    display_name = PUBLIC_SPECIES[species]
    source_frames, source_hashes = load_source_frames(source_dir, species)
    role_scales: dict[str, float] = {}
    output_masks: list[set[tuple[int, int]]] = []
    for role_index, role in enumerate(ROLE_SPECS):
        role_frames = source_frames[role_index * 4 : role_index * 4 + 4]
        role_scale = min(allowed_scale(frame) for frame in role_frames) * 0.985
        role_masks: list[set[tuple[int, int]]] | None = None
        last_fit_error: CanvasFitError | None = None
        for unused_attempt in range(64):
            try:
                role_masks = [rasterize(frame, role_scale) for frame in role_frames]
                break
            except CanvasFitError as error:
                last_fit_error = error
                role_scale *= 0.995
        if role_masks is None:
            raise ValueError(
                f"{species}/{role.name}: could not quantize role into the safe canvas"
            ) from last_fit_error
        role_scales[role.name] = role_scale
        output_masks.extend(role_masks)
    encoded_frames = [frame_bytes(mask) for mask in output_masks]

    role_evidence: list[dict[str, object]] = []
    for role_index, role in enumerate(ROLE_SPECS):
        role_masks = output_masks[role_index * 4 : role_index * 4 + 4]
        hashes = [sha256_bytes(frame_bytes(mask)) for mask in role_masks]
        unique_frames = len(set(hashes))
        changed_pixels = len(set().union(*role_masks) - set.intersection(*role_masks))
        if unique_frames < 3:
            raise ValueError(
                f"{species}/{role.name}: needs at least three distinct stored frames"
            )
        if changed_pixels < 8:
            raise ValueError(
                f"{species}/{role.name}: animation changes only {changed_pixels} pixels"
            )
        role_evidence.append(
            {
                "role": role.name,
                "role_id": role.role,
                "mode": ("hold", "once", "loop", "pingpong")[role.mode],
                "durations_ms": list(role.durations_ms),
                "unique_frames": unique_frames,
                "changed_pixels": changed_pixels,
                "frame_sha256": hashes,
            }
        )
        write_role_gif(
            role_masks, role, evidence_dir / f"{species}-{role.name}.gif"
        )

    pack = build_pack(species, display_name, encoded_frames)
    pack_path = pack_dir / f"{species}.k868"
    pack_path.write_bytes(pack)
    contact_path = evidence_dir / f"{species}-48-frame-contact.png"
    write_contact_sheet(output_masks, contact_path)

    geometry = []
    for frame, mask in zip(source_frames, output_masks, strict=True):
        left, top, right, bottom = bounds(mask)
        geometry.append(
            {
                "role": frame.role.name,
                "phase": frame.phase,
                "bounds": [left, top, right, bottom],
                "body_axis_x": median_x(mask),
                "floor_y": bottom,
                "ink_pixels": len(mask),
            }
        )

    return {
        "species": species,
        "display_name": display_name,
        "pack_id": f"{struct.unpack_from('<I', pack, 0x18)[0]:08X}",
        "revision": PACK_REVISION,
        "format": "K868PK1",
        "frame_canvas": [FRAME_WIDTH, FRAME_HEIGHT],
        "stored_frames": 48,
        "clips": 12,
        "steps": 48,
        "body_axis_target_x": BODY_AXIS_X,
        "floor_target_y": FLOOR_Y,
        "role_source_scales": role_scales,
        "pack_file": pack_path.name,
        "pack_bytes": len(pack),
        "pack_sha256": sha256_bytes(pack),
        "source_sha256": source_hashes,
        "contact_sheet": contact_path.name,
        "contact_sheet_sha256": sha256_file(contact_path),
        "roles": role_evidence,
        "geometry": geometry,
    }


def parse_args() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source-dir", type=Path, default=root / "assets" / "companion-sources"
    )
    parser.add_argument(
        "--pack-dir", type=Path, default=root / "assets" / "packs"
    )
    parser.add_argument(
        "--evidence-dir",
        type=Path,
        default=root / "assets" / "pack-evidence",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    source_dir = args.source_dir.resolve()
    pack_dir = args.pack_dir.resolve()
    evidence_dir = args.evidence_dir.resolve()
    pack_dir.mkdir(parents=True, exist_ok=True)
    evidence_dir.mkdir(parents=True, exist_ok=True)

    unexpected_sources = sorted(
        path.name
        for path in source_dir.glob("*.png")
        if path.stem.split("-", 1)[0] not in PUBLIC_SPECIES
    )
    if unexpected_sources:
        raise ValueError(
            "non-public companion sources entered the default build: "
            + ", ".join(unexpected_sources)
        )

    results = [
        build_species(source_dir, pack_dir, evidence_dir, species)
        for species in PUBLIC_SPECIES
    ]
    manifest = {
        "schema": "kitsu-default-pack-release-v1",
        "public_default_species": list(PUBLIC_SPECIES),
        "brand_mascot_is_not_a_pack": True,
        "private_companions_included": False,
        "display_contract": {
            "device": "Heltec WiFi LoRa 32 V3/V3.2",
            "oled_orientation": "portrait",
            "oled_pixels": [64, 128],
            "pack_frame_pixels": [64, 64],
            "body_axis_x": BODY_AXIS_X,
            "floor_y": FLOOR_Y,
            "safe_bounds": [SAFE_LEFT, SAFE_TOP, SAFE_RIGHT, FLOOR_Y],
        },
        "animation_contract": {
            "roles": [role.name for role in ROLE_SPECS],
            "stored_frames_per_role": 4,
            "required_unique_frames_per_role": 3,
            "required_changed_pixels_per_role": 8,
        },
        "packs": results,
    }
    manifest_path = pack_dir / "default-packs-manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    checksum_lines = [
        f"{sha256_file(pack_dir / f'{species}.k868')}  {species}.k868"
        for species in PUBLIC_SPECIES
    ]
    checksum_lines.append(
        f"{sha256_file(manifest_path)}  {manifest_path.name}"
    )
    (pack_dir / "SHA256SUMS.txt").write_text(
        "\n".join(checksum_lines) + "\n", encoding="ascii"
    )

    for result in results:
        print(
            "PACK_BUILT "
            f"name={result['display_name']} file={result['pack_file']} "
            f"frames={result['stored_frames']} clips={result['clips']} "
            f"steps={result['steps']} bytes={result['pack_bytes']} "
            f"sha256={result['pack_sha256']}"
        )
    print(f"PACK_MANIFEST {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
