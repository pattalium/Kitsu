from __future__ import annotations

import hashlib
import struct
import unittest
from dataclasses import dataclass, field
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TRANSPORT = (ROOT / "src" / "kitsu_mesh_transport.cpp").read_text(
    encoding="utf-8"
)
TRANSPORT_HEADER = (ROOT / "src" / "kitsu_mesh_transport.h").read_text(
    encoding="utf-8"
)
CHAT_HEADER = (ROOT / "src" / "kitsu_chat_contract.h").read_text(
    encoding="utf-8"
)
CHAT = (ROOT / "src" / "kitsu_chat_contract.cpp").read_text(
    encoding="utf-8"
)
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
SESSION = (ROOT / "src" / "kitsu_ble_session.cpp").read_text(
    encoding="utf-8"
)
REPEAT_WIRE = (ROOT / "src" / "kitsu_repeat_wire.cpp").read_text(
    encoding="utf-8"
)
CHANNEL_TRACKER = (
    ROOT / "src" / "kitsu_channel_repeat_tracker.cpp"
).read_text(encoding="utf-8")
ADVERT_TRACKER = (
    ROOT / "src" / "kitsu_advert_repeat_tracker.cpp"
).read_text(encoding="utf-8")


def cpp_function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for cursor in range(opening, len(source)):
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
            if depth == 0:
                return source[start : cursor + 1]
    raise AssertionError(f"unterminated C++ function: {signature}")


UINT32_MAX = 0xFFFFFFFF
RECOVERED_V1_SHA256 = (
    "95849ab709747f0eeb33923751589672e255f45671e6d86330d9f8cc7bc5bdb4"
)
PUBLIC_SECRET = bytes.fromhex(
    "8B3387E9C5CDEA6AC9E5EDBAA115CD72" + "00" * 16
)


def messaging_crc(record: bytes) -> int:
    crc = UINT32_MAX
    for index, byte in enumerate(record):
        value = 0 if 8 <= index < 12 else byte
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)))
    return (~crc) & UINT32_MAX


def recovered_v1_record() -> bytes:
    header = struct.pack("<4sHHI", b"KMS1", 1, 1920, 0xD2E6D416)
    contacts = bytes(137 * 12)
    public_name = b"Public\0" + bytes(33 - 7)
    channel0 = struct.pack("<B33s32s", 1, public_name, PUBLIC_SECRET)
    record = header + contacts + channel0 + bytes(66 * 3)
    assert len(record) == 1920
    return record


def finalize_compact(record: bytearray) -> bytes:
    record[8:12] = bytes(4)
    record[8:12] = struct.pack("<I", messaging_crc(bytes(record)))
    return bytes(record)


def compact_default(generation: int = 1) -> bytes:
    record = bytearray(1148)
    record[:16] = struct.pack("<4sHHII", b"KMS1", 2, 1148, 0, generation)
    channel0 = 16 + 72 * 12
    record[channel0] = 1
    record[channel0 + 1 : channel0 + 8] = b"Public\0"
    record[channel0 + 34 : channel0 + 66] = PUBLIC_SECRET
    record[channel0 + 66] = 0
    return finalize_compact(record)


def compact_semantically_valid(record: bytes) -> bool:
    if len(record) != 1148:
        return False
    magic, schema, record_bytes, stored_crc, generation = struct.unpack(
        "<4sHHII", record[:16]
    )
    if (
        magic != b"KMS1"
        or schema != 2
        or record_bytes != len(record)
        or generation == 0
        or stored_crc != messaging_crc(record)
    ):
        return False
    for index in range(12):
        contact = record[16 + index * 72 : 16 + (index + 1) * 72]
        used, pinned = contact[0], contact[1]
        if used not in (0, 1) or pinned not in (0, 1):
            return False
        if used == 0 and any(contact[1:]):
            return False
        if used == 1 and pinned != 1:
            return False
    channels = 16 + 72 * 12
    for index in range(4):
        channel = record[channels + index * 67 : channels + (index + 1) * 67]
        used, scope = channel[0], channel[66]
        if used not in (0, 1) or scope not in (0, 1):
            return False
        if index == 0 and (used != 1 or scope != 0):
            return False
        if used == 0 and (any(channel[1:66]) or scope != 0):
            return False
    return True


@dataclass(frozen=True)
class CompactRecordModel:
    generation: int
    payload: bytes
    valid: bool = True


def generation_order_model(a: int, b: int) -> int:
    delta = (a - b) & UINT32_MAX
    if delta in (0, 0x80000000):
        return 0
    return 1 if delta < 0x80000000 else -1


