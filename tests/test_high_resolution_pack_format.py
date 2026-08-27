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
TESTS = ROOT / "tests"
if str(TESTS) not in sys.path:
    sys.path.insert(0, str(TESTS))

import build_default_packs as builder  # noqa: E402
import build_wild_packs as wild_builder  # noqa: E402
import companion_raster_contract as raster_contract  # noqa: E402
import make_companion_gif_sheet as gif_sheet  # noqa: E402
import sync_wild_portraits as portrait_sync  # noqa: E402
from test_companion_raster_contract import (  # noqa: E402
    imagegen_action_logical_outline,
    imagegen_logical_outline,
    refresh_test_generated_semantic_lock,
    write_imagegen_action_sheet,
    write_imagegen_raw,
    write_valid_imagegen_action_sheet_sources,
    write_valid_imagegen_sources,
)
from install_pack import PackValidationError, validate_pack  # noqa: E402


STARTER_SHA256 = {
    "cat": "8d19d6b8bc584d9aaee5a6867504fd23c1862c907bbeb1affd9611e35bf2a6d7",
    "dog": "8652aad28816d52fca334766ebefb5c38aec1b09dcc72783414998d17a46e261",
    "fox": "c868386770b6083dcd8f7c01ec7fe455faec476a96c724ab62f09770fdcdab38",
}


def write_direct_v2_species(
    root: Path, species: str
) -> raster_contract.HighResIdentityLock:
    species_dir = root / species
    species_dir.mkdir(parents=True)
    identity_path = species_dir / "identity.png"
    identity = Image.new("1", (64, 80), 1)
    ImageDraw.Draw(identity).rectangle((20, 30, 43, 77), fill=0)
    identity.save(identity_path)

    portrait = Image.new("1", (16, 18), 1)
    ImageDraw.Draw(portrait).rectangle((4, 3, 11, 15), fill=0)
    portrait.save(species_dir / "portrait.png")

    cutouts = ((24, 40), (29, 40), (34, 40), (39, 40))
    for role in builder.ROLE_SPECS:
        for phase, (x, y) in enumerate(cutouts):
            frame = Image.new("1", (64, 80), 1)
            draw = ImageDraw.Draw(frame)
            draw.rectangle((20, 30, 43, 77), fill=0)
            draw.rectangle((x, y, x + 1, y + 1), fill=1)
            path = species_dir / role.name / f"{phase:02d}.png"
            path.parent.mkdir(parents=True, exist_ok=True)
            frame.save(path)

    return raster_contract.HighResIdentityLock(
        identity_key=species,
        identity_sha256=raster_contract.sha256_file(identity_path),
        frame_canvas=(64, 80),
        approved=True,
    )


