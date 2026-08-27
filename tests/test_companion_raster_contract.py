#!/usr/bin/env python3
"""Regression tests for destructive companion-raster failure modes."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from dataclasses import replace
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
BLINK = base.ROLE_SPECS[1]
TEST_ACTION_OUTPUT_OFFSET = (-1, 30)


def imagegen_import_transform() -> contract.ImageGenImportTransform:
    return contract.recommended_imagegen_import_transform(
        action_output_offset=TEST_ACTION_OUTPUT_OFFSET
    )


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


def write_high_res_frame(
    path: Path,
    variant: int,
    *,
    box: tuple[int, int, int, int] = (20, 30, 43, 77),
    mode: str = "1",
) -> None:
    image = Image.new(
        mode,
        (contract.HIGH_RES_FRAME_WIDTH, contract.HIGH_RES_FRAME_HEIGHT),
        1 if mode == "1" else 255,
    )
    draw = ImageDraw.Draw(image)
    draw.rectangle(box, fill=0)
    # Four small, intentionally distinct negative-space details.  They change
    # the final native pixels without changing camera scale or subject bounds.
    cutouts = ((24, 40), (29, 40), (34, 40), (39, 40))
    x, y = cutouts[variant]
    draw.rectangle((x, y, x + 1, y + 1), fill=1 if mode == "1" else 255)
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)


def write_valid_high_res_sources(
    root: Path, species: str
) -> contract.HighResIdentityLock:
    species_dir = root / species
    species_dir.mkdir(parents=True)
    identity_path = species_dir / "identity.png"
    identity = Image.new(
        "1",
        (contract.HIGH_RES_FRAME_WIDTH, contract.HIGH_RES_FRAME_HEIGHT),
        1,
    )
    ImageDraw.Draw(identity).rectangle((20, 30, 43, 77), fill=0)
    identity.save(identity_path)

    portrait = Image.new(
        "1",
        (contract.HIGH_RES_PORTRAIT_WIDTH, contract.HIGH_RES_PORTRAIT_HEIGHT),
        1,
    )
    ImageDraw.Draw(portrait).rectangle((4, 3, 11, 15), fill=0)
    portrait.save(species_dir / "portrait.png")

    for phase in range(4):
        write_high_res_frame(species_dir / "idle" / f"{phase:02d}.png", phase)

    return contract.HighResIdentityLock(
        identity_key=species,
        identity_sha256=contract.sha256_file(identity_path),
        frame_canvas=(
            contract.HIGH_RES_FRAME_WIDTH,
            contract.HIGH_RES_FRAME_HEIGHT,
        ),
        approved=True,
    )


def imagegen_logical_outline(variant: int | None = None) -> set[tuple[int, int]]:
    mask = {
        (x, y)
        for y in range(29, 51)
        for x in range(10, 54)
        if x in (10, 53) or y in (29, 50)
    }
    if variant is not None:
        x = 20 + variant * 5
        mask.update({(x, 30), (x + 1, 30), (x, 31), (x + 1, 31)})
    return mask


def imagegen_action_logical_outline(
    variant: int | None = None,
) -> set[tuple[int, int]]:
    """Same final identity placement through the action cell's own offset."""

    mask = {
        (x, y)
        for y in range(26, 48)
        for x in range(10, 54)
        if x in (10, 53) or y in (26, 47)
    }
    if variant is not None:
        x = 20 + variant * 5
        mask.update({(x, 27), (x + 1, 27), (x, 28), (x + 1, 28)})
    return mask


def write_imagegen_raw(
    path: Path,
    transform: contract.ImageGenImportTransform,
    logical_mask: set[tuple[int, int]],
) -> None:
    logical = Image.new(
        "1",
        (contract.HIGH_RES_FRAME_WIDTH, contract.HIGH_RES_FRAME_HEIGHT),
        1,
    )
    pixels = logical.load()
    for x, y in logical_mask:
        pixels[x, y] = 0
    left, top, right, bottom = transform.crop_rect
    generated = logical.resize(
        (right - left, bottom - top), Image.Resampling.NEAREST
    ).convert("RGB")
    source = Image.new("RGB", transform.source_canvas, "white")
    source.paste(generated, (left, top))
    path.parent.mkdir(parents=True, exist_ok=True)
    source.save(path)


