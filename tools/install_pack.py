"""Validate and install one raw Kitsu868 companion pack over USB.

The installer deliberately writes only the raw ``spiffs`` data partition used
as Kitsu868's single pack slot.  It does not erase flash and does not write the
bootloader, partition table, applications, OTA metadata, or NVS.
"""

from __future__ import annotations

import argparse
import binascii
import os
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


PACK_MAGIC = b"K868PK1\0"
PACK_VERSION = 1
PACK_HEADER_BYTES = 64
PACK_CLIP_BYTES = 12
PACK_STEP_BYTES = 4
PACK_FRAME_WIDTH = 64
PACK_FRAME_HEIGHT = 64
PACK_FRAME_BYTES = PACK_FRAME_WIDTH * PACK_FRAME_HEIGHT // 8
PACK_MAX_CLIPS = 512
PACK_MAX_STEPS = 65535
PACK_MAX_STEPS_PER_CLIP = 256
PACK_MAX_ROLE = 11
PACK_MAX_PLAYBACK_MODE = 3
PACK_MIN_STEP_MS = 100
PACK_MAX_STEP_MS = 60000

PACK_FLASH_OFFSET = 0x670000
PACK_PARTITION_BYTES = 0x140000
PACK_FLASH_END = PACK_FLASH_OFFSET + PACK_PARTITION_BYTES


class PackValidationError(ValueError):
    """Raised when bytes do not conform to the Kitsu868 pack v1 format."""


@dataclass(frozen=True)
class PackInfo:
    path: Path
    display_name: str
    pack_id: int
    revision: int
    width: int
    height: int
    frame_count: int
    clip_count: int
    step_count: int
    total_bytes: int
    payload_crc32: int
    header_crc32: int


def _decode_display_name(encoded: bytes) -> str:
    terminator = encoded.find(b"\0")
    if terminator < 0:
        name_bytes = encoded
    else:
        name_bytes = encoded[:terminator]
        if any(encoded[terminator:]):
            raise PackValidationError("display name has data after its NUL terminator")
    if not name_bytes:
        raise PackValidationError("display name is empty")
    if any(value < 0x20 or value > 0x7E for value in name_bytes):
        raise PackValidationError("display name must contain printable ASCII only")
    return name_bytes.decode("ascii")


