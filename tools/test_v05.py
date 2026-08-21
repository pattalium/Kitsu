"""Hardware smoke test for Kitsu868 firmware 0.5.0 and one raw pack."""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path

import serial

from install_pack import PACK_PARTITION_BYTES, PackValidationError, validate_pack


SELFTEST_PATTERN = re.compile(r"KITSU_SELFTEST\s+(\{[^\r\n]+\})")


def collect(port: serial.Serial, seconds: float) -> str:
    deadline = time.monotonic() + seconds
    chunks: list[bytes] = []
    while time.monotonic() < deadline:
        data = port.read(port.in_waiting or 1)
        if data:
            chunks.append(data)
    return b"".join(chunks).decode("utf-8", errors="replace")


def command(port: serial.Serial, value: str, wait: float = 0.8) -> str:
    port.write((value + "\n").encode("ascii"))
    port.flush()
    return collect(port, wait)


def states(output: str) -> list[dict[str, object]]:
    parsed: list[dict[str, object]] = []
    for record in SELFTEST_PATTERN.findall(output):
        try:
            value = json.loads(record)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            parsed.append(value)
    return parsed


def reset_board(port: serial.Serial) -> str:
    # The Heltec V3's CP210x reset line is driven by RTS. DTR remains inactive
    # so GPIO0/PRG is not asserted into the ROM bootloader during this reset.
    port.rts = True
    time.sleep(0.15)
    port.rts = False
    return collect(port, 3.5)


def expected_state(pack, expected_name: str, expected_uid: str) -> dict[str, object]:
    return {
        "firmware": "Kitsu868",
        "version": "0.5.0",
        "board": "heltec-v3.2",
        "oled": True,
        "radio": True,
        "radio_code": 0,
        "storage": True,
        "button_released": True,
        "tx_enabled": False,
        "uid": expected_uid,
        "companion": expected_name,
        "orientation": "portrait",
        "ui_width": 64,
        "ui_height": 128,
        "pack_present": True,
        "pack_valid": True,
        "pack_error": "none",
        "pack_id": f"{pack.pack_id:08X}",
        "pack_revision": pack.revision,
        "pack_frames": pack.frame_count,
        "pack_bytes": pack.total_bytes,
        "pack_capacity": PACK_PARTITION_BYTES,
        "active_source": "pack",
    }


def state_failures(
    state: dict[str, object], expected: dict[str, object]
) -> dict[str, tuple[object, object]]:
    return {
        key: (state.get(key), wanted)
        for key, wanted in expected.items()
        if state.get(key) != wanted
    }


def integer_stat(state: dict[str, object], key: str) -> int:
    value = state.get(key)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"self-test field {key!r} is not an integer: {value!r}")
    if not 0 <= value <= 100:
        raise ValueError(f"self-test field {key!r} is outside 0..100: {value}")
    return value


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Exercise Kitsu868 0.5.0, its installed raw pack, and NVS persistence."
    )
    parser.add_argument("--port", default="COM3", help="Heltec serial port")
    parser.add_argument("--baud", type=int, default=115200, help="firmware serial baud")
    parser.add_argument(
        "--expected-name",
        required=True,
        help="companion name expected from the installed pack",
    )
    parser.add_argument(
        "--expected-pack",
        required=True,
        type=Path,
        help="local .k868 file whose metadata must match the installed pack",
    )
    parser.add_argument(
        "--expected-uid",
        required=True,
        help="exact six-character hardware UID read from the device (format example: KTDEAD)",
    )
    return parser.parse_args(argv)


