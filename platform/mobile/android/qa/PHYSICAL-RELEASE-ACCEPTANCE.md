# Kitsu 0.20.3 physical release acceptance

This is the release gate for the public local-only Kitsu product. It has two
deliberately separate retained records:

1. a private candidate/hardware record covering the one-time `0.20.2` to
   `0.20.3` storage migration, dedicated restore, Android behavior, and signed
   Bluetooth OTA; and
2. a public-delivery smoke covering the exact Android APK and signed
   `.kitsu-fw` package downloaded by an ordinary owner.

The exact `kitsu.ble-firmware.v1` manifest has no physical-evidence field and
rejects extra fields. Record 1 is therefore never embedded into that manifest.
The separate final promotion record externally binds record 1, the exact final
manifest/signature/package, and record 2 without modifying any of them. Every
required case must pass against the exact frozen bytes. A partial run, a run
against a locally patched APK, or a run using a different application image is
not publication authorization.

Record 1 is a private, physically supervised release ceremony. Its dedicated
ROM-loader migration and restore tooling is not a product feature and is not
published as an ordinary owner journey. Record 2 uses only visible owner
controls and public HTTPS downloads. ADB, Docker, a server account, a gateway,
direct database repair, and hidden app controls are not part of either product
acceptance journey.

## Immutable historical boundary

The already-published, signed seven-write Web Serial release remains immutable
authority only for its accepted pre-`0.20.3` legacy rollback layout. Its
manifests, signatures, assets, documentation, evidence, and offsets must not be
edited, relabeled, or cited as current-layout acceptance.

That historical bundle is explicitly out of scope for this gate. It must never
install, migrate, repair, or recover `0.20.3`: it writes the partition table too
early, targets the legacy application slots, and clears storage that the
reviewed transition must first preserve. Generic Web Serial flashing, ordinary
PlatformIO upload, filesystem upload, and whole-chip erase are also forbidden
for the `0.20.3` transition and recovery.

The only accepted transition is the private, table-last workflow owned by
`tools/migrate_kitsu_0203_storage.py`. After transition, ordinary field updates
use an authenticated, signed `.kitsu-fw` package and the A/B OTA path. A
`.kitsu-fw` package is application-only and cannot perform the partition-layout
transition.

## Public release asset boundary

GitHub Releases and the public download site use a strict public-asset
allowlist. The downloadable files are:

- installable, production-signed Android APKs;
- the exact accepted, signed `.kitsu-fw` container; and
- distributable Kitsu pet bundles.

Do not publish raw full-flash images, migration or restore manifests, backup
bytes, volume identifiers, ROM-loader transcripts, NVS-oracle artifacts,
controller material, signing inputs, or physical-acceptance evidence. Raw
application binaries, partition tables, boot helpers, journal clears, and
private migration bundles remain controlled release inputs rather than
ordinary owner downloads.

Android App Bundles (`.aab`), APK signing sidecars (`.idsig`), mapping files,
source-provenance archives, and private validation reports also remain private.
A Play-bound AAB may be transferred privately to Google Play only by the
project owner after a separate explicit publication decision; it is never a
public download asset.

## Test equipment and frozen inputs

Use:

- one supported Heltec WiFi LoRa 32 V3/V3.2 ESP32-S3 8 MiB board running the
  accepted legacy-layout `0.20.2` release and containing representative owner,
  companion, pack, controller, MeshCore, OTA, and message state;
- one ordinary Android phone, including at least one Android 8-11 run for the
  legacy Bluetooth Location-services prerequisite;
- a data-capable USB cable used only by the private migration/restore ceremony;
- two owner-controlled physical storage volumes for independent private 8 MiB
  backup authorities; and
- a second Kitsu board for direct-peer messaging, with a third when available
  for saved-device selection. The focused automated contract test may cover
  the three-saved-device bound without requiring a fourth physical board.

Freeze and record before candidate hardware testing:

- source commit, clean-tree status, scoped source archive, and production build
  hashes;
- Android package name, version/code, APK and AAB bytes/SHA-256, production
  signing-certificate SHA-256, bundletool evidence, source-provenance digest,
  and 16 KiB ZIP/ELF page-alignment evidence;
- the target device MAC, ESP32-S3 identity, 8 MiB flash size, pinned esptool
  bytes/hash, and dedicated migration-tool bytes/hash;
- the exact accepted legacy partition table and selected source application
  size/hash;
