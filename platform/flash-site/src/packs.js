import { sha256Hex } from "./release.js";

export const PACK_SLOT = Object.freeze({
  offset: 0x670000,
  bytes: 0x140000,
});

export const REPLACEMENT_TRANSACTION = Object.freeze({
  // Separate sectors preserve PREPARED if programming COMMITTED is
  // interrupted. Fresh installs clear kitsu_conn before writing PREPARED;
  // exact-target retries retain both sectors and clear only the signed suffix
  // beginning at 0x7b2000.
  prepared: Object.freeze({ offset: 0x7b0000, bytes: 0x1000 }),
  committed: Object.freeze({ offset: 0x7b1000, bytes: 0x1000 }),
});

export const UNLOCKED_PACK_ID = "unlocked";

const K868_FORMAT = Object.freeze({
  headerBytes: 64,
  clipBytes: 12,
  stepBytes: 4,
  versions: Object.freeze({
    1: Object.freeze({ width: 64, height: 64, frameBytes: 512 }),
    2: Object.freeze({ width: 64, height: 80, frameBytes: 640 }),
  }),
  maxClips: 512,
  maxSteps: 65535,
  maxStepsPerClip: 256,
  maxRole: 11,
  maxPlaybackMode: 3,
  minStepMs: 100,
  maxStepMs: 60000,
});

const K868_MAGIC = Object.freeze([0x4b, 0x38, 0x36, 0x38, 0x50, 0x4b, 0x31, 0x00]);
const REPLACEMENT_INTENT_MAGIC = Object.freeze([0x4b, 0x38, 0x36, 0x38, 0x52, 0x50, 0x31, 0x00]);
const REPLACEMENT_INTENT_SCHEMA = 1;
const REPLACEMENT_INTENT_RECORD_BYTES = 40;

const CRC32_TABLE = Uint32Array.from({ length: 256 }, (_, value) => {
  let crc = value;
  for (let bit = 0; bit < 8; bit += 1) {
    crc = (crc & 1) === 1 ? (crc >>> 1) ^ 0xedb88320 : crc >>> 1;
  }
  return crc >>> 0;
});

export const PACK_CATALOG = Object.freeze({
  fox: Object.freeze({
    id: "fox",
    name: "Fox",
    filename: "Kitsu868-v0.17.2-fox.k868",
    assetUrl: new URL("../../../assets/packs/fox.k868", import.meta.url).href,
    bytes: 24976,
    sha256: "c868386770b6083dcd8f7c01ec7fe455faec476a96c724ab62f09770fdcdab38",
    displayName: "FOX",
    packId: 0x6c393e21,
    revision: 2,
    payloadCrc32: 0x2301202e,
    headerCrc32: 0xac7b0040,
  }),
  cat: Object.freeze({
    id: "cat",
    name: "Cat",
    filename: "Kitsu868-v0.17.2-cat.k868",
    assetUrl: new URL("../../../assets/packs/cat.k868", import.meta.url).href,
    bytes: 24976,
    sha256: "8d19d6b8bc584d9aaee5a6867504fd23c1862c907bbeb1affd9611e35bf2a6d7",
    displayName: "CAT",
    packId: 0xfdc79d6f,
    revision: 2,
    payloadCrc32: 0x6d3e1b6f,
    headerCrc32: 0xa72b02a6,
  }),
  dog: Object.freeze({
    id: "dog",
    name: "Dog",
    filename: "Kitsu868-v0.17.2-dog.k868",
    assetUrl: new URL("../../../assets/packs/dog.k868", import.meta.url).href,
    bytes: 24976,
    sha256: "8652aad28816d52fca334766ebefb5c38aec1b09dcc72783414998d17a46e261",
    displayName: "DOG",
    packId: 0xe2b5e7ba,
    revision: 2,
    payloadCrc32: 0x6d783e49,
    headerCrc32: 0x93f7dfb4,
  }),
});

