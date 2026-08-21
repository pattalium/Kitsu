# Kitsu platform backend

This directory contains the Rust service boundary for Kitsu's private
companion platform. PostgreSQL is its only durable store. It does not contain
firmware, the PC gateway, a native app, the authenticated companion Web UI, or
the public project/flasher site.

The service requires Rust 1.88 or newer and a locked dependency build.
Production enrollment verifies PKCS#10/P-256 proof of possession, seals the
companion secret with RFC 9180 HPKE, wraps its at-rest key through a configured
KMS provider, and obtains the client certificate through a configured CA
provider. The default production build supports a TPM-delivered AES-256-GCM
wrapping key and P-256 CA; `aws-kms` and `aws-private-ca` remain
supported feature-gated alternatives. No enrollment response or database
column contains the plaintext companion secret.

## Security boundaries

- Identity is delegated to one configured OpenID Connect issuer. The reference
  deployment self-hosts that issuer with Keycloak, but it remains a separate
  password/identity boundary; the backend has no password database and does
  not mint user access tokens.
- Native apps use OAuth 2.1 Authorization Code with PKCE and present
  short-lived OIDC access tokens as Bearer credentials.
- The companion Web UI uses a backend-for-frontend flow. Authorization Code +
  PKCE + state + nonce terminate here. A random opaque
  `__Host-kitsu_session` cookie is `Secure`, `HttpOnly`, and `SameSite=Lax`;
  PostgreSQL stores only its SHA-256 digest.
- Browser mutations require both an exact configured `Origin` and a matching
  `X-CSRF-Token`. The session endpoint returns the token only after it reads
  the raw host-only CSRF cookie and constant-time verifies its digest against
  the authenticated server-side session. The session cookie remains
  `HttpOnly`.
- Credentialed CORS is an exact allowlist for the authenticated companion Web
  UI. The public project/flasher site is a separate origin and must never be
  added. Browser WebSockets use an exact-origin check and a one-use,
  30-second ticket.
- Gateways connect through a dedicated mTLS ingress. The ingress validates the
  gateway CA, strips untrusted identity headers, and injects certificate
  attributes. The backend accepts those headers only from configured trusted
  proxy CIDRs.
- A native mobile relay authenticates only as its human owner with the existing
  OIDC Bearer token. Its installation UUID is immutably bound to an active
  logical gateway UUID; it receives no gateway certificate or device secret.
  Device enrollment proof and signed-envelope HMAC verification are identical
  to the PC gateway path.
- A fresh gateway cannot self-register through those trusted headers. An owner
  first creates a short-lived one-use bootstrap. The PC submits a signed P-256
  CSR with that bearer token, receives a certificate whose SAN is the
  backend-assigned gateway UUID, and only then can establish an mTLS session.
- Companion HMAC secrets are 32 random bytes. The selected KMS provider wraps
  the per-record DEK;
  AES-256-GCM encrypts the companion secret with AAD binding companion UUID,
  key version, and crypto version. Plaintext data keys and companion secrets
  are zeroized and are never logged.
- Signed device envelopes bind the companion UUID, stable gateway UUID,
  sequence, request UUID, key version, type, and exact payload bytes. HMAC is
  checked before payload parsing. Sequence advancement and all derived rows
  commit in one PostgreSQL transaction, so parse or persistence failure does
  not burn a sequence.
- Remote actions are signed end to end by the backend with the companion
  secret. A gateway routes the exact opaque action bytes and therefore cannot
  forge or alter a companion command. PC mTLS or mobile owner OIDC
  authenticates the routing session; neither is device authorization.

PostgreSQL `LISTEN/NOTIFY` is only a low-latency wake-up for the service
instance that currently owns a gateway socket. Actions remain durable in
tables and are polled at connection time and every ten seconds. A lost notify
cannot lose an action. NATS JetStream is deliberately omitted until scale or
cross-region routing justifies another durable system.

## Local setup

Requirements:

- Rust 1.88 (the committed lockfile is part of the build contract);
- PostgreSQL 16 or newer;
- an OIDC issuer/client registration (a separate local Keycloak is supported);
- either a 32-byte local wrapping key plus a P-256 client CA, both loaded from
  protected credential files, or the optional AWS KMS/Private CA providers.

Copy `.env.example` to `.env` and replace every placeholder. The server runs
SQLx migrations during startup:

```text
cargo run --locked --release
```

On the Windows workspace used for this project, use the workspace-local GNU
toolchain from `work/wisp868`:

```powershell
$env:RUSTUP_HOME=(Resolve-Path '..\.toolchains\rustup').Path
$env:CARGO_HOME=(Resolve-Path '..\.toolchains\cargo').Path
$env:PATH='C:\Ruby33-x64\msys64\ucrt64\bin;'+$env:PATH
& "$env:CARGO_HOME\bin\rustup.exe" run 1.88.0-x86_64-pc-windows-gnu cargo test --manifest-path platform\backend\Cargo.toml --locked
```

The public listener serves `/health/live`, OIDC metadata, owner/BFF APIs, and
the trusted-mTLS-proxy gateway routes. The separately bound operations
listener serves `/health/ready` and `/metrics`; do not expose it publicly.

## API map

Owner routes accept either a valid native-app Bearer token or a browser BFF
session. Browser POST routes additionally require `Origin` and
`X-CSRF-Token`. Mobile-relay routes are native-only and reject browser
sessions.

| Method and path | Purpose |
| --- | --- |
| `GET /v1/auth/config` | Public OIDC/native-client discovery values. |
| `GET /v1/browser/auth/start?return_url=...` | Begin browser Code + PKCE flow. |
| `GET /v1/browser/auth/callback` | OIDC callback; creates opaque BFF session. |
| `GET /v1/browser/session` | Return owner/session expiry and verified `csrf_token`. |
| `POST /v1/browser/logout` | Revoke session and clear host-only cookies. |
| `POST /v1/browser/ws-ticket` | Mint one-use 30-second browser WS ticket. |
| `GET /v1/browser/ws?ticket=...` | Read-only owner update WebSocket. |
| `GET /v1/companions` | List the authenticated owner's companions. |
| `GET /v1/companions/{id}/snapshot` | Initial Web/native projection with ETag. |
| `GET /v1/companions/{id}/peers` | Durable latest peer summaries. |
| `GET /v1/companions/{id}/events?after={cursor}&limit={n}` | Ordered durable event stream. |
| `GET /v1/companions/{id}/actions` | Recent durable actions and state. |
| `POST /v1/companions/{id}/actions` | Create an idempotent signed action. |
| `POST /v1/enrollments` | Create a short-lived physical enrollment claim. |
| `POST /v1/gateway-bootstraps` | Owner-authorize a fresh PC gateway and return its token once. |
| `POST /v1/gateway-bootstraps/{id}/claim` | Claim a first gateway mTLS identity with a signed P-256 CSR; no prior mTLS. |
| `POST /v1/gateways/{id}/certificate-rotations` | Authorize replacement gateway certificate. |
| `POST /v1/gateways/{gateway_id}/certificates/{certificate_id}/revoke` | Revoke a proven certificate. |
| `POST /v1/gateway/enrollments/{id}/claim` | Verify device proof and return cert plus device-only HPKE-sealed secret. |
| `POST /v1/gateway/certificate-rotations/{id}/activate` | Prove replacement certificate over mTLS. |
| `POST /v1/gateway/envelopes` | Upload an untouched device-signed envelope. |
| `GET /v1/gateway/session` | Durable backend-to-gateway action WebSocket. |
| `PUT /v1/mobile-relays/{installation_id}` | Idempotently create an owner-bound mobile relay and logical gateway. |
| `GET /v1/mobile-relays/{installation_id}` | Get that owner's relay binding. |
| `POST /v1/mobile-relays/{installation_id}/enrollments/{enrollment_id}/claim` | Relay the exact existing device enrollment claim. |
| `POST /v1/mobile-relays/{installation_id}/envelopes` | Upload an untouched device envelope with its spool-record header. |
| `GET /v1/mobile-relays/{installation_id}/session` | Owner-authenticated action WebSocket using exact gateway action frames. |

The native app chooses non-nil installation and logical gateway UUIDs. Create
the immutable binding with:

```http
PUT /v1/mobile-relays/aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa
Authorization: Bearer <OIDC access token>
Content-Type: application/json

{"gateway_id":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"}
```

Create and get return the same shape (with the original creation time):

```json
{
  "installation_id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
  "gateway_id": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
  "created_at": "2026-08-21T12:00:00Z",
  "ca_cert_der_b64": "canonical-unpadded-base64url-current-device-signing-ca-der"
}
```