- the reviewed `0.20.3` partition-table sector, application size/hash,
  relocated `boot_app0.bin` size/hash, and private OTA-journal clear hash;
- both original full-flash backup paths, physical-volume identities, sizes,
  SHA-256 values, and readback-verification results;
- the captured 20 KiB NVS-prefix hash and exact 256 KiB expanded candidate hash;
- the fresh NVS-oracle record, including pinned IDF 4.4.7 archive, runner,
  harness, compiler executables, two byte-identical builds/logs, retained test
  binary, result, expanded-image binding, and every SHA-256;
- the record-1 signed `.kitsu-fw` acceptance container, its exact canonical
  manifest/signature, release/update IDs, application bytes/hash, and pinned
  update-authority fingerprint; and
- board revision, Android version, tester, and UTC start time.

Record 1 must not contain a final public URL or record 2. The exact signed
`kitsu.ble-firmware.v1` manifest must not contain record 1, record 2, a
`physical_acceptance` object, or any other extra field. Record 2 and the final
promotion record externally bind the immutable record-1 digest to that exact
manifest/signature/package and the public artifacts.

Never put controller roots, pairing secrets, Wi-Fi credentials, private keys,
message text, precise location, raw flash dumps, or signing material in either
evidence record. Record 1 may identify private backup authorities by path,
volume, size, and hash, but retained public evidence must not contain their
bytes.

## Current layout that must be proved

Firmware `0.20.3` uses exactly this 8 MiB layout:

| Region | Offset | Size | Acceptance requirement |
| --- | ---: | ---: | --- |
| `nvs` | `0x009000` | `0x040000` | old 20 KiB prefix preserved; 59 new erased pages |
| `otadata` | `0x049000` | `0x002000` | reviewed relocated OTA helper/selection data |
| `app0` / `ota_0` | `0x050000` | `0x300000` | application at most `0x2ff000`; final 4 KiB journal |
| `app1` / `ota_1` | `0x350000` | `0x300000` | application at most `0x2ff000`; final 4 KiB journal |
| `spiffs` | `0x670000` | `0x140000` | companion packs preserved during migration |
| `kitsu_conn` | `0x7b0000` | `0x040000` | untouched by migration; retired by reviewed runtime policy |
| `coredump` | `0x7f0000` | `0x010000` | preserved during migration |

The unpartitioned ranges `0x04b000..0x04ffff` and
`0x650000..0x66ffff` must be erased. The final 4 KiB sector in each
application slot is a private OTA resume journal and is never application
payload. The firmware version, device class, layout identity, slot size,
journal size, and maximum application size are one indivisible identity.

## A. Private capture, double backup, and fresh NVS oracle — record 1

1. Start from the frozen, accepted legacy-layout `0.20.2` device. Record its
   visible firmware identity and representative state without collecting
   secrets: selected companion/pack, controller count, progression, MeshCore
   configuration, peer/channel presence, message counts, and current slot.
2. Invoke only the dedicated migration tool's `capture` operation with the
   pinned esptool and expected device MAC. It must enter the ROM loader, read
   the complete 8 MiB flash twice, persist and readback-check two private copies
   in owner-only directories on different physical volumes, and bind both
   volume identities and SHA-256 values.
3. Confirm both copies are byte-identical and independently usable. Reject a
   short read, different hash, same physical volume, permissive destination,
   unexpected MAC/chip/flash size, or source layout/image mismatch.
4. Leave the board held in the ROM loader. Do not reset or boot between capture,
   offline audit/manifest freeze, oracle creation, and migration; runtime changes
   to NVS or OTA data would invalidate the frozen equality gate.
5. Construct the exact 256 KiB candidate NVS image from the newly captured
   20 KiB prefix followed by 59 erased 4 KiB pages. Do not use an older capture,
   an all-erased substitute, or a locally edited prefix.
6. Run `tools/run_kitsu_nvs_expansion_oracle.sh` on the protected builder with
   the pinned IDF 4.4.7 archive. Require pinned GCC/G++ hashes, two
   byte-identical builds and logs of the real IDF `Storage`, `PageManager`, and
   `Page` code, successful enumeration of critical namespaces/keys, and zero
   writes and zero erases.
7. Freeze the oracle record and all hashes. The migration tool's `audit`,
   `migrate`, and `resume` operations must each reject a missing, stale,
   differently hashed, writable, or non-fresh oracle input before any flash
   mutation.

