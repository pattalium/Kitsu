# K32 Kitsu Web Serial installer

This static browser application uses Espressif's Apache-2.0 `esptool-js`
runtime, bundled into the release by Vite. It does not load scripts, flash
code, manifests, or keys from a CDN.

The production gate downloads `latest.json`, its raw 64-byte detached Ed25519
signature, and the public SPKI from `updates.k32.run`. The SPKI must match the
installed Kitsu authority fingerprint before the exact signed JSON bytes are
accepted. Both artifacts are SHA-256 verified before an install is enabled and
the latest signed manifest and artifacts are fetched again immediately before
writing. Both written regions are then read from flash and hash-verified.

The signed manifest and runtime both constrain an install to exactly two
writes, in order:

1. the reviewed 3,072-byte Kitsu partition table at `0x008000`;
2. the ESP32-S3 application within app0 at `0x010000`.

There is no full-chip erase command and no bootloader, OTA-data, NVS,
companion-pack, connectivity-state, or eFuse write path. Flash writes
necessarily erase only the target flash sectors before programming them;
`eraseAll` remains false.

Run `npm ci` followed by `npm run check`. Deploy only `dist/`, never this source
tree or `node_modules/`. Physical browser acceptance still requires a Heltec
V3 connected to current desktop Chrome or Edge over HTTPS.
