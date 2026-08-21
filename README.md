# Kitsu

Kitsu turns a Heltec WiFi LoRa 32 V3 into a portrait companion with memory,
care, games, messages, Bluetooth, Wi-Fi remote access, and MeshCore radio
support.

![Kitsu](assets/brand/kitsu-app-icon.png)

## Start here

- [Product and signed Android download](https://k32.run)
- [Complete user manual](https://docs.k32.run)
- [Browser firmware installer](https://flash.k32.run)
- [Authenticated companion web app](https://app.k32.run)
- [Public service status](https://status.k32.run)

## What is included

| Area | Purpose |
| --- | --- |
| `platform/mobile/android/` | Native portrait Android app for nearby Bluetooth, remote access, care, setup, and messaging. |
| `platform/web/` | Authenticated browser companion. |
| `platform/backend/` | Owner API, persistence, authorization, enrollment, certificate issuance, and signed remote actions. |
| `platform/gateway/` | Local device TLS/mTLS gateway with durable queueing and replay protection. |
| `platform/public-site/` | Product website, policies, signed APK manifest, and Android release. |
| `platform/docs-site/` | User manual for setup, controls, connectivity, messages, updates, and recovery. |
| `platform/flash-site/` | Web Serial firmware installer with signature, target, artifact, and read-back verification. |
| `platform/status-site/` | Direct public service checks. |

## First use

1. Open the browser firmware installer on a desktop Chromium-based browser.
2. Connect a supported Heltec board with a USB data cable.
3. Install the verified Kitsu release and wait for read-back verification.
4. Install the signed Android APK from the product page.
5. Open `PHONE` on Kitsu, compare the six-digit pairing value, and approve only
   when both displays match.
6. Hold PRG when prompted to confirm the pairing, then hold it again on
   `PHONE READY` to grant the phone.
7. Use Bluetooth nearby, or follow the manual to add Wi-Fi and owner-approved
   remote access.

The full procedure, including screenshots and troubleshooting language, lives
in the [Getting started guide](https://docs.k32.run/getting-started/).

## Device controls

A short PRG press is less than 750 ms. A hold is 750 ms or longer.

- Home: short press pets Kitsu; hold opens the menu.
- Menus: short press moves; hold selects.
- Inbox: short press moves through older messages; hold returns Home.
- Games: short press acts; hold quits.
- Pair Phone: hold confirms the matching code or grants the pending phone.

Do not hold PRG while resetting unless firmware-download mode is intentional.

## Security model

Kitsu uses authenticated Bluetooth enrollment, physical confirmation, pinned
TLS/mTLS, signed remote actions, expiry, replay protection, Android Keystore,
encrypted application storage, and signed release manifests.

The firmware is deliberately owner-reflashable. It does not burn permanent
eFuses or lock the Heltec to Kitsu, so the board can be erased, restored to
stock MeshCore, or repurposed. Physical access is therefore not a tamper-proof
security boundary.

## Public-repository privacy

This repository excludes deployment-specific machine names, private network
addresses, credential locations, certificate labels, applied edge state, and
backup destinations. Real installations keep that inventory in protected
configuration outside the public source tree.

## Development

The platform CI workflow exercises Rust services, Android, public release
integrity, the browser app, firmware installer, documentation, and encoding
checks. Each component also carries its own build and test instructions.

Kitsu targets the Heltec WiFi LoRa 32 V3 / V3.2 with 8 MiB flash. Radio use
must follow the configured regional profile and local regulations.
