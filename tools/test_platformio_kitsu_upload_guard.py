#!/usr/bin/env python3
"""Host tests for the exact PlatformIO upload-address transformation."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GUARD = ROOT / "tools" / "platformio_kitsu_upload_guard.py"
sys.path.insert(0, str(ROOT / "tools"))
import migrate_kitsu_0203_storage as migration  # noqa: E402


def load_guard():
    spec = importlib.util.spec_from_file_location("kitsu_upload_guard", GUARD)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def raises_value_error(callback, expected: str) -> None:
    try:
        callback()
    except ValueError as error:
        assert expected in str(error), (expected, str(error))
    else:
        raise AssertionError("guard unexpectedly accepted an unsafe upload plan")


def main() -> None:
    guard = load_guard()
    marker = (
        b"KITSU-ID1|schema=1|length=0331|version=0.20.4|"
        b"device_class=heltec-v3.2|layout=kitsu-8m-dual-ota-3m-v1|"
        b"flash=00800000|nvs=00009000/00040000|"
        b"otadata=00049000/00002000|"
        b"app0=00050000|app1=00350000|slot=00300000|"
        b"journal=00001000|max=002ff000|"
        b"spiffs=00670000/00140000|conn=007b0000/00040000|"
        b"coredump=007f0000/00010000|crc32=c1e61d12|end\x00"
    )
    identity = guard.parse_identity(b"\xe9fixture" + marker + b"tail")
    assert identity["firmware_version"] == "0.20.4"
    assert identity["partition_bytes"] == 0x300000
    assert guard.source_version(ROOT / "src" / "main.cpp") == "0.20.4"
    raises_value_error(
        lambda: guard.parse_identity(marker + marker),
        "exactly one",
    )
    raises_value_error(
        lambda: guard.parse_identity(marker.replace(b"c1e61d12", b"00000000")),
        "CRC32",
    )
    nm_output = "\n".join((
        "1 T __wrap_esp_partition_erase_range",
        "2 T esp_partition_erase_range",
        "3 T initArduino",
        "4 T kitsu868::connectivity::destructiveNvsEraseBlocked()",
    ))
    disassembly = """
00000001 <__wrap_esp_partition_erase_range>:
  1: call8 2 <esp_partition_erase_range>
00000002 <initArduino>:
  2: call8 1 <__wrap_esp_partition_erase_range>
00000003 <applicationErase>:
  3: call8 1 <__wrap_esp_partition_erase_range>
"""
    proof = guard.validate_nvs_erase_wrapper_link(nm_output, disassembly)
    assert proof == {"wrapped_call_sites": 2, "real_call_sites": 1}
    raises_value_error(
        lambda: guard.validate_nvs_erase_wrapper_link(
            nm_output,
            disassembly + "\n00000004 <bypass>:\n  4: call8 2 <esp_partition_erase_range>\n",
        ),
        "bypass",
    )
    boot_app0 = r"C:\framework\tools\partitions\boot_app0.bin"
    images = [
        ("0x0000", r"C:\build\bootloader.bin"),
        ("0x8000", r"C:\build\partitions.bin"),
        ("0xe000", boot_app0),
    ]
    rewritten_images = guard.rewrite_flash_extra_images(images)
    assert rewritten_images[-1] == ("0x049000", boot_app0)

    flags = ["--chip", "esp32s3"]
    for offset, image in images:
        flags.extend((offset, image))
    rewritten_flags = guard.rewrite_uploader_flags(flags)
    guard.validate_upload_plan("0x050000", rewritten_flags)
    assert "0xe000" not in [str(value).lower() for value in rewritten_flags]
    assert rewritten_flags[-2:] == ["0x049000", boot_app0]

    with tempfile.TemporaryDirectory(prefix="kitsu-upload-guard-") as raw:
        partition_path = Path(raw) / "partitions.bin"
        partition_path.write_bytes(migration.partition_table(migration.NEW_LAYOUT))
        partition_sha = guard.validate_partition_table(partition_path)
        assert len(partition_sha) == 64
        corrupted_table = bytearray(partition_path.read_bytes())
        corrupted_table[0x2C] ^= 1
        partition_path.write_bytes(corrupted_table)
        raises_value_error(
            lambda: guard.validate_partition_table(partition_path),
            "MD5",
        )

        journal_clear = guard.prepare_journal_clear(Path(raw))
        assert journal_clear.read_bytes() == b"\xff" * 0x1000
        complete_images = guard.add_journal_clear_images(
            rewritten_images, journal_clear
        )
        assert [(int(str(offset), 0), Path(str(path)).name)
                for offset, path in complete_images[-2:]] == [
            (0x34F000, "kitsu_ota_journal_ff.bin"),
            (0x64F000, "kitsu_ota_journal_ff.bin"),
        ]
        complete_flags = guard.add_journal_clear_flags(
            rewritten_flags, journal_clear
        )
        guard.validate_upload_plan("0x050000", complete_flags, journal_clear)
        raises_value_error(
            lambda: guard.add_journal_clear_flags(complete_flags, journal_clear),
            "already present",
        )
        wrong_flags = list(complete_flags)
        wrong_flags[-2] = "0x63f000"
        raises_value_error(
            lambda: guard.validate_upload_plan(
                "0x050000", wrong_flags, journal_clear
            ),
            "journal clears",
        )

    raises_value_error(
        lambda: guard.rewrite_flash_extra_images(images[:-1]),
        "exactly three",
    )
    raises_value_error(
        lambda: guard.rewrite_flash_extra_images(
            images + [("0xe000", boot_app0)]
        ),
        "exactly three",
    )
    raises_value_error(
        lambda: guard.rewrite_flash_extra_images(
            images[:-1] + [("0x49000", boot_app0)]
        ),
        "mapping is unexpected",
    )
    raises_value_error(
        lambda: guard.validate_upload_plan("0x10000", rewritten_flags),
        "application upload offset",
    )
    raises_value_error(
        lambda: guard.validate_upload_plan("0x50000", flags),
        "NVS extension",
    )

    profile = (ROOT / "platformio.ini").read_text(encoding="utf-8")
    assert "board_upload.offset_address = 0x050000" in profile
    assert "extra_scripts = post:tools/platformio_kitsu_upload_guard.py" in profile
    guard_source = GUARD.read_text(encoding="utf-8")
    assert "generic PlatformIO upload is permanently" in guard_source
    print("Kitsu PlatformIO upload guard tests passed.")


if __name__ == "__main__":
    main()
