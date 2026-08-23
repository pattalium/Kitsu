"""Reference Kitsu v0.9 chat serial contract.

This module has no serial, radio, BLE, Wi-Fi, HTTP, or filesystem side effects.
It builds bounded local commands and validates the prefixed JSON records a
future native companion app consumes.  Command words are ASCII; user-visible
name and message tails are UTF-8 and retain their original case.
"""

from __future__ import annotations

import hashlib
import json
import math
import re
from dataclasses import dataclass
from enum import Enum
from typing import Any
from urllib.parse import parse_qs, urlsplit


CHAT_PROTOCOL = 1
MAX_INPUT_BYTES = 224
MAX_RECORD_BYTES = 4096
MESHCORE_WIRE_TEXT_MAX_BYTES = 160
DIRECT_TEXT_MAX_BYTES = 128
CHANNEL_TEXT_MAX_BYTES = 128
CONTACT_NAME_MAX_BYTES = 31
CHANNEL_NAME_MAX_BYTES = 31
# Explicit serial provisioning is capped at 31 bytes so its bounded command
# always fits.  A verified MeshCore advert, however, can populate the
# transport's complete 32-byte display field; contact listings and inbound
# sender records must accept that one additional byte.
DISCOVERED_NAME_MAX_BYTES = 32
CONTACT_CAPACITY = 12
CHANNEL_CAPACITY = 4
INBOX_CAPACITY = 24
PUBLIC_CHANNEL_SECRET_HEX = "8B3387E9C5CDEA6AC9E5EDBAA115CD72"

_HEX_64 = re.compile(r"[0-9A-Fa-f]{64}")
_HEX_32 = re.compile(r"[0-9A-Fa-f]{32}")
_HEX_12 = re.compile(r"[0-9A-Fa-f]{12}")
_HEX_2 = re.compile(r"[0-9A-Fa-f]{2}")
_HEX_8 = re.compile(r"[0-9A-Fa-f]{8}")
_TOKEN = re.compile(r"[a-z][a-z0-9_]{0,31}")


class ContractError(ValueError):
    """A command argument or recognized device record violates the contract."""


class ContactRole(str, Enum):
    CLIENT = "client"
    REPEATER = "repeater"
    ROOM = "room"
    SENSOR = "sensor"


_ROLE_FROM_QR_TYPE = {
    "1": ContactRole.CLIENT,
    "2": ContactRole.REPEATER,
    "3": ContactRole.ROOM,
    "4": ContactRole.SENSOR,
}


@dataclass(frozen=True)
class ChatStatus:
    available: bool
    meshcore_version: str
    session: str
    contacts: int
    contact_capacity: int
    channels: int
    channel_capacity: int
    messages: int
    inbox_capacity: int
    dropped_messages: int
    time_valid: bool
    tx_unlocked: bool
    tx_ready: bool
    direct_text_max_bytes: int
    channel_text_max_bytes: int


@dataclass(frozen=True)
class ContactRecord:
    index: int
    contact_id: str
    public_key: str
    name: str
    role: ContactRole
    favorite: bool
    last_advert: int
    last_heard: int
    route_hint: str
    dm_capable: bool


@dataclass(frozen=True)
class ContactEnd:
    count: int


@dataclass(frozen=True)
class ChannelRecord:
    index: int
    name: str
    configured: bool
    public: bool
    channel_hash: str | None
    region_scope: str | None


@dataclass(frozen=True)
class ChannelEnd:
    count: int


@dataclass(frozen=True)
class MessageRecord:
    message_id: int
    direction: str
    kind: str
    contact: str | None
    channel: int | None
    sender: str | None
    timestamp: int
    text: str
    state: str
    route: str | None
    snr_db: float | None
    authenticated: bool


@dataclass(frozen=True)
class MessageEnd:
    count: int
    newest_id: int
    dropped: int
    session: str


@dataclass(frozen=True)
class ChatResult:
    action: str
    status: str
    error: str | None
    message_id: int | None
    route: str | None


