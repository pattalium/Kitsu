"""One-shot active MeshCore TX QA for Kitsu868 v0.9.0.

This is deliberately separate from the receive-only ``test_v09.py`` harness.
It sends exactly one Public-channel message and, only when requested, one
nearby Client advert.  It never retries.  The board is reset first, the exact
UK/EU Narrow profile is selected, location is forced hidden, and the volatile
TX gate is locked again from ``finally`` on every exit path.

Running ``--help`` or omitting ``--i-understand-rf-tx`` never imports pyserial,
enumerates ports, opens COM, or transmits.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
import time
from dataclasses import dataclass
from typing import Any


FIRMWARE_VERSION = "0.9.0"
MESHCORE_VERSION = "1.17.1"
MAX_TEXT_BYTES = 128
MAX_COMMAND_BYTES = 224
PINNED_PROFILE = {
    "name": "UK/EU Narrow",
    "frequency_hz": 869_618_000,
    "bandwidth_hz": 62_500,
    "spreading_factor": 8,
    "coding_rate": 5,
    "sync_word": 0x12,
    "preamble_symbols": 32,
    "tx_power_dbm": 22,
}


class TestFailure(RuntimeError):
    def __init__(self, message: str, code: int = 3) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class JsonRecord:
    value: dict[str, Any]
    line: str


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Send exactly one Kitsu Public message at the pinned UK/EU Narrow "
            "22 dBm setting, then lock TX. No retries are performed."
        )
    )
    parser.add_argument("--port", default="COM3", help="Heltec serial port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--expected-uid",
        required=True,
        help="exact six-character UID read from the device (format example: KTDEAD)",
    )
    parser.add_argument(
        "--message", required=True, help="one Public message (1..128 UTF-8 bytes)"
    )
    parser.add_argument(
        "--advert",
        action="store_true",
        help="also send exactly one nearby standard MeshCore Client advert",
    )
    parser.add_argument(
        "--settle-seconds",
        type=float,
        default=5.0,
        help="listen after the physical sent event before locking (default: 5)",
    )
    parser.add_argument(
        "--i-understand-rf-tx",
        action="store_true",
        help="required acknowledgement that this test intentionally emits RF",
    )
    return parser.parse_args(argv)


def require(condition: bool, message: str, code: int = 6) -> None:
    if not condition:
        raise TestFailure(message, code)


def validate_args(args: argparse.Namespace) -> None:
    require(args.i_understand_rf_tx, "refusing RF TX without --i-understand-rf-tx", 2)
    args.expected_uid = args.expected_uid.upper()
    require(
        re.fullmatch(r"KT[0-9A-F]{4}", args.expected_uid) is not None,
        "--expected-uid must look like KTDEAD",
        2,
    )
    require(args.baud > 0, "--baud must be positive", 2)
    require(
        math.isfinite(args.settle_seconds) and 0 <= args.settle_seconds <= 30,
        "--settle-seconds must be between 0 and 30",
        2,
    )
    encoded = args.message.encode("utf-8")
    require(1 <= len(encoded) <= MAX_TEXT_BYTES,
            "--message must contain 1..128 UTF-8 bytes", 2)
    require(
        not any(ord(character) < 0x20 or ord(character) == 0x7F
                for character in args.message),
        "--message cannot contain control characters",
        2,
    )
    require(
        len(b"chat send ch 0 " + encoded + b"\n") <= MAX_COMMAND_BYTES,
        "encoded serial command exceeds firmware framing limit",
        2,
    )


def collect(port: Any, seconds: float) -> str:
    deadline = time.monotonic() + seconds
    chunks: list[bytes] = []
    while time.monotonic() < deadline:
        data = port.read(port.in_waiting or 1)
        if data:
            chunks.append(data)
    return b"".join(chunks).decode("utf-8", errors="replace")


def reset_board(port: Any) -> str:
    port.dtr = False
    port.rts = True
    time.sleep(0.15)
    port.rts = False
    return collect(port, 5.5)


def command(port: Any, value: str, wait: float = 0.8) -> str:
    payload = (value + "\n").encode("utf-8")
    require(len(payload) <= MAX_COMMAND_BYTES, "command exceeds serial limit", 2)
    port.write(payload)
    port.flush()
    return collect(port, wait)


def json_records(output: str, prefix: str) -> list[JsonRecord]:
    marker = prefix + " "
    records: list[JsonRecord] = []
    for raw_line in output.splitlines():
        line = raw_line.strip()
        location = line.find(marker)
        if location < 0:
            continue
        try:
            value = json.loads(line[location + len(marker):].strip())
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            records.append(JsonRecord(value, line))
    return records


def latest_json(output: str, prefix: str) -> dict[str, Any]:
    records = json_records(output, prefix)
    require(bool(records), f"no valid {prefix} record")
    return records[-1].value


def expect_result(output: str, prefix: str, action: str, status: str) -> dict[str, Any]:
    record = latest_json(output, prefix)
    require(record.get("protocol") == 1, f"invalid {prefix} protocol")
    require(record.get("action") == action,
            f"unexpected {prefix} action: {record.get('action')!r}")
    require(record.get("status") == status,
            f"{action} was not {status}: {record!r}")
    require(record.get("error") is None, f"{action} returned an error: {record!r}")
    return record


def validate_status(record: dict[str, Any], uid: str, unlocked: bool) -> None:
    require(record.get("meshcore_version") == MESHCORE_VERSION,
            "wrong MeshCore version")
    require(record.get("uid") == uid, "wrong Kitsu UID")
    require(record.get("role") == "client", "Kitsu is not a MeshCore Client")
    require(record.get("enabled") is True, "MeshCore is disabled")
    require(record.get("location", {}).get("mode") == "hidden",
            "location is not hidden")
    profile = record.get("profile", {})
    for key, expected in PINNED_PROFILE.items():
        require(profile.get(key) == expected,
                f"profile {key} is {profile.get(key)!r}, expected {expected!r}")
    require(record.get("tx_unlocked") is unlocked,
            f"TX unlocked state is not {unlocked}")
    require(record.get("tx_ready") is unlocked,
            f"TX ready state is not {unlocked}")
    if unlocked:
        require(record.get("time_valid") is True, "MeshCore clock is not valid")


def wait_for_tx_event(
    port: Any, message_id: int, initial: str = "", timeout: float = 15.0
) -> str:
    deadline = time.monotonic() + timeout
    output: list[str] = [initial]
    while time.monotonic() < deadline:
        for record in json_records("".join(output), "KITSU_CHAT_EVENT"):
            value = record.value
            if value.get("message_id") != message_id:
                continue
            if value.get("event") == "tx" and value.get("state") == "sent":
                return "".join(output)
            if value.get("event") == "tx" and value.get("state") in {
                "failed", "cancelled"
            }:
                raise TestFailure(f"physical TX failed: {value!r}")
        chunk = collect(port, min(0.5, max(0.0, deadline - time.monotonic())))
        if chunk:
            output.append(chunk)
    raise TestFailure(f"no physical sent event for message {message_id}")


def run(args: argparse.Namespace, serial_module: Any) -> tuple[str, int]:
    transcript: list[str] = []
    message_id = 0
    with serial_module.Serial(
        args.port, args.baud, timeout=0.1, dsrdtr=False, rtscts=False
    ) as port:
        port.dtr = False
        port.rts = False
        boot = reset_board(port)
        transcript.append(boot)
        require(f"version={FIRMWARE_VERSION}" in boot, "wrong or missing firmware boot")
        require("tx_enabled=true" not in boot, "legacy TX was enabled at boot")

        lock_error: Exception | None = None
        try:
            initial_lock = command(port, "mesh tx lock")
            transcript.append(initial_lock)
            expect_result(initial_lock, "KITSU_MESH_RESULT", "tx_lock", "ok")

            config = command(port, "mesh config on", 1.2)
            transcript.append(config)
            expect_result(config, "KITSU_MESH_RESULT", "config", "ok")

            hidden = command(port, "mesh location hidden")
            transcript.append(hidden)
            expect_result(hidden, "KITSU_MESH_RESULT", "location_hidden", "ok")

            before = command(port, "mesh status")
            transcript.append(before)
            validate_status(latest_json(before, "KITSU_MESH"), args.expected_uid, False)

            epoch = int(time.time())
            time_result = command(port, f"mesh time {epoch}")
            transcript.append(time_result)
            expect_result(time_result, "KITSU_MESH_RESULT", "time", "ok")

            unlock = command(port, "mesh tx unlock")
            transcript.append(unlock)
            expect_result(unlock, "KITSU_MESH_RESULT", "tx_unlock", "ok")

            armed = command(port, "mesh status")
            transcript.append(armed)
            validate_status(latest_json(armed, "KITSU_MESH"), args.expected_uid, True)

            if args.advert:
                advert = command(port, "mesh introduce nearby", 1.0)
                transcript.append(advert)
                expect_result(
                    advert, "KITSU_MESH_RESULT", "introduce_nearby", "queued"
                )
                # Keep the optional advert and the single application message
                # separated without imposing this delay on protocol ACKs.
                transcript.append(collect(port, 5.0))

            sent = command(port, f"chat send ch 0 {args.message}", 0.5)
            result = expect_result(sent, "KITSU_CHAT_RESULT", "send", "queued")
            raw_id = result.get("message_id")
            require(isinstance(raw_id, int) and raw_id > 0,
                    f"invalid queued message id: {raw_id!r}")
            message_id = raw_id
            # ``sent`` may arrive in the same serial collection as ``queued``;
            # include that first chunk so a fast radio completion is not lost.
            transcript.append(wait_for_tx_event(port, message_id, sent))
            if args.settle_seconds:
                transcript.append(collect(port, args.settle_seconds))
        finally:
            try:
                locked = command(port, "mesh tx lock", 1.0)
                transcript.append(locked)
                expect_result(locked, "KITSU_MESH_RESULT", "tx_lock", "ok")
                status_output = command(port, "mesh status", 1.0)
                transcript.append(status_output)
                status = latest_json(status_output, "KITSU_MESH")
                validate_status(status, args.expected_uid, False)
                require(status.get("queued_adverts") == 0,
                        "outbound queue is not empty after locking")
            except Exception as error:  # preserve the original failure, if any
                lock_error = error
            if lock_error is not None and sys.exc_info()[0] is None:
                raise lock_error
    return "".join(transcript), message_id


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")
    args = parse_args(argv)
    try:
        validate_args(args)
    except TestFailure as error:
        print(f"TEST_FAIL {error}", file=sys.stderr)
        return error.code

    try:
        import serial
    except ImportError as error:
        print(f"TEST_FAIL pyserial is required: {error}", file=sys.stderr)
        return 2

    try:
        transcript, message_id = run(args, serial)
    except TestFailure as error:
        print(f"TEST_FAIL {error}", file=sys.stderr)
        return error.code
    except serial.SerialException as error:
        print(f"TEST_FAIL serial error on {args.port}: {error}", file=sys.stderr)
        return 8

    print(transcript, end="" if transcript.endswith("\n") else "\n")
    advert_text = " plus one nearby Client advert" if args.advert else ""
    print(
        "TEST_PASS one-shot MeshCore RF TX; "
        f"profile=UK/EU_Narrow power=22dBm message_id={message_id}{advert_text}; "
        "no retry; TX locked; queue empty"
    )
    print(f"EXPECT_STOCK_APP 🦊 Kitsu {args.expected_uid}: {args.message}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
