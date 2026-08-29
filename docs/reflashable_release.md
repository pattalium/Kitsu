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
not retained: the local-only firmware erases and verifies the isolated legacy
connectivity partition and removes only the retired LAN-action NVS namespace.
The separately retained USB-bootstrap implementation also cleared that region
in its exact pre-0.20.3 layout, but it is immutable history—not a 0.20.3
install, migration, or recovery path.

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

The current owner image is:

```text
PlatformIO environment: heltec_wifi_lora_32_V3_reflashable
compile marker:          KITSU_SECURITY_MODE_REFLASHABLE=1
partition layout:        partitions_kitsu_8MB.csv
firmware version:        0.20.3
```

The version and physical layout are one indivisible release identity:
`0.20.3` must never be built, packaged, or installed with the legacy
`0x330000` OTA-slot layout.

`platformio.ini` must exclude the legacy connectivity, enrollment, gateway,
LAN, and mobile-relay source units from the normal product build. The linked
ELF/map and final application image must be audited to prove that those product
operations, URLs, certificates, and runtime symbols are absent.

The withdrawn `heltec_wifi_lora_32_V3_production` environment and its old
partition layout are forensic inputs only and are not release profiles.

## Partition and A/B update contract

Firmware `0.20.3` and later use this exact 8 MiB layout:

```text
nvs         0x009000  0x040000
otadata     0x049000  0x002000
app0        0x050000  0x300000  ota_0
app1        0x350000  0x300000  ota_1
spiffs      0x670000  0x140000  companion packs
kitsu_conn  0x7b0000  0x040000  isolated legacy connectivity region
coredump    0x7f0000  0x010000
```

BLE OTA accepts only the inactive `ota_0` or `ota_1` partition whose exact
size is `0x300000`. Application bytes are limited to `0x2ff000`; the final
4 KiB sector is a private, readback-verified, append-only resume journal.
Only the inactive application slot and, after complete verification, normal
OTA selection metadata may change.

The old 20 KiB NVS prefix remains byte-for-byte at `0x009000..0x00dfff`.
The additional 59 erased 4 KiB pages occupy `0x00e000..0x048fff`. The gaps at
`0x04b000..0x04ffff` and `0x650000..0x66ffff` are intentionally erased and
unpartitioned. SPIFFS, `kitsu_conn`, and coredump retain their old addresses,
sizes, and bytes during migration.

### One-time 0.20.2 to 0.20.3 storage migration

An application-only BLE OTA, ordinary PlatformIO upload, and the historical
seven-write Web Serial bundle cannot change the partition table safely. The
one-time transition therefore uses `tools/migrate_kitsu_0203_storage.py` with
the pinned esptool, expected device MAC, exact reviewed source and target
application sizes and SHA-256 values, and two independently verified private
8 MiB backups. The two copies must use different owner-only directories on
different physical volumes; the frozen manifest records both volume identities.
Each backup parent is a newly created, otherwise-empty dedicated leaf outside
the source tree, user profile, volume root, and their ancestors. Capture
replaces inheritance with one current-owner full-control DACL before reading
flash, then queries that protected DACL on both directories and files. A
pre-existing/nonempty leaf, extra/inherited principal, non-NTFS Windows volume,
same-volume copy, hardlink, or unverifiable ACL fails before serial access.
Backups and evidence remain outside the source and public release trees.

`capture` enters the ROM loader, reads the full flash twice, persists and
readback-checks both private copies, and deliberately leaves the board held in
the loader. It must not reboot between capture, offline audit/manifest freeze,
and migration: ordinary runtime could legitimately change NVS or OTA metadata
and invalidate the frozen backup equality gate.

