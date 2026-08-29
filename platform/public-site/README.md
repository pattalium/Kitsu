# k32.run public site

This dependency-free static site distributes the signed native Android app,
verifies signed Android OTA firmware packages, links documentation, and points to the public
source repository. The Kitsu app is local-first: it talks to the device through
authenticated Bluetooth and has no product account, gateway, or runtime-server
dependency. iOS is not part of the current release scope.

Android and firmware have independent release gates. The Android download may
be available while the firmware card safely reports that no physically
accepted signed firmware package is available.

`config.json` points to the public source repository at
`https://github.com/pattalium/Kitsu`.

## Android release trust

The download card exposes only a signed local-first Android release. A valid
historical manifest or APK may remain in `downloads/` for audit and rollback,
but `site.js` exposes only the exact Android 2.2.10 / version-code 31 production
contract. Install the accepted release directly from the browser on a supported
Android device.

The Android download has two independent integrity boundaries:

1. Android verifies the APK's dedicated Kitsu release certificate. Its SHA-256
   certificate digest is pinned in both `downloads/latest.json` and `site.js`.
2. The site verifies the exact bytes of `downloads/latest.json` with the pinned
   Ed25519 update public key before it exposes the download link.

The private Android keystore and Ed25519 update key live only in protected
release configuration. They must never be copied into this
directory or committed. `downloads/update-ed25519-public.pem` is public by
design. `downloads/latest.json.sig` is a raw 64-byte Ed25519 signature over the
exact `downloads/latest.json` bytes.

The browser accepts only a same-origin content-addressed APK path, the
stable/release package `ptl.kitsu.app`, exact version 2.2.10 / code 31, bounded
numeric fields, canonical lowercase SHA-256, the pinned direct-download APK
certificate, and a valid manifest timestamp. Repository tests additionally
verify the detached signature and hash the exact APK bytes.

The legacy `app.kitsu.mobile` package and the direct-download/Play signing
tracks do not cross-update. Before switching, connect to the Kitsu and use
Forget authorization, uninstall the old app, install the chosen track, and pair
again. The signed Play app is not distributed from this static site.

When the fixed `latest.json` authority advances, exact signed historical
manifests remain available under immutable names. The 2.0.0 authority is
`downloads/android-stable-2.0.0-20260822t123928z.json`; the 2.2.0 authority is
`downloads/android-stable-2.2.0-20260825t170821z.json`; the 2.2.1 authority is
`downloads/android-stable-2.2.1-20260826t002057z.json`; the 2.2.3 authority is
`downloads/android-stable-2.2.3-20260826t132716z.json`; the 2.2.4 authority is
`downloads/android-stable-2.2.4-20260828t092529z.json`; the 2.2.5 authority is
`downloads/android-stable-2.2.5-20260828t143256z.json`; and the immediately
prior 2.2.6 authority is
`downloads/android-stable-2.2.6-20260829t015805z.json`. Each retains its raw
detached signature. Historical APK and testing-preview bytes remain available
for audit and rollback but are not advertised as the current download.
Android 2.2.7 through 2.2.9 were never public release authorities, so their
candidate manifests and APKs are intentionally absent from the public tree.

## Firmware release trust

`firmware-release.js` now pins the one production-signed package accepted for
public firmware 0.20.3:

- URL: `/downloads/kitsu-firmware-0.20.3-022e01c0106007c6bb86ef3854a8ebd3c7fb41a2bdeda9a9285474eebe91af51.kitsu-fw`
- bytes: `1228050`
- SHA-256: `022e01c0106007c6bb86ef3854a8ebd3c7fb41a2bdeda9a9285474eebe91af51`
- release ID: `kitsu-0.20.3-reflashable-1`
- firmware version: `0.20.3`

Candidate images, unsigned manifests, private acceptance evidence, and
migration artifacts must never be copied into this public directory.

The final publication contract names one same-origin, content-addressed package
with its exact byte count, SHA-256, release ID, and firmware version. Before the
link is enabled, the browser verifies all of the following:

1. The fetched package matches that exact size, digest, and content-addressed
   path.
2. The `KITSUFW1` header has canonical bounded sections and exact end of file.
3. The embedded 64-byte Ed25519 signature over the canonical
   `kitsu.ble-firmware.v1` manifest verifies with the pinned Kitsu update
   authority.
4. The signed manifest binds the exact ESP32-S3 application bytes, SHA-256,
   device class, 3 MiB A/B slot, 4 KiB journal, rollback flag, and 4 KiB chunks.
5. The application has valid ESP segment bounds, checksum, appended digest, and
   exactly one bounded `KITSU-ID1` marker for firmware 0.20.3 and the current
   8 MiB dual-OTA flash geometry.

The signed inner package is the firmware authority. There is no second outer
manifest or signature. The deployment script independently reads the pinned
browser contract, rejects missing, extra, or mismatched `.kitsu-fw` files, and
opens only the exact package route. If a future source snapshot deliberately
sets the contract to `null`, it must contain no firmware package and the script
creates no firmware route or probe.

Public firmware 0.20.3 is only for a Heltec V3 board already migrated to the
current layout. The one-time 0.20.2 to 0.20.3 transition is a private,
double-backed-up, table-last serial procedure and is not an owner download.
The historical seven-write browser installer must not be used on a migrated or
unknown layout. After migration, normal releases use the verified signed
`.kitsu-fw` package through Android with A/B rollback handling.

The support card is a plain outbound HTTPS link to `https://ko-fi.com/pattalium`.
Support is voluntary and grants no app feature, content, badge, or other
benefit. The site does not embed payment code, collect payment details, or add
tracking parameters to that destination.
