"""Host-side model and source-contract checks for CompanionBrain.

The authoritative implementation is src/companion_brain.cpp.  This small
stdlib-only model keeps progression thresholds and encounter-filter behavior
executable on machines without a native C++ toolchain; the C++ harness beside
this file exercises the public API when a host compiler is available.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src" / "companion_brain.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "src" / "companion_brain.cpp").read_text(encoding="utf-8")

THRESHOLDS = (0, 15, 35, 65, 105, 155, 220, 300, 395, 505, 630)


def u32(value: int) -> int:
    return value & 0xFFFFFFFF


def fingerprint(text: str) -> int:
    value = 2_166_136_261
    for byte in text.encode("ascii"):
        value = u32((value ^ byte) * 16_777_619)
    return value


def mix32(value: int) -> int:
    value = u32(value ^ (value >> 16))
    value = u32(value * 0x7FEB352D)
    value = u32(value ^ (value >> 15))
    value = u32(value * 0x846CA68B)
    return u32(value ^ (value >> 16))


def bond_level(xp: int) -> int:
    return max(index for index, threshold in enumerate(THRESHOLDS)
               if xp >= threshold)


def evolution(xp: int, peers: int) -> int:
    level = bond_level(xp)
    if level >= 10 and peers >= 8:
        return 4
    if level >= 8 and peers >= 3:
        return 3
    if level >= 5 and peers >= 1:
        return 2
    if level >= 2:
        return 1
    return 0


class EncounterFilter:
    def __init__(self, device: int, pack_id: int) -> None:
        self.device = device
        self.pack_id = pack_id
        self.words = [0] * 8

    def bits(self, peer: int):
        first = mix32(peer ^ self.device)
        stride = mix32(peer ^ self.pack_id ^ 0xA5B35705) | 1
        for probe in range(3):
            yield u32(first + probe * stride + probe * probe * 17) & 0xFF

    def add(self, peer: int) -> bool:
        positions = tuple(self.bits(peer))
        seen = all(self.words[bit >> 5] & (1 << (bit & 31))
                   for bit in positions)
        for bit in positions:
            self.words[bit >> 5] |= 1 << (bit & 31)
        return not seen


class CompanionBrainModelTest(unittest.TestCase):
    def test_layout_and_schema_contract(self):
        self.assertIn("KITSU_BRAIN_SCHEMA_VERSION = 1", HEADER)
        self.assertIn("KITSU_BRAIN_MEMORY_CAPACITY = 24", HEADER)
        self.assertIn("KITSU_BRAIN_PERSISTED_BYTES = 300", HEADER)
        self.assertIn("KITSU_BRAIN_STORED_BYTES =", HEADER)
        self.assertIn("static_assert(sizeof(PersistedState)", HEADER)
        self.assertIn('"kitsu_brain"', SOURCE)
        self.assertIn('{"brain_a", "brain_b"}', SOURCE)
        self.assertIn("calculateCrc(candidate)", SOURCE)

    def test_cpp_thresholds_match_model(self):
        match = re.search(r"kThresholds\[11\]\s*=\s*\{([^}]+)\}", SOURCE)
        self.assertIsNotNone(match)
        values = tuple(map(int, re.findall(r"\d+", match.group(1))))
        self.assertEqual(values, THRESHOLDS)
        self.assertEqual([bond_level(x) for x in (0, 14, 15, 629, 630)],
                         [0, 0, 1, 9, 10])

    def test_evolution_requires_bond_and_unique_peers(self):
        self.assertEqual(evolution(34, 99), 0)
        self.assertEqual(evolution(35, 0), 1)
        self.assertEqual(evolution(155, 0), 1)
        self.assertEqual(evolution(155, 1), 2)
        self.assertEqual(evolution(395, 3), 3)
        self.assertEqual(evolution(630, 7), 3)
        self.assertEqual(evolution(630, 8), 4)

    def test_peer_replay_is_not_unique(self):
        peers = EncounterFilter(fingerprint("KTDEAD"), 0x13579BDF)
        first = fingerprint("KT1111")
        self.assertTrue(peers.add(first))
        self.assertFalse(peers.add(first))
        for suffix in range(2, 10):
            self.assertTrue(peers.add(fingerprint(f"KT{suffix:04d}")))

    def test_personality_seed_uses_uid_and_pack(self):
        uid = fingerprint("KTDEAD")
        seed_a = mix32(uid ^ mix32(0x13579BDF ^ 0x4B697473))
        seed_b = mix32(uid ^ mix32(0x13579BE0 ^ 0x4B697473))
        self.assertEqual(seed_a,
                         mix32(fingerprint("KTDEAD") ^
                               mix32(0x13579BDF ^ 0x4B697473)))
        self.assertNotEqual(seed_a, seed_b)

    def test_all_ui_labels_fit_ten_glyph_cells(self):
        blocks = []
        for name in ("personalityLabel", "moodLabel", "stageLabel",
                     "memoryText"):
            start = SOURCE.index(f"CompanionBrain::{name}")
            next_function = SOURCE.find("\n}\n\n", start)
            blocks.append(SOURCE[start:next_function])
        labels = re.findall(r'"([A-Z ]+)"', "".join(blocks))
        self.assertGreater(len(labels), 30)
        self.assertTrue(all(len(label) <= 10 for label in labels), labels)


if __name__ == "__main__":
    unittest.main(verbosity=2)
