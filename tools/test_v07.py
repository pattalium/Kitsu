"""Non-destructive serial QA for the integrated Kitsu868 0.7.0 firmware.

The test intentionally has no firmware upload, flash erase, pack write, LoRa
listen, or radio-transmit path.  It exercises only the firmware's serial
commands, and the encounter is injected as a validated 19-byte protocol frame
over USB serial.  Normal pet/feed/play and encounter progression is additive;
the script never resets existing companion statistics or NVS.
"""

from __future__ import annotations

import argparse
import json
import re
import secrets
import struct
import sys
import time
from dataclasses import dataclass
from typing import Any, Iterable


FIRMWARE_NAME = "Kitsu868"
FIRMWARE_VERSION = "0.7.0"
EXPECTED_BOARD = "heltec-v3.2"
ENCOUNTER_WIRE_BYTES = 19
MEMORY_CAPACITY = 24
MAX_BOND_XP = 630

BOOT_PATTERN = re.compile(
    r"KITSU_BOOT\s+firmware=(\S+)\s+version=(\S+)\s+"
    r"board=(\S+)\s+tx_enabled=(true|false)"
)

PERSONALITIES = {"GENTLE", "BOLD", "CURIOUS", "PLAYFUL", "SHY", "IMPISH"}
MOODS = {
    "CONTENT",
    "DREAMING",
    "LISTENING",
    "DROWSY",
    "LONELY",
    "CURIOUS",
    "EXCITED",
    "DEVOTED",
    "IMPISH",
    "LOVED",
    "SATISFIED",
    "PLAYFUL",
    "PROUD",
    "STARTLED",
    "AWAKE",
}
EVOLUTION_NAMES = {0: "NEW", 1: "FAMILIAR", 2: "TRUSTED", 3: "RESONANT", 4: "ASCENDED"}
PACK_SNAPSHOT_FIELDS = (
    "companion",
    "pack_present",
    "pack_valid",
    "pack_error",
    "pack_id",
    "pack_revision",
    "pack_frames",
    "pack_bytes",
    "pack_capacity",
    "active_source",
)
PERSISTED_BRAIN_FIELDS = (
    "bond_level",
    "bond_xp",
    "bond_progress",
    "evolution_stage",
    "evolution_name",
    "appearance_variant",
    "unlocks",
    "memories",
    "lifetime_pets",
    "lifetime_feeds",
    "lifetime_plays",
    "games_played",
    "perfect_games",
    "encounters",
    "unique_encounters",
)
FORBIDDEN_SERIAL_COMMANDS = {"listen", "tx", "send", "transmit"}


class TestFailure(RuntimeError):
    """An assertion failure with a stable process exit code."""

    def __init__(self, message: str, code: int = 3) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class SyntheticEncounter:
    uid: int
    pack_id: int
    nonce: int
    wire: bytes

    @property
    def hex(self) -> str:
        return self.wire.hex().upper()


def parse_hex32(value: str) -> int:
    text = value.strip().removeprefix("0x").removeprefix("0X")
    if not re.fullmatch(r"[0-9A-Fa-f]{1,8}", text):
        raise argparse.ArgumentTypeError("expected one to eight hexadecimal digits")
    parsed = int(text, 16)
    if parsed == 0:
        raise argparse.ArgumentTypeError("pack ID must be non-zero")
    return parsed


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run USB-serial-only Kitsu868 0.7.0 hardware QA. The test never "
            "starts LoRa listening, never transmits RF, never flashes firmware, "
            "and never writes a companion pack."
        )
    )
    parser.add_argument(
        "--port",
        default="COM3",
        help="Heltec USB serial port (default: COM3; opened only while the test runs)",
    )
    parser.add_argument("--baud", type=int, default=115200, help="serial baud (default: 115200)")
    parser.add_argument(
        "--expected-uid",
        required=True,
        help="exact six-character UID read from the device (format example: KTDEAD)",
    )
    parser.add_argument(
        "--expected-name",
        default="CAT",
        help='installed companion display name (default: "CAT")',
    )
    parser.add_argument(
        "--expected-pack-id",
        type=parse_hex32,
        default=parse_hex32("FDC79D6F"),
        help="installed pack ID in hexadecimal (default: FDC79D6F)",
    )
    parser.add_argument(
        "--expected-pack-revision",
        type=int,
        default=2,
        help="installed pack revision (default: 2)",
    )
    parser.add_argument(
        "--expected-pack-frames",
        type=int,
        default=48,
        help="installed pack frame count (default: 48)",
    )
    parser.add_argument(
        "--expected-pack-bytes",
        type=int,
        default=24976,
        help="installed pack byte count (default: 24976)",
    )
    return parser.parse_args(argv)


