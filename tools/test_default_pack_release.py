#!/usr/bin/env python3
"""Fail-closed release tests for the public Cat/Fox/Dog K868 packs."""

from __future__ import annotations

import binascii
import hashlib
import json
import struct
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PACK_DIR = ROOT / "assets" / "packs"
SOURCE_DIR = ROOT / "assets" / "companion-sources"
EVIDENCE_DIR = ROOT / "assets" / "pack-evidence"
MAGIC = b"K868PK1\0"
APPROVED_PACKS = {
    "cat": {
        "name": "CAT",
        "id": 0xFDC79D6F,
        "sha256": "8d19d6b8bc584d9aaee5a6867504fd23c1862c907bbeb1affd9611e35bf2a6d7",
    },
    "fox": {
        "name": "FOX",
        "id": 0x6C393E21,
        "sha256": "c868386770b6083dcd8f7c01ec7fe455faec476a96c724ab62f09770fdcdab38",
    },
    "dog": {
        "name": "DOG",
        "id": 0xE2B5E7BA,
        "sha256": "8652aad28816d52fca334766ebefb5c38aec1b09dcc72783414998d17a46e261",
    },
}
EXPECTED_ROLES = tuple(range(12))
FRAME_WIDTH = 64
FRAME_HEIGHT = 64
FRAME_BYTES = 512