def make_test_generated_semantic_contract(
    species: str,
    identity: contract.HighResFrame,
    frames: list[contract.HighResFrame],
    *,
    identity_source_sha256: str,
    identity_frame_sha256: str,
    source_layout: str,
    role_source_sha256: str | None = None,
    role: base.RoleSpec = IDLE,
) -> contract.GeneratedActionSemanticContract:
    identity_mask = set(identity.mask)
    frame_masks = [set(frame.mask) for frame in frames]
    baseline_policy = contract.GENERATED_ROLE_BASELINE_POLICY[role.name]
    role_pose_mask = frame_masks[0]
    frozen_baseline = (
        identity_mask if baseline_policy == "identity-anchored" else role_pose_mask
    )
    stable_ink = sorted(
        set.intersection(frozen_baseline, *frame_masks)
        - {
            point
            for point in frozen_baseline
            if point[1] == contract.HIGH_RES_FLOOR_Y
        },
        key=lambda point: (point[1], point[0]),
    )
    protected = contract.native_region_mask_lock(set(stable_ink[:8]))
    planted = contract.native_region_mask_lock(
        {(x, contract.HIGH_RES_FLOOR_Y) for x in range(contract.HIGH_RES_FRAME_WIDTH)}
    )
    reference = contract.ImmutableIdentityReference(
        kind="immutable-approved-identity-source",
        relative_path="identity.png",
        identity_key=species,
        source_sha256=identity_source_sha256,
        frame_sha256=identity_frame_sha256,
    )
    phase_locks: list[contract.GeneratedPhaseSemanticLock] = []
    motion: set[tuple[int, int]] = set()
    for phase, (frame, frame_mask) in enumerate(zip(frames, frame_masks, strict=True)):
        semantic_baseline = (
            identity_mask
            if baseline_policy == "identity-anchored" or phase == 0
            else role_pose_mask
        )
        changed = semantic_baseline ^ frame_mask
        motion_baseline = (
            identity_mask if baseline_policy == "identity-anchored" else role_pose_mask
        )
        motion.update(motion_baseline ^ frame_mask)
        source_sha256 = (
            frame.source_sha256
            if source_layout == "independent-frame"
            else role_source_sha256
        )
        assert source_sha256 is not None
        phase_locks.append(
            contract.GeneratedPhaseSemanticLock(
                phase=phase,
                semantic_baseline=(
                    "approved-identity"
                    if baseline_policy == "identity-anchored"
                    else (
                        "approved-identity-pose-gate"
                        if phase == 0
                        else "immutable-role-phase-0"
                    )
                ),
                identity_reference=reference,
                generated_asset=contract.GeneratedPhaseAsset(
                    layout=source_layout,
                    relative_path=(
                        f"{role.name}/{phase:02d}.png"
                        if source_layout == "independent-frame"
                        else f"{role.name}.png"
                    ),
                    source_sha256=source_sha256,
                    source_region_sha256=frame.source_sha256,
                ),
                allowed_change_region=contract.native_region_mask_lock(changed),
                maximum_out_of_region_changed_pixels=0,
                frozen_regions=(
                    contract.FrozenSemanticRegion(
                        kind="planted-contact",
                        name="floor-contact",
                        region=planted,
                        maximum_changed_pixels=0,
                    ),
                    contract.FrozenSemanticRegion(
                        kind="protected-identity-landmark",
                        name="face-landmark",
                        region=protected,
                        maximum_changed_pixels=0,
                    ),
                ),
            )
        )
    return contract.GeneratedActionSemanticContract(
        schema=contract.GENERATED_ACTION_SEMANTIC_SCHEMA,
        roles=(
            contract.GeneratedRoleSemanticLock(
                role=role.name,
                baseline_policy=baseline_policy,
                contact_policy=(
                    contract.GENERATED_ROLE_CONTACT_POLICY_DEFAULTS[role.name]
                ),
                role_pose_baseline_frame_sha256=hashlib.sha256(
                    frames[0].packed
                ).hexdigest(),
                maximum_role_pose_component_count_delta=2,
                maximum_contact_changed_pixels_per_phase=(
                    contract.GENERATED_CONTACT_POLICY_MAXIMUMS[
                        contract.GENERATED_ROLE_CONTACT_POLICY_DEFAULTS[role.name]
                    ]
                ),
                role_pose_identity_landmarks=(
                    contract.RolePoseIdentityLandmarkLock(
                        name="identity-marking",
                        identity_region=contract.native_region_mask_lock(
                            set(stable_ink[:8])
                        ),
                        role_pose_region=contract.native_region_mask_lock(
                            set(stable_ink[:8])
                        ),
                        minimum_ink_pixels=4,
                        minimum_ink_retention_per_mille=800,
                        maximum_component_count_delta=2,
                    ),
                ),
                phases=tuple(phase_locks),
                motion_landmarks=(
                    contract.MotionLandmarkLock(
                        name="role-motion",
                        region=contract.native_region_mask_lock(motion),
                        minimum_changed_pixels=max(
                            contract.GENERATED_MIN_MOTION_LANDMARK_PIXELS,
                            min(len(motion), 8),
                        ),
                    ),
                ),
            ),
        ),
    )


