import { sha256Hex } from "./release.js";

export const PACK_SLOT = Object.freeze({
  offset: 0x670000,
  bytes: 0x140000,
});

export const UNLOCKED_PACK_ID = "unlocked";

const K868_FORMAT = Object.freeze({
  version: 1,
  headerBytes: 64,
  clipBytes: 12,
  stepBytes: 4,
  frameWidth: 64,
  frameHeight: 64,
  frameBytes: 512,
  maxClips: 512,
  maxSteps: 65535,
  maxStepsPerClip: 256,
  maxRole: 11,
  maxPlaybackMode: 3,
  minStepMs: 100,
  maxStepMs: 60000,
});

const K868_MAGIC = Object.freeze([0x4b, 0x38, 0x36, 0x38, 0x50, 0x4b, 0x31, 0x00]);

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
    filename: "Kitsu868-v0.16.5-fox.k868",
    assetUrl: new URL("../../../assets/packs/fox.k868", import.meta.url).href,
    bytes: 24976,
    sha256: "c868386770b6083dcd8f7c01ec7fe455faec476a96c724ab62f09770fdcdab38",
  }),
  cat: Object.freeze({
    id: "cat",
    name: "Cat",
    filename: "Kitsu868-v0.16.5-cat.k868",
    assetUrl: new URL("../../../assets/packs/cat.k868", import.meta.url).href,
    bytes: 24976,
    sha256: "8d19d6b8bc584d9aaee5a6867504fd23c1862c907bbeb1affd9611e35bf2a6d7",
  }),
  dog: Object.freeze({
    id: "dog",
    name: "Dog",
    filename: "Kitsu868-v0.16.5-dog.k868",
    assetUrl: new URL("../../../assets/packs/dog.k868", import.meta.url).href,
    bytes: 24976,
    sha256: "8652aad28816d52fca334766ebefb5c38aec1b09dcc72783414998d17a46e261",
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
    fail(`unlocked companion pack exceeds the ${PACK_SLOT.bytes.toLocaleString()}-byte companion slot`);
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

  if (version !== K868_FORMAT.version) fail(`unsupported companion pack version ${version}`);
  if (headerBytes !== K868_FORMAT.headerBytes) fail(`invalid companion pack header size ${headerBytes}`);
  if (totalBytes !== bytes.byteLength) {
    fail(`companion pack length is ${bytes.byteLength} bytes, header declares ${totalBytes}`);
  }
  if (PACK_SLOT.offset + totalBytes > PACK_SLOT.offset + PACK_SLOT.bytes) {
    fail("companion pack write would cross the dedicated slot boundary");
  }
  if (packId === 0) fail("companion pack ID must be nonzero");
  if (revision === 0) fail("companion pack revision must be nonzero");
  if (width !== K868_FORMAT.frameWidth || height !== K868_FORMAT.frameHeight) {
    fail(`unsupported companion frame canvas ${width}x${height}`);
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
    + frameCount * K868_FORMAT.frameBytes;
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
    fail(`unlocked companion pack exceeds the ${PACK_SLOT.bytes.toLocaleString()}-byte companion slot`);
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
