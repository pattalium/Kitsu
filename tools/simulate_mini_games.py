#!/usr/bin/env python3
"""Deterministic, dependency-free preview of Kitsu868's mini-game motion.

This is a small host-side oracle for the public timing contract.  It prints the
same marker/object positions expected from mini_games.cpp, including a run that
crosses the uint32 millis() rollover.  The C++ assertions live alongside this
script in mini_games_host_test.cpp.
"""

from __future__ import annotations

MASK32 = 0xFFFFFFFF


def u32(value: int) -> int:
    return value & MASK32


def elapsed(now: int, start: int) -> int:
    return u32(now - start)


def xorshift32(value: int) -> int:
    value ^= u32(value << 13)
    value ^= value >> 17
    value ^= u32(value << 5)
    return u32(value)


def signal_x(now: int, start: int, left: int = 5, right: int = 58,
             one_way_ms: int = 1800) -> int:
    phase = elapsed(now, start) % (one_way_ms * 2)
    distance_time = phase if phase <= one_way_ms else one_way_ms * 2 - phase
    return left + (distance_time * (right - left) + one_way_ms // 2) // one_way_ms


def object_x(now: int, start: int, left_to_right: bool,
             left: int = 5, right: int = 58, travel_ms: int = 2800) -> int:
    age = min(elapsed(now, start), travel_ms)
    offset = (age * (right - left) + travel_ms // 2) // travel_ms
    return left + offset if left_to_right else right - offset


def main() -> None:
    seed = 0x12345678
    next_seed = xorshift32(seed)
    target_left = 5 + next_seed % (58 - 5 - 11 + 2)
    target_right = target_left + 10
    print(f"SIGNAL seed=0x{seed:08X} target={target_left}..{target_right}")
    print("  ms : " + " ".join(f"{ms:4}" for ms in range(0, 3601, 300)))
    print("   x : " + " ".join(f"{signal_x(ms, 0):4}" for ms in range(0, 3601, 300)))

    direction_seed = xorshift32(seed)
    left_to_right = (direction_seed & 1) == 0
    direction = "L->R" if left_to_right else "R->L"
    print(f"POUNCE seed=0x{seed:08X} direction={direction} catch=25..38")
    print("  ms : " + " ".join(f"{ms:4}" for ms in range(0, 2801, 280)))
    print("   x : " + " ".join(
        f"{object_x(ms, 0, left_to_right):4}" for ms in range(0, 2801, 280)
    ))

    wrap_start = 0xFFFFFF00
    wrapped = [signal_x(u32(wrap_start + ms), wrap_start) for ms in range(0, 1201, 100)]
    normal = [signal_x(100 + ms, 100) for ms in range(0, 1201, 100)]
    assert wrapped == normal
    assert min(wrapped) >= 5 and max(wrapped) <= 58
    print("ROLLOVER parity=PASS samples=" + ",".join(map(str, wrapped)))
    print("MINI_GAMES_SIM_PASS")


if __name__ == "__main__":
    main()