def write_valid_imagegen_sources(
    root: Path, species: str
) -> contract.ImageGenImportLock:
    transform = imagegen_import_transform()
    species_dir = root / species
    species_dir.mkdir(parents=True)
    identity_path = species_dir / "identity.png"
    write_imagegen_raw(identity_path, transform, imagegen_logical_outline())

    portrait = Image.new(
        "1",
        (contract.HIGH_RES_PORTRAIT_WIDTH, contract.HIGH_RES_PORTRAIT_HEIGHT),
        1,
    )
    ImageDraw.Draw(portrait).rectangle((4, 3, 11, 15), fill=0)
    portrait.save(species_dir / "portrait.png")
    for phase in range(4):
        write_imagegen_raw(
            species_dir / "idle" / f"{phase:02d}.png",
            transform,
            imagegen_logical_outline(phase),
        )

    identity = contract.load_imagegen_import_frame(
        identity_path, "identity", -1, transform
    )
    identity_source_sha256 = contract.sha256_file(identity_path)
    identity_frame_sha256 = hashlib.sha256(identity.packed).hexdigest()
    frames = [
        contract.load_imagegen_import_frame(
            species_dir / "idle" / f"{phase:02d}.png",
            "idle",
            phase,
            transform,
            identity_mask=set(identity.mask),
        )
        for phase in range(4)
    ]
    semantic = make_test_generated_semantic_contract(
        species,
        identity,
        frames,
        identity_source_sha256=identity_source_sha256,
        identity_frame_sha256=identity_frame_sha256,
        source_layout="independent-frame",
    )
    return contract.ImageGenImportLock(
        identity_key=species,
        identity_source_sha256=identity_source_sha256,
        identity_frame_sha256=identity_frame_sha256,
        transform_sha256=contract.imagegen_import_transform_sha256(transform),
        transform=transform,
        action_semantic_contract_sha256=(
            contract.generated_action_semantic_contract_sha256(semantic)
        ),
        action_semantic_contract=semantic,
        approved=True,
    )


def write_imagegen_action_sheet(
    path: Path,
    logical_masks: list[set[tuple[int, int]]],
) -> None:
    """Write one named action without changing any cell's fixed camera."""

    if len(logical_masks) != 4:
        raise ValueError("an action sheet requires exactly four logical masks")
    sheet = Image.new(
        "RGB", contract.IMAGEGEN_ACTION_SHEET_SOURCE_CANVAS, "white"
    )
    for mask, rect in zip(
        logical_masks, contract.IMAGEGEN_ACTION_SHEET_PHASE_RECTS
    ):
        logical = Image.new(
            "1",
            (contract.HIGH_RES_FRAME_WIDTH, contract.HIGH_RES_FRAME_HEIGHT),
            1,
        )
        pixels = logical.load()
        for x, y in mask:
            pixels[x, y] = 0
        left, top, right, bottom = rect
        cell = logical.resize(
            (right - left, bottom - top), Image.Resampling.NEAREST
        ).convert("RGB")
        sheet.paste(cell, (left, top))
    path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(path)


def write_valid_imagegen_action_sheet_sources(
    root: Path, species: str
) -> contract.ImageGenImportLock:
    transform = imagegen_import_transform()
    species_dir = root / species
    species_dir.mkdir(parents=True)
    identity_path = species_dir / "identity.png"
    write_imagegen_raw(identity_path, transform, imagegen_logical_outline())

    portrait = Image.new(
        "1",
        (contract.HIGH_RES_PORTRAIT_WIDTH, contract.HIGH_RES_PORTRAIT_HEIGHT),
        1,
    )
    ImageDraw.Draw(portrait).rectangle((4, 3, 11, 15), fill=0)
    portrait.save(species_dir / "portrait.png")
    write_imagegen_action_sheet(
        species_dir / "idle.png",
        [imagegen_action_logical_outline(phase) for phase in range(4)],
    )

    identity = contract.load_imagegen_import_frame(
        identity_path, "identity", -1, transform
    )
    identity_source_sha256 = contract.sha256_file(identity_path)
    identity_frame_sha256 = hashlib.sha256(identity.packed).hexdigest()
    role_source_sha256 = contract.sha256_file(species_dir / "idle.png")
    frames = list(
        contract.load_imagegen_action_sheet_frames(
            species_dir / "idle.png",
            species,
            IDLE,
            transform,
            set(identity.mask),
        )
    )
    semantic = make_test_generated_semantic_contract(
        species,
        identity,
        frames,
        identity_source_sha256=identity_source_sha256,
        identity_frame_sha256=identity_frame_sha256,
        source_layout="one-action-sheet-region",
        role_source_sha256=role_source_sha256,
    )
    return contract.ImageGenImportLock(
        identity_key=species,
        identity_source_sha256=identity_source_sha256,
        identity_frame_sha256=identity_frame_sha256,
        transform_sha256=contract.imagegen_import_transform_sha256(transform),
        transform=transform,
        action_semantic_contract_sha256=(
            contract.generated_action_semantic_contract_sha256(semantic)
        ),
        action_semantic_contract=semantic,
        approved=True,
    )


