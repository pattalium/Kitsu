# Kitsu

Kitsu turns a Heltec WiFi LoRa 32 V3/V3.2 into a portrait companion with
memory, care, games, messages, authenticated Bluetooth control, signed
Bluetooth firmware updates, and MeshCore radio support.

![Kitsu](assets/brand/kitsu-app-icon.png)

Kitsu is local-first. The supported Android application talks directly to a
nearby Kitsu over Bluetooth and has no Internet permission. The shipped
firmware contains no Kitsu account, Wi-Fi provisioning, HTTP API, public or
self-hosted gateway, mobile relay, LAN controller, TLS client, or server
certificate runtime.

## Start here

- [Product and signed Android download](https://k32.run)
- [Complete user manual](https://docs.k32.run)
- [Signed USB/Web Serial bootstrap and recovery](https://flash.k32.run)
- [Public release-surface status](https://status.k32.run)

The product page exposes only the currently accepted, signed, local-first
Android release. The firmware installer remains fail-closed until the matching
local-only firmware finishes physical acceptance; an unaccepted candidate is
never presented as a public stable release.

## What is included

| Area | Purpose |
| --- | --- |
| `src/` | Heltec companion, MeshCore radio, authenticated BLE controller, controller revocation, and signed A/B BLE OTA. |
| `platform/mobile/android/` | Direct-Bluetooth Android app for saved Kitsu selection, state, care, messages, MeshCore control, cleanup, and offline firmware import. |
| `platform/public-site/` | Product website, policies, and signed Android release manifest. |
| `platform/docs-site/` | User manual for pairing, controls, messages, updates, security, and recovery. |
| `platform/flash-site/` | Web Serial bootstrap/recovery with signature, target, fixed-region, artifact, and readback verification. |
| `platform/status-site/` | Reachability checks for the retained static distribution surfaces. |
| `tools/package_kitsu_ble_firmware.mjs` | Strict offline `.kitsu-fw` assembler and ESP32-S3 image verifier. |

Historical backend, identity, browser-app, and gateway source may remain in
the repository for a bounded rollback/archive period. It is not part of the
supported local-first product, firmware link, Android package, normal product
CI, or user workflow. While the previously published runtime remains online,
its frozen Rust dependencies retain a narrow scheduled security audit; that
temporary audit is removed only when those services are retired.

## First use

1. Open the browser firmware installer in current desktop Chrome or Edge. If
   it reports that no accepted release is available, stop and wait for the
   firmware release gate; do not substitute a local candidate.
2. Connect a supported 8 MiB Heltec with a USB data cable.
3. Install the signed Kitsu release. The installer writes and reads back the
   reviewed rollback-enabled bootloader, partition table, both A/B application
   slots, both clean OTA journals, and the isolated legacy-connectivity clear
   image without full-chip erase.
4. Install the production-signed Android APK from the product page.
5. On Kitsu, hold PRG from Home, hold again while `CONNECT` is selected, then
   hold on `BLUETOOTH` to open the bounded `PAIR PHONE` window.
6. In Android, choose **Pair this phone**, compare the six-digit values, and
   approve only when they match.
7. Hold PRG for the system numeric-comparison prompt, then hold it again when
   Kitsu shows `PHONE READY` to grant the authenticated controller.
8. Connect directly over Bluetooth. Airplane mode is supported after
   Bluetooth is turned back on.

After this one USB bootstrap, normal signed firmware updates are imported as a
`.kitsu-fw` file in Android and transferred over the authenticated Bluetooth
session to the inactive application slot. Transfer checkpoints, flash
readback, SHA-256, ESP image validation, and bootloader rollback are enforced
on the Heltec; no online service is involved.

The complete procedure and troubleshooting steps are in the
[Getting started guide](https://docs.k32.run/getting-started/).

## Real owner controls

Android exposes only implemented local operations:

- save and select up to three nearby Kitsu authorizations;
- Pair, Finish pairing, Connect, Disconnect, and Forget authorization;
- state, history, peers, channels, messages, battery, needs, and progression;
- pet, feed, play, listen once, and send a direct or channel message;
- enable or disable the installed MeshCore radio profile; and
- import, install, resume, abort, reboot, and confirm a signed BLE update.

Forget authorization is device-first: the Heltec durably revokes that phone's
controller before Android removes its local root. It does not erase packs,
progression, MeshCore state, or other authorized phones.

## Device controls

A short PRG press is less than 750 ms. A hold is 750 ms or longer.

- Home: short press pets Kitsu; hold opens the menu.
- Menus: short press moves; hold selects.
- Inbox: short press moves through older messages; hold returns Home.
- Games: short press acts; hold quits.
- Pair Phone: hold confirms the matching code or grants the pending phone.

Do not hold PRG while resetting unless firmware-download mode is intentional.

## Security and recovery model

Kitsu uses Bluetooth LE Secure Connections, numeric comparison, explicit PRG
confirmation, device-issued controller roots, fresh authenticated sessions,
bounded encrypted envelopes, sequence/replay checks, Android Keystore-backed
storage, and signed release manifests.

Firmware OTA is authorized only inside an authenticated controller session.
The device independently verifies the canonical Ed25519 manifest, exact image
hash and ESP32-S3 structure, writes only the inactive application slot, and
requires a healthy pending-verification boot before confirming it.

The firmware remains deliberately owner-reflashable. The installer does not
burn eFuses, enable Secure Boot or Flash Encryption, disable ROM download,
lock debugging, or prevent whole-chip erase and restoration of stock MeshCore.
Physical possession is therefore not presented as a tamper-proof boundary.

The exact build, package, partition, signing, and physical-acceptance contract
is documented in [docs/reflashable_release.md](docs/reflashable_release.md).

## Development

The active platform CI builds the exact reflashable ESP32-S3 target, runs the
native BLE/session/security/OTA and MeshCore suites, validates the offline
firmware package, compiles and tests Android, and audits the static product,
manual, status, and Web Serial surfaces. No Docker service is required for the
supported local-first product gates.

Kitsu targets the Heltec WiFi LoRa 32 V3 / V3.2 with 8 MiB flash. Radio use
must follow the installed regional profile and local regulations.