Before migration audit, construct the exact 256 KiB candidate NVS image from
the newly captured 20 KiB prefix followed by 59 erased pages. Run
`tools/run_kitsu_nvs_expansion_oracle.sh` on the protected `zzz-001` builder
with the pinned IDF 4.4.7 source archive. The runner pins the GCC/G++ executable
hashes, compiles the real IDF `Storage`, `PageManager`, and `Page` sources twice,
requires byte-identical binaries and build logs, mounts and enumerates critical
namespaces/keys, and proves zero writes and zero erases. `oracle-record` binds
the retained binary, both build logs, exact result, runner/harness/archive
hashes, and expanded-image hash to the captured prefix. `audit`, `migrate`, and
`resume` reject a missing or differently hashed oracle record.

The migration holds the device in the ROM loader, never performs a whole-chip
erase, and applies these readback-gated stages:

1. Validate the legacy table, selected `ota_0` image, 20 KiB NVS prefix,
   device MAC, ESP32-S3 identity, 8 MiB flash size, candidate image, moved
   `boot_app0.bin`, and frozen canonical input manifest.
2. Erase the full new app1 slot, write the reviewed image at `0x350000`, and
   verify the image, erased remainder, and resume journal.
3. Erase and verify only the NVS extension, moved OTA-data range, lower gap,
   new app0 slot, and upper gap; write the exact OTA helper at `0x049000` and
   the reviewed image at `0x050000`.
4. Re-read both slots and journals, the byte-identical legacy NVS prefix, the
   bootloader prefix at `0x000000..0x007fff`, every erased migration range, and
   all untouched bytes at `0x670000..0x7fffff`.
5. Write and read back the new partition-table sector at `0x008000` as the
   final flash mutation. Re-read the complete 8 MiB result before the sole
   reset.

The complete pre-reset readback is durably recorded as `flash_verified`
before issuing that reset. On Windows, the same-leaf temporary has its bytes
flushed and private DACL verified before `MoveFileExW` publishes the status with
`REPLACE_EXISTING|WRITE_THROUGH`; failure leaves `prepared` and suppresses the
reset. POSIX uses atomic publication plus mandatory directory fsync. Backup and
evidence directory entries use the same fail-closed durable publisher.

The sole transition out of the ROM loader is pinned esptool 4.11's read-only
`read_mac` operation with global `--after hard_reset`. Its exact 29-file Python
source tree is SHA-256 pinned. The `run` operation is forbidden because it
would launch the application once before the global reset. All ceremony writes
use `--flash_mode keep --flash_freq keep --flash_size keep`, so restoring at
offset zero cannot rewrite the frozen legacy bootloader header.

If the reset succeeds but its host acknowledgement
is lost, `resume` accepts only that exact bound record, target MAC, and frozen
manifest. It then revalidates the bootloader/table plus all gaps, application
slots, and SPIFFS bytes before retrying the reset. Runtime-owned NVS, OTA-data,
`kitsu_conn`, and coredump bytes may legitimately have changed after the first
boot and are deliberately excluded from this second immutable-range check.
Restore uses the same rule for its legacy-layout `flash_verified` state.

From the NVS-extension erase until the final table commit, a power loss leaves
the board recoverable only through the ROM loader and one of the frozen full
backups. `resume` classifies the table sector before doing anything: an exact
new table is verified without erasing expanded NVS again, an exact old table
replays the reviewed stages from the original backup, and a partial/unknown
table requires `restore`. Restore first invalidates the table sector, restores
all non-table ranges, and writes the old table last. No transitional flash
capture may replace the original recovery authority.

After migration, ordinary PlatformIO firmware upload remains fail-closed via
`tools/platformio_kitsu_upload_guard.py`. It relocates the exact pinned
8192-byte Arduino `boot_app0.bin` helper from the unsafe framework default
`0x00e000` to `0x049000`, enforces application offset `0x050000` and maximum
`0x2ff000`, validates the compiled partition table, and includes exact FF
clears for both private OTA journals. Physical generic `upload`, filesystem,
and erase targets are permanently blocked: the one-time transition uses the
reviewed migration and subsequent field updates use signed BLE OTA.

