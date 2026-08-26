import assert from "node:assert/strict";
import { createDecipheriv, createHash } from "node:crypto";
import { readFile } from "node:fs/promises";
import { resolve } from "node:path";

const wasmPath = resolve(process.argv[2] ?? "");
const packPath = resolve(process.argv[3] ?? "assets/packs/fox.k868");
const firmwareSourcePath = resolve("src/main.cpp");
const [wasm, pack, firmwareSource] = await Promise.all([
  readFile(wasmPath),
  readFile(packPath),
  readFile(firmwareSourcePath, "utf8"),
]);
const firmwareVersion = firmwareSource.match(
  /constexpr char FIRMWARE_VERSION\[\]\s*=\s*"([^"]+)"/,
)?.[1];
assert.ok(firmwareVersion, "firmware source must declare FIRMWARE_VERSION");
const module = await WebAssembly.compile(wasm);
assert.deepEqual(
  WebAssembly.Module.imports(module).map(({ module: namespace, name }) =>
    `${namespace}.${name}`),
  [
    "wasi_snapshot_preview1.fd_close",
    "wasi_snapshot_preview1.fd_write",
    "wasi_snapshot_preview1.fd_seek",
  ],
);

const encoder = new TextEncoder();

function deterministicEntropy(seed) {
  const output = new Uint8Array(48);
  let state = seed >>> 0;
  for (let index = 0; index < output.length; ++index) {
    state ^= state << 13;
    state ^= state >>> 17;
    state ^= state << 5;
    output[index] = state & 0xff;
  }
  return output;
}

async function instantiateModule({ deviceLow, deviceHigh, entropySeed }) {
  let memory;
  const writeU32 = (address, value) => {
    if (memory && address) {
      new DataView(memory.buffer).setUint32(address, value, true);
    }
  };
  const instance = await WebAssembly.instantiate(module, {
    wasi_snapshot_preview1: {
      fd_close: () => 0,
      fd_write: (_fd, _iov, _count, written) => {
        writeU32(written, 0);
        return 0;
      },
      fd_seek: (_fd, _low, _high, _whence, result) => {
        if (memory && result) {
          const view = new DataView(memory.buffer);
          view.setUint32(result, 0, true);
          view.setUint32(result + 4, 0, true);
        }
        return 0;
      },
    },
  });
  const api = instance.exports;
  memory = api.memory;
  api._initialize();
  assert.equal(api.kitsu_emulator_abi_version(), 2);
  assert.equal(api.kitsu_emulator_set_device_id(deviceLow, deviceHigh), 1);
  assert.equal(api.kitsu_emulator_boot(), 0,
    "firmware boot must fail closed before browser CSPRNG entropy arrives");
  const entropy = deterministicEntropy(entropySeed);
  writeBytes(api, api.kitsu_emulator_entropy_buffer(),
    api.kitsu_emulator_entropy_capacity(), entropy);
  assert.equal(api.kitsu_emulator_entropy_commit(entropy.byteLength), 1);
  assert.equal(api.kitsu_emulator_entropy_ready(), 1);
  return api;
}

function bootFirmware(api) {
  assert.equal(api.kitsu_emulator_boot(), 1);
  assert.equal(api.kitsu_emulator_set_device_id(0, 0), 0,
    "hardware identity must be immutable after firmware setup");
  assert.equal(api.kitsu_emulator_entropy_commit(32), 0,
    "the browser entropy source must be immutable after setup");
  for (let index = 0; index < 10; ++index) {
    assert.equal(api.kitsu_emulator_step(16), 1);
  }
  return api;
}

async function instantiateFirmware(options) {
  const api = await instantiateModule(options);
  assert.ok(pack.byteLength <= api.kitsu_emulator_pack_capacity());
  new Uint8Array(api.memory.buffer, api.kitsu_emulator_pack_buffer(), pack.length)
    .set(pack);
  assert.equal(api.kitsu_emulator_pack_commit(pack.length), 1);
  return bootFirmware(api);
}

