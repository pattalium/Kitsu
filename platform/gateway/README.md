# Kitsu Home Gateway

The default gateway is the owner-operated PC service intended to sit between a
Kitsu device on the home Wi-Fi network and the hosted Kitsu backend. It is not
the companion Web UI and it is not a radio implementation. The same binary can
also run an explicitly selected, bounded public deployment scope, but a public
instance is a separate deployment and never turns an owner's private instance
into shared infrastructure.

This document specifies the gateway service's production contract; it is not
evidence that the current physical Heltec has established that path. Firmware
`main.cpp` now instantiates authenticated-BLE/PRG owner enrollment, private
bootstrap and credential install, steady TLS/mTLS, queue/ACK processing,
durable action replay, heartbeat, and the authenticated action bridge. Those
pieces are part of the owner-reflashable release contract.

Accordingly, configured gateway trust without an enrollment reports
`enrollment_pending`. After enrollment, ordinary Wi-Fi, trusted-time, replay,
TLS/mTLS, and backend gates apply. The release reports the reflashable physical
security limit instead of requiring eFuses or returning `security_blocked`.
Direct authenticated BLE remains the preferred nearby device/app path.

## Private owner instance and public instance

`private` is the default deployment scope. Its safe code defaults bind ports
7442/7443 to loopback. A usable owner instance must explicitly bind each port to
one RFC 1918 IPv4, IPv4/IPv6 link-local, IPv6 unique-local, or loopback address
owned by the gateway host. Validation rejects wildcard, multicast,
documentation, and globally routable listener addresses. mDNS discovery is
available, and the owner controls its gateway identity, certificates, and WAL.

That address validation cannot detect router port forwarding, one-to-one NAT,
host firewall mistakes, a forwarding proxy, or later routing changes. The
owner must still enforce trusted-LAN-only ingress in the host firewall and edge
router. Publishing a private instance through DNS, NAT, port forwarding, or a
proxy is unsupported even when the process accepted its LAN bind address.

`public` is opt-in with `--deployment-scope public` or
`KITSU_DEPLOYMENT_SCOPE=public`. Start from `.env.public.example`, not a copied
private environment. Public validation fails closed unless mDNS is disabled,
the admin listener is loopback, and `KITSU_PUBLIC_INSTANCE_MANIFEST` names a
strict, bounded instance manifest. Public listener addresses must be configured
deliberately; wildcard listeners are allowed only because the public host's
firewall and TCP ingress are part of the required security boundary.

A public process must have its own service unit or container, gateway ID,
backend client identity, server identity, device CA policy, spool directory,
and listener addresses. Only ports 7442/7443 belong at public ingress; port 7444
remains loopback-only and should be reached through an authenticated operations
channel such as SSH.

The public instance manifest is at most 16 KiB, must be a regular non-symlink
file, and rejects duplicate, missing, or unknown JSON fields. It has this exact
schema:

```json
{
  "schema": "kitsu.public-gateway-instance.v1",
  "scope": "public",
  "gateway_id": "00000000-0000-0000-0000-000000000000",
  "canonical_spool_root": "/var/lib/kitsu/public-gateway/spool",
  "backend_origin": "https://api.example.invalid/",
  "server_certificate_sha256": "<64 lowercase hex characters>",
  "device_ca_sha256": "<64 lowercase hex characters>",
  "gateway_client_identity_sha256": "<64 lowercase hex characters>"
}
```

The nil UUID above is deliberately invalid and must be replaced with this
public instance's separately provisioned gateway UUID. Startup rejects it.

`canonical_spool_root` is the operating system's absolute canonical path to an
already provisioned real directory; the spool directory itself may not be a
symlink. `backend_origin` must exactly match the normalized HTTPS origin,
including its trailing slash. Each SHA-256 value hashes the exact bytes of the
configured PEM file—not a displayed certificate fingerprint and not decoded
DER. Generate the manifest only after provisioning the distinct public files
and spool root. Any path, origin, UUID, scope, or digest mismatch aborts startup.
The manifest contains fingerprints and identifiers, never private-key bytes.

Every spool also contains strict identity metadata bound to its deployment
scope and gateway UUID. The process holds an exclusive operating-system lock on
the spool for its complete lifetime. A mismatched identity or concurrent second
process fails startup instead of sharing WAL state. When public identity
metadata is absent, initialization is allowed only in a truly empty,
pre-provisioned spool directory; legacy state, WAL, or any other content aborts
startup. Private scope alone may migrate an unidentified legacy spool containing
only the recognized state slots and canonical WAL segment names, after the
ordinary WAL validation succeeds.

Both listener classes have process-wide admission caps. The defaults are four
bootstrap sessions and 256 steady-device sessions; configure them with
`KITSU_BOOTSTRAP_CONCURRENCY_LIMIT` (1..128) and
`KITSU_STEADY_CONCURRENCY_LIMIT` (1..4096). A permit covers the TLS handshake
and the complete session. When no permit is available, the newly accepted TCP
connection is closed instead of being queued or spawned, so untrusted peers
cannot create unbounded handshake or session tasks. Capacity warnings are
coalesced to avoid rejection-log floods.

These process caps do not prevent an attacker from filling every handshake slot
for one timeout window. Public deployment therefore requires upstream and host
firewall controls that bound new connections and per-source connection rates,
plus provider-level denial-of-service protection. Those controls are mandatory,
not optional substitutes for application authentication.

