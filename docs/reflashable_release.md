# Kitsu local-first owner-reflashable release contract

This is the authoritative firmware security, update, and physical-acceptance
contract for Kitsu868 0.12.x and later local-first releases. Kitsu deliberately
remains erasable, serial-reflashable, and repurposable like a stock MeshCore
board. No release, installer, signer, or operator is authorized to burn an
eFuse or disable an owner recovery/debug interface.

The withdrawn Secure Boot/Flash Encryption ceremony is retained only for
forensic history in [production_provisioning.md](production_provisioning.md).
Its artifacts are not Kitsu releases and its commands must not be executed.

## Product boundary

The supported runtime has two local radios:

- authenticated Bluetooth between the Android controller and nearby Kitsu;
- MeshCore over LoRa for the device's configured regional radio profile.

The Android application and shipped firmware do not require or expose a Kitsu
account, HTTP API, identity provider, public or self-hosted gateway, mobile
relay, Wi-Fi provisioning, LAN control, TLS client, or server certificate.
Historical server source may be retained for a bounded rollback/archive period,
but it is excluded from the product build. Stored connectivity generations are
not retained: the local-only firmware and USB bootstrap both erase and verify
the isolated legacy connectivity partition, and firmware removes only the
retired LAN-action NVS namespace.

The active authenticated Bluetooth operation allowlist is:

```text
state.get
history.get
peers.get
messages.get
channels.get
clock.sync
mesh.configure
action.apply
controller.forget
firmware.update.status
firmware.update.begin
firmware.update.write
firmware.update.finish
firmware.update.reboot
firmware.update.abort
```

No other control operation may be added without a matching Android UI,
firmware implementation, focused contract test, and physical-user acceptance.

## Reflashable security boundary

The application reports these facts truthfully:

| Field | Required value | Meaning |
|---|---:|---|
| `security_mode` | `"reflashable"` | Owner recovery and repurposing are intentional. |
| `secure_boot` | `false` | ROM does not enforce a Kitsu signing key. |
| `flash_encryption` | `false` | Raw flash is not confidential against physical access. |
| `nvs_encryption` | `false` | There is no hardware-backed encrypted-NVS boundary. |
| `hardware_root_protected` | `false` | Application secrets have no irreversible hardware root. |
| `application_encrypted` | `true` | Controller records use authenticated application-layer generations. |

The following remain enabled and unburned:

- ROM serial download and ordinary `esptool` write/verify operations;
- deliberate whole-chip erase and clean reflash;
- UART and USB download/recovery paths;
- the unmodified board's JTAG/debug capability; and
- all Secure Boot, Flash Encryption, anti-rollback, HMAC-root,
  download-mode-lock, JTAG-lock, UART-lock, and USB-lock eFuses.

Kitsu does not claim verified boot, physical-tamper resistance, flash
confidentiality, or resistance to malicious firmware installed by someone
holding the board. Signed Bluetooth OTA authenticates a normal update; it does
not remove the owner's serial recovery right.

## Protections that remain mandatory

- Bluetooth LE Secure Connections and numeric comparison;
- an explicit physical PRG confirmation before granting a controller;
- a device-issued controller root stored in Android Keystore-backed encrypted
  storage;
- fresh application-session keys, authenticated envelopes, bounded framing,
  sequence checks, and replay protection;
- durable `controller.forget` on the device before Android removes its copy;
- encrypted bounded Android cache with Android backup disabled;
- no Android `INTERNET` or foreground-service permission;
- signed Android distribution with the established application certificate;
- signed canonical `.kitsu-fw` manifests, exact image hashes, inactive-slot
  writes, readback, and bootloader rollback; and
- no eFuse, bootloader, arbitrary-partition, NVS, pack, controller-store,
  MeshCore-state, or coredump write through the BLE OTA protocol.

## Lost-phone and controller-table recovery

Controller recovery is deliberately local to the Heltec. It has no BLE,
Internet, gateway, or backend operation, and Bluetooth proximity is not
authorization to inspect or revoke a controller.

The physically present owner opens `MENU > CONNECT > CONTROLLERS` with PRG.
Entering the manager closes the pairing window, cancels a pending grant, stops
BLE advertising, rejects new links, and disconnects the current link. The OLED
then exposes exactly four bounded controller slots, each as `EMPTY` or a short
controller-ID fingerprint, plus `RESET ALL` and `BACK`. No controller root is
displayed or logged.

