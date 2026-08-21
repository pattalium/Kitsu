# Kitsu Android physical release acceptance

This checklist is a mandatory release gate for the signed Android APK and the
exact owner-reflashable Heltec firmware named in the release record. Emulator,
unit, lint, and host firmware tests do not replace it. Never burn eFuses during
this procedure.

## Frozen evidence identity

Record all of the following before testing:

- APK path, byte length, SHA-256, package name, version code/name, signing
  certificate SHA-256, source-provenance tree SHA-256, and build timestamp.
- Firmware binary/bundle SHA-256, build environment, firmware version, and
  flash-offset manifest.
- Android manufacturer/model, OS build, API level, Bluetooth chipset if known,
  and whether battery optimization is enabled for Kitsu.
- Heltec hardware revision, device UID, configured gateway UUID, gateway host,
  bootstrap port `7442`, steady mTLS port `7443`, and the exact gateway
  `/health/live` URI (direct HTTPS route or explicit loopback operations tunnel).
- Backend/gateway release identifiers and the test start/end time in UTC.

Use `qa/capture-physical-acceptance.ps1` to create the Android-side evidence
directory. It requires explicit paths for `adb`, `apksigner`, and `aapt`, the
signed APK, source-provenance manifest, firmware release metadata and bundle,
and the frozen backend/gateway identities. It rejects emulator serials and
Android targets reporting QEMU, Goldfish, or Ranchu hardware. It also verifies
the package, version, and signing-certificate digest before creating the
write-once-by-tool `acceptance-record.json`. Before writing, it makes one
bounded, no-redirect request to the exact gateway health URI and requires a 2xx
healthy response carrying the expected gateway UUID, deployment scope, protocol,
and online backend state. The response body is used only transiently and is
never written. The exact backend HTTPS `/health/live` URI is derived from and
frozen with the backend base URL. The evidence directory must be outside and
disjoint from the source repository.

Do not include passwords, OAuth tokens, Wi-Fi passphrases, claim tokens,
controller roots, private keys, raw BLE envelopes, or authorization headers in
evidence. The tooling scans labelled text secrets, but that is a backstop—not
permission to capture raw protocol traffic or unreviewed logs.

## Evidence tooling

The tools use create-new files and never overwrite a completed record, but a
local directory is not immutable or tamper-proof. Keep it outside the clone,
restrict access, preserve the recorded hashes, and copy the completed evidence
to independently controlled retention storage. A failed or blocked attempt is
retained; a retry receives another timestamped attempt directory. Interrupted
pending attempt directories are ignored by finalization and may be retained for
forensics; only atomically completed attempt directories count as evidence.

1. Initialize an empty evidence directory with
   `capture-physical-acceptance.ps1`. Pass
   `-ExcludeMeshCorePhysicalProof` only when the release owner explicitly
   excludes MeshCore physical proof from this acceptance scope. Set
   `-GatewayExposure public` for the shared public gateway.
2. Record every attempt with `record-physical-case.ps1`. Strict UTF-8 text is
   secret-scanned. Binary evidence is limited to PNG, JPEG, GIF, WebP, MP4, and
   WebM with matching file magic and requires the explicit
   `-ConfirmAttachmentsSanitized` operator attestation. Unknown types, archives,
   APKs, firmware, key stores, certificates, and private keys are forbidden even
   when renamed. Case timestamps must follow initialization and cannot be in the
   future.
3. Run `watch-physical-reliability.ps1` for at least 24 real hours. It stores
   only device/app availability, battery/thermal readings, adapter enabled
   states, health status codes/latency, the non-secret gateway UUID/scope/protocol
   fields needed for identity verification, and optional bounded TCP
   reachability. It never stores a health response body, Wi-Fi identifiers,
   credentials, tokens, or BLE frames. The run is bound to the frozen acceptance
   digest, exact backend `/health/live` URI, and exact gateway-health URI.
   Android, app, backend, and identity-bound gateway health must each be at least
   99%, no unhealthy streak may exceed five
   samples, and every channel must end with at least five consecutive healthy
   observations. For the sampled TCP channel, that means its final five recorded
   TCP observations. Planned outages remain bounded by those thresholds and must
   be explained in a later `reliability-review` PASS linked with
   `-ReliabilityRunId <run-id>`; the recorder automatically binds that review to
   the exact sample-file and completion-file digests.
