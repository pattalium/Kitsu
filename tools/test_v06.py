"""Hardware smoke test for Kitsu868 0.6.0 and its rich Fox pack."""

from __future__ import annotations

import argparse
from collections import Counter
import json
import re
import struct
import sys
import time
from pathlib import Path

import serial

from install_pack import (
    PACK_CLIP_BYTES,
    PACK_HEADER_BYTES,
    PACK_PARTITION_BYTES,
    PackInfo,
    PackValidationError,
    validate_pack,
)


SELFTEST_PATTERN = re.compile(r"KITSU_SELFTEST\s+(\{[^\r\n]+\})")
ANIMATION_PATTERN = re.compile(
    r"KITSU_ANIM\s+role=([a-z]+)\s+duration_ms=(\d+)\s+mode=(\d+)"
)
ANIMATION_ROLES = (
    "idle",
    "blink",
    "pet",
    "sleep",
    "listen",
    "surprise",
    "play",
    "tired",
    "feed",
    "wake",
    "meet",
    "evolve",
)
EXPECTED_RICH_REVISION = 3
EXPECTED_RICH_FRAMES = 48
EXPECTED_RICH_CLIPS = 12
EXPECTED_RICH_STEPS = 48
ANIMATION_ACCEPT_WAIT = 0.35
DISPLAY_SETTLE_MARGIN = 0.25


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


def validate_rich_fox_pack(pack: PackInfo) -> None:
    expected_metadata = {
        "revision": EXPECTED_RICH_REVISION,
        "frame_count": EXPECTED_RICH_FRAMES,
        "clip_count": EXPECTED_RICH_CLIPS,
        "step_count": EXPECTED_RICH_STEPS,
    }
    failures = {
        key: (getattr(pack, key), wanted)
        for key, wanted in expected_metadata.items()
        if getattr(pack, key) != wanted
    }
    if failures:
        raise PackValidationError(f"not the expected rich Fox pack: {failures}")

    raw = pack.path.read_bytes()
    roles: list[int] = []
    for index in range(pack.clip_count):
        offset = PACK_HEADER_BYTES + index * PACK_CLIP_BYTES
        role, variant, unused_mode, unused_weight, unused_first, count, unused_reserved = (
            struct.unpack_from("<BBBBIHH", raw, offset)
        )
        roles.append(role)
        if variant != 0:
            raise PackValidationError(
                f"rich Fox clip {index} uses unsupported appearance variant {variant}"
            )
        if count != 4:
            raise PackValidationError(
                f"rich Fox clip {index} has {count} steps instead of 4"
            )

    role_counts = Counter(roles)
    expected_roles = set(range(len(ANIMATION_ROLES)))
    if set(role_counts) != expected_roles or any(count != 1 for count in role_counts.values()):
        raise PackValidationError(
            "rich Fox pack must contain exactly one clip for every semantic role; "
            f"found {dict(sorted(role_counts.items()))}"
        )


def expected_state(pack: PackInfo, expected_name: str, expected_uid: str) -> dict[str, object]:
    return {
        "firmware": "Kitsu868",
        "version": "0.6.0",
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
        "animation_pack": True,
    }


def state_failures(
    state: dict[str, object], expected: dict[str, object]
) -> dict[str, tuple[object, object]]:
    failures = {
        key: (state.get(key), wanted)
        for key, wanted in expected.items()
        if state.get(key) != wanted
    }
    animation_role = state.get("animation_role")
    allowed_roles = {"none", *ANIMATION_ROLES}
    if animation_role not in allowed_roles:
        failures["animation_role"] = (animation_role, "none or a semantic role")
    return failures