def select_compact_model(
    a: CompactRecordModel | None, b: CompactRecordModel | None
) -> str | None:
    valid_a = a is not None and a.valid and a.generation != 0
    valid_b = b is not None and b.valid and b.generation != 0
    if valid_a and valid_b:
        assert a is not None and b is not None
        order = generation_order_model(a.generation, b.generation)
        if order > 0:
            return "state2a"
        if order < 0:
            return "state2b"
        if a.generation == b.generation and a == b:
            return "state2a"
        return None
    if valid_a:
        return "state2a"
    if valid_b:
        return "state2b"
    return None


@dataclass
class FaultStoreModel:
    records: dict[str, bytes | CompactRecordModel] = field(default_factory=dict)
    read_errors: set[str] = field(default_factory=set)
    preerase_fail: set[str] = field(default_factory=set)
    cleanup_fail: set[str] = field(default_factory=set)
    invalidate_fail: set[str] = field(default_factory=set)
    put_fault: str | None = None
    readback_fault: str | None = None

    def erase(self, key: str, phase: str) -> bool:
        failures = {
            "pre": self.preerase_fail,
            "cleanup": self.cleanup_fail,
            "invalidate": self.invalidate_fail,
        }[phase]
        if key in failures:
            return False
        self.records.pop(key, None)
        return True


def boot_model(store: FaultStoreModel) -> tuple[str, str | None]:
    if store.read_errors & {"state2a", "state2b", "state1"}:
        return "failed", "read_failed"
    a = store.records.get("state2a")
    b = store.records.get("state2b")
    compact_a = a if isinstance(a, CompactRecordModel) else None
    compact_b = b if isinstance(b, CompactRecordModel) else None
    selected = select_compact_model(compact_a, compact_b)
    if selected is not None:
        return "compact", selected
    legacy = store.records.get("state1")
    if legacy == recovered_v1_record():
        return "legacy", "state2_invalid_legacy_usable" if a or b else "legacy_migration_pending"
    if legacy is not None:
        return "failed", "orphaned_legacy_record"
    return "failed", "state2_invalid" if a or b else "missing_record"


def commit_model(
    store: FaultStoreModel,
    active: str | None,
    previous_schema: int,
    expected: CompactRecordModel,
) -> dict[str, object]:
    target = "state2b" if active == "state2a" else "state2a"

    if active is not None and "state1" in store.records:
        if not store.erase("state1", "pre"):
            return {"ok": False, "usable": True, "active": active, "reason": "clear_failed"}
    if active is None and previous_schema == 1:
        for key in ("state2a", "state2b"):
            if not store.erase(key, "pre"):
                return {"ok": False, "usable": True, "active": None, "reason": "clear_failed"}
    if not store.erase(target, "pre"):
        return {"ok": False, "usable": True, "active": active, "reason": "clear_failed"}

    if store.put_fault == "enospc":
        store.records[target] = CompactRecordModel(expected.generation, b"partial", False)
        if not store.erase(target, "invalidate"):
            return {"ok": False, "usable": False, "active": active, "reason": "commit_ambiguous"}
        return {"ok": False, "usable": True, "active": active, "reason": "write_failed"}

    store.records[target] = expected
    if store.readback_fault == "reopen":
        store.erase(target, "invalidate")
        return {"ok": False, "usable": False, "active": active, "reason": "commit_ambiguous"}
    if store.readback_fault is not None:
        if store.readback_fault == "corrupt":
            store.records[target] = CompactRecordModel(expected.generation, b"corrupt", False)
        if not store.erase(target, "invalidate"):
            return {"ok": False, "usable": False, "active": active, "reason": "commit_ambiguous"}
        return {"ok": False, "usable": True, "active": active, "reason": "verify_failed"}

    cleanup_ok = True
    for key in filter(None, (active, "state1" if previous_schema == 1 else None)):
        cleanup_ok = store.erase(key, "cleanup") and cleanup_ok
    stale = "state2b" if target == "state2a" else "state2a"
    if stale != active:
        cleanup_ok = store.erase(stale, "cleanup") and cleanup_ok
    return {
        "ok": True,
        "usable": True,
        "active": target,
        "reason": "cleanup_pending" if not cleanup_ok else "ready",
        "cleanup_pending": not cleanup_ok,
    }


