#!/usr/bin/env python3
"""Offline fault/order tests for the Kitsu 0.20.3 migration core."""

from __future__ import annotations

import hashlib
import inspect
import json
import os
import struct
import sys
import tempfile
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import migrate_kitsu_0203_storage as migration  # noqa: E402


def application(version: str = migration.TARGET_FIRMWARE_VERSION) -> bytes:
    data = bytearray((index & 0xFF) for index in range(4096))
    prefix = (
        "KITSU-ID1|schema=1|length=0000|version=" + version +
        "|device_class=heltec-v3.2|layout=kitsu-8m-dual-ota-3m-v1|"
        "flash=00800000|nvs=00009000/00040000|"
        "otadata=00049000/00002000|"
        "app0=00050000|app1=00350000|slot=00300000|"
        "journal=00001000|max=002ff000|"
        "spiffs=00670000/00140000|conn=007b0000/00040000|"
        "coredump=007f0000/00010000"
    )
    total = len((prefix + "|crc32=00000000|end\0").encode("ascii"))
    prefix = prefix.replace("length=0000", f"length={total:04d}")
    marker = (
        prefix + f"|crc32={zlib.crc32(prefix.encode('ascii')) & 0xffffffff:08x}|end\0"
    ).encode("ascii")
    data[128:128 + len(marker)] = marker
    data = bytes(data)
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = 1
    header[2] = 2
    header[3] = 0x3F
    header[4:8] = struct.pack("<I", 0x40370000)
    header[12:14] = struct.pack("<H", migration.ESP32S3_CHIP_ID)
    header[23] = 1
    segment = struct.pack("<II", 0x3FC90000, len(data)) + data
    body = bytes(header) + segment
    checksum = migration.ESP_IMAGE_CHECKSUM_SEED
    for byte in data:
        checksum ^= byte
    checksum_offset = len(body) + (15 - (len(body) % 16))
    checked = body + b"\x00" * (checksum_offset - len(body)) + bytes([checksum])
    return checked + hashlib.sha256(checked).digest()


def legacy_nvs() -> bytes:
    output = bytearray(b"\xff" * migration.LEGACY_NVS_BYTES)
    for page in range(4):
        offset = page * migration.SECTOR_BYTES
        output[offset:offset + 4] = b"\xfe\xff\xff\xff"
        output[offset + 4:offset + 8] = struct.pack("<I", page + 100)
        output[offset + 8] = 0xFE
        output[offset + 28:offset + 32] = struct.pack(
            "<I", __import__("zlib").crc32(
                output[offset + 4:offset + 28], 0xFFFFFFFF
            ) & 0xFFFFFFFF
        )
    namespace = bytearray(b"\xff" * 32)
    namespace[0] = 0
    namespace[1] = 0x01
    namespace[2] = 1
    namespace[3] = 0xFF
    namespace[8:8 + len(b"kitsu\x00")] = b"kitsu\x00"
    namespace[24] = 1
    crc = __import__("zlib").crc32(namespace[:4], 0xFFFFFFFF) & 0xFFFFFFFF
    crc = __import__("zlib").crc32(namespace[8:], crc) & 0xFFFFFFFF
    namespace[4:8] = struct.pack("<I", crc)
    output[64:96] = namespace
    # Entry 0 is WRITTEN (0b10). All other two-bit states remain EMPTY
    # (0b11), matching the pinned IDF CompressedEnumTable layout.
    output[32] &= 0xFE
    return bytes(output)


def boot_app0() -> bytes:
    output = bytearray(b"\xff" * migration.OTA_DATA_BYTES)
    output[:4] = struct.pack("<I", 1)
    output[28:32] = struct.pack(
        "<I", __import__("zlib").crc32(output[:4], 0xFFFFFFFF) & 0xFFFFFFFF
    )
    return bytes(output)


def nvs_oracle_binding(prefix: bytes) -> dict:
    result = (
        "KITSU_NVS_IDF447_ORACLE_OK total=8064 used=494 free=7570 "
        "namespaces=14 iterable=70 reads=1307 writes=0 erases=0"
    )
    runner_sha = migration.sha256_file(migration.nvs_oracle_runner_path())
    harness_sha = migration.sha256_file(migration.nvs_oracle_source_path())
    expanded_sha = migration.sha256_bytes(
        prefix + b"\xff" * migration.NVS_EXTENSION_BYTES
    )
    runner = (
        "KITSU_NVS_IDF447_RUNNER_OK "
        f"runner_sha256={runner_sha} harness_sha256={harness_sha} "
        f"archive_sha256={migration.NVS_ORACLE_IDF_ARCHIVE_SHA256} "
        f"expanded_sha256={expanded_sha} binary_sha256={'a' * 64} "
        f"build_log_sha256={'b' * 64} "
        f"gcc=15.2.0 gcc_sha256={migration.NVS_ORACLE_GCC_SHA256} "
        f"gxx=15.2.0 gxx_sha256={migration.NVS_ORACLE_GXX_SHA256} "
        "builds=2 deterministic=true"
    )
    output = runner + "\n" + result
    record = migration.build_nvs_oracle_record(
        prefix, output, migration.NVS_ORACLE_IDF_ARCHIVE_SHA256,
        harness_sha, runner_sha,
    )
    return {
        "evidence_sha256": migration.sha256_bytes(
            migration.canonical_json_bytes(record)
        ),
        **record,
    }