def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def decoded_frame(frame: bytes) -> set[tuple[int, int]]:
    require(len(frame) == FRAME_BYTES, "frame size is not 512 bytes")
    return {
        (x, y)
        for y in range(FRAME_HEIGHT)
        for x in range(FRAME_WIDTH)
        if frame[y * 8 + x // 8] & (1 << (x & 7))
    }


def median_x(points: set[tuple[int, int]]) -> float:
    xs = sorted(x for x, unused_y in points)
    middle = len(xs) // 2
    return (
        float(xs[middle])
        if len(xs) % 2
        else (xs[middle - 1] + xs[middle]) / 2
    )


def inspect_pack(path: Path, approved: dict[str, object]) -> dict[str, object]:
    pack = path.read_bytes()
    require(pack[:8] == MAGIC, f"{path.name}: bad magic")
    (
        version,
        header_bytes,
        total_bytes,
        payload_crc,
        header_crc,
        pack_id,
        revision,
        width,
        height,
        frame_count,
        clip_count,
        step_count,
        flags,
        encoded_name,
    ) = struct.unpack_from("<HHIIIIIHHHHII16s", pack, 8)
    name = encoded_name.split(b"\0", 1)[0].decode("ascii")
    require(version == 1 and header_bytes == 64, f"{path.name}: bad format")
    require(total_bytes == len(pack), f"{path.name}: bad declared length")
    require((width, height) == (64, 64), f"{path.name}: bad frame canvas")
    require(frame_count == 48, f"{path.name}: must store 48 frames")
    require(clip_count == 12, f"{path.name}: must store 12 clips")
    require(step_count == 48, f"{path.name}: must store 48 steps")
    require(revision >= 2, f"{path.name}: advanced pack revision is required")
    require(flags == 0, f"{path.name}: bad header flags")
    require(pack_id == approved["id"], f"{path.name}: unapproved pack ID")
    require(name == approved["name"], f"{path.name}: unexpected display name {name!r}")
    require(
        binascii.crc32(pack[64:]) & 0xFFFFFFFF == payload_crc,
        f"{path.name}: payload CRC mismatch",
    )
    header_copy = bytearray(pack[8:64])
    header_copy[12:16] = b"\0\0\0\0"
    require(
        binascii.crc32(header_copy) & 0xFFFFFFFF == header_crc,
        f"{path.name}: header CRC mismatch",
    )
    require(sha256(path) == approved["sha256"], f"{path.name}: unapproved payload")

    clips_offset = 64
    steps_offset = clips_offset + clip_count * 12
    frames_offset = steps_offset + step_count * 4
    roles = []
    for index in range(clip_count):
        role, variant, mode, weight, first_step, count, reserved = struct.unpack_from(
            "<BBBBIHH", pack, clips_offset + index * 12
        )
        require(0 <= mode <= 3, f"{path.name}: invalid mode")
        require(variant == 0 and weight > 0 and reserved == 0, f"{path.name}: bad clip")
        require(count == 4, f"{path.name}: every role needs four stored steps")
        require(first_step == index * 4, f"{path.name}: non-canonical step table")
        roles.append(role)
    require(tuple(roles) == EXPECTED_ROLES, f"{path.name}: incomplete role table")

    frames = [
        pack[frames_offset + index * FRAME_BYTES : frames_offset + (index + 1) * FRAME_BYTES]
        for index in range(frame_count)
    ]
    for role in EXPECTED_ROLES:
        role_frames = frames[role * 4 : role * 4 + 4]
        require(len(set(role_frames)) >= 3, f"{path.name}: role {role} lacks animation")
        masks = [decoded_frame(frame) for frame in role_frames]
        changed = len(set().union(*masks) - set.intersection(*masks))
        require(changed >= 8, f"{path.name}: role {role} motion is too small")
        for phase, mask in enumerate(masks):
            require(mask, f"{path.name}: empty frame {role}/{phase}")
            xs = [x for x, unused_y in mask]
            ys = [y for unused_x, y in mask]
            require(min(xs) >= 2 and max(xs) <= 61, f"{path.name}: horizontal clipping")
            require(min(ys) >= 2 and max(ys) == 61, f"{path.name}: floor alignment")
            require(
                31.5 <= median_x(mask) <= 32.5,
                f"{path.name}: body-axis misalignment in {role}/{phase}",
            )

    return {
        "name": name,
        "pack_id": f"{pack_id:08X}",
        "revision": revision,
        "sha256": sha256(path),
        "bytes": len(pack),
    }


def test_release_boundary() -> None:
    actual_packs = {path.stem for path in PACK_DIR.glob("*.k868")}
    require(actual_packs == set(APPROVED_PACKS), f"release pack set is {sorted(actual_packs)}")
    actual_sources = {
        path.stem.rsplit("-", 1)[0] for path in SOURCE_DIR.glob("*.png")
    }
    require(actual_sources == set(APPROVED_PACKS), f"source species set is {sorted(actual_sources)}")
    for species in APPROVED_PACKS:
        for sheet in ("core", "life", "social"):
            require((SOURCE_DIR / f"{species}-{sheet}.png").is_file(), "missing sheet")
        require((EVIDENCE_DIR / f"{species}-48-frame-contact.png").is_file(), "missing contact")
        for role in (
            "idle", "blink", "pet", "sleep", "listen", "surprise",
            "play", "tired", "feed", "wake", "meet", "evolve",
        ):
            require((EVIDENCE_DIR / f"{species}-{role}.gif").is_file(), "missing GIF")


def test_manifest(results: list[dict[str, object]]) -> None:
    manifest_path = PACK_DIR / "default-packs-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    require(manifest["schema"] == "kitsu-default-pack-release-v1", "bad schema")
    require(manifest["public_default_species"] == list(APPROVED_PACKS), "bad allow-list")
    require(manifest["brand_mascot_is_not_a_pack"] is True, "mascot boundary missing")
    require(manifest["private_companions_included"] is False, "privacy marker missing")
    require(
        [item["display_name"] for item in manifest["packs"]]
        == [approved["name"] for approved in APPROVED_PACKS.values()],
        "manifest pack names are not CAT/FOX/DOG",
    )
    by_name = {result["name"]: result for result in results}
    for item in manifest["packs"]:
        result = by_name[item["display_name"]]
        require(item["pack_sha256"] == result["sha256"], "manifest hash mismatch")
        require(len(item["roles"]) == 12 and len(item["geometry"]) == 48, "evidence incomplete")


def test_public_literal_boundary() -> None:
    # Construct the non-public label so the label itself is absent from this
    # public test source while the scan remains explicit and fail-closed.
    animal = "".join(chr(value) for value in (102, 111, 120))
    person = "".join(chr(value) for value in (103, 105, 114, 108))
    forbidden = {
        f"{animal}-{person}".encode(),
        f"{animal}_{person}".encode(),
        f"{animal} {person}".encode(),
    }
    text_suffixes = {
        ".md", ".py", ".cjs", ".js", ".mjs", ".ts", ".tsx", ".kt",
        ".kts", ".cpp", ".h", ".json", ".txt", ".html", ".css", ".toml",
    }
    offenders = []
    for path in ROOT.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in text_suffixes:
            continue
        if any(part in {".git", "node_modules", "build", ".gradle"} for part in path.parts):
            continue
        lowered = path.read_bytes().lower()
        if any(token in lowered for token in forbidden):
            offenders.append(path.relative_to(ROOT).as_posix())
    require(not offenders, "non-public companion label found in: " + ", ".join(offenders))


def main() -> int:
    try:
        test_release_boundary()
        results = [
            inspect_pack(PACK_DIR / f"{species}.k868", approved)
            for species, approved in APPROVED_PACKS.items()
        ]
        test_manifest(results)
        test_public_literal_boundary()
    except (AssertionError, OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"DEFAULT_PACK_RELEASE_FAIL {error}", file=sys.stderr)
        return 1
    for result in results:
        print(
            "DEFAULT_PACK_RELEASE_PASS "
            f"name={result['name']} id={result['pack_id']} revision={result['revision']} "
            f"bytes={result['bytes']} sha256={result['sha256']}"
        )
    print("DEFAULT_PACK_PRIVACY_PASS allowlist=CAT,FOX,DOG exact_ids_and_hashes=pinned")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
