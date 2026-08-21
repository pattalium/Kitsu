"""Regression tests for versioned stable firmware and pack release metadata."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import audit_kitsu_stable_bundle as auditor  # noqa: E402
import package_kitsu_stable as stable  # noqa: E402


class StableReleaseVersionTests(unittest.TestCase):
    def test_expected_version_is_part_of_the_fixed_accepted_identity(self) -> None:
        self.assertEqual(
            (
                stable.EXPECTED_FIRMWARE_VERSION,
                stable.EXPECTED_AUTHORIZATION_SHA256,
                stable.EXPECTED_EVIDENCE_SHA256,
                stable.EXPECTED_APPLICATION_SHA256,
                stable.EXPECTED_PARTITIONS_SHA256,
            ),
            (
                "0.10.2",
                "59bc4294966267b59a6738fd783310eeeade6d91e70f962d06b8c5d3a52e3dcf",
                "0430ac36d30a65c57b7209c8669fad695345796c3f8d4b37cf30c79c5f281b7f",
                "37c9f694d0d595115284297e6ed3fbf6de9c076f087b094e54d2b8a3a2cd30d9",
                "f9b22e16fcfb701520dd6c7e0791582ececbbd44c317c8d519e3d6b2b9ce8b7a",
            ),
        )
        self.assertEqual(
            stable.require_expected_firmware_version(stable.EXPECTED_FIRMWARE_VERSION),
            "0.10.2",
        )

    def test_requested_version_controls_pack_names_and_guides(self) -> None:
        version = "0.10.2"
        names = stable.public_pack_asset_names(version)

        self.assertEqual(
            names,
            (
                "Kitsu868-v0.10.2-cat.k868",
                "Kitsu868-v0.10.2-fox.k868",
                "Kitsu868-v0.10.2-dog.k868",
            ),
        )
        pack_guide = stable.installing_packs_guide(version)
        flashing_guide = stable.flashing_guide(version)
        for name in names:
            self.assertIn(name, pack_guide)
        self.assertIn("../Kitsu868-v0.10.2-cat.k868 --port COM3 --dry-run", pack_guide)
        self.assertIn("# Kitsu owner-reflashable firmware 0.10.2", flashing_guide)
        self.assertNotIn("0.10.0", pack_guide + flashing_guide)

    def test_future_version_does_not_fall_back_to_legacy_name(self) -> None:
        version = "1.2.3-rc.4"
        combined = stable.installing_packs_guide(version) + stable.flashing_guide(version)

        self.assertIn("Kitsu868-v1.2.3-rc.4-dog.k868", combined)
        self.assertIn("firmware 1.2.3-rc.4", combined)
        self.assertNotIn("0.10.0", combined)

    def test_default_audit_enforces_intrinsic_version_with_optional_repeat(self) -> None:
        manifest: dict[str, object] = {"firmware_version": "0.10.2"}

        self.assertEqual(auditor.audited_firmware_version(manifest), "0.10.2")
        self.assertEqual(
            auditor.audited_firmware_version(
                manifest, expected_firmware_version="0.10.2"
            ),
            "0.10.2",
        )
        with self.assertRaisesRegex(AssertionError, "wrong firmware version"):
            auditor.audited_firmware_version(
                manifest, expected_firmware_version="0.10.0"
            )

    def test_unrelated_versions_are_rejected_by_packager_and_unpinned_auditor(self) -> None:
        unrelated_versions = (
            "999.0.0",
            "unrelated-build",
            "0.10.2_unrelated_build",
        )
        for version in unrelated_versions:
            with self.subTest(version=version, component="packager"):
                with self.assertRaises(SystemExit):
                    stable.require_expected_firmware_version(version)
            with self.subTest(version=version, component="auditor"):
                with self.assertRaises(AssertionError):
                    auditor.audited_firmware_version({"firmware_version": version})

    def test_main_rejects_unrelated_version_before_processing_release_inputs(self) -> None:
        common_arguments = [
            "package_kitsu_stable.py",
            "--project-root",
            ".",
            "--build-dir",
            ".",
            "--output-dir",
            "unused-output",
            "--esptool",
            "missing-esptool",
            "--physical-qa-authorization",
            "missing-authorization",
            "--physical-qa-evidence",
            "missing-evidence",
        ]
        for version in ("999.0.0", "unrelated-build", "0.10.2_unrelated_build"):
            with self.subTest(version=version), patch.object(
                sys,
                "argv",
                [*common_arguments, "--firmware-version", version],
            ):
                with self.assertRaises(SystemExit):
                    stable.main()

    def test_public_version_grammar_matches_flash_site(self) -> None:
        for version in ("0.10.2", "1.2.3-rc.4", "1.2.3+build.5"):
            with self.subTest(version=version):
                self.assertEqual(stable.require_public_firmware_version(version), version)

        for version in (
            "unrelated-build",
            "1.2",
            "1.2.3_rc1",
            "1.2.3-rc+build",
            "1.2.3-",
            "1.2.3+",
            "1.2.3+" + "a" * 59,
        ):
            with self.subTest(version=version):
                with self.assertRaises(SystemExit):
                    stable.require_public_firmware_version(version)

    def test_version_validation_rejects_path_syntax(self) -> None:
        with self.assertRaises(SystemExit):
            stable.public_pack_asset_names("../0.10.2")
        with self.assertRaisesRegex(AssertionError, "firmware version is invalid"):
            auditor.audited_firmware_version({"firmware_version": "../0.10.2"})
        with self.assertRaisesRegex(
            AssertionError, "expected firmware version is invalid"
        ):
            auditor.audited_firmware_version(
                {"firmware_version": "0.10.2"},
                expected_firmware_version="../0.10.2",
            )


if __name__ == "__main__":
    unittest.main()
