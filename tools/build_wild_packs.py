#!/usr/bin/env python3
"""Build the explicitly published Kitsu wild-creature companion packs.

Wild packs use the exact K868PK1 frame, clip, step, timing, alignment, and CRC
contracts used by the three starter companions.  Their source art lives in a
separate directory so extending the wild catalog cannot widen the starter-pack
release allow-list.
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

import build_default_packs as base
from install_pack import validate_pack


WILD_SPECIES = {
    "frog": {
        "display_name": "FROG",
        "rarity": "common",
        "pack_id": "5CAC86A3",
    },
}


def build_species(
    source_dir: Path,
    pack_dir: Path,
    evidence_dir: Path,
    species: str,
) -> dict[str, object]:
    definition = WILD_SPECIES[species]
    display_name = str(definition["display_name"])
    source_frames, source_hashes = base.load_source_frames(source_dir, species)
    role_scales: dict[str, float] = {}
    output_masks: list[set[tuple[int, int]]] = []

    for role_index, role in enumerate(base.ROLE_SPECS):
        role_frames = source_frames[role_index * 4 : role_index * 4 + 4]
        role_scale = min(base.allowed_scale(frame) for frame in role_frames) * 0.985
        role_masks: list[set[tuple[int, int]]] | None = None
        last_fit_error: base.CanvasFitError | None = None
        for unused_attempt in range(64):
            try:
                role_masks = [base.rasterize(frame, role_scale) for frame in role_frames]
                break
            except base.CanvasFitError as error:
                last_fit_error = error
                role_scale *= 0.995
        if role_masks is None:
            raise ValueError(
                f"{species}/{role.name}: could not quantize role into the safe canvas"
            ) from last_fit_error
        role_scales[role.name] = role_scale
        output_masks.extend(role_masks)

    encoded_frames = [base.frame_bytes(mask) for mask in output_masks]
    role_evidence: list[dict[str, object]] = []
    for role_index, role in enumerate(base.ROLE_SPECS):
        role_masks = output_masks[role_index * 4 : role_index * 4 + 4]
        hashes = [base.sha256_bytes(base.frame_bytes(mask)) for mask in role_masks]
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

    pack = base.build_pack(species, display_name, encoded_frames)
    pack_path = pack_dir / f"{species}.k868"
    pack_path.write_bytes(pack)
    contact_path = evidence_dir / f"{species}-48-frame-contact.png"
    base.write_contact_sheet(output_masks, contact_path)

    geometry: list[dict[str, object]] = []
    for frame, mask in zip(source_frames, output_masks, strict=True):
        left, top, right, bottom = base.bounds(mask)
        geometry.append(
            {
                "role": frame.role.name,
                "phase": frame.phase,
                "bounds": [left, top, right, bottom],
                "body_axis_x": base.median_x(mask),
                "floor_y": bottom,
                "ink_pixels": len(mask),
            }
        )

    parsed = validate_pack(pack_path)
    actual_pack_id = f"{parsed.pack_id:08X}"
    if actual_pack_id != definition["pack_id"]:
        raise ValueError(
            f"{species}: expected pack ID {definition['pack_id']}, got {actual_pack_id}"
        )

    return {
        "species": species,
        "display_name": display_name,
        "rarity": definition["rarity"],
        "pack_id": actual_pack_id,
        "revision": base.PACK_REVISION,
        "format": "K868PK1",
        "frame_canvas": [base.FRAME_WIDTH, base.FRAME_HEIGHT],
        "stored_frames": len(output_masks),
        "clips": len(base.ROLE_SPECS),
        "steps": len(base.ROLE_SPECS) * 4,
        "body_axis_target_x": base.BODY_AXIS_X,
        "floor_target_y": base.FLOOR_Y,
        "role_source_scales": role_scales,
        "pack_file": pack_path.name,
        "pack_bytes": len(pack),
        "pack_sha256": base.sha256_bytes(pack),
        "source_sha256": source_hashes,
        "contact_sheet": contact_path.name,
        "contact_sheet_sha256": base.sha256_file(contact_path),
        "roles": role_evidence,
        "geometry": geometry,
    }


def parse_args() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source-dir", type=Path, default=root / "assets" / "wild-companion-sources"
    )
    parser.add_argument(
        "--pack-dir", type=Path, default=root / "assets" / "wild-packs"
    )
    parser.add_argument(
        "--evidence-dir",
        type=Path,
        default=root / "assets" / "wild-pack-evidence",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    source_dir = args.source_dir.resolve()
    pack_dir = args.pack_dir.resolve()
    evidence_dir = args.evidence_dir.resolve()
    pack_dir.mkdir(parents=True, exist_ok=True)
    evidence_dir.mkdir(parents=True, exist_ok=True)

    expected_sources = {
        f"{species}-{sheet}.png"
        for species in WILD_SPECIES
        for sheet in ("core", "life", "social")
    }
    actual_sources = {path.name for path in source_dir.glob("*.png")}
    if actual_sources != expected_sources:
        raise ValueError(
            "wild source set does not match the explicit publication allow-list: "
            f"expected={sorted(expected_sources)} actual={sorted(actual_sources)}"
        )

    results = [
        build_species(source_dir, pack_dir, evidence_dir, species)
        for species in WILD_SPECIES
    ]
    manifest = {
        "schema": "kitsu-wild-pack-release-v1",
        "public_wild_species": list(WILD_SPECIES),
        "starter_companions_included": False,
        "private_companions_included": False,
        "display_contract": {
            "device": "Heltec WiFi LoRa 32 V3/V3.2",
            "oled_orientation": "portrait",
            "oled_pixels": [64, 128],
            "pack_frame_pixels": [64, 64],
            "body_axis_x": base.BODY_AXIS_X,
            "floor_y": base.FLOOR_Y,
            "safe_bounds": [base.SAFE_LEFT, base.SAFE_TOP, base.SAFE_RIGHT, base.FLOOR_Y],
        },
        "animation_contract": {
            "roles": [role.name for role in base.ROLE_SPECS],
            "stored_frames_per_role": 4,
            "required_unique_frames_per_role": 3,
            "required_changed_pixels_per_role": 8,
        },
        "packs": results,
    }
    manifest_path = pack_dir / "wild-packs-manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    for result in results:
        print(
            "WILD_PACK_BUILT "
            f"name={result['display_name']} rarity={result['rarity']} "
            f"id={result['pack_id']} frames={result['stored_frames']} "
            f"clips={result['clips']} steps={result['steps']} "
            f"bytes={result['pack_bytes']} sha256={result['pack_sha256']}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
