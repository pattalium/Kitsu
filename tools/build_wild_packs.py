#!/usr/bin/env python3
"""Build private Kitsu wild-creature packs from per-action identity-locked art.

Every creature owns one approved canonical identity image and twelve separate
2x2 action assets. This tool requires all four frames of each action, applies
one identity-locked full-cell nearest-neighbour scale, rigidly translates each
frame to the fixed center/floor, builds ordinary CRC-only K868PK1 packs, and
derives one 16x18 private portrait from the identity master.

Action art, serialized frames, GIFs, contact sheets, manifests containing frame
hashes, and pack bytes are private release inputs. The tool refuses to read or
write those inputs inside the public checkout and has no publication path.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import json
import os
import shutil
import struct
import tempfile
from pathlib import Path

from PIL import Image

import build_default_packs as base
import companion_raster_contract as raster_contract
from install_pack import validate_pack


WILD_SPECIES: dict[str, dict[str, str]] = {
    "frog": dict(display_name="FROG", public_name="Frog", rarity="common", pack_id="5CAC86A3", slug="frog"),
    "hamster": dict(display_name="HAMSTER", public_name="Hamster", rarity="common", pack_id="13793DC7", slug="hamster"),
    "turtle": dict(display_name="TURTLE", public_name="Turtle", rarity="common", pack_id="7495DBFB", slug="turtle"),
    "rabbit": dict(display_name="RABBIT", public_name="Rabbit", rarity="uncommon", pack_id="68D9554E", slug="rabbit"),
    "hedgehog": dict(display_name="HEDGEHOG", public_name="Hedgehog", rarity="uncommon", pack_id="5DF6BE74", slug="hedgehog"),
    "ferret": dict(display_name="FERRET", public_name="Ferret", rarity="uncommon", pack_id="E59408E0", slug="ferret"),
    "otter": dict(display_name="OTTER", public_name="Otter", rarity="rare", pack_id="29B4B2F7", slug="otter"),
    "axolotl": dict(display_name="AXOLOTL", public_name="Axolotl", rarity="rare", pack_id="69276D0C", slug="axolotl"),
    "chinchilla": dict(display_name="CHINCHILLA", public_name="Chinchilla", rarity="rare", pack_id="2DFB0797", slug="chinchilla"),
    "raccoon": dict(display_name="RACCOON", public_name="Raccoon", rarity="very_rare", pack_id="C163EFED", slug="raccoon"),
    "capybara": dict(display_name="CAPYBARA", public_name="Capybara", rarity="very_rare", pack_id="374D2540", slug="capybara"),
    "sugar_glider": dict(display_name="SUGAR GLIDER", public_name="Sugar Glider", rarity="very_rare", pack_id="39FC5B1A", slug="sugar-glider"),
    "red_panda": dict(display_name="RED PANDA", public_name="Red Panda", rarity="epic", pack_id="91A2DE7B", slug="red-panda"),
    "pangolin": dict(display_name="PANGOLIN", public_name="Pangolin", rarity="epic", pack_id="E04EC405", slug="pangolin"),
    "tasmanian_devil": dict(display_name="TAS DEVIL", public_name="Tasmanian Devil", rarity="epic", pack_id="8E0E1B03", slug="tasmanian-devil"),
    "snow_leopard": dict(display_name="SNOW LEOPARD", public_name="Snow Leopard", rarity="legendary", pack_id="533B9B30", slug="snow-leopard"),
    "okapi": dict(display_name="OKAPI", public_name="Okapi", rarity="legendary", pack_id="86F3BB5D", slug="okapi"),
    "shoebill": dict(display_name="SHOEBILL", public_name="Shoebill", rarity="legendary", pack_id="2D1D89AF", slug="shoebill"),
    "cat_girl": dict(display_name="CAT GIRL", public_name="Cat Girl", rarity="mythical", pack_id="A52160C5", slug="cat-girl"),
    "rabbit_girl": dict(display_name="RABBIT GIRL", public_name="Rabbit Girl", rarity="mythical", pack_id="F0F750BD", slug="rabbit-girl"),
    "deer_girl": dict(display_name="DEER GIRL", public_name="Deer Girl", rarity="mythical", pack_id="52A1C03A", slug="deer-girl"),
}

PORTRAIT_WIDTH = 16
PORTRAIT_HEIGHT = 18
PORTRAIT_BYTES = PORTRAIT_WIDTH * PORTRAIT_HEIGHT // 8
WILD_PACK_REVISION = 3


def is_within(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def require_private_path(path: Path, project_root: Path, label: str) -> None:
    if is_within(path, project_root):
        raise ValueError(f"{label} must stay outside the public checkout: {path}")


def make_portrait(mask: set[tuple[int, int]]) -> tuple[set[tuple[int, int]], bytes]:
    source = base.mask_image(mask)
    available_width = PORTRAIT_WIDTH
    available_height = PORTRAIT_HEIGHT
    scale = min(available_width / source.width, available_height / source.height)
    width = max(1, round(source.width * scale))
    height = max(1, round(source.height * scale))
    reduced = source.resize((width, height), Image.Resampling.NEAREST)
    if reduced.mode != "1":
        raise ValueError("nearest-neighbour portrait conversion is not strictly 1-bit")
    portrait: set[tuple[int, int]] = set()
    origin_x = (PORTRAIT_WIDTH - width) // 2
    origin_y = (PORTRAIT_HEIGHT - height) // 2
    for y in range(height):
        for x in range(width):
            if reduced.getpixel((x, y)) == 0:
                portrait.add((x + origin_x, y + origin_y))
    if not 16 <= len(portrait) <= 220:
        raise ValueError(f"derived portrait has implausible ink density: {len(portrait)}")
    components = sorted(base.connected_components(portrait), key=len, reverse=True)
    if len(components) > raster_contract.MAX_OUTPUT_COMPONENTS:
        raise ValueError(
            f"derived portrait has {len(components)} fragmented components"
        )
    if len(components[0]) / len(portrait) < 0.55:
        raise ValueError("derived portrait no longer has one dominant identity subject")

    packed = bytearray(PORTRAIT_BYTES)
    for x, y in portrait:
        packed[y * 2 + x // 8] |= 1 << (x & 7)
    round_trip = {
        (x, y)
        for y in range(PORTRAIT_HEIGHT)
        for x in range(PORTRAIT_WIDTH)
        if packed[y * 2 + x // 8] & (1 << (x & 7))
    }
    if round_trip != portrait:
        raise ValueError("16x18 portrait packing changed 1-bit pixels")
    return portrait, bytes(packed)


def write_portrait_png(mask: set[tuple[int, int]], path: Path) -> None:
    image = Image.new("1", (PORTRAIT_WIDTH, PORTRAIT_HEIGHT), 1)
    pixels = image.load()
    for x, y in mask:
        pixels[x, y] = 0
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path, optimize=False)


def build_wild_pack(species: str, display_name: str, frames: list[bytes]) -> bytes:
    """Build K868PK1 while advancing the leaked draft's revision to v3."""

    pack = bytearray(base.build_pack(species, display_name, frames))
    struct.pack_into("<I", pack, 0x1C, WILD_PACK_REVISION)
    header_for_crc = bytearray(pack[8 : base.PACK_HEADER_BYTES])
    header_for_crc[0x14 - 8 : 0x18 - 8] = b"\0\0\0\0"
    struct.pack_into(
        "<I", pack, 0x14, binascii.crc32(header_for_crc) & 0xFFFFFFFF
    )
    return bytes(pack)


