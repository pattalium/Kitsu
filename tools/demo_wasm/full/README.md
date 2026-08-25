# Kitsu full-firmware WebAssembly target

This target compiles the unmodified production `src/main.cpp`, twenty other
production firmware modules, and the pinned MeshCore client implementation
with Emscripten. It supplies browser-side implementations only at hardware and
operating-system boundaries: Arduino time/GPIO/serial, OLED, preferences/NVS,
flash partitions, ESP security primitives, NimBLE GATT, and the physical
SX1262. The normal Heltec/PlatformIO build is unchanged.

Build offline with the pinned canonical Emscripten 6.0.5 container:

```powershell
tools\demo_wasm\full\build.ps1
```

The script uses `--network none`, hashes the result, and writes only a
content-addressed file below `dist`. Verify a result with the actual fox pack:

```powershell
node tools\demo_wasm\full\verify.mjs `
  tools\demo_wasm\full\dist\kitsu-firmware-full.<sha256>.wasm `
  assets\packs\fox.k868
```

The application boundary is deliberately explicit. The creature state
machine, menus, timing, PRG handling, rendering, fox-pack parser/playback,
games, brain, discovery journal, BLE session/protocol, OTA state machine,
`KitsuMeshTransport`, and MeshCore packet parsing/routing/cryptography/dedup/
scheduling are production sources. The old `injected_mesh_transport.cpp`
scaffold remains only as historical source and is excluded from `build.rsp`.

The browser radio adapter receives and emits complete MeshCore wire frames.
JavaScript can stage at most 255 raw bytes in
`kitsu_emulator_radio_io_buffer`, inject them with RSSI/SNR through
`kitsu_emulator_radio_inject_rx`, and inspect exact serialized TX frames with
the `kitsu_emulator_radio_tx_*` queue API. It cannot inject decoded adverts or
messages. `verify.mjs` creates two firmware instances with distinct identities,
uses the real serial command parser to enable their radios, captures a signed
advert from one instance, injects it into the other, and confirms that a
signature-corrupted variant is rejected.

The browser BLE adapter replaces only NimBLE callbacks and notifications. RX
is still the frozen four-byte big-endian length frame, is bounded to real
512-byte GATT writes, passes through the production `LengthFrameParser`, and
is delivered to the production session only from the firmware loop. Link
encryption/authentication/bonding gates, handshake-versus-session limits,
assembly timeouts, one-request-in-flight behavior, notify subscription, and
MTU chunking/backpressure are preserved. A host stages physical RX writes with
the `kitsu_emulator_ble_rx_chunk_*` API and consumes notification chunks with
`kitsu_emulator_ble_tx_chunk_*`; it cannot call the session delegate directly.

Browser reset persistence is intentionally component-based. The compact,
versioned, CRC-protected `kitsu_emulator_persistence_*` blob contains the
committed companion pack, Preferences, device-security slots, discovery
journal, and MeshCore message NVS. The two large virtual OTA app partitions
and active boot selection are exposed separately through
`kitsu_emulator_ota_*` so they are not copied into WebAssembly memory twice.
To model RST, instantiate a new module, call `_initialize`, set the same stable
hardware ID, commit fresh browser entropy, import the compact blob, restore
both OTA buffers and the active slot, and only then call
`kitsu_emulator_boot`. Runtime clocks, button state, entropy/DRBG state,
BLE/radio queues, and transient UI globals are deliberately not persisted.

Before boot, the browser must fill `kitsu_emulator_entropy_buffer` with 32 to
64 bytes from `crypto.getRandomValues()` and commit it exactly once with
`kitsu_emulator_entropy_commit`. The HAL expands that seed through its
HMAC-SHA256 DRBG for every `esp_random`/security request; boot fails closed if
entropy was not supplied. Entropy/DRBG state is volatile and a newly
instantiated module receives a fresh browser seed. The verifier uses named,
deterministic entropy only to make its two-peer protocol proof reproducible.

The OLED HAL exposes the current 64x128 portrait framebuffer plus a bounded
chronological history of actual firmware presentations, allowing the browser
to replay the real boot/hatch sequence. Serial input is a bounded queue that
is consumed by the unchanged production `pollSerial()` path. `verify.mjs`
also checks split GATT RX, MTU-chunked TX backpressure, serial draining, boot
frame history, and a fresh-module persistence/OTA reset round trip.

The selected AES and Ed25519 verification sources from the firmware's pinned
`rweather/Crypto` 0.4.0 dependency are vendored under
`vendor/crypto-0.4.0`; provenance and the canonical archive digest are recorded
there. MeshCore's own pinned Ed25519 C implementation remains responsible for
identity creation, signing, and key exchange, matching the device build.

This is source-identical application and protocol logic over a browser HAL,
not instruction-level ESP32-S3 emulation. Physical RF/CAD/noise/IRQ timing,
NimBLE radio/SMP, flash wear, boot-ROM behavior, and the ESP bootloader are
modeled boundaries. The public UI must continue to identify it as demo mode
and must not describe injected radio traffic or virtual flashing as a live
device function.