async function instantiateRestored(options, snapshot) {
  const api = await instantiateModule(options);
  writeBytes(api, api.kitsu_emulator_persistence_buffer(),
    api.kitsu_emulator_persistence_capacity(), snapshot.persistence);
  assert.equal(api.kitsu_emulator_persistence_import(
    snapshot.persistence.length), 1);
  assert.equal(api.kitsu_emulator_ota_partition_bytes(),
    snapshot.ota[0].length);
  for (let slot = 0; slot < snapshot.ota.length; ++slot) {
    writeBytes(api, api.kitsu_emulator_ota_partition_buffer(slot),
      api.kitsu_emulator_ota_partition_bytes(), snapshot.ota[slot]);
  }
  assert.equal(api.kitsu_emulator_ota_restore_active_boot_slot(
    snapshot.activeBootSlot), 1);
  return bootFirmware(api);
}

function writeBytes(api, pointer, capacity, bytes) {
  assert.ok(bytes.length <= capacity);
  new Uint8Array(api.memory.buffer, pointer, bytes.length).set(bytes);
}

function serialText(api) {
  return Buffer.from(new Uint8Array(
    api.memory.buffer,
    api.kitsu_emulator_serial_buffer(),
    api.kitsu_emulator_serial_bytes(),
  )).toString("utf8");
}

function sendSerial(api, commands) {
  const bytes = encoder.encode(commands.endsWith("\n")
    ? commands
    : `${commands}\n`);
  writeBytes(api, api.kitsu_emulator_serial_input_buffer(),
    api.kitsu_emulator_serial_input_capacity(), bytes);
  assert.equal(api.kitsu_emulator_serial_input_commit(bytes.length), 1);
  for (let index = 0;
    index < 4000 && api.kitsu_emulator_serial_input_queued() !== 0;
    ++index) {
    assert.equal(api.kitsu_emulator_step(2), 1);
  }
  assert.equal(api.kitsu_emulator_serial_input_queued(), 0,
    "the production serial parser must drain the complete command");
  for (let index = 0; index < 4; ++index) {
    assert.equal(api.kitsu_emulator_step(4), 1);
  }
}

function debugView(api) {
  const bytes = api.kitsu_emulator_debug_view_bytes();
  assert.equal(bytes, 40 * Uint32Array.BYTES_PER_ELEMENT);
  const pointer = api.kitsu_emulator_debug_view();
  return Uint32Array.from(new Uint32Array(
    api.memory.buffer, pointer, bytes / Uint32Array.BYTES_PER_ELEMENT,
  ));
}

function bleStatusView(api) {
  const bytes = api.kitsu_emulator_ble_status_view_bytes();
  assert.equal(bytes, 22 * Uint32Array.BYTES_PER_ELEMENT);
  return Uint32Array.from(new Uint32Array(
    api.memory.buffer,
    api.kitsu_emulator_ble_status_view(),
    bytes / Uint32Array.BYTES_PER_ELEMENT,
  ));
}

function encodeGattFrame(text) {
  const payload = encoder.encode(text);
  const frame = new Uint8Array(4 + payload.length);
  new DataView(frame.buffer).setUint32(0, payload.length, false);
  frame.set(payload, 4);
  return frame;
}

function verifyBootFrameHistory(api) {
  const count = api.kitsu_emulator_frame_history_count();
  assert.ok(count >= 4 &&
    count <= api.kitsu_emulator_frame_history_capacity(),
  "the host must retain actual setup/hatch OLED presentations");
  let previousRevision = 0;
  let previousMillis = 0;
  const pixelCounts = new Set();
  for (let index = 0; index < count; ++index) {
    const pointer = api.kitsu_emulator_frame_history_frame(index);
    assert.notEqual(pointer, 0);
    const pixels = new Uint8Array(
      api.memory.buffer, pointer, api.kitsu_emulator_framebuffer_bytes(),
    );
    pixelCounts.add(pixels.reduce((sum, value) => sum + (value ? 1 : 0), 0));
    const revision = api.kitsu_emulator_frame_history_revision(index);
    const presentedAt = api.kitsu_emulator_frame_history_millis(index);
    assert.ok(revision > previousRevision);
    assert.ok(presentedAt >= previousMillis);
    assert.equal(api.kitsu_emulator_frame_history_display_on(index), 1);
    previousRevision = revision;
    previousMillis = presentedAt;
  }
  assert.ok(pixelCounts.size >= 2,
    "boot history must contain visually distinct real firmware frames");
  assert.ok(api.kitsu_emulator_framebuffer_revision() >= previousRevision);
  return count;
}