def build_species(
    source_dir: Path,
    private_output: Path,
    species: str,
    identity_lock: raster_contract.IdentityLock,
) -> dict[str, object]:
    definition = WILD_SPECIES[species]
    raster = raster_contract.load_species_raster(
        source_dir, species, identity_lock
    )
    raster_frames = list(raster.frames)
    source_frames = [frame.source_frame for frame in raster_frames]
    output_masks = [set(frame.output_mask) for frame in raster_frames]
    identity_mask = set(raster.identity.output_mask)
    idle_similarity = raster_frames[0].identity_jaccard
    role_scales = {
        role.name: raster.fixed_action_scale for role in base.ROLE_SPECS
    }

    evidence_dir = private_output / "evidence"
    evidence_dir.mkdir(parents=True, exist_ok=True)
    role_evidence: list[dict[str, object]] = []
    for role_index, role in enumerate(base.ROLE_SPECS):
        role_rasters = raster_frames[role_index * 4 : role_index * 4 + 4]
        role_masks = output_masks[role_index * 4 : role_index * 4 + 4]
        hashes = [frame.output_sha256 for frame in role_rasters]
        unique_frames = len(set(hashes))
        changed_pixels = len(set().union(*role_masks) - set.intersection(*role_masks))
        similarities = [round(frame.identity_jaccard, 4) for frame in role_rasters]
        role_evidence.append(
            {
                "role": role.name,
                "role_id": role.role,
                "mode": ("hold", "once", "loop", "pingpong")[role.mode],
                "durations_ms": list(role.durations_ms),
                "unique_frames": unique_frames,
                "changed_pixels": changed_pixels,
                "identity_jaccard": similarities,
                "apparent_scale_ratio": [
                    round(frame.apparent_scale_ratio, 4) for frame in role_rasters
                ],
                "source_mask_sha256": [
                    frame.source_mask_sha256 for frame in role_rasters
                ],
                "frame_sha256": hashes,
            }
        )
        base.write_role_gif(
            role_masks, role, evidence_dir / f"{species}-{role.name}.gif"
        )

    encoded_frames = [base.frame_bytes(mask) for mask in output_masks]
    pack = build_wild_pack(species, definition["display_name"], encoded_frames)
    pack_path = private_output / f"{species}.k868"
    with pack_path.open("xb") as output:
        output.write(pack)
    parsed = validate_pack(pack_path)
    actual_pack_id = f"{parsed.pack_id:08X}"
    if actual_pack_id != definition["pack_id"]:
        raise ValueError(
            f"{species}: expected pack ID {definition['pack_id']}, got {actual_pack_id}"
        )

    contact_path = evidence_dir / f"{species}-48-frame-contact.png"
    base.write_contact_sheet(output_masks, contact_path)
    portrait_mask, portrait_bytes = make_portrait(identity_mask)
    portrait_path = private_output / "portraits" / f"{species}-16x18.png"
    write_portrait_png(portrait_mask, portrait_path)

    geometry: list[dict[str, object]] = []
    for source, raster_frame, mask in zip(
        source_frames, raster_frames, output_masks, strict=True
    ):
        left, top, right, bottom = base.bounds(mask)
        components = base.connected_components(mask)
        geometry.append(
            {
                "role": source.role.name,
                "phase": source.phase,
                "bounds": [left, top, right, bottom],
                "body_axis_x": base.median_x(mask),
                "floor_y": bottom,
                "ink_pixels": len(mask),
                "components": len(components),
                "smallest_component_pixels": min(map(len, components)),
                "source_bounds": list(raster_frame.source_metrics.bounds),
                "source_components": raster_frame.source_metrics.components,
                "source_primary_fraction": round(
                    raster_frame.source_metrics.primary_fraction, 4
                ),
            }
        )

    return {
        "identity_key": species,
        "slug": definition["slug"],
        "display_name": definition["public_name"],
        "pack_display_name": definition["display_name"],
        "rarity": definition["rarity"],
        "pack_id": actual_pack_id,
        "revision": WILD_PACK_REVISION,
        "format": "K868PK1",
        "frame_canvas": [base.FRAME_WIDTH, base.FRAME_HEIGHT],
        "stored_frames": len(output_masks),
        "clips": len(base.ROLE_SPECS),
        "steps": len(base.ROLE_SPECS) * 4,
        "pack_file": pack_path.name,
        "pack_bytes": len(pack),
        "pack_sha256": base.sha256_bytes(pack),
        "source_sha256": raster.source_sha256,
        "identity_lock": {
            "schema": raster_contract.IDENTITY_LOCK_SCHEMA,
            "identity_sha256": identity_lock.identity_sha256,
            "source_canvas": list(identity_lock.source_canvas),
            "target_long_axis_pixels": identity_lock.target_long_axis_pixels,
        },
        "identity_idle_jaccard": round(idle_similarity, 4),
        "fixed_action_scale": raster.fixed_action_scale,
        "raster_transform": "full-cell-nearest-neighbour-resize-then-translation",
        "auto_crop": False,
        "auto_shrink": False,
        "source_cleanup": False,
        "role_source_scales": role_scales,
        "contact_sheet": contact_path.name,
        "contact_sheet_sha256": base.sha256_file(contact_path),
        "portrait": {
            "width": PORTRAIT_WIDTH,
            "height": PORTRAIT_HEIGHT,
            "source": "approved-identity-master-64x64",
            "resampling": "full-frame-nearest-neighbour-1-bit",
            "storage": "XBM least-significant-bit first, two bytes per row",
            "bytes": len(portrait_bytes),
            "bitmap_hex": portrait_bytes.hex(),
            "bitmap_base64": base64.b64encode(portrait_bytes).decode("ascii"),
            "png_file": portrait_path.name,
            "png_sha256": base.sha256_file(portrait_path),
        },
        "roles": role_evidence,
        "geometry": geometry,
    }


