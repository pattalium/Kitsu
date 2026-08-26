import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFile } from "node:fs/promises";
import test from "node:test";

import {
  PACK_CATALOG,
  PACK_SLOT,
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

test("official companion catalog pins only Cat, Fox, and Dog to the one pack slot", async () => {
  assert.deepEqual(Object.keys(PACK_CATALOG).sort(), ["cat", "dog", "fox"]);
  assert.deepEqual(PACK_SLOT, { offset: 0x670000, bytes: 0x140000 });
  assert.equal(PACK_SLOT.offset + PACK_SLOT.bytes, 0x7b0000);

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

test("unlocked companion files fail closed on naming, size, header, layout, or CRC changes", async () => {
  const valid = new Uint8Array(await readFile(new URL("assets/packs/fox.k868", root)));

  await assert.rejects(loadUnlockedPack(localFile("fox.bin", valid)), /must end in \.k868/);
  await assert.rejects(loadUnlockedPack(localFile("fox.k868", valid, valid.byteLength - 1)), /changed while/);

  const badMagic = valid.slice();
  badMagic[0] ^= 0xff;
  assert.throws(() => validateUnlockedPackBytes(badMagic), /magic/);

  const badVersion = valid.slice();
  new DataView(badVersion.buffer).setUint16(0x08, 2, true);
  assert.throws(() => validateUnlockedPackBytes(badVersion), /unsupported companion pack version 2/);

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