function snapshotPersistentState(api) {
  assert.equal(api.kitsu_emulator_persistence_schema_version(), 2);
  const persistenceBytes = api.kitsu_emulator_persistence_export();
  assert.ok(persistenceBytes > api.kitsu_emulator_pack_bytes());
  const persistence = Uint8Array.from(new Uint8Array(
    api.memory.buffer,
    api.kitsu_emulator_persistence_buffer(),
    persistenceBytes,
  ));
  const otaBytes = api.kitsu_emulator_ota_partition_bytes();
  const ota = [0, 1].map((slot) => Uint8Array.from(new Uint8Array(
    api.memory.buffer,
    api.kitsu_emulator_ota_partition_buffer(slot),
    otaBytes,
  )));
  return {
    persistence,
    ota,
    activeBootSlot: api.kitsu_emulator_ota_active_boot_slot(),
  };
}

function verifySecurityRecordInteroperability(persistence, hardwareId) {
  const bytes = Buffer.from(
    persistence.buffer, persistence.byteOffset, persistence.byteLength);
  const packBytes = bytes.readUInt32LE(12);
  const preferenceBytes = bytes.readUInt32LE(16);
  const platformBytes = bytes.readUInt32LE(20);
  const platformOffset = 36 + packBytes + preferenceBytes;
  assert.ok(platformOffset + platformBytes <= bytes.length);
  assert.equal(bytes.readUInt32LE(platformOffset), 1,
    "platform persistence schema must remain inspectable");
  const securityBytes = bytes.readUInt32LE(platformOffset + 4);
  const journalBytes = bytes.readUInt32LE(platformOffset + 8);
  assert.equal(12 + securityBytes + journalBytes, platformBytes);

  const securityOffset = platformOffset + 12;
  const securityEnd = securityOffset + securityBytes;
  assert.equal(bytes.readUInt32LE(securityOffset), 2,
    "device security must retain two production journal slots");

  const challenge = Buffer.alloc(32);
  Buffer.from("Kitsu868 wrap root v1", "ascii").copy(challenge);
  hardwareId.copy(challenge, 24);
  const wrappingKey = createHash("sha256").update(challenge).digest();
  challenge.fill(0);

  let cursor = securityOffset + 4;
  let decryptedRecords = 0;
  for (let slot = 0; slot < 2; ++slot) {
    assert.ok(cursor + 4 <= securityEnd);
    const recordBytes = bytes.readUInt32LE(cursor);
    cursor += 4;
    assert.ok(cursor + recordBytes <= securityEnd);
    if (recordBytes !== 0) {
      const record = bytes.subarray(cursor, cursor + recordBytes);
      assert.equal(record.subarray(0, 4).toString("ascii"), "KSEC");
      assert.equal(record.readUInt16LE(4), 2);
      assert.equal(record.readUInt16LE(6), 44);
      assert.equal(record.readUInt16LE(12), 320);
      assert.equal(record.readUInt16LE(14), 320);
      assert.equal(record.length, 44 + 320);
      const generation = record.readUInt32LE(8);
      const aad = Buffer.alloc(4);
      aad.writeUInt32BE(generation);
      const decipher = createDecipheriv(
        "aes-256-gcm", wrappingKey, record.subarray(16, 28),
        { authTagLength: 16 });
      decipher.setAAD(aad);
      decipher.setAuthTag(record.subarray(28, 44));
      const plaintext = Buffer.concat([
        decipher.update(record.subarray(44)), decipher.final(),
      ]);
      assert.equal(plaintext.subarray(0, 4).toString("ascii"), "KMAT",
        "standard AES-256-GCM must open the WASM security record");
      assert.equal(plaintext.readUInt16LE(4), 2);
      assert.equal(plaintext.readUInt16LE(6), 320);
      plaintext.fill(0);
      ++decryptedRecords;
    }
    cursor += recordBytes;
  }
  wrappingKey.fill(0);
  assert.equal(cursor, securityEnd);
  assert.ok(decryptedRecords >= 1,
    "production setup must persist at least one encrypted security record");
}

