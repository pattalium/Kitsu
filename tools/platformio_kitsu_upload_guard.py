"""Fail-closed PlatformIO upload layout guard for Kitsu 0.20.3+.

Arduino-ESP32 2.0.17 unconditionally adds ``boot_app0.bin`` at 0x0e000.
That address is part of Kitsu's expanded NVS partition, so leaving the
framework default in the esptool command would corrupt persistent state. This
post script proves the corrected reviewed plan while permanently blocking the
generic physical upload target.

The one-time legacy-to-0.20.3 repartition must use the separately reviewed
table-last migration workflow.  An ordinary PlatformIO upload is deliberately
incapable of performing that transition.
"""

from __future__ import annotations

import hashlib
import re
import struct
import subprocess
from pathlib import Path
from pathlib import PurePath
from typing import Any, Iterable

try:
    from kitsu_firmware_identity import parse_identity, source_version
except ModuleNotFoundError:
    from tools.kitsu_firmware_identity import parse_identity, source_version


EXPECTED_APP_OFFSET = 0x050000
FRAMEWORK_OTA_DATA_OFFSET = 0x00E000
KITSU_OTA_DATA_OFFSET = 0x049000
APP0_JOURNAL_OFFSET = 0x34F000
APP1_JOURNAL_OFFSET = 0x64F000
MAX_APPLICATION_BYTES = 0x2FF000
BOOT_APP0_BYTES = 0x2000
BOOT_APP0_SHA256 = "f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0"
JOURNAL_CLEAR_BYTES = 0x1000
JOURNAL_CLEAR_SHA256 = "f47a8ec3e9aff2318d896942282ad4fe37d6391c82914f54a5da8a37de1300c6"
PARTITION_TABLE_BYTES = 0x0C00
EXPECTED_LAYOUT = (
    ("nvs", 0x01, 0x02, 0x009000, 0x040000, 0),
    ("otadata", 0x01, 0x00, 0x049000, 0x002000, 0),
    ("app0", 0x00, 0x10, 0x050000, 0x300000, 0),
    ("app1", 0x00, 0x11, 0x350000, 0x300000, 0),
    ("spiffs", 0x01, 0x82, 0x670000, 0x140000, 0),
    ("kitsu_conn", 0x01, 0x40, 0x7B0000, 0x040000, 0),
    ("coredump", 0x01, 0x03, 0x7F0000, 0x010000, 0),
)


def _integer(value: Any) -> int:
    if isinstance(value, int):
        return value
    return int(str(value), 0)


def _is_boot_app0(value: Any) -> bool:
    return PurePath(str(value).replace("\\", "/")).name.lower() == "boot_app0.bin"


def rewrite_flash_extra_images(
    images: Iterable[tuple[Any, Any]],
) -> list[tuple[Any, Any]]:
    images = list(images)
    expected = {
        "bootloader.bin": 0x000000,
        "partitions.bin": 0x008000,
        "boot_app0.bin": FRAMEWORK_OTA_DATA_OFFSET,
    }
    if len(images) != len(expected):
        raise ValueError("exactly three reviewed framework flash images are required")
    output: list[tuple[Any, Any]] = []
    found = 0
    for offset, image in images:
        name = PurePath(str(image).replace("\\", "/")).name.lower()
        if name not in expected or _integer(offset) != expected[name]:
            raise ValueError("framework flash image mapping is unexpected")
        if name == "boot_app0.bin":
            found += 1
            output.append((f"0x{KITSU_OTA_DATA_OFFSET:06x}", image))
        else:
            output.append((offset, image))
    if found != 1:
        raise ValueError("exactly one framework boot_app0.bin image is required")
    return output


def validate_boot_app0(path: Any) -> None:
    candidate = Path(str(path))
    if not candidate.is_file() or candidate.stat().st_size != BOOT_APP0_BYTES:
        raise ValueError("boot_app0.bin is missing or is not exactly 8192 bytes")
    digest = hashlib.sha256(candidate.read_bytes()).hexdigest()
    if digest != BOOT_APP0_SHA256:
        raise ValueError("boot_app0.bin does not match the pinned framework SHA-256")