A slot removal or all-controller reset requires a second, uninterrupted
five-second PRG hold on a screen that names the exact action and displays both
the hold countdown and tap-to-cancel control. Confirmation expires after 15
seconds and browsing expires after 30 seconds. All recovery authority is
volatile, so cancellation, timeout, or reboot cannot leave a recovery grant
open. If a storage result is uncertain after a commit attempt, BLE stays
locked and the OLED requires a reboot before pairing can resume.

Recovery rewrites only the encrypted `kitsu_sec` controller table through its
two-record retirement transaction. It preserves the device ID and device
secret, companion/brain state, installed pack, MeshCore identity and settings,
legacy Wi-Fi/LAN retirement evidence, OTA state, discovery journal, and every
unrelated NVS namespace. The authenticated `controller.forget` operation still
revokes only the controller that established that verified session.

## Build profile

The owner image is:

```text
PlatformIO environment: heltec_wifi_lora_32_V3_reflashable
compile marker:          KITSU_SECURITY_MODE_REFLASHABLE=1
partition layout:        partitions_kitsu_8MB.csv
firmware version:        0.16.5
```

`platformio.ini` must exclude the legacy connectivity, enrollment, gateway,
LAN, and mobile-relay source units from the normal product build. The linked
ELF/map and final application image must be audited to prove that those product
operations, URLs, certificates, and runtime symbols are absent.

The withdrawn `heltec_wifi_lora_32_V3_production` environment and its old
partition layout are forensic inputs only and are not release profiles.

## Partition and A/B update contract

The supported 8 MiB layout includes:

```text
otadata     0x00e000  0x002000
app0        0x010000  0x330000  ota_0
app1        0x340000  0x330000  ota_1
pack        0x670000
retired     0x7b0000  0x40000   legacy connectivity region, cleared
coredump    0x7f0000
```

BLE OTA accepts only the inactive `ota_0` or `ota_1` partition whose exact
size is `0x330000`. Application bytes are limited to `0x32f000`; the final
4 KiB sector is a private, readback-verified, append-only resume journal.
Only the inactive application slot and, after complete verification, normal
OTA selection metadata may change.

The first boot of a new slot remains `PENDING_VERIFY`. Kitsu requires successful
legacy-connectivity retirement, critical controller storage, BLE initialization,
valid OTA image/journal binding, and a live main loop for 30 continuous seconds
before calling `esp_ota_mark_app_valid_cancel_rollback()`. A missing or corrupt
running-slot journal, critical-init failure, crash, watchdog reset, or power
interruption before confirmation rolls back to the prior application.

The public USB/Web Serial bootstrap is the trust anchor for that rollback
behavior, including on a stock Heltec. Its signed v2 release manifest permits
exactly seven readback-verified writes: the reviewed rollback-enabled Kitsu
bootloader at `0x000000`, the partition table at `0x008000`, the same accepted
application in app0 and app1, a 4 KiB all-`0xff` journal clear at `0x33f000`
and `0x66f000`, and a 256 KiB all-`0xff` retirement image at `0x7b0000`.
The final isolated write removes historical Wi-Fi, gateway, mTLS, and backend
secrets. It preserves OTA selection data, companion state and packs, controller
records, MeshCore state, and coredump. Firmware also erases the retired
`kitsu_lan_act` namespace without clearing unrelated NVS. A physical-acceptance
authorization v2 binds the bootloader, partition table, application,
journal-clear, and legacy-connectivity-clear SHA-256 values. A stock or unknown
bootloader must never be treated as proof that rollback is enabled.

## Offline `.kitsu-fw` package

`tools/package_kitsu_ble_firmware.mjs` owns the public offline container. The
20-byte header is:

```text
0..7    ASCII KITSUFW1
8..11   manifest byte length, u32 big-endian, 1..1024
12..13  signature byte length, u16 big-endian, exactly 64
14..15  flags, u16 big-endian, exactly 0
16..19  application byte length, u32 big-endian, 1..0x32f000
```

It is followed by exact canonical manifest bytes, a raw 64-byte Ed25519
signature over those bytes, one raw ESP32-S3 application image, and exact EOF.
The canonical manifest has this exact field order:

```json
{"schema":"kitsu.ble-firmware.v1","release_id":"...","firmware_version":"...","device_class":"heltec-wifi-lora-32-v3-esp32s3-8mb","image_format":"esp32s3-app","image_bytes":0,"image_sha256":"...","partition_bytes":3342336,"chunk_bytes":4096,"rollback":true}
```

