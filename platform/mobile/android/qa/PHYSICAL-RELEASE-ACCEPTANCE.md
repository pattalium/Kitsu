# Kitsu local-only physical release acceptance

This is the release gate for the public local-only Kitsu product. It has two
deliberately separate records: candidate hardware acceptance first, then a
public-delivery smoke of the final signed release. This avoids asking an
evidence file to hash a manifest that embeds the evidence file's own hash.

The complete gate must exercise the same public HTTPS pages, production-signed
Android app, Web Serial flow, and authenticated Bluetooth that an ordinary
owner uses. ADB, Docker, a server account, a gateway, direct database repair, a
serial console, and private maintenance controls are not part of acceptance.

Every required case must pass against the exact frozen hashes. A partial run, a
run against a locally patched APK, or a run using a different firmware image is
not publication authorization.

## Test equipment and frozen inputs

Use:

- one supported Heltec WiFi LoRa 32 V3/V3.2 ESP32-S3 8 MiB board;
- one ordinary Android phone, including at least one Android 8-11 run for the
  legacy Bluetooth Location-services prerequisite;
- a data-capable USB cable for the initial/recovery Web Serial operation; and
- two Kitsu boards for the direct-peer messaging case, with a third board when
  available for saved-device selection. Do not require a fourth physical board
  merely to test the three-device storage limit; the focused automated contract
  test covers that bound.

Freeze and record before candidate hardware testing:

- source commit and clean-tree status;
- public-site, flash-site, and documentation build hashes;
- Android package name, version/code, APK and AAB bytes/SHA-256, production
  signing certificate SHA-256, bundletool-validation evidence, source-provenance
  digest, and 16 KiB ZIP/ELF page-alignment evidence;
- the seven-region Web Serial plan, flash-site source/dist hashes, and intended
  release ID, but not a downstream final signed release manifest;
- bootloader, partition-table, application, 4 KiB journal-clear, and 256 KiB
  legacy-connectivity-clear bytes/SHA-256;
- `.kitsu-fw` container bytes/SHA-256, signed manifest bytes/SHA-256, update ID,
  application bytes/SHA-256, and update-authority public-key fingerprint;
- Heltec model/board revision, Android version, UTC start time, and tester.

The candidate evidence defined below must not contain the final Web Serial
manifest bytes/hash, its signature, or the final public-release URL. Only after
that evidence is complete and hashed may the final manifest embed its
`evidence_sha256`, be signed, and enter the separate public-delivery smoke.

Do not put controller roots, pairing secrets, Wi-Fi credentials, private keys,
message text, precise location, flash dumps, or signing material in evidence.

## A. Candidate USB bootstrap and recovery contract — record 1

1. Generate and use the frozen seven-write candidate from
   `tools/package_kitsu_reflashable.cmd`; do not use an older four-write bundle.
   Install the Heltec before a publication manifest exists. This step
   proves the artifact/partition behavior; it does not claim that public
   delivery has passed. Confirm the same flash-site build rejects the previously
   published v1/two-write contract in its automated browser gate.
2. Connect the Heltec and leave whole-chip erase unavailable/off. The candidate
   path must perform and read back exactly these seven fixed writes, in this
   bounded order:

   | Role | Offset | Required bytes |
   | --- | ---: | --- |
   | rollback-enabled bootloader | `0x000000` | signed bootloader artifact |
   | partition table | `0x008000` | signed partition artifact |
   | application A | `0x010000` | signed application artifact |
   | application A OTA journal clear | `0x33f000` | 4,096 bytes of `0xff` |
   | application B | `0x340000` | the same application bytes/hash as A |
   | application B OTA journal clear | `0x66f000` | the same 4,096-byte clear artifact |
   | retired connectivity storage clear | `0x7b0000` | 262,144 bytes of `0xff` |

3. Confirm every readback hash equals its signed artifact hash. In particular,
   both application hashes must match each other, both journal-clear hashes
   must match, and the complete `0x7b0000..0x7effff` readback must equal the
   signed legacy-connectivity-clear hash. There must be no eighth write.
4. Confirm the candidate contract reports that OTA data, companion/controller state,
   MeshCore state, the installed companion pack, and coredump storage are
   preserved. Confirm the seven regions above do not overlap any of them.
5. Reboot normally. The visible boot/version state must report the exact frozen
   local-only firmware version. The device must expose Bluetooth pairing and
   must not request Wi-Fi, gateway enrollment, an account, or a server.