class RecoveredPersistenceFixtureTests(unittest.TestCase):
    def test_exact_recovered_v1_record_identity_and_fields(self) -> None:
        record = recovered_v1_record()
        self.assertEqual(len(record), 1920)
        self.assertEqual(hashlib.sha256(record).hexdigest(), RECOVERED_V1_SHA256)
        self.assertEqual(record[:4], b"KMS1")
        self.assertEqual(struct.unpack("<H", record[4:6])[0], 1)
        self.assertEqual(struct.unpack("<H", record[6:8])[0], 1920)
        self.assertEqual(struct.unpack("<I", record[8:12])[0], 0xD2E6D416)
        self.assertEqual(messaging_crc(record), 0xD2E6D416)
        self.assertEqual(record[12 : 12 + 137 * 12], bytes(137 * 12))
        channel0 = record[12 + 137 * 12 : 12 + 137 * 12 + 66]
        self.assertEqual(channel0[0], 1)
        self.assertEqual(channel0[1:8], b"Public\0")
        self.assertEqual(channel0[34:66], PUBLIC_SECRET)
        self.assertEqual(record[-66 * 3 :], bytes(66 * 3))

    def test_crc_valid_hidden_bytes_are_semantically_rejected(self) -> None:
        canonical = compact_default()
        self.assertTrue(compact_semantically_valid(canonical))

        unused_contact_key = bytearray(canonical)
        unused_contact_key[16 + 3] = 0xA5
        unused_contact_key = finalize_compact(unused_contact_key)
        self.assertEqual(
            struct.unpack("<I", unused_contact_key[8:12])[0],
            messaging_crc(unused_contact_key),
        )
        self.assertFalse(compact_semantically_valid(unused_contact_key))

        unused_contact_pinned = bytearray(canonical)
        unused_contact_pinned[16 + 1] = 1
        self.assertFalse(
            compact_semantically_valid(finalize_compact(unused_contact_pinned))
        )

        unused_channel_secret = bytearray(canonical)
        channel1 = 16 + 72 * 12 + 67
        unused_channel_secret[channel1 + 34] = 0x5A
        self.assertFalse(
            compact_semantically_valid(finalize_compact(unused_channel_secret))
        )


