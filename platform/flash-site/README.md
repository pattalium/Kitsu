# K32 Kitsu legacy-layout Web Serial recovery

This static browser application uses Espressif's Apache-2.0 `esptool-js`
runtime, bundled into the release by Vite. It does not load scripts, flash
code, manifests, or keys from a CDN.

This application is deliberately frozen to the exact reviewed pre-`0.20.3`
partition layout. It is not the `0.20.2` to `0.20.3` migration tool and must
never write a board that already has the 256 KiB NVS layout. On every
connection it reads the complete physical partition-table sector; only the
exact legacy 3,072-byte table SHA-256 with an erased 1 KiB sector tail is
eligible. The same sector is read and classified again after owner
confirmation and immediately before the first possible mutation. The exact
`0.20.3` table, any unknown table, a short read, changed bytes, or non-erased
tail fail closed with zero writes.

The one-time storage transition is a separate private, table-last serial
ceremony with two full-flash backups and a fresh pinned-IDF NVS mount oracle.
After migration, ordinary field updates use Android and a signed `.kitsu-fw`
package. The historical offsets below remain documented solely so this
legacy-layout recovery implementation and its immutable signed manifest can
be audited; they are not current-layout instructions.

The release gate downloads `latest.json`, its raw 64-byte detached Ed25519
signature, and the public SPKI from `updates.k32.run`. The SPKI must match the
installed Kitsu authority fingerprint before the exact signed JSON bytes are
accepted. All five unique artifact files are SHA-256 verified before an install is enabled and
the latest signed manifest and artifacts are fetched again immediately before
writing. All seven signed core regions are then read from flash and hash-verified.

For an eligible legacy-layout board only, the signed v2 manifest and runtime
both constrain the core phase to exactly seven writes, in order:

1. the reviewed rollback-enabled Kitsu bootloader at `0x000000`;
2. the reviewed 3,072-byte Kitsu partition table at `0x008000`;
3. the ESP32-S3 application within app0 at `0x010000`;
4. an erased 4 KiB OTA journal at `0x33f000`;
5. the exact same application bytes within app1 at `0x340000`;
6. the same erased 4 KiB OTA journal at `0x66f000`; and
7. a 256 KiB all-`0xff` retirement image over the isolated legacy
   `kitsu_conn` partition at `0x7b0000`.

Writing both A/B slots is required because the preserved OTA-data partition may
select either slot after a Bluetooth update or rollback. A serial recovery must
therefore make both possible boot selections refer to the same accepted image;
an older two-write manifest is rejected rather than risking a stale app1 boot.
Both private OTA journals are erased so a signed USB recovery cannot inherit a
receiving, ready, or confirmed Bluetooth-update record from an older image.
The final write removes historical Wi-Fi, gateway, mTLS, and backend secrets;
it cannot overlap NVS controller records, companion packs, MeshCore state, or
the coredump partition. Local-only firmware independently verifies the same
retirement for upgrade paths that do not pass through this installer.

The exact bootloader write is required for a stock or older Heltec because A/B
rollback is a bootloader feature. Its bytes and SHA-256 are bound by the signed
manifest and physical-acceptance record and read back like every other region.
It remains an ordinary, replaceable ESP32-S3 bootloader and does not enable
Secure Boot, Flash Encryption, anti-rollback eFuses, or a debug lock.

After those seven core regions pass readback, the owner may use the same open
Web Serial/ROM-loader session to install one companion bundle into the single
slot at `0x670000`. Cat, Fox, and Dog remain the only built-in starter choices
and are pinned by exact size and SHA-256. An owner can instead choose a local
unlocked `.k868` file. The browser does not upload that file: it validates the
K868PK1 version, header and fixed layout, canvas and count bounds, clip and step
references, display name, slot boundary, payload CRC32, and header CRC32 before
computing its readback SHA-256. The selected bundle is written in a separate
bounded phase and read back before the one final hard reset. "Keep current
pet" is forced on every page load and performs no companion-partition write,
including no write to a private or unlocked pack already on the board.