4. Run `finalize-physical-acceptance.ps1`. It validates case chronology,
   attachment hashes, ambiguous equal completion timestamps, unresolved
   severity-1/2 defect lifecycles, secret scans, sample
   continuity, both UTC and monotonic 24-hour duration, and the frozen artifact
   identities. It creates a new PASS or FAIL decision and never changes the
   initialization record; external retention is what makes that decision
   independently tamper-evident.

The following case IDs are mandatory for this Android/BLE/Wi-Fi/gateway gate:

- `install-upgrade`
- `install-fresh-second-device`
- `portrait-small-accessibility`
- `portrait-large-accessibility`
- `launcher-fox-head`
- `pairing-success`
- `pairing-negative`
- `ble-status-history`
- `ble-care-actions`
- `explicit-disconnect`
- `wifi-provisioning`
- `gateway-catalog`
- `owner-enrollment`
- `gateway-bootstrap-mtls`
- `backend-snapshot-binding`
- `remote-fallback`
- `authentication-lifecycle`
- `reliability-review`
- `reflash-recovery`
- `severity-review`

Public-gateway runs additionally require `public-gateway-perimeter`. Runs that
do not explicitly exclude MeshCore additionally require
`meshcore-advert-map`, `meshcore-direct-message`, `meshcore-channel-message`,
and `meshcore-repeater-interoperability`.

`qa/test-acceptance-harness.ps1` performs parser, secret-rejection,
write-once-record, chronology, attachment-magic, reparse/repository-boundary,
synthetic-duration rejection, gateway-identity, and fail-closed-finalizer
self-tests. Its instant, fully offline synthetic reliability fixture must be
rejected and can never become physical evidence.

## Acceptance sequence

Every item requires a recorded pass. A retry is a new run; do not erase the
first failure.

1. Install/upgrade integrity
   - Verify the APK digest and signing certificate before installation.
   - Upgrade from the currently published signed build without uninstalling.
   - Confirm Android accepts the same signing identity and retains only expected
     encrypted app state.
   - Fresh-install on a second supported Android device (API 26 or newer).
2. Portrait and accessibility
   - Run at the smallest supported portrait display with font scale 1.30.
   - Run on a modern large portrait display with font scale 1.15 or higher.
   - Verify Home, Chat, Mesh, Care, and More remain reachable.
   - Open the message keyboard and verify recipient, message, byte count, and
     Send control stay on-screen; test TalkBack focus/order and labels.
   - Verify the launcher uses the approved black-and-white fox-head master. The
     source asset SHA-256 must be
     `4f850b551e8fc242b0b31577ab76407cf1ade0e1a59bfaaf21edde3653b0ef42`.
3. First phone pairing
   - Erase only the app's prior bond/controller record and the device's intended
     test controller slot; do not erase firmware or unrelated device data.
   - Enter `Pair Phone` with PRG. Confirm Android and Heltec show the same
     six-digit numeric-comparison code before accepting.
   - Hold PRG for the separate controller grant only when `PHONE READY` appears.
   - Verify a wrong/rejected code, cancelled grant, timeout, and interrupted
     pairing leave no usable pending controller credential.
   - Reconnect using the committed controller after phone and Heltec reboots.
4. Direct BLE behavior
   - Read status, retained history, peers, channels, and messages.
   - Pet/feed/play and bounded listen work; malformed/expired actions fail.
   - Send direct text to a canonical 43-character base64url peer key.
   - Send channel text to known configured slot 0 and another configured slot.
   - Verify UTF-8 byte limit, duplicate action ID, delivery state, and unread
     state behavior.