class AlternatingPersistenceModelTests(unittest.TestCase):
    def test_generation_wrap_ties_and_half_range(self) -> None:
        self.assertEqual((UINT32_MAX + 1) & UINT32_MAX, 0)
        self.assertEqual(generation_order_model(1, UINT32_MAX), 1)
        self.assertEqual(generation_order_model(UINT32_MAX, 1), -1)
        self.assertEqual(generation_order_model(7, 7), 0)
        self.assertEqual(generation_order_model(0x80000001, 1), 0)

        old = CompactRecordModel(UINT32_MAX, b"old")
        wrapped = CompactRecordModel(1, b"wrapped")
        self.assertEqual(select_compact_model(old, wrapped), "state2b")
        duplicate = CompactRecordModel(9, b"same")
        self.assertEqual(select_compact_model(duplicate, duplicate), "state2a")
        self.assertIsNone(
            select_compact_model(
                CompactRecordModel(9, b"left"),
                CompactRecordModel(9, b"right"),
            )
        )
        self.assertIsNone(
            select_compact_model(
                CompactRecordModel(1, b"left"),
                CompactRecordModel(0x80000001, b"right"),
            )
        )

    def test_invalid_compact_falls_back_to_exact_valid_legacy(self) -> None:
        store = FaultStoreModel(
            records={
                "state1": recovered_v1_record(),
                "state2a": CompactRecordModel(1, b"partial", False),
            }
        )
        self.assertEqual(
            boot_model(store), ("legacy", "state2_invalid_legacy_usable")
        )
        store.read_errors.add("state2a")
        self.assertEqual(boot_model(store), ("failed", "read_failed"))

    def test_legacy_preclean_failure_preserves_legacy_and_ram(self) -> None:
        store = FaultStoreModel(
            records={
                "state1": recovered_v1_record(),
                "state2a": CompactRecordModel(5, b"ambiguous-a"),
                "state2b": CompactRecordModel(5, b"ambiguous-b"),
            },
            preerase_fail={"state2b"},
        )
        result = commit_model(
            store, None, 1, CompactRecordModel(1, compact_default())
        )
        self.assertFalse(result["ok"])
        self.assertTrue(result["usable"])
        self.assertEqual(store.records["state1"], recovered_v1_record())
        self.assertNotIn("state2a", store.records)

    def test_enospc_partial_target_is_removed_and_active_survives(self) -> None:
        active = CompactRecordModel(3, b"active")
        store = FaultStoreModel(
            records={"state2a": active}, put_fault="enospc"
        )
        result = commit_model(store, "state2a", 2, CompactRecordModel(4, b"new"))
        self.assertFalse(result["ok"])
        self.assertTrue(result["usable"])
        self.assertEqual(store.records, {"state2a": active})
        self.assertEqual(boot_model(store), ("compact", "state2a"))

    def test_failed_invalidation_is_explicitly_ambiguous(self) -> None:
        active = CompactRecordModel(3, b"active")
        store = FaultStoreModel(
            records={"state2a": active},
            readback_fault="error",
            invalidate_fail={"state2b"},
        )
        result = commit_model(store, "state2a", 2, CompactRecordModel(4, b"new"))
        self.assertFalse(result["ok"])
        self.assertFalse(result["usable"])
        self.assertEqual(result["reason"], "commit_ambiguous")
        self.assertEqual(boot_model(store), ("compact", "state2b"))

    def test_readback_reopen_failure_is_fail_closed(self) -> None:
        active = CompactRecordModel(3, b"active")
        store = FaultStoreModel(
            records={"state2a": active}, readback_fault="reopen"
        )
        result = commit_model(store, "state2a", 2, CompactRecordModel(4, b"new"))
        self.assertFalse(result["ok"])
        self.assertFalse(result["usable"])
        self.assertEqual(result["reason"], "commit_ambiguous")
        self.assertEqual(store.records, {"state2a": active})

    def test_corrupt_readback_is_removed_before_rollback(self) -> None:
        active = CompactRecordModel(7, b"active")
        store = FaultStoreModel(
            records={"state2a": active}, readback_fault="corrupt"
        )
        result = commit_model(store, "state2a", 2, CompactRecordModel(8, b"new"))
        self.assertFalse(result["ok"])
        self.assertTrue(result["usable"])
        self.assertEqual(store.records, {"state2a": active})

    def test_cleanup_failure_leaves_newest_record_authoritative(self) -> None:
        old = CompactRecordModel(10, b"old")
        new = CompactRecordModel(11, b"new")
        store = FaultStoreModel(
            records={"state2a": old}, cleanup_fail={"state2a"}
        )
        result = commit_model(store, "state2a", 2, new)
        self.assertTrue(result["ok"])
        self.assertTrue(result["cleanup_pending"])
        self.assertEqual(result["reason"], "cleanup_pending")
        self.assertEqual(boot_model(store), ("compact", "state2b"))

    def test_power_cut_points_always_leave_one_bootable_authority(self) -> None:
        legacy_after_preclean = FaultStoreModel(
            records={"state1": recovered_v1_record()}
        )
        self.assertEqual(
            boot_model(legacy_after_preclean),
            ("legacy", "legacy_migration_pending"),
        )

        active = CompactRecordModel(20, b"active")
        partial_target = FaultStoreModel(
            records={
                "state2a": active,
                "state2b": CompactRecordModel(21, b"partial", False),
            }
        )
        self.assertEqual(boot_model(partial_target), ("compact", "state2a"))

        valid_before_cleanup = FaultStoreModel(
            records={
                "state2a": active,
                "state2b": CompactRecordModel(21, b"new"),
            }
        )
        self.assertEqual(
            boot_model(valid_before_cleanup), ("compact", "state2b")
        )

    def test_measured_peak_entry_budget(self) -> None:
        other, legacy, compact, usable = 392, 62, 39, 504
        self.assertEqual(other + legacy + compact, 493)
        self.assertLessEqual(other + legacy + compact, usable)
        self.assertEqual(other + compact * 2, 470)
        self.assertLessEqual(other + compact * 2, usable)
        self.assertGreater(other + legacy + compact * 2, usable)

    def test_reset_from_orphan_model_is_semantically_lossless_for_fixture(self) -> None:
        store = FaultStoreModel(records={"state1": b"orphan-index-only"})
        self.assertEqual(boot_model(store), ("failed", "orphaned_legacy_record"))
        store.records.clear()
        expected = CompactRecordModel(1, compact_default())
        result = commit_model(store, None, 0, expected)
        self.assertTrue(result["ok"])
        self.assertEqual(boot_model(store), ("compact", "state2a"))


