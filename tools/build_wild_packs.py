#!/usr/bin/env python3
"""Build private Kitsu wild-creature packs from identity-locked format-v2 art.

Every creature owns one approved canonical identity image, an independently
authored exact 16x18 portrait, and four independent frames for each of twelve
actions. Direct sources must already be exact one-bit 64x80 release rasters.
ImageGen sources use one manifest-pinned identity transform for the entire
creature, one pre-frozen native change mask per phase, and deterministic
baseline-outside/candidate-inside composition. Role-base phases 1..3 are
independent edits of the same accepted phase 0; no phase is chained from a
previous generated edit and no per-frame fitting or cleanup exists.

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
WILD_PACK_FORMAT_VERSION = 2
WILD_FRAME_WIDTH = 64
WILD_FRAME_HEIGHT = 80
WILD_FRAME_BYTES = WILD_FRAME_WIDTH * WILD_FRAME_HEIGHT // 8
WILD_FLOOR_Y = 77
WILD_PACK_REVISION = 3
LEGACY_PRIVATE_MANIFEST_SCHEMA = "kitsu-wild-pack-private-release-v6"
PRIVATE_MANIFEST_SCHEMA = "kitsu-wild-pack-private-release-v7"
DIRECT_RASTER_TRANSFORM = "none-direct-exact-target"
IMAGEGEN_RASTER_TRANSFORM = (
    "rgba-over-white-box-area-fixed-role-registration-bounded-native-composite-v1"
)
TRANSFORM_CONTROLS = {
    "auto_fit": False,
    "crop_by_subject": False,
    "cleanup": False,
    "per_frame_translation": False,
    "per_frame_threshold": False,
}


def is_within(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def require_private_path(path: Path, project_root: Path, label: str) -> None:
    if is_within(path, project_root):
        raise ValueError(f"{label} must stay outside the public checkout: {path}")


def load_release_identity_locks(
    path: Path, selected: list[str]
) -> tuple[
    str,
    str,
    dict[
        str,
        raster_contract.HighResIdentityLock
        | raster_contract.ImageGenImportLock,
    ],
]:
    """Select exactly one v2 source contract without legacy reinterpretation."""

    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read private identity lock: {error}") from error
    if not isinstance(payload, dict):
        raise ValueError("private identity lock must be a JSON object")
    schema = payload.get("schema")
    if schema == raster_contract.HIGH_RES_IDENTITY_LOCK_SCHEMA:
        return (
            "direct-exact-target",
            schema,
            raster_contract.load_high_res_identity_locks(path, selected),
        )
    if schema in {
        raster_contract.LEGACY_IMAGEGEN_IMPORT_LOCK_SCHEMA,
        raster_contract.IMAGEGEN_IMPORT_LOCK_SCHEMA,
    }:
        return (
            "imagegen-locked-import",
            schema,
            raster_contract.load_imagegen_import_locks(path, selected),
        )
    raise ValueError(
        "identity lock must use the exact format-v2 direct-target or "
        "ImageGen import schema; legacy v1 locks and unsafe ImageGen v2/v3 "
        "action locks are forbidden"
    )


def build_wild_pack(species: str, display_name: str, frames: list[bytes]) -> bytes:
    """Build a native 64x80 K868PK1 v2 pack at content revision three."""

    pack = bytearray(
        base.build_pack(
            species,
            display_name,
            frames,
            format_version=WILD_PACK_FORMAT_VERSION,
            frame_width=WILD_FRAME_WIDTH,
            frame_height=WILD_FRAME_HEIGHT,
        )
    )
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
    identity_lock: (
        raster_contract.HighResIdentityLock
        | raster_contract.ImageGenImportLock
    ),
    source_kind: str,
) -> dict[str, object]:
    definition = WILD_SPECIES[species]
    if source_kind == "direct-exact-target":
        if not isinstance(identity_lock, raster_contract.HighResIdentityLock):
            raise ValueError(f"{species}: direct build requires a v2 identity lock")
        raster = raster_contract.load_high_res_species(
            source_dir, species, identity_lock
        )
        raster_transform = DIRECT_RASTER_TRANSFORM
        identity_raster_scale = 1.0
        action_cell_raster_scale = 1.0
        action_source_layout = None
        action_source_layout_sha256 = None
        action_output_offset = None
        lock_record: dict[str, object] = {
            "schema": raster_contract.HIGH_RES_IDENTITY_LOCK_SCHEMA,
            "identity_sha256": identity_lock.identity_sha256,
            "identity_frame_sha256": base.sha256_bytes(raster.identity.packed),
            "frame_canvas": list(identity_lock.frame_canvas),
        }
    elif source_kind == "imagegen-locked-import":
        if not isinstance(identity_lock, raster_contract.ImageGenImportLock):
            raise ValueError(
                f"{species}: ImageGen build requires an ImageGen import lock"
            )
        # Independent full-canvas frames use output_offset for identity and
        # actions alike. The legacy-reserved action_output_offset field is
        # hash-bound but never consumed by the v4/v5 builder.
        raster = raster_contract.load_high_res_generated_species(
            source_dir, species, identity_lock
        )
        raster_transform = IMAGEGEN_RASTER_TRANSFORM
        crop_width = (
            identity_lock.transform.crop_rect[2]
            - identity_lock.transform.crop_rect[0]
        )
        identity_raster_scale = WILD_FRAME_WIDTH / crop_width
        action_cell_raster_scale = identity_raster_scale
        action_source_layout = None
        action_source_layout_sha256 = None
        action_output_offset = None
        lock_record = {
            "schema": identity_lock.schema,
            "action_semantic_contract": (
                raster_contract.generated_action_semantic_contract_record(
                    identity_lock.action_semantic_contract
                )
            ),
            "action_semantic_contract_sha256": (
                identity_lock.action_semantic_contract_sha256
            ),
            "identity_source_sha256": identity_lock.identity_source_sha256,
            "identity_frame_sha256": identity_lock.identity_frame_sha256,
            "transform": raster_contract.imagegen_import_transform_record(
                identity_lock.transform
            ),
            "transform_sha256": identity_lock.transform_sha256,
        }
    elif source_kind == "imagegen-one-action-sheets":
        raise ValueError(
            f"{species}: ImageGen import v4/v5 forbids one-action sheets; four "
            "independent full-canvas phase edits are required"
        )
    else:
        raise ValueError(f"{species}: unsupported private source kind {source_kind!r}")

    raster_frames = list(raster.frames)
    if len(raster_frames) != len(base.ROLE_SPECS) * 4:
        raise ValueError(
            f"{species}: expected 48 exact release frames, got {len(raster_frames)}"
        )
    output_masks = [set(frame.mask) for frame in raster_frames]
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
        if [frame.role for frame in role_rasters] != [role.name] * 4 or [
            frame.phase for frame in role_rasters
        ] != list(range(4)):
            raise ValueError(f"{species}/{role.name}: non-canonical frame order")
        hashes = [base.sha256_bytes(frame.packed) for frame in role_rasters]
        unique_frames = len(set(hashes))
        if unique_frames != 4:
            raise ValueError(
                f"{species}/{role.name}: distinct sources collapsed to "
                f"{unique_frames} final 64x80 frames"
            )
        changed_pixels = len(set().union(*role_masks) - set.intersection(*role_masks))
        similarities = [round(frame.identity_jaccard, 4) for frame in role_rasters]
        role_record: dict[str, object] = {
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
            "final_mask_sha256": [
                raster_contract.mask_sha256(
                    frame.mask, WILD_FRAME_WIDTH, WILD_FRAME_HEIGHT
                )
                for frame in role_rasters
            ],
            "frame_sha256": hashes,
        }
        role_record["source_sha256"] = [
            frame.source_sha256 for frame in role_rasters
        ]
        if source_kind == "imagegen-locked-import":
            if raster.generated_semantic_evidence is None:
                raise ValueError(
                    f"{species}/{role.name}: generated semantic evidence is missing"
                )
            role_record["semantic_locality"] = (
                raster.generated_semantic_evidence[role.name]
            )
        role_evidence.append(role_record)
        base.write_role_gif(
            role_masks,
            role,
            evidence_dir / f"{species}-{role.name}.gif",
            WILD_FRAME_WIDTH,
            WILD_FRAME_HEIGHT,
        )

    encoded_frames: list[bytes] = []
    for index, (frame, mask) in enumerate(
        zip(raster_frames, output_masks, strict=True)
    ):
        if len(frame.packed) != WILD_FRAME_BYTES:
            raise ValueError(
                f"{species}: frame {index} has {len(frame.packed)} packed bytes"
            )
        if raster_contract.decode_high_res_frame_bytes(frame.packed) != mask:
            raise ValueError(f"{species}: frame {index} packing changed pixels")
        encoded_frames.append(frame.packed)

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
    if (
        parsed.format_version != WILD_PACK_FORMAT_VERSION
        or (parsed.width, parsed.height) != (WILD_FRAME_WIDTH, WILD_FRAME_HEIGHT)
        or parsed.total_bytes != 31_120
    ):
        raise ValueError(f"{species}: serialized pack is not exact K868PK1 v2")

    contact_path = evidence_dir / f"{species}-48-frame-contact.png"
    base.write_contact_sheet(
        output_masks, contact_path, WILD_FRAME_WIDTH, WILD_FRAME_HEIGHT
    )
    portrait_bytes = raster.portrait.packed
    if len(portrait_bytes) != PORTRAIT_BYTES:
        raise ValueError(
            f"{species}: portrait has {len(portrait_bytes)} bytes, expected 36"
        )
    portrait_path = private_output / "portraits" / f"{species}-16x18.png"
    portrait_path.parent.mkdir(parents=True, exist_ok=True)
    with raster.portrait.path.open("rb") as source, portrait_path.open("xb") as output:
        shutil.copyfileobj(source, output, length=1024 * 1024)
    if base.sha256_file(portrait_path) != raster.portrait.source_sha256:
        raise ValueError(f"{species}: exact 16x18 portrait copy changed bytes")

    geometry: list[dict[str, object]] = []
    for raster_frame, mask in zip(raster_frames, output_masks, strict=True):
        left, top, right, bottom = base.bounds(mask)
        components = base.connected_components(mask)
        geometry.append(
            {
                "role": raster_frame.role,
                "phase": raster_frame.phase,
                "bounds": [left, top, right, bottom],
                "body_axis_x": base.median_x(mask),
                "floor_y": bottom,
                "ink_pixels": len(mask),
                "components": len(components),
                "smallest_component_pixels": min(map(len, components)),
                "source_bounds": list(raster_frame.metrics.bounds),
                "source_components": raster_frame.metrics.components,
                "source_primary_fraction": round(
                    raster_frame.metrics.primary_fraction, 4
                ),
            }
        )

    result = {
        "identity_key": species,
        "slug": definition["slug"],
        "display_name": definition["public_name"],
        "pack_display_name": definition["display_name"],
        "rarity": definition["rarity"],
        "pack_id": actual_pack_id,
        "revision": WILD_PACK_REVISION,
        "format": "K868PK1",
        "format_version": WILD_PACK_FORMAT_VERSION,
        "frame_canvas": [WILD_FRAME_WIDTH, WILD_FRAME_HEIGHT],
        "stored_frames": len(output_masks),
        "clips": len(base.ROLE_SPECS),
        "steps": len(base.ROLE_SPECS) * 4,
        "pack_file": pack_path.name,
        "pack_bytes": len(pack),
        "pack_sha256": base.sha256_bytes(pack),
        "source_sha256": raster.source_sha256,
        "source_kind": source_kind,
        "identity_lock": lock_record,
        "identity_idle_jaccard": round(idle_similarity, 4),
        "fixed_action_scale": raster.fixed_action_scale,
        "identity_raster_scale": identity_raster_scale,
        "action_cell_raster_scale": action_cell_raster_scale,
        "raster_transform": raster_transform,
        "transform_controls": dict(TRANSFORM_CONTROLS),
        "role_source_scales": role_scales,
        "contact_sheet": contact_path.name,
        "contact_sheet_sha256": base.sha256_file(contact_path),
        "portrait": {
            "width": PORTRAIT_WIDTH,
            "height": PORTRAIT_HEIGHT,
            "source": "independently-authored-exact-16x18",
            "source_sha256": raster.portrait.source_sha256,
            "resampling": "none",
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
    if action_source_layout is not None:
        result["action_source_layout"] = action_source_layout
        result["action_source_layout_sha256"] = action_source_layout_sha256
        result["action_output_offset"] = action_output_offset
    return result


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
    """Copy every hash-pinned source/preauthorization byte without overwrite."""

    destination_dir = staging_output / "source-snapshot" / species
    destination_dir.mkdir(parents=True, exist_ok=False)
    copied: dict[str, str] = {}
    for filename, expected_hash in sorted(expected_hashes.items()):
        relative = Path(filename)
        if (
            relative.is_absolute()
            or filename != relative.as_posix()
            or any(part in {"", ".", ".."} for part in relative.parts)
        ):
            raise ValueError(f"{species}: invalid source snapshot path {filename!r}")
        source_root = (source_dir / species).resolve()
        source = (source_root / relative).resolve()
        if not is_within(source, source_root) or not source.is_file():
            raise ValueError(f"{species}/{filename}: source snapshot file is invalid")
        destination = destination_dir / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
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
            "Private kitsu-wild-identity-lock-v2 direct-target file or "
            "kitsu-wild-imagegen-import-lock-v4/v5 file. The schemas are "
            "distinct; audited v4 remains two-reference and v5 records each "
            "optional Image 3 explicitly."
        ),
    )
    parser.add_argument(
        "--species",
        action="append",
        choices=tuple(WILD_SPECIES),
        help="Build only the named identity for a private checkpoint; repeat as needed.",
    )
    parser.add_argument(
        "--imagegen-source-layout",
        choices=("independent-frames", "one-action-sheets"),
        default="independent-frames",
        help=(
            "Explicit ImageGen source tree. v4/v5 accepts four independent "
            "full-canvas files per action; the legacy sheet choice is retained "
            "only so it can fail with an explicit migration diagnostic."
        ),
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
    source_kind, identity_lock_schema, identity_locks = (
        load_release_identity_locks(identity_lock_path, selected)
    )
    if args.imagegen_source_layout == "one-action-sheets":
        raise ValueError(
            "ImageGen import v4/v5 forbids one-action sheets; author four "
            "independent full-canvas edits with frozen per-phase masks"
        )
    private_output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=f".{private_output.name}.staging-", dir=private_output.parent
    ) as temporary:
        staging_output = Path(temporary)
        results: list[dict[str, object]] = []
        for species in selected:
            result = build_species(
                source_dir,
                staging_output,
                species,
                identity_locks[species],
                source_kind,
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
            "schema": PRIVATE_MANIFEST_SCHEMA,
            "identity_keys": selected,
            "complete_roster": selected == list(WILD_SPECIES),
            "public_static_assets_only": True,
            "private_action_assets": True,
            "private_pack_delivery": True,
            "format_security": "ordinary K868PK1 structural checks and CRC32",
            "identity_lock_schema": identity_lock_schema,
            "identity_lock_sha256": base.sha256_file(identity_lock_path),
            "non_destructive_build": True,
            "display_contract": {
                "device": "Heltec WiFi LoRa 32 V3/V3.2",
                "oled_pixels": [64, 128],
                "pack_format_version": WILD_PACK_FORMAT_VERSION,
                "pack_frame_pixels": [WILD_FRAME_WIDTH, WILD_FRAME_HEIGHT],
                "frame_bytes": WILD_FRAME_BYTES,
                "body_axis_x": raster_contract.HIGH_RES_BODY_AXIS_X,
                "floor_y": WILD_FLOOR_Y,
                "safe_bounds": [
                    raster_contract.HIGH_RES_SAFE_LEFT,
                    raster_contract.HIGH_RES_SAFE_TOP,
                    raster_contract.HIGH_RES_SAFE_RIGHT,
                    WILD_FLOOR_Y,
                ],
                "bottom_guard_rows": list(
                    raster_contract.HIGH_RES_BOTTOM_GUARD_ROWS
                ),
            },
            "raster_contract": {
                "allowed_pack_transforms": [
                    DIRECT_RASTER_TRANSFORM,
                    IMAGEGEN_RASTER_TRANSFORM,
                ],
                **TRANSFORM_CONTROLS,
                "source_snapshots": True,
                "portrait_source": "independently-authored-exact-16x18",
                "portrait_resampling": "none",
            },
            "animation_contract": {
                "roles": [role.name for role in base.ROLE_SPECS],
                "stored_frames_per_role": 4,
                "required_unique_frames_per_role": 4,
                "required_changed_pixels_per_role": 16,
                "source_asset_layouts": [
                    "one-independent-file-per-phase",
                ],
                "independent_final_frame_per_phase": True,
                "generated_action_semantic_schema": (
                    raster_contract.GENERATED_ACTION_SEMANTIC_SCHEMA
                ),
                "generated_action_semantic_schemas": [
                    raster_contract.LEGACY_GENERATED_ACTION_SEMANTIC_SCHEMA,
                    raster_contract.GENERATED_ACTION_SEMANTIC_SCHEMA,
                ],
                "immutable_identity_reference_per_generated_phase": True,
                "identity_reference_image_number": 1,
                "immutable_edit_target_reference_per_generated_phase": True,
                "edit_target_reference_image_number": 2,
                "generation_reference_modes": [
                    raster_contract.P0_GENERATION_REFERENCE_MODE,
                    raster_contract.TWO_REFERENCE_GENERATION_MODE,
                    raster_contract.THREE_REFERENCE_GENERATION_MODE,
                ],
                "native_grid_reference_schema": (
                    raster_contract.NATIVE_GRID_REFERENCE_SCHEMA
                ),
                "native_grid_reference_image_number": 3,
                "native_grid_reference_prompt_only": True,
                "native_grid_reference_phase_0": False,
                "native_grid_reference_per_phase_truthful": True,
                "native_grid_reference_same_role_p0_for_all_uses": True,
                "native_grid_reference_zero_registration_exact_copy_only": True,
                "native_grid_reference_derivation": (
                    raster_contract.NATIVE_GRID_REFERENCE_DERIVATION
                ),
                "generated_phase_chaining": False,
                "identity_anchored_later_phases_use_role_p0_edit_target": True,
                "role_phase_0_generation_target": "approved-identity",
                "role_phase_0_exact_identity_baseline_copy_without_generation": (
                    True
                ),
                "identity_baseline_copy_requires_byte_exact_source_and_zero_registration": (
                    True
                ),
                "role_phase_1_to_3_generation_target": (
                    "same-immutable-accepted-role-phase-0"
                ),
                "role_phase_0_is_generation_reference_for_role_phases_1_to_3": True,
                "role_baseline_policy": dict(
                    raster_contract.GENERATED_ROLE_BASELINE_POLICY
                ),
                "role_contact_policy_defaults": dict(
                    raster_contract.GENERATED_ROLE_CONTACT_POLICY_DEFAULTS
                ),
                "role_contact_policy_capabilities": {
                    role: sorted(capabilities)
                    for role, capabilities in (
                        raster_contract.GENERATED_ROLE_CONTACT_POLICY_CAPABILITIES.items()
                    )
                },
                "preauthorization_schema": (
                    raster_contract.GENERATED_PHASE_PREAUTHORIZATION_SCHEMA
                ),
                "phase_mask_frozen_before_generation": True,
                "exact_allowed_change_masks": True,
                "bounded_composition_mode": (
                    raster_contract.GENERATED_COMPOSITION_MODE
                ),
                "role_registration_schema": (
                    raster_contract.GENERATED_ROLE_REGISTRATION_SCHEMA
                ),
                "maximum_absolute_role_output_offset_pixels": (
                    raster_contract.GENERATED_ROLE_OUTPUT_OFFSET_MAXIMUM
                ),
                "per_phase_registration_override": False,
                "production_out_of_region_pixel_budget": 0,
                "zero_tolerance_frozen_contact_and_anatomy_masks": True,
                "role_motion_landmark_proof": True,
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
