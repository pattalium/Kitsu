import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFile } from "node:fs/promises";
import test from "node:test";

import {
  PACK_CATALOG,
  PACK_SLOT,
  packDefinition,
  reverifyPack,
  verifyPackBytes,
} from "../src/packs.js";

const root = new URL("../../../", import.meta.url);

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