class PersistenceMigrationSourceTests(unittest.TestCase):
    def test_packed_v1_and_compact_v2_sizes_and_peak_budgets_are_exact(self) -> None:
        contact_v1_bytes = struct.calcsize("<BBBB32s33sI64s")
        contact_v2_bytes = struct.calcsize("<BBB32s33sI")
        channel_v1_bytes = struct.calcsize("<B33s32s")
        channel_v2_bytes = struct.calcsize("<B33s32sB")
        self.assertEqual(contact_v1_bytes, 137)
        self.assertEqual(contact_v2_bytes, 72)
        self.assertEqual(12 + contact_v1_bytes * 12 + channel_v1_bytes * 4, 1920)
        self.assertEqual(16 + contact_v2_bytes * 12 + channel_v2_bytes * 4, 1148)
        for required in (
            "kMessagingSchemaV1 = 1U",
            "kMessagingSchema = 2U",
            "sizeof(PersistedMessagingStateV1) == 1920U",
            "sizeof(PersistedMessagingState) == 1148U",
            "PersistedMessagingStateV1 v1;",
            "PersistedMessagingState v2;",
            "kMessagingCompactSinglePageEntries == 38U",
            "kMessagingCompactMeasuredSplitEntries = 39U",
            "kMessagingMeasuredOtherLiveEntries = 392U",
            "kMessagingLegacyLiveEntries = 62U",
            "kMessagingMeasuredUsableEntries = 504U",
        ):
            self.assertIn(required, TRANSPORT)
        self.assertIn(
            "kMessagingLegacyLiveEntries +\n"
            "                      kMessagingCompactMeasuredSplitEntries <=\n"
            "                  kMessagingMeasuredUsableEntries",
            TRANSPORT,
        )
        self.assertIn(
            "2U * kMessagingCompactMeasuredSplitEntries <=\n"
            "                  kMessagingMeasuredUsableEntries",
            TRANSPORT,
        )

    def test_boot_is_read_only_and_open_errors_fail_closed(self) -> None:
        begin = cpp_function(TRANSPORT, "bool begin()")
        for required in (
            "nvs_open(",
            "NVS_READONLY",
            "namespaceResult == ESP_ERR_NVS_NOT_FOUND",
            "namespaceResult != ESP_OK",
            "MessagingStorageReason::NamespaceOpenFailed",
            "nvs_get_blob(",
            "MessagingStorageReason::ReadFailed",
        ):
            self.assertIn(required, begin)
        for forbidden in (
            "nvs_set_blob(",
            "nvs_erase_key(",
            "nvs_erase_all(",
            "nvs_commit(",
            "persistCompactRecord(",
            "replaceInvalidStorage(",
            "save()",
        ):
            self.assertNotIn(forbidden, begin)

    def test_v1_stays_usable_and_migration_is_lazy(self) -> None:
        begin = cpp_function(TRANSPORT, "bool begin()")
        for required in (
            "storedBytes == sizeof(PersistedMessagingStateV1)",
            "record.schema == kMessagingSchemaV1",
            "record.recordBytes == sizeof(record)",
            "record.crc32 == messagingCrc(",
            "decode(record)",
            "storageUsable_ = true",
            "persistedSchema_ = kMessagingSchemaV1",
            "migrationPending_ = true",
            "MessagingStorageReason::LegacyMigrationPending",
            "MessagingStorageReason::State2InvalidLegacyUsable",
        ):
            self.assertIn(required, begin)
        self.assertIn(
            "Promotion is attempted once, and only by an explicit owner mutation",
            begin,
        )

        v1_decode_start = TRANSPORT.index(
            "bool decode(const PersistedMessagingStateV1& record)"
        )
        v1_decode_end = TRANSPORT.index(
            "bool decode(const PersistedMessagingState& record)", v1_decode_start
        )
        v1_decode = TRANSPORT[v1_decode_start:v1_decode_end]
        for preserved in (
            "destination.type = source.type",
            "destination.lastAdvertTimestamp = source.lastAdvertTimestamp",
            "memcpy(destination.publicKey, source.publicKey",
            "memcpy(destination.name, source.name",
            "memcpy(destination.channel.secret, source.secret",
            "deriveChannelHash(destination.channel)",
            "destination.regionScope = ChannelRegionScope::Legacy",
        ):
            self.assertIn(preserved, v1_decode)
        self.assertIn("destination.outPathLen = kUnknownPath", v1_decode)
        self.assertNotIn("memcpy(destination.outPath", v1_decode)

    def test_orphaned_legacy_and_invalid_compact_are_distinct(self) -> None:
        begin = cpp_function(TRANSPORT, "bool begin()")
        for required in (
            "bytesRead != sizeof(record)",
            "MessagingStorageReason::OrphanedLegacyRecord",
            "candidateA == CompactCandidateState::Invalid",
            "candidateB == CompactCandidateState::Invalid",
            "!compactSelectionValid",
            "MessagingStorageReason::State2Invalid",
            "CompactCandidateState::ReadError",
            "MessagingStorageReason::ReadFailed",
        ):
            self.assertIn(required, begin)

    def test_compact_selection_is_wrap_aware_and_ambiguous_pairs_fail_closed(self) -> None:
        order = cpp_function(TRANSPORT, "static int generationOrder(")
        self.assertIn("const uint32_t delta = a - b", order)
        self.assertIn("delta == 0U || delta == 0x80000000UL", order)
        self.assertIn("delta < 0x80000000UL ? 1 : -1", order)
        self.assertIn("current == UINT32_MAX ? 1U : current + 1U", TRANSPORT)

        select = cpp_function(TRANSPORT, "bool selectCompactCandidate(")
        self.assertIn("generationOrder(recordA.generation", select)
        self.assertIn("recordA.generation == recordB.generation", select)
        self.assertIn("memcmp(&recordA, &recordB, sizeof(recordA)) == 0", select)
        self.assertIn("return false", select)

    def test_compact_semantics_require_canonical_empty_records(self) -> None:
        valid_start = TRANSPORT.index(
            "bool valid(const PersistedMessagingState& record) const"
        )
        valid_end = TRANSPORT.index(
            "CompactCandidateState readCompactCandidate(", valid_start
        )
        valid = TRANSPORT[valid_start:valid_end]
        for required in (
            "record.generation == 0U",
            "source.pinned != 0U",
            "source.type != 0U",
            "source.lastAdvertTimestamp != 0U",
            "bytesAreZero(source.publicKey",
            "bytesAreZero(source.name",
            "source.pinned != 1U",
            "validCanonicalStoredName(source.name)",
            "bytesAreZero(source.secret",
            "!source.used && scope != ChannelRegionScope::Legacy",
            "index == 0U && scope != ChannelRegionScope::Legacy",
        ):
            self.assertIn(required, valid)

        encode = cpp_function(TRANSPORT, "bool encodeCurrentRecord(")
        self.assertIn("initializeRecord(record, generation)", encode)
        self.assertIn("const size_t nameBytes = strnlen(source.name, 32U)", encode)
        self.assertNotIn(
            "memcpy(destination.name, source.name, sizeof(destination.name))",
            encode,
        )

    def test_ab_commit_never_writes_active_and_verifies_before_authority(self) -> None:
        persist = cpp_function(TRANSPORT, "bool persistCompactRecord(")
        for required in (
            "previousActive == CompactSlot::A",
            "previousActive == CompactSlot::B",
            "target == previousActive",
            "removeAndConfirm(kMessagingLegacyKey)",
            "previousSchema == kMessagingSchemaV1",
            "removeAndConfirm(kMessagingCompactKeyA)",
            "removeAndConfirm(kMessagingCompactKeyB)",
            "removeAndConfirm(targetKey)",
            "nvs_set_blob(",
            "nvs_commit(writeHandle)",
            "nvs_get_blob(",
            "readbackBytes == sizeof(readback)",
            "valid(readback)",
            "memcmp(&expected, &readback, sizeof(expected)) == 0",
            "activeCompactSlot_ = target",
            "generation_ = expected.generation",
        ):
            self.assertIn(required, persist)
        self.assertLess(
            persist.index("removeAndConfirm(targetKey)"),
            persist.index("nvs_set_blob("),
        )
        self.assertLess(
            persist.index("const bool verified"),
            persist.index("activeCompactSlot_ = target"),
        )
        self.assertLess(
            persist.index("activeCompactSlot_ = target"),
            persist.index("compactKey(previousActive)"),
        )

    def test_failed_write_or_readback_is_invalidated_or_marked_ambiguous(self) -> None:
        persist = cpp_function(TRANSPORT, "bool persistCompactRecord(")
        self.assertGreaterEqual(persist.count("removeAndConfirm(targetKey)"), 3)
        self.assertGreaterEqual(persist.count("MessagingStorageWriteResult::Ambiguous"), 2)
        self.assertGreaterEqual(persist.count("MessagingStorageReason::CommitAmbiguous"), 2)
        self.assertIn("storageUsable_ = false", persist)
        self.assertIn("writeResult != ESP_OK", persist)
        self.assertIn("if (!verified)", persist)
        self.assertIn("NVS_READONLY", persist)
        self.assertIn("readbackOpenResult != ESP_OK", persist)
        self.assertIn("storageUsable_ = false", persist)
        self.assertLess(
            persist.index("nvs_close(writeHandle)"),
            persist.index("NVS_READONLY"),
        )

    def test_nvs_absence_is_not_inferred_from_zero_length(self) -> None:
        present = cpp_function(TRANSPORT, "bool keyPresent(")
        self.assertIn("nvs_get_blob(", present)
        self.assertIn("result == ESP_ERR_NVS_NOT_FOUND", present)
        self.assertIn("result != ESP_OK", present)
        self.assertNotIn("storedBytes == 0U", present)

        fresh = cpp_function(TRANSPORT, "bool keyPresentFresh(")
        self.assertIn("NVS_READONLY", fresh)
        self.assertIn("keyPresent(readHandle, key, present)", fresh)

        remove = cpp_function(TRANSPORT, "bool removeAndConfirm(")
        self.assertGreaterEqual(remove.count("keyPresentFresh("), 2)
        self.assertIn("nvs_erase_key(writeHandle, key)", remove)
        self.assertIn("nvs_commit(writeHandle)", remove)
        self.assertLess(remove.index("nvs_close(writeHandle)"), remove.rindex("keyPresentFresh("))

        messaging_start = TRANSPORT.index("class MessagingState")
        messaging_end = TRANSPORT.index("class AdvertSink", messaging_start)
        messaging = TRANSPORT[messaging_start:messaging_end]
        self.assertNotIn("getBytesLength", messaging)
        self.assertNotIn("putBytes", messaging)
        self.assertNotIn("Preferences ", messaging)

    def test_reset_from_orphan_uses_non_aliasing_readback_and_installs_ram_last(self) -> None:
        reset = cpp_function(TRANSPORT, "bool reset()")
        self.assertIn("replaceInvalidStorage(record)", reset)
        self.assertLess(reset.index("if (!saved) return false"), reset.index("clearRam()"))
        self.assertLess(reset.index("clearRam()"), reset.index("installPublicChannel()"))

        replace = cpp_function(TRANSPORT, "bool replaceInvalidStorage(")
        self.assertIn("nvs_erase_all(namespaceHandle)", replace)
        self.assertIn("nvs_commit(namespaceHandle)", replace)
        self.assertLess(replace.index("nvs_erase_all(namespaceHandle)"), replace.index("persistCompactRecord(expected)"))

        persist = cpp_function(TRANSPORT, "bool persistCompactRecord(")
        self.assertIn("PersistedMessagingState* readbackTarget", persist)
        self.assertIn("if (readbackTarget == &expected)", persist)
        self.assertIn("readbackTarget = &compactCandidateB_", persist)
        self.assertNotIn("PersistedMessagingState& readback = compactCandidateA_", persist)

    def test_v2_rejects_unknown_scope_and_public_scope(self) -> None:
        start = TRANSPORT.index("bool valid(const PersistedMessagingState& record)")
        end = TRANSPORT.index("CompactCandidateState readCompactCandidate(", start)
        validate = TRANSPORT[start:end]
        self.assertIn("!validChannelRegionScope(scope)", validate)
        self.assertIn("index == 0U && scope != ChannelRegionScope::Legacy", validate)
        self.assertIn("!source.used && scope != ChannelRegionScope::Legacy", validate)

        public = cpp_function(TRANSPORT, "void installPublicChannel()")
        self.assertIn("channel.regionScope = ChannelRegionScope::Legacy", public)
        reset = cpp_function(TRANSPORT, "bool reset()")
        self.assertIn("installPublicChannel()", reset)


