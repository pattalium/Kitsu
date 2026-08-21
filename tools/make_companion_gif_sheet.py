#!/usr/bin/env python3
"""Render a labeled animated comparison sheet from serialized .k868 packs.

The preview deliberately decodes the bitmap bytes stored in each pack instead of
using source artwork.  Every stored step in every semantic role is therefore
visible in the resulting GIF.  Preview timing is normalized so several clips of
different lengths can be compared in one sheet; it does not rewrite pack timing.
"""

from __future__ import annotations

import argparse
import binascii
import struct
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


MAGIC = b"K868PK1\0"
HEADER_BYTES = 64
CLIP_BYTES = 12
STEP_BYTES = 4
FRAME_WIDTH = 64
FRAME_HEIGHT = 64
FRAME_BYTES = FRAME_WIDTH * FRAME_HEIGHT // 8

ROLE_NAMES = (
    "IDLE",
    "BLINK",
    "PET",
    "SLEEP",
    "LISTEN",
    "SURPRISE",
    "PLAY",
    "TIRED",
    "FEED",
    "WAKE",
    "MEET",
    "EVOLVE",
)
MODE_NAMES = ("HOLD", "ONCE", "LOOP", "PINGPONG")


@dataclass(frozen=True)
class Clip:
    role: int
    variant: int
    mode: int
    weight: int
    first_step: int
    step_count: int


@dataclass(frozen=True)
class Step:
    frame_index: int
    duration_ms: int


@dataclass(frozen=True)
class Pack:
    path: Path
    name: str
    revision: int
    frames: tuple[Image.Image, ...]
    clips: tuple[Clip, ...]
    steps: tuple[Step, ...]

    def clip_for_role(self, role: int) -> Clip | None:
        matches = [clip for clip in self.clips if clip.role == role]
        if not matches:
            return None
        return next((clip for clip in matches if clip.variant == 0), matches[0])


