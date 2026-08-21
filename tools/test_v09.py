"""Receive-only hardware QA for Kitsu868 0.9.0 and MeshCore v1.17.1.

This harness talks to an already-flashed Heltec V3.2 over USB serial.  It
never flashes, sets the MeshCore clock, opens the volatile TX gate, introduces
the node, sends chat, changes contacts/channels, or resets messaging storage.
Its serial writer uses a small positive allow-list, and explicitly rejects
every TX-capable command family.

The board is reset immediately after the port opens so each QA phase starts
with Kitsu's boot-scoped TX gate locked.  Enabling UK/EU Narrow starts passive
RX only.  Optional message/advert requirements merely observe asynchronous
records and query the RAM-only inbox; they never acknowledge traffic while
the gate is locked.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

from install_pack import PACK_PARTITION_BYTES, PackInfo, PackValidationError, validate_pack


FIRMWARE_NAME = "Kitsu868"
FIRMWARE_VERSION = "0.9.0"
MESHCORE_VERSION = "1.17.1"
EXPECTED_BOARD = "heltec-v3.2"

CONTACT_CAPACITY = 12
CHANNEL_CAPACITY = 4
INBOX_CAPACITY = 24
OUTBOUND_TEXT_BYTES = 128
PUBLIC_CHANNEL_HASH = "11"
DEFAULT_PACK = Path(__file__).resolve().parents[1] / "assets" / "packs" / "cat.k868"

UK_EU_NARROW = {
    "id": "4B55454E",
    "name": "UK/EU Narrow",
    "frequency_hz": 869_618_000,
    "bandwidth_hz": 62_500,
    "spreading_factor": 8,
    "coding_rate": 5,
    "sync_word": 0x12,
    "preamble_symbols": 32,
    "tx_power_dbm": 22,
}

BOOT_PATTERN = re.compile(
    r"KITSU_BOOT\s+firmware=(\S+)\s+version=(\S+)\s+"
    r"board=(\S+)\s+tx_enabled=(true|false)"
)
PUBLIC_KEY_PATTERN = re.compile(r"[0-9A-F]{64}")
CONTACT_ID_PATTERN = re.compile(r"[0-9A-F]{12}")
PUBLIC_PREFIX_PATTERN = re.compile(r"[0-9A-F]{8}")
SESSION_PATTERN = re.compile(r"[0-9A-F]{8}")
BYTE_HASH_PATTERN = re.compile(r"[0-9A-F]{2}")

# This is deliberately an allow-list.  Query commands with a numeric inbox
# cursor are admitted separately by ``safe_command``.
SAFE_COMMANDS = {
    "selftest",
    "mesh status",
    "mesh config on",
    "mesh location hidden",
    "mesh publish-map",
    "chat status",
    "chat contacts",
    "chat channels",
    "chat inbox",
}
FORBIDDEN_COMMAND_PREFIXES = (
    "mesh tx",
    "mesh introduce",
    "mesh time",
    "chat send",
    "chat contact",
    "chat channel",
    "chat reset",
    "listen",
    "send",
    "transmit",
    "tx",
    "inject",
)


class TestFailure(RuntimeError):
    """An assertion failure with a stable process exit code."""

    def __init__(self, message: str, code: int = 3) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class JsonRecord:
    value: dict[str, Any]
    line: str


@dataclass(frozen=True)
class AdvertObservation:
    kind: str
    name: str
    public_prefix: str


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            f"Run receive-only Kitsu868 {FIRMWARE_VERSION}/MeshCore QA over USB. "
            "The command allow-list cannot unlock or transmit."
        )
    )
    parser.add_argument(
        "--port",
        default="COM3",
        help="Heltec USB serial port (default: COM3; opened only when QA runs)",
    )
    parser.add_argument(
        "--baud", type=int, default=115200, help="serial baud (default: 115200)"
    )
    parser.add_argument(
        "--expected-uid",
        required=True,
        help="exact six-character UID read from the device (format example: KTDEAD)",
    )
    parser.add_argument(
        "--expected-name",
        default="CAT",
        help="installed companion display name (default: CAT)",
    )
    parser.add_argument(
        "--expected-pack",
        type=Path,
        default=DEFAULT_PACK,
        help="local .k868 whose validated metadata must match the installed slot",
    )
    parser.add_argument(
        "--enable-rx",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "select/persist UK/EU Narrow and enable passive MeshCore RX "
            "without opening TX (default: enabled)"
        ),
    )
    parser.add_argument(
        "--observe-seconds",
        type=float,
        default=0.0,
        metavar="SECONDS",
        help="passively observe adverts/messages after reboot (default: 0)",
    )
    parser.add_argument(
        "--require-advert",
        action="append",
        choices=("any", "client", "repeater"),
        default=[],
        metavar="TYPE",
        help="require a passive signed advert; repeat to require multiple types",
    )
    parser.add_argument(
        "--require-message",
        action="append",
        choices=("any", "channel", "direct"),
        default=[],
        metavar="KIND",
        help=(
            "require an inbound message in the passive window; repeat to "
            "require channel and direct"
        ),
    )
    return parser.parse_args(argv)


def require(condition: bool, message: str, code: int = 6) -> None:
    if not condition:
        raise TestFailure(message, code)


def require_exact(record: dict[str, Any], expected: dict[str, Any], label: str) -> None:
    differences = {
        key: (record.get(key), wanted)
        for key, wanted in expected.items()
        if record.get(key) != wanted
    }
    require(not differences, f"{label} mismatch: {differences}")


def integer_field(
    record: dict[str, Any], key: str, minimum: int = 0, maximum: int = 0xFFFFFFFF
) -> int:
    value = record.get(key)
    require(
        not isinstance(value, bool) and isinstance(value, int),
        f"field {key!r} is not an integer: {value!r}",
    )
    require(minimum <= value <= maximum, f"field {key!r} outside range: {value}")
    return value


def number_field(record: dict[str, Any], key: str) -> float:
    value = record.get(key)
    require(
        not isinstance(value, bool) and isinstance(value, (int, float)),
        f"field {key!r} is not numeric: {value!r}",
    )
    result = float(value)
    require(math.isfinite(result), f"field {key!r} is not finite")
    return result


def collect(port: Any, seconds: float) -> str:
    deadline = time.monotonic() + seconds
    chunks: list[bytes] = []
    while time.monotonic() < deadline:
        data = port.read(port.in_waiting or 1)
        if data:
            chunks.append(data)
    return b"".join(chunks).decode("utf-8", errors="replace")


def normalized_command(value: str) -> str:
    return " ".join(value.strip().lower().split())


def safe_command(command: str) -> bool:
    if command in SAFE_COMMANDS:
        return True
    return re.fullmatch(r"chat inbox [0-9]+", command) is not None


def serial_command(port: Any, value: str, wait: float = 0.75) -> str:
    command = normalized_command(value)
    if any(
        command == prefix or command.startswith(prefix + " ")
        for prefix in FORBIDDEN_COMMAND_PREFIXES
    ):
        raise TestFailure(f"RF safety guard rejected command {value!r}", 2)
    if not safe_command(command):
        raise TestFailure(f"command is not in the receive-only allow-list: {value!r}", 2)

    port.write((command + "\n").encode("ascii"))
    port.flush()
    output = collect(port, wait)
    require("tx_enabled=true" not in output, "firmware reported legacy TX enabled", 4)
    require('"tx_unlocked":true' not in output, "firmware reported TX unlocked", 4)
    require('"tx_ready":true' not in output, "firmware reported TX ready", 4)
    return output


def reset_board(port: Any) -> str:
    # CP210x RTS resets the Heltec.  DTR remains inactive so GPIO0/PRG is not
    # asserted and the board returns to Kitsu rather than the ROM loader.
    port.dtr = False
    port.rts = True
    time.sleep(0.15)
    port.rts = False
    return collect(port, 5.5)


def json_records(output: str, prefix: str) -> list[JsonRecord]:
    marker = prefix + " "
    records: list[JsonRecord] = []
    for raw_line in output.splitlines():
        line = raw_line.strip()
        location = line.find(marker)
        if location < 0:
            continue
        payload = line[location + len(marker) :].strip()
        try:
            value = json.loads(payload)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            records.append(JsonRecord(value, line))
    return records


def latest_json(output: str, prefix: str) -> JsonRecord:
    records = json_records(output, prefix)
    if not records:
        raise TestFailure(f"no valid {prefix} JSON record", 5)
    return records[-1]


def validate_boot(output: str) -> None:
    boots = BOOT_PATTERN.findall(output)
    require(boots, "no KITSU_BOOT record after RTS reset", 7)
    expected = (FIRMWARE_NAME, FIRMWARE_VERSION, EXPECTED_BOARD, "false")
    require(boots[-1] == expected, f"invalid boot record: {boots[-1]!r}", 7)
    require("tx_enabled=true" not in output, "boot enabled legacy TX", 7)
    require('"tx_unlocked":true' not in output, "boot output reported unlocked TX", 7)


def validate_selftest(
    record: dict[str, Any], expected_uid: str, expected_name: str, pack: PackInfo
) -> None:
    require_exact(
        record,
        {
            "firmware": FIRMWARE_NAME,
            "version": FIRMWARE_VERSION,
            "board": EXPECTED_BOARD,
            "orientation": "portrait",
            "ui_width": 64,
            "ui_height": 128,
            "tx_enabled": False,
            "uid": expected_uid,
            "companion": expected_name,
            "pack_present": True,
            "pack_valid": True,
            "pack_error": "none",
            "pack_id": f"{pack.pack_id:08X}",
            "pack_revision": pack.revision,
            "pack_frames": pack.frame_count,
            "pack_bytes": pack.total_bytes,
            "pack_capacity": PACK_PARTITION_BYTES,
            "active_source": "pack",
            "animation_pack": True,
            "mesh_protocol": 1,
            "meshcore_version": MESHCORE_VERSION,
            "mesh_tx_unlocked": False,
            "mesh_profile": "UK/EU Narrow",
            "chat_protocol": 1,
        },
        "self-test",
    )
    require(record.get("storage") is True, "NVS storage is unavailable")
    require(record.get("chat_storage") is True, "chat storage is unavailable")
    require(record.get("oled") is True, "OLED self-test failed")
    require(record.get("button_released") is True, "PRG button is held during QA")
    require(isinstance(record.get("mesh_enabled"), bool), "mesh_enabled is not boolean")
    require(isinstance(record.get("mesh_rx_ready"), bool), "mesh_rx_ready is not boolean")
    require(isinstance(record.get("mesh_time_valid"), bool), "mesh_time_valid is not boolean")
    integer_field(record, "mesh_adverts")
    integer_field(record, "chat_contacts", 0, CONTACT_CAPACITY)
    integer_field(record, "chat_channels", 1, CHANNEL_CAPACITY)
    integer_field(record, "chat_messages", 0, INBOX_CAPACITY)
    integer_field(record, "chat_unread", 0, 255)


def validate_public_identity(record: dict[str, Any], expected_uid: str) -> str:
    require_exact(
        record,
        {
            "protocol": 1,
            "meshcore_version": MESHCORE_VERSION,
            "available": True,
            "configured": True,
            "role": "client",
            "kitsu": True,
            "uid": expected_uid,
            "marker": "fox",
            "advert_name": f"🦊 Kitsu {expected_uid}",
            "map_upload": "phone_only",
        },
        "mesh status identity",
    )
    public_key = record.get("public_key")
    require(
        isinstance(public_key, str) and PUBLIC_KEY_PATTERN.fullmatch(public_key) is not None,
        f"public_key is not 32 bytes of uppercase hex: {public_key!r}",
    )
    require(public_key[:2] not in {"00", "FF"}, "public key has a forbidden prefix")
    return public_key


def validate_mesh_status(
    record: dict[str, Any], expected_uid: str, *, expect_enabled: bool
) -> str:
    public_key = validate_public_identity(record, expected_uid)
    require_exact(record.get("profile", {}), UK_EU_NARROW, "UK/EU Narrow profile")
    require_exact(
        record,
        {
            "enabled": expect_enabled,
            "tx_unlocked": False,
            "tx_ready": False,
        },
        "mesh runtime TX gate",
    )
    expected_policy = "explicit_session" if expect_enabled else "locked"
    require(
        record.get("tx_policy") == expected_policy,
        f"unexpected TX policy: {record.get('tx_policy')!r}",
    )
    require(record.get("rx_ready") is expect_enabled, "RX readiness disagrees with config")
    require_exact(
        record.get("location", {}),
        {"mode": "hidden", "lat_e6": None, "lon_e6": None},
        "hidden location",
    )
    require(isinstance(record.get("time_valid"), bool), "time_valid is not boolean")
    integer_field(record, "epoch")
    integer_field(record, "received_adverts")
    integer_field(record, "dropped_adverts")
    integer_field(record, "queued_adverts")
    return public_key


def validate_chat_status(record: dict[str, Any]) -> dict[str, int]:
    require_exact(
        record,
        {
            "protocol": 1,
            "available": True,
            "meshcore_version": MESHCORE_VERSION,
            "contact_capacity": CONTACT_CAPACITY,
            "channel_capacity": CHANNEL_CAPACITY,
            "inbox_capacity": INBOX_CAPACITY,
            "direct_text_max_bytes": OUTBOUND_TEXT_BYTES,
            "channel_text_max_bytes": OUTBOUND_TEXT_BYTES,
            "tx_unlocked": False,
            "tx_ready": False,
        },
        "chat status",
    )
    session = record.get("session")
    require(
        isinstance(session, str) and SESSION_PATTERN.fullmatch(session) is not None,
        f"chat session is malformed: {session!r}",
    )
    require(isinstance(record.get("time_valid"), bool), "chat time_valid is not boolean")
    return {
        "contacts": integer_field(record, "contacts", 0, CONTACT_CAPACITY),
        "channels": integer_field(record, "channels", 1, CHANNEL_CAPACITY),
        "messages": integer_field(record, "messages", 0, INBOX_CAPACITY),
        # Boot-scoped aggregate: journal evictions + transient receive-event
        # drops + transient delivery-lifecycle drops.
        "dropped": integer_field(record, "dropped_messages"),
    }


def validate_result(record: dict[str, Any], action: str) -> None:
    require_exact(
        record,
        {
            "protocol": 1,
            "action": action,
            "status": "ok",
            "error": None,
        },
        f"mesh {action} result",
    )


def validate_hidden_map_rejection(record: JsonRecord) -> None:
    require_exact(
        record.value,
        {
            "protocol": 1,
            "status": "rejected",
            "error": "location_hidden",
            "uploader": "phone",
            "firmware_upload": False,
            "advert_hex": None,
            "location": None,
        },
        "hidden map publication",
    )
    require(
        re.search(r'"advert_hex"\s*:\s*"', record.line) is None,
        "hidden map rejection leaked an advert string",
    )
    require(
        not any(len(value) > 64 for value in re.findall(r"[0-9A-F]{32,}", record.line)),
        "hidden map rejection leaked a long hexadecimal payload",
    )


def validate_contacts(output: str) -> int:
    records = json_records(output, "KITSU_CONTACT")
    end = latest_json(output, "KITSU_CONTACT_END").value
    require_exact(end, {"protocol": 1}, "contact-list terminator")
    count = integer_field(end, "count", 0, CONTACT_CAPACITY)
    require(len(records) == count, f"contact list emitted {len(records)} records, says {count}")
    keys: set[str] = set()
    ids: set[str] = set()
    for expected_index, wrapped in enumerate(records):
        record = wrapped.value
        require_exact(record, {"protocol": 1, "index": expected_index}, "contact")
        contact_id = record.get("id")
        public_key = record.get("public_key")
        require(
            isinstance(contact_id, str) and CONTACT_ID_PATTERN.fullmatch(contact_id) is not None,
            f"malformed contact id: {contact_id!r}",
        )
        require(
            isinstance(public_key, str) and PUBLIC_KEY_PATTERN.fullmatch(public_key) is not None,
            f"malformed contact public key: {public_key!r}",
        )
        require(contact_id == public_key[:12], "contact id is not the six-byte key prefix")
        require(public_key not in keys, "duplicate full contact key")
        keys.add(public_key)
        ids.add(contact_id)
        name = record.get("name")
        # Explicitly provisioned names are capped at 31 bytes; signed-advert
        # discoveries may use the transport's complete 32-byte display field.
        require(isinstance(name, str) and 0 < len(name.encode("utf-8")) <= 32,
                f"invalid contact name: {name!r}")
        role = record.get("role")
        require(role in {"client", "repeater", "room", "sensor"}, f"invalid role: {role!r}")
        require(isinstance(record.get("favorite"), bool), "favorite is not boolean")
        integer_field(record, "last_advert")
        # Message and advert timestamps are independent MeshCore observations;
        # either may still be zero, and neither is required to equal the other.
        integer_field(record, "last_heard")
        require(record.get("route_hint") in {"flood", "direct"}, "invalid route hint")
        require(record.get("dm_capable") is (role == "client"), "dm_capable disagrees with role")
        require("secret" not in record and "shared_secret" not in record,
                "contact listing exposed a secret")
    require(len(keys) == count, "contact keys are not unique")
    # Six-byte display IDs can theoretically collide; the firmware resolves
    # that safely at send time.  The harness therefore does not require IDs to
    # be unique, but does validate their derivation above.
    return count


def validate_channels(output: str) -> int:
    records = json_records(output, "KITSU_CHANNEL")
    end = latest_json(output, "KITSU_CHANNEL_END").value
    require_exact(end, {"protocol": 1, "count": CHANNEL_CAPACITY}, "channel terminator")
    require(len(records) == CHANNEL_CAPACITY, "channel listing must include all four slots")
    configured = 0
    for expected_index, wrapped in enumerate(records):
        record = wrapped.value
        require_exact(
            record,
            {"protocol": 1, "index": expected_index, "public": expected_index == 0},
            "channel",
        )
        is_configured = record.get("configured")
        require(isinstance(is_configured, bool), "channel configured is not boolean")
        name = record.get("name")
        require(isinstance(name, str), "channel name is not a string")
        channel_hash = record.get("hash")
        if is_configured:
            configured += 1
            require(0 < len(name.encode("utf-8")) <= 32, "configured channel name is invalid")
            require(
                isinstance(channel_hash, str)
                and BYTE_HASH_PATTERN.fullmatch(channel_hash) is not None,
                f"configured channel hash is malformed: {channel_hash!r}",
            )
        else:
            require(name == "" and channel_hash is None, "empty channel slot is not canonical")
        require("secret" not in record and "key" not in record,
                "channel listing exposed key material")

    public = records[0].value
    require_exact(
        public,
        {
            "name": "Public",
            "configured": True,
            "public": True,
            "hash": PUBLIC_CHANNEL_HASH,
        },
        "MeshCore Public channel",
    )
    return configured


def validate_message(record: dict[str, Any], after_id: int) -> tuple[int, str]:
    require_exact(record, {"protocol": 1}, "message")
    message_id = integer_field(record, "id", 1)
    require(message_id > after_id, "inbox returned an entry at/before its cursor")
    direction = record.get("direction")
    kind = record.get("kind")
    require(direction in {"in", "out"}, f"invalid message direction: {direction!r}")
    require(kind in {"direct", "channel"}, f"invalid message kind: {kind!r}")
    contact = record.get("contact")
    channel = record.get("channel")
    if kind == "direct":
        require(
            isinstance(contact, str) and CONTACT_ID_PATTERN.fullmatch(contact) is not None,
            f"direct contact is malformed: {contact!r}",
        )
        require(channel is None, "direct message has a channel")
    else:
        require(contact is None, "channel message has a contact")
        require(
            not isinstance(channel, bool)
            and isinstance(channel, int)
            and 0 <= channel < CHANNEL_CAPACITY,
            f"channel slot is invalid: {channel!r}",
        )
    sender = record.get("sender")
    require(sender is None or isinstance(sender, str), "sender must be string or null")
    if isinstance(sender, str):
        require(len(sender.encode("utf-8")) <= 32, "sender exceeds receive display bound")
    if direction == "out":
        require(sender is None, "outbound message sender must be null")
    elif kind == "direct":
        require(isinstance(sender, str) and bool(sender),
                "inbound direct message requires its stored contact name")
    integer_field(record, "timestamp")
    text = record.get("text")
    require(isinstance(text, str), "message text is not a string")
    require(0 < len(text.encode("utf-8")) <= 160,
            "message is empty or exceeds MeshCore inbound text bound")
    state = record.get("state")
    route = record.get("route")
    require(route in {"flood", "direct"}, f"invalid route: {route!r}")
    if direction == "in":
        require(state == "received", f"inbound message has state {state!r}")
        number_field(record, "snr_db")
    else:
        require(record.get("snr_db") is None, "outbound message has receive SNR")
        allowed = {"queued", "sent", "failed", "cancelled"}
        if kind == "direct":
            allowed |= {"delivered", "unconfirmed"}
        require(state in allowed, f"invalid outbound {kind} state: {state!r}")
    authenticated = record.get("authenticated")
    require(authenticated is (kind == "direct"), "message authentication semantic mismatch")
    require(not (kind == "channel" and state in {"delivered", "unconfirmed"}),
            "channel message was falsely given recipient-delivery state")
    return message_id, str(kind)


def validate_inbox(output: str, after_id: int) -> tuple[int, list[dict[str, Any]]]:
    records = json_records(output, "KITSU_MESSAGE")
    end = latest_json(output, "KITSU_MESSAGE_END").value
    require_exact(end, {"protocol": 1}, "inbox terminator")
    count = integer_field(end, "count", 0, INBOX_CAPACITY)
    require(len(records) == count, f"inbox emitted {len(records)} records, says {count}")
    newest = integer_field(end, "newest_id")
    # Same aggregate definition as KITSU_CHAT.dropped_messages.
    integer_field(end, "dropped")
    session = end.get("session")
    require(
        isinstance(session, str) and SESSION_PATTERN.fullmatch(session) is not None,
        f"inbox session is malformed: {session!r}",
    )
    previous_id = after_id
    values: list[dict[str, Any]] = []
    for wrapped in records:
        message_id, _ = validate_message(wrapped.value, after_id)
        require(message_id > previous_id, "inbox IDs are not strictly increasing")
        previous_id = message_id
        values.append(wrapped.value)
    if records:
        require(newest >= previous_id, "inbox newest_id precedes emitted messages")
    return newest, values


def validate_advert(record: dict[str, Any]) -> AdvertObservation:
    kind = record.get("type")
    require(
        kind in {"client", "repeater", "room", "sensor", "unknown"},
        f"invalid advert type: {kind!r}",
    )
    name = record.get("name")
    require(isinstance(name, str), f"advert name is not a string: {name!r}")
    require(isinstance(record.get("kitsu_named"), bool), "kitsu_named is not boolean")
    prefix = record.get("public_prefix")
    require(
        isinstance(prefix, str) and PUBLIC_PREFIX_PATTERN.fullmatch(prefix) is not None,
        f"advert key prefix is malformed: {prefix!r}",
    )
    integer_field(record, "timestamp")
    has_location = record.get("has_location")
    require(isinstance(has_location, bool), "advert has_location is not boolean")
    latitude = integer_field(record, "lat_e6", -90_000_000, 90_000_000)
    longitude = integer_field(record, "lon_e6", -180_000_000, 180_000_000)
    if not has_location:
        require((latitude, longitude) == (0, 0), "hidden advert exposed coordinates")
    number_field(record, "rssi")
    number_field(record, "snr")
    return AdvertObservation(str(kind), name, str(prefix))


def observations(outputs: Iterable[str]) -> list[AdvertObservation]:
    result: list[AdvertObservation] = []
    for output in outputs:
        for record in json_records(output, "KITSU_MESH_ADVERT"):
            result.append(validate_advert(record.value))
    return result


def validate_required_adverts(observed: list[AdvertObservation], required: list[str]) -> None:
    kinds = {item.kind for item in observed}
    for requirement in set(required):
        if requirement == "any":
            require(bool(kinds & {"client", "repeater"}),
                    "passive window saw no signed client/repeater advert", 9)
        else:
            require(requirement in kinds,
                    f"passive window saw no signed {requirement} advert", 9)


def passive_message_ids(output: str) -> set[int]:
    result: set[int] = set()
    for wrapped in json_records(output, "KITSU_CHAT_EVENT"):
        record = wrapped.value
        require_exact(record, {"protocol": 1}, "chat event")
        message_id = integer_field(record, "message_id", 1)
        event = record.get("event")
        require(event in {"message", "tx", "delivery"}, f"invalid chat event: {event!r}")
        if event == "message":
            require(record.get("state") is None, "message event unexpectedly has state")
            require(record.get("round_trip_ms") is None,
                    "message event unexpectedly has round-trip time")
            result.add(message_id)
    return result


def validate_required_messages(
    passive_ids: set[int], messages: list[dict[str, Any]], required: list[str]
) -> None:
    observed = {
        str(record["kind"])
        for record in messages
        if record.get("direction") == "in" and record.get("id") in passive_ids
    }
    for requirement in set(required):
        if requirement == "any":
            require(bool(observed), "passive window saw no inbound chat message", 10)
        else:
            require(requirement in observed,
                    f"passive window saw no inbound {requirement} message", 10)


def query_contract(port: Any, transcript: list[str]) -> tuple[dict[str, int], int]:
    status_output = serial_command(port, "chat status")
    transcript.append(status_output)
    counts = validate_chat_status(latest_json(status_output, "KITSU_CHAT").value)

    contacts_output = serial_command(port, "chat contacts")
    transcript.append(contacts_output)
    validate_contacts(contacts_output)

    channels_output = serial_command(port, "chat channels")
    transcript.append(channels_output)
    require(validate_channels(channels_output) == counts["channels"],
            "chat status channel count disagrees with listing")

    inbox_output = serial_command(port, "chat inbox")
    transcript.append(inbox_output)
    newest, messages = validate_inbox(inbox_output, 0)
    # Signed adverts and messages can arrive between independent queries, so
    # contact/inbox snapshots need not equal the earlier status counters.
    # Channel configuration cannot change from passive RF and is compared.
    return counts, newest


def run_hardware_test(args: argparse.Namespace, serial_module: Any) -> tuple[str, str]:
    transcript: list[str] = []

    with serial_module.Serial(
        args.port,
        args.baud,
        timeout=0.1,
        dsrdtr=False,
        rtscts=False,
    ) as port:
        port.dtr = False
        port.rts = False

        # Reset before the first command.  This revokes any session gate that
        # may have been open before the harness attached.
        first_boot = reset_board(port)
        transcript.append(first_boot)
        validate_boot(first_boot)

        selftest_output = serial_command(port, "selftest")
        transcript.append(selftest_output)
        validate_selftest(
            latest_json(selftest_output, "KITSU_SELFTEST").value,
            args.expected_uid,
            args.expected_name,
            args.pack_info,
        )

        initial_mesh_output = serial_command(port, "mesh status")
        transcript.append(initial_mesh_output)
        initial_mesh = latest_json(initial_mesh_output, "KITSU_MESH").value
        initial_key = validate_public_identity(initial_mesh, args.expected_uid)
        require(initial_mesh.get("tx_unlocked") is False, "TX gate open after first boot")
        require(initial_mesh.get("tx_ready") is False, "TX ready after first boot")

        expected_enabled = bool(initial_mesh.get("enabled"))
        if args.enable_rx:
            config_output = serial_command(port, "mesh config on", 1.0)
            transcript.append(config_output)
            validate_result(latest_json(config_output, "KITSU_MESH_RESULT").value, "config")
            expected_enabled = True

        hidden_output = serial_command(port, "mesh location hidden")
        transcript.append(hidden_output)
        validate_result(
            latest_json(hidden_output, "KITSU_MESH_RESULT").value,
            "location_hidden",
        )

        before_reset_output = serial_command(port, "mesh status")
        transcript.append(before_reset_output)
        before_reset = latest_json(before_reset_output, "KITSU_MESH").value
        before_key = validate_mesh_status(
            before_reset, args.expected_uid, expect_enabled=expected_enabled
        )
        require(before_key == initial_key, "identity changed while selecting passive RX")
        require(integer_field(before_reset, "queued_adverts") == 0,
                "outbound MeshCore queue is non-zero before reboot")

        query_contract(port, transcript)

        map_output = serial_command(port, "mesh publish-map")
        transcript.append(map_output)
        map_records = json_records(map_output, "KITSU_MAP_PUBLISH")
        require(len(map_records) == 1, f"expected one map response, got {len(map_records)}")
        validate_hidden_map_rejection(map_records[0])

        # Second boot proves the persisted receiver/profile remains useful but
        # the volatile TX grant and clock do not survive reset.
        second_boot = reset_board(port)
        transcript.append(second_boot)
        validate_boot(second_boot)

        reboot_mesh_output = serial_command(port, "mesh status", 0.9)
        transcript.append(reboot_mesh_output)
        reboot_mesh = latest_json(reboot_mesh_output, "KITSU_MESH").value
        reboot_key = validate_mesh_status(
            reboot_mesh, args.expected_uid, expect_enabled=expected_enabled
        )
        require(reboot_key == before_key, "MeshCore identity did not persist across reboot")
        require(reboot_mesh.get("time_valid") is False, "session clock survived reboot")
        require(integer_field(reboot_mesh, "queued_adverts") == 0,
                "outbound MeshCore queue is non-zero after reboot")

        _, baseline_newest = query_contract(port, transcript)

        map_after_reset = serial_command(port, "mesh publish-map")
        transcript.append(map_after_reset)
        map_after_records = json_records(map_after_reset, "KITSU_MAP_PUBLISH")
        require(len(map_after_records) == 1,
                f"expected one post-reboot map response, got {len(map_after_records)}")
        validate_hidden_map_rejection(map_after_records[0])

        passive_output = ""
        if args.observe_seconds > 0:
            # No serial write occurs in this window.  Kitsu remains locked and
            # background MeshCore receive is already active when requested.
            passive_output = collect(port, args.observe_seconds)
            transcript.append(passive_output)

        observed_adverts = observations([passive_output])
        validate_required_adverts(observed_adverts, args.require_advert)
        event_message_ids = passive_message_ids(passive_output)

        new_inbox_output = serial_command(port, f"chat inbox {baseline_newest}")
        transcript.append(new_inbox_output)
        _, new_messages = validate_inbox(new_inbox_output, baseline_newest)
        validate_required_messages(event_message_ids, new_messages, args.require_message)

        final_mesh_output = serial_command(port, "mesh status", 0.9)
        transcript.append(final_mesh_output)
        final_mesh = latest_json(final_mesh_output, "KITSU_MESH").value
        final_key = validate_mesh_status(
            final_mesh, args.expected_uid, expect_enabled=expected_enabled
        )
        require(final_key == before_key, "identity changed during passive observation")
        require(integer_field(final_mesh, "queued_adverts") == 0,
                "passive QA caused an outbound MeshCore packet")

        final_chat_output = serial_command(port, "chat status")
        transcript.append(final_chat_output)
        final_chat = latest_json(final_chat_output, "KITSU_CHAT").value
        validate_chat_status(final_chat)
        require(final_chat.get("tx_unlocked") is False, "chat TX gate opened")
        require(final_chat.get("tx_ready") is False, "chat TX became ready")

        advert_counts = {
            "client": sum(item.kind == "client" for item in observed_adverts),
            "repeater": sum(item.kind == "repeater" for item in observed_adverts),
        }
        message_counts = {
            "channel": sum(
                record.get("direction") == "in" and record.get("kind") == "channel"
                for record in new_messages
            ),
            "direct": sum(
                record.get("direction") == "in" and record.get("kind") == "direct"
                for record in new_messages
            ),
        }
        summary = (
            f"uid={args.expected_uid} public_key={before_key[:8]}... "
            "profile=UK/EU_Narrow/SF8/BW62.5/preamble32 "
            f"rx={'on' if expected_enabled else 'off'} tx=locked "
            f"observed_client={advert_counts['client']} "
            f"observed_repeater={advert_counts['repeater']} "
            f"observed_channel={message_counts['channel']} "
            f"observed_direct={message_counts['direct']}"
        )
        return "".join(transcript), summary


def validate_arguments(args: argparse.Namespace) -> None:
    args.expected_uid = args.expected_uid.upper()
    require(
        re.fullmatch(r"KT[0-9A-F]{4}", args.expected_uid) is not None,
        "--expected-uid must look like KTDEAD",
        2,
    )
    require(args.baud > 0, "--baud must be positive", 2)
    require(
        math.isfinite(args.observe_seconds) and 0 <= args.observe_seconds <= 3600,
        "--observe-seconds must be between 0 and 3600",
        2,
    )
    require(
        not (args.require_advert or args.require_message) or args.observe_seconds > 0,
        "passive requirements need --observe-seconds greater than zero",
        2,
    )
    require(
        not (args.require_advert or args.require_message) or args.enable_rx,
        "passive requirements need --enable-rx",
        2,
    )
    try:
        args.pack_info = validate_pack(args.expected_pack)
    except PackValidationError as error:
        raise TestFailure(f"invalid --expected-pack: {error}", 2) from error
    require(
        args.pack_info.display_name == args.expected_name,
        "--expected-name does not match --expected-pack metadata",
        2,
    )


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")

    args = parse_args(argv)
    try:
        validate_arguments(args)
    except TestFailure as error:
        print(f"TEST_FAIL {error}", file=sys.stderr)
        return error.code

    # Import only after argument validation: importing this module or running
    # --help cannot enumerate or open a serial port.
    try:
        import serial
    except ImportError as error:
        print(f"TEST_FAIL pyserial is required for hardware QA: {error}", file=sys.stderr)
        return 2

    try:
        transcript, summary = run_hardware_test(args, serial)
    except TestFailure as error:
        print(f"TEST_FAIL {error}", file=sys.stderr)
        return error.code
    except serial.SerialException as error:
        print(f"TEST_FAIL serial error on {args.port}: {error}", file=sys.stderr)
        return 8

    print(transcript, end="" if transcript.endswith("\n") else "\n")
    print(
        f"TEST_PASS Kitsu868 v{FIRMWARE_VERSION} receive-only MeshCore QA; "
        f"{summary}; Public=11; capacities=12/4/24; outbound=128/128; "
        "identity persisted; hidden map rejected; no unlock, introduction, or send issued"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
