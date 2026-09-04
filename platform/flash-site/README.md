# K32 Kitsu Web Serial flasher

This static site installs signed Kitsu firmware on the Heltec WiFi LoRa 32 V3
from desktop Chrome or Edge over HTTPS. Espressif's `esptool-js` runtime is
bundled locally; the page does not load flashing code, firmware, manifests, or
keys from a CDN.

## Current Kitsu layout

The site verifies the exact signed `0.20.5` `.kitsu-fw` package already shipped
on the public site. After identifying an ESP32-S3 with 8 MiB flash and the exact
current partition table, it reads the OTA selection records and writes the
application only to the boot-selected slot:

- app0 at `0x050000`; or
- app1 at `0x350000`.

It does not perform a full-chip erase and does not write the bootloader,
partition table, NVS, OTA metadata, the other app slot, private OTA journals,
the custom companion pack, connectivity data, or coredump. The complete OTA
metadata region and complete `0x140000`-byte companion-pack region are read and
hashed before and after the app write. Any mismatch fails the install. The
application itself is read back and SHA-256 verified before the single reset.

## Legacy Kitsu layout

An exact legacy partition table remains eligible for the existing signed
historical recovery release. Its seven reviewed core writes and every readback
remain bound by that release's signed manifest. The site no longer offers any
companion selection or companion replacement path. OTA metadata and the full
companion-pack region are also hashed before and after legacy recovery.

Unknown, partial, or changed layouts fail closed before writing. A write or
verification failure after flashing begins leaves the Heltec in its ROM loader
for a clean retry instead of automatically booting an unverified result.

Run `npm ci` and then `npm run check`. Deploy only `dist/`.
