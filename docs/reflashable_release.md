# Kitsu owner-reflashable release contract

This is the authoritative physical-device security and release contract for
Kitsu868. Kitsu deliberately remains eraseable, serial-reflashable, and
repurposable like a stock MeshCore board. No release, installer, signing
ceremony, or operator is authorized to burn an eFuse or disable an owner
recovery/debug interface.

The withdrawn Secure Boot/Flash Encryption ceremony is retained only for
forensic history in [production_provisioning.md](production_provisioning.md).
Its artifacts are not Kitsu releases and its commands must not be executed.

## Security boundary

The reflashable image reports these exact device facts:

| Field | Required value | Meaning |
|---|---:|---|
| `security_mode` | `"reflashable"` | Owner recovery and repurposing are intentional product behavior. |
| `secure_boot` | `false` | ROM does not enforce a Kitsu signing key. |
| `flash_encryption` | `false` | Raw flash is not confidential against physical access. |
| `nvs_encryption` | `false` | There is no hardware-backed encrypted-NVS boundary. |
| `hardware_root_protected` | `false` | Application secrets have no irreversible hardware root. |
| `application_encrypted` | `true` | `kitsu_conn` records use application-layer authenticated encryption and rotation. |
| `remote_connectivity_allowed` | boolean | Becomes true after the application security store initializes; it is never inferred from eFuse state. |

`application_encrypted:true` is not a physical-security claim. The wrapping
material is available to reflashable software. An operator or attacker with
physical flash, UART, USB, or JTAG access can extract state, replace firmware,
erase identity, or make replacement firmware use or bypass application keys.

The following remain enabled and unburned:

- ROM serial download and ordinary `esptool` write/verify operations;
- deliberate whole-chip erase and clean reflash;
- UART and USB download/recovery paths;
- JTAG/debug capability provided by the unmodified board/chip state; and
- all Secure Boot, Flash Encryption, anti-rollback, HMAC-root,
  download-mode-lock, JTAG-lock, UART-lock, and USB-lock eFuses.

No Kitsu release may claim verified boot, anti-rollback, flash
confidentiality, malicious-reflash resistance, or physical-tamper resistance.

## Protections that remain mandatory

Reflashability does not weaken the normal network/application boundary. The
release must retain:

- Bluetooth LE Secure Connections, numeric comparison, physical PRG consent,
  authenticated controller sessions, sequence/replay checks, and bounded
  framing;
- authenticated BLE for Wi-Fi, gateway trust, and one-use owner enrollment;
- TLS 1.2 or newer with CA, hostname/SNI, SPKI, ALPN, and time validation;
- device mTLS with certificate/key binding and canonical companion URI SAN;
- backend owner authorization, certificate-bound gateway identity, signed
  remote actions, expiry, durable replay protection, and strict ACK handling;
  and
- application-layer authenticated encryption, CRC/readback verification,
  generation rotation, and power-loss recovery for `kitsu_conn`.

These controls protect normal Bluetooth, LAN, and Internet use. Replacement
firmware installed through physical access can bypass them; that is the
explicit tradeoff for owner repurposability.

## Build profile

The default owner image is:

```text
PlatformIO environment: heltec_wifi_lora_32_V3_reflashable
compile marker:          KITSU_SECURITY_MODE_REFLASHABLE=1
partition layout:        partitions_kitsu_8MB.csv
```

The image must not enable Secure Boot signing/padding, Flash Encryption,
encrypted NVS, anti-rollback, first-boot eFuse mutation, or interface locks.
The withdrawn `heltec_wifi_lora_32_V3_production` environment and its
`partitions_kitsu_production_8MB.csv` layout are forensic inputs only and are
not release profiles.

## Machine-readable release manifest

`tools/package_kitsu_reflashable.py` creates the strict
`kitsu.firmware-reflashable-release.v1` manifest. Its exact top-level fields
are:

```text
schema
created_at
artifact_status
firmware_version
release_channel
device_class
checksum_index
build_profile
partition_layout
security_profile
network_security
release_requirements
flash_artifacts
serial_flash
warnings
```

Before physical hardware QA, the only valid publication state is:

```text
artifact_status: release-candidate-owner-reflashable
release_channel: candidate
```

The manifest contains no device ID or device-specific secret. Its
`security_profile` states:

```json
{
  "mode": "reflashable",
  "secure_boot": false,
  "flash_encryption": false,
  "nvs_encryption": false,
  "hardware_root_protected": false,
  "firmware_images_encrypted": false,
  "application_layer_encryption": true,
  "efuse_writes": false,
  "efuse_locks": false,
  "jtag_disabled": false,
  "uart_download_disabled": false,
  "usb_download_disabled": false,
  "serial_erase_reflash_available": true,
  "full_chip_erase_available": true,
  "stock_meshcore_restore_available": true,
  "physical_extraction_reflash_can_bypass": true
}
```

`release_requirements` must set device-specific XTS/HMAC secrets, Secure Boot
signing/recovery/rotation keys, and all eFuse operations to false. Every flash
artifact records its exact role, file, address, byte count, and SHA-256. ESP
images are structurally validated where applicable; every artifact states
`secure_boot_signed:false` and `encrypted:false`. `SHA256SUMS.txt` covers the
complete immutable output.

After a clean build, create the candidate bundle with the checked-in wrapper
(the output directory must be new or empty):

```powershell
tools\package_kitsu_reflashable.cmd `
  dist\kitsu-0.11.0-owner-reflashable `
  0.11.0
```

The wrapper fixes the project root, the
`.pio\build\heltec_wifi_lora_32_V3_reflashable` input, and the repository's
validated `esptool` implementation. The packager has no device-ID, private-key,
signing-stage, XTS, HMAC, or eFuse option.

Audit both the selected profile and the packager's positive/negative contract
before using an artifact:

```powershell
tools\test_kitsu_reflashable_profile.cmd
tools\test_package_kitsu_reflashable.cmd
```

The former production packager, staged signer, readiness audit, and production
build guard entry points terminate with
`KITSU_WITHDRAWN_NOT_AUTHORIZED`. Their helper source remains available only
for forensic review and negative tests; it is not a second release path.

The manifest carries exactly these stable warnings:

- `PHYSICAL_ACCESS_CAN_REPLACE_FIRMWARE`
- `NO_VERIFIED_BOOT_CHAIN`
- `APPLICATION_ENCRYPTION_NOT_HARDWARE_ROOTED`
- `SERIAL_RECOVERY_INTENTIONALLY_PRESERVED`
- `NETWORK_AUTH_RETAINED`

A detached release signature can authenticate distribution metadata to the
owner, but the ESP32-S3 boot path deliberately does not enforce it. The owner
can always choose and serial-flash different firmware.

## Release and hardware QA gates

A candidate is acceptable for controlled hardware testing only after:

1. a clean build of `heltec_wifi_lora_32_V3_reflashable` succeeds;
2. host suites for device-security reporting, encrypted connectivity storage,
   authenticated enrollment, bootstrap, steady TLS/LAN, replay, and remote
   actions pass;
3. the reflashable packager validates the exact PlatformIO flash arguments,
   partition table, image headers, offsets, sizes, and checksums;
4. static scans find no eFuse burn, first-boot encryption, secure-download
   lock, JTAG/UART/USB disable, Secure Boot, or Flash Encryption behavior in
   the active reflashable profile;
5. a pre-flash full backup is captured and verified when preserving the
   current board state matters; and
6. the owner explicitly authorizes the reversible serial write to the named
   port and exact artifact set.

Hardware acceptance then verifies:

- the exact security projection above;
- authenticated BLE and explicit Disconnect behavior;
- Wi-Fi/gateway configuration, PRG-confirmed enrollment, bootstrap, steady
  mTLS, queue/ACK/replay, and signed remote actions;
- companion pack and retained-state behavior across an ordinary update; and
- ROM serial recovery remains available.

A deliberate whole-chip erase/reflash recovery drill is destructive and must
use a disposable board or separate explicit owner authorization. It is never
implied by a build or test command. Passing a recovery drill proves
repurposability, not confidentiality or verified boot.

Promotion from `candidate` requires recorded physical-device results and a new
manifest/release decision. It never requires an eFuse, signing-custody, XTS,
HMAC, or irreversible provisioning ceremony.

## Owner recovery

An ordinary matching-layout update should preserve NVS, `kitsu_conn`, and the
pack partition. A whole-chip erase intentionally destroys them. Before an
erase, export anything the owner wants to retain; after an erase, the owner can
flash stock MeshCore or any other compatible image. Kitsu-specific identity,
controller authorization, Wi-Fi, gateway enrollment, progression, and packs
must then be restored or recreated deliberately.

No recovery instruction may imply that application encryption makes a raw
physical backup secret-safe. Handle backups as sensitive owner data.