Record PASS/FAIL, both backup authorities, both verified hashes, the frozen
input manifest hash, and the complete oracle binding. A single backup or a
synthetic/mock-only NVS parse is a failure.

## B. Private table-last migration — record 1

1. Run the dedicated offline `audit` against the frozen manifest, both original
   backups, expected device identity, accepted legacy table/source app, target
   image, target table, relocated OTA helper, candidate NVS image, and fresh
   oracle. Any mismatch must fail before erase or write.
2. Run `migrate` while the board remains in the ROM loader. Whole-chip erase is
   forbidden. The old partition table remains authoritative until the final
   stage.
3. Erase the full new `app1` slot, write the reviewed application at
   `0x350000`, and read back the exact application hash, erased remainder, and
   blank private journal.
4. Erase and verify only the NVS extension, moved OTA-data range, lower gap,
   new `app0` slot, and upper gap. Write the exact relocated OTA helper at
   `0x049000` and reviewed application at `0x050000`.
5. Before changing the table, re-read both applications and journals, the
   byte-identical legacy NVS prefix, bootloader prefix, every migration erase
   range, and every untouched byte from `0x670000..0x7fffff`. Any mismatch
   must stop with the device still in the ROM loader.
6. Write and read back the reviewed current partition-table sector at
   `0x008000` as the final flash mutation. Re-read the complete 8 MiB result and
   durably record `flash_verified` before issuing the sole reset.
7. Confirm the mutation transcript proves the partition table was last. There
   must be no write after it and no reset before the complete readback record.

Record every erase/write/readback offset, length, hash, state transition, and
safe result code. A success message without the complete pre-reset 8 MiB
readback and durable `flash_verified` state is a failure.

## C. Migration interruption, restore, and final current-layout boot — record 1

1. Exercise the documented interruption path on the acceptance board. After a
   reviewed interruption between NVS-extension erase and table commit, confirm
   ordinary boot is not attempted and recovery begins in the ROM loader with
   the frozen original backups and manifest.
2. Run `resume`. It must classify the table sector before mutating anything:
   exact current table revalidates the completed immutable ranges, exact legacy
   table replays only the reviewed migration stages from the original backup,
   and a partial or unknown table requires `restore`.
3. Run the dedicated restore drill from one frozen original backup. Restore
   must invalidate the table sector first, restore and read back every non-table
   range, write the accepted legacy table last, re-read the complete 8 MiB
   result, durably record `flash_verified`, and then perform the sole reset. A
   transitional capture must never replace either original backup authority.
4. Confirm the restored device boots the exact legacy release with the captured
   owner/companion/controller/MeshCore state intact. Then repeat Sections B1-B7
   without recapturing or replacing the frozen authorities, returning the
   device to the exact accepted current layout.
5. On the first `0.20.3` boot, confirm the visible firmware reports version
   `0.20.3`, device class `heltec-v3.2`, layout
   `kitsu-8m-dual-ota-3m-v1`, 8 MiB flash, two `0x300000` slots,
   `0x1000` journals, and `0x2ff000` maximum application bytes.
6. Confirm the old NVS prefix and captured companion, installed pack,
   progression, controller, MeshCore, and message state survive. Confirm the
   expanded NVS opens without a storage error and remains healthy across a
   second cold boot.
7. Confirm the reviewed runtime retirement removes only retired connectivity
   state and the `kitsu_lan_act` namespace. No old Wi-Fi, gateway, account, or
   backend configuration may reappear. Do not prove this by recording or
   decrypting an old secret.
8. Read back the final table, both application slots and journals, current OTA
   data, NVS geometry, erased gaps, and preserved upper partitions. Bind the
   exact post-migration state and all expected/runtime-owned exceptions in the
   record.

## D. Candidate Android install, permissions, and pairing — record 1

1. Before signing, confirm the highest version code already present in the
   authoritative Play Console Draft and freeze a strictly greater candidate
   code plus matching version name. Build with the checked Gradle wrapper and
   target API 36, validate the AAB with bundletool, and verify every packaged
   native ABI for 16 KiB ZIP and ELF `PT_LOAD` alignment.
2. Install the production-signed candidate through Android's ordinary package
   installer. Confirm package `ptl.kitsu.app`, frozen version/code, minimum
   Android 8/API 26, and target API 36. Test a clean install; it must not inherit
   authorizations from the retired pre-Play app.
