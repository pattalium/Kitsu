# K32 Kitsu companion Web UI

This is the browser companion surface. It is deliberately separate from the
native Android app and from the public `k32.run` project/flasher site.
It has no local database and no server-side copy of companion state: the Rust
backend and PostgreSQL are the durable authority.

## Authentication and data flow

- The backend performs external OIDC Authorization Code + PKCE and issues an
  opaque `__Host-kitsu_session` cookie. The browser never stores a bearer token.
- Mutations use the session-bound CSRF token returned by
  `GET /v1/browser/session`, an exact allowed `Origin`, and a fresh
  `Idempotency-Key`.
- The UI first lists the owner's companions, then loads an owner-scoped snapshot
  and peer list. It never relies on a magic `primary` identifier.
- Care actions become expiring, end-to-end-authenticated remote actions. The
  home gateway only routes them; the Heltec remains the final policy authority.
- When `VITE_KITSU_API_BASE` is absent, the interface fails closed in the
  self-host/server-required state. It renders no companion snapshot and no
  action controls.

## Local checks

Use Node.js 22.13 or newer:

```text
npm install
npm run lint
npm test
```

The canonical production browser origin is `https://app.k32.run`, with the
owner API at `https://api.k32.run` and identity issuer at
`https://auth.k32.run`. They remain environment-configurable for development.
Configure the backend OIDC client and exact browser-origin allowlist before
enabling live data.

Set `VITE_KITSU_SERVER_REPOSITORY_URL` to the owner-server repository. If an
authenticated owner has no enrolled server or companion, the UI sends them to
the connectivity guide instead of fabricating a route.

This UI can be hosted independently because it is stateless. Do not add D1,
SQLite, local account storage, Wi-Fi credentials, companion secrets, or gateway
private keys to this project.

## Hosting model

`npm run build` produces the static `dist/` directory. The production model is
to serve that directory from local nginx on the Kitsu host and expose only the
named HTTPS route through Cloudflare Tunnel and Cloudflare DNS. Keep the local
origin bound to the LAN or loopback interface; do not port-forward it directly.

Recommended public host split:

- `k32.run`: public Kitsu project and Android downloads.
- `app.k32.run`: authenticated companion Web UI from this directory.
- `api.k32.run`: owner API and browser BFF.
- `auth.k32.run`: external OIDC issuer.
- `flash.k32.run`: immutable historical pre-0.20.3 Web Serial installer; it is
  noncurrent and unsupported for migrated or unknown-layout boards.

The native Android app never embeds this UI. It connects directly over BLE
when the companion is nearby and uses the owner API only when BLE is genuinely
absent.