const target = await instantiateFirmware({
  deviceLow: 0x55667788,
  deviceHigh: 0x11223344,
  entropySeed: 0x13579bdf,
});
const peer = await instantiateFirmware({
  deviceLow: 0xddeeff00,
  deviceHigh: 0x99aabbcc,
  entropySeed: 0x2468ace1,
});

assert.equal(target.kitsu_emulator_framebuffer_width(), 64);
assert.equal(target.kitsu_emulator_framebuffer_height(), 128);
assert.equal(target.kitsu_emulator_framebuffer_bytes(), 8192);
const frame = new Uint8Array(
  target.memory.buffer,
  target.kitsu_emulator_framebuffer(),
  target.kitsu_emulator_framebuffer_bytes(),
);
const litPixels = frame.reduce((sum, value) => sum + (value ? 1 : 0), 0);
assert.ok(litPixels > 0, "firmware must render at least one OLED pixel");
const bootFrameCount = verifyBootFrameHistory(target);
const bootSerial = serialText(target);
assert.match(
  bootSerial,
  new RegExp(`KITSU_BOOT firmware=Kitsu868 version=${
    firmwareVersion.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
  }`),
);
assert.match(bootSerial, /KITSU_READY uid=/);

// The host replaces only NimBLE transport. Exercise the same encrypted,
// length-prefixed, MTU-chunked GATT carrier used by the production session.
let bleStatus = bleStatusView(target);
assert.equal(bleStatus[2], 1);
assert.equal(bleStatus[3], 1);
assert.equal(target.kitsu_emulator_ble_connect_authenticated(), 1);
assert.equal(target.kitsu_emulator_ble_set_mtu(23), 1);
assert.equal(target.kitsu_emulator_ble_set_notify_subscription(1), 1);
assert.equal(target.kitsu_emulator_step(1), 1);
bleStatus = bleStatusView(target);
assert.deepEqual(Array.from(bleStatus.slice(4, 10)), [1, 1, 1, 1, 1, 1]);
assert.equal(bleStatus[15], 23);

const malformedHello = encodeGattFrame("{}");
for (const chunk of [malformedHello.subarray(0, 2),
  malformedHello.subarray(2)]) {
  writeBytes(target, target.kitsu_emulator_ble_rx_chunk_buffer(),
    target.kitsu_emulator_ble_rx_chunk_capacity(), chunk);
  assert.equal(target.kitsu_emulator_ble_rx_chunk_commit(chunk.length), 1);
}
assert.equal(bleStatusView(target)[18], 1,
  "complete input stays queued until the real firmware loop runs");
assert.equal(target.kitsu_emulator_ble_tx_chunk_bytes(), 0);
assert.equal(target.kitsu_emulator_step(1), 1);

