import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFile } from "node:fs/promises";
import test from "node:test";

import {
  buildReplacementIntent,
  companionPackTransition,
  inspectInstalledPack,
  inspectReplacementTransaction,
  PACK_CATALOG,
  PACK_SLOT,
  REPLACEMENT_TRANSACTION,
  replacementTransactionTargets,
  UNLOCKED_PACK_ID,
  loadUnlockedPack,
  packDefinition,
  reverifyPack,
  validateUnlockedPackBytes,
  verifyPackBytes,
} from "../src/packs.js";

const root = new URL("../../../", import.meta.url);

function localFile(name, source, declaredSize = source.byteLength) {
  const bytes = Uint8Array.from(source);
  return {
    name,
    size: declaredSize,
    async arrayBuffer() {
      return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
    },
  };
}

function testCrc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = (crc & 1) === 1 ? (crc >>> 1) ^ 0xedb88320 : crc >>> 1;
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function promoteToV2(source) {
  const original = Uint8Array.from(source);
  const originalView = new DataView(
    original.buffer,
    original.byteOffset,
    original.byteLength,
  );
  const frameCount = originalView.getUint16(0x24, true);
  const clipCount = originalView.getUint16(0x26, true);
  const stepCount = originalView.getUint32(0x28, true);
  const framesOffset = 64 + clipCount * 12 + stepCount * 4;
  const bytes = new Uint8Array(original.byteLength + frameCount * (640 - 512));
  bytes.set(original.subarray(0, framesOffset));
  for (let index = 0; index < frameCount; index += 1) {
    bytes.set(
      original.subarray(framesOffset + index * 512, framesOffset + (index + 1) * 512),
      framesOffset + index * 640,
    );
  }
  const view = new DataView(bytes.buffer);
  view.setUint16(0x08, 2, true);
  view.setUint32(0x0c, bytes.byteLength, true);
  view.setUint16(0x22, 80, true);
  view.setUint32(0x10, testCrc32(bytes.subarray(64)), true);
  const headerForCrc = bytes.slice(8, 64);
  headerForCrc.fill(0, 0x14 - 8, 0x18 - 8);
  view.setUint32(0x14, testCrc32(headerForCrc), true);
  return bytes;
}

test("official companion catalog pins only Cat, Fox, and Dog to the one pack slot", async () => {
  assert.deepEqual(Object.keys(PACK_CATALOG).sort(), ["cat", "dog", "fox"]);
  assert.deepEqual(PACK_SLOT, {
    offset: 0x670000,
    bytes: 0x140000,
  });
  assert.deepEqual(REPLACEMENT_TRANSACTION, {
    prepared: { offset: 0x7b0000, bytes: 0x1000 },
    committed: { offset: 0x7b1000, bytes: 0x1000 },
  });
  assert.equal(PACK_SLOT.offset + PACK_SLOT.bytes, 0x7b0000);
  assert.equal(REPLACEMENT_TRANSACTION.prepared.offset, PACK_SLOT.offset + PACK_SLOT.bytes);
  assert.equal(
    REPLACEMENT_TRANSACTION.committed.offset,
    REPLACEMENT_TRANSACTION.prepared.offset + REPLACEMENT_TRANSACTION.prepared.bytes,
  );

  for (const definition of Object.values(PACK_CATALOG)) {
    const bytes = new Uint8Array(await readFile(new URL(`assets/packs/${definition.id}.k868`, root)));
    assert.equal(bytes.byteLength, 24976, definition.id);
    assert.equal(createHash("sha256").update(bytes).digest("hex"), definition.sha256, definition.id);
    await verifyPackBytes(definition, bytes);
    await reverifyPack({
      definition,
      record: {
        role: `companion_pack_${definition.id}`,
        offset: PACK_SLOT.offset,
        bytes: definition.bytes,
        sha256: definition.sha256,
      },
      bytes,
    });
  }
});

test("installed-pack inspection validates the current slot before replacement", async () => {
  const fox = new Uint8Array(await readFile(new URL("assets/packs/fox.k868", root)));
  const loader = {
    async readFlash(offset, length) {
      assert.equal(offset, PACK_SLOT.offset);
      return fox.slice(0, length);
    },
  };
  const installed = await inspectInstalledPack(loader);
  assert.deepEqual(
    {
      status: installed.status,
      name: installed.name,
      packId: installed.packId,
      revision: installed.revision,
      bytes: installed.bytes,
    },
    {
      status: "valid",
      name: "FOX",
      packId: 0x6c393e21,
      revision: 2,
      bytes: 24976,
    },
  );

  const empty = await inspectInstalledPack({
    async readFlash() { return new Uint8Array(64).fill(0xff); },
  });
  assert.deepEqual(empty, { status: "empty", packId: 0, name: "No installed pet" });

  assert.deepEqual(
    await inspectInstalledPack({ async readFlash() { return new Uint8Array(64); } }),
    {
      status: "invalid",
      packId: null,
      name: "Unreadable companion pack",
      reason: "installed companion slot does not contain a valid K868PK1 header",
    },
  );
});

