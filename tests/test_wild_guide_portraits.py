"""Exact-data and layout checks for native wild-creature OLED portraits."""

from __future__ import annotations

import hashlib
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PORTRAITS = (ROOT / "src" / "wild_guide_portraits.cpp").read_text(
    encoding="utf-8"
)
CATALOG = (ROOT / "src" / "wild_creature_catalog.cpp").read_text(
    encoding="utf-8"
)
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")

EXPECTED_IDLE_SHA256 = {
    "frog": "34dd6807c5631cdc98c80629267ab6d1a0b75218b573a69373c493bc6c8463bd",
    "hamster": "8c60c5414dbb5e45b14bd0168645c862deaca94e3a71b5c42a9905eb53438113",
    "turtle": "7474f9f844a66619fd000b3a35cc239ef9df1c5096e5dd99c6dc5b7ed2a393be",
    "rabbit": "da4494901a6405407cfc183215400241924a49cc76bccf8c44dc7e9576d50e91",
    "hedgehog": "9b56f6d67362284333a8dc38506ac8c6193a7c49590f3a83c6399843b3f21bfe",
    "ferret": "a2162d036481fcf4a79a8e24b1b51fbb7282881b8125c898060e7f7997c6ab03",
    "otter": "0610237f4122e74a5c9f7803d030628c5edc008750ba226af6820eea494f8d2d",
    "axolotl": "2b394dc83216005070e16babded5cdfcdb09b6c00a0e8655f94acdec8c95e95a",
    "chinchilla": "4a73778a5619a5edcb127059c5a03bfcd06ee4f4b0053274a51e63ff16c2efe2",
    "raccoon": "c4534ed35f67369ba3ec95f640b8c0485489f2f65dcb2204d3753f32b50bef7d",
    "capybara": "fef90cae297a519f04cb552c3bacbc28acb3c0a6e4a6217ae79610f45d1e947a",
    "sugar_glider": "20ca07d9255649c1923caaf0064ea4ef0c57f9f7f161a80e7959babe66dab3c0",
    "red_panda": "338d493f868ac2fdfab0fc7613873d449386303d8594657bd0c910ff10c640d4",
    "pangolin": "4d516a42eea58bf7c817d953aaab103633dda2ee53942411aebae64fc17546ba",
    "tasmanian_devil": "25ee30cafa5c85e82946a43751752f19c6da7fb11bbd4aaf66f53131cdd9f5ae",
    "snow_leopard": "a9c2853fc68e9f4ae5924c545ce413829c3eeb8211d089f898b4fd4dd728a3d3",
    "okapi": "ad7f501c137e704427e994f0f9d4a16d05047d9783399eed7365225766a18ebf",
    "shoebill": "9cc6a9496abc887188aae98acd253a93b19c5dfc32560cd31a6cfcaf77d16754",
    "cat_girl": "f33940e5b43311a01e8e71196d3e7036584beccaa579af91a83554910398519d",
    "rabbit_girl": "660000654b77b6655415aa9bfa7623c5e30b72bf161f6865d47b2f618f86710d",
    "deer_girl": "94496220241c2ba4c96bc1f2e0214ee2d893c03099392aa5c4124fb23fd3c82e",
}

EXPECTED_PORTRAIT_ORDER = (
    "Frog",
    "Hamster",
    "Turtle",
    "Rabbit",
    "Hedgehog",
    "Ferret",
    "Otter",
    "Axolotl",
    "Chinchilla",
    "Raccoon",
    "Capybara",
    "SugarGlider",
    "RedPanda",
    "Pangolin",
    "TasmanianDevil",
    "SnowLeopard",
    "Okapi",
    "Shoebill",
    "CatGirl",
    "RabbitGirl",
    "DeerGirl",
)


def wrap_name(name: str, capacity: int = 12) -> tuple[list[str], str]:
    """Model the firmware's two-line, word-aware name path."""
    remaining = name.strip()
    lines: list[str] = []
    while remaining and len(lines) < 2:
        if len(remaining) <= capacity:
            lines.append(remaining)
            remaining = ""
            continue
        break_at = remaining.rfind(" ", 0, capacity + 1)
        if break_at <= 0:
            break_at = capacity
        lines.append(remaining[:break_at])
        remaining = remaining[break_at:].lstrip()
    return lines, remaining


class WildGuidePortraitContractTest(unittest.TestCase):
    def test_all_21_embedded_frames_are_exact_64x80_idle_bytes(self) -> None:
        entries = re.findall(
            r"\{\s*// ([a-z_]+): ([0-9a-f]{64})\s*(.*?)\n\s*\},",
            PORTRAITS,
            re.S,
        )
        self.assertEqual(len(entries), 21)
        self.assertEqual([identity for identity, _, _ in entries],
                         list(EXPECTED_IDLE_SHA256))
        for identity, declared_sha, body in entries:
            with self.subTest(identity=identity):
                frame = bytes(
                    int(value, 16)
                    for value in re.findall(r"0x([0-9a-f]{2})", body)
                )
                self.assertEqual(len(frame), 64 * 80 // 8)
                expected_sha = EXPECTED_IDLE_SHA256[identity]
                self.assertEqual(declared_sha, expected_sha)
                self.assertEqual(hashlib.sha256(frame).hexdigest(), expected_sha)
                self.assertTrue(any(frame))

    def test_table_has_an_explicit_catalog_identity_order(self) -> None:
        order = re.search(
            r"kGuidePortraitOrder\[kCatalogCreatureCount\]\s*=\s*\{(.*?)\};",
            PORTRAITS,
            re.S,
        )
        self.assertIsNotNone(order)
        self.assertEqual(
            tuple(re.findall(r"Portrait::([A-Za-z]+)", order.group(1))),
            EXPECTED_PORTRAIT_ORDER,
        )

    def test_every_catalog_name_fits_without_ellipsis(self) -> None:
        names = re.findall(r'\{0x[0-9a-f]+UL, "([^"]+)"', CATALOG)
        self.assertEqual(len(names), 21)
        for name in names:
            with self.subTest(name=name):
                lines, remaining = wrap_name(name)
                self.assertFalse(remaining)
                self.assertLessEqual(len(lines), 2)
                self.assertTrue(all(len(line) <= 12 for line in lines))
                self.assertEqual(" ".join(lines), name)
        self.assertEqual(wrap_name("Tasmanian Devil"),
                         (["Tasmanian", "Devil"], ""))
        self.assertIn("uiWrappedText(creature.name, 0, 2U, 8U);", MAIN)
        self.assertIn(
            "uiWrappedText(wildEncounterView.creature.name, 0, 2U, 8U);",
            MAIN,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