def test_backup_location_acl_and_durability_contract() -> None:
    if os.name == "nt":
        project = Path("C:/source/kitsu")
        profile = Path("C:/Users/TestOwner")
        good = migration.validate_backup_leaf_location(
            Path("C:/KitsuRecovery/primary/flash.bin"), "test backup",
            project_root=project, profile_root=profile,
        )
        assert good == Path("C:/KitsuRecovery/primary/flash.bin")
        for unsafe in (
            Path("D:/flash.bin"),
            Path("C:/Users/TestOwner/flash.bin"),
            Path("C:/Users/TestOwner/private/flash.bin"),
            Path("C:/Users/flash.bin"),
            Path("C:/source/kitsu/private/flash.bin"),
            Path("C:/source/flash.bin"),
        ):
            raises(
                lambda candidate=unsafe: migration.validate_backup_leaf_location(
                    candidate, "test backup", project_root=project,
                    profile_root=profile,
                ),
                "test backup",
            )

        sid = "S-1-5-21-100-200-300-400"
        valid_directory_acl = {
            "protected": True,
            "rules": [{
                "sid": sid,
                "type": "Allow",
                "rights": migration.WINDOWS_FULL_CONTROL,
                "inherited": False,
                "inheritance": "ContainerInherit, ObjectInherit",
                "propagation": "None",
            }],
        }
        summary = migration.validate_windows_acl_snapshot(
            valid_directory_acl, sid, True
        )
        assert summary["protected"] and summary["rule_count"] == 1
        unsafe_acl = json.loads(json.dumps(valid_directory_acl))
        unsafe_acl["rules"].append(dict(unsafe_acl["rules"][0]))
        unsafe_acl["rules"][1]["sid"] = "S-1-5-18"
        raises(
            lambda: migration.validate_windows_acl_snapshot(
                unsafe_acl, sid, True
            ),
            "unexpected principals",
        )
        inherited_acl = json.loads(json.dumps(valid_directory_acl))
        inherited_acl["protected"] = False
        raises(
            lambda: migration.validate_windows_acl_snapshot(
                inherited_acl, sid, True
            ),
            "inherits",
        )

    with tempfile.TemporaryDirectory(prefix="kitsu-private-output-") as raw:
        root = Path(raw)
        base = root / "base"
        base.mkdir()
        output = base / "new-leaf" / "flash.bin"
        migration.create_hardened_backup_leaf(output, "test backup")
        assert output.parent.is_dir() and not any(output.parent.iterdir())
        raises(
            lambda: migration.create_hardened_backup_leaf(
                output, "test backup"
            ),
            "already exists",
        )
        nonempty = base / "nonempty"
        nonempty.mkdir()
        (nonempty / "unrelated.txt").write_text("do not touch", encoding="utf-8")
        raises(
            lambda: migration.create_hardened_backup_leaf(
                nonempty / "flash.bin", "test backup"
            ),
            "already exists",
        )

        acl_file = output.parent / "acl-probe.bin"
        acl_file.write_bytes(b"private")
        directory_acl = migration.inspect_private_acl(output.parent, True)
        file_acl = migration.harden_private_file(acl_file)
        assert directory_acl["protected"] and file_acl["protected"]
        assert directory_acl["rule_count"] == file_acl["rule_count"] == 1

        evidence = root / "durable-evidence.json"
        move_calls: list[bool] = []
        original_move = migration.windows_durable_move
        if os.name == "nt":
            def observed_move(source: Path, destination: Path, replace: bool) -> None:
                move_calls.append(replace)
                original_move(source, destination, replace)
            migration.windows_durable_move = observed_move
        try:
            migration.exclusive_json(evidence, {"status": "prepared"})
            migration.replace_private_json(
                evidence, {"status": "flash_verified"},
                expected_current=[{"status": "prepared"}],
            )
        finally:
            migration.windows_durable_move = original_move
        assert evidence.read_text(encoding="utf-8") == (
            '{"status":"flash_verified"}\n'
        )
        assert not evidence.with_name(evidence.name + ".tmp").exists()
        if os.name == "nt":
            assert move_calls == [False, True]