def validate_partition_table(path: Any) -> str:
    candidate = Path(str(path))
    if not candidate.is_file() or candidate.stat().st_size != PARTITION_TABLE_BYTES:
        raise ValueError("partitions.bin is missing or is not exactly 0xC00 bytes")
    data = candidate.read_bytes()
    records = []
    cursor = 0
    md5_seen = False
    while cursor + 32 <= len(data):
        entry = data[cursor:cursor + 32]
        if entry == b"\xff" * 32:
            break
        if entry[:2] == b"\xeb\xeb":
            if entry[2:16] != b"\xff" * 14:
                raise ValueError("partitions.bin MD5 marker is malformed")
            if entry[16:] != hashlib.md5(data[:cursor]).digest():  # nosec B324
                raise ValueError("partitions.bin embedded MD5 does not verify")
            md5_seen = True
            cursor += 32
            break
        magic, part_type, subtype, offset, size = struct.unpack_from(
            "<HBBII", entry, 0
        )
        if magic != 0x50AA:
            raise ValueError("partitions.bin contains an invalid entry magic")
        label_raw = entry[12:28]
        label = label_raw.split(b"\x00", 1)[0].decode("ascii")
        flags = struct.unpack_from("<I", entry, 28)[0]
        records.append((label, part_type, subtype, offset, size, flags))
        cursor += 32
    if not md5_seen or any(value != 0xFF for value in data[cursor:]):
        raise ValueError("partitions.bin has no terminal MD5 or has trailing bytes")
    if tuple(records) != EXPECTED_LAYOUT:
        raise ValueError("partitions.bin is not the exact 0.20.3 dual-slot layout")
    return hashlib.sha256(data).hexdigest()


def prepare_journal_clear(build_dir: Any) -> Path:
    output = Path(str(build_dir)) / "kitsu_ota_journal_ff.bin"
    output.parent.mkdir(parents=True, exist_ok=True)
    expected = b"\xff" * JOURNAL_CLEAR_BYTES
    if output.exists() and output.read_bytes() != expected:
        raise ValueError("existing OTA journal clear artifact is not exact FF")
    if not output.exists():
        output.write_bytes(expected)
    if (output.stat().st_size != JOURNAL_CLEAR_BYTES or
            hashlib.sha256(output.read_bytes()).hexdigest() !=
            JOURNAL_CLEAR_SHA256):
        raise ValueError("OTA journal clear artifact failed exact validation")
    return output


def add_journal_clear_images(
    images: Iterable[tuple[Any, Any]], journal_clear: Any,
) -> list[tuple[Any, Any]]:
    output = list(images)
    if any(PurePath(str(image).replace("\\", "/")).name.lower() ==
           "kitsu_ota_journal_ff.bin" for _, image in output):
        raise ValueError("OTA journal clear image is already present")
    output.extend((
        (f"0x{APP0_JOURNAL_OFFSET:06x}", journal_clear),
        (f"0x{APP1_JOURNAL_OFFSET:06x}", journal_clear),
    ))
    return output


def rewrite_uploader_flags(flags: Iterable[Any]) -> list[Any]:
    output = list(flags)
    matches = [index for index, value in enumerate(output) if _is_boot_app0(value)]
    if len(matches) != 1 or matches[0] == 0:
        raise ValueError("exactly one boot_app0.bin uploader argument is required")
    image_index = matches[0]
    if _integer(output[image_index - 1]) != FRAMEWORK_OTA_DATA_OFFSET:
        raise ValueError("boot_app0.bin uploader offset is not the framework default")
    output[image_index - 1] = f"0x{KITSU_OTA_DATA_OFFSET:06x}"
    return output


def add_journal_clear_flags(flags: Iterable[Any], journal_clear: Any) -> list[Any]:
    output = list(flags)
    if any(PurePath(str(value).replace("\\", "/")).name.lower() ==
           "kitsu_ota_journal_ff.bin" for value in output):
        raise ValueError("OTA journal clear uploader argument is already present")
    output.extend((
        f"0x{APP0_JOURNAL_OFFSET:06x}", journal_clear,
        f"0x{APP1_JOURNAL_OFFSET:06x}", journal_clear,
    ))
    return output


def validate_upload_plan(
    app_offset: Any, flags: Iterable[Any], journal_clear: Any | None = None,
) -> None:
    if _integer(app_offset) != EXPECTED_APP_OFFSET:
        raise ValueError("application upload offset is not 0x050000")
    values = [str(value).lower().replace("\\", "/") for value in flags]
    if any(value in {"0xe000", "0x0e000", "0x00e000"} for value in values):
        raise ValueError("uploader still references the NVS extension at 0x0e000")
    boot_indices = [index for index, value in enumerate(values)
                    if value.endswith("/boot_app0.bin") or value == "boot_app0.bin"]
    if len(boot_indices) != 1 or boot_indices[0] == 0:
        raise ValueError("uploader boot_app0.bin argument is missing or ambiguous")
    if _integer(values[boot_indices[0] - 1]) != KITSU_OTA_DATA_OFFSET:
        raise ValueError("uploader boot_app0.bin is not bound to 0x049000")
    mappings = {}
    for index, value in enumerate(values):
        name = PurePath(value).name.lower()
        if name in {"bootloader.bin", "partitions.bin", "boot_app0.bin"}:
            if index == 0 or name in mappings:
                raise ValueError("uploader framework-image mapping is ambiguous")
            mappings[name] = _integer(values[index - 1])
    if mappings != {
        "bootloader.bin": 0x000000,
        "partitions.bin": 0x008000,
        "boot_app0.bin": KITSU_OTA_DATA_OFFSET,
    }:
        raise ValueError("uploader has missing, extra, or misaddressed framework images")
    journal_name = "kitsu_ota_journal_ff.bin"
    journal_mappings = []
    for index, value in enumerate(values):
        if PurePath(value).name.lower() != journal_name:
            continue
        if index == 0:
            raise ValueError("OTA journal clear uploader mapping is malformed")
        journal_mappings.append(_integer(values[index - 1]))
    if journal_clear is None:
        if journal_mappings:
            raise ValueError("unexpected OTA journal clear uploader mappings")
    else:
        expected_journal = str(journal_clear).lower().replace("\\", "/")
        if (journal_mappings != [APP0_JOURNAL_OFFSET, APP1_JOURNAL_OFFSET] or
                any(PurePath(value).name.lower() == journal_name and
                    value != expected_journal for value in values)):
            raise ValueError("OTA journal clears are missing, extra, or misaddressed")


