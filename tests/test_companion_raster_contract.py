#!/usr/bin/env python3
"""Regression tests for destructive companion-raster failure modes."""

from __future__ import annotations

import hashlib
import sys
import tempfile
import unittest
from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import build_default_packs as base  # noqa: E402
import build_wild_packs as wild  # noqa: E402
import companion_raster_contract as contract  # noqa: E402
import sync_wild_portraits as portrait_sync  # noqa: E402


CANVAS = 512
CELL = CANVAS // 2
IDLE = base.ROLE_SPECS[0]


def draw_local_frame(
    draw: ImageDraw.ImageDraw,
    phase: int,
    *,
    box: tuple[int, int, int, int] = (98, 90, 157, 159),
    variant: int | None = None,
) -> None:
    column = phase % 2
    row = phase // 2
    offset_x = column * CELL
    offset_y = row * CELL
    left, top, right, bottom = box
    draw.rectangle(
        (left + offset_x, top + offset_y, right + offset_x, bottom + offset_y),
        fill=0,
    )
    selected = phase if variant is None else variant
    additions = (
        (124, 84, 130, 91),
        (157, 112, 163, 121),
        (92, 126, 99, 135),
        (108, 84, 115, 91),
    )
    extra = additions[selected]
    draw.rectangle(
        (
            extra[0] + offset_x,
            extra[1] + offset_y,
            extra[2] + offset_x,
            extra[3] + offset_y,
        ),
        fill=0,
    )


def write_valid_idle_sources(root: Path, species: str) -> contract.IdentityLock:
    species_dir = root / species
    species_dir.mkdir(parents=True)

    identity = Image.new("L", (CANVAS, CANVAS), 255)
    ImageDraw.Draw(identity).rectangle((196, 180, 315, 319), fill=0)
    identity_path = species_dir / "identity.png"
    identity.save(identity_path)

    action = Image.new("L", (CANVAS, CANVAS), 255)
    action_draw = ImageDraw.Draw(action)
    for phase in range(4):
        draw_local_frame(action_draw, phase)
    action.save(species_dir / "idle.png")

    return contract.IdentityLock(
        identity_key=species,
        identity_sha256=contract.sha256_file(identity_path),
        source_canvas=(CANVAS, CANVAS),
        target_long_axis_pixels=44,
        approved=True,
    )