test("destructive replacement intent is one sector and binds old ID to exact new pack", async () => {
  const bytes = new Uint8Array(await readFile(new URL("assets/packs/fox.k868", root)));
  const definition = PACK_CATALOG.fox;
  const pack = {
    definition,
    record: {
      role: "companion_pack_fox",
      offset: PACK_SLOT.offset,
      bytes: definition.bytes,
      sha256: definition.sha256,
    },
    bytes,
  };
  const sourcePackId = 0x492e6628;
  const intent = buildReplacementIntent(sourcePackId, pack);
  const view = new DataView(intent.buffer, intent.byteOffset, intent.byteLength);
  assert.equal(intent.byteLength, REPLACEMENT_TRANSACTION.prepared.bytes);
  assert.deepEqual([...intent.subarray(0, 8)], [0x4b, 0x38, 0x36, 0x38, 0x52, 0x50, 0x31, 0]);
  assert.equal(view.getUint16(0x08, true), 1);
  assert.equal(view.getUint16(0x0a, true), 40);
  assert.equal(view.getUint32(0x0c, true), sourcePackId);
  assert.equal(view.getUint32(0x10, true), definition.packId);
  assert.equal(view.getUint32(0x14, true), definition.revision);
  assert.equal(view.getUint32(0x18, true), definition.bytes);
  assert.equal(view.getUint32(0x1c, true), definition.payloadCrc32);
  assert.equal(view.getUint32(0x20, true), definition.headerCrc32);
  assert.equal(view.getUint32(0x24, true), 0x6098fe41);
  assert.equal(intent.subarray(40).every((value) => value === 0xff), true);
  assert.throws(
    () => buildReplacementIntent(definition.packId, pack),
    /same-species pack updates must not request/,
  );
});

test("replacement inspection recovers source from PREPARED across erased or torn COMMITTED", async () => {
  const foxBytes = new Uint8Array(await readFile(new URL("assets/packs/fox.k868", root)));
  const catBytes = new Uint8Array(await readFile(new URL("assets/packs/cat.k868", root)));
  const foxPack = {
    definition: PACK_CATALOG.fox,
    record: {
      role: "companion_pack_fox",
      offset: PACK_SLOT.offset,
      bytes: PACK_CATALOG.fox.bytes,
      sha256: PACK_CATALOG.fox.sha256,
    },
    bytes: foxBytes,
  };
  const catPack = {
    definition: PACK_CATALOG.cat,
    record: {
      role: "companion_pack_cat",
      offset: PACK_SLOT.offset,
      bytes: PACK_CATALOG.cat.bytes,
      sha256: PACK_CATALOG.cat.sha256,
    },
    bytes: catBytes,
  };
  const prepared = buildReplacementIntent(0x492e6628, foxPack);
  const erased = new Uint8Array(REPLACEMENT_TRANSACTION.committed.bytes).fill(0xff);
  const loaderFor = (preparedSector, committedSector) => ({
    async readFlash(offset, bytes) {
      if (offset === REPLACEMENT_TRANSACTION.prepared.offset) {
        assert.equal(bytes, REPLACEMENT_TRANSACTION.prepared.bytes);
        return preparedSector.slice();
      }
      assert.equal(offset, REPLACEMENT_TRANSACTION.committed.offset);
      assert.equal(bytes, REPLACEMENT_TRANSACTION.committed.bytes);
      return committedSector.slice();
    },
  });

  const preparedOnly = await inspectReplacementTransaction(loaderFor(prepared, erased));
  assert.equal(preparedOnly.status, "prepared");
  assert.equal(preparedOnly.committedState, "empty");
  assert.equal(preparedOnly.sourcePackId, 0x492e6628);
  assert.equal(preparedOnly.targetPackId, PACK_CATALOG.fox.packId);
  assert.equal(replacementTransactionTargets(preparedOnly, foxPack), true);
  assert.equal(replacementTransactionTargets(preparedOnly, catPack), false);

  const committed = await inspectReplacementTransaction(loaderFor(prepared, prepared));
  assert.equal(committed.status, "committed");
  assert.equal(committed.committedState, "valid");
  assert.deepEqual(committed.preparedBytes, committed.committedBytes);

  const tornCommitted = prepared.slice();
  tornCommitted[0x24] ^= 0x01;
  const recovered = await inspectReplacementTransaction(loaderFor(prepared, tornCommitted));
  assert.equal(recovered.status, "prepared");
  assert.equal(recovered.committedState, "invalid");
  assert.match(recovered.committedError, /CRC mismatch/);
  assert.equal(recovered.sourcePackId, 0x492e6628);
  assert.equal(replacementTransactionTargets(recovered, foxPack), true);

  const differentCommit = buildReplacementIntent(0x12345678, foxPack);
  const mismatched = await inspectReplacementTransaction(loaderFor(prepared, differentCommit));
  assert.equal(mismatched.status, "prepared");
  assert.equal(mismatched.committedState, "invalid");
  assert.match(mismatched.committedError, /records differ/);
});