3. Verify the app has no Internet or foreground-service permission and exposes
   no account, sign-in, Wi-Fi, gateway, remote companion, or server controls.
4. Start with Bluetooth off and tap **Connect**. A real **Turn on Bluetooth**
   action must appear, and the app must not claim a connection until Android
   and the Heltec agree.
5. On Android 8-11, deny scan permission once, grant it through the visible
   recovery action, disable Location services, and confirm a real **Location
   settings** action. Returning must retry the selected Kitsu rather than
   silently doing nothing.
6. Enable airplane mode, then turn Bluetooth back on. Leave airplane mode on
   for the remaining direct-Bluetooth cases.
7. Begin with no saved authorization. On Kitsu open the bounded **PAIR PHONE**
   window, tap **Pair this phone**, compare the Android Secure Connections code
   on both devices, and complete the uninterrupted physical PRG confirmation.
   Confirm exactly one controller grant and a connected authenticated session.
8. Exercise a GATT interruption/status-22 failure after the code or final grant
   may have been committed. Relaunch the app. It must retain the bounded pending
   candidate and offer **Finish pairing**, not force a blind return to the start
   or allocate another controller slot.
9. If the candidate is authoritative, **Finish pairing** must authenticate and
   promote it to saved state. If the device authoritatively rejects it, Android
   must clear only that pending candidate and permit a clean retry. A generic
   disconnect or timeout is not proof of rejection.
10. Disconnect and reconnect with the saved root. Clock synchronization must
    either succeed or expose a bounded retryable error without invalidating the
    authenticated session. It must not turn a clock-sync failure into a false
    "Heltec disconnected" state.
11. Save a second and, when available, third Kitsu. **Select** must close the old
    GATT session and connect only the selected address. The app must never evict
    a saved Kitsu silently; the focused storage-bound test must prove a fourth
    is refused until the owner explicitly forgets one.
12. Confirm the launch window and Compose UI are dark before the first frame,
    with no white splash. Exercise compact, landscape, and expanded widths;
    content and IME must remain inside insets, navigation must adapt, predictive
    Back must return to Kitsu/Home, and a locked firmware update must remain
    non-dismissible.

## E. Local controls, mesh, messages, and GATT stability — record 1

1. Confirm exactly four primary destinations: **Kitsu**, **Mesh**,
   **Messages**, and **Settings**. Compare companion, battery, needs, bond,
   mood, peers, channels, and message counts with the Heltec. Firmware and pack
   identifiers belong in details/settings rather than the Home hierarchy.
2. Tap **Refresh** after changing device state. The new state must appear, and a
   failure must produce a visible non-secret error rather than a no-op.
3. Run **Pet**, **Feed**, **Play**, and **Listen once**. Each accepted action is
   applied exactly once, including after one deliberately lost receipt. A
   rejected precondition must expose its exact safe code and never claim
   success.
4. Fill the eight-entry durable recent-action window with unexpired unique
   actions, then submit one more. Firmware must return `idempotency_busy`, run
   no side effect, restore the in-memory snapshot, and keep durable replay and
   storage readiness true. Android must say: **Too many recent actions are
   still protected. Wait a moment and retry.** After expiry, retry the same
   action and confirm exactly one effect.
5. Independently force a snapshot, restore, persistence, or readiness failure.
   That condition must return `idempotency_unavailable`, and Android must say:
   **Durable action storage is unavailable.** It must never be represented as
   the transient busy condition. `idempotency_busy` is not an advertise-
   readiness error and must be rejected if received in readiness telemetry.
6. Toggle **Local mesh radio** off and on, run **Advertise now** for both explicit
   `nearby` and `mesh` scopes, fetch peers/channels, and send one direct and one
   channel message. Every MeshCore operation must preserve the authenticated
   Bluetooth session; there must be no disconnect, false disconnected state,
   pairing prompt, or clock-sync loop.
7. For **Advertise now**, each deliberate tap uses a unique action ID. A queued
   receipt shows success followed by authoritative `advertise_cooldown` state
   and remaining duration; a rejected cooldown uses the same visible retry UI.
   Exercise one other prerequisite/busy rejection and show its safe firmware
   code rather than false success.
8. Confirm direct messages contain a peer and no channel slot; channel messages
   contain a channel slot and no peer. A wrapped 24-entry device snapshot must
   not erase or oscillate in Android.