@dataclass(frozen=True)
class ChatEvent:
    event: str
    message_id: int | None
    state: str | None
    round_trip_ms: int | None


ParsedRecord = (
    ChatStatus
    | ContactRecord
    | ContactEnd
    | ChannelRecord
    | ChannelEnd
    | MessageRecord
    | MessageEnd
    | ChatResult
    | ChatEvent
)


def chat_status_command() -> str:
    return _command("chat status")


def chat_reset_command() -> str:
    """Reset only the device-local MeshCore contacts and channel state.

    This is an explicit owner recovery action. The firmware closes the
    volatile TX session and purges queued protocol packets before resetting;
    issuing this command never transmits a radio packet.
    """

    return _command("chat reset")


def chat_contacts_command() -> str:
    return _command("chat contacts")


def chat_channels_command() -> str:
    return _command("chat channels")


def chat_inbox_command(after_message_id: int = 0) -> str:
    _plain_uint32(after_message_id, "after_message_id")
    return _command("chat inbox" if after_message_id == 0 else f"chat inbox {after_message_id}")


def chat_contact_set_command(
    public_key_hex: str, role: ContactRole | str, name: str
) -> str:
    public_key = _normalized_hex(public_key_hex, _HEX_64, "public_key")
    if int(public_key, 16) == 0:
        raise ContractError("public_key must not be all zero")
    try:
        normalized_role = ContactRole(role)
    except (TypeError, ValueError) as error:
        raise ContractError("role must be client, repeater, room, or sensor") from error
    name = _user_text(name, CONTACT_NAME_MAX_BYTES, "name")
    return _command(f"chat contact set {public_key} {normalized_role.value} {name}")


def chat_contact_drop_command(public_key_hex: str) -> str:
    public_key = _normalized_hex(public_key_hex, _HEX_64, "public_key")
    if int(public_key, 16) == 0:
        raise ContractError("public_key must not be all zero")
    return _command(f"chat contact drop {public_key}")


def chat_channel_set_command(
    slot: int,
    secret_hex: str,
    name: str,
    region_scope: str | None = None,
) -> str:
    _private_channel_slot(slot)
    secret = _normalized_hex(secret_hex, _HEX_32, "channel secret")
    if int(secret, 16) == 0:
        raise ContractError("channel secret must not be all zero")
    name = _user_text(name, CHANNEL_NAME_MAX_BYTES, "name")
    if region_scope is not None and region_scope != "EU":
        raise ContractError("channel region_scope must be EU or absent")
    # The returned local command necessarily carries the secret. Device
    # results and routine events never echo it; callers must not log commands.
    scope = " region_scope=EU" if region_scope == "EU" else ""
    return _command(f"chat channel set {slot}{scope} {secret} {name}")


def chat_channel_clear_command(slot: int) -> str:
    _private_channel_slot(slot)
    return _command(f"chat channel clear {slot}")


def chat_send_direct_command(contact_reference: str, text: str) -> str:
    reference = _normalized_hex(contact_reference, _HEX_12, "contact_reference")
    text = _user_text(text, DIRECT_TEXT_MAX_BYTES, "text")
    return _command(f"chat send dm {reference} {text}")


def chat_send_channel_command(slot: int, text: str) -> str:
    _channel_slot(slot)
    text = _user_text(text, CHANNEL_TEXT_MAX_BYTES, "text")
    return _command(f"chat send ch {slot} {text}")


def chat_contact_command_from_uri(uri: str) -> str:
    """Convert an official query-form MeshCore contact QR into a command.

    This supports `meshcore://contact/add?name=...&public_key=...&type=1`.
    Legacy opaque `meshcore://<advert-hex>` cards need a signed-advert decoder;
    the native app should verify those with an official MeshCore library and
    then call :func:`chat_contact_set_command` with the extracted public data.
    """

    parts = _meshcore_uri(uri, "contact")
    if parts.path != "/add":
        raise ContractError("contact URI path must be /add")
    query = _single_query(parts.query, required={"name", "public_key", "type"})
    role = _ROLE_FROM_QR_TYPE.get(query["type"])
    if role is None:
        raise ContractError("contact URI type must be 1..4")
    return chat_contact_set_command(query["public_key"], role, query["name"])