class HighResolutionPackFormatTests(unittest.TestCase):
    def validate_bytes(self, payload: bytes):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "candidate.k868"
            path.write_bytes(payload)
            return validate_pack(path)

    def test_v2_is_exact_native_64_by_80_and_fits_the_existing_slot(self):
        frames = [bytes(640) for _ in range(48)]
        pack = builder.build_pack(
            "test-native",
            "TEST NATIVE",
            frames,
            format_version=2,
            frame_width=64,
            frame_height=80,
        )
        self.assertEqual(len(pack), 64 + 12 * 12 + 48 * 4 + 48 * 640)
        self.assertEqual(len(pack), 31_120)
        self.assertLessEqual(len(pack), 0x140000)
        self.assertAlmostEqual(len(pack) / 0x140000, 0.02374267578125)

        info = self.validate_bytes(pack)
        self.assertEqual(info.format_version, 2)
        self.assertEqual((info.width, info.height), (64, 80))
        self.assertEqual(info.total_bytes, 31_120)

    def test_v1_default_serializer_stays_byte_compatible(self):
        frames = [bytes(512) for _ in range(48)]
        implicit = builder.build_pack("compat", "COMPAT", frames)
        explicit = builder.build_pack(
            "compat",
            "COMPAT",
            frames,
            format_version=1,
            frame_width=64,
            frame_height=64,
        )
        self.assertEqual(implicit, explicit)
        info = self.validate_bytes(implicit)
        self.assertEqual(info.format_version, 1)
        self.assertEqual((info.width, info.height), (64, 64))
        self.assertEqual(info.total_bytes, 24_976)

    def test_version_and_canvas_must_be_the_canonical_pair(self):
        frames = [bytes(640) for _ in range(48)]
        pack = bytearray(
            builder.build_pack(
                "mismatch",
                "MISMATCH",
                frames,
                format_version=2,
                frame_width=64,
                frame_height=80,
            )
        )
        pack[0x22:0x24] = (64).to_bytes(2, "little")
        with self.assertRaisesRegex(PackValidationError, "version/canvas 2/64x64"):
            self.validate_bytes(bytes(pack))

        with self.assertRaisesRegex(ValueError, "version/canvas 2/64x64"):
            builder.build_pack(
                "mismatch",
                "MISMATCH",
                [bytes(512) for _ in range(48)],
                format_version=2,
                frame_width=64,
                frame_height=64,
            )

    def test_frame_encoder_never_clips_or_wraps_out_of_canvas_pixels(self):
        encoded = builder.frame_bytes({(0, 0), (63, 79)}, 64, 80)
        self.assertEqual(len(encoded), 640)
        self.assertEqual(encoded[0], 0x01)
        self.assertEqual(encoded[-1], 0x80)
        with self.assertRaisesRegex(ValueError, "outside 64x80"):
            builder.frame_bytes({(64, 79)}, 64, 80)
        with self.assertRaisesRegex(ValueError, "outside 64x80"):
            builder.frame_bytes({(63, 80)}, 64, 80)

    def test_approved_starter_pack_blobs_are_unchanged(self):
        for species, expected in STARTER_SHA256.items():
            path = ROOT / "assets" / "packs" / f"{species}.k868"
            actual = hashlib.sha256(path.read_bytes()).hexdigest()
            self.assertEqual(actual, expected, species)
            info = validate_pack(path)
            self.assertEqual(info.format_version, 1, species)
            self.assertEqual((info.width, info.height), (64, 64), species)
            self.assertEqual(info.total_bytes, 24_976, species)

    def test_renderer_has_separate_non_overlapping_v2_layout(self):
        source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn("drawCreatureSprite(20, 16)", source)
        self.assertIn("drawCreatureSprite(25, 17)", source)
        self.assertIn("drawCreatureSprite(22, 16)", source)
        self.assertIn("highResolution ? 99 : 93", source)
        self.assertIn("highResolution ? 113 : 110", source)
        self.assertLess(16 + 80 - 1, 99)
        self.assertLess(17 + 80 - 1, 101)
        self.assertLess(16 + 80 - 1, 99)

    def test_gif_inspector_decodes_all_native_v2_rows(self):
        native_frame = bytearray(640)
        native_frame[-1] = 0x80
        pack = builder.build_pack(
            "gif-v2",
            "GIF V2",
            [bytes(native_frame) for _ in range(48)],
            format_version=2,
            frame_width=64,
            frame_height=80,
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "gif-v2.k868"
            path.write_bytes(pack)
            parsed = gif_sheet.parse_pack(path)
        self.assertEqual(parsed.format_version, 2)
        self.assertEqual((parsed.width, parsed.height), (64, 80))
        self.assertEqual(parsed.frames[0].getpixel((63, 79)), 255)

    def test_wild_builder_serializes_only_validated_exact_v2_frames(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            output = root / "output"
            lock = write_direct_v2_species(source, "ferret")
            result = wild_builder.build_species(
                source,
                output,
                "ferret",
                lock,
                "direct-exact-target",
            )
            snapshot = wild_builder.snapshot_species_sources(
                source,
                output,
                "ferret",
                dict(result["source_sha256"]),
            )
            result["source_snapshot"] = {"byte_exact_sha256": snapshot}

            info = validate_pack(output / "ferret.k868")
            self.assertEqual(result["raster_transform"], "none-direct-exact-target")
            self.assertEqual(result["pack_bytes"], 31_120)
            self.assertEqual((info.format_version, info.width, info.height), (2, 64, 80))
            self.assertEqual(len(snapshot), 50)
            self.assertEqual(
                result["portrait"]["png_sha256"],
                result["source_sha256"]["portrait.png"],
            )
            self.assertTrue(
                all(role["unique_frames"] == 4 for role in result["roles"])
            )
            portrait_sync.require_pack_raster_provenance(result, "ferret")

            portrait_sync.require_fail_closed_manifest(
                {
                    "schema": wild_builder.PRIVATE_MANIFEST_SCHEMA,
                    "complete_roster": True,
                    "non_destructive_build": True,
                    "identity_lock_schema": (
                        raster_contract.HIGH_RES_IDENTITY_LOCK_SCHEMA
                    ),
                    "animation_contract": dict(
                        portrait_sync.REQUIRED_ANIMATION_CONTRACT_VALUES
                    ),
                    "raster_contract": {
                        "allowed_pack_transforms": [
                            wild_builder.DIRECT_RASTER_TRANSFORM,
                            wild_builder.IMAGEGEN_RASTER_TRANSFORM,
                        ],
                        **wild_builder.TRANSFORM_CONTROLS,
                        "source_snapshots": True,
                        "portrait_source": (
                            "independently-authored-exact-16x18"
                        ),
                        "portrait_resampling": "none",
                    },
                }
            )

    def test_wild_builder_rejects_legacy_identity_lock_schema(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "legacy-lock.json"
            path.write_text(
                json.dumps(
                    {
                        "schema": raster_contract.IDENTITY_LOCK_SCHEMA,
                        "identities": [],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "legacy v1 locks.*forbidden"):
                wild_builder.load_release_identity_locks(path, ["ferret"])

            path.write_text(
                json.dumps(
                    {
                        "schema": "kitsu-wild-imagegen-import-lock-v1",
                        "identities": [],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "legacy v1 locks.*forbidden"):
                wild_builder.load_release_identity_locks(path, ["ferret"])

            path.write_text(
                json.dumps(
                    {
                        "schema": "kitsu-wild-imagegen-import-lock-v2",
                        "identities": [],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                ValueError, "unsafe ImageGen v2/v3 action locks are forbidden"
            ):
                wild_builder.load_release_identity_locks(path, ["ferret"])

        with self.assertRaisesRegex(ValueError, "unsupported or legacy identity lock"):
            portrait_sync.require_fail_closed_manifest(
                {
                    "schema": wild_builder.PRIVATE_MANIFEST_SCHEMA,
                    "complete_roster": True,
                    "non_destructive_build": True,
                    "identity_lock_schema": "kitsu-wild-imagegen-import-lock-v1",
                    "animation_contract": dict(
                        portrait_sync.REQUIRED_ANIMATION_CONTRACT_VALUES
                    ),
                    "raster_contract": dict(
                        portrait_sync.EXPECTED_RASTER_CONTRACT
                    ),
                }
            )

    def test_independent_imagegen_frames_allow_shared_identity_offset(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            output = root / "output"
            lock = write_valid_imagegen_sources(source, "red_panda")
            equal_transform = replace(
                lock.transform,
                output_offset=(0, 17),
                action_output_offset=(0, 17),
            )
            species_dir = source / "red_panda"

            def red_panda_source_placement(
                mask: set[tuple[int, int]],
            ) -> set[tuple[int, int]]:
                return {(x, y + 10) for x, y in mask}

            write_imagegen_raw(
                species_dir / "identity.png",
                equal_transform,
                red_panda_source_placement(imagegen_logical_outline()),
            )
            for role in builder.ROLE_SPECS:
                for phase in range(4):
                    write_imagegen_raw(
                        species_dir / role.name / f"{phase:02d}.png",
                        equal_transform,
                        red_panda_source_placement(
                            imagegen_logical_outline(phase)
                        ),
                    )
            identity = raster_contract.load_imagegen_import_frame(
                species_dir / "identity.png",
                "identity",
                -1,
                equal_transform,
            )
            lock = replace(
                lock,
                identity_source_sha256=raster_contract.sha256_file(
                    species_dir / "identity.png"
                ),
                identity_frame_sha256=hashlib.sha256(
                    identity.packed
                ).hexdigest(),
                transform=equal_transform,
                transform_sha256=(
                    raster_contract.imagegen_import_transform_sha256(
                        equal_transform
                    )
                ),
            )
            lock = refresh_test_generated_semantic_lock(
                source,
                "red_panda",
                lock,
                builder.ROLE_SPECS,
                source_layout="independent-frame",
            )

            result = wild_builder.build_species(
                source,
                output,
                "red_panda",
                lock,
                "imagegen-locked-import",
            )
            result["source_snapshot"] = {
                "byte_exact_sha256": dict(result["source_sha256"])
            }

        self.assertEqual(
            result["identity_lock"]["transform"]["action_output_offset"],
            result["identity_lock"]["transform"]["output_offset"],
        )
        self.assertEqual(
            result["identity_lock"]["transform"]["output_offset"],
            [0, 17],
        )
        self.assertNotIn("action_output_offset", result)
        self.assertNotIn("action_source_layout", result)
        self.assertNotIn("action_source_layout_sha256", result)
        self.assertTrue(
            all("action_output_offset" not in role for role in result["roles"])
        )
        portrait_sync.require_pack_raster_provenance(result, "red_panda")
        for role_record in result["roles"]:
            portrait_sync.require_generated_semantic_evidence(
                "red_panda",
                role_record,
                result["identity_lock"],
                result["source_sha256"],
            )

        drifted_role = json.loads(json.dumps(result["roles"][3]))
        drifted_role["semantic_locality"]["phases"][2][
            "composition_baseline_frame_sha256"
        ] = "f" * 64
        with self.assertRaisesRegex(ValueError, r"baseline hash drifted"):
            portrait_sync.require_generated_semantic_evidence(
                "red_panda",
                drifted_role,
                result["identity_lock"],
                result["source_sha256"],
            )

    def test_action_sheet_imagegen_source_kind_is_rejected_by_v4(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            output = root / "output"
            lock = write_valid_imagegen_action_sheet_sources(source, "ferret")
            equal_transform = replace(
                lock.transform,
                action_output_offset=lock.transform.output_offset,
            )
            lock = replace(
                lock,
                transform=equal_transform,
                transform_sha256=(
                    raster_contract.imagegen_import_transform_sha256(
                        equal_transform
                    )
                ),
            )

            with self.assertRaisesRegex(
                ValueError,
                r"v4 forbids one-action sheets.*independent full-canvas",
            ):
                wild_builder.build_species(
                    source,
                    output,
                    "ferret",
                    lock,
                    "imagegen-one-action-sheets",
                )

    def test_wild_builder_rejects_even_well_formed_legacy_action_sheets(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            output = root / "output"
            lock = write_valid_imagegen_action_sheet_sources(source, "ferret")
            with self.assertRaisesRegex(
                ValueError,
                r"v4 forbids one-action sheets.*independent full-canvas",
            ):
                wild_builder.build_species(
                    source,
                    output,
                    "ferret",
                    lock,
                    "imagegen-one-action-sheets",
                )

    def test_portrait_sync_pins_the_complete_imagegen_transform(self):
        transform = raster_contract.recommended_imagegen_import_transform(
            action_output_offset=(-1, 30)
        )
        transform_record = raster_contract.imagegen_import_transform_record(transform)
        source_hashes = {
            "identity.png": "a" * 64,
            "portrait.png": "b" * 64,
        }
        semantic_contract = {
            "schema": raster_contract.GENERATED_ACTION_SEMANTIC_SCHEMA,
            "roles": [{"role": role.name} for role in builder.ROLE_SPECS],
        }
        semantic_payload = json.dumps(
            semantic_contract,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
        ).encode("ascii")
        pack = {
            "raster_transform": wild_builder.IMAGEGEN_RASTER_TRANSFORM,
            "source_kind": "imagegen-locked-import",
            "transform_controls": dict(wild_builder.TRANSFORM_CONTROLS),
            "format_version": 2,
            "frame_canvas": [64, 80],
            "pack_bytes": 31_120,
            "identity_lock": {
                "schema": raster_contract.IMAGEGEN_IMPORT_LOCK_SCHEMA,
                "action_semantic_contract": semantic_contract,
                "action_semantic_contract_sha256": hashlib.sha256(
                    semantic_payload
                ).hexdigest(),
                "identity_source_sha256": "a" * 64,
                "identity_frame_sha256": "c" * 64,
                "transform": transform_record,
                "transform_sha256": (
                    raster_contract.imagegen_import_transform_sha256(transform)
                ),
            },
            "fixed_action_scale": 64 / (
                transform.crop_rect[2] - transform.crop_rect[0]
            ),
            "identity_raster_scale": 64 / (
                transform.crop_rect[2] - transform.crop_rect[0]
            ),
            "action_cell_raster_scale": 64 / (
                transform.crop_rect[2] - transform.crop_rect[0]
            ),
            "source_sha256": source_hashes,
            "source_snapshot": {"byte_exact_sha256": dict(source_hashes)},
        }
        portrait_sync.require_pack_raster_provenance(pack, "ferret")

        drifted = json.loads(json.dumps(pack))
        drifted["identity_lock"]["transform"]["output_offset"][0] += 1
        with self.assertRaisesRegex(ValueError, "transform hash does not match"):
            portrait_sync.require_pack_raster_provenance(drifted, "ferret")

        missing_transform_offset = json.loads(json.dumps(pack))
        del missing_transform_offset["identity_lock"]["transform"][
            "action_output_offset"
        ]
        with self.assertRaisesRegex(ValueError, "unexpected fields"):
            portrait_sync.require_pack_raster_provenance(
                missing_transform_offset, "ferret"
            )

        independent_override = json.loads(json.dumps(pack))
        independent_override["action_output_offset"] = list(
            transform.action_output_offset
        )
        with self.assertRaisesRegex(ValueError, "independent-frame layout is invalid"):
            portrait_sync.require_pack_raster_provenance(
                independent_override, "ferret"
            )

        legacy_action_sheet = json.loads(json.dumps(pack))
        legacy_action_sheet["raster_transform"] = (
            "rgba-over-white-fixed-action-sheet-cells-box-area-black-coverage-"
            "then-fixed-offset"
        )
        legacy_action_sheet["source_kind"] = "imagegen-one-action-sheets"
        with self.assertRaisesRegex(
            ValueError, r"not built by the fail-closed raster path"
        ):
            portrait_sync.require_pack_raster_provenance(
                legacy_action_sheet, "ferret"
            )

        equal_offsets = json.loads(json.dumps(pack))
        equal_offsets["identity_lock"]["transform"]["action_output_offset"] = list(
            equal_offsets["identity_lock"]["transform"]["output_offset"]
        )
        canonical = json.dumps(
            equal_offsets["identity_lock"]["transform"],
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
        ).encode("ascii")
        equal_offsets["identity_lock"]["transform_sha256"] = hashlib.sha256(
            canonical
        ).hexdigest()
        self.assertNotIn("action_output_offset", equal_offsets)
        portrait_sync.require_pack_raster_provenance(equal_offsets, "ferret")

if __name__ == "__main__":
    unittest.main()