The existing offline update authority remains pinned by its Ed25519 SPKI
SHA-256:

```text
df530766fbc4fc93e82cdbd354ebe4a17a453c83e9bb7fe2af30ca2d202494ab
```

The private key never enters the repository, Android package, firmware image,
public host, or ordinary build environment. Protected signing receives only
the exact canonical manifest and returns only its raw detached signature.

The public packager and both clients independently validate the ESP image
header, ESP32-S3 chip ID, segment count and bounds, ROM checksum placement and
value, appended-hash flag, appended SHA-256, declared image hash/size, and exact
EOF.

## Candidate build and local package checks

Build and test the exact target:

```powershell
pio run -e heltec_wifi_lora_32_V3_reflashable
cmd /c tools\test_kitsu_ble_ota.cmd
node --test tools/test_package_kitsu_ble_firmware.mjs
cmd /c tools\test_kitsu_reflashable_profile.cmd
```

The current repeat-correlation candidate identifies itself as firmware
`0.16.5`. Do not publish or flash these bytes under any already-used firmware
identity; the source version, package version, binary hash, and physical
acceptance record must agree exactly.

The generic serial candidate packager remains candidate-only. Pass the current
version explicitly and use a new/empty destination:

```powershell
tools\package_kitsu_reflashable.cmd `
  dist\kitsu-0.16.5-owner-reflashable-candidate `
  0.16.5
```

The checked stable/public packager remains frozen to the last accepted release
until a new exact binary passes physical acceptance. Do not relabel an older
authorization/evidence digest or edit the stable pins speculatively.

## Two-record physical and delivery acceptance

Acceptance deliberately uses two retained records so no evidence digest is
self-referential. Use supported Heltec hardware, ordinary Android phones, and
only visible owner controls. ADB, Docker, and direct database repair are not
part of acceptance.

Record 1 is the prepublication candidate/hardware record. It binds the frozen
source and site builds, production-signed Android candidate, signed
`.kitsu-fw`, and the five unique USB artifact byte sequences. Before any final
Web Serial manifest exists, it must prove all seven bounded USB writes/readbacks,
local pairing and lost-receipt recovery, airplane-mode controls and messages,
stable GATT, per-device Disconnect/Forget, BLE OTA resume/cancel/reboot,
pending verification, rollback, and USB recovery. The 30-second OTA health gate
includes successful legacy-connectivity and LAN-action retirement, controller
storage, BLE initialization, OTA image/journal binding, and the live loop.
Record 1 must not contain the downstream final manifest, signature, or public
URL. Its frozen SHA-256 becomes `physical_acceptance.evidence_sha256` in the
final signed v2 manifest.

Record 2 is the public-delivery smoke. After record 1 passes, create and sign
the final manifest, stage the coordinated release behind an atomic rollback,
then use the actual public HTTPS pages as an ordinary owner: perform all seven
Web Serial writes/readbacks, install Android fresh, pair, use local controls and
messages in airplane mode, Disconnect/reconnect, install the public
`.kitsu-fw`, confirm the new slot, Forget/re-pair, and run public USB recovery.
Record 2 binds the exact final manifest/signature and public artifacts, but its
digest is never fed back into that manifest. Failure restores the prior public
release and remains retained as a failure.

The external final promotion decision binds both evidence digests, exact Android
APK/version/certificate, firmware and `.kitsu-fw` hashes, final Web Serial
manifest/signature, source commit, timestamps, device class, and PASS/FAIL. It
is not embedded into the manifest. Do not collect controller secrets, private
keys, full flash dumps, message text, precise location, or third-party traffic.

The authoritative numbered cases and evidence fields are in
`platform/mobile/android/qa/PHYSICAL-RELEASE-ACCEPTANCE.md`. Both records must
pass; a single circular evidence document may not authorize itself.

## Promotion and recovery

Only record-1-accepted bytes may be bound into the signed BLE package and Web
Serial publication manifest. Only after the separate record-2 public-delivery
smoke passes may that coordinated Android/site/docs/status/Web Serial/raw-update
release be declared stable. Keep the atomic rollback available throughout.

After the one-time local-only retirement of the legacy LAN-action namespace, an
ordinary matching-layout update preserves the remaining NVS, companion state,
packs, controller authorization, MeshCore configuration, and coredump. A
whole-chip erase intentionally destroys them and requires separate explicit
owner authorization. Raw backups are sensitive because this reflashable design
does not provide physical flash confidentiality.
