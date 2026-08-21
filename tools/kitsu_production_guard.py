"""PlatformIO guard for a production-configured but unsigned candidate.

Signing is performed by isolated sign_kitsu_production_stage.py invocations;
package_kitsu_production.py verifies those stages and encrypts the device-bound
bundle. An ordinary build never receives keys and can never upload an unsigned
production image by mistake.
"""

from pathlib import Path

Import("env")  # type: ignore[name-defined]  # PlatformIO/SCons injection
from SCons.Script import COMMAND_LINE_TARGETS


print(
    "KITSU_WITHDRAWN_NOT_AUTHORIZED: the secure-boot/eFuse production build "
    "was withdrawn by owner decision; use heltec_wifi_lora_32_V3_reflashable"
)
env.Exit(1)


blocked = {
    "upload",
    "uploadfs",
    "uploadfsota",
    "erase",
    "sign",
    "encrypt",
}
requested = {target.lower() for target in COMMAND_LINE_TARGETS}
if requested & blocked or any(target.startswith("__upload") for target in requested):
    print(
        "KITSU_PRODUCTION_BUILD_ERROR: direct upload/sign/encrypt is blocked; "
        "build the candidate, complete tools/sign_kitsu_production_stage.py "
        "for all three roles, then run tools/package_kitsu_production.py"
    )
    env.Exit(1)


def validate_candidate_layout(source, target, env):
    del source, target
    build_dir = Path(env.subst("$BUILD_DIR"))
    bootloader = build_dir / "bootloader.bin"
    if not bootloader.is_file():
        print("KITSU_PRODUCTION_BUILD_ERROR: bootloader candidate is missing")
        env.Exit(1)
    bootloader_bytes = bootloader.stat().st_size
    if bootloader_bytes == 0 or bootloader_bytes % 0x1000 != 0:
        print(
            "KITSU_PRODUCTION_BUILD_ERROR: unsigned bootloader is not a "
            "non-empty 4 KiB-aligned remote-signing candidate"
        )
        env.Exit(1)
    if bootloader_bytes > 0x7000 or bootloader_bytes + 0x1000 > 0x8000:
        print(
            "KITSU_PRODUCTION_BUILD_ERROR: unsigned bootloader does not leave "
            "one complete Secure Boot V2 signature sector before the 0x8000 "
            "partition table"
        )
        env.Exit(1)
    print(
        "KITSU_PRODUCTION_LAYOUT_OK: unsigned bootloader "
        f"0x{bootloader_bytes:X}; signed ceiling 0x{bootloader_bytes + 0x1000:X}; "
        "partition table 0x8000"
    )


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", validate_candidate_layout)

print("KITSU_PRODUCTION_CANDIDATE: unsigned and non-deployable by design")
