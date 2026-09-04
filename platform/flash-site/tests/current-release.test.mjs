import assert from "node:assert/strict";
import test from "node:test";
import { CURRENT_FLASH_PLAN, parseCurrentOtaSelection } from "../src/current-release.js";

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