def refresh_test_generated_semantic_lock(
    root: Path,
    species: str,
    lock: contract.ImageGenImportLock,
    roles: tuple[base.RoleSpec, ...],
    *,
    source_layout: str,
) -> contract.ImageGenImportLock:
    species_dir = root / species
    identity = contract.load_imagegen_import_frame(
        species_dir / "identity.png", "identity", -1, lock.transform
    )
    semantic_roles: list[contract.GeneratedRoleSemanticLock] = []
    for role in roles:
        if source_layout == "independent-frame":
            frames = [
                contract.load_imagegen_import_frame(
                    species_dir / role.name / f"{phase:02d}.png",
                    role.name,
                    phase,
                    lock.transform,
                    identity_mask=set(identity.mask),
                )
                for phase in range(4)
            ]
            role_source_sha256 = None
        else:
            role_path = species_dir / f"{role.name}.png"
            frames = list(
                contract.load_imagegen_action_sheet_frames(
                    role_path,
                    species,
                    role,
                    lock.transform,
                    set(identity.mask),
                )
            )
            role_source_sha256 = contract.sha256_file(role_path)
        one_role = make_test_generated_semantic_contract(
            species,
            identity,
            frames,
            identity_source_sha256=lock.identity_source_sha256,
            identity_frame_sha256=lock.identity_frame_sha256,
            source_layout=source_layout,
            role_source_sha256=role_source_sha256,
            role=role,
        )
        semantic_roles.append(one_role.roles[0])
    semantic = contract.GeneratedActionSemanticContract(
        schema=contract.GENERATED_ACTION_SEMANTIC_SCHEMA,
        roles=tuple(semantic_roles),
    )
    return replace(
        lock,
        action_semantic_contract=semantic,
        action_semantic_contract_sha256=(
            contract.generated_action_semantic_contract_sha256(semantic)
        ),
    )


