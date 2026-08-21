# MeshCore upstream provenance

This directory contains a deliberately small, pinned source subset of the
official MeshCore firmware library.

- Repository: <https://github.com/meshcore-dev/MeshCore>
- Release tag: `companion-v1.17.1`
- Commit: `d92964352441e53b93e8667b802e04f6e072b39e`
- Commit date: 2026-08-14
- Vendored on: 2026-08-16
- MeshCore license: MIT; see `LICENSE-MeshCore.txt`
- Bundled Ed25519 license: see `src/ed25519/license.txt`

## Included upstream files

The following files were copied from the pinned commit:

- `src/MeshCore.h`
- `src/Utils.h`, `src/Utils.cpp`
- `src/Packet.h`, `src/Packet.cpp`
- `src/Identity.h`, `src/Identity.cpp`
- `src/Dispatcher.h` (see the local lifecycle hook below),
  `src/Dispatcher.cpp`
- `src/Mesh.h`, `src/Mesh.cpp` (see the local safety patch below)
- `src/helpers/AdvertDataHelpers.h`,
  `src/helpers/AdvertDataHelpers.cpp`
- `src/helpers/UTF8Helpers.h`
- `src/helpers/StaticPoolPacketManager.h`,
  `src/helpers/StaticPoolPacketManager.cpp`
- `src/helpers/SimpleMeshTables.h`
- `src/helpers/ArduinoHelpers.h`
- `src/helpers/RefCountedDigitalPin.h`
- `src/helpers/radiolib/RadioLibWrappers.h`,
  `src/helpers/radiolib/RadioLibWrappers.cpp`
- `src/helpers/radiolib/CustomSX1262.h`
- `src/helpers/radiolib/CustomSX1262Wrapper.h`
- `src/helpers/radiolib/SX126xReset.h`
- every upstream file from `lib/ed25519`, relocated unchanged to
  `src/ed25519` so PlatformIO compiles it with this library

The root `library.json` and `src/ed_25519.h` forwarding header are Kitsu
packaging files. They are not upstream files. The forwarding header preserves
upstream `Identity.cpp` unchanged after the Ed25519 directory relocation.

### Local safety patch

Kitsu adds one bounds check to `src/Mesh.cpp` before the upstream PATH decoder
forms pointers into decrypted plaintext.  It verifies that the encoded path
bytes and required extra-type byte fit inside the authenticated plaintext.
This prevents an authenticated malformed PATH packet from underflowing
`extra_len`.  No wire format, valid packet behavior, or cryptographic primitive
is changed.  Keep this patch when refreshing the vendor snapshot unless the
selected upstream release contains an equivalent check.

### Local lifecycle hook

Kitsu adds one protected, read-only `currentOutboundPacket()` accessor to
`src/Dispatcher.h`. MeshCore removes a packet from `PacketManager` before its
asynchronous radio send completes; this accessor lets Kitsu's immediate TX
lock cancel only packets that are still queued while preserving honest
`sent`/failure and direct-ACK tracking for the packet already on air. It does
not alter scheduling, packet ownership, RF behavior, or the wire protocol.

## Intentionally excluded

Kitsu constructs/signs/verifies advertisements and implements the smallest
plain direct/group text layer with the core `Mesh` primitives already in this
snapshot. It still excludes upstream examples, Companion protocol/UI/BLE/Wi-Fi
interfaces, `BaseChatMesh`, its filesystem stores, GPS and environmental
sensors, repeaters, room servers, bridges, OTA, CLI, and display code. Contact,
channel, message-ring and delivery state are Kitsu-owned bounded structures;
stock helper/storage code is not vendored. Those exclusions also avoid pulling
in RTClib, Melopero RV3028, CayenneLPP, Base64, and sensor libraries.

The only dependency declared by this vendored library is
`rweather/Crypto@0.4.0`. RadioLib remains an application-level dependency.
MeshCore `companion-v1.17.1` pins RadioLib commit
`6d8934836678d8894e3d556550475b37dce3e2b6`; Kitsu should use that exact
commit or explicitly document and test a different pin. The application must
compile with `RADIOLIB_STATIC_ONLY=1` and `RADIOLIB_GODMODE=1`, matching
upstream's build requirements for the custom SX1262 wrapper.

## Upgrade rule

Do not copy files from a moving branch. Select a released Companion tag,
record its full commit, compare every included file against this inventory,
run host tests, build the Heltec V3 firmware, and perform receive-only testing
before allowing any transmission. Revisit the excluded-source list whenever
the transport grows beyond advertisements.