5. Explicit Disconnect
   - Tap Disconnect during idle, scan, active GATT, event refresh, and enrollment.
   - Verify scanning, notifications, GATT, backend polling, and auto-reconnect
     stop. The system Bluetooth adapter may remain enabled.
   - Sign in, sign out, background/foreground, rotate system UI, force-stop,
     cold relaunch, kill/recreate the app process, and reboot the phone; none may
     clear Disconnect suppression. Only explicit Connect may clear it.
6. Wi-Fi and gateway provisioning
   - Store Wi-Fi only over authenticated BLE and verify inputs disappear after
     completion and are absent from screenshots/logs/cache.
   - Fetch the owner gateway catalog and verify exact UUID, host/SNI, CA, SPKI,
     bootstrap `7442`, and steady mTLS `7443` are transferred.
   - Reject equal ports, invalid CA/SPKI, IP-as-SNI, URL-as-host, uppercase UUID,
     and catalog records from an untrusted origin.
7. Owner enrollment
   - Require signed-in owner, configured Wi-Fi, configured v2 gateway record,
     and `remote_connectivity_allowed:true` before Start enables.
   - Create the one-use claim, confirm with PRG, and verify BLE finish is bound to
     the exact enrollment UUID.
   - Before physical acceptance is reported, verify `Stop and disconnect` closes
     BLE, suppresses reconnect/fallback, and leaves an unaccepted short-lived
     claim to expire. Exercise the race where PRG acceptance and Disconnect occur
     together; the app must report uncertainty and require an explicit reconnect
     to verify the device state.
   - After `ready_for_wifi` is accepted, verify no enrollment Cancel/Stop control
     remains. The UI must state that device bootstrap is committed and cannot be
     revoked by this frozen wire contract. Explicit Disconnect may stop app-side
     monitoring, but must not claim that it stopped the Heltec bootstrap.
   - Verify the app intentionally hands off from BLE to Wi-Fi.
   - Confirm bootstrap uses `7442`, installs the device identity, and steady
     traffic switches to mTLS `7443`.
   - Mark complete only when the backend snapshot identifies the same hardware
     UID and gateway UUID with `online:true`, provenance
     `gateway_mtls_device_hmac`, and a valid `last_seen_at`.
   - Repeat negative cases: wrong gateway, wrong companion, offline snapshot,
     stale/malformed timestamp, unproven provenance, expired claim, cancelled
     PRG, backend unavailable, and user Disconnect.
8. Remote fallback
   - With the known bonded Heltec genuinely out of BLE range, verify the app
     selects remote only after the bounded absence result.
   - With Heltec present but GATT/auth malformed, verify no silent remote gain.
   - Verify remote peer keys are canonical base64url even when the backend still
     carries an explicit legacy hex migration field.
   - Verify remote channel metadata is shown only when authenticated device
     snapshot data exists; an explicit slot 0..3 is labelled as device-validated
     when metadata is unknown.
   - Send remote direct and channel messages and confirm durable accepted,
     transmitted, delivered/failed, and duplicate/idempotency states.
9. Authentication lifecycle
   - Complete Authorization Code + PKCE against issuer
     `https://auth.k32.run/realms/kitsu`.
   - Verify token refresh, process restart, revoked refresh token, expired access
     token, offline issuer, sign-out local deletion, and best-effort RFC 7009
     revocation.
   - Confirm tokens and owner data never appear in logs or Android backup.
10. Reliability and recovery
    - Run at least 24 hours with BLE/Wi-Fi transitions, message traffic, screen
      off/on, Doze, phone reboot, Heltec reboot, gateway restart, WAN loss, and
      Cloudflare/API interruption.
    - Verify cache bounds, event cursor recovery, no cross-companion cursor/data
      leakage, no reconnect storm, and no sustained battery/thermal regression.
    - Reflash the documented owner recovery image over serial and confirm the
      board remains repurposable. Do not burn or lock any eFuse/interface.

## Release decision

Production is `PASS` only when all tests above identify the exact staged APK,
firmware, backend, and gateway artifacts and no unresolved severity-1 or
severity-2 defect remains. Anything else is a release candidate, not stable.