The final ELF is linked with `--wrap=esp_partition_erase_range`. The wrapper
rejects only an exact whole-partition erase of the main `nvs` partition at
`0x009000`, for either legacy size `0x005000` or current size `0x040000`, and
latches the attempt before `setup()`. Partial NVS garbage-collection erases and
all other partitions still reach the real IDF function. The first statement
path in `setup()` halts on the latch. Host spies plus final `nm`/`objdump`
inspection prove Arduino `initArduino` calls the wrapper and that the wrapper
is the sole call site for the real erase function.

The first boot of a new slot remains `PENDING_VERIFY`. Kitsu requires successful
legacy-connectivity retirement, critical controller storage, BLE initialization,
valid OTA image/journal binding, and a live main loop for 30 continuous seconds
before calling `esp_ota_mark_app_valid_cancel_rollback()`. A missing or corrupt
running-slot journal, critical-init failure, crash, watchdog reset, or power
interruption before confirmation rolls back to the prior application.

The historical public USB/Web Serial bootstrap is the trust anchor for the
legacy rollback layout, including on a stock Heltec. Its signed v2 release
manifest permits exactly seven readback-verified writes: the reviewed
rollback-enabled Kitsu bootloader at `0x000000`, the legacy partition table at
`0x008000`, the same accepted application in legacy app0 and app1, two legacy
journal clears, and a 256 KiB all-`0xff` retirement image at `0x7b0000`.
The final isolated write removes historical Wi-Fi, gateway, mTLS, and backend
secrets. It preserves OTA selection data, companion state and packs, controller
records, MeshCore state, and coredump. Firmware also erases the retired
`kitsu_lan_act` namespace without clearing unrelated NVS. A physical-acceptance
authorization v2 binds the bootloader, partition table, application,
journal-clear, and legacy-connectivity-clear SHA-256 values. A stock or unknown
bootloader must never be treated as proof that rollback is enabled.

That historical bundle is not a `0.20.3` migration mechanism. It writes the
partition table too early, targets the legacy slots, clears `kitsu_conn`, and
resets on failure. Active tooling rejects the current CSV and directs the
operator to the table-last migration workflow.

## Offline `.kitsu-fw` package

`tools/package_kitsu_ble_firmware.mjs` owns the public offline container. The
20-byte header is:

```text
0..7    ASCII KITSUFW1
8..11   manifest byte length, u32 big-endian, 1..1024
12..13  signature byte length, u16 big-endian, exactly 64
14..15  flags, u16 big-endian, exactly 0
16..19  application byte length, u32 big-endian, 1..0x2ff000
```

It is followed by exact canonical manifest bytes, a raw 64-byte Ed25519
signature over those bytes, one raw ESP32-S3 application image, and exact EOF.
The canonical manifest has this exact field order:

```json
{"schema":"kitsu.ble-firmware.v1","release_id":"...","firmware_version":"...","device_class":"heltec-wifi-lora-32-v3-esp32s3-8mb","image_format":"esp32s3-app","image_bytes":0,"image_sha256":"...","partition_bytes":3145728,"chunk_bytes":4096,"rollback":true}
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
cmd /c tools\test_kitsu_nvs_erase_guard.cmd
node --test tools/test_package_kitsu_ble_firmware.mjs
python tools/test_platformio_kitsu_upload_guard.py
python tools/test_migrate_kitsu_0203_storage.py
python tools/test_kitsu_reflashable_profile.py
```

The expanded-storage candidate identifies itself as firmware `0.20.3`. Do not
publish or flash these bytes under any already-used firmware
identity; the source version, package version, binary hash, and physical
acceptance record must agree exactly.

The generic serial candidate and stable packagers are retained only for their
physically accepted legacy layouts. They intentionally reject the `0.20.3`
CSV. PlatformIO may build and audit the post-migration image, but its physical
upload targets remain permanently blocked. Use the dedicated migration/restore
ceremony for USB transition or recovery, and signed BLE OTA for subsequent
field updates after the device reports the exact new layout.

The checked stable/public packager remains frozen to the last accepted release
until a new exact binary passes physical acceptance. Do not relabel an older
authorization/evidence digest or edit the stable pins speculatively.

## Two-record physical and delivery acceptance