function fail(message) {
  throw new Error(message);
}

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc = CRC32_TABLE[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function hexadecimal32(value) {
  return value.toString(16).padStart(8, "0").toUpperCase();
}

function decodeDisplayName(bytes) {
  const terminator = bytes.indexOf(0);
  const nameBytes = terminator < 0 ? bytes : bytes.subarray(0, terminator);
  if (terminator >= 0 && bytes.subarray(terminator).some((value) => value !== 0)) {
    fail("display name has data after its NUL terminator");
  }
  if (nameBytes.byteLength === 0) fail("display name is empty");
  if (nameBytes.some((value) => value < 0x20 || value > 0x7e)) {
    fail("display name must contain printable ASCII only");
  }
  return String.fromCharCode(...nameBytes);
}

export function validateUnlockedPackBytes(bytes) {
  if (!(bytes instanceof Uint8Array)) fail("unlocked companion pack is not a byte array");
  if (bytes.byteLength < K868_FORMAT.headerBytes) {
    fail(`unlocked companion pack is shorter than its ${K868_FORMAT.headerBytes}-byte header`);
  }
  if (bytes.byteLength > PACK_SLOT.bytes) {
    fail(`unlocked companion pack exceeds the dedicated companion slot`);
  }
  if (K868_MAGIC.some((value, index) => bytes[index] !== value)) {
    fail("unlocked companion pack has invalid K868PK1 magic");
  }

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const version = view.getUint16(0x08, true);
  const headerBytes = view.getUint16(0x0a, true);
  const totalBytes = view.getUint32(0x0c, true);
  const payloadCrc32 = view.getUint32(0x10, true);
  const headerCrc32 = view.getUint32(0x14, true);
  const packId = view.getUint32(0x18, true);
  const revision = view.getUint32(0x1c, true);
  const width = view.getUint16(0x20, true);
  const height = view.getUint16(0x22, true);
  const frameCount = view.getUint16(0x24, true);
  const clipCount = view.getUint16(0x26, true);
  const stepCount = view.getUint32(0x28, true);
  const flags = view.getUint32(0x2c, true);

  const frameFormat = K868_FORMAT.versions[version];
  if (!frameFormat) fail(`unsupported companion pack version ${version}`);
  if (headerBytes !== K868_FORMAT.headerBytes) fail(`invalid companion pack header size ${headerBytes}`);
  if (totalBytes !== bytes.byteLength) {
    fail(`companion pack length is ${bytes.byteLength} bytes, header declares ${totalBytes}`);
  }
  if (totalBytes > PACK_SLOT.bytes) {
    fail("companion pack write exceeds the dedicated companion slot");
  }
  if (packId === 0) fail("companion pack ID must be nonzero");
  if (revision === 0) fail("companion pack revision must be nonzero");
  if (width !== frameFormat.width || height !== frameFormat.height) {
    fail(`unsupported companion pack version/canvas ${version}/${width}x${height}`);
  }
  if (frameCount === 0) fail("companion pack contains no frames");
  if (clipCount === 0 || clipCount > K868_FORMAT.maxClips) {
    fail(`invalid companion clip count ${clipCount}`);
  }
  if (stepCount === 0 || stepCount > K868_FORMAT.maxSteps) {
    fail(`invalid companion animation-step count ${stepCount}`);
  }
  if (flags !== 0) fail(`unsupported companion pack flags 0x${hexadecimal32(flags)}`);

  const displayName = decodeDisplayName(bytes.subarray(0x30, K868_FORMAT.headerBytes));
  const expectedTotal = K868_FORMAT.headerBytes
    + clipCount * K868_FORMAT.clipBytes
    + stepCount * K868_FORMAT.stepBytes
    + frameCount * frameFormat.frameBytes;
  if (totalBytes !== expectedTotal) {
    fail(`invalid companion pack layout: expected ${expectedTotal} bytes, found ${totalBytes}`);
  }

  const actualPayloadCrc32 = crc32(bytes.subarray(K868_FORMAT.headerBytes));
  if (actualPayloadCrc32 !== payloadCrc32) {
    fail(
      `companion payload CRC mismatch: expected ${hexadecimal32(payloadCrc32)}, `
      + `calculated ${hexadecimal32(actualPayloadCrc32)}`,
    );
  }
  const headerForCrc = bytes.slice(0x08, K868_FORMAT.headerBytes);
  headerForCrc.fill(0, 0x14 - 0x08, 0x18 - 0x08);
  const actualHeaderCrc32 = crc32(headerForCrc);
  if (actualHeaderCrc32 !== headerCrc32) {
    fail(
      `companion header CRC mismatch: expected ${hexadecimal32(headerCrc32)}, `
      + `calculated ${hexadecimal32(actualHeaderCrc32)}`,
    );
  }

  const clipsOffset = K868_FORMAT.headerBytes;
  const stepsOffset = clipsOffset + clipCount * K868_FORMAT.clipBytes;
  let hasBaseIdle = false;
  for (let index = 0; index < clipCount; index += 1) {
    const offset = clipsOffset + index * K868_FORMAT.clipBytes;
    const role = view.getUint8(offset);
    const variant = view.getUint8(offset + 1);
    const mode = view.getUint8(offset + 2);
    const weight = view.getUint8(offset + 3);
    const firstStep = view.getUint32(offset + 4, true);
    const count = view.getUint16(offset + 8, true);
    const reserved = view.getUint16(offset + 10, true);
    if (role > K868_FORMAT.maxRole) fail(`companion clip ${index} has invalid role ${role}`);
    if (mode > K868_FORMAT.maxPlaybackMode) {
      fail(`companion clip ${index} has invalid playback mode ${mode}`);
    }
    if (weight === 0) fail(`companion clip ${index} has zero selection weight`);
    if (
      count === 0
      || count > K868_FORMAT.maxStepsPerClip
      || firstStep + count > stepCount
    ) {
      fail(`companion clip ${index} references invalid animation steps`);
    }
    if (mode === 0 && count !== 1) {
      fail(`HOLD companion clip ${index} must contain exactly one animation step`);
    }
    if (reserved !== 0) fail(`companion clip ${index} has nonzero reserved data`);
    hasBaseIdle ||= role === 0 && variant === 0;
  }
  if (!hasBaseIdle) fail("companion pack has no base IDLE clip");

  for (let index = 0; index < stepCount; index += 1) {
    const offset = stepsOffset + index * K868_FORMAT.stepBytes;
    const frameIndex = view.getUint16(offset, true);
    const durationMs = view.getUint16(offset + 2, true);
    if (frameIndex >= frameCount) {
      fail(`companion animation step ${index} references missing frame ${frameIndex}`);
    }
    if (durationMs < K868_FORMAT.minStepMs || durationMs > K868_FORMAT.maxStepMs) {
      fail(`companion animation step ${index} has invalid duration ${durationMs} ms`);
    }
  }

  return Object.freeze({
    format: "K868PK1",
    version,
    headerBytes,
    totalBytes,
    payloadCrc32,
    headerCrc32,
    packId,
    revision,
    width,
    height,
    frameCount,
    clipCount,
    stepCount,
    displayName,
  });
}

export async function loadUnlockedPack(file) {
  if (!file || typeof file.arrayBuffer !== "function") fail("choose an unlocked .k868 file");
  if (typeof file.name !== "string" || !/\.k868$/iu.test(file.name)) {
    fail("unlocked companion pack filename must end in .k868");
  }
  if (!Number.isSafeInteger(file.size) || file.size < K868_FORMAT.headerBytes) {
    fail("unlocked companion pack file size is invalid");
  }
  if (file.size > PACK_SLOT.bytes) {
    fail("unlocked companion pack exceeds the dedicated companion slot");
  }
  const fileBuffer = await file.arrayBuffer();
  if (!(fileBuffer instanceof ArrayBuffer)) fail("unlocked companion pack could not be read safely");
  const bytes = new Uint8Array(fileBuffer).slice();
  if (bytes.byteLength !== file.size) fail("unlocked companion pack changed while it was being read");
  const metadata = validateUnlockedPackBytes(bytes);
  const sha256 = await sha256Hex(bytes);
  const definition = Object.freeze({
    id: UNLOCKED_PACK_ID,
    source: "unlocked_file",
    name: metadata.displayName,
    filename: file.name,
    bytes: metadata.totalBytes,
    sha256,
    packId: metadata.packId,
    revision: metadata.revision,
    payloadCrc32: metadata.payloadCrc32,
    headerCrc32: metadata.headerCrc32,
  });
  return Object.freeze({
    definition,
    record: Object.freeze({
      role: `companion_pack_${hexadecimal32(metadata.packId).toLowerCase()}`,
      offset: PACK_SLOT.offset,
      bytes: metadata.totalBytes,
      sha256,
    }),
    bytes,
  });
}

export function packDefinition(packId) {
  if (packId === "preserve") return null;
  const definition = PACK_CATALOG[packId];
  if (!definition) fail("selected companion pack is not supported");
  if (definition.bytes < 1 || definition.bytes > PACK_SLOT.bytes) {
    fail(`${definition.name} companion pack exceeds the dedicated pack slot`);
  }
  return definition;
}

export async function verifyPackBytes(definition, bytes) {
  if (!(bytes instanceof Uint8Array) || bytes.byteLength !== definition.bytes) {
    fail(`${definition.name} companion pack size does not match the installed catalog`);
  }
  if (await sha256Hex(bytes) !== definition.sha256) {
    fail(`${definition.name} companion pack SHA-256 does not match the installed catalog`);
  }
  const metadata = validateUnlockedPackBytes(bytes);
  if (
    metadata.displayName !== definition.displayName
    || metadata.packId !== definition.packId
    || metadata.revision !== definition.revision
    || metadata.payloadCrc32 !== definition.payloadCrc32
    || metadata.headerCrc32 !== definition.headerCrc32
  ) {
    fail(`${definition.name} companion pack structure does not match the installed catalog`);
  }
}

export async function inspectInstalledPack(loader) {
  if (!loader || typeof loader.readFlash !== "function") {
    fail("companion-pack inspection requires an active ROM loader");
  }
  const header = await loader.readFlash(PACK_SLOT.offset, K868_FORMAT.headerBytes);
  if (!(header instanceof Uint8Array) || header.byteLength !== K868_FORMAT.headerBytes) {
    fail("installed companion header readback has an unexpected length");
  }
  if (header.every((value) => value === 0xff)) {
    return Object.freeze({ status: "empty", packId: 0, name: "No installed pet" });
  }
  if (K868_MAGIC.some((value, index) => header[index] !== value)) {
    fail("installed companion slot does not contain a valid K868PK1 header");
  }
  const totalBytes = new DataView(
    header.buffer,
    header.byteOffset,
    header.byteLength,
  ).getUint32(0x0c, true);
  if (totalBytes < K868_FORMAT.headerBytes || totalBytes > PACK_SLOT.bytes) {
    fail("installed companion declares an unsafe length");
  }
  const bytes = await loader.readFlash(PACK_SLOT.offset, totalBytes);
  if (!(bytes instanceof Uint8Array) || bytes.byteLength !== totalBytes) {
    fail("installed companion readback has an unexpected length");
  }
  const metadata = validateUnlockedPackBytes(bytes);
  return Object.freeze({
    status: "valid",
    name: metadata.displayName,
    packId: metadata.packId,
    revision: metadata.revision,
    bytes: metadata.totalBytes,
    payloadCrc32: metadata.payloadCrc32,
    headerCrc32: metadata.headerCrc32,
    sha256: await sha256Hex(bytes),
  });
}

function replacementIntentMetadata(bytes, phase) {
  if (!(bytes instanceof Uint8Array)
    || bytes.byteLength !== REPLACEMENT_TRANSACTION.prepared.bytes) {
    fail(`${phase} replacement sector readback has an unexpected length`);
  }
  if (bytes.every((value) => value === 0xff)) return null;
  if (bytes.subarray(REPLACEMENT_INTENT_RECORD_BYTES).some((value) => value !== 0xff)) {
    fail(`${phase} replacement sector contains data outside its intent record`);
  }
  if (REPLACEMENT_INTENT_MAGIC.some((value, index) => bytes[index] !== value)) {
    fail(`${phase} replacement intent has invalid K868RP1 magic`);
  }

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const schema = view.getUint16(0x08, true);
  const recordBytes = view.getUint16(0x0a, true);
  const sourcePackId = view.getUint32(0x0c, true);
  const targetPackId = view.getUint32(0x10, true);
  const targetRevision = view.getUint32(0x14, true);
  const targetBytes = view.getUint32(0x18, true);
  const targetPayloadCrc32 = view.getUint32(0x1c, true);
  const targetHeaderCrc32 = view.getUint32(0x20, true);
  const recordCrc32 = view.getUint32(0x24, true);
  if (schema !== REPLACEMENT_INTENT_SCHEMA) {
    fail(`${phase} replacement intent has unsupported schema ${schema}`);
  }
  if (recordBytes !== REPLACEMENT_INTENT_RECORD_BYTES) {
    fail(`${phase} replacement intent has invalid record size ${recordBytes}`);
  }
  if (sourcePackId === 0 || targetPackId === 0 || sourcePackId === targetPackId) {
    fail(`${phase} replacement intent has invalid source or target identity`);
  }
  if (targetRevision === 0 || targetBytes === 0 || targetBytes > PACK_SLOT.bytes) {
    fail(`${phase} replacement intent has invalid target bounds`);
  }

  const crcInput = bytes.slice(8, REPLACEMENT_INTENT_RECORD_BYTES);
  new DataView(crcInput.buffer).setUint32(0x24 - 8, 0, true);
  const calculatedRecordCrc32 = crc32(crcInput);
  if (recordCrc32 !== calculatedRecordCrc32) {
    fail(
      `${phase} replacement intent CRC mismatch: expected ${hexadecimal32(recordCrc32)}, `
      + `calculated ${hexadecimal32(calculatedRecordCrc32)}`,
    );
  }

  return Object.freeze({
    sourcePackId,
    targetPackId,
    targetRevision,
    targetBytes,
    targetPayloadCrc32,
    targetHeaderCrc32,
    recordCrc32,
  });
}

function intentRecordsMatch(left, right) {
  for (let index = 0; index < REPLACEMENT_INTENT_RECORD_BYTES; index += 1) {
    if (left[index] !== right[index]) return false;
  }
  return true;
}

export async function inspectReplacementTransaction(loader) {
  if (!loader || typeof loader.readFlash !== "function") {
    fail("replacement-transaction inspection requires an active ROM loader");
  }
  // Keep these reads ordered: Web Serial transports are not safe for
  // concurrent ROM-loader commands.
  const preparedBytes = await loader.readFlash(
    REPLACEMENT_TRANSACTION.prepared.offset,
    REPLACEMENT_TRANSACTION.prepared.bytes,
  );
  const committedBytes = await loader.readFlash(
    REPLACEMENT_TRANSACTION.committed.offset,
    REPLACEMENT_TRANSACTION.committed.bytes,
  );
  const prepared = replacementIntentMetadata(preparedBytes, "PREPARED");
  if (!prepared) {
    const committed = replacementIntentMetadata(committedBytes, "COMMITTED");
    if (!committed) return Object.freeze({ status: "empty" });
    fail("replacement transaction has COMMITTED without PREPARED");
  }

  // PREPARED is the durable source-identity anchor. A torn, malformed, or
  // mismatched COMMITTED sector never authorizes firmware, but it also must
  // not destroy the ability to retry the exact PREPARED target.
  let committed;
  let committedError;
  try {
    committed = replacementIntentMetadata(committedBytes, "COMMITTED");
  } catch (error) {
    committedError = error instanceof Error ? error.message : String(error);
  }
  const committedMatches = committed
    && intentRecordsMatch(preparedBytes, committedBytes);
  if (committed && !committedMatches) {
    committedError = "replacement transaction PREPARED and COMMITTED records differ";
  }

  return Object.freeze({
    status: committedMatches ? "committed" : "prepared",
    committedState: committedMatches ? "valid" : committedError ? "invalid" : "empty",
    committedError: committedError ?? null,
    ...prepared,
    preparedBytes: preparedBytes.slice(),
    committedBytes: committedMatches ? committedBytes.slice() : null,
  });
}

export function replacementTransactionTargets(transaction, pack) {
  if (!transaction || !["prepared", "committed"].includes(transaction.status)) return false;
  if (!pack?.bytes) return false;
  const target = validateUnlockedPackBytes(pack.bytes);
  return transaction.targetPackId === target.packId
    && transaction.targetRevision === target.revision
    && transaction.targetBytes === target.totalBytes
    && transaction.targetPayloadCrc32 === target.payloadCrc32
    && transaction.targetHeaderCrc32 === target.headerCrc32;
}

export function companionPackTransition(current, targetPack, transaction) {
  const pending = transaction?.status === "prepared"
    || transaction?.status === "committed";
  if (!targetPack) {
    if (pending
      && !(current?.status === "valid" && current.packId === transaction.sourcePackId)) {
      fail(
        "Keep current pet cannot erase PREPARED unless the freshly inspected physical pack matches its saved source ID",
      );
    }
    return Object.freeze({
      destructive: false,
      repair: false,
      retry: false,
      sourcePackId: null,
    });
  }
  if (pending) {
    if (!replacementTransactionTargets(transaction, targetPack)) {
      fail(
        `pending replacement is bound to target ID ${hexadecimal32(transaction.targetPackId)}; choose that exact pack to retry`,
      );
    }
    return Object.freeze({
      destructive: true,
      repair: false,
      retry: true,
      sourcePackId: transaction.sourcePackId,
    });
  }
  if (transaction?.status === "invalid") {
    fail("a malformed replacement transaction blocks all companion writes");
  }
  if (current?.status === "empty") {
    return Object.freeze({
      destructive: false,
      repair: false,
      retry: false,
      sourcePackId: 0,
    });
  }
  if (current?.status === "invalid" && transaction?.status === "empty") {
    return Object.freeze({
      destructive: false,
      repair: true,
      retry: false,
      sourcePackId: null,
    });
  }
  if (current?.status !== "valid") {
    fail("a new pet cannot be installed until the current companion slot has been validated");
  }
  return Object.freeze({
    destructive: current.packId !== targetPack.definition.packId,
    repair: false,
    retry: false,
    sourcePackId: current.packId,
  });
}

export function buildReplacementIntent(sourcePackId, pack) {
  if (!Number.isInteger(sourcePackId) || sourcePackId <= 0 || sourcePackId > 0xffffffff) {
    fail("replacement intent requires the validated current companion ID");
  }
  if (!pack?.bytes) fail("replacement intent requires a validated target pack");
  const target = validateUnlockedPackBytes(pack.bytes);
  if (sourcePackId === target.packId) {
    fail("same-species pack updates must not request a destructive replacement");
  }
  const bytes = new Uint8Array(REPLACEMENT_TRANSACTION.prepared.bytes).fill(0xff);
  bytes.set(REPLACEMENT_INTENT_MAGIC, 0);
  const view = new DataView(bytes.buffer);
  view.setUint16(0x08, REPLACEMENT_INTENT_SCHEMA, true);
  view.setUint16(0x0a, REPLACEMENT_INTENT_RECORD_BYTES, true);
  view.setUint32(0x0c, sourcePackId, true);
  view.setUint32(0x10, target.packId, true);
  view.setUint32(0x14, target.revision, true);
  view.setUint32(0x18, target.totalBytes, true);
  view.setUint32(0x1c, target.payloadCrc32, true);
  view.setUint32(0x20, target.headerCrc32, true);
  view.setUint32(0x24, 0, true);
  view.setUint32(0x24, crc32(bytes.subarray(8, REPLACEMENT_INTENT_RECORD_BYTES)), true);
  return bytes;
}

export async function fetchOfficialPack(packId) {
  const definition = packDefinition(packId);
  if (!definition) return null;
  const url = new URL(definition.assetUrl, window.location.href);
  if (url.origin !== window.location.origin) fail(`${definition.name} companion pack leaves this flasher origin`);
  const response = await fetch(url, {
    cache: "no-store",
    credentials: "omit",
    mode: "same-origin",
    referrerPolicy: "no-referrer",
    signal: AbortSignal.timeout(15000),
  });
  if (!response.ok) fail(`${definition.name} companion pack returned HTTP ${response.status}`);
  const declaredLength = response.headers.get("content-length");
  if (declaredLength !== null && Number(declaredLength) !== definition.bytes) {
    fail(`${definition.name} companion pack response has an unexpected size`);
  }
  const bytes = new Uint8Array(await response.arrayBuffer());
  await verifyPackBytes(definition, bytes);
  return Object.freeze({
    definition,
    record: Object.freeze({
      role: `companion_pack_${definition.id}`,
      offset: PACK_SLOT.offset,
      bytes: definition.bytes,
      sha256: definition.sha256,
    }),
    bytes,
  });
}

export async function reverifyPack(pack) {
  if (!pack?.definition || !pack?.record) fail("selected companion pack is not loaded");
  if (pack.definition.source === "unlocked_file") {
    const metadata = validateUnlockedPackBytes(pack.bytes);
    if (
      pack.definition.id !== UNLOCKED_PACK_ID
      || pack.record.offset !== PACK_SLOT.offset
      || pack.record.bytes !== metadata.totalBytes
      || pack.record.bytes !== pack.definition.bytes
      || pack.record.sha256 !== pack.definition.sha256
      || pack.definition.name !== metadata.displayName
      || pack.definition.packId !== metadata.packId
      || pack.definition.revision !== metadata.revision
      || pack.definition.payloadCrc32 !== metadata.payloadCrc32
      || pack.definition.headerCrc32 !== metadata.headerCrc32
    ) {
      fail("selected unlocked companion pack metadata changed after verification");
    }
    if (await sha256Hex(pack.bytes) !== pack.definition.sha256) {
      fail("selected unlocked companion pack SHA-256 changed after verification");
    }
    return;
  }
  const expected = packDefinition(pack.definition.id);
  if (
    pack.record.offset !== PACK_SLOT.offset
    || pack.record.bytes !== expected.bytes
    || pack.record.sha256 !== expected.sha256
  ) {
    fail("selected companion pack metadata changed after verification");
  }
  await verifyPackBytes(expected, pack.bytes);
}