class RouteContractSourceTests(unittest.TestCase):
    def test_only_explicit_persisted_eu_channel_selects_transport_flood(self) -> None:
        self.assertIn("enum class ChannelRegionScope", TRANSPORT_HEADER)
        self.assertIn("Legacy = 0", TRANSPORT_HEADER)
        self.assertIn("Eu = 1", TRANSPORT_HEADER)
        prepare = cpp_function(TRANSPORT, "bool prepareFloodRoute(")
        self.assertIn("regionScope == ChannelRegionScope::Legacy", prepare)
        self.assertIn("ROUTE_TYPE_FLOOD", prepare)
        self.assertIn("ROUTE_TYPE_TRANSPORT_FLOOD", prepare)
        self.assertIn("calculateDefaultTransportCode(", prepare)

        channel = cpp_function(TRANSPORT, "TransportStatus sendChannelText(")
        self.assertIn("prepareFloodRoute(packet, channel.regionScope)", channel)
        self.assertIn("sendFloodRoute(packet, channel.regionScope)", channel)

        for signature in (
            "TransportStatus sendDirectText(ContactEntry& recipient",
            "void onPeerDataRecv(",
            "void sendAckTo(",
            "TransportStatus KitsuMeshTransport::exportSignedAdvert(",
            "TransportStatus KitsuMeshTransport::introduce(",
            "TransportStatus KitsuMeshTransport::introduceOnce(",
        ):
            body = cpp_function(TRANSPORT, signature)
            self.assertIn("ChannelRegionScope::Legacy", body)
            self.assertNotIn("ChannelRegionScope::Eu", body)

        helper = cpp_function(TRANSPORT, "bool sendFloodRoute(")
        self.assertEqual(helper.count("sendFlood("), 2)
        self.assertNotIn("retry", helper.lower())
        self.assertNotIn("fallback", helper.lower())

    def test_inbound_channel_decrypt_is_not_filtered_by_outbound_scope(self) -> None:
        receive = cpp_function(TRANSPORT, "void onGroupDataRecv(")
        self.assertIn("packet->isRouteFlood()", receive)
        self.assertNotIn("regionScope", receive)
        self.assertNotIn("transport_codes", receive)

    def test_actual_physical_route_is_bound_before_repeat_correlation(self) -> None:
        log_tx = cpp_function(TRANSPORT, "void logTx(::mesh::Packet* packet")
        self.assertEqual(log_tx.count("captureFloodRoute(packet, sentRoute)"), 2)
        log_rx = cpp_function(TRANSPORT, "void logRxRaw(")
        self.assertIn("floodRouteBindingFromWire(wire, receivedRoute)", log_rx)
        self.assertIn("receivedRoute, wire.pathCount", log_rx)
        for tracker in (CHANNEL_TRACKER, ADVERT_TRACKER):
            self.assertIn("sameFloodRouteBinding", tracker)
            self.assertIn("WireMismatch", tracker)
        self.assertIn("transportCodes[0]", REPEAT_WIRE)
        self.assertIn("transportCodes[1]", REPEAT_WIRE)
        self.assertIn("pathHashSize", REPEAT_WIRE)

    def test_wire_mismatch_has_distinct_saturating_hardware_diagnostics(self) -> None:
        log_rx = cpp_function(TRANSPORT, "void logRxRaw(")
        for required in (
            "ChannelRepeatObserveResult::WireMismatch",
            "repeatDiagnostics_.channelWireMismatches",
            "AdvertRepeatObserveResult::WireMismatch",
            "repeatDiagnostics_.advertWireMismatches",
            "RepeatDiagnosticResult::WireMismatch",
        ):
            self.assertIn(required, log_rx)
        self.assertIn('return "wire_mismatch"', MAIN)
        self.assertEqual(MAIN.count("channel_wire_mismatches"), 2)
        self.assertEqual(MAIN.count("advert_wire_mismatches"), 2)