9. Before the first send, confirm the current mesh terms/prohibited-content
   policy gates the composer. Acceptance, selected tab, route, recipient, and
   draft survive activity recreation. Outbound messages have no moderation
   actions. Inbound channel/direct overflow actions, report export, local block,
   reversible unblock, and their no-server wording must match product policy.
10. Leave the app connected for at least three minutes while alternating state,
    clock, mesh, advertise, and message operations. The GATT session must remain
    stable, with no rapid disconnect/reconnect churn, PRG prompt, background
    enrollment, HTTP request, or rate-limit error.
11. Tap **Disconnect**, relaunch, and wait at least 30 seconds. It must stay
    disconnected until **Connect** is tapped. Reconnect must use the saved root
    and bond without pairing again; clock sync and the first MeshCore operation
    must not drop that session.

## F. Controller cleanup and physical recovery — record 1

1. With multiple saved Kitsu, choose **Forget authorization** for one nearby
   device and confirm the destructive prompt. Android removes its saved root
   only after the authenticated `controller.forget` receipt is accepted.
2. Interrupt the final receipt once. Android must offer **Finish forgetting**.
   A later authoritative handshake rejection with that root may complete
   cleanup; a generic disconnect cannot.
3. Confirm the forgotten phone cannot authenticate after a Heltec reboot. Other
   authorizations, the installed pack, companion/progression state, MeshCore
   configuration, and messages remain intact.
4. Pair again through the ordinary physical owner flow. No hidden server
   cleanup, device reset, or storage repair may be required.
5. Fill all four controller slots and attempt one more pairing. On
   `controller_full`, Android directs the owner to the physical
   **CONNECT > CONTROLLERS** menu, slot removal, reopening **PAIR PHONE**, and
   retry. Android must not expose remote slot enumeration, reset, or recovery.
6. Exercise one slot removal and **RESET ALL**, including cancel, timeout,
   interrupted storage commit, and reboot. Only the encrypted controller table
   may change; device identity, device secret, companion, pack, MeshCore, OTA,
   journal, and unrelated NVS state must remain intact. An uncertain storage
   commit keeps BLE locked until reboot rather than claiming success.

## G. Signed post-migration Bluetooth firmware update — record 1

1. On the proven current layout, import the exact signed `.kitsu-fw` through
   Android's document picker while airplane mode remains on. Confirm version,
   image bytes/hash, release/update IDs, device class, partition size, and pinned
   signing authority match the frozen record. Never offer it as a legacy-layout
   migration.
2. The current package must contain exactly one bounded `KITSU-ID1` identity in
   its extracted ESP application. Before `firmware.update.begin` or flash erase,
   Android must reject a missing, duplicate, non-ASCII, unterminated, corrupt,
   noncanonical, wrong-length, bad-CRC, wrong-version, wrong-device, wrong-layout,
   or wrong-geometry identity.
3. Also reject copies with one modified manifest byte, signature byte, or image
   byte; wrong ESP32 chip ID; malformed segment range; bad ROM checksum; bad or
   missing appended digest; oversized image; or trailing byte. Every failure
   must delete the staged image and leave device and OTA state unchanged.
4. Begin the valid update. It must target only the inactive `0x300000` slot,
   reserve its final `0x1000` journal, and limit application bytes to
   `0x2ff000`. While active, every non-update control is disabled. Only
   **Cancel update** is allowed before `ready_to_reboot`; it must perform an
   authenticated abort and verify authoritative `idle` before unlocking.
5. Interrupt GATT during transfer, reconnect, and continue from the device's
   authoritative offset without rapid blind retry or duplicate committed
   chunk. Repeat with power interruption after a durable 64 KiB checkpoint;
   retransmission is bounded to the last incomplete checkpoint and the active
   application still boots.
6. Complete transfer and confirm the final receipt reaches Android before
   reboot. The device boots the inactive slot as `pending_verify`; Android must
   not report success yet.
7. Leave the device healthy for at least 30 continuous seconds. Confirmation is
   allowed only after connectivity retirement, controller storage, BLE startup,
   image/journal/layout identity binding, and live-loop health all pass. Android
   then reports `confirmed` for the exact update ID.
8. Repeat the same update and interrupt power during pending verification. The
   rollback-enabled bootloader must return to the prior slot and Android must
   report `rolled_back`, never `confirmed`. Repeat without interruption and
   obtain `confirmed`, proving both A-to-B and B-to-A operation.