def verify_visual_acceptance(
    acceptance_path: Path,
    results: list[dict[str, object]],
    project_root: Path,
) -> str:
    """Bind human visual approval to the exact rasterized release inputs.

    Mechanical pack checks deliberately cannot decide whether an animal still
    looks like itself or whether four frames tell the named action. Publication
    therefore requires a separate, private record that approves every role and
    pins both the resulting pack and the reviewed contact sheet.
    """
    resolved = acceptance_path.resolve()
    require_private_path(resolved, project_root, "visual acceptance record")
    payload = json.loads(resolved.read_text(encoding="utf-8"))
    if not isinstance(payload, dict) or set(payload) != {"schema", "packs"}:
        raise ValueError("visual acceptance record must contain exactly schema and packs")
    if payload["schema"] != "kitsu-wild-visual-acceptance-v1":
        raise ValueError("unsupported visual acceptance schema")
    records = payload["packs"]
    if not isinstance(records, list) or len(records) != len(results):
        raise ValueError("visual acceptance must cover every selected pack exactly once")

    expected_roles = [role.name for role in base.ROLE_SPECS]
    by_identity: dict[str, dict[str, object]] = {}
    for record in records:
        if not isinstance(record, dict) or set(record) != {
            "accepted_roles",
            "contact_sheet_sha256",
            "identity_key",
            "pack_sha256",
        }:
            raise ValueError("visual acceptance pack record has unexpected fields")
        identity = record["identity_key"]
        if not isinstance(identity, str) or identity in by_identity:
            raise ValueError("visual acceptance contains an invalid or duplicate identity")
        accepted_roles = record["accepted_roles"]
        if accepted_roles != expected_roles:
            raise ValueError(
                f"{identity}: visual acceptance must approve all roles in canonical order"
            )
        by_identity[identity] = record

    if set(by_identity) != {str(result["identity_key"]) for result in results}:
        raise ValueError("visual acceptance identity set differs from the selected build")
    for result in results:
        identity = str(result["identity_key"])
        record = by_identity[identity]
        if record["pack_sha256"] != result["pack_sha256"]:
            raise ValueError(f"{identity}: accepted pack SHA-256 does not match")
        if record["contact_sheet_sha256"] != result["contact_sheet_sha256"]:
            raise ValueError(f"{identity}: accepted contact-sheet SHA-256 does not match")
    return base.sha256_file(resolved)