class ProvisioningAndApiSourceTests(unittest.TestCase):
    def test_serial_marker_is_before_secret_and_absent_means_legacy(self) -> None:
        self.assertIn("ChannelRegionScope::Legacy", CHAT_HEADER)
        parser = cpp_function(CHAT, "ParseStatus parseCommand(")
        marker = parser.index('tokenStartsWithExact(secret, "region_scope=")')
        decode = parser.index("decodeHex(secret, output.channelSecret")
        self.assertLess(marker, decode)
        self.assertIn('tokenEqualsExact(secret, "region_scope=EU")', parser)
        self.assertIn("ParseStatus::InvalidRegionScope", parser)
        self.assertIn("output.channelRegionScope = ChannelRegionScope::Eu", parser)

        serial = cpp_function(MAIN, "void printChatChannels()")
        self.assertIn('Serial.print(",\\\"region_scope\\\":")', serial)
        self.assertIn('Serial.print("\\\"EU\\\"")', serial)
        self.assertIn('Serial.print("null")', serial)

    def test_authenticated_v1_shape_is_frozen_and_v2_is_negotiated(self) -> None:
        v1 = cpp_function(MAIN, "bool buildChannels(")
        self.assertIn("kitsu.channels.v1", v1)
        self.assertNotIn("region_scope", v1)
        v2 = cpp_function(MAIN, "bool buildChannelsV2(")
        self.assertIn("kitsu.channels.v2", v2)
        self.assertIn("region_scope", v2)
        self.assertIn("ChannelRegionScope::Eu", v2)
        self.assertIn('output += "null"', v2)

        allowed = cpp_function(SESSION, "bool operationAllowed(")
        self.assertIn('"channels.get"', allowed)
        self.assertIn('"channels.get.v2"', allowed)
        handler_source = MAIN[
            MAIN.rindex(
                "__attribute__((noinline)) bool handleCompanionBleRequest("
            ) :
        ]
        handler = cpp_function(
            handler_source,
            "__attribute__((noinline)) bool handleCompanionBleRequest(",
        )
        self.assertIn('strcmp(request.operation, "channels.get")', handler)
        self.assertIn('strcmp(request.operation, "channels.get.v2")', handler)
        self.assertIn("companion_api::buildChannelsV2", handler)

    def test_storage_diagnostics_are_authenticated_and_exact(self) -> None:
        storage = cpp_function(MAIN, "bool buildChatStorage(")
        for required in (
            "kitsu.chat-storage.v1",
            "usable",
            "persisted_schema",
            "migration_pending",
            "cleanup_pending",
            "generation",
            "writable_last_result",
            "reason",
        ):
            self.assertIn(required, storage)
        allowed = cpp_function(SESSION, "bool operationAllowed(")
        self.assertIn('"chat.storage.get"', allowed)
        handler_source = MAIN[
            MAIN.rindex(
                "__attribute__((noinline)) bool handleCompanionBleRequest("
            ) :
        ]
        handler = cpp_function(
            handler_source,
            "__attribute__((noinline)) bool handleCompanionBleRequest(",
        )
        self.assertIn('strcmp(request.operation, "chat.storage.get")', handler)
        self.assertIn("companion_api::buildChatStorage", handler)

        serial = cpp_function(MAIN, "void printChatStorageStatus()")
        self.assertIn("KITSU_CHAT_STORAGE", serial)
        for key in (
            "usable",
            "persisted_schema",
            "migration_pending",
            "cleanup_pending",
            "generation",
            "writable_last_result",
            "reason",
        ):
            self.assertIn(key, serial)

        legacy = cpp_function(MAIN, "void printChatStatus()")
        self.assertNotIn("storage", legacy)


if __name__ == "__main__":
    unittest.main(verbosity=2)