def collect(port: Any, seconds: float) -> str:
    deadline = time.monotonic() + seconds
    chunks: list[bytes] = []
    while time.monotonic() < deadline:
        data = port.read(port.in_waiting or 1)
        if data:
            chunks.append(data)
    return b"".join(chunks).decode("utf-8", errors="replace")


def serial_command(port: Any, value: str, wait: float = 0.65) -> str:
    normalized = " ".join(value.strip().lower().split())
    first_word = normalized.split(" ", 1)[0] if normalized else ""
    if normalized in FORBIDDEN_SERIAL_COMMANDS or first_word in FORBIDDEN_SERIAL_COMMANDS:
        raise TestFailure(f"internal safety guard rejected serial command {value!r}", 2)
    port.write((value + "\n").encode("ascii"))
    port.flush()
    output = collect(port, wait)
    if "KITSU_RADIO listening=true" in output:
        raise TestFailure("firmware unexpectedly entered LoRa receive mode", 4)
    if "tx_enabled=true" in output:
        raise TestFailure("firmware unexpectedly reported RF transmit enabled", 4)
    return output


def reset_board(port: Any) -> str:
    # CP210x RTS resets the Heltec. DTR stays inactive so PRG/GPIO0 is not
    # asserted and the board boots the application instead of the ROM loader.
    port.dtr = False
    port.rts = True
    time.sleep(0.15)
    port.rts = False
    return collect(port, 5.0)


def prefixed_json_records(output: str, prefix: str) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    marker = prefix + " "
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
            records.append(value)
    return records


def latest_json(output: str, prefix: str) -> dict[str, Any]:
    records = prefixed_json_records(output, prefix)
    if not records:
        raise TestFailure(f"no valid {prefix} JSON record", 5)
    return records[-1]


def journal_records(output: str) -> list[dict[str, Any]]:
    records = prefixed_json_records(output, "KITSU_MEMORY")
    records.sort(key=lambda value: int(value.get("index", 1 << 30)))
    return records


def require(condition: bool, message: str, code: int = 6) -> None:
    if not condition:
        raise TestFailure(message, code)


def require_exact(record: dict[str, Any], expected: dict[str, Any], label: str) -> None:
    failures = {
        key: (record.get(key), wanted)
        for key, wanted in expected.items()
        if record.get(key) != wanted
    }
    require(not failures, f"{label} mismatch: {failures}")


def integer_field(
    record: dict[str, Any], key: str, minimum: int = 0, maximum: int = 0xFFFFFFFF
) -> int:
    value = record.get(key)
    require(
        not isinstance(value, bool) and isinstance(value, int),
        f"field {key!r} is not an integer: {value!r}",
    )
    require(minimum <= value <= maximum, f"field {key!r} outside {minimum}..{maximum}: {value}")
    return value