6. If the board previously held gateway/Wi-Fi state, confirm startup completes
   only after legacy connectivity storage and the retired `kitsu_lan_act` NVS
   namespace are retired. The old network configuration must not reappear after
   a second reboot. Do not prove this by recording or decrypting the old secret.

Record PASS/FAIL and all seven readback hashes. A success message without seven
matching readbacks is a failure. Do not cite this case as public-page proof.

## B. Candidate Android install and permission recovery — record 1

1. Before signing, confirm the highest version code already present in the
   authoritative Play Console Draft and freeze a strictly greater candidate
   code plus its matching version name. Build the candidate from the checked
   Gradle wrapper with target API 36, bind it to the exact scoped source archive,
   validate the AAB with bundletool, and verify every packaged native ABI for
   16 KiB ZIP and ELF `PT_LOAD` alignment.
2. Install the production-signed candidate with Android's ordinary package
   installer. Confirm package `ptl.kitsu.app`, the exact frozen version/code,
   minimum Android 8/API 26, and target API 36. This Play identity is a separate
   install from the retired pre-Play app, not an in-place upgrade; it must not
   inherit that app's authorizations or claim upgrade compatibility. Repeat the
   cases below on a clean `ptl.kitsu.app` installation and pair again.
3. Verify the installed app has no Internet or foreground-service permission
   and exposes no account, sign-in, Wi-Fi, gateway, remote companion, or server
   controls.
4. With a Kitsu retained by the upgrade—or after Section C on a fresh install—
   start with Bluetooth off and tap **Connect**. A real **Turn on Bluetooth**
   action must appear, and the app must not claim to be connected until Android
   and the Heltec agree.
5. On Android 8-11, deny the scan permission once during Connect or Pair, then
   grant it using the visible recovery action. Disable system Location services
   and confirm a real **Location settings** action appears. Returning from
   Location settings must retry the selected Kitsu rather than silently doing
   nothing.
6. Enable airplane mode, then turn Bluetooth back on. Leave airplane mode on
   for the remaining direct-Bluetooth cases. The app must remain usable without
   a network route.
7. On a clean install, confirm the launch window and Compose UI are dark before
   the first frame, with no white splash. **Dark** must remain the persisted
   default. **System** may follow Android only after the owner explicitly opts
   in.
8. Exercise a compact phone, landscape phone, and tablet/large-width device.
   Confirm there is no fixed-orientation declaration, content and IME remain
   inside system insets, compact width uses bottom navigation, and expanded
   width uses a navigation rail. Predictive Back from Mesh, Messages, or
   Settings must return to Kitsu/Home; a locked firmware update must remain
   non-dismissible.

## C. First-owner pairing and lost-receipt recovery — record 1

1. Begin with no saved authorization for the test phone. On Kitsu, hold PRG from
   Home, hold while **CONNECT** is selected, then hold on **BLUETOOTH** to open
   the bounded **PAIR PHONE** window. In Android tap **Pair this phone**. Confirm
   Android Secure Connections numeric comparison on both phone and Heltec, then
   perform the physical confirmation requested by the Heltec.
2. Confirm the Kitsu appears under Saved Kitsu, is selected, authenticates, and
   reaches the connected state without any account, gateway, or Wi-Fi step.
3. Exercise the lost-final-receipt path once using a controlled GATT interruption
   after the commit may have reached the device. Relaunch the app. It must retain
   the pending candidate and offer **Finish pairing**; it must not create another
   controller slot blindly.
4. Tap **Finish pairing**. If the candidate is authoritative, authentication must
   promote it to saved state. If the device authoritatively rejects it, the app
   must clear only that pending candidate and permit a clean retry.
5. Save a second and, when available, third Kitsu. **Select** must close the old
   GATT session and connect only the selected address. The app must never evict a
   saved Kitsu silently; the automated storage-bound test must prove a fourth is
   refused until the owner explicitly forgets one.

## D. Truthful local controls, state, and messages — record 1

1. Confirm the app exposes exactly four primary destinations: **Kitsu**,
   **Mesh**, **Messages**, and **Settings**. With one Kitsu selected, compare the
   visible companion name, battery, needs, bond, mood, peers, channels, and
   message list with the Heltec state. Firmware and pack identifiers belong in
   details/settings rather than the Home hierarchy.
2. Tap **Refresh** after changing device state. The new state must appear, and a
   refresh failure must produce a visible non-secret error rather than a no-op.