A normal Cloudflare Tunnel hostname is not transparent raw-TCP ingress usable
by the Heltec firmware, so ordinary Cloudflare Tunnel is unsupported for these
listeners. A public instance requires Cloudflare Spectrum configured for raw
TCP/TLS passthrough, a hardened TCP-forwarding VPS, or direct public TCP/NAT
protected by mandatory host/edge per-source rate limits and denial-of-service
controls. The ingress must preserve the gateway's end-to-end TLS bytes: port
7442 remains pinned server-authenticated TLS and port 7443 remains
device-certificate mTLS. An HTTP reverse proxy, an ordinary proxied DNS record,
or TLS termination in front of the gateway does not satisfy that contract.

The public runtime mode and validation do not provision DNS, Cloudflare,
firewalls, certificates, backend ownership, rate limits, denial-of-service
protection, or a public endpoint. Those are deployment prerequisites and must be
verified independently before devices are routed to the public instance.

Security and durability rules:

- An owner-enrolled Heltec initiates a mutually authenticated TLS
  connection to port 7443.
  A device certificate must chain to the configured device CA. Discovery over
  `_kitsu-gw._tcp.local.` proposes an address only; it never supplies trust.
- The verified leaf certificate must contain exactly one canonical URI SAN in
  the form `urn:kitsu:companion:<lowercase UUID>`. Every envelope's companion
  UUID must match it. A self-asserted JSON identifier is never a routing
  identity.
- A fresh Heltec that does not yet own a client certificate uses the separate
  port 7442 enrollment listener. That listener is server-authenticated only,
  and firmware must pin the CA and SPKI provisioned over authenticated BLE.
  It accepts one bounded request per connection, relays the exact CSR/proof
  claim through the gateway's backend mTLS identity, validates that the reply
  contains only a certificate chain and RFC 9180 sealed 32-byte secret, and
  then closes. It cannot decrypt the result. Steady traffic is never accepted
  on this bootstrap listener.
- The catalog keeps the routing endpoint separate from the TLS server name.
  A deployment may use a reserved LAN address for routing while authenticating
  a DNS server name. Firmware must never substitute a numeric route as SNI.
- Signed device envelopes remain byte-for-byte unchanged. The gateway checks
  strict public bounds, writes the opaque bytes to an append-only CRC32C WAL,
  calls `sync_data`, and only then acknowledges the Heltec.
- The companion HMAC secret is never provisioned to or decrypted by this
  process. The backend is the end-to-end verifier.
- WAL deletion happens only after the backend commits and echoes the exact
  gateway spool record ID. A generic HTTP success is insufficient.
- The gateway keeps a second, mutually authenticated `wss://` session to
  `/v1/gateway/session` for backend-to-device actions. Each server text frame
  is the complete `kitsu.remote-action.v1` object. The gateway validates only
  its strict public routing shape and passes the original UTF-8 bytes to the
  certificate-bound companion; it cannot verify or recreate the end-to-end
  HMAC.
- An action is not considered delivered merely because the WebSocket or LAN
  write succeeded. The backend leaves it queued until the device uploads a
  signed `action_acceptance` or `action_result` envelope through the WAL path.
  Exact action-ID retries are suppressed per live device session, while a
  conflicting reuse of an action ID tears down the backend action session.
- There is no SQLite database. The segmented WAL repairs only a partial tail
  in the newest segment; checksum corruption fails startup closed.
- The operations listener is restricted to loopback. It exposes health and
  queue depth, the selected deployment scope, separate upload/action-session
  connectivity, and the number of live device sessions, but no secret, message
  body, Wi-Fi credential, or signed envelope.
- Private keys and certificates are supplied as files by the installer or OS
  service account. Production installers must apply owner-only ACLs and use
  the operating system certificate/key facility where available. Secrets are
  never accepted on command-line flags.

The backend URL and certificates are deliberately configuration. In the default
private scope, device ports 7442/7443 remain LAN listeners and must never be
published. In the distinct public scope they are raw TLS ingress, never web
origins. The backend enrollment route validates the sealed CSR/HPKE request. A
production deployment must configure the backend certificate issuer; the
gateway never receives a plaintext companion secret.

## First gateway identity

`kitsu-gateway-bootstrap` implements the owner-authorized PC bootstrap. After
the authenticated owner API returns a bootstrap UUID and one-use 32-byte
base64url claim, place the claim alone in a root-readable file and run:

```text
kitsu-gateway-bootstrap \
  --backend-url https://api.k32.run \
  --bootstrap-id <canonical-uuid> \
  --claim-token-file /run/kitsu-gateway-bootstrap/claim \
  --output-dir /etc/kitsu/gateway
```

The claim is never accepted as a command-line value. The tool creates and
durably retains the P-256 key before its first request, so a network retry uses
the exact CSR; it validates the returned key match, exact
`urn:kitsu:gateway:<uuid>` URI SAN, client-auth profile, and adjacent chain
signatures before atomically publishing `gateway-client-identity.pem`,
`gateway-client-ca.pem`, and `gateway-id`. It refuses to replace an existing
identity. The service must remain disabled until those files and the separate
LAN server identity are installed.

The distinct device operation is implemented too: authenticated BLE begin
stages the backend-issued claim, PRG confirms it, finish creates the device
CSR/HPKE proof, and the intentional disconnect permits the 7442 exchange and
credential install. The gateway only relays the exact claim and cannot open
the sealed companion secret. The owner-reflashable release permits this flow
without an eFuse gate; its prerequisites are authenticated ownership, physical
confirmation, stored configuration, trusted time, and the strict TLS/bootstrap
checks above.

## Development checks

Use the pinned Rust 1.88.0 toolchain:

```text
cargo fmt --check
cargo clippy --all-targets -- -D warnings
cargo test
```

Do not point a development instance at a production companion CA or backend.