def validate_nvs_erase_wrapper_link(nm_output: str, disassembly: str) -> dict[str, int]:
    required = {
        "__wrap_esp_partition_erase_range",
        "esp_partition_erase_range",
        "initArduino",
        "kitsu868::connectivity::destructiveNvsEraseBlocked()",
    }
    missing = [symbol for symbol in required if symbol not in nm_output]
    if missing:
        raise ValueError("linked ELF is missing the NVS erase-wrapper symbols")
    function_pattern = re.compile(
        r"^[0-9a-f]+ <([^>]+)>:\n(.*?)(?=^[0-9a-f]+ <|\Z)",
        flags=re.MULTILINE | re.DOTALL,
    )
    functions = {name: body for name, body in function_pattern.findall(disassembly)}
    wrapper = functions.get("__wrap_esp_partition_erase_range")
    init_arduino = functions.get("initArduino")
    if wrapper is None or init_arduino is None:
        raise ValueError("linked ELF disassembly has no wrapper or initArduino body")
    direct_pattern = re.compile(
        r"\bcall\w*\s+[0-9a-f]+ <esp_partition_erase_range>"
    )
    wrapper_pattern = re.compile(
        r"\bcall\w*\s+[0-9a-f]+ <__wrap_esp_partition_erase_range>"
    )
    direct_calls = len(direct_pattern.findall(disassembly))
    wrapped_calls = len(wrapper_pattern.findall(disassembly))
    if direct_calls != 1 or len(direct_pattern.findall(wrapper)) != 1:
        raise ValueError("linked ELF has an NVS erase-wrapper bypass")
    if wrapped_calls < 2 or len(wrapper_pattern.findall(init_arduino)) != 1:
        raise ValueError("Arduino initialization is not bound to the NVS erase wrapper")
    return {
        "wrapped_call_sites": wrapped_calls,
        "real_call_sites": direct_calls,
    }


