# Kitsu connected platform

This directory is the production connectivity layer around the Heltec
companion. It does not replace the on-device companion core or MeshCore client.

## Surfaces

- `gateway/` — owner-operated Rust service on the home PC. Its production
  contract accepts a Heltec over private Wi-Fi and device TLS/mTLS, durably
  spools signed device envelopes without SQLite, and routes opaque
  backend-signed actions.
- `backend/` — Rust service with PostgreSQL, external OIDC, gateway mTLS, KMS
  envelope encryption, durable replay protection, owner APIs, and browser BFF.
- `mobile/android/` — native Kotlin/Compose app. A known companion is used over
  authenticated Bluetooth first; the remote backend is fallback only when the
  bonded device is genuinely absent.
- `web/` — authenticated browser companion UI. It is a first-class owner
  surface, but it is not used as the phone app and has no local database.

Android is the only native mobile release currently implemented. There is no
iOS application in this repository or in the current release scope.

## Public services

The production domain split provides the public product and Android download,
browser companion, owner API, identity, manual, firmware installer, status,
and signed-update services. Browser code does not scan the LAN, trust mDNS, or
connect to an arbitrary private address. The gateway establishes its own
outbound authenticated backend session.

Machine-specific inventory, operating-system account names, private addresses,
certificate labels, credential locations, backup destinations, and applied
edge state are deliberately excluded from this public repository. Deployments
provide those values through protected configuration. See `ops/README.md` for
the publication boundary.

If no enrolled companion exists, the browser surface links the real setup
manual and repository. It never substitutes preview state or an inactive link.

## Security invariants

The platform source and owner-reflashable firmware release contain the full
device path: authenticated-BLE/PRG owner enrollment, device-generated CSR/HPKE
proof, private bootstrap, Wi-Fi policy, steady TLS/mTLS, signed-envelope
queue/ACK handling, durable replay, heartbeat, and the authenticated
remote-action bridge. Production acceptance binds the matching signed Android
release to a real device-to-gateway-to-backend test.

This is intentionally not a verified-boot or physical-tamper boundary. The
release burns no eFuses, leaves serial erase/reflash and debug/download paths
available, and reports Secure Boot, Flash Encryption, NVS Encryption, and
hardware-root protection as false. A physical reflash can extract or bypass
application state; TLS/mTLS, authenticated BLE, owner authorization, and signed
remote actions remain mandatory for normal network operation.

The gates are fail-closed and deliberately ordered. Configured gateway trust
without an enrollment reports authoritative `lan_state:"enrollment_pending"`.
After enrollment, ordinary Wi-Fi, time, replay, TLS/mTLS, and backend gates
apply. Device status reports `security_mode:"reflashable"`; it does not require
eFuses or use `security_blocked` as a substitute for network validation.
Authenticated BLE remains the preferred nearby path; remote state is accepted
only after a current device-authenticated snapshot is available.

- Companion/backend HMAC secrets are separate from MeshCore identity,
  Bluetooth controller roots, Wi-Fi credentials, and TLS private keys.
- The PC gateway must not receive the companion/backend HMAC secret. The
  backend seals it directly to the device-held one-use HPKE key and issues a
  certificate whose SAN binds the companion UUID; the gateway relays only the
  exact opaque claim and sealed response.
- Remote actions are signed end-to-end by the backend and verified on the
  Heltec. Gateway mTLS provides transport authentication but is not action
  authorization.
- A gateway or browser action never bypasses expiry, consent, radio policy, or
  firmware validation.
- PostgreSQL is the hosted durable database. The gateway uses a checksummed,
  fsynced segmented WAL; mobile caches are encrypted bounded files. There is no
  SQLite/D1/Room/Core Data database in this platform.