def validate_selftest(state: dict[str, Any], args: argparse.Namespace) -> None:
    require_exact(
        state,
        {
            "firmware": FIRMWARE_NAME,
            "version": FIRMWARE_VERSION,
            "board": EXPECTED_BOARD,
            "oled": True,
            "radio": True,
            "radio_code": 0,
            "storage": True,
            "button_released": True,
            "tx_enabled": False,
            "uid": args.expected_uid,
            "companion": args.expected_name,
            "orientation": "portrait",
            "ui_width": 64,
            "ui_height": 128,
            "pack_present": True,
            "pack_valid": True,
            "pack_error": "none",
            "pack_id": f"{args.expected_pack_id:08X}",
            "pack_revision": args.expected_pack_revision,
            "pack_frames": args.expected_pack_frames,
            "pack_bytes": args.expected_pack_bytes,
            "pack_capacity": 0x140000,
            "active_source": "pack",
            "animation_pack": True,
            "brain_storage": True,
            "encounter_protocol": 1,
            "sync_protocol": 1,
            "sync_transport": "serial",
            "remote_available": False,
        },
        "self-test",
    )

    integer_field(state, "boot", 1)
    integer_field(state, "energy", 0, 100)
    integer_field(state, "curiosity", 0, 100)
    integer_field(state, "affection", 0, 100)
    require(state.get("personality") in PERSONALITIES, f"invalid personality: {state.get('personality')!r}")
    require(state.get("mood") in MOODS, f"invalid mood: {state.get('mood')!r}")
    integer_field(state, "bond_level", 0, 10)
    integer_field(state, "bond_xp", 0, MAX_BOND_XP)
    integer_field(state, "bond_progress", 0, 100)
    stage = integer_field(state, "evolution_stage", 0, 4)
    require(state.get("evolution_name") == EVOLUTION_NAMES[stage], "evolution stage/name disagree")
    require(state.get("appearance_variant") == stage, "appearance variant does not follow evolution stage")
    integer_field(state, "unlocks", 0, 0x7FF)
    integer_field(state, "memories", 1, MEMORY_CAPACITY)
    for key in (
        "lifetime_minutes",
        "lifetime_pets",
        "lifetime_feeds",
        "lifetime_plays",
        "games_played",
        "perfect_games",
        "encounters",
        "unique_encounters",
    ):
        integer_field(state, key)

    require(isinstance(state.get("brain_loaded"), bool), "brain_loaded must be boolean")
    require(isinstance(state.get("battery_present"), bool), "battery_present must be boolean")
    battery_mv = integer_field(state, "battery_mv", 0, 20000)
    battery_pct = integer_field(state, "battery_pct", -1, 100)
    if state["battery_present"]:
        require(battery_mv > 0 and battery_pct >= 0, "present battery has invalid measurement")
    else:
        require(battery_pct == -1, "absent battery must report battery_pct=-1")
    require(isinstance(state.get("display_sleeping"), bool), "display_sleeping must be boolean")
    require(state.get("game") in {"none", "signal", "pounce"}, f"invalid game state: {state.get('game')!r}")
    require(re.fullmatch(r"[0-9A-F]{4}", str(state.get("last_peer", ""))) is not None, "last_peer is malformed")
    integer_field(state, "last_trait", -1, 15)
    integer_field(state, "last_gift", -1, 11)


def validate_sync(sync: dict[str, Any], state: dict[str, Any], args: argparse.Namespace) -> None:
    require_exact(
        sync,
        {
            "protocol": 1,
            "uid": args.expected_uid,
            "pack_id": f"{args.expected_pack_id:08X}",
            "companion": args.expected_name,
            "energy": state["energy"],
            "curiosity": state["curiosity"],
            "affection": state["affection"],
            "mood": state["mood"],
            "bond": state["bond_level"],
            "bond_xp": state["bond_xp"],
            "stage": state["evolution_stage"],
            "battery_pct": state["battery_pct"],
            "remote": False,
        },
        "sync",
    )
    require(isinstance(sync.get("sleeping"), bool), "sync sleeping must be boolean")
    require(sync.get("listening") is False, "device was already listening; refusing to mutate it")
    integer_field(sync, "memory_seq", 1, 0xFFFF)
    integer_field(sync, "memory_event", 0, 11)


def validate_journal(records: list[dict[str, Any]], expected_count: int) -> None:
    require(records, "journal produced no KITSU_MEMORY records")
    require(len(records) == expected_count, f"journal record count {len(records)} != {expected_count}")
    require([record.get("index") for record in records] == list(range(len(records))), "journal indexes are not contiguous")
    for record in records:
        integer_field(record, "sequence", 1, 0xFFFF)
        integer_field(record, "event", 0, 11)
        integer_field(record, "detail", 0, 255)
        integer_field(record, "value")
        require(isinstance(record.get("line1"), str) and record["line1"], "journal line1 is empty")
        require(isinstance(record.get("line2"), str) and record["line2"], "journal line2 is empty")


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def make_synthetic_encounter(own_uid: str, own_pack_id: int) -> SyntheticEncounter:
    own_suffix = int(own_uid[2:], 16)
    peer_uid = secrets.randbelow(0xFFFF) + 1
    if peer_uid == own_suffix:
        peer_uid = 1 if peer_uid == 0xFFFF else peer_uid + 1

    peer_pack_id = secrets.randbits(32) or 1
    if peer_uid == own_suffix and peer_pack_id == own_pack_id:
        peer_pack_id ^= 0xA5A55A5A
        if peer_pack_id == 0:
            peer_pack_id = 1
    nonce = secrets.randbits(32) or 1

    # Offer, appearance=2, evolution=1, bond=37, mood=CURIOUS(5), emote=3.
    appearance_and_stage = 2 | (1 << 5)
    mood_and_emote = 5 | (3 << 4)
    payload = struct.pack(
        "<2sBBHIBBBI",
        b"K8",
        1,
        1,
        peer_uid,
        peer_pack_id,
        appearance_and_stage,
        37,
        mood_and_emote,
        nonce,
    )
    require(len(payload) == 17, f"internal encounter payload is {len(payload)} bytes", 2)
    wire = payload + struct.pack("<H", crc16_ccitt_false(payload))
    require(len(wire) == ENCOUNTER_WIRE_BYTES, "internal encounter wire size is invalid", 2)
    return SyntheticEncounter(peer_uid, peer_pack_id, nonce, wire)