3. Run **Pet**, **Feed**, **Play**, and **Listen once**. Confirm each accepted
   action is applied exactly once. Exercise at least one rejected precondition;
   its exact safe error must be visible and the UI must not claim success.
4. Toggle **Local mesh radio** off and on. The visible state must match the
   device after Refresh and after reconnect.
5. Send one direct peer message and one channel message using the real selector.
   Confirm direct messages have a peer and no channel slot; channel messages
   have a channel slot and no peer. Confirm the 24-entry snapshot refresh does
   not erase or oscillate when the device ring has wrapped.
6. Before the first send, confirm the current versioned mesh terms and prohibited-
   content policy gate the composer. Accept the policy, recreate the activity,
   and confirm acceptance, selected tab, route, recipient, and draft survive.
   An outbound message must have no moderation actions. An inbound channel
   message offers only **Report message** in its compact overflow. An inbound
   direct message offers **Report message**, **Report sender**, and **Block
   sender** in that overflow—never as a permanent button row. Report exports
   must bind `report_type`, write only to the owner-chosen document, and state
   truthfully that nothing was submitted automatically. Blocking must persist,
   hide that direct peer locally, prevent new sends to it, and remain reversible
   in Settings without claiming a radio-wide or server-side ban.
7. With firmware supporting `advertise_once`, confirm **Advertise now** appears
   only as a real enabled control. Nearby is the default and sends explicit
   scope `nearby`; Mesh sends explicit scope `mesh`. Each deliberate tap uses a
   unique action ID. A queued receipt must show success followed by the
   authoritative `advertise_cooldown` state and remaining duration; a rejected
   cooldown uses the same visible retry UI. Exercise one other prerequisite or
   busy rejection and confirm its safe firmware code is shown rather than a
   false success.
8. Leave the app connected for at least three minutes. The GATT session must
   remain stable; there must be no two-second disconnect/reconnect churn, PRG
   prompt, background enrollment, HTTP request, or rate-limit error.
9. Tap **Disconnect**, relaunch the app, and wait at least 30 seconds. It must stay
   disconnected until **Connect** is tapped. Reconnect must use the saved root
   and bond without pairing again.

## E. Controller cleanup and recovery — record 1

1. With multiple saved Kitsu, choose **Forget authorization** for one nearby
   device and confirm the destructive prompt. The app must remove its saved root
   only after the authenticated `controller.forget` receipt is accepted.
2. Interrupt the final receipt once. The app must offer **Finish forgetting**.
   A later authoritative handshake rejection with that same root may complete
   cleanup; a generic disconnect must not be treated as proof.
3. Confirm the forgotten phone can no longer authenticate after a Heltec reboot.
   Other phone authorizations, saved Kitsu, the installed pack, companion name,
   progression/brain state, MeshCore state, and messages must remain intact.
4. Pair the forgotten phone again through the normal owner flow and confirm no
   hidden server cleanup or device reset is required.
5. Fill all four controller slots and attempt one more pairing. On
   `controller_full`, Android must direct the owner to Kitsu's physical
   **CONNECT > CONTROLLERS** menu, slot removal, reopening **Pair Phone**, and
   retry. Android must not expose remote slot enumeration, reset, or recovery.

## F. Signed Bluetooth firmware update — record 1

1. Import the exact signed `.kitsu-fw` through Android's document picker while
   airplane mode remains on. Confirm version, image bytes/hash, release ID, and
   update ID match the frozen record.
2. Before a valid import, try copies with one modified manifest byte, signature
   byte, image byte, wrong ESP32 chip ID, malformed segment range, bad ROM
   checksum, bad appended digest, missing digest, oversized image, and trailing
   byte. Every copy must fail before `firmware.update.begin` or flash erase.
3. Begin the valid update. While it is active, every non-update control—including
   Disconnect, Select, Pair, Forget, Refresh, care, Mesh, and Send—must be disabled.
   The screen must stay awake. Only **Cancel update** is allowed before
   `ready_to_reboot`; cancellation must perform authenticated abort and verify
   authoritative `idle` before unlocking controls.
4. Interrupt GATT during transfer, reconnect, and continue. Android must query
   device status and resume at its authoritative offset, with no rapid blind
   retry loop or duplicate committed chunk. Repeat with a power interruption
   after a durable 64 KiB checkpoint; retransmission must be bounded to the last
   incomplete checkpoint and the active application must still boot.