def chat_channel_command_from_uri(uri: str, slot: int) -> str:
    """Convert an official MeshCore channel-add QR into a private-slot command."""

    parts = _meshcore_uri(uri, "channel")
    if parts.path != "/add":
        raise ContractError("channel URI path must be /add")
    query = _single_query(
        parts.query,
        required={"name", "secret"},
        optional={"region_scope"},
    )
    supplied_scope = query.get("region_scope")
    if supplied_scope is not None and supplied_scope != "EU":
        raise ContractError("channel URI region_scope must be EU")
    return chat_channel_set_command(
        slot, query["secret"], query["name"], supplied_scope
    )


def chat_hashtag_channel_command(name: str, slot: int) -> str:
    """Build a standard guessable hashtag channel using SHA-256(name)[:16]."""

    name = _user_text(name, CHANNEL_NAME_MAX_BYTES, "name")
    if not name.startswith("#"):
        raise ContractError("hashtag channel name must start with #")
    secret = hashlib.sha256(name.encode("utf-8")).digest()[:16].hex().upper()
    return chat_channel_set_command(slot, secret, name)


def parse_device_line(line: str | bytes) -> ParsedRecord | None:
    raw = _decode_line(line)
    recognized = (
        "KITSU_CHAT",
        "KITSU_CONTACT",
        "KITSU_CONTACT_END",
        "KITSU_CHANNEL",
        "KITSU_CHANNEL_END",
        "KITSU_MESSAGE",
        "KITSU_MESSAGE_END",
        "KITSU_CHAT_RESULT",
        "KITSU_CHAT_EVENT",
    )
    prefix = next((value for value in recognized if raw.startswith(value + " ")), None)
    if prefix is None:
        return None
    value = _strict_json_object(raw[len(prefix) + 1 :])
    _protocol(value)

    if prefix == "KITSU_CHAT":
        return _parse_status(value)
    if prefix == "KITSU_CONTACT":
        role = _enum(ContactRole, value, "role")
        public_key = _field_hex(value, "public_key", _HEX_64)
        contact_id = _field_hex(value, "id", _HEX_12)
        if not public_key.startswith(contact_id):
            raise ContractError("contact id must be the first six public-key bytes")
        dm_capable = _field(value, "dm_capable", bool)
        if dm_capable != (role is ContactRole.CLIENT):
            raise ContractError("only client contacts are DM-capable in v0.9")
        return ContactRecord(
            index=_nonnegative(value, "index"),
            contact_id=contact_id,
            public_key=public_key,
            name=_record_text(value, "name", DISCOVERED_NAME_MAX_BYTES),
            role=role,
            favorite=_field(value, "favorite", bool),
            last_advert=_uint32(value, "last_advert"),
            last_heard=_uint32(value, "last_heard"),
            route_hint=_one_of(value, "route_hint", {"direct", "flood"}),
            dm_capable=dm_capable,
        )
    if prefix == "KITSU_CONTACT_END":
        return ContactEnd(_bounded(value, "count", 0, CONTACT_CAPACITY))
    if prefix == "KITSU_CHANNEL":
        index = _bounded(value, "index", 0, CHANNEL_CAPACITY - 1)
        configured = _field(value, "configured", bool)
        public = _field(value, "public", bool)
        if public != (index == 0):
            raise ContractError("only channel zero may be public")
        channel_hash = value.get("hash")
        if channel_hash is not None:
            channel_hash = _validated_hex(channel_hash, _HEX_2, "hash")
        if configured and channel_hash is None:
            raise ContractError("configured channel must include its one-byte hash")
        if not configured and channel_hash is not None:
            raise ContractError("empty channel must use a null hash")
        region_scope = value.get("region_scope")
        if region_scope is not None:
            region_scope = _validated_one_of(
                region_scope, {"EU"}, "region_scope"
            )
        # Missing is intentionally identical to null so tools remain able to
        # consume serial records from pre-0.16.4 firmware.
        if region_scope is not None and (not configured or public):
            raise ContractError(
                "only a configured private channel may use region_scope=EU"
            )
        return ChannelRecord(
            index=index,
            name=_record_text(value, "name", CHANNEL_NAME_MAX_BYTES, allow_empty=True),
            configured=configured,
            public=public,
            channel_hash=channel_hash,
            region_scope=region_scope,
        )
    if prefix == "KITSU_CHANNEL_END":
        return ChannelEnd(_bounded(value, "count", 0, CHANNEL_CAPACITY))
    if prefix == "KITSU_MESSAGE":
        return _parse_message(value)
    if prefix == "KITSU_MESSAGE_END":
        return MessageEnd(
            count=_bounded(value, "count", 0, INBOX_CAPACITY),
            newest_id=_uint32(value, "newest_id"),
            dropped=_nonnegative(value, "dropped"),
            session=_field_hex(value, "session", _HEX_8),
        )
    if prefix == "KITSU_CHAT_RESULT":
        return _parse_result(value)
    return _parse_event(value)


