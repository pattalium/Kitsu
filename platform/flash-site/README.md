# K32 Kitsu Web Serial flasher

This static site installs signed Kitsu firmware on the Heltec WiFi LoRa 32 V3
from desktop Chrome or Edge over HTTPS. Espressif's `esptool-js` runtime is
bundled locally; the page does not load flashing code, firmware, manifests, or
keys from a CDN.

## Stock new Heltec

The exact stock Heltec V3 partition table is a supported first-install source.
The initializer verifies the signed 0.20.5 application, signed Kitsu
bootloader, current partition table, and OTA selection bytes before writing.
It resets stock NVS, connectivity, and coredump state, installs the application
in both current A/B slots, reads every region back, and commits the current
partition table last. The owner can also select Fox, Cat, Dog, or a locally
validated `.k868` file as the starter. With **Keep installed companion**
selected, the complete pack region must hash identically before and after.

## Current Kitsu layout

The site verifies the exact signed `0.20.5` `.kitsu-fw` package already shipped
on the public site. After identifying an ESP32-S3 with 8 MiB flash and the exact
current partition table, it reads the OTA selection records and writes the
application only to the boot-selected slot:

- app0 at `0x050000`; or
- app1 at `0x350000`.

The firmware action does not perform a full-chip erase and does not write the bootloader,
partition table, NVS, OTA metadata, the other app slot, private OTA journals,
the custom companion pack, connectivity data, or coredump. The complete OTA
metadata region and complete `0x140000`-byte companion-pack region are read and
hashed before and after the app write. Any mismatch fails the install. The
application itself is read back and SHA-256 verified before the single reset.

## Legacy Kitsu layout

An exact legacy partition table remains eligible for the existing signed
historical recovery release. Its seven reviewed core writes and every readback
remain bound by that release's signed manifest. OTA metadata and the full
companion-pack region are hashed before and after legacy recovery. Companion
installation becomes available after the board uses the current layout.

## Companion packs

On a current-layout board, the companion action accepts the three built-in
starter packs or a local `.k868` file. Local files never leave the browser and
must pass format, bounds, CRC32, and SHA-256 validation. A same-companion update
keeps progress. Replacing a different companion writes the fixed PREPARED
record, writes and reads back the pack, then writes the matching COMMITTED
record so firmware can authorize the change and reset only that companion's
progress. Firmware actions remain separate and continue to preserve the pack.

Unknown, partial, or changed layouts fail closed before writing and show their
partition-table digest. A write or verification failure after flashing begins
leaves the Heltec in its ROM loader for a clean retry instead of automatically
booting an unverified result.

Run `npm ci` and then `npm run check`. Deploy only `dist/`.
