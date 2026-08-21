#!/usr/bin/env python3
"""Compatibility validator for one public K868 default pack.

Full 48-frame geometry, timing, motion, source, and privacy checks live in
``test_default_pack_release.py``. This entry point preserves the earlier
single-pack CLI while applying the same structural and alignment contract.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

from test_default_pack_release import APPROVED_PACKS, inspect_pack

PACK_PARTITION_BYTES = 0x140000


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--expect-name")
    parser.add_argument("--zip-member")
    parser.add_argument("--array")
    parser.add_argument("--gif", type=Path)
    parser.add_argument("--artifacts-dir", type=Path)
    args = parser.parse_args()
    if args.zip_member or args.array:
        parser.error("header/ZIP modes were retired; validate a public .k868 file")
    source = args.source.resolve()
    if not source.is_file():
        parser.error(f"pack does not exist: {source}")
    if source.stat().st_size > PACK_PARTITION_BYTES:
        parser.error(f"pack exceeds {PACK_PARTITION_BYTES}-byte partition")
    raw = source.read_bytes()
    if raw[:8] != b"K868PK1\0" or len(raw) < 64:
        parser.error("source is not a K868PK1 pack")
    encoded_name = struct.unpack_from("<16s", raw, 0x30)[0]
    name = encoded_name.split(b"\0", 1)[0].decode("ascii")
    if args.expect_name and args.expect_name != name:
        parser.error(f"expected {args.expect_name!r}, found {name!r}")
    try:
        approved = next(
            metadata
            for species, metadata in APPROVED_PACKS.items()
            if species == source.stem and metadata["name"] == name
        )
        result = inspect_pack(source, approved)
    except (AssertionError, OSError, StopIteration, ValueError) as error:
        print(f"PACK_ALIGNMENT_FAIL {error}", file=sys.stderr)
        return 2
    print(
        "PACK_ALIGNMENT_PASS "
        f"name={result['name']} id={result['pack_id']} revision={result['revision']} "
        f"bytes={result['bytes']} sha256={result['sha256']}"
    )
    if args.gif or args.artifacts_dir:
        print("PACK_ALIGNMENT_INFO canonical previews are under assets/pack-evidence")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