Acceptance deliberately uses two retained records so no evidence digest is
self-referential. Use supported Heltec hardware, ordinary Android phones, and
only visible owner controls. ADB, Docker, and direct database repair are not
part of acceptance.

Record 1 is the prepublication candidate/hardware record. It binds the frozen
source and production builds, production-signed Android candidate, signed
`.kitsu-fw`, exact migration input manifest, both private full-flash backup
authorities, fresh pinned-IDF NVS-oracle result, and every dedicated migration
readback. On the one legacy-layout device used for transition acceptance, it
must prove the reviewed table-last migration/restore ceremony, exact preserved
NVS prefix and upper data, both new application slots, moved OTA data, blank
private journals, and the final partition-table commit. The legacy seven-write
Web Serial flow is explicitly out of scope and must never be used to install or
recover `0.20.3`.

After the dedicated transition succeeds, record 1 must also prove local pairing
and lost-receipt recovery, airplane-mode controls and messages, stable GATT,
per-device Disconnect/Forget, replay-window saturation as transient
`idempotency_busy` followed by expiry recovery, signed BLE OTA
resume/cancel/reboot, pending verification, rollback, and dedicated USB
restore. The 30-second OTA health gate includes controller storage, BLE
initialization, OTA image/journal/layout identity binding, and the live loop.
Record 1 does not contain the downstream public URL or delivery record. Its
frozen SHA-256 is bound externally by the final promotion/delivery record; it
is not embedded in the exact `kitsu.ble-firmware.v1` manifest.

Record 2 is the public-delivery smoke. After record 1 passes, re-inspect and
stage the exact already signed record-1 manifest and `.kitsu-fw` package behind
an atomic rollback. Record 2 must never create, rewrite, or sign a replacement
manifest: exact `kitsu.ble-firmware.v1` package byte identity is preserved from
record 1 through promotion. Then use the actual public HTTPS pages as an
ordinary owner: install Android fresh, pair, use local controls and messages in
airplane mode, Disconnect/reconnect, install the public `.kitsu-fw` through
authenticated BLE, confirm the new slot, and Forget/re-pair. Public delivery
must not expose a generic Web Serial migration, raw full-flash backup,
controller material, or a legacy seven-write recovery path. Dedicated USB
migration/restore stays a private, physically supervised recovery ceremony.
Record 2 binds the exact final BLE manifest/signature and public artifacts, but
its digest is never fed back into that manifest. Failure restores the prior
public release and remains retained as a failure.

The external final promotion decision binds both evidence digests, exact Android
APK/version/certificate, firmware and `.kitsu-fw` hashes, final Web Serial
manifest/signature only for any separately retained legacy release, the new
signed BLE manifest/signature, source commit, timestamps, device class, and
PASS/FAIL. It is not embedded into either manifest. Record 1 may hash and refer
to private full-flash backup authorities, but retained public evidence must not
contain their bytes. Do not collect controller secrets, private keys, message
text, precise location, or third-party traffic.

The authoritative numbered cases and evidence fields are in
`platform/mobile/android/qa/PHYSICAL-RELEASE-ACCEPTANCE.md`. Both records must
pass; a single circular evidence document may not authorize itself.

## Promotion and recovery

Only the exact already signed BLE package bytes exercised and accepted by
record 1 may be copied into the public tree and named by its metadata. They
must not be rebuilt, repackaged, or re-signed after record 1. Only after the
separate record-2 public-delivery smoke passes may that coordinated
Android/site/docs/status/BLE-update release be declared stable. The legacy Web
Serial manifest remains immutable historical authority; it is neither
rewritten nor used for the 0.20.3 layout. Keep the atomic rollback available
throughout.

After the one-time local-only retirement of the legacy LAN-action namespace, an
ordinary matching-layout update preserves the remaining NVS, companion state,
packs, controller authorization, MeshCore configuration, and coredump. A
whole-chip erase intentionally destroys them and requires separate explicit
owner authorization. Raw backups are sensitive because this reflashable design
does not provide physical flash confidentiality.