def _parse_status(value: dict[str, Any]) -> ChatStatus:
    available = _field(value, "available", bool)
    if not available:
        raise ContractError("KITSU_CHAT status must describe an available adapter")
    contact_capacity = _bounded(value, "contact_capacity", 1, CONTACT_CAPACITY)
    channel_capacity = _bounded(value, "channel_capacity", 1, CHANNEL_CAPACITY)
    inbox_capacity = _bounded(value, "inbox_capacity", 1, INBOX_CAPACITY)
    direct_limit = _bounded(value, "direct_text_max_bytes", 1, DIRECT_TEXT_MAX_BYTES)
    channel_limit = _bounded(value, "channel_text_max_bytes", 1, CHANNEL_TEXT_MAX_BYTES)
    if direct_limit != DIRECT_TEXT_MAX_BYTES or channel_limit != CHANNEL_TEXT_MAX_BYTES:
        raise ContractError("v0.9 text limits do not match the pinned MeshCore core")
    tx_unlocked = _field(value, "tx_unlocked", bool)
    tx_ready = _field(value, "tx_ready", bool)
    if tx_ready and not tx_unlocked:
        raise ContractError("chat TX cannot be ready while the volatile gate is locked")
    return ChatStatus(
        available=available,
        meshcore_version=_field(value, "meshcore_version", str),
        session=_field_hex(value, "session", _HEX_8),
        contacts=_bounded(value, "contacts", 0, contact_capacity),
        contact_capacity=contact_capacity,
        channels=_bounded(value, "channels", 0, channel_capacity),
        channel_capacity=channel_capacity,
        messages=_bounded(value, "messages", 0, inbox_capacity),
        inbox_capacity=inbox_capacity,
        dropped_messages=_nonnegative(value, "dropped_messages"),
        time_valid=_field(value, "time_valid", bool),
        tx_unlocked=tx_unlocked,
        tx_ready=tx_ready,
        direct_text_max_bytes=direct_limit,
        channel_text_max_bytes=channel_limit,
    )