def _configure_platformio() -> None:
    # ``Import`` is injected by SCons.  Keeping all transformation logic in
    # pure functions above lets the host test exercise the exact guard code.
    Import("env")  # type: ignore[name-defined]  # noqa: F821
    from SCons.Script import COMMAND_LINE_TARGETS  # type: ignore[import-not-found]

    requested = {str(target).lower() for target in COMMAND_LINE_TARGETS}
    always_blocked = {"erase", "uploadfs", "uploadfsota"}
    if requested & always_blocked or any(
        target.startswith("__uploadfs") for target in requested
    ):
        print(
            "KITSU_UPLOAD_BLOCKED: erase/filesystem targets are never permitted "
            "by the NVS-preserving firmware profile"
        )
        env.Exit(1)  # type: ignore[name-defined]  # noqa: F821

    journal_clear = prepare_journal_clear(env.subst("$BUILD_DIR"))  # type: ignore[name-defined]  # noqa: F821
    extra_images = rewrite_flash_extra_images(env.get("FLASH_EXTRA_IMAGES", []))  # type: ignore[name-defined]  # noqa: F821
    validate_boot_app0(next(image for _, image in extra_images if _is_boot_app0(image)))
    extra_images = add_journal_clear_images(extra_images, journal_clear)
    env.Replace(FLASH_EXTRA_IMAGES=extra_images)  # type: ignore[name-defined]  # noqa: F821

    uploader_flags = rewrite_uploader_flags(env.get("UPLOADERFLAGS", []))  # type: ignore[name-defined]  # noqa: F821
    uploader_flags = add_journal_clear_flags(uploader_flags, journal_clear)
    env.Replace(UPLOADERFLAGS=uploader_flags)  # type: ignore[name-defined]  # noqa: F821
    app_offset = env.subst("$ESP32_APP_OFFSET")  # type: ignore[name-defined]  # noqa: F821
    validate_upload_plan(app_offset, uploader_flags, journal_clear)

    if "upload" in requested or any(
        target.startswith("__upload") and not target.startswith("__uploadfs")
        for target in requested
    ):
        print(
            "KITSU_UPLOAD_BLOCKED: generic PlatformIO upload is permanently "
            "disabled; use the table-last migration once, then signed BLE OTA"
        )
        env.Exit(1)  # type: ignore[name-defined]  # noqa: F821

    reviewed_version = source_version(
        Path(env.subst("$PROJECT_DIR")) / "src" / "main.cpp"
    )

    def validate_application(source: Any, target: Any, env: Any) -> None:
        del source, target
        candidate = Path(env.subst("$BUILD_DIR")) / f"{env.subst('$PROGNAME')}.bin"
        if (not candidate.is_file() or candidate.stat().st_size < 24 or
                candidate.stat().st_size > MAX_APPLICATION_BYTES):
            print(
                "KITSU_BUILD_ERROR: application exceeds the 0x2ff000 OTA "
                "image boundary or is missing"
            )
            raise RuntimeError("unsafe application image size")
        image = candidate.read_bytes()
        if image[:1] != b"\xe9":
            raise RuntimeError("application is not an ESP image")
        identity = parse_identity(image)
        if identity["firmware_version"] != reviewed_version:
            raise RuntimeError("application identity does not match firmware source")
        print(
            f"KITSU_APP_BOUNDARY_OK bytes={candidate.stat().st_size} "
            f"maximum=0x2ff000 version={reviewed_version} "
            f"identity_offset=0x{identity['marker_offset']:x}"
        )

    env.AddPostAction(  # type: ignore[name-defined]  # noqa: F821
        "$BUILD_DIR/${PROGNAME}.bin", validate_application
    )

    def validate_partitions(source: Any, target: Any, env: Any) -> None:
        del source, target
        candidate = Path(env.subst("$BUILD_DIR")) / "partitions.bin"
        digest = validate_partition_table(candidate)
        print(f"KITSU_PARTITION_LAYOUT_OK sha256={digest}")

    env.AddPostAction(  # type: ignore[name-defined]  # noqa: F821
        "$BUILD_DIR/partitions.bin", validate_partitions
    )

    def validate_erase_wrapper(source: Any, target: Any, env: Any) -> None:
        del source, target
        candidate = Path(env.subst("$BUILD_DIR")) / f"{env.subst('$PROGNAME')}.elf"
        def locate_tool(name: str) -> str:
            for candidate_name in (name, name + ".exe"):
                located = env.WhereIs(candidate_name)
                if located:
                    return str(located)
            raise RuntimeError(f"cannot locate linked-ELF inspection tool {name}")

        commands = (
            (locate_tool("xtensa-esp32s3-elf-nm"), ["-C", str(candidate)]),
            (locate_tool("xtensa-esp32s3-elf-objdump"),
             ["-d", "-C", str(candidate)]),
        )
        outputs = []
        for executable, arguments in commands:
            completed = subprocess.run(
                [executable, *arguments], check=False, capture_output=True,
                text=True, encoding="utf-8", errors="strict",
            )
            if completed.returncode != 0 or completed.stderr:
                raise RuntimeError("cannot inspect the linked NVS erase wrapper")
            outputs.append(completed.stdout)
        proof = validate_nvs_erase_wrapper_link(outputs[0], outputs[1])
        print(
            "KITSU_NVS_ERASE_WRAP_OK "
            f"wrapped_call_sites={proof['wrapped_call_sites']} "
            f"real_call_sites={proof['real_call_sites']}"
        )

    env.AddPostAction(  # type: ignore[name-defined]  # noqa: F821
        "$BUILD_DIR/${PROGNAME}.elf", validate_erase_wrapper
    )

    def print_plan(source: Any, target: Any, env: Any) -> None:
        del source, target, env
        print(
            "KITSU_UPLOAD_PLAN app=0x050000 otadata=0x049000 "
            "partitions=0x008000 journals=0x34f000,0x64f000 "
            "nvs_forbidden=0x00e000 generic_upload=blocked migration=false"
        )

    env.AddCustomTarget(  # type: ignore[name-defined]  # noqa: F821
        "kitsu_upload_plan",
        ["$BUILD_DIR/partitions.bin", "$BUILD_DIR/${PROGNAME}.bin"],
        print_plan,
        title="Auditing the non-destructive post-migration upload plan",
        description="Print exact Kitsu application/OTA-data upload addresses",
    )


try:
    Import  # type: ignore[name-defined]  # noqa: F821
except NameError:
    # Ordinary Python import for the pure host tests.
    pass
else:
    _configure_platformio()