The first PUT atomically creates the active logical gateway when the owner has
no PC gateway. Repeating the same tuple is idempotent; an attempted rebind is
`409`, and another owner's installation or gateway is `404`. Enrollment claim
bodies/responses, envelope bodies/responses, and WebSocket action frames are
byte-for-byte the existing gateway contracts. Mobile envelope uploads require
the same single canonical `X-Kitsu-Spool-Record-Id` decimal `u64` header. The
relay CA field is the current companion/device-signing CA certificate for the
pre-enrollment device trust handoff; it is not the backend HTTPS CA or a
gateway/client credential.

`GET /v1/companions/{id}/snapshot` returns:

```json
{
  "companion": {"id":"...","hardware_uid":"...","display_name":"...","status":"active","created_at":"..."},
  "vitals": {},
  "mood": {},
  "bond": {},
  "evolution": {},
  "connectivity": {
    "online": true,
    "provenance": "gateway_mtls_device_hmac",
    "gateway_id": "...",
    "last_seen_at": "...",
    "gateway_last_proof_at": "..."
  },
  "mesh": {},
  "counts": {"peers": 0, "messages": 0, "unread_messages": 0},
  "recent_events": [],
  "cursor": "0"
}
```

The `mesh` projection carries Mesh profile/TX state supplied by signed state
events. `cursor` is a canonical decimal PostgreSQL event cursor. Responses
include an ETag over the canonical projection, accept `If-None-Match`, use
`Cache-Control: private, max-age=0, must-revalidate`, and return `304` when
unchanged.

## Device-to-backend envelope

The gateway forwards this JSON object unchanged:

```json
{
  "schema": "kitsu.device-envelope.v1",
  "companion_id": "00112233-4455-6677-8899-aabbccddeeff",
  "gateway_id": "ffeeddcc-bbaa-9988-7766-554433221100",
  "sequence": "42",
  "issued_epoch": "0",
  "nonce_b64": "base64url-16-bytes",
  "request_id": "00000000-0000-0000-0000-000000000001",
  "key_version": 1,
  "payload_type": "peer_snapshot",
  "payload_b64": "base64url-exact-utf8-json-bytes",
  "signature_b64": "base64url-32-byte-hmac"
}
```

All `_b64` fields are canonical unpadded base64url. UUID bytes are RFC 4122
network order. `sequence` is canonical decimal `1..i64::MAX`, encoded as
`u64be` in the transcript. `issued_epoch` is canonical decimal `0` when the
device has no valid clock, otherwise it is 2020-01-01 through 2100-01-01
inclusive; it is advisory and never replay authority. `payload_type` is
lowercase protocol ASCII, at most 64 bytes. The backend accepts at most
256 KiB decoded payload bytes.

The HMAC-SHA-256 transcript is:

```text
"KITSU-DEVICE-1\0"
|| companion_uuid16
|| gateway_uuid16
|| u64be(sequence)
|| i64be(issued_epoch)
|| nonce16
|| request_uuid16
|| u32be(key_version)
|| u16be(payload_type_utf8_length) || payload_type_utf8
|| u32be(payload_length) || exact_payload_bytes
```

Upload with:

```text
POST /v1/gateway/envelopes
X-Kitsu-Spool-Record-Id: <canonical u64 decimal>
Content-Type: application/json
```

Only after the PostgreSQL transaction commits, the service returns exactly:

```json
{"accepted":true,"spool_record_id":"<same value>","sequence":"<device sequence>"}
```

An exact retry of a committed `(companion_id, request_id, sequence,
transcript)` is accepted idempotently and returns the same response contract.
Reusing a request UUID with different signed content is a conflict. A
decreasing/reused sequence that is not that exact committed request is a
replay. Invalid authenticated JSON rolls back and leaves the prior sequence
available for a corrected retry.

## Sealed physical enrollment

The owner creates an enrollment with `POST /v1/enrollments`; the one-time
256-bit claim token is returned once and only its SHA-256 digest is stored.
The companion generates a P-256 signing key/CSR and an HPKE P-256 recipient
key. The gateway is only a relay.

The frozen claim request is:

```json
{
  "claim_token": "one-time-token",
  "hardware_uid": "KITSU868-...",
  "device_csr_der_b64": "canonical-unpadded-base64url-csr-der",
  "hpke_recipient_b64": "canonical-unpadded-base64url-65-byte-uncompressed-p256-point",
  "device_nonce_b64": "canonical-unpadded-base64url-16-bytes",
  "device_proof_b64": "canonical-unpadded-base64url-64-byte-ieee-p1363-ecdsa"
}
```

The device proof is ECDSA P-256/SHA-256 over this transcript:

```text
"KITSU-ENROLL-DEVICE-1\0"
|| enrollment_uuid16
|| u16be(hardware_uid_utf8_length) || hardware_uid_utf8
|| SHA256(csr_der)
|| hpke_recipient_uncompressed_point65
|| device_nonce16
```

The production success response is frozen as:

```json
{
  "companion_id": "uuid",
  "gateway_id": "uuid",
  "key_version": 1,
  "device_certificate_der_b64": "base64url-leaf-der",
  "device_certificate_chain_der_b64": ["base64url-issuer-der"],
  "sealed_secret": {
    "suite": "DHKEM(P-256,HKDF-SHA256)/HKDF-SHA256/AES-256-GCM",
    "enc_b64": "base64url-65-byte-enc",
    "ciphertext_b64": "base64url-48-byte-ciphertext-and-tag"
  }
}
```

The HPKE suite IDs are KEM `0x0010`, KDF `0x0001`, and AEAD `0x0002`.
The 32-byte companion HMAC secret is the complete HPKE plaintext—there is no
JSON and no gateway-readable LAN key. For both HPKE `info` and AEAD `aad`, use
the identical 74-byte context:

```text
"KITSU-ENROLL-SECRET-1\0"
|| enrollment_uuid16
|| companion_uuid16
|| gateway_uuid16
|| u32be(key_version)
```

The issued leaf certificate SAN URI is exactly
`urn:kitsu:companion:<lowercase-uuid>`. The backend verifies the DER PKCS#10
signature, requires an unextended P-256 CSR, verifies the 64-byte IEEE-P1363
device proof with the CSR key, and verifies the returned certificate's key,
SAN, client-auth usage, leaf validity, adjacent chain signatures, serial, and
fingerprint before committing it. Full PKIX path validation—including issuer
validity, CA constraints, revocation, and anchoring to the deployed CA—is a
mandatory responsibility of the mTLS ingress/PKI operator. The enrollment row
reserves a stable companion UUID while issuance is in progress. Before calling
the CA, the service durably marks the provider boundary and persists the
provider job identity. The local CA writes a deterministic issuance job
atomically before returning and resumes it byte-for-byte after restart. The
AWS provider persists the certificate ARN and resumes `GetCertificate`; its
narrow pre-ARN ambiguity uses the same AWS idempotency token only inside the
backend's conservative 255-second window. Outside a provider's safe replay
contract the row fails closed rather than risking a second valid certificate.
The backend returns HTTP `409` with stable error code `replacement_required`,
and the owner must create a new one-use enrollment challenge.
Exact completed token/request retries return the same persisted certificate
and HPKE bytes, while the same token with different request material is
rejected. A claim that crossed the provider boundary while valid remains
resumable after its bearer-token deadline, so a slow CA cannot orphan an
already-issued certificate.

The RFC 9180 fixture in `src/pki.rs` covers UUID network byte order, the exact
74-byte `info`/`aad`, deterministic ephemeral test key material, `enc`, the
32-byte plaintext, and ciphertext/tag. Production encapsulation always uses
the operating-system CSPRNG. The local issuer enforces P-256 CA/signing-key
matching, emits only client-auth leaves with the exact backend-assigned URI
SAN, and atomically publishes a complete CRL across the current and retained
rotation identities. The AWS PCA implementation requires an
ECDSA-keyed Private CA compatible with `SHA256WITHECDSA` and uses the documented
`EndEntityClientAuthCertificate_APIPassthrough/V1` template; the configured
ARN remains explicit so a deployment cannot silently fall back to a template
that ignores the backend-assigned SAN. Its runtime role also needs
`acm-pca:GetCertificateAuthorityCertificate`; startup retrieves and freezes
the current signing CA for the authenticated mobile pre-enrollment handoff.

## First gateway bootstrap

The owner creates a one-use bootstrap over native OIDC Bearer auth or the
browser BFF session:

```http
POST /v1/gateway-bootstraps
Content-Type: application/json

{"display_name":"Home PC"}
```

The `201` response returns `{bootstrap,claim_token}`. Only SHA-256 of the
256-bit token is stored. Before it has any client certificate, the fresh PC
generates and retains a P-256 private key, then claims:

```http
POST /v1/gateway-bootstraps/{bootstrap_uuid}/claim
Content-Type: application/json

{"claim_token":"...","gateway_csr_der_b64":"canonical-unpadded-base64url-DER"}
```

The CSR signature is the proof of private-key possession. The backend assigns
the gateway UUID; it never trusts an identity requested by the CSR. The
response is:

```json
{
  "gateway_id": "uuid",
  "device_certificate_der_b64": "base64url-leaf-der",
  "device_certificate_chain_der_b64": ["base64url-issuer-der"]
}
```

The SAN is exactly `urn:kitsu:gateway:<lowercase-uuid>`. The token is bound to
the CSR digest on first valid use. Certificate-provider jobs use the same
durable resume and conservative ambiguous-window rules as companion
enrollment. An exact completed retry returns the same persisted certificate
bytes; a different CSR conflicts. If an unobserved provider call is
quarantined after its idempotency window, the owner creates a replacement
bootstrap. The PC stores its private key, gateway UUID, leaf, and chain, then
uses that identity for the mTLS gateway session and device enrollment relay.
The private key never enters this API.

## mTLS gateway certificate lifecycle

There are two deliberately separate ingress planes in a production deployment:

- Cloudflare Tunnel/public nginx exposes owner and browser routes plus the
  no-client-certificate `POST /v1/gateway-bootstraps/{id}/claim`. Public nginx
  hard-denies every `/v1/gateway/*` route and strips client-certificate/XFCC
  and internal proxy-auth headers.
- An unpublished local Envoy listener directly terminates gateway mTLS and is
  the only route to `/v1/gateway/*`. This machine path does not traverse
  Cloudflare.

The local Envoy contract is frozen as follows:

1. pin Envoy `1.38.0` or newer and validate the deployed configuration with
   `envoy --mode validate -c kitsu-gateway.yaml`; JSON-format XFCC is not
   available in older builds;
2. require a client certificate chaining to the configured ECDSA gateway CA
   and enforce certificate expiry plus the deployed CRL/OCSP policy;
3. use `forward_client_cert_details: SANITIZE_SET` so client-supplied XFCC is
   discarded;
4. use XFCC JSON format and
   `set_current_client_cert_details: { cert: true, uri: true }`;
5. strip a caller-supplied proxy-auth header, then inject the independent
   secret-manager-provisioned 256-bit value;
6. connect from only a narrow CIDR in `KITSU_TRUSTED_MTLS_PROXY_CIDRS`;
7. disable Envoy's admin interface, or isolate it behind a permissioned local
   socket with config-dump unavailable, because static config contains the
   proxy credential; and
8. redact XFCC, proxy-auth, and claim-token values from every access log.

The backend accepts one JSON XFCC array containing exactly one record with
Envoy's lowercase SHA-256 `hash`, one PEM `cert`, and singleton `uri`. It parses
the leaf DER,
recomputes and constant-time checks the fingerprint, derives the singleton
Kitsu URI SAN and `notBefore`/`notAfter` locally, checks current validity, and
then requires the fingerprint/URI pair to match the durable gateway
certificate row. It does not trust forwarded date, fingerprint, or SAN scalar
headers. Source CIDR alone is not authentication: the proxy token is also
mandatory, and the backend listener must be isolated from unrelated local
processes by a private container/network namespace or host ACL.

For `k32.run`, `https://api.k32.run` is the configurable public API/BFF origin
and `https://app.k32.run` is the exact authenticated Web UI origin. The public
edge must separately rate-limit bootstrap claims; behind a tunnel, the
backend's source-IP bucket is intentionally only defense in depth. Localhost
domains remain configurable for development.

A gateway UUID is a stable logical identity, independent of any certificate.
For rotation, the owner creates a short-lived one-use rotation claim, the new
certificate activates it by proving possession over mTLS, and the backend
registers its fingerprint. The new certificate must keep the exact stable
gateway URI SAN. Both certificates are accepted only for the bounded
configured overlap. The old fingerprint is denied after that deadline.
The configured `KITSU_MTLS_XFCC_HEADER` is therefore mandatory for gateway
requests. Explicit owner revocation is immediate but refuses to revoke the
gateway's only proven active certificate; CA/CRL distribution remains the PKI
operator's responsibility.

## Backend-to-device remote actions

Create an action with a unique `Idempotency-Key` header (8..128 safe ASCII
characters) and:

```json
{
  "action_type": "companion.pet",
  "parameters": {},
  "expires_in_seconds": 60
}
```

The supported action schemas are closed; unknown keys are rejected:

