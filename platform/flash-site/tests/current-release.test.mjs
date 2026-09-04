import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import test from "node:test";
import { CURRENT_FLASH_PLAN, parseCurrentOtaSelection } from "../src/current-release.js";
import {
  buildFactoryApplicationSlot,
  buildFactoryOtaData,
  FACTORY_INIT_PLAN,
} from "../src/factory-init.js";

function crc32ForSequence(sequence) {
  let value = 0;
  for (let index = 0; index < 4; index += 1) {
    value ^= (sequence >>> (index * 8)) & 0xff;
    for (let bit = 0; bit < 8; bit += 1) {
      value = (value >>> 1) ^ ((value & 1) ? 0xedb8_8320 : 0);
    }
  }
  return (value ^ 0xffff_ffff) >>> 0;
}

function otaData(entries = []) {
  const bytes = new Uint8Array(CURRENT_FLASH_PLAN.otaDataBytes).fill(0xff);
  for (const { index, sequence, state } of entries) {
    const view = new DataView(bytes.buffer, index * 0x1000, 32);
    view.setUint32(0, sequence, true);
    view.setUint32(24, state, true);
    view.setUint32(28, crc32ForSequence(sequence), true);
  }
  return bytes;
}

test("an erased OTA selection defaults safely to app0", () => {
  assert.deepEqual(parseCurrentOtaSelection(otaData()), {
    slot: 0,
    label: "app0",
    offset: CURRENT_FLASH_PLAN.app0Offset,
    source: "bootloader default",
    sequence: null,
    state: 0xffff_ffff,
    stateName: "initial",
  });
});

test("the highest valid OTA sequence selects the bootloader's app slot", () => {
  const selection = parseCurrentOtaSelection(otaData([
    { index: 0, sequence: 1, state: 2 },
    { index: 1, sequence: 2, state: 2 },
  ]));
  assert.equal(selection.label, "app1");
  assert.equal(selection.offset, CURRENT_FLASH_PLAN.app1Offset);
  assert.equal(selection.sequence, 2);
});

test("a slot still pending boot verification cannot be overwritten", () => {
  assert.throws(
    () => parseCurrentOtaSelection(otaData([{ index: 0, sequence: 1, state: 1 }])),
    /pending verification.*boot it once/i,
  );
});

test("new-board initialization builds exact OTA data and a cleared current application slot", () => {
  const otaData = buildFactoryOtaData();
  assert.equal(otaData.byteLength, CURRENT_FLASH_PLAN.otaDataBytes);
  assert.equal(
    createHash("sha256").update(otaData).digest("hex"),
    FACTORY_INIT_PLAN.bootApp0Sha256,
  );

  const application = Uint8Array.from([0xe9, 0x01, 0x02, 0x03]);
  const slot = buildFactoryApplicationSlot(application);
  assert.equal(slot.byteLength, CURRENT_FLASH_PLAN.applicationSlotBytes);
  assert.deepEqual(slot.subarray(0, application.byteLength), application);
  assert.equal(slot.subarray(application.byteLength).every((byte) => byte === 0xff), true);
  assert.throws(
    () => buildFactoryApplicationSlot(new Uint8Array(
      CURRENT_FLASH_PLAN.applicationSlotBytes - CURRENT_FLASH_PLAN.otaJournalBytes + 1,
    )),
    /overlaps the private OTA journal/,
  );
});
