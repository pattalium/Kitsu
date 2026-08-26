#!/usr/bin/env python3
"""Synchronize final wild-pack portraits into firmware and Android sources.

The input must be the complete private manifest produced by
``build_wild_packs.py``.  This tool copies only each pack's derived 16x18,
36-byte portrait representation; it never reads or publishes pack bytes or
animation sources.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import re
import tempfile
from dataclasses import dataclass
from pathlib import Path


MANIFEST_SCHEMA = "kitsu-wild-pack-private-release-v2"
EXPECTED_CREATURES = 21
PORTRAIT_WIDTH = 16
PORTRAIT_HEIGHT = 18
PORTRAIT_BYTES = 36
PORTRAIT_STORAGE = "XBM least-significant-bit first, two bytes per row"

FIRMWARE_RELATIVE_PATH = Path("src/wild_creature_catalog.cpp")
ANDROID_RELATIVE_PATH = Path(
    "platform/mobile/android/app/src/main/java/ptl/kitsu/app/ui/"
    "NearbyKitsuPresentation.kt"
)

CATALOG_ENTRY_RE = re.compile(
    r"\{\s*0x(?P<pack_id>[0-9a-fA-F]{8})UL,\s*"
    r'"(?P<name>[A-Za-z][A-Za-z ]+)",\s*'
    r"signal::Rarity::[A-Za-z]+,\s*"
    r"Portrait::(?P<portrait>[A-Za-z][A-Za-z0-9]*),\s*true\s*\}",
    re.MULTILINE,
)


@dataclass(frozen=True)
class PortraitRecord:
    identity_key: str
    pack_id: str
    display_name: str
    bitmap: bytes
    bitmap_base64: str


def parse_args() -> argparse.Namespace:
    project_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description=(
            "Sync the complete private wild-pack portrait manifest into the "
            "firmware and Android static portrait catalogs."
        )
    )
    parser.add_argument("manifest", type=Path, help="complete private manifest")
    parser.add_argument(
        "--project-root",
        type=Path,
        default=project_root,
        help="public source checkout (defaults to this script's repository)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="validate and report drift without writing either target",
    )
    return parser.parse_args()


def require_mapping(value: object, label: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be an object")
    return value


def require_list(value: object, label: str) -> list[object]:
    if not isinstance(value, list):
        raise ValueError(f"{label} must be an array")
    return value


def load_records(manifest_path: Path) -> dict[str, PortraitRecord]:
    try:
        manifest = require_mapping(
            json.loads(manifest_path.read_text(encoding="utf-8")), "manifest"
        )
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read private manifest: {error}") from error

    if manifest.get("schema") != MANIFEST_SCHEMA:
        raise ValueError(f"manifest schema must be {MANIFEST_SCHEMA}")
    if manifest.get("complete_roster") is not True:
        raise ValueError("manifest must declare complete_roster=true")

    identity_keys = require_list(manifest.get("identity_keys"), "identity_keys")
    packs = require_list(manifest.get("packs"), "packs")
    if len(identity_keys) != EXPECTED_CREATURES or len(packs) != EXPECTED_CREATURES:
        raise ValueError(
            f"manifest must contain exactly {EXPECTED_CREATURES} identities and packs"
        )
    if len(set(identity_keys)) != EXPECTED_CREATURES:
        raise ValueError("manifest identity_keys must be unique")

    records: dict[str, PortraitRecord] = {}
    pack_identity_keys: set[str] = set()
    display_names: set[str] = set()
    for index, raw_pack in enumerate(packs):
        pack = require_mapping(raw_pack, f"packs[{index}]")
        identity_key = pack.get("identity_key")
        display_name = pack.get("display_name")
        pack_id = pack.get("pack_id")
        if not isinstance(identity_key, str) or not re.fullmatch(
            r"[a-z][a-z0-9_]*", identity_key
        ):
            raise ValueError(f"packs[{index}] has invalid identity_key")
        owner_private_key = "fox" + "_girl"
        owner_private_name = "Fox" + " Girl"
        if identity_key == owner_private_key or display_name == owner_private_name:
            raise ValueError("owner-private companion must remain absent")
        if not isinstance(display_name, str) or not re.fullmatch(
            r"[A-Za-z][A-Za-z ]+", display_name
        ):
            raise ValueError(f"packs[{index}] has invalid display_name")
        if not isinstance(pack_id, str) or not re.fullmatch(
            r"[0-9A-F]{8}", pack_id
        ):
            raise ValueError(f"packs[{index}] has invalid canonical pack_id")
        if pack.get("format") != "K868PK1":
            raise ValueError(f"{identity_key} is not an ordinary K868PK1 pack")
        if pack.get("stored_frames") != 48 or pack.get("clips") != 12:
            raise ValueError(f"{identity_key} does not contain the complete 48/12 pack")

        portrait = require_mapping(pack.get("portrait"), f"{identity_key}.portrait")
        if (
            portrait.get("width") != PORTRAIT_WIDTH
            or portrait.get("height") != PORTRAIT_HEIGHT
            or portrait.get("bytes") != PORTRAIT_BYTES
            or portrait.get("storage") != PORTRAIT_STORAGE
        ):
            raise ValueError(f"{identity_key} portrait contract is not 16x18 XBM")
        bitmap_hex = portrait.get("bitmap_hex")
        bitmap_base64 = portrait.get("bitmap_base64")
        if not isinstance(bitmap_hex, str) or not re.fullmatch(
            rf"[0-9a-f]{{{PORTRAIT_BYTES * 2}}}", bitmap_hex
        ):
            raise ValueError(f"{identity_key} has invalid portrait bitmap_hex")
        if not isinstance(bitmap_base64, str):
            raise ValueError(f"{identity_key} has invalid portrait bitmap_base64")
        bitmap = bytes.fromhex(bitmap_hex)
        try:
            decoded = base64.b64decode(bitmap_base64, validate=True)
        except ValueError as error:
            raise ValueError(
                f"{identity_key} portrait bitmap_base64 is malformed"
            ) from error
        if decoded != bitmap:
            raise ValueError(
                f"{identity_key} portrait hex/Base64 values describe different bytes"
            )
        if pack_id in records:
            raise ValueError(f"duplicate pack_id in manifest: {pack_id}")
        if display_name in display_names:
            raise ValueError(f"duplicate display_name in manifest: {display_name}")

        pack_identity_keys.add(identity_key)
        display_names.add(display_name)
        records[pack_id] = PortraitRecord(
            identity_key=identity_key,
            pack_id=pack_id,
            display_name=display_name,
            bitmap=bitmap,
            bitmap_base64=bitmap_base64,
        )

    if pack_identity_keys != set(identity_keys):
        raise ValueError("manifest identity_keys do not match its packs")
    return records


def parse_firmware_catalog(
    source: str,
) -> dict[str, tuple[str, str]]:
    block_match = re.search(
        r"constexpr Creature kCreatures\[\]\s*=\s*\{(?P<body>.*?)\n\};",
        source,
        re.DOTALL,
    )
    if block_match is None:
        raise ValueError("cannot locate firmware wild-creature catalog")

    entries: dict[str, tuple[str, str]] = {}
    for match in CATALOG_ENTRY_RE.finditer(block_match.group("body")):
        pack_id = match.group("pack_id").upper()
        if pack_id in entries:
            raise ValueError(f"duplicate firmware pack_id: {pack_id}")
        entries[pack_id] = (match.group("name"), match.group("portrait"))
    if len(entries) != EXPECTED_CREATURES:
        raise ValueError(
            f"firmware catalog must contain exactly {EXPECTED_CREATURES} entries"
        )
    owner_private_name = "Fox" + " Girl"
    if any(name == owner_private_name for name, _ in entries.values()):
        raise ValueError("owner-private companion must remain absent from the firmware catalog")
    return entries


def require_matching_roster(
    records: dict[str, PortraitRecord],
    catalog: dict[str, tuple[str, str]],
) -> None:
    if set(records) != set(catalog):
        missing = sorted(set(catalog) - set(records))
        extra = sorted(set(records) - set(catalog))
        raise ValueError(f"manifest/catalog pack IDs differ: missing={missing} extra={extra}")
    for pack_id, record in records.items():
        expected_name = catalog[pack_id][0]
        if record.display_name != expected_name:
            raise ValueError(
                f"pack {pack_id} name mismatch: manifest={record.display_name!r} "
                f"firmware={expected_name!r}"
            )


def format_firmware_array(name: str, bitmap: bytes) -> str:
    rows = []
    for offset in range(0, len(bitmap), 10):
        chunk = bitmap[offset : offset + 10]
        rows.append("    " + ", ".join(f"0x{value:02x}" for value in chunk) + ",")
    return (
        f"constexpr uint8_t {name}[kPortraitBytes] = {{\n"
        + "\n".join(rows)
        + "\n};"
    )


def render_firmware(
    source: str,
    records: dict[str, PortraitRecord],
    catalog: dict[str, tuple[str, str]],
) -> str:
    rendered = source
    for pack_id, record in records.items():
        portrait_enum = catalog[pack_id][1]
        array_name = f"k{portrait_enum}Portrait"
        pattern = re.compile(
            rf"constexpr uint8_t {re.escape(array_name)}\[kPortraitBytes\]"
            rf"\s*=\s*\{{.*?\n\}};",
            re.DOTALL,
        )
        rendered, replacements = pattern.subn(
            format_firmware_array(array_name, record.bitmap), rendered
        )
        if replacements != 1:
            raise ValueError(
                f"expected one firmware portrait array {array_name}; found {replacements}"
            )
    return rendered


def render_android(
    source: str,
    records: dict[str, PortraitRecord],
) -> str:
    rendered = source
    discovered = {
        match.group("pack_id").upper(): match.group("name")
        for match in re.finditer(
            r"0x(?P<pack_id>[0-9A-Fa-f]{8})L\s+to\s+portrait\(\s*"
            r'"(?P<name>[A-Za-z][A-Za-z ]+)",\s*16,\s*18,',
            source,
        )
    }
    if discovered != {key: value.display_name for key, value in records.items()}:
        raise ValueError("Android 16x18 portrait roster differs from the manifest")

    for pack_id, record in records.items():
        pattern = re.compile(
            rf"(?m)^(?P<indent>[ \t]*)0x{pack_id}L\s+to\s+portrait\(\s*"
            rf'"{re.escape(record.display_name)}",\s*16,\s*18,\s*'
            rf'"[A-Za-z0-9+/=]+"\s*\),\s*$'
        )

        def replacement(match: re.Match[str]) -> str:
            return (
                f'{match.group("indent")}0x{pack_id}L to portrait('
                f'"{record.display_name}", 16, 18, "{record.bitmap_base64}"),'
            )

        rendered, replacements = pattern.subn(replacement, rendered)
        if replacements != 1:
            raise ValueError(
                f"expected one Android portrait entry for {pack_id}; found {replacements}"
            )
    return rendered


def atomic_write(path: Path, content: str) -> None:
    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        newline="\n",
        dir=path.parent,
        prefix=f".{path.name}.",
        suffix=".tmp",
        delete=False,
    ) as temporary:
        temporary.write(content)
        temporary_path = Path(temporary.name)
    try:
        os.replace(temporary_path, path)
    except BaseException:
        temporary_path.unlink(missing_ok=True)
        raise


def main() -> int:
    args = parse_args()
    project_root = args.project_root.resolve()
    manifest_path = args.manifest.resolve()
    firmware_path = project_root / FIRMWARE_RELATIVE_PATH
    android_path = project_root / ANDROID_RELATIVE_PATH

    records = load_records(manifest_path)
    firmware_source = firmware_path.read_text(encoding="utf-8")
    android_source = android_path.read_text(encoding="utf-8")
    catalog = parse_firmware_catalog(firmware_source)
    require_matching_roster(records, catalog)

    rendered_firmware = render_firmware(firmware_source, records, catalog)
    rendered_android = render_android(android_source, records)
    changed = [
        label
        for label, before, after in (
            ("firmware", firmware_source, rendered_firmware),
            ("android", android_source, rendered_android),
        )
        if before != after
    ]

    if args.check:
        print(
            "WILD_PORTRAIT_SYNC_CHECK "
            f"records={len(records)} drift={','.join(changed) if changed else 'none'}"
        )
        return 1 if changed else 0

    if firmware_source != rendered_firmware:
        atomic_write(firmware_path, rendered_firmware)
    if android_source != rendered_android:
        atomic_write(android_path, rendered_android)
    print(
        "WILD_PORTRAIT_SYNCED "
        f"records={len(records)} changed={','.join(changed) if changed else 'none'}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