const gattResponseChunks = [];
let expectedGattResponseBytes = 0;
for (let index = 0; index < 16; ++index) {
  const chunkBytes = target.kitsu_emulator_ble_tx_chunk_bytes();
  if (chunkBytes !== 0) {
    assert.ok(chunkBytes <= 20, "MTU 23 allows at most 20 payload bytes");
    const revision = target.kitsu_emulator_ble_tx_chunk_revision();
    if (gattResponseChunks.length === 0) {
      assert.equal(target.kitsu_emulator_step(1), 1);
      assert.equal(target.kitsu_emulator_ble_tx_chunk_revision(), revision,
        "unconsumed notification must apply backpressure");
      assert.equal(target.kitsu_emulator_ble_tx_chunk_bytes(), chunkBytes);
    }
    gattResponseChunks.push(Buffer.from(new Uint8Array(
      target.memory.buffer,
      target.kitsu_emulator_ble_tx_chunk_buffer(),
      chunkBytes,
    )));
    const assembled = Buffer.concat(gattResponseChunks);
    if (assembled.length >= 4 && expectedGattResponseBytes === 0) {
      expectedGattResponseBytes = 4 + assembled.readUInt32BE(0);
    }
    const completesResponse = expectedGattResponseBytes !== 0 &&
      assembled.length === expectedGattResponseBytes;
    if (completesResponse) {
      assert.equal(bleStatusView(target)[13], 1,
        "the final staged response must still block a second request");
    }
    assert.equal(target.kitsu_emulator_ble_tx_chunk_consume(), 1);
    if (completesResponse) {
      break;
    }
  }
  assert.equal(target.kitsu_emulator_step(1), 1);
}
const gattResponse = Buffer.concat(gattResponseChunks);
assert.equal(gattResponse.length, expectedGattResponseBytes);
assert.match(gattResponse.subarray(4).toString("utf8"), /"auth_failed"/);
assert.equal(bleStatusView(target)[13], 0,
  "request-in-flight clears only after every framed TX chunk is accepted");
target.kitsu_emulator_ble_disconnect();
assert.equal(target.kitsu_emulator_step(1), 1);
bleStatus = bleStatusView(target);
assert.equal(bleStatus[4], 0);
assert.equal(bleStatus[3], 1);

const epoch = 1787600000;
for (const api of [target, peer]) {
  api.kitsu_emulator_serial_clear();
  sendSerial(api, `mesh config on\nmesh time ${epoch}`);
  const debug = debugView(api);
  assert.equal(debug[36], 1, "mesh settings must be enabled by real serial");
  assert.equal(debug[37], 1, "the real Kitsu transport must be active");
  assert.equal(debug[38], 1, "the fake physical radio must initialize");
  // The production serial `mesh time` command intentionally updates the
  // MeshCore RTC only; system wall-clock sync remains a BLE-session command.
  assert.equal(api.kitsu_emulator_epoch_valid(), 0);
  assert.match(serialText(api), /"action":"config","status":"ok"/);
  assert.match(serialText(api), /"action":"time","status":"ok"/);
}

sendSerial(target, "listen");
let targetDebug = debugView(target);
assert.equal(targetDebug[15], 1,
  "the target must enter the production RADIO/listen activity");
const curiosityBefore = targetDebug[28];

peer.kitsu_emulator_serial_clear();
sendSerial(peer, "mesh tx unlock\nmesh introduce mesh");
for (let index = 0; index < 40 && peer.kitsu_emulator_radio_tx_count() === 0;
  ++index) {
  assert.equal(peer.kitsu_emulator_step(16), 1);
}
assert.equal(peer.kitsu_emulator_radio_tx_count(), 1,
  "real MeshCore must serialize the signed advert to the fake radio");
const wireBytes = peer.kitsu_emulator_radio_tx_peek_length();
assert.ok(wireBytes > 100 && wireBytes <= 255);
assert.equal(peer.kitsu_emulator_radio_tx_copy(), wireBytes);
const signedAdvert = Uint8Array.from(new Uint8Array(
  peer.memory.buffer,
  peer.kitsu_emulator_radio_io_buffer(),
  wireBytes,
));
assert.equal(peer.kitsu_emulator_radio_tx_consume(), 1);

writeBytes(target, target.kitsu_emulator_radio_io_buffer(),
  target.kitsu_emulator_radio_io_capacity(), signedAdvert);
assert.equal(target.kitsu_emulator_radio_inject_rx(
  signedAdvert.length, -7000, 1200), 1);
for (let index = 0; index < 400; ++index) {
  assert.equal(target.kitsu_emulator_step(16), 1);
}
targetDebug = debugView(target);
assert.equal(targetDebug[28], curiosityBefore,
  "MeshCore adverts must not masquerade as nearby Kitsu encounters");