def _parse_message(value: dict[str, Any]) -> MessageRecord:
    direction = _one_of(value, "direction", {"in", "out"})
    kind = _one_of(value, "kind", {"direct", "channel"})
    contact = value.get("contact")
    channel = value.get("channel")
    if kind == "direct":
        contact = _validated_hex(contact, _HEX_12, "contact")
        if channel is not None:
            raise ContractError("direct message channel must be null")
        text_limit = (
            MESHCORE_WIRE_TEXT_MAX_BYTES
            if direction == "in"
            else DIRECT_TEXT_MAX_BYTES
        )
    else:
        if contact is not None:
            raise ContractError("channel message contact must be null")
        channel = _plain_int(channel, "channel")
        if not 0 <= channel < CHANNEL_CAPACITY:
            raise ContractError("channel must be within 0..3")
        # Received MeshCore group text includes the sender-name prefix, so the
        # stored decoded record may occupy the full 160-byte wire text limit.
        text_limit = MESHCORE_WIRE_TEXT_MAX_BYTES if direction == "in" else CHANNEL_TEXT_MAX_BYTES
    state = _one_of(
        value,
        "state",
        {
            "received",
            "queued",
            "sent",
            "delivered",
            "unconfirmed",
            "failed",
            "cancelled",
        },
    )
    if direction == "in" and state != "received":
        raise ContractError("inbound messages must use state=received")
    if direction == "out" and state == "received":
        raise ContractError("outbound messages cannot use state=received")
    if direction == "out" and kind == "channel" and state in {
        "delivered",
        "unconfirmed",
    }:
        raise ContractError("channel messages have no delivery acknowledgement")
    sender = value.get("sender")
    if direction == "out":
        if sender is not None:
            raise ContractError("outbound message sender must be null")
    elif sender is not None:
        sender = _user_text(sender, DISCOVERED_NAME_MAX_BYTES, "sender")
    if direction == "in" and kind == "direct" and sender is None:
        raise ContractError("inbound direct message requires its contact name")
    route = value.get("route")
    if route is not None:
        route = _validated_one_of(route, {"direct", "flood"}, "route")
    snr = value.get("snr_db")
    if snr is not None:
        if isinstance(snr, bool) or not isinstance(snr, (int, float)) or not math.isfinite(snr):
            raise ContractError("snr_db must be a finite number or null")
        snr = float(snr)
    return MessageRecord(
        message_id=_uint32(value, "id", nonzero=True),
        direction=direction,
        kind=kind,
        contact=contact,
        channel=channel,
        sender=sender,
        timestamp=_uint32(value, "timestamp"),
        text=_record_text(value, "text", text_limit),
        state=state,
        route=route,
        snr_db=snr,
        authenticated=_message_authentication(value, kind),
    )


def _parse_result(value: dict[str, Any]) -> ChatResult:
    action = _machine_token(value, "action")
    status = _one_of(value, "status", {"ok", "queued", "rejected"})
    error = value.get("error")
    if status == "rejected":
        if not isinstance(error, str) or _TOKEN.fullmatch(error) is None:
            raise ContractError("rejected result requires a machine-token error")
    elif error is not None:
        raise ContractError("successful result error must be null")
    message_id = value.get("message_id")
    route = value.get("route")
    if action == "send" and status == "queued":
        message_id = _validated_uint32(message_id, "message_id", nonzero=True)
        route = _validated_one_of(route, {"direct", "flood"}, "route")
    elif message_id is not None or route is not None:
        raise ContractError("only a queued send result carries message_id and route")
    return ChatResult(action, status, error, message_id, route)


def _parse_event(value: dict[str, Any]) -> ChatEvent:
    event = _one_of(value, "event", {"message", "tx", "delivery"})
    message_id = value.get("message_id")
    state = value.get("state")
    round_trip = value.get("round_trip_ms")
    if event == "message":
        message_id = _validated_uint32(message_id, "message_id", nonzero=True)
        if state is not None or round_trip is not None:
            raise ContractError("message event carries only message_id")
    elif event == "tx":
        message_id = _validated_uint32(message_id, "message_id", nonzero=True)
        state = _validated_one_of(state, {"sent", "failed", "cancelled"}, "state")
        if round_trip is not None:
            raise ContractError("TX lifecycle event has no round-trip time")
    elif event == "delivery":
        message_id = _validated_uint32(message_id, "message_id", nonzero=True)
        state = _validated_one_of(state, {"delivered", "unconfirmed"}, "state")
        if state == "delivered":
            round_trip = _validated_uint32(round_trip, "round_trip_ms")
        elif round_trip is not None:
            raise ContractError("unconfirmed delivery has no round-trip time")
    return ChatEvent(event, message_id, state, round_trip)