test("replacement inspection fails closed without one structurally valid PREPARED source", async () => {
  const foxBytes = new Uint8Array(await readFile(new URL("assets/packs/fox.k868", root)));
  const foxPack = {
    definition: PACK_CATALOG.fox,
    record: {
      role: "companion_pack_fox",
      offset: PACK_SLOT.offset,
      bytes: PACK_CATALOG.fox.bytes,
      sha256: PACK_CATALOG.fox.sha256,
    },
    bytes: foxBytes,
  };
  const intent = buildReplacementIntent(0x492e6628, foxPack);
  const erased = new Uint8Array(REPLACEMENT_TRANSACTION.prepared.bytes).fill(0xff);
  const loaderFor = (preparedSector, committedSector) => ({
    async readFlash(offset) {
      return (offset === REPLACEMENT_TRANSACTION.prepared.offset
        ? preparedSector
        : committedSector).slice();
    },
  });

  assert.deepEqual(
    await inspectReplacementTransaction(loaderFor(erased, erased)),
    { status: "empty" },
  );
  await assert.rejects(
    inspectReplacementTransaction(loaderFor(erased, intent)),
    /COMMITTED without PREPARED/,
  );
  const malformedPrepared = intent.slice();
  malformedPrepared[0] ^= 0xff;
  await assert.rejects(
    inspectReplacementTransaction(loaderFor(malformedPrepared, erased)),
    /PREPARED replacement intent has invalid/,
  );
});

test("install-time transition rechecks safe cancellation and supports authorization-free repair", async () => {
  const foxBytes = new Uint8Array(await readFile(new URL("assets/packs/fox.k868", root)));
  const catBytes = new Uint8Array(await readFile(new URL("assets/packs/cat.k868", root)));
  const packFor = (definition, bytes) => ({
    definition,
    record: {
      role: `companion_pack_${definition.id}`,
      offset: PACK_SLOT.offset,
      bytes: definition.bytes,
      sha256: definition.sha256,
    },
    bytes,
  });
  const foxPack = packFor(PACK_CATALOG.fox, foxBytes);
  const catPack = packFor(PACK_CATALOG.cat, catBytes);
  const intent = buildReplacementIntent(0x492e6628, foxPack);
  const prepared = await inspectReplacementTransaction({
    async readFlash(offset) {
      return offset === REPLACEMENT_TRANSACTION.prepared.offset
        ? intent.slice()
        : new Uint8Array(REPLACEMENT_TRANSACTION.committed.bytes).fill(0xff);
    },
  });

  assert.deepEqual(
    companionPackTransition(
      { status: "valid", packId: prepared.sourcePackId },
      null,
      prepared,
    ),
    { destructive: false, repair: false, retry: false, sourcePackId: null },
  );
  assert.throws(
    () => companionPackTransition(
      { status: "valid", packId: prepared.targetPackId },
      null,
      prepared,
    ),
    /freshly inspected physical pack matches its saved source ID/,
  );
  assert.throws(
    () => companionPackTransition({ status: "invalid" }, null, prepared),
    /freshly inspected physical pack matches its saved source ID/,
  );

  assert.deepEqual(
    companionPackTransition({ status: "invalid" }, foxPack, { status: "empty" }),
    { destructive: false, repair: true, retry: false, sourcePackId: null },
  );
  assert.deepEqual(
    companionPackTransition({ status: "empty" }, foxPack, { status: "empty" }),
    { destructive: false, repair: false, retry: false, sourcePackId: 0 },
  );
  assert.deepEqual(
    companionPackTransition({ status: "invalid" }, foxPack, prepared),
    { destructive: true, repair: false, retry: true, sourcePackId: 0x492e6628 },
  );
  assert.throws(
    () => companionPackTransition({ status: "invalid" }, catPack, prepared),
    /choose that exact pack to retry/,
  );
  assert.throws(
    () => companionPackTransition(
      { status: "valid", packId: 0x492e6628 },
      foxPack,
      { status: "invalid" },
    ),
    /malformed replacement transaction blocks/,
  );
});

