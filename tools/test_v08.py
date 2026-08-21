"""RF-safe hardware QA for the Kitsu868 0.8.0 MeshCore slice.

The test talks only over USB serial.  It does not flash firmware, erase NVS,
write a companion pack, set a clock, export a visible-location map card, or
send a LoRa packet.  In particular, its command allow-list cannot issue a
MeshCore transmit-gate unlock or either introduction command.

By default the test selects the built-in UK/EU Narrow profile so the receiver
is useful after QA.  Selecting that profile starts passive reception but does
not open Kitsu's non-persistent transmit gate.  Use ``--no-enable-rx`` to
preserve and validate the adapter's current enabled/disabled state.

``KITSU_MESH_ADVERT`` records are emitted by firmware only after MeshCore has
accepted a signed advertisement.  The optional passive observation validates
those records, but deliberately does not claim to be a second cryptographic
verifier: the serial event contains a public-key prefix, not the wire packet's
signature.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
import time
from dataclasses import dataclass
from typing import Any, Iterable


FIRMWARE_NAME = "Kitsu868"
FIRMWARE_VERSION = "0.8.0"
MESHCORE_VERSION = "1.17.1"
EXPECTED_BOARD = "heltec-v3.2"

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
PUBLIC_PREFIX_PATTERN = re.compile(r"[0-9A-F]{8}")

# This is intentionally an allow-list, not merely a block-list.  Adding a
# serial action to the QA therefore requires an explicit safety review.
SAFE_COMMANDS = {
    "selftest",
    "mesh status",
    "mesh config on",
    "mesh location hidden",
    "mesh publish-map",
}
FORBIDDEN_COMMAND_PREFIXES = (
    "mesh tx",
    "mesh introduce",
    "listen",
    "send",
    "transmit",
    "tx",
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
    rssi: float
    snr: float


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run RF-safe Kitsu868 0.8.0/MeshCore hardware QA over USB. "
            "No command in this test can unlock TX or introduce the node."
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
        "--enable-rx",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "select/persist UK/EU Narrow and enable passive MeshCore RX "
            "without unlocking TX (default: enabled)"
        ),
    )
    parser.add_argument(
        "--observe-seconds",
        type=float,
        default=0.0,
        metavar="SECONDS",
        help=(
            "passively read signed MeshCore advert events after reset; zero "
            "skips the observation window (default: 0)"
        ),
    )
    parser.add_argument(
        "--require-advert",
        action="append",
        choices=("any", "client", "repeater"),
        default=[],
        metavar="TYPE",
        help=(
            "require TYPE during the passive window; repeat for both client "
            "and repeater (requires --observe-seconds > 0)"
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


def serial_command(port: Any, value: str, wait: float = 0.75) -> str:
    command = normalized_command(value)
    if any(
        command == prefix or command.startswith(prefix + " ")
        for prefix in FORBIDDEN_COMMAND_PREFIXES
    ):
        raise TestFailure(f"RF safety guard rejected command {value!r}", 2)
    if command not in SAFE_COMMANDS:
        raise TestFailure(f"serial command is not in the RF-safe allow-list: {value!r}", 2)

    port.write((command + "\n").encode("ascii"))
    port.flush()
    output = collect(port, wait)
    require("tx_enabled=true" not in output, "firmware reported legacy RF TX enabled", 4)
    return output


def reset_board(port: Any) -> str:
    # CP210x RTS resets the Heltec.  DTR remains inactive so PRG/GPIO0 is not
    # asserted and the board returns to the application, not the ROM loader.
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


def validate_selftest(record: dict[str, Any], expected_uid: str) -> None:
    require_exact(
        record,
        {
            "firmware": FIRMWARE_NAME,
            "version": FIRMWARE_VERSION,
            "board": EXPECTED_BOARD,
            "tx_enabled": False,
            "uid": expected_uid,
            "mesh_protocol": 1,
            "meshcore_version": MESHCORE_VERSION,
            "mesh_tx_unlocked": False,
            "mesh_profile": "UK/EU Narrow",
        },
        "self-test",
    )
    require(record.get("storage") is True, "NVS storage is unavailable")
    require(record.get("oled") is True, "OLED self-test failed")
    require(record.get("button_released") is True, "PRG button is held during QA")
    require(isinstance(record.get("mesh_enabled"), bool), "mesh_enabled is not boolean")
    require(isinstance(record.get("mesh_rx_ready"), bool), "mesh_rx_ready is not boolean")
    require(isinstance(record.get("mesh_time_valid"), bool), "mesh_time_valid is not boolean")
    integer_field(record, "mesh_adverts")


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
        f"unexpected TX policy: {record.get('tx_policy')!r} != {expected_policy!r}",
    )
    if expect_enabled:
        require(record.get("rx_ready") is True, "UK/EU Narrow receiver is not ready")
    else:
        require(record.get("rx_ready") is False, "disabled adapter unexpectedly has RX ready")

    require_exact(
        record.get("location", {}),
        {"mode": "hidden", "lat_e6": None, "lon_e6": None},
        "hidden location",
    )
    require(isinstance(record.get("time_valid"), bool), "time_valid is not boolean")
    integer_field(record, "epoch")
    integer_field(record, "received_adverts")
    integer_field(record, "queued_adverts")
    return public_key


def validate_result(record: dict[str, Any], action: str) -> None:
    require_exact(
        record,
        {"protocol": 1, "action": action, "status": "ok", "error": None},
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


def number_field(record: dict[str, Any], key: str) -> float:
    value = record.get(key)
    require(
        not isinstance(value, bool) and isinstance(value, (int, float)),
        f"advert field {key!r} is not numeric: {value!r}",
    )
    result = float(value)
    require(math.isfinite(result), f"advert field {key!r} is not finite")
    return result


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
        f"advert public-key prefix is malformed: {prefix!r}",
    )
    integer_field(record, "timestamp")
    has_location = record.get("has_location")
    require(isinstance(has_location, bool), "advert has_location is not boolean")
    latitude = integer_field(record, "lat_e6", -90_000_000, 90_000_000)
    longitude = integer_field(record, "lon_e6", -180_000_000, 180_000_000)
    if not has_location:
        require((latitude, longitude) == (0, 0), "location-hidden advert exposed coordinates")
    rssi = number_field(record, "rssi")
    snr = number_field(record, "snr")
    return AdvertObservation(str(kind), name, prefix, rssi, snr)


def observations(outputs: Iterable[str]) -> list[AdvertObservation]:
    result: list[AdvertObservation] = []
    for output in outputs:
        for record in json_records(output, "KITSU_MESH_ADVERT"):
            result.append(validate_advert(record.value))
    return result


def validate_required_adverts(
    observed: list[AdvertObservation], required: list[str]
) -> None:
    kinds = {item.kind for item in observed}
    for requirement in set(required):
        if requirement == "any":
            require(
                bool(kinds & {"client", "repeater"}),
                "passive window saw no signed client/repeater advertisement",
                9,
            )
        else:
            require(
                requirement in kinds,
                f"passive window saw no signed {requirement} advertisement",
                9,
            )


def run_hardware_test(args: argparse.Namespace, serial_module: Any) -> tuple[str, str]:
    transcript: list[str] = []
    passive_outputs: list[str] = []

    with serial_module.Serial(
        args.port,
        args.baud,
        timeout=0.1,
        dsrdtr=False,
        rtscts=False,
    ) as port:
        port.dtr = False
        port.rts = False
        transcript.append(collect(port, 0.8))

        selftest_output = serial_command(port, "selftest")
        transcript.append(selftest_output)
        validate_selftest(latest_json(selftest_output, "KITSU_SELFTEST").value, args.expected_uid)

        initial_status_output = serial_command(port, "mesh status")
        transcript.append(initial_status_output)
        initial_status = latest_json(initial_status_output, "KITSU_MESH").value
        initial_key = validate_public_identity(initial_status, args.expected_uid)
        require_exact(initial_status.get("profile", {}), UK_EU_NARROW, "initial profile")
        require(initial_status.get("tx_unlocked") is False, "TX gate was open before QA")
        require(initial_status.get("tx_ready") is False, "TX was ready before QA")

        expected_enabled = bool(initial_status.get("enabled"))
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

        status_output = serial_command(port, "mesh status")
        transcript.append(status_output)
        before_reset = latest_json(status_output, "KITSU_MESH").value
        before_key = validate_mesh_status(
            before_reset, args.expected_uid, expect_enabled=expected_enabled
        )
        require(before_key == initial_key, "public identity changed while selecting RX profile")
        queued_before = integer_field(before_reset, "queued_adverts")

        map_output = serial_command(port, "mesh publish-map")
        transcript.append(map_output)
        map_records = json_records(map_output, "KITSU_MAP_PUBLISH")
        require(len(map_records) == 1, f"expected one map response, got {len(map_records)}")
        validate_hidden_map_rejection(map_records[0])

        reset_output = reset_board(port)
        transcript.append(reset_output)
        boot_records = BOOT_PATTERN.findall(reset_output)
        require(boot_records, "no KITSU_BOOT record after RTS reset", 7)
        require(
            boot_records[-1]
            == (FIRMWARE_NAME, FIRMWARE_VERSION, EXPECTED_BOARD, "false"),
            f"invalid boot record: {boot_records[-1]!r}",
            7,
        )
        require("tx_enabled=true" not in reset_output, "reset enabled legacy RF TX", 7)

        reboot_status_output = serial_command(port, "mesh status", 0.9)
        transcript.append(reboot_status_output)
        after_reset = latest_json(reboot_status_output, "KITSU_MESH").value
        after_key = validate_mesh_status(
            after_reset, args.expected_uid, expect_enabled=expected_enabled
        )
        require(after_key == before_key, "MeshCore public identity did not persist across reset")
        require(
            integer_field(after_reset, "queued_adverts") == 0,
            "outbound advert queue was non-zero after reset",
        )

        map_after_reset_output = serial_command(port, "mesh publish-map")
        transcript.append(map_after_reset_output)
        map_after_reset_records = json_records(map_after_reset_output, "KITSU_MAP_PUBLISH")
        require(
            len(map_after_reset_records) == 1,
            f"expected one post-reset map response, got {len(map_after_reset_records)}",
        )
        validate_hidden_map_rejection(map_after_reset_records[0])

        # No serial write occurs in this window.  Background MeshCore RX is
        # already active after boot when --enable-rx is used.
        if args.observe_seconds > 0:
            passive_output = collect(port, args.observe_seconds)
            transcript.append(passive_output)
            passive_outputs.append(passive_output)

        observed = observations(passive_outputs)
        validate_required_adverts(observed, args.require_advert)

        final_status_output = serial_command(port, "mesh status", 0.9)
        transcript.append(final_status_output)
        final_status = latest_json(final_status_output, "KITSU_MESH").value
        final_key = validate_mesh_status(
            final_status, args.expected_uid, expect_enabled=expected_enabled
        )
        require(final_key == before_key, "public identity changed during passive observation")
        require(
            integer_field(final_status, "queued_adverts") == 0,
            "QA queued an outbound MeshCore advert",
        )
        require(queued_before == 0, "outbound advert queue was non-zero before reset")

        counts = {
            "client": sum(item.kind == "client" for item in observed),
            "repeater": sum(item.kind == "repeater" for item in observed),
        }
        summary = (
            f"uid={args.expected_uid} public_key={before_key[:8]}... "
            f"profile=UK/EU_Narrow/SF8/BW62.5/preamble32 "
            f"rx={'on' if expected_enabled else 'off'} tx=locked "
            f"observed_client={counts['client']} observed_repeater={counts['repeater']}"
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
        not args.require_advert or args.observe_seconds > 0,
        "--require-advert needs --observe-seconds greater than zero",
        2,
    )
    require(
        not args.require_advert or args.enable_rx,
        "--require-advert needs passive RX enabled",
        2,
    )


def main(argv: list[str] | None = None) -> int:
    # Windows terminals may inherit a legacy CP-1252 codec even though the
    # firmware contract intentionally contains the fox emoji.  Keep the
    # transcript machine-readable instead of failing after all QA checks ran.
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

    # Importing pyserial only after argument parsing keeps --help completely
    # offline; importing this module also cannot touch COM3.
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
        "TEST_PASS Kitsu868 v0.8.0 RF-safe MeshCore QA; "
        f"{summary}; identity persisted; hidden map export rejected; "
        "no TX unlock or introduction command issued"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