def backup() -> bytes:
    output = bytearray(b"\xa5" * migration.FLASH_BYTES)
    old_table = migration.partition_table(migration.OLD_LAYOUT)
    output[migration.PARTITION_TABLE_OFFSET:
           migration.PARTITION_TABLE_OFFSET + len(old_table)] = old_table
    output[migration.NVS_OFFSET:migration.NVS_EXTENSION_OFFSET] = legacy_nvs()
    output[0x00E000:0x010000] = boot_app0()
    old_app = application(migration.SOURCE_FIRMWARE_VERSION)
    output[0x010000:0x010000 + len(old_app)] = old_app
    return bytes(output)


def raises(callback, expected: str) -> None:
    try:
        callback()
    except (ValueError, RuntimeError) as error:
        assert expected in str(error), (expected, str(error))
    else:
        raise AssertionError("unsafe migration fixture unexpectedly passed")


class MemorySession:
    def __init__(
        self, flash: bytes, fail_mutation: int | None = None,
        partial_failure: bool = False, fail_finish: bool = False,
        fail_finish_after_reset: bool = False,
        finish_mutations: tuple[tuple[int, bytes], ...] = (),
    ):
        self.flash = bytearray(flash)
        self.fail_mutation = fail_mutation
        self.partial_failure = partial_failure
        self.fail_finish = fail_finish
        self.fail_finish_after_reset = fail_finish_after_reset
        self.finish_mutations = finish_mutations
        self.mutations = 0
        self.log: list[tuple[str, int, int]] = []
        self.finished = False

    def _will_fail(self) -> bool:
        return self.fail_mutation == self.mutations + 1

    def _mutated(self) -> None:
        self.mutations += 1
        if self.fail_mutation == self.mutations:
            raise RuntimeError("injected migration interruption")

    def erase(self, offset: int, size: int) -> None:
        if self.partial_failure and self._will_fail():
            partial = max(migration.SECTOR_BYTES, size // 2)
            partial = min(size, partial)
            self.flash[offset:offset + partial] = b"\xff" * partial
            self.log.append(("erase-partial", offset, partial))
            self._mutated()
        self.flash[offset:offset + size] = b"\xff" * size
        self.log.append(("erase", offset, size))
        self._mutated()

    def write(self, offset: int, path: Path) -> None:
        value = path.read_bytes()
        start = offset & ~(migration.SECTOR_BYTES - 1)
        end = (offset + len(value) + migration.SECTOR_BYTES - 1) & ~(
            migration.SECTOR_BYTES - 1
        )
        if self.partial_failure and self._will_fail():
            self.flash[start:end] = b"\xff" * (end - start)
            partial = max(1, len(value) // 2)
            self.flash[offset:offset + partial] = value[:partial]
            self.log.append(("write-partial", offset, partial))
            self._mutated()
        self.flash[start:end] = b"\xff" * (end - start)
        self.flash[offset:offset + len(value)] = value
        self.log.append(("write", offset, len(value)))
        self._mutated()

    def read(self, offset: int, size: int, output: Path) -> bytes:
        del output
        self.log.append(("read", offset, size))
        return bytes(self.flash[offset:offset + size])

    def finish(self) -> None:
        if self.fail_finish:
            raise RuntimeError("injected reset failure")
        self.finished = True
        self.log.append(("reset", 0, 0))
        for offset, value in self.finish_mutations:
            self.flash[offset:offset + len(value)] = value
        if self.fail_finish_after_reset:
            raise RuntimeError("injected reset acknowledgement loss")


def main() -> None:
    test_backup_location_acl_and_durability_contract()
    capture_source = inspect.getsource(migration.capture_command)
    assert "session.finish(" not in capture_source
    assert "held_in_loader=true" in capture_source
    assert capture_source.index("validate_reviewed_source_application") < (
        capture_source.index("private_exclusive_bytes")
    )
    original = backup()
    app = application()
    table = migration.partition_table(migration.NEW_LAYOUT)
    ota = boot_app0()
    oracle = nvs_oracle_binding(legacy_nvs())
    assert oracle["result"]["write_operations"] == 0
    assert oracle["result"]["erase_operations"] == 0
    assert oracle["expanded_nvs_bytes"] == 0x40000
    raises(
        lambda: migration.parse_nvs_oracle_result_line(
            "KITSU_NVS_IDF447_ORACLE_OK total=8064 used=494 free=7570 "
            "namespaces=14 iterable=70 reads=1307 writes=1 erases=0"
        ),
        "not read-only",
    )
    # The production framework helper is pinned by hash.  The synthetic helper
    # exercises ordering/layout without weakening that production gate.
    original_hash = migration.BOOT_APP0_SHA256
    migration.BOOT_APP0_SHA256 = migration.sha256_bytes(ota)
    invalid_ota = bytearray(ota)
    invalid_ota[24:28] = struct.pack("<I", 3)
    assert migration.ota_select_slot(bytes(invalid_ota)) == 0
    malformed_ota = bytearray(ota)
    malformed_ota[28] ^= 1
    raises(lambda: migration.ota_select_slot(bytes(malformed_ota)), "no valid")
    final, evidence = migration.simulate_migration(original, table, app, ota)
    migration.verify_final_image(original, final, table, app, ota)

    assert evidence["partition_table_commit_is_last_write"] is True
    assert evidence["operations"][
        next(index for index, operation in enumerate(evidence["operations"])
             if operation["name"] == "verify_boot_prefix_precommit")
    ]["offset"] == 0
    writes = [operation for operation in evidence["operations"]
              if operation["kind"] == "write"]
    assert writes[-1]["name"] == "partition_table_commit_last"
    assert next(operation for operation in writes
                if operation["name"] == "stage_app0_write")["size"] == len(app)
    assert next(operation for operation in writes
                if operation["name"] == "stage_app1_write")["size"] == len(app)
    assert evidence["operations"][-1]["name"] == "reset_after_verified_commit"
    assert evidence["operations"][-2]["name"] == "verify_complete_flash"
    assert evidence["serial_tooling"] == {
        "esptool_version": "4.11.0",
        "esptool_python_tree_sha256": (
            "aa4aa5a3bfeef8d050efd262cf5a9b78d68abd7c2437b307e4379d47919eb83a"
        ),
        "write_flash_settings": "mode=keep,freq=keep,size=keep",
        "final_operation": "read_mac_then_global_hard_reset_once",
        "run_command_forbidden": True,
    }
    commit_index = evidence["partition_table_commit_index"]
    assert evidence["operations"][commit_index]["name"] == (
        "partition_table_commit_last"
    )
    assert final[migration.NVS_OFFSET:migration.NVS_EXTENSION_OFFSET] == legacy_nvs()
    assert final[migration.PRESERVED_UPPER_OFFSET:] == original[migration.PRESERVED_UPPER_OFFSET:]
    assert final[migration.APP0_OFFSET + len(app):
                 migration.APP0_OFFSET + migration.APP_SLOT_BYTES] == (
        b"\xff" * (migration.APP_SLOT_BYTES - len(app))
    )
    assert final[migration.APP1_OFFSET + migration.MAX_IMAGE_BYTES:
                 migration.APP1_OFFSET + migration.APP_SLOT_BYTES] == (
        b"\xff" * migration.OTA_JOURNAL_BYTES
    )

    wrong_table = bytearray(table)
    wrong_table[0x40] ^= 1
    raises(
        lambda: migration.simulate_migration(original, bytes(wrong_table), app, ota),
        "MD5",
    )
    bad_nvs = bytearray(original)
    bad_nvs[migration.NVS_OFFSET + 8] = 0xFD
    raises(
        lambda: migration.simulate_migration(bytes(bad_nvs), table, app, ota),
        "version/reserved",
    )
    ghost_nvs = bytearray(legacy_nvs())
    ghost_nvs[32] |= 0x01  # entry0 EMPTY while bytes still resemble namespace
    raises(
        lambda: migration.validate_legacy_nvs_prefix(bytes(ghost_nvs)),
        "EMPTY entry",
    )
    no_gc_page = bytearray(original)
    last = migration.NVS_EXTENSION_OFFSET - migration.SECTOR_BYTES
    no_gc_page[last:last + migration.SECTOR_BYTES] = legacy_nvs()[:migration.SECTOR_BYTES]
    full_final, full_evidence = migration.simulate_migration(
        bytes(no_gc_page), table, app, ota
    )
    assert full_evidence["legacy_nvs"]["blank_pages"] == 0
    migration.verify_final_image(bytes(no_gc_page), full_final, table, app, ota)
    raises(
        lambda: migration.simulate_migration(original[:-1], table, app, ota),
        "exactly 8 MiB",
    )

    wrong_source = bytearray(original)
    source = application("0.20.1")
    wrong_source[0x010000:0x010000 + len(source)] = source
    raises(
        lambda: migration.simulate_migration(bytes(wrong_source), table, app, ota),
        "required 0.20.2",
    )
    reviewed_source = migration.application_at(original, 0x010000, 0x32F000)
    reviewed_source_sha = migration.sha256_bytes(reviewed_source)
    assert migration.validate_reviewed_source_application(
        reviewed_source, reviewed_source_sha, len(reviewed_source)
    ) == reviewed_source_sha
    same_version_wrong_bytes = bytearray(reviewed_source)
    same_version_wrong_bytes[-1] ^= 0x01
    raises(
        lambda: migration.validate_reviewed_source_application(
            bytes(same_version_wrong_bytes), reviewed_source_sha,
            len(reviewed_source),
        ),
        "not the reviewed input",
    )
    raises(
        lambda: migration.simulate_migration(
            original, table, application("0.20.4"), ota
        ),
        "required 0.20.3",
    )
    raises(lambda: migration.normalized_mac("3c:0f:02:e5:7a"), "MAC")

    old_sector = original[
        migration.PARTITION_TABLE_OFFSET:
        migration.PARTITION_TABLE_OFFSET + migration.PARTITION_TABLE_SECTOR_BYTES
    ]
    assert migration.classify_resume_table(old_sector, original, table) == "legacy"
    assert migration.classify_resume_table(
        migration.table_sector(table), original, table
    ) == "current"
    partial = bytearray(old_sector)
    partial[0] ^= 1
    assert migration.classify_resume_table(bytes(partial), original, table) == "partial"

    with tempfile.TemporaryDirectory(prefix="kitsu-migration-memory-") as raw:
        temp = Path(raw)
        (temp / "backup-a").mkdir()
        (temp / "backup-b").mkdir()
        raises(
            lambda: migration.validate_backup_volume_pair(
                temp / "backup-a" / "flash.bin",
                temp / "backup-b" / "flash.bin",
            ),
            "distinct storage devices",
        )
        table_path = temp / "partitions.bin"
        app_path = temp / "firmware.bin"
        ota_path = temp / "boot_app0.bin"
        table_path.write_bytes(table)
        app_path.write_bytes(app)
        ota_path.write_bytes(ota)
        oracle_path = temp / "nvs-oracle.json"
        oracle_record = dict(oracle)
        del oracle_record["evidence_sha256"]
        migration.exclusive_json(oracle_path, oracle_record)
        validated_oracle = migration.validate_nvs_oracle_evidence(
            oracle_path, migration.sha256_file(oracle_path), legacy_nvs()
        )
        assert validated_oracle == oracle
        raises(
            lambda: migration.validate_nvs_oracle_evidence(
                oracle_path, "0" * 64, legacy_nvs()
            ),
            "not reviewed",
        )
        evidence = migration.bound_evidence(
            original, table, app, ota, validated_oracle,
            "3c:0f:02:e5:7a:f0",
            migration.sha256_bytes(
                migration.application_at(original, 0x010000, 0x32F000)
            ),
            migration.sha256_bytes(app),
            len(migration.application_at(original, 0x010000, 0x32F000)),
            len(app),
        )
        manifest_path = temp / "migration-manifest.json"
        migration.exclusive_json(manifest_path, evidence)
        manifest_sha = migration.sha256_file(manifest_path)
        assert migration.validate_restore_authority(
            original, manifest_path, manifest_sha, "3c:0f:02:e5:7a:f0",
            evidence["reviewed_source_app0_sha256"],
            evidence["reviewed_source_app0_bytes"],
        ) == manifest_sha
        raises(
            lambda: migration.validate_restore_authority(
                original, manifest_path, "0" * 64, "3c:0f:02:e5:7a:f0",
                evidence["reviewed_source_app0_sha256"],
                evidence["reviewed_source_app0_bytes"],
            ),
            "frozen authority",
        )
        raises(
            lambda: migration.validate_restore_authority(
                original, manifest_path, manifest_sha, "3c:0f:02:e5:7a:f0",
                "0" * 64, evidence["reviewed_source_app0_bytes"],
            ),
            "reviewed input",
        )

        result_preflight = temp / "preflight-result.json"
        preflight_binding = migration.result_binding(evidence, manifest_sha)
        assert migration.preflight_private_json_output(
            result_preflight, preflight_binding
        ) == (
            result_preflight.resolve(), "prepared"
        )
        assert result_preflight.is_file()
        # An exact same-operation reservation is resume-safe and idempotent.
        migration.preflight_private_json_output(
            result_preflight, preflight_binding
        )
        assert not result_preflight.with_name(
            result_preflight.name + ".tmp"
        ).exists()
        raises(
            lambda: migration.preflight_private_json_output(
                ROOT / "unsafe-result.json", preflight_binding
            ),
            "outside the source repository",
        )

        # The durable flash_verified transition is a hard precondition for the
        # sole reset. A failed write-through rename leaves PREPARED intact and
        # must not invoke finish(), so runtime-owned bytes cannot begin changing
        # under a status record that still requires full-flash equality.
        durability_result = temp / "durability-fault.result.json"
        migration.preflight_private_json_output(
            durability_result, preflight_binding
        )
        durability_session = MemorySession(final)
        original_publish = migration.durable_publish
        def fail_verified_publish(
            source: Path, destination: Path, replace: bool,
        ) -> None:
            if replace:
                raise RuntimeError("injected write-through status failure")
            original_publish(source, destination, replace)
        migration.durable_publish = fail_verified_publish
        try:
            raises(
                lambda: migration.finalize_verified_migration(
                    durability_session, durability_result, preflight_binding,
                    "prepared",
                ),
                "write-through status failure",
            )
        finally:
            migration.durable_publish = original_publish
        assert not durability_session.finished
        assert durability_result.read_bytes() == migration.canonical_json_bytes(
            migration.migration_result(preflight_binding, "prepared")
        )

        # Every destructive interruption before the table write leaves the
        # complete old table untouched and never resets the board.
        for failure in range(1, 10):
            session = MemorySession(original, fail_mutation=failure)
            try:
                migration.execute_stages(
                    session, temp, original, table, app, ota,
                    temp / f"failure-{failure}.result.json",
                    evidence, "0" * 64,
                )
            except RuntimeError as error:
                assert "injected" in str(error)
            else:
                raise AssertionError("injected failure unexpectedly completed")
            assert bytes(session.flash[
                migration.PARTITION_TABLE_OFFSET:
                migration.PARTITION_TABLE_OFFSET +
                migration.PARTITION_TABLE_SECTOR_BYTES
            ]) == old_sector
            assert not session.finished

        for failure in range(1, 11):
            session = MemorySession(
                original, fail_mutation=failure, partial_failure=True
            )
            try:
                migration.execute_stages(
                    session, temp, original, table, app, ota,
                    temp / f"partial-{failure}.result.json",
                    evidence, "0" * 64,
                )
            except RuntimeError as error:
                assert "injected" in str(error)
            else:
                raise AssertionError("partial migration fault unexpectedly completed")
            assert not session.finished
            if failure < 10:
                assert bytes(session.flash[
                    migration.PARTITION_TABLE_OFFSET:
                    migration.PARTITION_TABLE_OFFSET +
                    migration.PARTITION_TABLE_SECTOR_BYTES
                ]) == old_sector

        # Failure on the table commit may leave the new sector, but still must
        # not reset; resume classifies and verifies it before any reset.
        table_failure = MemorySession(original, fail_mutation=10)
        try:
            migration.execute_stages(
                table_failure, temp, original, table, app, ota,
                temp / "table-failure.result.json", evidence,
                "0" * 64,
            )
        except RuntimeError as error:
            assert "injected" in str(error)
        else:
            raise AssertionError("table-write interruption unexpectedly completed")
        assert not table_failure.finished

        success = MemorySession(original)
        success_result = temp / "success.result.json"
        migration.preflight_private_json_output(
            success_result, preflight_binding
        )
        migration.execute_stages(
            success, temp, original, table, app, ota, success_result,
            evidence, manifest_sha,
        )
        assert success.finished
        writes = [item for item in success.log if item[0] == "write"]
        assert writes[-1][1] == migration.PARTITION_TABLE_OFFSET
        assert bytes(success.flash) == final
        assert migration.preflight_private_json_output(
            success_result, preflight_binding
        )[1] == "flash_verified"

        # A reset-command failure leaves an exact durable flash-verified state.
        # Resume accepts only the same binding, re-verifies the committed image,
        # retries reset, then atomically records completion.
        reset_failure_result = temp / "reset-failure.result.json"
        migration.preflight_private_json_output(
            reset_failure_result, preflight_binding
        )
        reset_failure = MemorySession(original, fail_finish=True)
        try:
            migration.execute_stages(
                reset_failure, temp, original, table, app, ota,
                reset_failure_result, evidence, manifest_sha,
            )
        except RuntimeError as error:
            assert "reset failure" in str(error)
        else:
            raise AssertionError("injected reset failure unexpectedly completed")
        assert bytes(reset_failure.flash) == final
        _, reset_status = migration.preflight_private_json_output(
            reset_failure_result, preflight_binding
        )
        assert reset_status == "flash_verified"
        resumed_reset = MemorySession(bytes(reset_failure.flash))
        migration.finalize_verified_migration(
            resumed_reset, reset_failure_result, preflight_binding,
            reset_status,
        )
        assert resumed_reset.finished
        assert migration.preflight_private_json_output(
            reset_failure_result, preflight_binding
        )[1] == "flash_verified"
        complete_retry = MemorySession(final)
        migration.finalize_verified_migration(
            complete_retry, reset_failure_result, preflight_binding,
            "flash_verified",
        )
        assert complete_retry.finished

        # If the reset really succeeds but the host loses its ACK, the durable
        # flash_verified result remains authoritative. Runtime changes in NVS,
        # OTA metadata, connectivity state, and coredump must not strand
        # resume; boot/table/apps/SPIFFS are still re-read byte-exactly.
        ack_loss_result = temp / "reset-ack-loss.result.json"
        migration.preflight_private_json_output(
            ack_loss_result, preflight_binding
        )
        ack_loss = MemorySession(
            original,
            fail_finish_after_reset=True,
            finish_mutations=(
                (migration.NVS_OFFSET + 0x120, b"\x11\x22"),
                (migration.OTA_DATA_OFFSET + 0x20, b"\x33\x44"),
                (migration.KITSU_CONN_OFFSET + 0x40, b"\x55\x66"),
                (0x7F0000 + 0x10, b"\x77\x88"),
            ),
        )
        try:
            migration.execute_stages(
                ack_loss, temp, original, table, app, ota,
                ack_loss_result, evidence, manifest_sha,
            )
        except RuntimeError as error:
            assert "acknowledgement loss" in str(error)
        else:
            raise AssertionError("reset ACK-loss fixture unexpectedly completed")
        assert ack_loss.finished and bytes(ack_loss.flash) != final
        assert migration.preflight_private_json_output(
            ack_loss_result, preflight_binding
        )[1] == "flash_verified"
        ack_loss_resume = MemorySession(bytes(ack_loss.flash))
        migration.resume_verified_migration(
            ack_loss_resume, temp, final, ack_loss_result,
            preflight_binding, "flash_verified",
        )
        assert ack_loss_resume.finished
        corrupt_app = MemorySession(bytes(ack_loss.flash))
        corrupt_app.flash[migration.APP0_OFFSET + 0x100] ^= 0x01
        try:
            migration.resume_verified_migration(
                corrupt_app, temp, final, ack_loss_result,
                preflight_binding, "flash_verified",
            )
        except RuntimeError as error:
            assert "runtime immutable" in str(error)
        else:
            raise AssertionError("runtime app corruption unexpectedly resumed")

        restore_binding = {
            "target_mac": evidence["target_mac"],
            "backup_sha256": evidence["backup_sha256"],
            "manifest_sha256": manifest_sha,
            "final_flash_sha256": evidence["backup_sha256"],
        }
        restore_result = temp / "restore-success.result.json"
        migration.preflight_restore_result(restore_result, restore_binding)
        restored = MemorySession(final)
        migration.execute_restore(
            restored, temp, original, restore_result, restore_binding
        )
        restore_writes = [item for item in restored.log if item[0] == "write"]
        assert [item[1] for item in restore_writes] == [
            migration.NVS_OFFSET, 0, migration.PARTITION_TABLE_OFFSET
        ]
        assert restored.log[0][:2] == ("erase", migration.PARTITION_TABLE_OFFSET)
        assert restored.finished and bytes(restored.flash) == original

        current_sector = migration.table_sector(table)
        for failure in range(1, 5):
            interrupted_restore = MemorySession(final, fail_mutation=failure)
            interrupted_result = temp / f"restore-failure-{failure}.json"
            migration.preflight_restore_result(
                interrupted_result, restore_binding
            )
            try:
                migration.execute_restore(
                    interrupted_restore, temp, original,
                    interrupted_result, restore_binding,
                )
            except RuntimeError as error:
                assert "injected" in str(error)
            else:
                raise AssertionError("restore fault unexpectedly completed")
            assert not interrupted_restore.finished
            if failure < 4:
                interrupted_table = bytes(interrupted_restore.flash[
                    migration.PARTITION_TABLE_OFFSET:
                    migration.PARTITION_TABLE_OFFSET +
                    migration.PARTITION_TABLE_SECTOR_BYTES
                ])
                assert interrupted_table != old_sector
                assert interrupted_table != current_sector

        for failure in range(1, 5):
            interrupted_restore = MemorySession(
                final, fail_mutation=failure, partial_failure=True
            )
            interrupted_result = temp / f"restore-partial-{failure}.json"
            migration.preflight_restore_result(
                interrupted_result, restore_binding
            )
            try:
                migration.execute_restore(
                    interrupted_restore, temp, original,
                    interrupted_result, restore_binding,
                )
            except RuntimeError as error:
                assert "injected" in str(error)
            else:
                raise AssertionError("partial restore fault unexpectedly completed")
            assert not interrupted_restore.finished
            interrupted_table = bytes(interrupted_restore.flash[
                migration.PARTITION_TABLE_OFFSET:
                migration.PARTITION_TABLE_OFFSET +
                migration.PARTITION_TABLE_SECTOR_BYTES
            ])
            assert interrupted_table != old_sector
            assert interrupted_table != current_sector

        restore_reset_result = temp / "restore-reset-failure.json"
        migration.preflight_restore_result(
            restore_reset_result, restore_binding
        )
        restore_reset_failure = MemorySession(final, fail_finish=True)
        try:
            migration.execute_restore(
                restore_reset_failure, temp, original,
                restore_reset_result, restore_binding,
            )
        except RuntimeError as error:
            assert "reset failure" in str(error)
        else:
            raise AssertionError("restore reset failure unexpectedly completed")
        assert bytes(restore_reset_failure.flash) == original
        _, restore_status = migration.preflight_restore_result(
            restore_reset_result, restore_binding
        )
        assert restore_status == "flash_verified"
        restore_reset_retry = MemorySession(original)
        migration.finalize_verified_restore(
            restore_reset_retry, restore_reset_result, restore_binding,
            restore_status,
        )
        assert restore_reset_retry.finished
        assert migration.preflight_restore_result(
            restore_reset_result, restore_binding
        )[1] == "flash_verified"

        restore_ack_result = temp / "restore-reset-ack-loss.json"
        migration.preflight_restore_result(
            restore_ack_result, restore_binding
        )
        restore_ack_loss = MemorySession(
            final,
            fail_finish_after_reset=True,
            finish_mutations=(
                (migration.NVS_OFFSET + 0x80, b"\x91\x92"),
                (migration.NVS_EXTENSION_OFFSET + 0x20, b"\x93\x94"),
                (migration.KITSU_CONN_OFFSET + 0x80, b"\x95\x96"),
                (0x7F0000 + 0x20, b"\x97\x98"),
            ),
        )
        try:
            migration.execute_restore(
                restore_ack_loss, temp, original,
                restore_ack_result, restore_binding,
            )
        except RuntimeError as error:
            assert "acknowledgement loss" in str(error)
        else:
            raise AssertionError(
                "restore reset ACK-loss fixture unexpectedly completed"
            )
        assert restore_ack_loss.finished
        assert bytes(restore_ack_loss.flash) != original
        assert migration.preflight_restore_result(
            restore_ack_result, restore_binding
        )[1] == "flash_verified"
        restore_ack_resume = MemorySession(bytes(restore_ack_loss.flash))
        migration.resume_verified_restore(
            restore_ack_resume, temp, original, restore_ack_result,
            restore_binding, "flash_verified",
        )
        assert restore_ack_resume.finished
        corrupt_legacy_app = MemorySession(bytes(restore_ack_loss.flash))
        corrupt_legacy_app.flash[migration.LEGACY_APP0_OFFSET + 0x100] ^= 0x01
        try:
            migration.resume_verified_restore(
                corrupt_legacy_app, temp, original, restore_ack_result,
                restore_binding, "flash_verified",
            )
        except RuntimeError as error:
            assert "runtime immutable" in str(error)
        else:
            raise AssertionError(
                "runtime legacy app corruption unexpectedly resumed"
            )

    invocation = object.__new__(migration.EsptoolSession)
    invocation.python = Path("python.exe")
    invocation.esptool = Path("esptool.py")
    invocation.port = "COM3"
    invocation.baud = 460800
    invocation.connected = False
    first_command = invocation.invocation(["read_mac"])
    assert first_command[first_command.index("--before") + 1] == "default_reset"
    assert first_command[first_command.index("--after") + 1] == "no_reset"
    invocation.connected = True
    held_command = invocation.invocation(["read_flash", "0x0", "0x1000", "x"])
    assert held_command[held_command.index("--before") + 1] == "no_reset"
    assert held_command[held_command.index("--after") + 1] == "no_reset"
    final_command = invocation.invocation(["read_mac"], final_reset=True)
    assert final_command[final_command.index("--after") + 1] == "hard_reset"
    assert final_command[-1] == "read_mac" and "run" not in final_command
    finish_source = inspect.getsource(migration.EsptoolSession.finish)
    assert 'self._run(["read_mac"], final_reset=True)' in finish_source
    assert 'self._run(["run"]' not in finish_source

    writes: list[list[str]] = []
    write_probe = object.__new__(migration.EsptoolSession)
    write_probe._run = lambda command, **kwargs: writes.append(list(command))
    migration.EsptoolSession.write(write_probe, 0, Path("legacy-backup.bin"))
    assert writes == [[
        "write_flash", "--flash_mode", "keep", "--flash_freq", "keep",
        "--flash_size", "keep", "--verify", "0x0", "legacy-backup.bin",
    ]]
    assert "8MB" not in writes[0]

    class Probe:
        def _run(self, command, **kwargs):
            del kwargs
            if command == ["read_mac"]:
                return "MAC: 3c:0f:02:e5:7a:f0\n"
            if command == ["flash_id"]:
                return "Detected flash size: 8MB\n"
            raise AssertionError(command)

    migration.EsptoolSession.probe(Probe(), "3c:0f:02:e5:7a:f0")
    try:
        migration.EsptoolSession.probe(Probe(), "3c:0f:02:e5:7a:f1")
    except RuntimeError as error:
        assert "MAC" in str(error)
    else:
        raise AssertionError("wrong target MAC unexpectedly passed")

    assert "erase_flash" not in (ROOT / "tools" / "migrate_kitsu_0203_storage.py").read_text(
        encoding="utf-8"
    )
    migration.BOOT_APP0_SHA256 = original_hash
    print("Kitsu 0.20.3 storage migration tests passed.")


if __name__ == "__main__":
    main()