| `action_type` | Exact parameters |
| --- | --- |
| `companion.pet` | `{}` |
| `companion.feed` | `{}` |
| `companion.play` | `{}` |
| `companion.listen_once` | `{"duration_ms":1000..60000}` |
| `sync.pull` | `{}` |
| `clock.set` | `{"epoch":1577836800..4102444800}` |
| `mesh.introduce` | `{"scope":"nearby"}` or `{"scope":"mesh"}` |
| `message.send` | `{"route":"direct|channel","target":"1..128 safe UTF-8 bytes","text":"1..128 UTF-8 bytes"}` |

The backend canonicalizes accepted parameters once with JCS, signs and stores
those exact bytes, and sends this JSON text frame directly on
`GET /v1/gateway/session`:

```json
{
  "schema": "kitsu.remote-action.v1",
  "action_id": "uuid",
  "companion_id": "uuid",
  "key_version": 1,
  "nonce_b64": "base64url-16-bytes",
  "action_type": "companion.pet",
  "created_epoch": "1800000000",
  "expires_epoch": "1800000060",
  "params_b64": "e30",
  "signature_b64": "base64url-32-byte-hmac"
}
```

`created_epoch` and `expires_epoch` are canonical decimal JSON strings but
`i64be` in the transcript. The signature is HMAC-SHA-256 over:

```text
"KITSU-ACTION-1\0"
|| action_uuid16
|| companion_uuid16
|| u32be(key_version)
|| nonce16
|| i64be(created_epoch)
|| i64be(expires_epoch)
|| u16be(action_type_utf8_length) || action_type_utf8
|| u32be(params_length) || exact_params_bytes
```

The gateway must treat the frame as opaque, route using the certificate-bound
companion identity, retain/retry exact bytes, and deduplicate only by
`action_id`. A successful WebSocket write records an
`action_delivery_attempts` row but **does not** change action state:

```text
queued --device-signed action_acceptance--> delivered
queued|delivered|expired --device-signed action_result--> succeeded|failed|rejected
queued|delivered --server expiry--> expired
```

`action_acceptance` is transport acceptance only; `action_result` is
authoritative. Both arrive through the normal HMAC device envelope upload, so
their state transition commits with replay protection. Until device
acceptance or result, a queued action remains eligible for durable redelivery.

## Persistence and operations

The migrations define owners, BFF attempts/sessions/tickets,
companions, versioned encrypted secrets, stable gateways and certificate
bindings, enrollments, replay state, idempotent device requests, durable
events, state projections, peer history, actions/delivery attempts,
append-only audit rows, rate-limit buckets, production companion-certificate
lifecycle, exact enrollment retry material, and one-use gateway bootstraps.
Database checks reinforce wire bounds. Audit UPDATE/DELETE is rejected by a
PostgreSQL trigger.

Structured tracing intentionally excludes request and response bodies. The
operations listener exposes readiness and Prometheus metrics. Deployment must
protect that listener and configure log retention without recording tokens,
proofs, private keys, plaintext secrets, cookies, or message bodies.

## Verification

Run:

```text
cargo fmt --all -- --check
cargo test --locked --all-features
cargo clippy --locked --all-targets --all-features -- -D warnings
```

Pure tests cover fixed transcript layout, RFC 4122 UUID byte order, known
remote-action HMAC output, exact-parameter sensitivity, expiry, byte-identical
retry, strict care/message/introduce schemas, canonical base64url, encrypted
secret identity/version binding, canonical non-nil UUID wrappers, trusted
mTLS-header behavior, PKCS#10 signature validation, certificate policy/chain
validation, the published device-proof signature, and the RFC 9180 HPKE
fixture. `tests/postgres_integration.rs` runs when `DATABASE_URL` is set; it
uses an isolated random schema and covers migration/reconnect durability,
owner-isolated one-use gateway bootstrap and companion enrollment, persisted
provider-job resume, bounded ambiguous-provider recovery, expiry after a valid
reservation, exact replay and different-request conflict, certificate-bound
gateway lookup, concurrent device-envelope replay, action idempotency, owner
isolation, and the append-only audit trigger. Platform CI supplies PostgreSQL
16 for that suite.

Deployment still requires operator configuration—not missing application
code: a reachable PostgreSQL 16+ database; an OIDC issuer plus native-PKCE and
browser-BFF client registrations; either protected local KMS/CA key material
with rotation/CRL distribution or the optional least-privilege AWS KMS/PCA
providers; public Cloudflare Tunnel/nginx routing for
`https://api.k32.run` that strips internal headers and hard-denies
`/v1/gateway/*`; and the separate private Envoy gateway listener described
above, with direct client-certificate validation, revocation enforcement,
network isolation, and secret-manager-injected proxy authentication.