class CompanionRasterContractTests(unittest.TestCase):
    def test_valid_frames_use_one_full_cell_scale_and_remain_distinct(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            lock = write_valid_idle_sources(source, "hamster")
            raster = contract.load_species_raster(
                source, "hamster", lock, roles=(IDLE,)
            )

        self.assertEqual(len(raster.frames), 4)
        self.assertEqual(len({frame.output_sha256 for frame in raster.frames}), 4)
        self.assertEqual(
            {base.bounds(frame.output_mask)[3] for frame in raster.frames},
            {base.FLOOR_Y},
        )
        self.assertTrue(
            all(
                base.BODY_AXIS_X - 0.5
                <= base.median_x(set(frame.output_mask))
                <= base.BODY_AXIS_X + 0.5
                for frame in raster.frames
            )
        )
        expected = (
            raster.identity.raster_scale
            * raster.identity.source_frame.cell_width
            / CELL
        )
        self.assertAlmostEqual(raster.fixed_action_scale, expected)

    def test_rabbit_ears_touching_quadrant_edge_are_rejected_not_cut(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            lock = write_valid_idle_sources(source, "rabbit")
            path = source / "rabbit" / "idle.png"
            image = Image.open(path).convert("L")
            ImageDraw.Draw(image).rectangle((124, 0, 130, 95), fill=0)
            image.save(path)
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"rabbit/idle/0: subject touches source-cell edge.*clipped",
            ):
                contract.load_species_raster(source, "rabbit", lock, roles=(IDLE,))

    def test_raccoon_camera_scale_drift_is_not_auto_normalized(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            lock = write_valid_idle_sources(source, "raccoon")
            path = source / "raccoon" / "idle.png"
            image = Image.open(path).convert("L")
            draw = ImageDraw.Draw(image)
            draw.rectangle((CELL, CELL, CANVAS - 1, CANVAS - 1), fill=255)
            draw_local_frame(
                draw,
                3,
                box=(78, 62, 177, 177),
                variant=3,
            )
            image.save(path)
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"raccoon/idle/3: apparent subject scale .* outside identity envelope",
            ):
                contract.load_species_raster(source, "raccoon", lock, roles=(IDLE,))

    def test_ferret_fragmentation_is_rejected_without_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            lock = write_valid_idle_sources(source, "ferret")
            path = source / "ferret" / "idle.png"
            image = Image.open(path).convert("L")
            draw = ImageDraw.Draw(image)
            for index in range(13):
                x = 25 + index * 14
                draw.rectangle((x, 35, x + 2, 37), fill=0)
            image.save(path)
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"ferret/idle/0: fragmented/debris source has .* components",
            ):
                contract.load_species_raster(source, "ferret", lock, roles=(IDLE,))

    def test_red_panda_one_pixel_downscale_debris_is_rejected(self) -> None:
        mask = {
            (x, y)
            for y in range(30, base.FLOOR_Y + 1)
            for x in range(20, 45)
        }
        mask.add((48, 50))
        with self.assertRaisesRegex(
            contract.RasterContractError,
            r"red_panda/idle/0: downscale produced a 1-pixel component",
        ):
            contract.validate_output_mask(mask, "red_panda/idle/0")

    def test_duplicate_action_cell_is_a_missing_frame_error(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            lock = write_valid_idle_sources(source, "capybara")
            path = source / "capybara" / "idle.png"
            image = Image.open(path).convert("L")
            draw = ImageDraw.Draw(image)
            draw.rectangle((CELL, 0, CANVAS - 1, CELL - 1), fill=255)
            draw_local_frame(draw, 1, variant=0)
            image.save(path)
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"all four source frames must be present and distinct",
            ):
                contract.load_species_raster(source, "capybara", lock, roles=(IDLE,))

    def test_fixed_scale_overflow_is_rejected_instead_of_shrunk(self) -> None:
        frame = base.SourceFrame(
            species="rabbit",
            role=IDLE,
            phase=0,
            mask={(x, y) for y in range(20, 236) for x in range(112, 145)},
            cell_width=CELL,
            cell_height=CELL,
        )
        output = contract.rasterize_full_cell(frame, 0.34)
        with self.assertRaisesRegex(
            contract.RasterContractError, r"bounds .* violate fixed canvas"
        ):
            contract.validate_output_mask(output, "rabbit/idle/0")

    def test_protected_starter_cannot_enter_wild_pipeline(self) -> None:
        lock = contract.IdentityLock(
            identity_key="cat",
            identity_sha256="0" * 64,
            source_canvas=(CANVAS, CANVAS),
            target_long_axis_pixels=44,
            approved=True,
        )
        with self.assertRaisesRegex(
            contract.RasterContractError, r"protected starter"
        ):
            contract.load_species_raster(Path("unused"), "cat", lock, roles=(IDLE,))

    def test_existing_output_is_never_overwritten(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staging = root / "staging"
            staging.mkdir()
            (staging / "candidate.txt").write_text("new", encoding="utf-8")
            destination = root / "accepted"
            destination.mkdir()
            sentinel = destination / "sentinel.txt"
            sentinel.write_text("keep", encoding="utf-8")
            before = hashlib.sha256(sentinel.read_bytes()).hexdigest()
            with self.assertRaisesRegex(ValueError, r"will not be overwritten"):
                wild.commit_staged_output(staging, destination)
            after = hashlib.sha256(sentinel.read_bytes()).hexdigest()

        self.assertEqual(before, after)

    def test_portrait_is_nearest_neighbour_one_bit_and_round_trips(self) -> None:
        mask = {
            (x, y)
            for y in range(12, base.FLOOR_Y + 1)
            for x in range(19, 46)
            if (x + y) % 3 != 0
        }
        portrait, packed = wild.make_portrait(mask)
        self.assertEqual(len(packed), wild.PORTRAIT_BYTES)
        decoded = {
            (x, y)
            for y in range(wild.PORTRAIT_HEIGHT)
            for x in range(wild.PORTRAIT_WIDTH)
            if packed[y * 2 + x // 8] & (1 << (x & 7))
        }
        self.assertEqual(decoded, portrait)

    def test_portrait_sync_rejects_old_destructive_manifest(self) -> None:
        legacy = {
            "schema": "kitsu-wild-pack-private-release-v2",
            "complete_roster": True,
            "non_destructive_build": False,
            "raster_contract": {},
        }
        with self.assertRaisesRegex(ValueError, r"schema must be .*v3"):
            portrait_sync.require_fail_closed_manifest(legacy)

    def test_portrait_sync_requires_exact_source_snapshot_provenance(self) -> None:
        pack = {
            "raster_transform": (
                "full-cell-nearest-neighbour-resize-then-translation"
            ),
            "auto_crop": False,
            "auto_shrink": False,
            "source_cleanup": False,
            "source_sha256": {"identity.png": "a" * 64},
            "source_snapshot": {
                "byte_exact_sha256": {"identity.png": "b" * 64}
            },
        }
        with self.assertRaisesRegex(ValueError, r"snapshot hashes do not match"):
            portrait_sync.require_pack_raster_provenance(pack, "ferret")


if __name__ == "__main__":
    unittest.main(verbosity=2)
