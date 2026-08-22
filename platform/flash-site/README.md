# K32 Kitsu Web Serial installer

This static browser application uses Espressif's Apache-2.0 `esptool-js`
runtime, bundled into the release by Vite. It does not load scripts, flash
code, manifests, or keys from a CDN.

The release gate downloads `latest.json`, its raw 64-byte detached Ed25519
signature, and the public SPKI from `updates.k32.run`. The SPKI must match the
installed Kitsu authority fingerprint before the exact signed JSON bytes are
accepted. All five unique artifact files are SHA-256 verified before an install is enabled and
the latest signed manifest and artifacts are fetched again immediately before
writing. All seven written regions are then read from flash and hash-verified.

The signed v2 manifest and runtime both constrain an install to exactly seven
writes, in order:

1. the reviewed rollback-enabled Kitsu bootloader at `0x000000`;
2. the reviewed 3,072-byte Kitsu partition table at `0x008000`;
3. the ESP32-S3 application within app0 at `0x010000`;
4. an erased 4 KiB OTA journal at `0x33f000`;
5. the exact same application bytes within app1 at `0x340000`;
6. the same erased 4 KiB OTA journal at `0x66f000`; and
7. a 256 KiB all-`0xff` retirement image over the isolated legacy
   `kitsu_conn` partition at `0x7b0000`.

Writing both A/B slots is required because the preserved OTA-data partition may
select either slot after a Bluetooth update or rollback. A serial recovery must
therefore make both possible boot selections refer to the same accepted image;
an older two-write manifest is rejected rather than risking a stale app1 boot.
Both private OTA journals are erased so a signed USB recovery cannot inherit a
receiving, ready, or confirmed Bluetooth-update record from an older image.
The final write removes historical Wi-Fi, gateway, mTLS, and backend secrets;
it cannot overlap NVS controller records, companion packs, MeshCore state, or
the coredump partition. Local-only firmware independently verifies the same
retirement for upgrade paths that do not pass through this installer.

The exact bootloader write is required for a stock or older Heltec because A/B
rollback is a bootloader feature. Its bytes and SHA-256 are bound by the signed
manifest and physical-acceptance record and read back like every other region.
It remains an ordinary, replaceable ESP32-S3 bootloader and does not enable
Secure Boot, Flash Encryption, anti-rollback eFuses, or a debug lock.

There is no full-chip erase command and no OTA-data, companion-state,
companion-pack, controller-store, MeshCore-state, coredump, or eFuse write path. Flash writes
necessarily erase only the target flash sectors before programming them;
`eraseAll` remains false.

Run `npm ci` followed by `npm run check`. Deploy only `dist/`, never this source
tree or `node_modules/`. Physical browser acceptance still requires a Heltec
V3 connected to current desktop Chrome or Edge over HTTPS.