def decode_frame(raw: bytes) -> Image.Image:
    if len(raw) != FRAME_BYTES:
        raise ValueError(f"frame has {len(raw)} bytes, expected {FRAME_BYTES}")
    image = Image.new("L", (FRAME_WIDTH, FRAME_HEIGHT), 0)
    pixels = image.load()
    for y in range(FRAME_HEIGHT):
        row = y * (FRAME_WIDTH // 8)
        for x in range(FRAME_WIDTH):
            if raw[row + x // 8] & (1 << (x & 7)):
                pixels[x, y] = 255
    return image


def parse_pack(path: Path) -> Pack:
    data = path.read_bytes()
    if len(data) < HEADER_BYTES or data[:8] != MAGIC:
        raise ValueError(f"{path}: missing K868PK1 header")

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
    ) = struct.unpack_from("<HHIIIIIHHHHII16s", data, 8)

    if version != 1 or header_bytes != HEADER_BYTES:
        raise ValueError(f"{path}: unsupported pack header")
    if total_bytes != len(data):
        raise ValueError(f"{path}: header says {total_bytes} bytes, file has {len(data)}")
    if not pack_id or not revision or flags:
        raise ValueError(f"{path}: invalid ID, revision, or flags")
    if (width, height) != (FRAME_WIDTH, FRAME_HEIGHT):
        raise ValueError(f"{path}: expected a 64x64 canvas, got {width}x{height}")

    expected_bytes = (
        HEADER_BYTES
        + clip_count * CLIP_BYTES
        + step_count * STEP_BYTES
        + frame_count * FRAME_BYTES
    )
    if expected_bytes != len(data):
        raise ValueError(f"{path}: invalid fixed-layout length")
    if binascii.crc32(data[HEADER_BYTES:]) & 0xFFFFFFFF != payload_crc:
        raise ValueError(f"{path}: payload CRC mismatch")
    header_for_crc = bytearray(data[8:HEADER_BYTES])
    header_for_crc[0x14 - 8 : 0x18 - 8] = b"\0\0\0\0"
    if binascii.crc32(header_for_crc) & 0xFFFFFFFF != header_crc:
        raise ValueError(f"{path}: header CRC mismatch")

    clips_offset = HEADER_BYTES
    steps_offset = clips_offset + clip_count * CLIP_BYTES
    frames_offset = steps_offset + step_count * STEP_BYTES
    clips: list[Clip] = []
    for index in range(clip_count):
        values = struct.unpack_from("<BBBBIHH", data, clips_offset + index * CLIP_BYTES)
        role, variant, mode, weight, first_step, count, reserved = values
        if mode >= len(MODE_NAMES) or not weight or reserved:
            raise ValueError(f"{path}: invalid clip {index}")
        if not count or first_step + count > step_count:
            raise ValueError(f"{path}: clip {index} references invalid steps")
        clips.append(Clip(role, variant, mode, weight, first_step, count))

    steps: list[Step] = []
    for index in range(step_count):
        frame_index, duration_ms = struct.unpack_from(
            "<HH", data, steps_offset + index * STEP_BYTES
        )
        if frame_index >= frame_count or not 100 <= duration_ms <= 60000:
            raise ValueError(f"{path}: invalid step {index}")
        steps.append(Step(frame_index, duration_ms))

    frames = tuple(
        decode_frame(data[offset : offset + FRAME_BYTES])
        for offset in range(frames_offset, len(data), FRAME_BYTES)
    )
    name = encoded_name.split(b"\0", 1)[0].decode("utf-8")
    if not name:
        raise ValueError(f"{path}: empty display name")
    return Pack(path, name, revision, frames, tuple(clips), tuple(steps))


def font(size: int, bold: bool = False) -> ImageFont.ImageFont:
    candidates = (
        "C:/Windows/Fonts/seguisb.ttf" if bold else "C:/Windows/Fonts/segoeui.ttf",
        "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf",
    )
    for candidate in candidates:
        try:
            return ImageFont.truetype(candidate, size)
        except OSError:
            pass
    return ImageFont.load_default()


def centered_text(
    draw: ImageDraw.ImageDraw,
    box: tuple[int, int, int, int],
    value: str,
    text_font: ImageFont.ImageFont,
    fill: tuple[int, int, int],
) -> None:
    left, top, right, bottom = box
    text_box = draw.textbbox((0, 0), value, font=text_font)
    width = text_box[2] - text_box[0]
    height = text_box[3] - text_box[1]
    draw.text(
        ((left + right - width) // 2, (top + bottom - height) // 2 - text_box[1]),
        value,
        font=text_font,
        fill=fill,
    )


def playback_steps(pack: Pack, clip: Clip) -> list[Step]:
    selected = list(pack.steps[clip.first_step : clip.first_step + clip.step_count])
    if clip.mode == 3 and len(selected) > 2:
        selected.extend(reversed(selected[1:-1]))
    return selected


def make_sheet(packs: list[Pack], output: Path, frame_ms: int) -> tuple[int, tuple[int, int]]:
    role_count = len(ROLE_NAMES)
    title_height = 60
    species_height = 52
    row_height = 92
    footer_height = 45
    margin = 18
    role_width = 112
    column_width = 188
    width = margin * 2 + role_width + column_width * len(packs)
    height = title_height + species_height + row_height * role_count + footer_height

    all_sequences: dict[tuple[int, int], list[Step]] = {}
    phase_count = 1
    for pack_index, pack in enumerate(packs):
        for role in range(role_count):
            clip = pack.clip_for_role(role)
            if clip is None:
                continue
            sequence = playback_steps(pack, clip)
            all_sequences[(pack_index, role)] = sequence
            phase_count = max(phase_count, len(sequence))

    title_font = font(25, bold=True)
    heading_font = font(17, bold=True)
    role_font = font(15, bold=True)
    small_font = font(12)
    tiny_font = font(11)
    images: list[Image.Image] = []

    for phase in range(phase_count):
        canvas = Image.new("RGB", (width, height), (13, 17, 24))
        draw = ImageDraw.Draw(canvas)
        centered_text(
            draw,
            (0, 0, width, title_height),
            "KITSU868 COMPANION PACKS",
            title_font,
            (246, 248, 252),
        )

        columns_x = margin + role_width
        draw.rounded_rectangle(
            (margin, title_height, margin + role_width - 8, title_height + species_height - 6),
            radius=8,
            fill=(25, 31, 42),
        )
        centered_text(
            draw,
            (margin, title_height, margin + role_width - 8, title_height + species_height - 6),
            "ROLE",
            heading_font,
            (147, 158, 177),
        )
        for pack_index, pack in enumerate(packs):
            left = columns_x + pack_index * column_width
            right = left + column_width - 8
            draw.rounded_rectangle(
                (left, title_height, right, title_height + species_height - 6),
                radius=8,
                fill=(31, 40, 54),
                outline=(75, 91, 116),
            )
            centered_text(
                draw,
                (left, title_height + 2, right, title_height + 29),
                pack.name.upper(),
                heading_font,
                (255, 255, 255),
            )
            centered_text(
                draw,
                (left, title_height + 27, right, title_height + species_height - 7),
                f"r{pack.revision}  •  {len(pack.frames)} FRAMES",
                tiny_font,
                (153, 174, 204),
            )

        for role, role_name in enumerate(ROLE_NAMES):
            top = title_height + species_height + role * row_height
            background = (20, 26, 36) if role % 2 == 0 else (17, 22, 31)
            draw.rectangle((margin, top, width - margin, top + row_height - 4), fill=background)
            draw.text(
                (margin + 8, top + 28),
                role_name,
                font=role_font,
                fill=(228, 233, 241),
            )
            draw.text(
                (margin + 8, top + 49),
                f"ROLE {role:02d}",
                font=tiny_font,
                fill=(112, 126, 148),
            )

            for pack_index, pack in enumerate(packs):
                left = columns_x + pack_index * column_width
                right = left + column_width - 8
                clip = pack.clip_for_role(role)
                if clip is None:
                    draw.rounded_rectangle(
                        (left + 16, top + 20, right - 16, top + 68),
                        radius=6,
                        outline=(57, 66, 80),
                    )
                    centered_text(
                        draw,
                        (left + 16, top + 20, right - 16, top + 68),
                        "NOT IN PACK",
                        small_font,
                        (91, 102, 119),
                    )
                    continue

                sequence = all_sequences[(pack_index, role)]
                sequence_index = min(len(sequence) - 1, phase * len(sequence) // phase_count)
                step = sequence[sequence_index]
                sprite = pack.frames[step.frame_index]
                sprite_rgb = Image.merge("RGB", (sprite, sprite, sprite))
                sprite_x = left + (column_width - 8 - FRAME_WIDTH) // 2
                sprite_y = top + 5
                canvas.paste(sprite_rgb, (sprite_x, sprite_y))
                mode = MODE_NAMES[clip.mode]
                centered_text(
                    draw,
                    (left, top + 69, right, top + row_height - 5),
                    f"{mode}  •  F{step.frame_index + 1:02d}",
                    tiny_font,
                    (125, 146, 174),
                )

        footer_top = height - footer_height
        centered_text(
            draw,
            (0, footer_top, width, height),
            f"EXACT SERIALIZED PACK PIXELS  •  COMPARISON PREVIEW: {frame_ms} ms / PHASE",
            small_font,
            (118, 132, 153),
        )
        images.append(canvas)

    output.parent.mkdir(parents=True, exist_ok=True)
    images[0].save(
        output,
        save_all=True,
        append_images=images[1:],
        duration=[frame_ms] * len(images),
        loop=0,
        optimize=False,
        disposal=2,
    )
    return len(images), (width, height)


def main() -> int:
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--packs",
        nargs="+",
        type=Path,
        default=[
            project / "assets/packs/cat.k868",
            project / "assets/packs/fox.k868",
            project / "assets/packs/dog.k868",
        ],
        help="Pack files, in comparison-column order",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=project / "assets/converted/kitsu868-all-companions.gif",
    )
    parser.add_argument(
        "--frame-ms",
        type=int,
        default=700,
        help="Normalized preview dwell time for each stored animation phase",
    )
    args = parser.parse_args()
    if not 100 <= args.frame_ms <= 10000:
        parser.error("--frame-ms must be between 100 and 10000")

    packs = [parse_pack(path.resolve()) for path in args.packs]
    frame_count, dimensions = make_sheet(packs, args.output.resolve(), args.frame_ms)
    print(f"OUTPUT {args.output.resolve()}")
    print(f"DIMENSIONS {dimensions[0]}x{dimensions[1]}")
    print(f"GIF_FRAMES {frame_count}")
    print(f"FRAME_MS {args.frame_ms}")
    print(f"LOOP_MS {frame_count * args.frame_ms}")
    for pack in packs:
        roles = sum(pack.clip_for_role(role) is not None for role in range(len(ROLE_NAMES)))
        print(
            f'PACK "{pack.name}" revision={pack.revision} '
            f"frames={len(pack.frames)} clips={len(pack.clips)} roles={roles}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