def validate_pack(path: Path | str) -> PackInfo:
    """Fully validate a .k868 v1 file and return its trusted metadata."""

    pack_path = Path(path).expanduser().resolve()
    if pack_path.suffix.lower() != ".k868":
        raise PackValidationError("pack filename must end in .k868")
    if not pack_path.is_file():
        raise PackValidationError(f"pack does not exist or is not a file: {pack_path}")

    file_bytes = pack_path.stat().st_size
    if file_bytes < PACK_HEADER_BYTES:
        raise PackValidationError(
            f"pack is shorter than its {PACK_HEADER_BYTES}-byte header"
        )
    if file_bytes > PACK_PARTITION_BYTES:
        raise PackValidationError(
            f"pack is {file_bytes} bytes; slot capacity is {PACK_PARTITION_BYTES}"
        )

    pack = pack_path.read_bytes()
    if len(pack) != file_bytes:
        raise PackValidationError("pack changed while it was being read")
    if pack[:8] != PACK_MAGIC:
        raise PackValidationError("invalid .k868 v1 magic")

    (
        magic,
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
    ) = struct.unpack_from("<8sHHIIIIIHHHHII16s", pack, 0)

    if magic != PACK_MAGIC:
        raise PackValidationError("invalid .k868 v1 magic")
    if version != PACK_VERSION:
        raise PackValidationError(f"unsupported pack version {version}")
    if header_bytes != PACK_HEADER_BYTES:
        raise PackValidationError(f"invalid header size {header_bytes}")
    if total_bytes != len(pack):
        raise PackValidationError(
            f"pack length is {len(pack)} bytes, header declares {total_bytes}"
        )
    if total_bytes > PACK_PARTITION_BYTES:
        raise PackValidationError("pack exceeds the 1.25 MiB raw pack slot")
    if PACK_FLASH_OFFSET + total_bytes > PACK_FLASH_END:
        raise PackValidationError("pack write would cross the raw pack partition boundary")
    if pack_id == 0:
        raise PackValidationError("pack ID must be nonzero")
    if revision == 0:
        raise PackValidationError("pack revision must be nonzero")
    if (width, height) != (PACK_FRAME_WIDTH, PACK_FRAME_HEIGHT):
        raise PackValidationError(f"unsupported frame canvas {width}x{height}")
    if frame_count == 0:
        raise PackValidationError("pack contains no frames")
    if clip_count == 0 or clip_count > PACK_MAX_CLIPS:
        raise PackValidationError(f"invalid clip count {clip_count}")
    if step_count == 0 or step_count > PACK_MAX_STEPS:
        raise PackValidationError(f"invalid animation-step count {step_count}")
    if flags != 0:
        raise PackValidationError(f"unsupported pack flags 0x{flags:08X}")

    display_name = _decode_display_name(encoded_name)

    expected_total = (
        PACK_HEADER_BYTES
        + clip_count * PACK_CLIP_BYTES
        + step_count * PACK_STEP_BYTES
        + frame_count * PACK_FRAME_BYTES
    )
    if total_bytes != expected_total:
        raise PackValidationError(
            f"invalid fixed layout: expected {expected_total} bytes, found {total_bytes}"
        )

    actual_payload_crc = binascii.crc32(pack[PACK_HEADER_BYTES:]) & 0xFFFFFFFF
    if actual_payload_crc != payload_crc:
        raise PackValidationError(
            "payload CRC mismatch: "
            f"expected {payload_crc:08X}, calculated {actual_payload_crc:08X}"
        )

    header_for_crc = bytearray(pack[8:PACK_HEADER_BYTES])
    # headerCrc32 occupies absolute bytes 0x14..0x17 and is zeroed while
    # calculating the CRC over header bytes 8..63.
    header_for_crc[0x14 - 8 : 0x18 - 8] = b"\0\0\0\0"
    actual_header_crc = binascii.crc32(header_for_crc) & 0xFFFFFFFF
    if actual_header_crc != header_crc:
        raise PackValidationError(
            "header CRC mismatch: "
            f"expected {header_crc:08X}, calculated {actual_header_crc:08X}"
        )

    clips_offset = PACK_HEADER_BYTES
    steps_offset = clips_offset + clip_count * PACK_CLIP_BYTES
    has_base_idle = False
    for index in range(clip_count):
        offset = clips_offset + index * PACK_CLIP_BYTES
        role, variant, mode, weight, first_step, count, reserved = struct.unpack_from(
            "<BBBBIHH", pack, offset
        )
        if role > PACK_MAX_ROLE:
            raise PackValidationError(f"clip {index} has invalid role {role}")
        if mode > PACK_MAX_PLAYBACK_MODE:
            raise PackValidationError(f"clip {index} has invalid playback mode {mode}")
        if weight == 0:
            raise PackValidationError(f"clip {index} has zero selection weight")
        if (
            count == 0
            or count > PACK_MAX_STEPS_PER_CLIP
            or first_step + count > step_count
        ):
            raise PackValidationError(f"clip {index} references invalid animation steps")
        if mode == 0 and count != 1:
            raise PackValidationError(
                f"HOLD clip {index} must contain exactly one animation step"
            )
        if reserved != 0:
            raise PackValidationError(f"clip {index} has nonzero reserved data")
        has_base_idle |= role == 0 and variant == 0

    if not has_base_idle:
        raise PackValidationError("pack has no base IDLE clip")

    for index in range(step_count):
        offset = steps_offset + index * PACK_STEP_BYTES
        frame_index, duration_ms = struct.unpack_from("<HH", pack, offset)
        if frame_index >= frame_count:
            raise PackValidationError(
                f"animation step {index} references missing frame {frame_index}"
            )
        if not PACK_MIN_STEP_MS <= duration_ms <= PACK_MAX_STEP_MS:
            raise PackValidationError(
                f"animation step {index} has invalid duration {duration_ms} ms"
            )

    return PackInfo(
        path=pack_path,
        display_name=display_name,
        pack_id=pack_id,
        revision=revision,
        width=width,
        height=height,
        frame_count=frame_count,
        clip_count=clip_count,
        step_count=step_count,
        total_bytes=total_bytes,
        payload_crc32=payload_crc,
        header_crc32=header_crc,
    )