Before any pet replacement, the installer reads both transaction sectors and
then structurally validates the physical pack. A same-ID revision update
preserves progression. A different pack ID is a destructive species
replacement and requires a second, species-specific confirmation. Its 40-byte
CRC-protected intent binds the validated old pack ID to the new pack ID,
revision, length, header CRC, and payload CRC. Identical copies use separate
4 KiB sectors in the already-retired `kitsu_conn` area: PREPARED at `0x7b0000`
and COMMITTED at `0x7b1000`.

For a fresh replacement, the normal signed core clear is read back first. The
browser then writes and reads back PREPARED before writing any target-pack byte.
Only after the entire target pack passes exact SHA-256 readback does it write
and read back COMMITTED. A power or USB loss can therefore leave the source
identity intact without authorizing a half-written target. Firmware requires
both structurally valid, byte-identical records, the stored source ID, and all
validated target metadata before companion state may be reset.

On reconnect, a valid PREPARED record remains the sole source of the old pet
identity even if COMMITTED is erased, torn, or mismatched and even if the
physical pack is already the target, partial, or invalid. Such a COMMITTED
sector is treated as uncommitted, never as authorization. Only the exact target
bound by PREPARED may retry; the browser never infers the source from the
post-failure physical pack. A retry keeps both transaction sectors untouched,
derives an erased suffix from the already SHA-verified signed retirement image,
and writes/readbacks only `0x7b2000..0x7effff`. It then verifies the retained
PREPARED sector again before touching the target and replaces COMMITTED only
after target readback. This avoids a retry-time erase window in which a power
loss could otherwise discard the only saved source ID.

The default Preserve choice is deliberately blocked while PREPARED is pending
and the physical pack is partial, invalid, already the target, or any ID other
than the saved source. Running the ordinary full core clear in that state would
silently discard the recovery record. Preserve may cancel a stale transaction
only when the complete physical pack validates with the saved source ID; then
no companion-pack byte or pet progress is changed. Malformed PREPARED and
COMMITTED-without-PREPARED states fail closed and block companion writes.

Firmware validates canonical record bytes and all-`0xff` sector padding before
retirement. With PREPARED-only or torn COMMITTED, it preserves the PREPARED
sector and erases/verifies the remaining retired tail. With a canonical,
byte-identical pair, it preserves both transaction sectors and retires only the
tail. It consumes both records only after durably saving an explicitly
authorized target identity. A missing, interrupted, stale, mismatched, or
non-durable transaction cannot clear companion state: firmware quarantines the
physical pack mismatch and loads the brain using the stored original pack ID.
Thus neither a failed pack write nor a temporary starter pack can overwrite the
existing companion. The transaction never borrows space from the companion
slot: the full original `0x140000`-byte `.k868` capacity, format, and
structural/CRC trust model remain unchanged.

If both transaction sectors are erased but the physical pack is invalid, the
owner may explicitly select a fully verified pack as a byte-level repair. This
path writes no PREPARED or COMMITTED record and therefore cannot authorize a
species reset. Firmware activates the repaired pack only when its ID matches
durable companion state or when migrating a legacy packless device; otherwise
it quarantines the pack. A true empty-slot first assignment preserves legacy
vitals while establishing the pack brain identity and clearing pack-specific
traits and gifts.

There is no full-chip erase command and no OTA-data, controller-store,
MeshCore-state, coredump, or eFuse write path. The browser never edits NVS;
only firmware may reset companion state after validating and consuming the
explicit one-shot species-replacement authorization. Flash writes necessarily
erase only the target flash sectors before programming them; `eraseAll` remains
false.

Run `npm ci` followed by `npm run check`. Deploy only `dist/`, never this source
tree or `node_modules/`. Physical browser acceptance still requires a legacy-
layout Heltec V3 connected to current desktop Chrome or Edge over HTTPS. A
migrated or unrecognized board must remain blocked and be directed to the
signed Android OTA or the dedicated restore ceremony.
