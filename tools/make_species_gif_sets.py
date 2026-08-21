#!/usr/bin/env python3
"""Build and render the public Cat, Fox, and Dog animation sets.

This compatibility entry point delegates to the canonical default-pack
builder, which emits one contact sheet and twelve role GIFs per species from
the exact serialized frame inputs.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def main() -> int:
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source-dir", type=Path,
        default=project / "assets" / "companion-sources",
    )
    parser.add_argument(
        "--packs-dir", type=Path,
        default=project / "assets" / "packs",
    )
    parser.add_argument(
        "--output-dir", type=Path,
        default=project / "assets" / "pack-evidence",
    )
    args = parser.parse_args()
    command = [
        sys.executable,
        str(project / "tools" / "build_default_packs.py"),
        "--source-dir", str(args.source_dir.resolve()),
        "--pack-dir", str(args.packs_dir.resolve()),
        "--evidence-dir", str(args.output_dir.resolve()),
    ]
    return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