target.kitsu_emulator_serial_clear();
sendSerial(target, "selftest");
assert.match(serialText(target), /"mesh_adverts":1/,
  "the real parser and Ed25519 verifier must accept the signed peer advert");

const forgedAdvert = Uint8Array.from(signedAdvert);
forgedAdvert[50] ^= 0x80;
writeBytes(target, target.kitsu_emulator_radio_io_buffer(),
  target.kitsu_emulator_radio_io_capacity(), forgedAdvert);
assert.equal(target.kitsu_emulator_radio_inject_rx(
  forgedAdvert.length, -6000, 1500), 1);
for (let index = 0; index < 240; ++index) {
  assert.equal(target.kitsu_emulator_step(16), 1);
}
assert.equal(debugView(target)[28], curiosityBefore,
  "a signature-corrupted raw advert must not reach the encounter state machine");
target.kitsu_emulator_serial_clear();
sendSerial(target, "selftest");
assert.match(serialText(target), /"mesh_adverts":1/,
  "a signature-corrupted advert must not enter the MeshCore advert journal");

// A browser RST creates a fresh module, restores only durable components,
// and boots the same production setup again. Prove core/brain/mesh/pack state
// plus both virtual OTA partitions and the selected boot slot survive it.
sendSerial(target, "pet");
const stateBeforeReset = debugView(target);
const otaBytes = target.kitsu_emulator_ota_partition_bytes();
const otaSentinelOffset = 4096;
assert.ok(otaSentinelOffset < otaBytes);
new Uint8Array(
  target.memory.buffer,
  target.kitsu_emulator_ota_partition_buffer(1),
  otaBytes,
)[otaSentinelOffset] = 0x5a;
assert.equal(target.kitsu_emulator_ota_restore_active_boot_slot(1), 1);
const snapshot = snapshotPersistentState(target);
assert.equal(Buffer.from(snapshot.persistence.subarray(0, 4)).toString("ascii"),
  "KPS2");
const targetHardwareId = Buffer.alloc(8);
targetHardwareId.writeUInt32LE(0x55667788, 0);
targetHardwareId.writeUInt32LE(0x11223344, 4);
verifySecurityRecordInteroperability(snapshot.persistence, targetHardwareId);
targetHardwareId.fill(0);

const restored = await instantiateRestored({
  deviceLow: 0x55667788,
  deviceHigh: 0x11223344,
  entropySeed: 0x10203040,
}, snapshot);
const stateAfterReset = debugView(restored);
for (const index of [27, 28, 29, 30, 31, 32, 33, 34, 36]) {
  assert.equal(stateAfterReset[index], stateBeforeReset[index],
    `persistent debug field ${index} must survive module replacement`);
}
assert.equal(stateAfterReset[39], stateBeforeReset[39] + 1,
  "restored production setup must record the next boot");
assert.equal(restored.kitsu_emulator_pack_bytes(), pack.length);
assert.equal(restored.kitsu_emulator_ota_active_boot_slot(), 1);
assert.equal(new Uint8Array(
  restored.memory.buffer,
  restored.kitsu_emulator_ota_partition_buffer(1),
  otaBytes,
)[otaSentinelOffset], 0x5a);
assert.equal(restored.kitsu_emulator_persistence_import(
  snapshot.persistence.length), 0,
"durable state cannot be replaced underneath a running firmware instance");

console.log(JSON.stringify({
  booted: target.kitsu_emulator_booted() === 1,
  loops: "real main loop",
  memoryBytes: target.memory.buffer.byteLength,
  framebufferBytes: frame.byteLength,
  litPixels,
  packBytes: target.kitsu_emulator_pack_bytes(),
  serialBytes: target.kitsu_emulator_serial_bytes(),
  ready: /KITSU_READY uid=/.test(bootSerial),
  meshWireBytes: wireBytes,
  signedAdvertAccepted: true,
  forgedAdvertRejected: true,
  bootFrameCount,
  bleCarrier: "length-framed, MTU-chunked",
  securityRecord: "standard AES-256-GCM interoperable",
  persistenceBytes: snapshot.persistence.length,
  resetRestored: true,
}));