9. Confirm pack, companion/controller state, progression, messages, expanded
   NVS health, and MeshCore configuration survive successful OTA and rollback.
10. Force one recoverable pre-header failure. **Reset interrupted update** must
    return an unbound failed state to `idle`; Android must not remain stuck on a
    null update ID.

## Record 1: private candidate/hardware evidence

For every numbered case A-G, record `pass`, `fail`, or `not_run`, UTC
start/end, the visible safe result code, and concise notes. Record all frozen
source/build/Android/package/migration/backup/oracle hashes listed above plus
the complete migration and restore state/readback records. `not_run` is a
failure unless a case explicitly delegates a bounded condition to an automated
contract test.

Record 1 must prove:

- two independently verified original backups on different physical volumes;
- a fresh real-IDF, zero-mutation NVS expansion oracle;
- current app1 staging, current app0 staging, and the current table written
  last;
- interruption classification, original-authority restore with the legacy
  table written last, and successful re-migration;
- preserved state and healthy expanded NVS on physical hardware;
- pairing/lost-receipt recovery, stable GATT during MeshCore operations, and
  correct `idempotency_busy` versus `idempotency_unavailable` behavior; and
- signed current-layout A/B OTA resume, cancel, pending verification, rollback,
  and recovery.

Hash the immutable canonical record after all cases pass. Never embed its
digest into the exact `kitsu.ble-firmware.v1` manifest, never rewrite record 1
after hashing it, and never reuse it for different bytes. Record 2 and the final
promotion decision must bind that digest externally to the exact final
manifest/signature/package. The private record does not authorize a public Web
Serial migration, and it does not rewrite historical release authority.

## H. Final public Android and `.kitsu-fw` delivery smoke — record 2

Only after record 1 passes:

1. Re-inspect the exact signed `.kitsu-fw` bytes already frozen and exercised
   in record 1. Independently verify the container, exact canonical
   `kitsu.ble-firmware.v1` manifest with no extra field, signature, application
   hash, identity marker, and exact EOF. The public package must be byte-for-byte
   identical to the record-1 package; do not rebuild, re-sign, or substitute a
   different package after hardware acceptance.
2. Stage the coordinated production-signed Android APK, public site/docs/status,
   and signed `.kitsu-fw` behind an atomic rollback. Do not stage a generic
   Web Serial migration, raw firmware, partition table, boot helper, backup,
   controller artifact, oracle record, or legacy recovery bundle as part of the
   `0.20.3` owner journey.
3. From an ordinary browser over the actual public HTTPS origin, confirm the
   Android and firmware-update surfaces name the exact release/device class,
   expose the exact APK and `.kitsu-fw` downloads, display accurate size/hash
   metadata, and describe the package as a post-migration authenticated BLE
   update. Record the final routes, response bytes/hashes, cache behavior, and
   immutable release identifiers.
4. Download the APK from that public route and perform a fresh install through
   the system package installer. Repeat D3-D7, D10, E1-E3, E6, E10-E11, and
   confirm there is no account, gateway, Wi-Fi, server, Internet-permission, or
   private migration path.
5. Download the public `.kitsu-fw` through the ordinary browser/document-picker
   journey. Verify its exact hash equals the final frozen package, then repeat
   G1, G4, G6-G7, and G9 on a device already proven to use the current layout.
   Confirm the new inactive slot and authoritative `confirmed` result.
6. Repeat F1-F4, pair again through the visible owner flow, and perform one
   direct or channel message while airplane mode remains on.
7. Verify public docs describe only the Android/BLE update and owner controls
   exercised here. They may identify dedicated USB migration/restore as private,
   physically supervised support, but must not expose its backup authorities,
   commands, or evidence, and must not present the historical seven-write flow
   as current recovery.
8. On any failure, retain the failed record, atomically restore the prior public
   release, and do not edit record 1 or the signed manifest to hide the failure.

Hash record 2 separately. The external final promotion decision binds both
evidence hashes, exact Android APK/version/certificate, final `.kitsu-fw` and
manifest/signature, source commit, public routes, timestamps, device class, and
PASS/FAIL. Neither evidence digest is embedded into the exact v1 manifest, and
the promotion decision is not embedded into either evidence record.

No Android APK, `.kitsu-fw`, current-layout claim, or public `0.20.3` release is
stable until records 1 and 2 both pass. The historical pre-`0.20.3` Web Serial
authority remains immutable and separate; this gate neither supersedes nor
modifies it.