def byte_exact_tree_snapshot(root: Path) -> dict[str, str]:
    return {
        path.relative_to(root).as_posix(): contract.sha256_file(path)
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


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

    def test_portrait_sync_rejects_old_destructive_manifest(self) -> None:
        legacy = {
            "schema": "kitsu-wild-pack-private-release-v2",
            "complete_roster": True,
            "non_destructive_build": False,
            "raster_contract": {},
        }
        with self.assertRaisesRegex(ValueError, r"schema must be .*v4"):
            portrait_sync.require_fail_closed_manifest(legacy)

    def test_portrait_sync_requires_exact_source_snapshot_provenance(self) -> None:
        pack = {
            "raster_transform": portrait_sync.DIRECT_RASTER_TRANSFORM,
            "transform_controls": dict(portrait_sync.TRANSFORM_CONTROLS),
            "format_version": 2,
            "frame_canvas": [64, 80],
            "pack_bytes": 31_120,
            "identity_lock": {
                "frame_canvas": [64, 80],
                "identity_frame_sha256": "c" * 64,
                "identity_sha256": "d" * 64,
                "schema": portrait_sync.DIRECT_LOCK_SCHEMA,
            },
            "source_kind": "direct-exact-target",
            "fixed_action_scale": 1.0,
            "identity_raster_scale": 1.0,
            "action_cell_raster_scale": 1.0,
            "source_sha256": {"identity.png": "a" * 64},
            "source_snapshot": {
                "byte_exact_sha256": {"identity.png": "b" * 64}
            },
        }
        with self.assertRaisesRegex(ValueError, r"snapshot hashes do not match"):
            portrait_sync.require_pack_raster_provenance(pack, "ferret")

    def test_high_res_frame_is_exact_native_64x80_and_round_trips(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            lock = write_valid_high_res_sources(source, "ferret")
            raster = contract.load_high_res_species(
                source, "ferret", lock, roles=(IDLE,)
            )

        self.assertEqual(raster.fixed_action_scale, 1.0)
        self.assertEqual(len(raster.frames), 4)
        self.assertEqual(
            {len(frame.packed) for frame in raster.frames},
            {contract.HIGH_RES_FRAME_BYTES},
        )
        self.assertEqual(
            {base.bounds(frame.mask)[3] for frame in raster.frames},
            {contract.HIGH_RES_FLOOR_Y},
        )
        self.assertTrue(
            all(
                contract.decode_high_res_frame_bytes(frame.packed)
                == set(frame.mask)
                for frame in raster.frames
            )
        )

    def test_high_res_identity_lock_is_exact_v2_canvas_not_legacy_scale(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock = write_valid_high_res_sources(root, "ferret")
            lock_path = root / "identity-lock-v2.json"
            lock_path.write_text(
                json.dumps(
                    {
                        "schema": contract.HIGH_RES_IDENTITY_LOCK_SCHEMA,
                        "identities": [
                            {
                                "approved": True,
                                "identity_key": "ferret",
                                "identity_sha256": lock.identity_sha256,
                                "frame_canvas": [64, 80],
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            loaded = contract.load_high_res_identity_locks(
                lock_path, ["ferret"]
            )

        self.assertEqual(loaded["ferret"].frame_canvas, (64, 80))

    def test_high_res_identity_lock_rejects_legacy_schema(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "legacy-lock.json"
            path.write_text(
                json.dumps({"schema": contract.IDENTITY_LOCK_SCHEMA, "identities": []}),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"schema must be kitsu-wild-identity-lock-v2",
            ):
                contract.load_high_res_identity_locks(path, [])

    def test_high_res_rejects_64x64_instead_of_resizing_or_padding(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "ferret" / "idle" / "00.png"
            path.parent.mkdir(parents=True)
            image = Image.new("1", (64, 64), 1)
            ImageDraw.Draw(image).rectangle((20, 20, 43, 61), fill=0)
            image.save(path)
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"expected \(64, 80\).*never resizes, crops, or pads",
            ):
                contract.load_high_res_frame(path, "idle", 0)

    def test_high_res_rejects_grayscale_even_when_pixels_are_black_white(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "ferret" / "idle" / "00.png"
            write_high_res_frame(path, 0, mode="L")
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"mode is 'L'.*no threshold",
            ):
                contract.load_high_res_frame(path, "idle", 0)

    def test_high_res_rabbit_ear_or_tail_clipping_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "rabbit" / "idle" / "00.png"
            write_high_res_frame(path, 0)
            image = Image.open(path).copy()
            ImageDraw.Draw(image).rectangle((31, 0, 32, 31), fill=0)
            image.save(path)
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"touches source-cell edge.*clipped",
            ):
                contract.load_high_res_frame(path, "idle", 0)

    def test_high_res_bottom_rows_are_a_hard_blank_guard(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "raccoon" / "idle" / "00.png"
            write_high_res_frame(path, 0, box=(20, 30, 43, 78))
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"touches source-cell edge.*clipped",
            ):
                contract.load_high_res_frame(path, "idle", 0)

    def test_high_res_duplicate_phase_is_rejected_as_missing_animation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            lock = write_valid_high_res_sources(source, "capybara")
            duplicate = source / "capybara" / "idle" / "01.png"
            duplicate.write_bytes(
                (source / "capybara" / "idle" / "00.png").read_bytes()
            )
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"all four exact canonical PNGs must be distinct",
            ):
                contract.load_high_res_species(
                    source, "capybara", lock, roles=(IDLE,)
                )

    def test_high_res_scale_pop_is_rejected_not_normalized(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            lock = write_valid_high_res_sources(source, "red_panda")
            write_high_res_frame(
                source / "red_panda" / "idle" / "03.png",
                3,
                box=(24, 50, 39, 77),
            )
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"apparent identity scale .*regenerate.*instead of resizing",
            ):
                contract.load_high_res_species(
                    source, "red_panda", lock, roles=(IDLE,)
                )

    def test_high_res_portrait_is_authored_not_derived(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            write_valid_high_res_sources(source, "shoebill")
            portrait = contract.load_high_res_portrait(
                source / "shoebill" / "portrait.png", "shoebill"
            )

        self.assertEqual(len(portrait.packed), contract.HIGH_RES_PORTRAIT_BYTES)
        self.assertEqual(len(portrait.packed), 36)

    def test_high_res_action_directory_must_have_exact_four_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            lock = write_valid_high_res_sources(source, "otter")
            (source / "otter" / "idle" / "03.png").unlink()
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"exact independent frame set required.*03.png",
            ):
                contract.load_high_res_species(
                    source, "otter", lock, roles=(IDLE,)
                )

    def test_imagegen_box_area_import_preserves_one_logical_pixel_contour(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            transform = imagegen_import_transform()
            path = Path(temporary) / "ferret" / "identity.png"
            logical = imagegen_logical_outline()
            write_imagegen_raw(path, transform, logical)
            frame = contract.load_imagegen_import_frame(
                path, "identity", -1, transform
            )

        offset_x, offset_y = transform.output_offset
        expected = {(x + offset_x, y + offset_y) for x, y in logical}
        self.assertEqual(set(frame.mask), expected)
        self.assertEqual(base.bounds(frame.mask)[3], contract.HIGH_RES_FLOOR_Y)

    def test_imagegen_source_canvas_one_pixel_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            transform = imagegen_import_transform()
            path = Path(temporary) / "ferret" / "idle" / "00.png"
            image = Image.new(
                "RGB",
                (transform.source_canvas[0] - 1, transform.source_canvas[1]),
                "white",
            )
            path.parent.mkdir(parents=True)
            image.save(path)
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"canvas .* differs from locked canvas.*no per-frame scale",
            ):
                contract.load_imagegen_import_frame(
                    path, "idle", 0, transform
                )

    def test_imagegen_locked_offset_cannot_auto_fit_a_shifted_action(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            transform = imagegen_import_transform()
            path = Path(temporary) / "rabbit" / "play" / "00.png"
            shifted = {(x - 10, y) for x, y in imagegen_logical_outline()}
            write_imagegen_raw(path, transform, shifted)
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"would need per-frame fit|locked output offset .* clips this action",
            ):
                contract.load_imagegen_import_frame(
                    path, "play", 0, transform
                )

    def test_imagegen_one_pixel_transform_drift_breaks_lock_hash(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "imagegen-lock.json"
            transform = imagegen_import_transform()
            record = contract.imagegen_import_transform_record(transform)
            record["output_offset"] = [
                transform.output_offset[0] + 1,
                transform.output_offset[1],
            ]
            path.write_text(
                json.dumps(
                    {
                        "schema": contract.IMAGEGEN_IMPORT_LOCK_SCHEMA,
                        "identities": [
                            {
                                "approved": True,
                                "identity_frame_sha256": "a" * 64,
                                "identity_key": "ferret",
                                "identity_source_sha256": "b" * 64,
                                "transform": record,
                                "transform_sha256": (
                                    contract.imagegen_import_transform_sha256(
                                        transform
                                    )
                                ),
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"transform SHA-256 mismatch.*one-pixel crop, scale, or offset change",
            ):
                contract.load_imagegen_import_locks(path, ["ferret"])

    def test_imagegen_lock_missing_action_output_offset_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "imagegen-lock.json"
            transform = imagegen_import_transform()
            record = contract.imagegen_import_transform_record(transform)
            del record["action_output_offset"]
            path.write_text(
                json.dumps(
                    {
                        "schema": contract.IMAGEGEN_IMPORT_LOCK_SCHEMA,
                        "identities": [
                            {
                                "approved": True,
                                "identity_frame_sha256": "a" * 64,
                                "identity_key": "ferret",
                                "identity_source_sha256": "b" * 64,
                                "transform": record,
                                "transform_sha256": (
                                    contract.imagegen_import_transform_sha256(
                                        transform
                                    )
                                ),
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"requires the explicit action_output_offset.*"
                r"missing=\['action_output_offset'\]",
            ):
                contract.load_imagegen_import_locks(path, ["ferret"])

    def test_imagegen_action_output_offset_drift_breaks_lock_hash(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "imagegen-lock.json"
            transform = imagegen_import_transform()
            record = contract.imagegen_import_transform_record(transform)
            record["action_output_offset"] = [
                transform.action_output_offset[0],
                transform.action_output_offset[1] + 1,
            ]
            path.write_text(
                json.dumps(
                    {
                        "schema": contract.IMAGEGEN_IMPORT_LOCK_SCHEMA,
                        "identities": [
                            {
                                "approved": True,
                                "identity_frame_sha256": "a" * 64,
                                "identity_key": "ferret",
                                "identity_source_sha256": "b" * 64,
                                "transform": record,
                                "transform_sha256": (
                                    contract.imagegen_import_transform_sha256(
                                        transform
                                    )
                                ),
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"transform SHA-256 mismatch.*offset change",
            ):
                contract.load_imagegen_import_locks(path, ["ferret"])

    def test_imagegen_import_lock_round_trips_exact_transform(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock = write_valid_imagegen_sources(root, "ferret")
            path = root / "imagegen-lock.json"
            path.write_text(
                json.dumps(
                    {
                        "schema": contract.IMAGEGEN_IMPORT_LOCK_SCHEMA,
                        "identities": [
                            {
                                "action_semantic_contract": (
                                    contract.generated_action_semantic_contract_record(
                                        lock.action_semantic_contract
                                    )
                                ),
                                "action_semantic_contract_sha256": (
                                    lock.action_semantic_contract_sha256
                                ),
                                "approved": True,
                                "identity_frame_sha256": (
                                    lock.identity_frame_sha256
                                ),
                                "identity_key": "ferret",
                                "identity_source_sha256": (
                                    lock.identity_source_sha256
                                ),
                                "transform": (
                                    contract.imagegen_import_transform_record(
                                        lock.transform
                                    )
                                ),
                                "transform_sha256": lock.transform_sha256,
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            loaded = contract.load_imagegen_import_locks(path, ["ferret"])

        self.assertEqual(loaded["ferret"], lock)

    def test_imagegen_generated_species_uses_one_transform_for_all_phases(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            lock = write_valid_imagegen_sources(source, "ferret")
            raster = contract.load_high_res_generated_species(
                source, "ferret", lock, roles=(IDLE,)
            )

        self.assertEqual(len(raster.frames), 4)
        self.assertEqual(
            raster.fixed_action_scale,
            contract.HIGH_RES_FRAME_WIDTH / 1120,
        )
        self.assertEqual(len({frame.packed for frame in raster.frames}), 4)

    def test_imagegen_distinct_raw_phases_that_collapse_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            lock = write_valid_imagegen_sources(source, "capybara")
            transform = lock.transform
            role_dir = source / "capybara" / "idle"
            for phase in range(4):
                path = role_dir / f"{phase:02d}.png"
                write_imagegen_raw(
                    path, transform, imagegen_logical_outline()
                )
                image = Image.open(path).copy()
                image.putpixel((500 + phase, 600), (0, 0, 0))
                image.save(path)
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"area import collapsed one or more phases",
            ):
                contract.load_high_res_generated_species(
                    source, "capybara", lock, roles=(IDLE,)
                )

    def test_imagegen_action_sheet_layout_is_byte_exact_and_hash_pinned(self) -> None:
        record = contract.imagegen_action_sheet_layout_record()

        self.assertEqual(record["source_canvas"], [1122, 1402])
        self.assertEqual(
            record["phase_viewports"],
            [
                [0, 0, 560, 700],
                [562, 0, 1122, 700],
                [0, 702, 560, 1402],
                [562, 702, 1122, 1402],
            ],
        )
        self.assertEqual(
            record["gutter_rects"],
            [[560, 0, 562, 1402], [0, 700, 1122, 702]],
        )
        self.assertEqual(record["cell_outer_safe_guard_pixels"], 2)
        self.assertIs(record["fixed_cell_extraction"], True)
        self.assertTrue(
            all(
                record[name] is False
                for name in (
                    "per_cell_cleanup",
                    "per_cell_crop",
                    "per_cell_fit",
                    "per_cell_offset",
                    "per_cell_threshold",
                )
            )
        )
        self.assertEqual(
            contract.imagegen_action_sheet_layout_sha256(),
            "7ce76bf5a00170641374b0b964e085f39e1aea7e51ad2dc0f019f27b9146552e",
        )

    def test_imagegen_one_action_sheet_imports_four_distinct_native_frames(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            lock = write_valid_imagegen_action_sheet_sources(source, "ferret")
            raster = contract.load_high_res_generated_action_sheet_species(
                source, "ferret", lock, roles=(IDLE,)
            )

        self.assertEqual(len(raster.frames), 4)
        self.assertEqual(len({frame.source_sha256 for frame in raster.frames}), 4)
        self.assertEqual(len({frame.packed for frame in raster.frames}), 4)
        self.assertEqual(
            raster.fixed_action_scale,
            contract.HIGH_RES_FRAME_WIDTH / 560,
        )
        self.assertEqual(
            set(raster.source_sha256),
            {"identity.png", "portrait.png", "idle.png"},
        )
        self.assertTrue(
            all(
                base.bounds(frame.mask)[3] == contract.HIGH_RES_FLOOR_Y
                for frame in raster.frames
            )
        )

    def test_imagegen_fixed_action_offset_is_shared_by_all_cells_and_actions(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            lock = write_valid_imagegen_action_sheet_sources(source, "ferret")
            write_imagegen_action_sheet(
                source / "ferret" / "blink.png",
                [imagegen_action_logical_outline(phase) for phase in range(4)],
            )
            lock = refresh_test_generated_semantic_lock(
                source,
                "ferret",
                lock,
                (IDLE, BLINK),
                source_layout="one-action-sheet-region",
            )
            raster = contract.load_high_res_generated_action_sheet_species(
                source, "ferret", lock, roles=(IDLE, BLINK)
            )

        identity_dx, identity_dy = lock.transform.output_offset
        self.assertEqual(
            set(raster.identity.mask),
            {
                (x + identity_dx, y + identity_dy)
                for x, y in imagegen_logical_outline()
            },
        )
        action_dx, action_dy = lock.transform.action_output_offset
        for role_index in range(2):
            for phase in range(4):
                frame = raster.frames[role_index * 4 + phase]
                self.assertEqual(
                    set(frame.mask),
                    {
                        (x + action_dx, y + action_dy)
                        for x, y in imagegen_action_logical_outline(phase)
                    },
                )

    def test_imagegen_action_sheet_gutter_ink_rejects_the_complete_action(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            lock = write_valid_imagegen_action_sheet_sources(source, "ferret")
            path = source / "ferret" / "idle.png"
            image = Image.open(path).copy()
            ImageDraw.Draw(image).line((560, 0, 560, 1401), fill=(0, 0, 0))
            image.save(path)
            before = byte_exact_tree_snapshot(source)

            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"center gutter .* contains ink.*labels, dividers, grid lines",
            ):
                contract.load_high_res_generated_action_sheet_species(
                    source, "ferret", lock, roles=(IDLE,)
                )

            self.assertEqual(byte_exact_tree_snapshot(source), before)

    def test_imagegen_one_shifted_sheet_cell_rejects_without_partial_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            lock = write_valid_imagegen_action_sheet_sources(source, "rabbit")
            masks = [imagegen_action_logical_outline(phase) for phase in range(4)]
            masks[2] = {(x - 10, y) for x, y in masks[2]}
            write_imagegen_action_sheet(source / "rabbit" / "idle.png", masks)
            before = byte_exact_tree_snapshot(source)

            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"rabbit/idle/2/raw-cell: subject touches source-cell edge",
            ):
                contract.load_high_res_generated_action_sheet_species(
                    source, "rabbit", lock, roles=(IDLE,)
                )

            self.assertEqual(byte_exact_tree_snapshot(source), before)
            self.assertFalse((source / "rabbit" / "idle").exists())

    def test_imagegen_one_oversized_sheet_cell_rejects_the_whole_action(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            lock = write_valid_imagegen_action_sheet_sources(source, "raccoon")
            masks = [imagegen_action_logical_outline(phase) for phase in range(4)]
            masks[1] = {
                (x, y)
                for y in range(15, 51)
                for x in range(2, 62)
                if x in (2, 61) or y in (15, 50)
            }
            write_imagegen_action_sheet(source / "raccoon" / "idle.png", masks)
            before = byte_exact_tree_snapshot(source)

            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"raccoon/idle/1: subject touches source-cell edge|"
                r"raccoon/idle/1: bounds .* violate format-v2 safe stage|"
                r"raccoon/idle/1: the identity-locked action output offset "
                r".* clips this phase",
            ):
                contract.load_high_res_generated_action_sheet_species(
                    source, "raccoon", lock, roles=(IDLE,)
                )

            self.assertEqual(byte_exact_tree_snapshot(source), before)

    def test_imagegen_duplicate_action_sheet_cells_are_missing_animation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            lock = write_valid_imagegen_action_sheet_sources(source, "capybara")
            write_imagegen_action_sheet(
                source / "capybara" / "idle.png",
                [imagegen_action_logical_outline() for _phase in range(4)],
            )
            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"all four exact canonical PNGs must be distinct|"
                r"area import collapsed one or more phases",
            ):
                contract.load_high_res_generated_action_sheet_species(
                    source, "capybara", lock, roles=(IDLE,)
                )

    def test_imagegen_action_sheet_second_animal_is_rejected_before_sampling(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            lock = write_valid_imagegen_action_sheet_sources(source, "otter")
            masks = [imagegen_action_logical_outline(phase) for phase in range(4)]
            masks[3] = {
                (x, y)
                for left, right in ((8, 29), (36, 57))
                for y in range(29, 51)
                for x in range(left, right + 1)
                if x in (left, right) or y in (29, 50)
            }
            write_imagegen_action_sheet(source / "otter" / "idle.png", masks)

            with self.assertRaisesRegex(
                contract.RasterContractError,
                r"primary subject is only .* second subject",
            ):
                contract.load_high_res_generated_action_sheet_species(
                    source, "otter", lock, roles=(IDLE,)
                )

    def test_imagegen_transform_may_remove_only_centered_two_pixel_border(self) -> None:
        transform = imagegen_import_transform()
        subject_crop = replace(
            transform,
            crop_rect=(11, 1, 1111, 1376),
        )
        with self.assertRaises(contract.RasterContractError):
            contract.validate_imagegen_import_transform(subject_crop, "ferret")


if __name__ == "__main__":
    unittest.main(verbosity=2)