def _message_authentication(value: dict[str, Any], kind: str) -> bool:
    authenticated = _field(value, "authenticated", bool)
    if authenticated != (kind == "direct"):
        raise ContractError(
            "direct messages are ECDH-MAC authenticated; channel senders are not"
        )
    return authenticated


def _command(value: str) -> str:
    encoded = value.encode("utf-8")
    if len(encoded) > MAX_INPUT_BYTES:
        raise ContractError(f"command exceeds {MAX_INPUT_BYTES} UTF-8 bytes")
    _reject_controls(value, "command")
    return value


def _user_text(value: str, limit: int, field: str) -> str:
    if not isinstance(value, str):
        raise ContractError(f"{field} must be a string")
    if not value:
        raise ContractError(f"{field} must not be empty")
    _reject_controls(value, field)
    try:
        encoded = value.encode("utf-8", "strict")
    except UnicodeError as error:
        raise ContractError(f"{field} must be valid UTF-8") from error
    if len(encoded) > limit:
        raise ContractError(f"{field} exceeds {limit} UTF-8 bytes")
    return value


def _reject_controls(value: str, field: str) -> None:
    for character in value:
        code = ord(character)
        if code <= 0x1F or code == 0x7F or 0x80 <= code <= 0x9F:
            raise ContractError(f"{field} contains a control character")


def _normalized_hex(value: str, pattern: re.Pattern[str], field: str) -> str:
    return _validated_hex(value, pattern, field).upper()


def _validated_hex(value: Any, pattern: re.Pattern[str], field: str) -> str:
    if not isinstance(value, str) or pattern.fullmatch(value) is None:
        raise ContractError(f"{field} has invalid hexadecimal length or content")
    return value.upper()