def plus_one_saturated(value: int) -> int:
    return min(0xFFFFFFFF, value + 1)


def expected_legacy_stats(initial: dict[str, Any]) -> dict[str, int]:
    energy = min(100, integer_field(initial, "energy", 0, 100) + 4)
    curiosity = min(100, integer_field(initial, "curiosity", 0, 100) + 1)
    affection = min(100, integer_field(initial, "affection", 0, 100) + 3)

    energy = min(100, energy + 18)
    affection = min(100, affection + 1)

    energy = energy - 6 if energy > 6 else 1
    curiosity = min(100, curiosity + 10)
    affection = min(100, affection + 4)

    curiosity = min(100, curiosity + 8)
    affection = min(100, affection + 2)
    return {"energy": energy, "curiosity": curiosity, "affection": affection}


def changed_values(
    current: dict[str, Any], expected: dict[str, Any], fields: Iterable[str]
) -> dict[str, tuple[Any, Any]]:
    return {
        key: (current.get(key), expected.get(key))
        for key in fields
        if current.get(key) != expected.get(key)
    }


def run_hardware_test(args: argparse.Namespace, serial_module: Any) -> tuple[str, str]:
    transcript: list[str] = []
    rf_sensitive_transcript: list[str] = []
    initial_sleeping: bool | None = None
    state_mutated = False
    game_active = False
    sleep_restored = False

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

        try:
            initial_self_output = serial_command(port, "selftest")
            transcript.append(initial_self_output)
            initial = latest_json(initial_self_output, "KITSU_SELFTEST")
            validate_selftest(initial, args)
            require(initial.get("game") == "none", "a game was already active; refusing to mutate device")

            initial_sync_output = serial_command(port, "sync")
            transcript.append(initial_sync_output)
            initial_sync = latest_json(initial_sync_output, "KITSU_SYNC")
            validate_sync(initial_sync, initial, args)
            initial_sleeping = bool(initial_sync["sleeping"])

            initial_journal_output = serial_command(port, "journal", 0.9)
            transcript.append(initial_journal_output)
            initial_journal = journal_records(initial_journal_output)
            validate_journal(initial_journal, integer_field(initial, "memories", 1, MEMORY_CAPACITY))
            require(initial_sync["memory_seq"] == initial_journal[0]["sequence"], "initial sync/journal sequence disagree")

            encounter = make_synthetic_encounter(args.expected_uid, args.expected_pack_id)

            # Four-step public-pack durations plus a 250 ms render margin. The
            # integration regression must not recreate the old QA bug where
            # the next command visibly cut a reaction short.
            action_waits = {"pet": 1.95, "feed": 2.35, "play": 1.85}
            for action in ("pet", "feed", "play"):
                state_mutated = True
                action_output = serial_command(port, action, action_waits[action])
                transcript.append(action_output)
                rf_sensitive_transcript.append(action_output)
                require(f"KITSU_EVENT {action}" in action_output, f"{action.upper()} produced no KITSU_EVENT")
                require("KITSU_ERROR" not in action_output, f"{action.upper()} was rejected")

            for game in ("signal", "pounce"):
                start_output = serial_command(port, f"game {game}", 0.2)
                transcript.append(start_output)
                rf_sensitive_transcript.append(start_output)
                game_active = True
                require(f"KITSU_GAME start={game}" in start_output, f"{game} game did not start")

                active_output = serial_command(port, "selftest", 0.35)
                transcript.append(active_output)
                active_state = latest_json(active_output, "KITSU_SELFTEST")
                require(active_state.get("game") == game, f"self-test did not expose active {game} game")

                cancel_output = serial_command(port, "game cancel", 0.2)
                transcript.append(cancel_output)
                rf_sensitive_transcript.append(cancel_output)
                cancelled_output = serial_command(port, "selftest", 0.35)
                transcript.append(cancelled_output)
                cancelled_state = latest_json(cancelled_output, "KITSU_SELFTEST")
                require(cancelled_state.get("game") == "none", f"{game} game did not cancel")
                game_active = False

            inject_output = serial_command(port, f"inject {encounter.hex}", 0.75)
            transcript.append(inject_output)
            rf_sensitive_transcript.append(inject_output)
            require("KITSU_ERROR" not in inject_output, "synthetic encounter was rejected")
            encounter_pattern = re.compile(
                rf"KITSU_ENCOUNTER uid={encounter.uid:04X} pack={encounter.pack_id:08X} "
                r"new=true trait=(\d+) gift=(\d+).*tx_enabled=false"
            )
            match = encounter_pattern.search(inject_output)
            require(match is not None, "synthetic encounter was not accepted as a new serial-injected peer")
            require(0 <= int(match.group(1)) < 16 and 0 <= int(match.group(2)) < 12, "encounter reward is out of range")

            if initial_sleeping:
                restore_output = serial_command(port, "sleep", 0.45)
                transcript.append(restore_output)
                rf_sensitive_transcript.append(restore_output)
                require("KITSU_EVENT sleeping=true" in restore_output, "initial sleep state was not restored")
                sleep_restored = True

            after_self_output = serial_command(port, "selftest", 0.7)
            transcript.append(after_self_output)
            after = latest_json(after_self_output, "KITSU_SELFTEST")
            validate_selftest(after, args)
            require(after.get("game") == "none", "a canceled game remained active")
            require(after.get("last_peer") == f"{encounter.uid:04X}", "self-test did not expose injected peer")
            integer_field(after, "last_trait", 0, 15)
            integer_field(after, "last_gift", 0, 11)

            legacy_expected = expected_legacy_stats(initial)
            require_exact(after, legacy_expected, "pet/feed/play/encounter stats")
            counter_expected = {
                "lifetime_pets": plus_one_saturated(integer_field(initial, "lifetime_pets")),
                "lifetime_feeds": plus_one_saturated(integer_field(initial, "lifetime_feeds")),
                "lifetime_plays": plus_one_saturated(integer_field(initial, "lifetime_plays")),
                "games_played": integer_field(initial, "games_played"),
                "encounters": plus_one_saturated(integer_field(initial, "encounters")),
                "unique_encounters": plus_one_saturated(integer_field(initial, "unique_encounters")),
            }
            require_exact(after, counter_expected, "brain lifetime counters")
            require(
                integer_field(after, "bond_xp", 0, MAX_BOND_XP) >= integer_field(initial, "bond_xp", 0, MAX_BOND_XP),
                "bond XP moved backwards",
            )
            pack_changes = changed_values(after, initial, PACK_SNAPSHOT_FIELDS)
            require(not pack_changes, f"companion pack metadata changed: {pack_changes}")

            after_sync_output = serial_command(port, "sync")
            transcript.append(after_sync_output)
            after_sync = latest_json(after_sync_output, "KITSU_SYNC")
            validate_sync(after_sync, after, args)
            require(after_sync["sleeping"] is initial_sleeping, "original awake/sleeping state was not preserved")

            after_journal_output = serial_command(port, "journal", 0.9)
            transcript.append(after_journal_output)
            after_journal = journal_records(after_journal_output)
            validate_journal(after_journal, integer_field(after, "memories", 1, MEMORY_CAPACITY))
            require(after_sync["memory_seq"] == after_journal[0]["sequence"], "post-action sync/journal sequence disagree")
            initial_sequences = {record["sequence"] for record in initial_journal}
            new_records = [record for record in after_journal if record["sequence"] not in initial_sequences]
            new_events = {record["event"] for record in new_records}
            missing_events = {1, 2, 3, 9} - new_events
            require(not missing_events, f"journal is missing new pet/feed/play/friend events: {sorted(missing_events)}")

            before_reset_boot = integer_field(after, "boot", 1)
            reset_output = reset_board(port)
            transcript.append(reset_output)
            boot_records = BOOT_PATTERN.findall(reset_output)
            require(boot_records, "no KITSU_BOOT record after RTS reset", 7)
            boot_firmware, boot_version, boot_board, boot_tx = boot_records[-1]
            require(
                (boot_firmware, boot_version, boot_board, boot_tx)
                == (FIRMWARE_NAME, FIRMWARE_VERSION, EXPECTED_BOARD, "false"),
                f"invalid boot record: {boot_records[-1]!r}",
                7,
            )
            require("KITSU_RADIO listening=true" not in reset_output, "reset unexpectedly started LoRa listening", 7)
            require("tx_enabled=true" not in reset_output, "reset unexpectedly enabled RF transmit", 7)

            reboot_records = prefixed_json_records(reset_output, "KITSU_SELFTEST")
            if reboot_records:
                final = reboot_records[-1]
            else:
                final_output = serial_command(port, "selftest", 0.8)
                transcript.append(final_output)
                final = latest_json(final_output, "KITSU_SELFTEST")
            validate_selftest(final, args)
            require(final.get("brain_loaded") is True, "brain did not reload its persisted slot")
            require(integer_field(final, "boot", 1) > before_reset_boot, "boot counter did not advance after RTS reset")
            require_exact(final, legacy_expected, "persisted legacy stats")

            persistent_changes = changed_values(final, after, PERSISTED_BRAIN_FIELDS)
            require(not persistent_changes, f"brain progression did not persist: {persistent_changes}")
            pack_changes = changed_values(final, initial, PACK_SNAPSHOT_FIELDS)
            require(not pack_changes, f"pack changed across RTS reset: {pack_changes}")

            final_sync_output = serial_command(port, "sync")
            transcript.append(final_sync_output)
            final_sync = latest_json(final_sync_output, "KITSU_SYNC")
            validate_sync(final_sync, final, args)
            require(final_sync["sleeping"] is initial_sleeping, "sleep state did not persist across reset")

            final_journal_output = serial_command(port, "journal", 0.9)
            transcript.append(final_journal_output)
            final_journal = journal_records(final_journal_output)
            validate_journal(final_journal, integer_field(final, "memories", 1, MEMORY_CAPACITY))
            require(final_journal == after_journal, "journal changed or failed to persist across reset")
            require(final_sync["memory_seq"] == final_journal[0]["sequence"], "final sync/journal sequence disagree")

            sensitive = "".join(rf_sensitive_transcript)
            require("KITSU_RADIO listening=true" not in sensitive, "test command entered LoRa receive mode")
            require("tx_enabled=true" not in sensitive, "test command enabled RF transmission")

            summary = (
                f"uid={args.expected_uid} companion=\"{args.expected_name}\" "
                f"pack={args.expected_pack_id:08X}/r{args.expected_pack_revision} "
                f"peer={encounter.uid:04X}-{encounter.pack_id:08X} "
                f"bond={initial['bond_xp']}->{after['bond_xp']} "
                f"memories={initial['memories']}->{after['memories']} "
                f"boot={before_reset_boot}->{final['boot']}"
            )
            return "".join(transcript), summary
        finally:
            # Leave a half-started game or altered sleep state behind only if
            # the serial link itself is already unusable. These cleanup writes
            # are still local USB actions and can never start the radio.
            try:
                if game_active:
                    transcript.append(serial_command(port, "game cancel", 0.2))
                if state_mutated and initial_sleeping and not sleep_restored:
                    transcript.append(serial_command(port, "sleep", 0.35))
            except Exception:
                pass


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    args.expected_uid = args.expected_uid.upper()
    if re.fullmatch(r"KT[0-9A-F]{4}", args.expected_uid) is None:
        print("TEST_FAIL --expected-uid must look like KTDEAD", file=sys.stderr)
        return 2
    if args.baud <= 0:
        print("TEST_FAIL --baud must be positive", file=sys.stderr)
        return 2
    if args.expected_pack_revision < 0 or args.expected_pack_frames <= 0 or args.expected_pack_bytes <= 0:
        print("TEST_FAIL expected pack metadata must be positive", file=sys.stderr)
        return 2

    # Importing pyserial only after argument parsing keeps --help completely
    # offline and guarantees that importing this module cannot touch COM3.
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
        "TEST_PASS Kitsu868 v0.7.0 serial-only QA; "
        f"{summary}; pet/feed/play persisted; both games start/cancel; "
        "synthetic encounter persisted; pack preserved; LoRa listen never started; RF TX disabled"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