def snapshot_species_sources(
    source_dir: Path,
    staging_output: Path,
    species: str,
    expected_hashes: dict[str, str],
) -> dict[str, str]:
    """Copy original PNG bytes into the private staged result without overwrite."""

    destination_dir = staging_output / "source-snapshot" / species
    destination_dir.mkdir(parents=True, exist_ok=False)
    copied: dict[str, str] = {}
    for filename, expected_hash in sorted(expected_hashes.items()):
        source = source_dir / species / filename
        destination = destination_dir / filename
        with source.open("rb") as source_handle, destination.open("xb") as output:
            shutil.copyfileobj(source_handle, output, length=1024 * 1024)
        actual_hash = base.sha256_file(destination)
        if actual_hash != expected_hash:
            raise ValueError(
                f"{species}/{filename}: immutable source snapshot hash changed"
            )
        copied[filename] = actual_hash
    return copied


def commit_staged_output(staging_output: Path, private_output: Path) -> None:
    """Rename a complete staging directory only when the destination is absent."""

    if private_output.exists():
        raise ValueError(
            f"private output appeared during build and will not be overwritten: "
            f"{private_output}"
        )
    os.rename(staging_output, private_output)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--private-output", type=Path, required=True)
    parser.add_argument(
        "--identity-lock",
        type=Path,
        required=True,
        help=(
            "Private kitsu-wild-identity-lock-v1 file pinning each approved "
            "identity master and its fixed release scale."
        ),
    )
    parser.add_argument(
        "--species",
        action="append",
        choices=tuple(WILD_SPECIES),
        help="Build only the named identity for a private checkpoint; repeat as needed.",
    )
    parser.add_argument(
        "--visual-acceptance",
        type=Path,
        help=(
            "Private kitsu-wild-visual-acceptance-v1 record binding every "
            "approved action to this exact private build."
        ),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    project_root = Path(__file__).resolve().parents[1]
    source_dir = args.source_dir.resolve()
    private_output = args.private_output.resolve()
    identity_lock_path = args.identity_lock.resolve()
    require_private_path(source_dir, project_root, "private action source directory")
    require_private_path(private_output, project_root, "private pack output directory")
    require_private_path(identity_lock_path, project_root, "private identity lock")
    if private_output.exists():
        raise ValueError(
            f"private output already exists and will not be overwritten: {private_output}"
        )
    selected = list(dict.fromkeys(args.species or WILD_SPECIES.keys()))
    identity_locks = raster_contract.load_identity_locks(
        identity_lock_path, selected
    )
    private_output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=f".{private_output.name}.staging-", dir=private_output.parent
    ) as temporary:
        staging_output = Path(temporary)
        results: list[dict[str, object]] = []
        for species in selected:
            result = build_species(
                source_dir, staging_output, species, identity_locks[species]
            )
            snapshot = snapshot_species_sources(
                source_dir,
                staging_output,
                species,
                dict(result["source_sha256"]),
            )
            result["source_snapshot"] = {
                "path": f"source-snapshot/{species}",
                "byte_exact_sha256": snapshot,
            }
            results.append(result)

        visual_acceptance_sha256 = None
        if args.visual_acceptance is not None:
            visual_acceptance_sha256 = verify_visual_acceptance(
                args.visual_acceptance, results, project_root
            )
        manifest = {
            "schema": "kitsu-wild-pack-private-release-v3",
            "identity_keys": selected,
            "complete_roster": selected == list(WILD_SPECIES),
            "public_static_assets_only": True,
            "private_action_assets": True,
            "private_pack_delivery": True,
            "format_security": "ordinary K868PK1 structural checks and CRC32",
            "identity_lock_schema": raster_contract.IDENTITY_LOCK_SCHEMA,
            "identity_lock_sha256": base.sha256_file(identity_lock_path),
            "non_destructive_build": True,
            "display_contract": {
                "device": "Heltec WiFi LoRa 32 V3/V3.2",
                "oled_pixels": [64, 128],
                "pack_frame_pixels": [64, 64],
                "body_axis_x": base.BODY_AXIS_X,
                "floor_y": base.FLOOR_Y,
                "safe_bounds": [
                    base.SAFE_LEFT,
                    base.SAFE_TOP,
                    base.SAFE_RIGHT,
                    base.FLOOR_Y,
                ],
            },
            "raster_contract": {
                "transform": (
                    "full-cell-nearest-neighbour-resize-then-translation"
                ),
                "auto_crop": False,
                "auto_shrink": False,
                "source_cleanup": False,
                "source_snapshots": True,
                "portrait_resampling": "full-frame-nearest-neighbour-1-bit",
            },
            "animation_contract": {
                "roles": [role.name for role in base.ROLE_SPECS],
                "stored_frames_per_role": 4,
                "required_unique_frames_per_role": 4,
                "required_changed_pixels_per_role": 8,
                "source_asset_per_role": True,
                "mechanical_validation_is_visual_acceptance": False,
            },
            "visual_acceptance_sha256": visual_acceptance_sha256,
            "packs": results,
        }
        manifest_path = staging_output / "wild-packs-manifest.json"
        with manifest_path.open("x", encoding="utf-8", newline="\n") as output:
            output.write(json.dumps(manifest, indent=2, sort_keys=True) + "\n")

        commit_staged_output(staging_output, private_output)

    for result in results:
        print(
            "WILD_PACK_BUILT "
            f"identity={result['identity_key']} name={result['display_name']} "
            f"rarity={result['rarity']} id={result['pack_id']} "
            f"frames={result['stored_frames']} clips={result['clips']} "
            f"bytes={result['pack_bytes']} sha256={result['pack_sha256']} "
            f"visual_accepted={visual_acceptance_sha256 is not None}"
        )
    print(f"PRIVATE_WILD_MANIFEST {private_output / 'wild-packs-manifest.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