test("preserve selects no write and unrecognized or changed packs fail closed", async () => {
  assert.equal(packDefinition("preserve"), null);
  assert.throws(() => packDefinition("wolf"), /not supported/);

  const fox = packDefinition("fox");
  const changed = new Uint8Array(await readFile(new URL("assets/packs/fox.k868", root)));
  changed[changed.byteLength - 1] ^= 0xff;
  await assert.rejects(verifyPackBytes(fox, changed), /SHA-256/);
  await assert.rejects(verifyPackBytes(fox, changed.subarray(0, -1)), /size/);
  await assert.rejects(reverifyPack({ definition: fox, record: { ...fox, offset: 0x670001 }, bytes: changed }), /metadata/);
});

test("a local unlocked .k868 file is fully parsed, hashed, and bound to the existing pack slot", async () => {
  const bytes = new Uint8Array(await readFile(new URL("assets/packs/fox.k868", root)));
  const metadata = validateUnlockedPackBytes(bytes);
  assert.deepEqual(
    {
      format: metadata.format,
      version: metadata.version,
      headerBytes: metadata.headerBytes,
      totalBytes: metadata.totalBytes,
      displayName: metadata.displayName,
      width: metadata.width,
      height: metadata.height,
      frameCount: metadata.frameCount,
      clipCount: metadata.clipCount,
      stepCount: metadata.stepCount,
    },
    {
      format: "K868PK1",
      version: 1,
      headerBytes: 64,
      totalBytes: 24976,
      displayName: "FOX",
      width: 64,
      height: 64,
      frameCount: 48,
      clipCount: 12,
      stepCount: 48,
    },
  );

  const pack = await loadUnlockedPack(localFile("my-unlocked-fox.k868", bytes));
  assert.equal(pack.definition.id, UNLOCKED_PACK_ID);
  assert.equal(pack.definition.source, "unlocked_file");
  assert.equal(pack.definition.name, "FOX");
  assert.equal(pack.definition.sha256, createHash("sha256").update(bytes).digest("hex"));
  assert.equal(pack.record.offset, PACK_SLOT.offset);
  assert.equal(pack.record.bytes, 24976);
  await reverifyPack(pack);
});

test("native 64x80 v2 packs use the same bounded slot and validator", async () => {
  const legacy = new Uint8Array(await readFile(new URL("assets/packs/fox.k868", root)));
  const bytes = promoteToV2(legacy);
  const metadata = validateUnlockedPackBytes(bytes);
  assert.equal(metadata.version, 2);
  assert.equal(metadata.width, 64);
  assert.equal(metadata.height, 80);
  assert.equal(metadata.totalBytes, 31_120);
  assert.ok(metadata.totalBytes < PACK_SLOT.bytes);

  const pack = await loadUnlockedPack(localFile("native-fox.k868", bytes));
  assert.equal(pack.record.offset, PACK_SLOT.offset);
  assert.equal(pack.record.bytes, 31_120);
  await reverifyPack(pack);

  const loader = {
    async readFlash(offset, length) {
      assert.equal(offset, PACK_SLOT.offset);
      return bytes.slice(0, length);
    },
  };
  const installed = await inspectInstalledPack(loader);
  assert.equal(installed.status, "valid");
  assert.equal(installed.bytes, 31_120);
});

test("unlocked companion files fail closed on naming, size, header, layout, or CRC changes", async () => {
  const valid = new Uint8Array(await readFile(new URL("assets/packs/fox.k868", root)));

  await assert.rejects(loadUnlockedPack(localFile("fox.bin", valid)), /must end in \.k868/);
  await assert.rejects(loadUnlockedPack(localFile("fox.k868", valid, valid.byteLength - 1)), /changed while/);

  const badMagic = valid.slice();
  badMagic[0] ^= 0xff;
  assert.throws(() => validateUnlockedPackBytes(badMagic), /magic/);

  const badVersion = valid.slice();
  new DataView(badVersion.buffer).setUint16(0x08, 2, true);
  assert.throws(() => validateUnlockedPackBytes(badVersion), /version\/canvas 2\/64x64/);

  const badLength = valid.slice();
  new DataView(badLength.buffer).setUint32(0x0c, valid.byteLength - 1, true);
  assert.throws(() => validateUnlockedPackBytes(badLength), /header declares/);

  const badHeaderCrc = valid.slice();
  badHeaderCrc[0x30] = 0x51;
  assert.throws(() => validateUnlockedPackBytes(badHeaderCrc), /header CRC mismatch/);

  const badPayloadCrc = valid.slice();
  badPayloadCrc[badPayloadCrc.byteLength - 1] ^= 0x01;
  assert.throws(() => validateUnlockedPackBytes(badPayloadCrc), /payload CRC mismatch/);

  const loaded = await loadUnlockedPack(localFile("fox.k868", valid));
  loaded.bytes[loaded.bytes.byteLength - 1] ^= 0x01;
  await assert.rejects(reverifyPack(loaded), /payload CRC mismatch|SHA-256 changed/);
});
