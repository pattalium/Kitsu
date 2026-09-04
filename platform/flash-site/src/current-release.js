import {
  parseFirmwarePackage,
  publishedFirmwareRelease,
  verifyFirmwarePackage,
} from "../../public-site/firmware-release.js";
import { sha256Hex } from "./release.js";

export const CURRENT_FLASH_PLAN = Object.freeze({
  flashSize: "8MB",
  otaDataOffset: 0x049000,
  otaDataBytes: 0x002000,
  app0Offset: 0x050000,
  app1Offset: 0x350000,
  applicationSlotBytes: 0x300000,
  otaJournalBytes: 0x001000,
  companionPackOffset: 0x670000,
  companionPackBytes: 0x140000,
});

const OTA_ENTRY_BYTES = 32;
const OTA_ENTRY_SECTOR_BYTES = 0x1000;
const OTA_STATE_NEW = 0;
const OTA_STATE_PENDING_VERIFY = 1;
const OTA_STATE_VALID = 2;
const OTA_STATE_INVALID = 3;
const OTA_STATE_ABORTED = 4;
const OTA_STATE_UNDEFINED = 0xffff_ffff;

function fail(message) {
  throw new Error(message);
}

function uint32LE(bytes, offset) {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(offset, true);
}

// ESP-IDF calculates the OTA-select CRC over ota_seq with seed UINT32_MAX.
function otaSequenceCrc32(sequence) {
  let value = 0;
  for (let index = 0; index < 4; index += 1) {
    value ^= (sequence >>> (index * 8)) & 0xff;
    for (let bit = 0; bit < 8; bit += 1) {
      value = (value >>> 1) ^ ((value & 1) ? 0xedb8_8320 : 0);
    }
  }
  return (value ^ 0xffff_ffff) >>> 0;
}

function parseOtaEntry(bytes, sectorOffset, index) {
  const record = bytes.subarray(sectorOffset, sectorOffset + OTA_ENTRY_BYTES);
  const sequence = uint32LE(record, 0);
  const state = uint32LE(record, 24);
  const crc = uint32LE(record, 28);
  const valid = sequence >= 1
    && sequence !== 0xffff_ffff
    && state !== OTA_STATE_INVALID
    && state !== OTA_STATE_ABORTED
    && crc === otaSequenceCrc32(sequence);
  return Object.freeze({ index, sequence, state, crc, valid });
}

function stateName(state) {
  return new Map([
    [OTA_STATE_NEW, "new"],
    [OTA_STATE_PENDING_VERIFY, "pending verification"],
    [OTA_STATE_VALID, "valid"],
    [OTA_STATE_INVALID, "invalid"],
    [OTA_STATE_ABORTED, "aborted"],
    [OTA_STATE_UNDEFINED, "undefined"],
  ]).get(state) ?? `unknown 0x${state.toString(16)}`;
}

export function parseCurrentOtaSelection(bytes) {
  if (!(bytes instanceof Uint8Array) || bytes.byteLength !== CURRENT_FLASH_PLAN.otaDataBytes) {
    fail("OTA selection read is incomplete");
  }
  const entries = [
    parseOtaEntry(bytes, 0, 0),
    parseOtaEntry(bytes, OTA_ENTRY_SECTOR_BYTES, 1),
  ];
  const valid = entries.filter((entry) => entry.valid);
  if (valid.length === 0) {
    return Object.freeze({
      slot: 0,
      label: "app0",
      offset: CURRENT_FLASH_PLAN.app0Offset,
      source: "bootloader default",
      sequence: null,
      state: OTA_STATE_UNDEFINED,
      stateName: "initial",
    });
  }
  const active = valid.length === 1
    ? valid[0]
    : valid[0].sequence >= valid[1].sequence ? valid[0] : valid[1];
  if (active.state === OTA_STATE_NEW || active.state === OTA_STATE_PENDING_VERIFY) {
    fail(`the selected firmware slot is ${stateName(active.state)}; boot it once before USB reinstall`);
  }
  if (active.state !== OTA_STATE_VALID && active.state !== OTA_STATE_UNDEFINED) {
    fail(`the selected firmware slot has unsupported OTA state ${stateName(active.state)}`);
  }
  const slot = (active.sequence - 1) % 2;
  return Object.freeze({
    slot,
    label: slot === 0 ? "app0" : "app1",
    offset: slot === 0 ? CURRENT_FLASH_PLAN.app0Offset : CURRENT_FLASH_PLAN.app1Offset,
    source: `OTA record ${active.index}`,
    sequence: active.sequence,
    state: active.state,
    stateName: stateName(active.state),
  });
}

export async function inspectCurrentOtaSelection(loader) {
  if (!loader || typeof loader.readFlash !== "function") {
    fail("OTA selection inspection requires an active ROM loader");
  }
  const bytes = await loader.readFlash(
    CURRENT_FLASH_PLAN.otaDataOffset,
    CURRENT_FLASH_PLAN.otaDataBytes,
  );
  const selection = parseCurrentOtaSelection(bytes);
  return Object.freeze({ ...selection, sha256: await sha256Hex(bytes) });
}

async function readExactResponse(response, expectedBytes) {
  const declared = response.headers.get("content-length");
  if (declared !== null && (!/^[0-9]+$/.test(declared) || Number(declared) !== expectedBytes)) {
    fail("latest firmware response length is wrong");
  }
  const bytes = new Uint8Array(await response.arrayBuffer());
  if (bytes.byteLength !== expectedBytes) fail("latest firmware download is incomplete");
  return bytes;
}

export async function fetchVerifiedCurrentRelease() {
  const response = await fetch(publishedFirmwareRelease.url, {
    cache: "no-store",
    credentials: "omit",
    redirect: "error",
    referrerPolicy: "no-referrer",
    signal: AbortSignal.timeout(20000),
  });
  if (!response.ok) fail(`latest firmware returned HTTP ${response.status}`);
  const bytes = await readExactResponse(response, publishedFirmwareRelease.bytes);
  const verified = await verifyFirmwarePackage({ bytes, contract: publishedFirmwareRelease });
  const image = parseFirmwarePackage(bytes).image.slice();
  if (image.byteLength > CURRENT_FLASH_PLAN.applicationSlotBytes - CURRENT_FLASH_PLAN.otaJournalBytes) {
    fail("latest firmware overlaps the private OTA journal");
  }
  return Object.freeze({
    ...verified,
    image,
    packageBytes: bytes.byteLength,
    packageSha256: await sha256Hex(bytes),
  });
}
