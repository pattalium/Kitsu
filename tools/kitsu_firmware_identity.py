"""Parse the immutable Kitsu identity embedded in a firmware application."""

from __future__ import annotations

import re
import zlib
from pathlib import Path


MAGIC = b"KITSU-ID1|"
SCHEMA = 1
LAYOUT = "kitsu-8m-dual-ota-3m-v1"
DEVICE_CLASS = "heltec-v3.2"
FLASH_BYTES = 0x800000
NVS_OFFSET = 0x009000
NVS_BYTES = 0x040000
OTA_DATA_OFFSET = 0x049000
OTA_DATA_BYTES = 0x002000
APP0_OFFSET = 0x050000
APP1_OFFSET = 0x350000
SLOT_BYTES = 0x300000
JOURNAL_BYTES = 0x1000
MAX_IMAGE_BYTES = SLOT_BYTES - JOURNAL_BYTES
SPIFFS_OFFSET = 0x670000
SPIFFS_BYTES = 0x140000
CONNECTIVITY_OFFSET = 0x7B0000
CONNECTIVITY_BYTES = 0x040000
COREDUMP_OFFSET = 0x7F0000
COREDUMP_BYTES = 0x010000
MARKER_MAX_BYTES = 384
CORE_MAXIMUM = 9_223_372_036_854_775_807

_NUMERIC = r"(?:0|[1-9][0-9]*)"
_NON_NUMERIC = r"(?:[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)"
_PRERELEASE = rf"(?:{_NUMERIC}|{_NON_NUMERIC})"
_BUILD = r"(?:[0-9A-Za-z-]+)"
_SEMVER = re.compile(
    rf"^({_NUMERIC})\.({_NUMERIC})\.({_NUMERIC})"
    rf"(?:-{_PRERELEASE}(?:\.{_PRERELEASE})*)?"
    rf"(?:\+{_BUILD}(?:\.{_BUILD})*)?$"
)
_MARKER = re.compile(
    rb"^KITSU-ID1\|schema=([0-9]+)\|length=([0-9]{4})\|version=([^|]+)"
    rb"\|device_class=([^|]+)\|layout=([^|]+)"
    rb"\|flash=([0-9a-f]{8})"
    rb"\|nvs=([0-9a-f]{8})/([0-9a-f]{8})"
    rb"\|otadata=([0-9a-f]{8})/([0-9a-f]{8})"
    rb"\|app0=([0-9a-f]{8})\|app1=([0-9a-f]{8})"
    rb"\|slot=([0-9a-f]{8})\|journal=([0-9a-f]{8})"
    rb"\|max=([0-9a-f]{8})"
    rb"\|spiffs=([0-9a-f]{8})/([0-9a-f]{8})"
    rb"\|conn=([0-9a-f]{8})/([0-9a-f]{8})"
    rb"\|coredump=([0-9a-f]{8})/([0-9a-f]{8})"
    rb"\|crc32=([0-9a-f]{8})\|end$"
)


def strict_semver(value: str) -> bool:
    try:
        if len(value.encode("ascii")) > 32:
            return False
    except (AttributeError, UnicodeEncodeError):
        return False
    match = _SEMVER.fullmatch(value)
    return bool(match) and all(
        int(identifier) <= CORE_MAXIMUM for identifier in match.groups()
    )


