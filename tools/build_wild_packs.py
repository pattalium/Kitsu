#!/usr/bin/env python3
"""Build private Kitsu wild-creature packs from per-action identity-locked art.

Every creature owns one canonical identity image and twelve independently
generated 2x2 action assets. This tool mechanically extracts the four frames
from each action, quantizes them to the production 64x64 OLED contract, builds
ordinary CRC-only K868PK1 packs, and derives one public 16x18 static portrait.

Action art, serialized frames, GIFs, contact sheets, manifests containing frame
hashes, and pack bytes are private release inputs. The tool refuses to read or
write those inputs inside the public checkout. Only the optional 16x18 portrait
publication step may write into the checkout, and it requires the full roster.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import json
import struct
from pathlib import Path

from PIL import Image

import build_default_packs as base
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


def load_mask(image: Image.Image) -> set[tuple[int, int]]:
    gray = image.convert("L")
    raw = {
        (x, y)
        for y in range(gray.height)
        for x in range(gray.width)
        if gray.getpixel((x, y)) < 170
    }
    return base.clean_mask(raw)


def load_identity_frame(species_dir: Path, species: str) -> tuple[base.SourceFrame, str]:
    path = species_dir / "identity.png"
    with Image.open(path) as image:
        if image.width < 512 or image.height < 512:
            raise ValueError(f"{path}: canonical identity image is too small")
        mask = load_mask(image)
        frame = base.SourceFrame(
            species=species,
            role=base.ROLE_SPECS[0],
            phase=0,
            mask=mask,
            cell_width=image.width,
            cell_height=image.height,
        )
    return frame, base.sha256_file(path)


def load_action_frames(
    source_dir: Path, species: str
) -> tuple[list[base.SourceFrame], dict[str, str], base.SourceFrame]:
    species_dir = source_dir / species
    if not species_dir.is_dir():
        raise FileNotFoundError(f"missing private creature source directory: {species_dir}")
    expected = {"identity.png", *(f"{role.name}.png" for role in base.ROLE_SPECS)}
    actual = {path.name for path in species_dir.glob("*.png")}
    if actual != expected:
        raise ValueError(
            f"{species}: action source set differs from the explicit contract: "
            f"missing={sorted(expected - actual)} unexpected={sorted(actual - expected)}"
        )

    identity_frame, identity_hash = load_identity_frame(species_dir, species)
    hashes = {"identity.png": identity_hash}
    frames: list[base.SourceFrame] = []
    for role in base.ROLE_SPECS:
        path = species_dir / f"{role.name}.png"
        hashes[path.name] = base.sha256_file(path)
        with Image.open(path) as image:
            if image.width < 512 or image.height < 512:
                raise ValueError(f"{path}: four-frame action image is too small")
            ratio = image.width / image.height
            if not 0.9 <= ratio <= 1.1:
                raise ValueError(f"{path}: action asset must use a square 2x2 layout")
            gray = image.convert("L")
            for phase in range(4):
                column = phase % 2
                row = phase // 2
                left = round(column * gray.width / 2)
                right = round((column + 1) * gray.width / 2)
                top = round(row * gray.height / 2)
                bottom = round((row + 1) * gray.height / 2)
                crop = gray.crop((left, top, right, bottom))
                frames.append(
                    base.SourceFrame(
                        species=species,
                        role=role,
                        phase=phase,
                        mask=load_mask(crop),
                        cell_width=crop.width,
                        cell_height=crop.height,
                    )
                )
    return frames, hashes, identity_frame


def rasterize_role(
    species: str, role: base.RoleSpec, frames: list[base.SourceFrame]
) -> tuple[list[set[tuple[int, int]]], float]:
    role_scale = min(base.allowed_scale(frame) for frame in frames) * 0.985
    last_fit_error: base.CanvasFitError | None = None
    for _unused_attempt in range(64):
        try:
            return [base.rasterize(frame, role_scale) for frame in frames], role_scale
        except base.CanvasFitError as error:
            last_fit_error = error
            role_scale *= 0.995
    raise ValueError(f"{species}/{role.name}: cannot fit safe 64x64 canvas") from last_fit_error


def jaccard(left: set[tuple[int, int]], right: set[tuple[int, int]]) -> float:
    union = left | right
    return len(left & right) / len(union) if union else 0.0


def make_portrait(mask: set[tuple[int, int]]) -> tuple[set[tuple[int, int]], bytes]:
    left, top, right, bottom = base.bounds(mask)
    source = base.mask_image(mask).convert("L").crop((left, top, right + 1, bottom + 1))
    available_width = PORTRAIT_WIDTH - 2
    available_height = PORTRAIT_HEIGHT - 2
    scale = min(available_width / source.width, available_height / source.height)
    width = max(1, round(source.width * scale))
    height = max(1, round(source.height * scale))
    reduced = source.resize((width, height), Image.Resampling.BOX)
    portrait: set[tuple[int, int]] = set()
    origin_x = (PORTRAIT_WIDTH - width) // 2
    origin_y = (PORTRAIT_HEIGHT - height) // 2
    for y in range(height):
        for x in range(width):
            if reduced.getpixel((x, y)) < 220:
                portrait.add((x + origin_x, y + origin_y))
    if not 24 <= len(portrait) <= 220:
        raise ValueError(f"derived portrait has implausible ink density: {len(portrait)}")

    packed = bytearray(PORTRAIT_BYTES)
    for x, y in portrait:
        packed[y * 2 + x // 8] |= 1 << (x & 7)
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
) -> dict[str, object]:
    definition = WILD_SPECIES[species]
    source_frames, source_hashes, identity_frame = load_action_frames(source_dir, species)
    role_scales: dict[str, float] = {}
    output_masks: list[set[tuple[int, int]]] = []

    for role_index, role in enumerate(base.ROLE_SPECS):
        role_frames = source_frames[role_index * 4 : role_index * 4 + 4]
        role_masks, scale = rasterize_role(species, role, role_frames)
        role_scales[role.name] = scale
        output_masks.extend(role_masks)

    identity_scale = base.allowed_scale(identity_frame) * 0.985
    identity_mask = base.rasterize(identity_frame, identity_scale)
    idle_similarity = jaccard(identity_mask, output_masks[0])
    # Canonical and Idle sources are generated at different canvas scales, so
    # a strict pixel overlap would reject the same silhouette after nearest-
    # neighbour quantization. 0.30 still rejects unrelated mascots while the
    # source-level visual review owns fine anatomical consistency.
    if idle_similarity < 0.30:
        raise ValueError(
            f"{species}: Idle frame drifted from locked identity "
            f"(Jaccard {idle_similarity:.3f})"
        )

    evidence_dir = private_output / "evidence"
    evidence_dir.mkdir(parents=True, exist_ok=True)
    role_evidence: list[dict[str, object]] = []
    for role_index, role in enumerate(base.ROLE_SPECS):
        role_masks = output_masks[role_index * 4 : role_index * 4 + 4]
        hashes = [base.sha256_bytes(base.frame_bytes(mask)) for mask in role_masks]
        unique_frames = len(set(hashes))
        changed_pixels = len(set().union(*role_masks) - set.intersection(*role_masks))
        if unique_frames < 3:
            raise ValueError(f"{species}/{role.name}: needs at least three unique frames")
        if changed_pixels < 8:
            raise ValueError(
                f"{species}/{role.name}: animation changes only {changed_pixels} pixels"
            )
        similarities = [round(jaccard(identity_mask, mask), 4) for mask in role_masks]
        role_evidence.append(
            {
                "role": role.name,
                "role_id": role.role,
                "mode": ("hold", "once", "loop", "pingpong")[role.mode],
                "durations_ms": list(role.durations_ms),
                "unique_frames": unique_frames,
                "changed_pixels": changed_pixels,
                "identity_jaccard": similarities,
                "frame_sha256": hashes,
            }
        )
        base.write_role_gif(
            role_masks, role, evidence_dir / f"{species}-{role.name}.gif"
        )

    encoded_frames = [base.frame_bytes(mask) for mask in output_masks]
    pack = build_wild_pack(species, definition["display_name"], encoded_frames)
    pack_path = private_output / f"{species}.k868"
    pack_path.write_bytes(pack)
    parsed = validate_pack(pack_path)
    actual_pack_id = f"{parsed.pack_id:08X}"
    if actual_pack_id != definition["pack_id"]:
        raise ValueError(
            f"{species}: expected pack ID {definition['pack_id']}, got {actual_pack_id}"
        )

    contact_path = evidence_dir / f"{species}-48-frame-contact.png"
    base.write_contact_sheet(output_masks, contact_path)
    portrait_mask, portrait_bytes = make_portrait(output_masks[0])
    portrait_path = private_output / "portraits" / f"{species}-16x18.png"
    write_portrait_png(portrait_mask, portrait_path)

    geometry: list[dict[str, object]] = []
    for frame, mask in zip(source_frames, output_masks, strict=True):
        left, top, right, bottom = base.bounds(mask)
        components = base.connected_components(mask)
        geometry.append(
            {
                "role": frame.role.name,
                "phase": frame.phase,
                "bounds": [left, top, right, bottom],
                "body_axis_x": base.median_x(mask),
                "floor_y": bottom,
                "ink_pixels": len(mask),
                "components": len(components),
                "smallest_component_pixels": min(map(len, components)),
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
        "source_sha256": source_hashes,
        "identity_idle_jaccard": round(idle_similarity, 4),
        "role_source_scales": role_scales,
        "contact_sheet": contact_path.name,
        "contact_sheet_sha256": base.sha256_file(contact_path),
        "portrait": {
            "width": PORTRAIT_WIDTH,
            "height": PORTRAIT_HEIGHT,
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


def publish_portraits(
    results: list[dict[str, object]],
    public_portrait_dir: Path,
    public_manifest: Path,
    private_output: Path,
) -> None:
    if len(results) != len(WILD_SPECIES):
        raise ValueError("static portrait publication requires the complete 21-creature roster")
    public_portrait_dir.mkdir(parents=True, exist_ok=True)
    expected_names: set[str] = set()
    records: list[dict[str, object]] = []
    for result in results:
        identity = str(result["identity_key"])
        slug = str(result["slug"])
        portrait = dict(result["portrait"])
        source = private_output / "portraits" / str(portrait["png_file"])
        filename = f"{slug}.{portrait['png_sha256']}.png"
        destination = public_portrait_dir / filename
        destination.write_bytes(source.read_bytes())
        expected_names.add(filename)
        records.append(
            {
                "identity_key": identity,
                "slug": slug,
                "display_name": result["display_name"],
                "rarity": result["rarity"],
                "pack_id": result["pack_id"],
                "pack_bytes": result["pack_bytes"],
                "pack_sha256": result["pack_sha256"],
                "portrait_width": PORTRAIT_WIDTH,
                "portrait_height": PORTRAIT_HEIGHT,
                "portrait_storage": portrait["storage"],
                "portrait_bytes": portrait["bytes"],
                "portrait_bitmap_hex": portrait["bitmap_hex"],
                "portrait_bitmap_base64": portrait["bitmap_base64"],
                "portrait_png": filename,
                "portrait_png_sha256": portrait["png_sha256"],
            }
        )
    unexpected = {
        path.name for path in public_portrait_dir.glob("*.png")
    } - expected_names
    if unexpected:
        raise ValueError(
            "public portrait directory contains files outside the exact roster: "
            + ", ".join(sorted(unexpected))
        )
    public_manifest.parent.mkdir(parents=True, exist_ok=True)
    public_manifest.write_text(
        json.dumps(
            {
                "schema": "kitsu-wild-static-portraits-v1",
                "creatures": records,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )


def parse_args() -> argparse.Namespace:
    project_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--private-output", type=Path, required=True)
    parser.add_argument(
        "--species",
        action="append",
        choices=tuple(WILD_SPECIES),
        help="Build only the named identity for a private checkpoint; repeat as needed.",
    )
    parser.add_argument("--publish-static-portraits", action="store_true")
    parser.add_argument(
        "--public-portrait-dir",
        type=Path,
        default=project_root / "platform" / "public-site" / "unlock" / "portraits",
    )
    parser.add_argument(
        "--public-portrait-manifest",
        type=Path,
        default=project_root / "assets" / "wild-portraits-manifest.json",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    project_root = Path(__file__).resolve().parents[1]
    source_dir = args.source_dir.resolve()
    private_output = args.private_output.resolve()
    require_private_path(source_dir, project_root, "private action source directory")
    require_private_path(private_output, project_root, "private pack output directory")
    private_output.mkdir(parents=True, exist_ok=True)

    selected = list(dict.fromkeys(args.species or WILD_SPECIES.keys()))
    results = [build_species(source_dir, private_output, species) for species in selected]
    manifest = {
        "schema": "kitsu-wild-pack-private-release-v2",
        "identity_keys": selected,
        "complete_roster": selected == list(WILD_SPECIES),
        "public_static_assets_only": True,
        "private_action_assets": True,
        "private_pack_delivery": True,
        "format_security": "ordinary K868PK1 structural checks and CRC32",
        "display_contract": {
            "device": "Heltec WiFi LoRa 32 V3/V3.2",
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
            "source_asset_per_role": True,
        },
        "packs": results,
    }
    manifest_path = private_output / "wild-packs-manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    if args.publish_static_portraits:
        publish_portraits(
            results,
            args.public_portrait_dir.resolve(),
            args.public_portrait_manifest.resolve(),
            private_output,
        )

    for result in results:
        print(
            "WILD_PACK_BUILT "
            f"identity={result['identity_key']} name={result['display_name']} "
            f"rarity={result['rarity']} id={result['pack_id']} "
            f"frames={result['stored_frames']} clips={result['clips']} "
            f"bytes={result['pack_bytes']} sha256={result['pack_sha256']}"
        )
    print(f"PRIVATE_WILD_MANIFEST {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