def fail(output: str, message: str, code: int) -> int:
    print(output)
    print(f"TEST_FAIL {message}", file=sys.stderr)
    return code


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if not re.fullmatch(r"KT[0-9A-Fa-f]{4}", args.expected_uid):
        print("TEST_FAIL --expected-uid must look like KTDEAD", file=sys.stderr)
        return 2
    expected_uid = args.expected_uid.upper()

    try:
        pack = validate_pack(args.expected_pack)
    except (OSError, PackValidationError) as error:
        print(f"TEST_FAIL expected pack is invalid: {error}", file=sys.stderr)
        return 2
    if pack.display_name != args.expected_name:
        print(
            "TEST_FAIL --expected-name does not match the expected pack: "
            f"{args.expected_name!r} != {pack.display_name!r}",
            file=sys.stderr,
        )
        return 2

    expected = expected_state(pack, args.expected_name, expected_uid)
    output = ""
    try:
        with serial.Serial(
            args.port,
            args.baud,
            timeout=0.1,
            dsrdtr=False,
            rtscts=False,
        ) as port:
            port.dtr = False
            port.rts = False

            output += collect(port, 1.5)
            initial_output = command(port, "selftest", 1.0)
            output += initial_output
            initial_states = states(initial_output)
            if not initial_states:
                return fail(output, "no initial KITSU_SELFTEST record", 3)
            initial = initial_states[-1]
            failures = state_failures(initial, expected)
            if failures:
                return fail(output, f"initial state mismatch: {failures}", 4)

            try:
                initial_energy = integer_stat(initial, "energy")
                initial_curiosity = integer_stat(initial, "curiosity")
                initial_affection = integer_stat(initial, "affection")
            except ValueError as error:
                return fail(output, str(error), 5)

            action_output = command(port, "feed", 0.9)
            action_output += command(port, "play", 0.9)
            output += action_output
            if "KITSU_EVENT feed" not in action_output:
                return fail(output, "FEED did not produce a KITSU_EVENT", 6)
            if "KITSU_EVENT play" not in action_output:
                return fail(output, "PLAY did not produce a KITSU_EVENT", 6)
            if "KITSU_ERROR no_pack" in action_output:
                return fail(output, "FEED/PLAY rejected the installed pack", 6)

            after_actions_output = command(port, "selftest", 1.0)
            output += after_actions_output
            action_states = states(after_actions_output)
            if not action_states:
                return fail(output, "no self-test after FEED/PLAY", 7)
            after_actions = action_states[-1]
            failures = state_failures(after_actions, expected)
            if failures:
                return fail(output, f"pack/hardware changed after actions: {failures}", 7)

            expected_energy = min(100, initial_energy + 18)
            expected_energy = expected_energy - 6 if expected_energy > 6 else 5
            expected_curiosity = min(100, initial_curiosity + 10)
            expected_affection = min(100, min(100, initial_affection + 1) + 4)
            expected_stats = {
                "energy": expected_energy,
                "curiosity": expected_curiosity,
                "affection": expected_affection,
            }
            action_stat_failures = {
                key: (after_actions.get(key), wanted)
                for key, wanted in expected_stats.items()
                if after_actions.get(key) != wanted
            }
            if action_stat_failures:
                return fail(
                    output,
                    f"FEED/PLAY state transition mismatch: {action_stat_failures}",
                    8,
                )

            initial_boot = initial.get("boot")
            reboot_output = reset_board(port)
            output += reboot_output
            reboot_states = states(reboot_output)
            if not reboot_states:
                requested = command(port, "selftest", 1.0)
                output += requested
                reboot_states = states(requested)
            if not reboot_states:
                return fail(output, "no self-test after hardware reset", 9)
            final = reboot_states[-1]
            failures = state_failures(final, expected)
            if failures:
                return fail(output, f"post-reset state mismatch: {failures}", 10)

            persistence_failures = {
                key: (final.get(key), after_actions.get(key))
                for key in ("energy", "curiosity", "affection")
                if final.get(key) != after_actions.get(key)
            }
            if persistence_failures:
                return fail(
                    output,
                    f"FEED/PLAY values did not persist: {persistence_failures}",
                    11,
                )

            final_boot = final.get("boot")
            if (
                isinstance(initial_boot, bool)
                or not isinstance(initial_boot, int)
                or isinstance(final_boot, bool)
                or not isinstance(final_boot, int)
                or final_boot <= initial_boot
            ):
                return fail(
                    output,
                    f"boot counter did not advance across reset: {initial_boot!r} -> {final_boot!r}",
                    12,
                )
    except serial.SerialException as error:
        return fail(output, f"serial error on {args.port}: {error}", 13)

    print(output)
    print(
        "TEST_PASS "
        f'Kitsu868 v0.5.0 uid={expected_uid} companion="{pack.display_name}" '
        f"pack_id={pack.pack_id:08X} frames={pack.frame_count} "
        f"bytes={pack.total_bytes}; OLED/radio/storage OK; "
        "FEED/PLAY persisted across reset; TX disabled"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