def parse_identity(image: bytes) -> dict[str, int | str]:
    starts: list[int] = []
    cursor = 0
    while True:
        cursor = image.find(MAGIC, cursor)
        if cursor < 0:
            break
        starts.append(cursor)
        cursor += 1
    if len(starts) != 1:
        raise ValueError("application must contain exactly one Kitsu identity marker")
    start = starts[0]
    terminator = image.find(b"\x00", start, start + MARKER_MAX_BYTES + 1)
    if terminator < 0:
        raise ValueError("Kitsu identity marker is not bounded and NUL-terminated")
    raw = image[start:terminator]
    match = _MARKER.fullmatch(raw)
    if not match:
        raise ValueError("Kitsu identity marker has an invalid canonical form")
    (schema_raw, length_raw, version_raw, device_raw, layout_raw, flash_raw,
     nvs_offset_raw, nvs_bytes_raw, ota_offset_raw, ota_bytes_raw, app0_raw,
     app1_raw, slot_raw, journal_raw, maximum_raw, spiffs_offset_raw,
     spiffs_bytes_raw, connectivity_offset_raw, connectivity_bytes_raw,
     coredump_offset_raw, coredump_bytes_raw, crc_raw) = match.groups()
    try:
        version = version_raw.decode("ascii")
        device = device_raw.decode("ascii")
        layout = layout_raw.decode("ascii")
    except UnicodeDecodeError as error:
        raise ValueError("Kitsu identity marker is not ASCII") from error
    schema = int(schema_raw, 10)
    length = int(length_raw, 10)
    flash = int(flash_raw, 16)
    nvs_offset = int(nvs_offset_raw, 16)
    nvs_bytes = int(nvs_bytes_raw, 16)
    ota_offset = int(ota_offset_raw, 16)
    ota_bytes = int(ota_bytes_raw, 16)
    app0 = int(app0_raw, 16)
    app1 = int(app1_raw, 16)
    slot = int(slot_raw, 16)
    journal = int(journal_raw, 16)
    maximum = int(maximum_raw, 16)
    spiffs_offset = int(spiffs_offset_raw, 16)
    spiffs_bytes = int(spiffs_bytes_raw, 16)
    connectivity_offset = int(connectivity_offset_raw, 16)
    connectivity_bytes = int(connectivity_bytes_raw, 16)
    coredump_offset = int(coredump_offset_raw, 16)
    coredump_bytes = int(coredump_bytes_raw, 16)
    crc32 = int(crc_raw, 16)
    crc_prefix = raw[:raw.index(b"|crc32=")]
    if length != len(raw) + 1:
        raise ValueError("Kitsu identity length field does not match the record")
    if zlib.crc32(crc_prefix) & 0xFFFFFFFF != crc32:
        raise ValueError("Kitsu identity CRC32 does not match")
    if (schema != SCHEMA or device != DEVICE_CLASS or layout != LAYOUT):
        raise ValueError("Kitsu identity schema, device, or layout is unsupported")
    if (flash != FLASH_BYTES or nvs_offset != NVS_OFFSET or
            nvs_bytes != NVS_BYTES or ota_offset != OTA_DATA_OFFSET or
            ota_bytes != OTA_DATA_BYTES or app0 != APP0_OFFSET or
            app1 != APP1_OFFSET or
            slot != SLOT_BYTES or journal != JOURNAL_BYTES or
            maximum != MAX_IMAGE_BYTES or
            spiffs_offset != SPIFFS_OFFSET or spiffs_bytes != SPIFFS_BYTES or
            connectivity_offset != CONNECTIVITY_OFFSET or
            connectivity_bytes != CONNECTIVITY_BYTES or
            coredump_offset != COREDUMP_OFFSET or
            coredump_bytes != COREDUMP_BYTES):
        raise ValueError("Kitsu identity flash geometry is unsupported")
    if not strict_semver(version):
        raise ValueError("Kitsu identity firmware version is invalid")
    return {
        "schema": schema,
        "firmware_version": version,
        "device_class": device,
        "layout": layout,
        "flash_bytes": flash,
        "nvs_offset": nvs_offset,
        "nvs_bytes": nvs_bytes,
        "ota_data_offset": ota_offset,
        "ota_data_bytes": ota_bytes,
        "app0_offset": app0,
        "app1_offset": app1,
        "partition_bytes": slot,
        "journal_bytes": journal,
        "maximum_image_bytes": maximum,
        "spiffs_offset": spiffs_offset,
        "spiffs_bytes": spiffs_bytes,
        "connectivity_offset": connectivity_offset,
        "connectivity_bytes": connectivity_bytes,
        "coredump_offset": coredump_offset,
        "coredump_bytes": coredump_bytes,
        "marker_offset": start,
        "marker_bytes": len(raw) + 1,
        "identity_crc32": f"{crc32:08x}",
    }


def source_version(path: Path) -> str:
    source = path.read_text(encoding="utf-8")
    versions = re.findall(
        r'^#define KITSU_FIRMWARE_VERSION_LITERAL "([^"]+)"$',
        source,
        flags=re.MULTILINE,
    )
    required = (
        "constexpr char FIRMWARE_VERSION[] = "
        "KITSU_FIRMWARE_VERSION_LITERAL;"
    )
    if len(versions) != 1 or source.count(required) != 1:
        raise ValueError("firmware source does not have one canonical version authority")
    if not strict_semver(versions[0]):
        raise ValueError("firmware source version is invalid")
    return versions[0]