def _plain_int(value: Any, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ContractError(f"{field} must be an integer")
    return value


def _plain_uint32(value: Any, field: str) -> int:
    value = _plain_int(value, field)
    if not 0 <= value <= 0xFFFFFFFF:
        raise ContractError(f"{field} must fit an unsigned 32-bit integer")
    return value


def _channel_slot(slot: int) -> int:
    slot = _plain_int(slot, "slot")
    if not 0 <= slot < CHANNEL_CAPACITY:
        raise ContractError("slot must be within 0..3")
    return slot


def _private_channel_slot(slot: int) -> int:
    slot = _channel_slot(slot)
    if slot == 0:
        raise ContractError("channel zero is the immutable built-in Public channel")
    return slot


def _meshcore_uri(uri: str, authority: str):
    if not isinstance(uri, str):
        raise ContractError("URI must be a string")
    parts = urlsplit(uri)
    if parts.scheme.lower() != "meshcore" or parts.netloc.lower() != authority:
        raise ContractError(f"URI must be meshcore://{authority}/...")
    if parts.fragment:
        raise ContractError("URI fragment is not supported")
    return parts


def _single_query(
    query: str,
    *,
    required: set[str],
    optional: set[str] | None = None,
) -> dict[str, str]:
    optional = optional or set()
    parsed = parse_qs(query, keep_blank_values=True, strict_parsing=True)
    if set(parsed) - required - optional:
        raise ContractError("URI contains unsupported query parameters")
    if not required.issubset(parsed):
        raise ContractError("URI is missing a required query parameter")
    if any(len(values) != 1 for values in parsed.values()):
        raise ContractError("URI query parameters must not be repeated")
    return {key: values[0] for key, values in parsed.items()}


def _decode_line(line: str | bytes) -> str:
    if isinstance(line, bytes):
        if len(line) > MAX_RECORD_BYTES:
            raise ContractError("device record is too large")
        try:
            line = line.decode("utf-8", "strict")
        except UnicodeError as error:
            raise ContractError("device record is not valid UTF-8") from error
    elif not isinstance(line, str):
        raise ContractError("device record must be str or bytes")
    if len(line.encode("utf-8")) > MAX_RECORD_BYTES:
        raise ContractError("device record is too large")
    return line.removesuffix("\n").removesuffix("\r")


def _strict_json_object(payload: str) -> dict[str, Any]:
    def pairs(values: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in values:
            if key in result:
                raise ContractError(f"duplicate JSON key: {key}")
            result[key] = value
        return result

    def bad_constant(value: str) -> None:
        raise ContractError(f"non-finite JSON number: {value}")

    try:
        value = json.loads(payload, object_pairs_hook=pairs, parse_constant=bad_constant)
    except ContractError:
        raise
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ContractError("invalid JSON device record") from error
    if not isinstance(value, dict):
        raise ContractError("device JSON payload must be an object")
    return value


def _protocol(value: dict[str, Any]) -> None:
    if _field(value, "protocol", int) != CHAT_PROTOCOL:
        raise ContractError("unsupported chat protocol")


def _field(value: dict[str, Any], name: str, expected: type):
    result = value.get(name)
    if expected is int and isinstance(result, bool):
        raise ContractError(f"{name} has incorrect type")
    if not isinstance(result, expected):
        raise ContractError(f"{name} has incorrect type")
    return result


def _field_hex(value: dict[str, Any], name: str, pattern: re.Pattern[str]) -> str:
    return _validated_hex(value.get(name), pattern, name)


def _nonnegative(value: dict[str, Any], name: str) -> int:
    result = _field(value, name, int)
    if result < 0:
        raise ContractError(f"{name} must be non-negative")
    return result


def _bounded(value: dict[str, Any], name: str, lower: int, upper: int) -> int:
    result = _field(value, name, int)
    if not lower <= result <= upper:
        raise ContractError(f"{name} must be within {lower}..{upper}")
    return result


def _validated_uint32(value: Any, name: str, *, nonzero: bool = False) -> int:
    result = _plain_int(value, name)
    lower = 1 if nonzero else 0
    if not lower <= result <= 0xFFFFFFFF:
        raise ContractError(f"{name} must fit an unsigned 32-bit integer")
    return result


def _uint32(value: dict[str, Any], name: str, *, nonzero: bool = False) -> int:
    return _validated_uint32(value.get(name), name, nonzero=nonzero)


def _one_of(value: dict[str, Any], name: str, choices: set[str]) -> str:
    return _validated_one_of(_field(value, name, str), choices, name)


def _validated_one_of(value: Any, choices: set[str], name: str) -> str:
    if not isinstance(value, str) or value not in choices:
        raise ContractError(f"{name} has unsupported value")
    return value


def _machine_token(value: dict[str, Any], name: str) -> str:
    result = _field(value, name, str)
    if _TOKEN.fullmatch(result) is None:
        raise ContractError(f"{name} must be a safe lowercase machine token")
    return result


def _enum(enum_type: type[Enum], value: dict[str, Any], name: str):
    raw = _field(value, name, str)
    try:
        return enum_type(raw)
    except ValueError as error:
        raise ContractError(f"{name} has unsupported value") from error


def _record_text(
    value: dict[str, Any], name: str, limit: int, *, allow_empty: bool = False
) -> str:
    result = _field(value, name, str)
    if not result and allow_empty:
        return result
    return _user_text(result, limit, name)


__all__ = [name for name in globals() if name.startswith("chat_")] + [
    "CHAT_PROTOCOL",
    "MAX_INPUT_BYTES",
    "DIRECT_TEXT_MAX_BYTES",
    "CHANNEL_TEXT_MAX_BYTES",
    "MESHCORE_WIRE_TEXT_MAX_BYTES",
    "CONTACT_NAME_MAX_BYTES",
    "CHANNEL_NAME_MAX_BYTES",
    "DISCOVERED_NAME_MAX_BYTES",
    "CONTACT_CAPACITY",
    "CHANNEL_CAPACITY",
    "INBOX_CAPACITY",
    "PUBLIC_CHANNEL_SECRET_HEX",
    "ContractError",
    "ContactRole",
    "ChatStatus",
    "ContactRecord",
    "ContactEnd",
    "ChannelRecord",
    "ChannelEnd",
    "MessageRecord",
    "MessageEnd",
    "ChatResult",
    "ChatEvent",
    "parse_device_line",
]