def integer_stat(state: dict[str, object], key: str) -> int:
    value = state.get(key)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"self-test field {key!r} is not an integer: {value!r}")
    if not 0 <= value <= 100:
        raise ValueError(f"self-test field {key!r} is outside 0..100: {value}")
    return value


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Exercise Kitsu868 0.6.0, every pack animation role, gameplay, "
            "and NVS persistence."
        )
    )
    parser.add_argument("--port", default="COM3", help="Heltec serial port")
    parser.add_argument("--baud", type=int, default=115200, help="firmware serial baud")
    parser.add_argument(
        "--expected-name",
        required=True,
        help="companion name expected from the installed rich Fox pack",
    )
    parser.add_argument(
        "--expected-pack",
        required=True,
        type=Path,
        help="local rich Fox .k868 file whose metadata must match the installed pack",
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
        validate_rich_fox_pack(pack)
    except (OSError, PackValidationError) as error:
        print(f"TEST_FAIL expected rich Fox pack is invalid: {error}", file=sys.stderr)
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
    animation_results: dict[str, tuple[int, int]] = {}
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

            for role in ANIMATION_ROLES:
                animation_output = command(
                    port, f"anim {role}", ANIMATION_ACCEPT_WAIT
                )
                output += animation_output
                if "KITSU_ERROR" in animation_output:
                    return fail(output, f"ANIM {role} was rejected", 6)
                records = ANIMATION_PATTERN.findall(animation_output)
                matching = [record for record in records if record[0] == role]
                if not matching:
                    return fail(output, f"ANIM {role} produced no acceptance record", 6)
                unused_role, duration_text, mode_text = matching[-1]
                duration_ms = int(duration_text)
                mode = int(mode_text)
                if duration_ms <= 0 or mode not in range(4):
                    return fail(
                        output,
                        f"ANIM {role} returned duration={duration_ms}, mode={mode}",
                        6,
                    )
                animation_results[role] = (duration_ms, mode)
                # Keep the role on the physical OLED for its complete declared
                # span. The old rapid sweep replaced every clip after 350 ms,
                # which made valid animations—especially LISTEN—look cut off.
                remaining_seconds = max(
                    0.0,
                    duration_ms / 1000.0 - ANIMATION_ACCEPT_WAIT,
                )
                output += collect(
                    port, remaining_seconds + DISPLAY_SETTLE_MARGIN
                )

            animation_state_output = command(port, "selftest", 0.5)
            output += animation_state_output
            animation_states = states(animation_state_output)
            if not animation_states:
                return fail(output, "no self-test after ANIM role sweep", 7)
            animation_state = animation_states[-1]
            failures = state_failures(animation_state, expected)
            if failures:
                return fail(output, f"state mismatch after ANIM role sweep: {failures}", 7)

            action_output = command(
                port,
                "feed",
                animation_results["feed"][0] / 1000.0
                + DISPLAY_SETTLE_MARGIN,
            )
            action_output += command(
                port,
                "play",
                animation_results["play"][0] / 1000.0
                + DISPLAY_SETTLE_MARGIN,
            )
            output += action_output
            if "KITSU_EVENT feed" not in action_output:
                return fail(output, "FEED did not produce a KITSU_EVENT", 8)
            if "KITSU_EVENT play" not in action_output:
                return fail(output, "PLAY did not produce a KITSU_EVENT", 8)
            if "KITSU_ERROR" in action_output:
                return fail(output, "FEED/PLAY was rejected", 8)

            after_actions_output = command(port, "selftest", 1.0)
            output += after_actions_output
            action_states = states(after_actions_output)
            if not action_states:
                return fail(output, "no self-test after FEED/PLAY", 9)
            after_actions = action_states[-1]
            failures = state_failures(after_actions, expected)
            if failures:
                return fail(output, f"pack/hardware changed after actions: {failures}", 9)

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
                    10,
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
                return fail(output, "no self-test after hardware reset", 11)
            final = reboot_states[-1]
            failures = state_failures(final, expected)
            if failures:
                return fail(output, f"post-reset state mismatch: {failures}", 12)

            persistence_failures = {
                key: (final.get(key), after_actions.get(key))
                for key in ("energy", "curiosity", "affection")
                if final.get(key) != after_actions.get(key)
            }
            if persistence_failures:
                return fail(
                    output,
                    f"FEED/PLAY values did not persist: {persistence_failures}",
                    13,
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
                    14,
                )
    except serial.SerialException as error:
        return fail(output, f"serial error on {args.port}: {error}", 15)

    print(output)
    accepted = ",".join(ANIMATION_ROLES)
    print(
        "TEST_PASS "
        f'Kitsu868 v0.6.0 uid={expected_uid} companion="{pack.display_name}" '
        f"pack_id={pack.pack_id:08X} revision={pack.revision} "
        f"frames={pack.frame_count} clips={pack.clip_count} steps={pack.step_count} "
        f"bytes={pack.total_bytes}; animations={accepted}; "
        "OLED/radio/storage OK; FEED/PLAY persisted across reset; TX disabled"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