5. Complete the transfer. Confirm the final receipt reaches Android before the
   Heltec reboots. The device must boot the inactive A/B slot as
   `pending_verify`; Android must not report success yet.
6. Leave the device healthy for at least 30 continuous seconds. Confirmation is
   allowed only after legacy-connectivity retirement, controller security, BLE
   startup, OTA-journal/image binding, and the live-loop health gate all pass.
   Android must then report `confirmed` for the exact update ID.
7. Start the same update again and interrupt power during the pending-verification
   window. Confirm the rollback-enabled bootloader returns to the previous slot
   and Android reports `rolled_back`, never `confirmed`. Repeat without the
   interruption and obtain `confirmed`.
8. Confirm the installed pack, companion/controller state, progression, messages,
   and MeshCore configuration survive successful OTA and rollback.
9. Force one recoverable pre-header update failure in the physical fault setup.
   **Reset interrupted update** must return an unbound failed state to `idle`;
   the app must not become permanently stuck on a null update ID.

## G. Candidate USB recovery after BLE OTA — record 1

1. Return to the same bounded candidate USB path and install the same candidate
   again without a whole-chip erase.
2. Require all seven signed writes and readbacks again. Confirm both A/B apps are
   identical, both OTA journals are clean, the complete retired connectivity
   region is `0xff`, and preserved local owner/companion/MeshCore state remains.
3. Power-cycle twice. The firmware must not resurrect an old gateway, Wi-Fi
   credential, controller authorization that was forgotten, or stale OTA journal.
4. Reconnect from Android and repeat one Refresh, one accepted action, one direct
   or channel message, Disconnect, and Connect.

## Record 1: candidate hardware evidence

For every numbered case A-G, record `pass`, `fail`, or `not_run`, UTC start/end,
the app-visible or tool-visible safe result code, and concise notes. Record the
frozen source/build/APK/AAB/provenance/16-KiB/artifact/`.kitsu-fw` hashes listed above and the SHA-256
of this completed candidate evidence. `not_run` is a failure unless the case
explicitly says an automated bound test covers it.

This record must end before the final Web Serial manifest is created. Its
canonical bytes must exclude the final manifest, final signature, and final
public URL. The `kitsu.firmware-publication-authorization.v2` object placed in
the final signed manifest may then bind this record's `evidence_sha256` plus the
bootloader, partition-table, application, OTA-journal-clear, and
legacy-connectivity-clear SHA-256 values. Never rewrite record 1 after hashing
it, and never reuse it for another binary.

## H. Final public-delivery smoke — record 2

Only after record 1 passes:

1. Construct the exact final v2 Web Serial manifest with record 1's digest in
   `physical_acceptance.evidence_sha256`, sign its exact bytes, and verify the
   signature and seven artifact hashes independently. Record the final manifest
   bytes/SHA-256, signature SHA-256, release ID, and immutable artifact URLs in
   record 2. None of those values are fed back into record 1 or the manifest.
2. Stage the coordinated Android, public-site, documentation, status, Web Serial,
   raw firmware, and `.kitsu-fw` release behind an atomic rollback. It is a
   delivery candidate, not stable, until this section passes.
3. From an ordinary browser over the public HTTPS origin, repeat Section A with
   the final signed manifest. Require the exact seven writes/readbacks and the
   exact manifest/signature recorded in step 1.
4. Download Android from the public page and perform a fresh install through the
   system package installer. Repeat B3-B6, C1-C2, all of D, and confirm there is
   no account, gateway, Wi-Fi, server, or Internet-permission path.
5. Download the public `.kitsu-fw` file, verify its hash equals record 1, and
   repeat F1, F3, F5, F6, and F8 through the public app/artifact journey.
6. Repeat E1 and E3, pair again through the visible owner flow, then repeat the
   complete Section G recovery through the public Web Serial origin.
7. Verify public docs describe the controls just exercised and public status
   checks only the retained static release surfaces. Retired API/account/gateway
   availability is not a success criterion.
8. On any failure, retain the failed record, atomically restore the prior public
   release, and do not edit record 1 to hide the failure.

Hash the completed public-delivery record separately. The final promotion
decision is an external retained record that binds both evidence hashes, the
final Web Serial manifest/signature, Android APK/certificate, `.kitsu-fw`, and
source commit. It is not embedded back into the signed manifest.

No APK, firmware, Web Serial manifest, BLE update, or public local-only claim is
stable until records 1 and 2 both pass. This two-record rule is mandatory; one
self-referential evidence document may not authorize itself.