def find_platformio_esptool() -> Path:
    candidates: list[Path] = []
    configured_core = os.environ.get("PLATFORMIO_CORE_DIR")
    if configured_core:
        candidates.append(Path(configured_core))

    user_profile = os.environ.get("USERPROFILE")
    if user_profile:
        candidates.append(Path(user_profile) / ".platformio")
    candidates.append(Path.home() / ".platformio")

    seen: set[Path] = set()
    for core_dir in candidates:
        esptool = (core_dir / "packages" / "tool-esptoolpy" / "esptool.py").resolve()
        if esptool in seen:
            continue
        seen.add(esptool)
        if esptool.is_file():
            return esptool
    searched = ", ".join(str(path) for path in seen)
    raise FileNotFoundError(f"PlatformIO's local esptool.py was not found; searched: {searched}")


def esptool_command(info: PackInfo, port: str, baud: int) -> list[str]:
    if not port.strip():
        raise ValueError("serial port cannot be empty")
    if baud <= 0:
        raise ValueError("baud rate must be positive")
    esptool = find_platformio_esptool()
    return [
        sys.executable,
        str(esptool),
        "--chip",
        "esp32s3",
        "--port",
        port,
        "--baud",
        str(baud),
        "--before",
        "default_reset",
        "--after",
        "hard_reset",
        "write_flash",
        "-z",
        f"0x{PACK_FLASH_OFFSET:X}",
        str(info.path),
    ]


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Validate a Kitsu868 .k868 v1 pack, then write only the raw "
            "single-pack slot at flash offset 0x670000."
        )
    )
    parser.add_argument("pack", type=Path, help="path to the .k868 v1 pack")
    parser.add_argument("--port", required=True, help="Heltec serial port, for example COM3")
    parser.add_argument("--baud", type=int, default=460800, help="USB flashing baud")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="validate and print the esptool command without writing flash",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        info = validate_pack(args.pack)
        command = esptool_command(info, args.port, args.baud)
    except (OSError, PackValidationError, ValueError) as error:
        print(f"PACK_INSTALL_FAIL {error}", file=sys.stderr)
        return 2

    print(
        "PACK_VALID "
        f'name="{info.display_name}" id={info.pack_id:08X} '
        f"revision={info.revision} frames={info.frame_count} "
        f"clips={info.clip_count} steps={info.step_count} bytes={info.total_bytes} "
        f"payload_crc={info.payload_crc32:08X} header_crc={info.header_crc32:08X}"
    )
    print(
        f"PACK_TARGET port={args.port} offset=0x{PACK_FLASH_OFFSET:X} "
        f"end=0x{PACK_FLASH_OFFSET + info.total_bytes:X} "
        "preserves=nvs,otadata,app0,app1"
    )

    if args.dry_run:
        print(f"PACK_DRY_RUN {subprocess.list2cmdline(command)}")
        return 0

    try:
        completed = subprocess.run(command, check=False)
    except OSError as error:
        print(f"PACK_INSTALL_FAIL could not start esptool: {error}", file=sys.stderr)
        return 3
    if completed.returncode != 0:
        print(
            f"PACK_INSTALL_FAIL esptool exited with status {completed.returncode}",
            file=sys.stderr,
        )
        return completed.returncode or 3

    print(
        f'PACK_INSTALLED name="{info.display_name}" id={info.pack_id:08X} '
        f"bytes={info.total_bytes} offset=0x{PACK_FLASH_OFFSET:X}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
