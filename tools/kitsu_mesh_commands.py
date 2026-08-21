"""Host-side Kitsu Mesh serial contract.

This module has no serial, radio, HTTP, or filesystem side effects.  It only
builds the bounded ASCII commands described in docs/mesh_phone_contract.md and
parses the corresponding prefixed JSON records.  Keeping it dependency-free
makes the implemented firmware boundary executable independently of a native
phone transport.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from enum import Enum
from typing import Any


MESH_PROTOCOL = 1
MAX_COMMAND_BYTES = 64
MAX_RECORD_BYTES = 4096
MAX_ADVERT_HEX_CHARS = 510
MESH_PRIVATE_SYNC_WORD = 0x12
# MeshCore companion-v1.17.1 applies preambleLengthForSF(), which selects 32
# symbols at SF8 for the UK/EU Narrow profile.
MESH_PREAMBLE_SYMBOLS = 32

_UID_RE = re.compile(r"KT[0-9A-F]{4}")
_HEX_64_RE = re.compile(r"[0-9A-F]{64}")
_ADVERT_HEX_RE = re.compile(r"(?:[0-9A-F]{2})+")
_TOKEN_RE = re.compile(r"[a-z][a-z0-9_]{0,31}")


class ContractError(ValueError):
    """A command argument or recognized device record violates the contract."""


class LocationMode(str, Enum):
    HIDDEN = "hidden"
    FIXED = "fixed"
    CURRENT_ONCE = "current_once"


class IntroduceScope(str, Enum):
    NEARBY = "nearby"
    MESH = "mesh"


@dataclass(frozen=True)
class Coordinates:
    """A deterministic WGS84 position represented in integer microdegrees."""

    latitude_e6: int
    longitude_e6: int

    def __post_init__(self) -> None:
        _require_plain_int(self.latitude_e6, "latitude_e6")
        _require_plain_int(self.longitude_e6, "longitude_e6")
        if not -90_000_000 <= self.latitude_e6 <= 90_000_000:
            raise ContractError("latitude_e6 must be within -90000000..90000000")
        if not -180_000_000 <= self.longitude_e6 <= 180_000_000:
            raise ContractError("longitude_e6 must be within -180000000..180000000")


@dataclass(frozen=True)
class MeshRadioConfig:
    frequency_hz: int
    bandwidth_hz: int
    spreading_factor: int
    coding_rate: int
    tx_power_dbm: int

    def __post_init__(self) -> None:
        for name in (
            "frequency_hz",
            "bandwidth_hz",
            "spreading_factor",
            "coding_rate",
            "tx_power_dbm",
        ):
            _require_plain_int(getattr(self, name), name)
        if not 863_000_000 <= self.frequency_hz <= 870_000_000:
            raise ContractError("frequency_hz is outside the Kitsu868 EU radio range")
        if self.bandwidth_hz not in {
            7_800,
            10_400,
            15_600,
            20_800,
            31_250,
            41_700,
            62_500,
            125_000,
            250_000,
            500_000,
        }:
            raise ContractError("bandwidth_hz is not an SX1262 LoRa bandwidth")
        if not 5 <= self.spreading_factor <= 12:
            raise ContractError("spreading_factor must be within 5..12")
        if not 5 <= self.coding_rate <= 8:
            raise ContractError("coding_rate must be the LoRa denominator 5..8")
        if not -9 <= self.tx_power_dbm <= 22:
            raise ContractError("tx_power_dbm must be within -9..22")


@dataclass(frozen=True)
class MeshLocation:
    mode: LocationMode
    coordinates: Coordinates | None

    def __post_init__(self) -> None:
        if self.mode is LocationMode.HIDDEN and self.coordinates is not None:
            raise ContractError("hidden location must not contain coordinates")
        if self.mode is not LocationMode.HIDDEN and self.coordinates is None:
            raise ContractError(f"{self.mode.value} location requires coordinates")


@dataclass(frozen=True)
class MeshStatus:
    available: bool
    configured: bool
    enabled: bool
    role: str
    kitsu: bool
    uid: str
    marker: str
    advert_name: str
    public_key: str
    profile: MeshRadioConfig | None
    location: MeshLocation
    time_valid: bool
    tx_policy: str
    tx_unlocked: bool
    tx_ready: bool
    rx_ready: bool
    received_adverts: int
    dropped_adverts: int
    queued_adverts: int
    map_upload: str


@dataclass(frozen=True)
class MeshResult:
    action: str
    status: str
    error: str | None


@dataclass(frozen=True)
class MapPublishPackage:
    status: str
    error: str | None
    uploader: str
    firmware_upload: bool
    advert_hex: str | None
    location: MeshLocation | None


ParsedRecord = MeshStatus | MeshResult | MapPublishPackage


def mesh_status_command() -> str:
    return _command("mesh", "status")


def mesh_enable_command(enabled: bool) -> str:
    """Select/enable UK/EU Narrow, or disable the Mesh adapter."""

    if not isinstance(enabled, bool):
        raise ContractError("enabled must be a boolean")
    return _command("mesh", "config", "on" if enabled else "off")


def mesh_config_command(config: MeshRadioConfig) -> str:
    """Build the advanced exact-profile override used by tests/tools."""

    if not isinstance(config, MeshRadioConfig):
        raise ContractError("config must be MeshRadioConfig")
    return _command(
        "mesh",
        "config",
        str(config.frequency_hz),
        str(config.bandwidth_hz),
        str(config.spreading_factor),
        str(config.coding_rate),
        str(config.tx_power_dbm),
    )


def mesh_location_hidden_command() -> str:
    return _command("mesh", "location", "hidden")


def mesh_location_fixed_command(coordinates: Coordinates) -> str:
    return _location_command("fixed", coordinates)


def mesh_location_current_once_command(coordinates: Coordinates) -> str:
    return _location_command("current-once", coordinates)


def mesh_introduce_command(scope: IntroduceScope) -> str:
    try:
        scope = IntroduceScope(scope)
    except (TypeError, ValueError) as error:
        raise ContractError("scope must be nearby or mesh") from error
    return _command("mesh", "introduce", scope.value)


def mesh_time_command(epoch_seconds: int) -> str:
    _require_plain_int(epoch_seconds, "epoch_seconds")
    if not 1 <= epoch_seconds <= 0xFFFFFFFF:
        raise ContractError("epoch_seconds must fit a non-zero unsigned 32-bit timestamp")
    return _command("mesh", "time", str(epoch_seconds))


def mesh_tx_command(unlocked: bool) -> str:
    if not isinstance(unlocked, bool):
        raise ContractError("unlocked must be a boolean")
    return _command("mesh", "tx", "unlock" if unlocked else "lock")


def mesh_publish_map_command() -> str:
    # Despite the user-facing verb, this asks firmware only to prepare a signed
    # advert.  The phone is the sole HTTP uploader.
    return _command("mesh", "publish-map")


def parse_device_line(line: str | bytes) -> ParsedRecord | None:
    """Parse one recognized prefixed JSON record.

    Unknown prefixes return ``None`` so v1 sync, boot, debug, and future record
    types can coexist.  A recognized prefix with malformed or unsafe JSON
    raises ``ContractError`` instead of being silently interpreted.
    """

    if isinstance(line, bytes):
        try:
            line = line.decode("utf-8", errors="strict")
        except UnicodeDecodeError as error:
            raise ContractError("device record is not valid UTF-8") from error
    if not isinstance(line, str):
        raise ContractError("device line must be str or bytes")
    if len(line.encode("utf-8")) > MAX_RECORD_BYTES:
        raise ContractError("device record exceeds the bounded JSON line size")

    line = line.rstrip("\r\n")
    parsers = {
        "KITSU_MESH ": _parse_mesh_status,
        "KITSU_MESH_RESULT ": _parse_mesh_result,
        "KITSU_MAP_PUBLISH ": _parse_map_publish,
    }
    for prefix, parser in parsers.items():
        if line.startswith(prefix):
            payload = _strict_json_object(line[len(prefix) :])
            _protocol_one(payload)
            return parser(payload)
    return None


def _command(*parts: str) -> str:
    value = " ".join(parts)
    try:
        encoded = value.encode("ascii", errors="strict")
    except UnicodeEncodeError as error:
        raise ContractError("commands must contain ASCII only") from error
    if b"\r" in encoded or b"\n" in encoded or b"\0" in encoded:
        raise ContractError("commands must be exactly one line")
    if len(encoded) > MAX_COMMAND_BYTES:
        raise ContractError("command exceeds the firmware 64-byte input limit")
    return value


def _location_command(mode: str, coordinates: Coordinates) -> str:
    if not isinstance(coordinates, Coordinates):
        raise ContractError("coordinates must be Coordinates")
    return _command(
        "mesh",
        "location",
        mode,
        str(coordinates.latitude_e6),
        str(coordinates.longitude_e6),
    )


def _strict_json_object(payload: str) -> dict[str, Any]:
    def reject_constant(value: str) -> None:
        raise ContractError(f"non-finite JSON number {value} is forbidden")

    def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ContractError(f"duplicate JSON key {key!r}")
            result[key] = value
        return result

    try:
        value = json.loads(
            payload,
            parse_constant=reject_constant,
            object_pairs_hook=unique_object,
        )
    except ContractError:
        raise
    except (json.JSONDecodeError, RecursionError) as error:
        raise ContractError("recognized device record contains invalid JSON") from error
    if not isinstance(value, dict):
        raise ContractError("recognized device record payload must be an object")
    return value


def _protocol_one(value: dict[str, Any]) -> None:
    if _integer(value, "protocol") != MESH_PROTOCOL:
        raise ContractError("unsupported mesh protocol version")


def _parse_mesh_status(value: dict[str, Any]) -> MeshStatus:
    available = _boolean(value, "available")
    configured = _boolean(value, "configured")
    enabled = _boolean(value, "enabled")
    kitsu = _boolean(value, "kitsu")
    tx_ready = _boolean(value, "tx_ready")
    rx_ready = _boolean(value, "rx_ready")
    received_adverts = _nonnegative_integer(value, "received_adverts")
    dropped_adverts = _nonnegative_integer(value, "dropped_adverts")
    queued_adverts = _nonnegative_integer(value, "queued_adverts")
    role = _string(value, "role")
    if role != "client":
        raise ContractError("Kitsu must remain the standard MeshCore client role")
    if not kitsu:
        raise ContractError("KITSU_MESH record does not identify a Kitsu client")

    uid = _string(value, "uid")
    if _UID_RE.fullmatch(uid) is None:
        raise ContractError("uid must look like KTDEAD")
    public_key = _string(value, "public_key")
    if _HEX_64_RE.fullmatch(public_key) is None:
        raise ContractError("public_key must be 32 bytes of uppercase hex")

    marker = _string(value, "marker")
    if marker != "fox":
        raise ContractError("the Kitsu brand marker must be fox")
    advert_name = _string(value, "advert_name")
    if not advert_name or len(advert_name.encode("utf-8")) > 64:
        raise ContractError("advert_name must contain 1..64 UTF-8 bytes")

    raw_profile = value.get("profile")
    profile = None if raw_profile is None else _parse_profile(_object(raw_profile, "profile"))
    if configured != (profile is not None):
        raise ContractError("configured and profile presence disagree")

    map_upload = _string(value, "map_upload")
    if map_upload != "phone_only":
        raise ContractError("map_upload must remain phone_only")

    time_valid = _boolean(value, "time_valid")
    tx_policy = _string(value, "tx_policy")
    if tx_policy not in {"locked", "explicit_session"}:
        raise ContractError("unknown tx_policy")
    tx_unlocked = _boolean(value, "tx_unlocked")
    if tx_unlocked and tx_policy != "explicit_session":
        raise ContractError("a locked TX policy cannot have an unlocked session")
    if tx_ready and not (
        available and configured and enabled and time_valid and tx_unlocked and rx_ready
    ):
        raise ContractError("tx_ready contradicts mesh, clock, or session state")
    if rx_ready and not (available and configured and enabled):
        raise ContractError("rx_ready contradicts mesh configuration state")
    if dropped_adverts > received_adverts:
        raise ContractError("dropped_adverts cannot exceed received_adverts")
    if queued_adverts and not tx_unlocked:
        raise ContractError("locked TX cannot retain queued adverts")

    return MeshStatus(
        available=available,
        configured=configured,
        enabled=enabled,
        role=role,
        kitsu=kitsu,
        uid=uid,
        marker=marker,
        advert_name=advert_name,
        public_key=public_key,
        profile=profile,
        location=_parse_location(_object(value.get("location"), "location")),
        time_valid=time_valid,
        tx_policy=tx_policy,
        tx_unlocked=tx_unlocked,
        tx_ready=tx_ready,
        rx_ready=rx_ready,
        received_adverts=received_adverts,
        dropped_adverts=dropped_adverts,
        queued_adverts=queued_adverts,
        map_upload=map_upload,
    )


def _parse_mesh_result(value: dict[str, Any]) -> MeshResult:
    action = _safe_token(value, "action")
    status = _safe_token(value, "status")
    if status not in {"ok", "queued", "rejected"}:
        raise ContractError("unknown mesh result status")
    raw_error = value.get("error")
    error = None if raw_error is None else _safe_token(value, "error")
    if (status == "rejected") != (error is not None):
        raise ContractError("only a rejected result carries an error")

    return MeshResult(
        action=action,
        status=status,
        error=error,
    )


def _parse_map_publish(value: dict[str, Any]) -> MapPublishPackage:
    status = _safe_token(value, "status")
    if status not in {"ready", "rejected"}:
        raise ContractError("map publish status must be ready or rejected")
    uploader = _string(value, "uploader")
    firmware_upload = _boolean(value, "firmware_upload")
    if uploader != "phone" or firmware_upload:
        raise ContractError("map publication must remain phone-side")

    raw_error = value.get("error")
    error = None if raw_error is None else _safe_token(value, "error")
    raw_advert = value.get("advert_hex")
    advert_hex = None if raw_advert is None else _string(value, "advert_hex")
    raw_location = value.get("location")
    location = None if raw_location is None else _parse_location(_object(raw_location, "location"))

    if status == "ready":
        if error is not None:
            raise ContractError("ready map package must not contain an error")
        if advert_hex is None or _ADVERT_HEX_RE.fullmatch(advert_hex) is None:
            raise ContractError("ready map package requires canonical uppercase advert_hex")
        if len(advert_hex) > MAX_ADVERT_HEX_CHARS:
            raise ContractError("advert_hex exceeds the bounded package size")
        if location is None or location.mode is LocationMode.HIDDEN:
            raise ContractError("ready map package requires an approved position")
    else:
        if error is None:
            raise ContractError("rejected map package requires an error")
        if advert_hex is not None or location is not None:
            raise ContractError("rejected map package must not leak advert or location data")

    return MapPublishPackage(
        status=status,
        error=error,
        uploader=uploader,
        firmware_upload=firmware_upload,
        advert_hex=advert_hex,
        location=location,
    )


def _parse_profile(value: dict[str, Any]) -> MeshRadioConfig:
    return MeshRadioConfig(
        frequency_hz=_integer(value, "frequency_hz"),
        bandwidth_hz=_integer(value, "bandwidth_hz"),
        spreading_factor=_integer(value, "spreading_factor"),
        coding_rate=_integer(value, "coding_rate"),
        tx_power_dbm=_integer(value, "tx_power_dbm"),
    )


def _parse_location(value: dict[str, Any]) -> MeshLocation:
    try:
        mode = LocationMode(_string(value, "mode"))
    except ValueError as error:
        raise ContractError("unknown location mode") from error
    raw_latitude = value.get("lat_e6")
    raw_longitude = value.get("lon_e6")
    if raw_latitude is None and raw_longitude is None:
        coordinates = None
    elif raw_latitude is None or raw_longitude is None:
        raise ContractError("location coordinates must be both present or both null")
    else:
        coordinates = Coordinates(
            _plain_integer(raw_latitude, "lat_e6"),
            _plain_integer(raw_longitude, "lon_e6"),
        )
    return MeshLocation(mode, coordinates)


def _require_plain_int(value: Any, name: str) -> None:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ContractError(f"{name} must be an integer")


def _plain_integer(value: Any, name: str) -> int:
    _require_plain_int(value, name)
    return value


def _integer(value: dict[str, Any], key: str) -> int:
    if key not in value:
        raise ContractError(f"missing integer field {key!r}")
    return _plain_integer(value[key], key)


def _nonnegative_integer(value: dict[str, Any], key: str) -> int:
    field = _integer(value, key)
    if field < 0:
        raise ContractError(f"field {key!r} must be non-negative")
    return field


def _boolean(value: dict[str, Any], key: str) -> bool:
    field = value.get(key)
    if not isinstance(field, bool):
        raise ContractError(f"field {key!r} must be boolean")
    return field


def _string(value: dict[str, Any], key: str) -> str:
    field = value.get(key)
    if not isinstance(field, str):
        raise ContractError(f"field {key!r} must be string")
    return field


def _safe_token(value: dict[str, Any], key: str) -> str:
    field = _string(value, key)
    if _TOKEN_RE.fullmatch(field) is None:
        raise ContractError(f"field {key!r} is not a safe token")
    return field


def _object(value: Any, name: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ContractError(f"field {name!r} must be an object")
    return value


# The normal Kitsu868 profile. ``mesh config on`` selects these exact PHY
# values before enabling reception; TX still requires an explicit session.
# It is instantiated after validation helpers are defined because the frozen
# dataclass validates itself in ``__post_init__``.
UK_EU_NARROW_PROFILE = MeshRadioConfig(869_618_000, 62_500, 8, 5, 22)
