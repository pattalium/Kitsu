# Kitsu local-first platform

The supported Kitsu product has no runtime account, API, gateway, relay,
Keycloak, Wi-Fi provisioning, or browser companion. Android talks directly to
a nearby, explicitly authorized Kitsu over Bluetooth. Firmware updates are
downloaded as signed files and transferred offline over that authenticated BLE
session.

## Supported surfaces

- `mobile/android/` — direct-Bluetooth Android application.
- `public-site/` — product information and the signed Android download.
- `docs-site/` — the complete local-first user manual.
- `flash-site/` — signed USB bootstrap and recovery.
- `status-site/` — reachability checks for those static release surfaces.

The product CI builds and tests only these surfaces plus the local Heltec
firmware and its signed offline update package. It does not start containers,
PostgreSQL, an identity provider, an API, or a gateway.

## Historical source

`auth/`, `backend/`, `gateway/`, and `web/` are frozen rollback sources from the
connected-platform design. They are not compiled, tested, deployed, linked,
advertised, or accepted as part of the local-first product. The old runtime
remains online temporarily for the last published clients and rollback until
the local-first Android and firmware release passes both acceptance records.
Its frozen Rust dependencies retain a narrow scheduled audit during that
transition; this is not a local-first product build.

After that release is promoted, the operational retirement preserves immutable
database and binary archives, replaces the old public API/auth/gateway origins
with explicit static retirement responses, and stops the runtime services.
PostgreSQL is shared infrastructure and is never dropped or globally stopped
as part of this product change.

Machine names, private addresses, credentials, signing keys, and deployment
inventory never belong in this public repository.
