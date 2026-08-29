#!/usr/bin/env node
/** Build and inspect the signed offline Kitsu BLE firmware container. */

import {
  createHash,
  createPublicKey,
  verify as verifySignature,
} from "node:crypto";
import {
  lstatSync,
  readFileSync,
  renameSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { basename, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";

export const MAGIC = Buffer.from("KITSUFW1", "ascii");
export const HEADER_BYTES = 20;
export const MAX_MANIFEST_BYTES = 1024;
export const SIGNATURE_BYTES = 64;
export const PARTITION_BYTES = 0x300000;
export const JOURNAL_BYTES = 0x1000;
export const MAX_IMAGE_BYTES = PARTITION_BYTES - JOURNAL_BYTES;
export const CHUNK_BYTES = 4096;
export const MANIFEST_SCHEMA = "kitsu.ble-firmware.v1";
export const DEVICE_CLASS = "heltec-wifi-lora-32-v3-esp32s3-8mb";
export const IMAGE_FORMAT = "esp32s3-app";
export const ESP_IMAGE_HEADER_BYTES = 24;
export const ESP_SEGMENT_HEADER_BYTES = 8;
export const ESP_IMAGE_MAX_SEGMENTS = 16;
export const ESP32S3_CHIP_ID = 0x0009;
export const ESP_IMAGE_CHECKSUM_SEED = 0xef;
export const ESP_IMAGE_DIGEST_BYTES = 32;
export const FIRMWARE_IDENTITY_MAGIC = Buffer.from("KITSU-ID1|", "ascii");
export const FIRMWARE_IDENTITY_SCHEMA = 1;
export const FIRMWARE_LAYOUT_ID = "kitsu-8m-dual-ota-3m-v1";
export const FIRMWARE_DEVICE_CLASS = "heltec-v3.2";
export const FIRMWARE_FLASH_BYTES = 0x800000;
export const FIRMWARE_NVS_OFFSET = 0x009000;
export const FIRMWARE_NVS_BYTES = 0x040000;
export const FIRMWARE_OTA_DATA_OFFSET = 0x049000;
export const FIRMWARE_OTA_DATA_BYTES = 0x002000;
export const FIRMWARE_APP0_OFFSET = 0x050000;
export const FIRMWARE_APP1_OFFSET = 0x350000;
export const FIRMWARE_SPIFFS_OFFSET = 0x670000;
export const FIRMWARE_SPIFFS_BYTES = 0x140000;
export const FIRMWARE_CONNECTIVITY_OFFSET = 0x7b0000;
export const FIRMWARE_CONNECTIVITY_BYTES = 0x040000;
export const FIRMWARE_COREDUMP_OFFSET = 0x7f0000;
export const FIRMWARE_COREDUMP_BYTES = 0x010000;
export const FIRMWARE_IDENTITY_MAX_BYTES = 384;
export const UPDATE_AUTHORITY_SPKI_SHA256 =
  "df530766fbc4fc93e82cdbd354ebe4a17a453c83e9bb7fe2af30ca2d202494ab";

const releaseIdPattern = /^[0-9A-Za-z][0-9A-Za-z._-]{0,63}$/;
const numericIdentifier = String.raw`(?:0|[1-9][0-9]*)`;
const nonNumericIdentifier = String.raw`(?:[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)`;
const prereleaseIdentifier = String.raw`(?:${numericIdentifier}|${nonNumericIdentifier})`;
const buildIdentifier = String.raw`(?:[0-9A-Za-z-]+)`;
const semverPattern = new RegExp(
  String.raw`^${numericIdentifier}\.${numericIdentifier}\.${numericIdentifier}` +
    String.raw`(?:-${prereleaseIdentifier}(?:\.${prereleaseIdentifier})*)?` +
    String.raw`(?:\+${buildIdentifier}(?:\.${buildIdentifier})*)?$`,
);
const maximumSemverCore = 9_223_372_036_854_775_807n;
const digestPattern = /^[0-9a-f]{64}$/;

function fail(message) {
  throw new Error(message);
}

function validSemver(value) {
  if (!semverPattern.test(value)) return false;
  const core = value.split(/[+-]/, 1)[0].split(".");
  return core.every((identifier) => BigInt(identifier) <= maximumSemverCore);
}

export function parseFirmwareIdentity(image) {
  if (!Buffer.isBuffer(image)) fail("application is not a byte buffer");
  const starts = [];
  let cursor = 0;
  while (true) {
    const start = image.indexOf(FIRMWARE_IDENTITY_MAGIC, cursor);
    if (start < 0) break;
    starts.push(start);
    cursor = start + 1;
  }
  if (starts.length !== 1) {
    fail("application must contain exactly one Kitsu identity marker");
  }
  const start = starts[0];
  const limit = Math.min(image.length, start + FIRMWARE_IDENTITY_MAX_BYTES + 1);
  const end = image.indexOf(0, start);
  if (end < 0 || end >= limit) {
    fail("Kitsu identity marker is not bounded and NUL-terminated");
  }
  const raw = image.subarray(start, end);
  if (!raw.equals(Buffer.from(raw.toString("ascii"), "ascii"))) {
    fail("Kitsu identity marker is not ASCII");
  }
  const match = raw.toString("ascii").match(
    /^KITSU-ID1\|schema=([0-9]+)\|length=([0-9]{4})\|version=([^|]+)\|device_class=([^|]+)\|layout=([^|]+)\|flash=([0-9a-f]{8})\|nvs=([0-9a-f]{8})\/([0-9a-f]{8})\|otadata=([0-9a-f]{8})\/([0-9a-f]{8})\|app0=([0-9a-f]{8})\|app1=([0-9a-f]{8})\|slot=([0-9a-f]{8})\|journal=([0-9a-f]{8})\|max=([0-9a-f]{8})\|spiffs=([0-9a-f]{8})\/([0-9a-f]{8})\|conn=([0-9a-f]{8})\/([0-9a-f]{8})\|coredump=([0-9a-f]{8})\/([0-9a-f]{8})\|crc32=([0-9a-f]{8})\|end$/,
  );
  if (!match) fail("Kitsu identity marker has an invalid canonical form");
  const schema = Number(match[1]);
  const length = Number(match[2]);
  const firmwareVersion = match[3];
  const deviceClass = match[4];
  const layout = match[5];
  const flashBytes = Number.parseInt(match[6], 16);
  const nvsOffset = Number.parseInt(match[7], 16);
  const nvsBytes = Number.parseInt(match[8], 16);
  const otaDataOffset = Number.parseInt(match[9], 16);
  const otaDataBytes = Number.parseInt(match[10], 16);
  const app0Offset = Number.parseInt(match[11], 16);
  const app1Offset = Number.parseInt(match[12], 16);
  const partitionBytes = Number.parseInt(match[13], 16);
  const journalBytes = Number.parseInt(match[14], 16);
  const maximumImageBytes = Number.parseInt(match[15], 16);
  const spiffsOffset = Number.parseInt(match[16], 16);
  const spiffsBytes = Number.parseInt(match[17], 16);
  const connectivityOffset = Number.parseInt(match[18], 16);
  const connectivityBytes = Number.parseInt(match[19], 16);
  const coredumpOffset = Number.parseInt(match[20], 16);
  const coredumpBytes = Number.parseInt(match[21], 16);
  const identityCrc32 = match[22];
  const crcBoundary = raw.indexOf(Buffer.from("|crc32=", "ascii"));
  if (length !== raw.length + 1) fail("Kitsu identity length field does not match");
  if (crc32(raw.subarray(0, crcBoundary)).toString(16).padStart(8, "0") !==
      identityCrc32) {
    fail("Kitsu identity CRC32 does not match");
  }
  if (schema !== FIRMWARE_IDENTITY_SCHEMA || deviceClass !== FIRMWARE_DEVICE_CLASS ||
      layout !== FIRMWARE_LAYOUT_ID) {
    fail("Kitsu identity schema, device, or layout is unsupported");
  }
  if (flashBytes !== FIRMWARE_FLASH_BYTES || nvsOffset !== FIRMWARE_NVS_OFFSET ||
      nvsBytes !== FIRMWARE_NVS_BYTES || otaDataOffset !== FIRMWARE_OTA_DATA_OFFSET ||
      otaDataBytes !== FIRMWARE_OTA_DATA_BYTES ||
      app0Offset !== FIRMWARE_APP0_OFFSET || app1Offset !== FIRMWARE_APP1_OFFSET ||
      partitionBytes !== PARTITION_BYTES || journalBytes !== JOURNAL_BYTES ||
      maximumImageBytes !== MAX_IMAGE_BYTES ||
      spiffsOffset !== FIRMWARE_SPIFFS_OFFSET || spiffsBytes !== FIRMWARE_SPIFFS_BYTES ||
      connectivityOffset !== FIRMWARE_CONNECTIVITY_OFFSET ||
      connectivityBytes !== FIRMWARE_CONNECTIVITY_BYTES ||
      coredumpOffset !== FIRMWARE_COREDUMP_OFFSET ||
      coredumpBytes !== FIRMWARE_COREDUMP_BYTES) {
    fail("Kitsu identity flash geometry is unsupported");
  }
  if (!validSemver(firmwareVersion)) {
    fail("Kitsu identity firmware version is invalid");
  }
  return {
    schema,
    firmwareVersion,
    deviceClass,
    layout,
    flashBytes,
    nvsOffset,
    nvsBytes,
    otaDataOffset,
    otaDataBytes,
    app0Offset,
    app1Offset,
    partitionBytes,
    journalBytes,
    maximumImageBytes,
    spiffsOffset,
    spiffsBytes,
    connectivityOffset,
    connectivityBytes,
    coredumpOffset,
    coredumpBytes,
    markerOffset: start,
    markerBytes: raw.length + 1,
    identityCrc32,
  };
}

function crc32(bytes) {
  let value = 0xffff_ffff;
  for (const byte of bytes) {
    value ^= byte;
    for (let bit = 0; bit < 8; bit += 1) {
      value = (value >>> 1) ^ ((value & 1) ? 0xedb8_8320 : 0);
    }
  }
  return (value ^ 0xffff_ffff) >>> 0;
}

export function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function regularFile(path, description, maximum = Number.MAX_SAFE_INTEGER) {
  const absolute = resolve(path);
  const stat = lstatSync(absolute, { throwIfNoEntry: false });
  if (!stat || !stat.isFile() || stat.isSymbolicLink()) {
    fail(`${description} must be a regular non-symlink file`);
  }
  if (stat.size <= 0 || stat.size > maximum) {
    fail(`${description} has an invalid byte length`);
  }
  return { absolute, stat };
}

function validateIdentity(releaseId, firmwareVersion) {
  if (!releaseIdPattern.test(releaseId)) fail("release id is invalid");
  if (Buffer.byteLength(firmwareVersion, "ascii") > 32 ||
      !validSemver(firmwareVersion)) {
    fail("firmware version is invalid");
  }
}

export function canonicalManifest({
  releaseId,
  firmwareVersion,
  imageBytes,
  imageSha256,
}) {
  validateIdentity(releaseId, firmwareVersion);
  if (!Number.isSafeInteger(imageBytes) || imageBytes < 1 || imageBytes > MAX_IMAGE_BYTES) {
    fail("application byte length is invalid");
  }
  if (!digestPattern.test(imageSha256)) fail("application SHA-256 is invalid");
  return Buffer.from(
    `{"schema":"${MANIFEST_SCHEMA}","release_id":"${releaseId}",` +
      `"firmware_version":"${firmwareVersion}","device_class":"${DEVICE_CLASS}",` +
      `"image_format":"${IMAGE_FORMAT}","image_bytes":${imageBytes},` +
      `"image_sha256":"${imageSha256}","partition_bytes":${PARTITION_BYTES},` +
      `"chunk_bytes":${CHUNK_BYTES},"rollback":true}`,
    "ascii",
  );
}

export function validateEsp32S3Application(image) {
  if (!Buffer.isBuffer(image) || image.length < ESP_IMAGE_HEADER_BYTES ||
      image.length > MAX_IMAGE_BYTES) {
    fail("application byte length is invalid");
  }
  const segmentCount = image[1];
  const chipId = image.readUInt16LE(12);
  const hashAppended = image[23];
  if (image[0] !== 0xe9 || segmentCount < 1 ||
      segmentCount > ESP_IMAGE_MAX_SEGMENTS) {
    fail("application has an invalid ESP image header");
  }
  if (chipId !== ESP32S3_CHIP_ID) {
    fail("application is not an ESP32-S3 image");
  }
  if (hashAppended !== 1) {
    fail("application must contain an appended validation digest");
  }

  let cursor = ESP_IMAGE_HEADER_BYTES;
  let checksum = ESP_IMAGE_CHECKSUM_SEED;
  for (let segment = 0; segment < segmentCount; segment += 1) {
    if (cursor + ESP_SEGMENT_HEADER_BYTES > image.length) {
      fail("application has a truncated ESP segment header");
    }
    const loadAddress = image.readUInt32LE(cursor);
    const dataBytes = image.readUInt32LE(cursor + 4);
    cursor += ESP_SEGMENT_HEADER_BYTES;
    if (dataBytes < 1 || dataBytes % 4 !== 0 ||
        loadAddress + dataBytes > 0x1_0000_0000 ||
        dataBytes > image.length - cursor) {
      fail("application has an invalid ESP segment range");
    }
    const end = cursor + dataBytes;
    for (let index = cursor; index < end; index += 1) checksum ^= image[index];
    cursor = end;
  }

  // ESP application images place the one-byte XOR checksum at offset 15 in
  // the next 16-byte block, then append SHA-256 over everything through that
  // checksum.  Exact EOF is part of the signed-image contract.
  const checksumOffset = cursor + (15 - (cursor % 16));
  const digestOffset = checksumOffset + 1;
  const expectedBytes = digestOffset + ESP_IMAGE_DIGEST_BYTES;
  if (expectedBytes !== image.length) {
    fail("application has truncated or trailing ESP image data");
  }
  if (image[checksumOffset] !== checksum) {
    fail("application ESP image checksum is invalid");
  }
  const expectedDigest = createHash("sha256")
    .update(image.subarray(0, digestOffset))
    .digest();
  if (!image.subarray(digestOffset).equals(expectedDigest)) {
    fail("application ESP image validation digest is invalid");
  }
  const identity = parseFirmwareIdentity(image);
  return {
    chipId,
    segmentCount,
    imageBytes: image.length,
    ...identity,
  };
}

function validateImage(image) {
  return {
    imageSha256: sha256(image),
    identity: validateEsp32S3Application(image),
  };
}

export function parseCanonicalManifest(bytes) {
  if (!Buffer.isBuffer(bytes) || bytes.length < 1 || bytes.length > MAX_MANIFEST_BYTES) {
    fail("manifest byte length is invalid");
  }
  if (!bytes.equals(Buffer.from(bytes.toString("utf8"), "utf8"))) {
    fail("manifest is not valid UTF-8");
  }
  let value;
  try {
    value = JSON.parse(bytes.toString("utf8"));
  } catch {
    fail("manifest is not valid JSON");
  }
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    fail("manifest must be an object");
  }
  const keys = [
    "schema", "release_id", "firmware_version", "device_class", "image_format",
    "image_bytes", "image_sha256", "partition_bytes", "chunk_bytes", "rollback",
  ];
  if (Object.keys(value).join("\0") !== keys.join("\0") ||
      value.schema !== MANIFEST_SCHEMA || value.device_class !== DEVICE_CLASS ||
      value.image_format !== IMAGE_FORMAT || value.partition_bytes !== PARTITION_BYTES ||
      value.chunk_bytes !== CHUNK_BYTES || value.rollback !== true) {
    fail("manifest contract is unsupported");
  }
  const canonical = canonicalManifest({
    releaseId: value.release_id,
    firmwareVersion: value.firmware_version,
    imageBytes: value.image_bytes,
    imageSha256: value.image_sha256,
  });
  if (!bytes.equals(canonical)) fail("manifest is not in exact canonical form");
  return value;
}

function verifiedPublicKey(publicKeyPem, expectedSpkiSha256) {
  const key = createPublicKey(publicKeyPem);
  if (key.asymmetricKeyType !== "ed25519") fail("update authority is not Ed25519");
  const spki = key.export({ type: "spki", format: "der" });
  if (sha256(spki) !== expectedSpkiSha256) fail("update authority SPKI is not pinned");
  return key;
}

export function prepareManifest({ image, releaseId, firmwareVersion }) {
  const { imageSha256, identity } = validateImage(image);
  if (identity.firmwareVersion !== firmwareVersion) {
    fail("application identity does not match requested firmware version");
  }
  return canonicalManifest({
    releaseId,
    firmwareVersion,
    imageBytes: image.length,
    imageSha256,
  });
}

export function assembleBundle({
  manifest,
  signature,
  image,
  publicKeyPem,
  expectedSpkiSha256 = UPDATE_AUTHORITY_SPKI_SHA256,
}) {
  const value = parseCanonicalManifest(manifest);
  if (!Buffer.isBuffer(signature) || signature.length !== SIGNATURE_BYTES) {
    fail("detached Ed25519 signature must be exactly 64 bytes");
  }
  const { imageSha256, identity } = validateImage(image);
  if (value.image_bytes !== image.length || value.image_sha256 !== imageSha256) {
    fail("manifest does not bind the exact application bytes");
  }
  if (value.firmware_version !== identity.firmwareVersion) {
    fail("manifest firmware version does not match application identity");
  }
  const key = verifiedPublicKey(publicKeyPem, expectedSpkiSha256);
  if (!verifySignature(null, manifest, key, signature)) {
    fail("manifest signature is invalid");
  }
  const header = Buffer.alloc(HEADER_BYTES);
  MAGIC.copy(header, 0);
  header.writeUInt32BE(manifest.length, 8);
  header.writeUInt16BE(signature.length, 12);
  header.writeUInt16BE(0, 14);
  header.writeUInt32BE(image.length, 16);
  return Buffer.concat([header, manifest, signature, image]);
}

export function inspectBundle({
  bundle,
  publicKeyPem,
  expectedSpkiSha256 = UPDATE_AUTHORITY_SPKI_SHA256,
}) {
  if (!Buffer.isBuffer(bundle) || bundle.length < HEADER_BYTES + SIGNATURE_BYTES + 1) {
    fail("firmware package is truncated");
  }
  if (!bundle.subarray(0, 8).equals(MAGIC)) fail("firmware package magic is invalid");
  const manifestBytes = bundle.readUInt32BE(8);
  const signatureBytes = bundle.readUInt16BE(12);
  const flags = bundle.readUInt16BE(14);
  const imageBytes = bundle.readUInt32BE(16);
  if (manifestBytes < 1 || manifestBytes > MAX_MANIFEST_BYTES ||
      signatureBytes !== SIGNATURE_BYTES || flags !== 0 ||
      imageBytes < 1 || imageBytes > MAX_IMAGE_BYTES) {
    fail("firmware package header is invalid");
  }
  const expectedBytes = HEADER_BYTES + manifestBytes + signatureBytes + imageBytes;
  if (bundle.length !== expectedBytes) fail("firmware package has truncated or trailing data");
  const manifest = bundle.subarray(HEADER_BYTES, HEADER_BYTES + manifestBytes);
  const signature = bundle.subarray(
    HEADER_BYTES + manifestBytes,
    HEADER_BYTES + manifestBytes + signatureBytes,
  );
  const image = bundle.subarray(HEADER_BYTES + manifestBytes + signatureBytes);
  const rebuilt = assembleBundle({
    manifest,
    signature,
    image,
    publicKeyPem,
    expectedSpkiSha256,
  });
  if (!bundle.equals(rebuilt)) fail("firmware package did not round-trip exactly");
  const value = parseCanonicalManifest(manifest);
  const identity = validateEsp32S3Application(image);
  return {
    releaseId: value.release_id,
    firmwareVersion: value.firmware_version,
    applicationVersion: identity.firmwareVersion,
    firmwareDeviceClass: identity.deviceClass,
    firmwareLayout: identity.layout,
    flashBytes: identity.flashBytes,
    nvsOffset: identity.nvsOffset,
    nvsBytes: identity.nvsBytes,
    otaDataOffset: identity.otaDataOffset,
    otaDataBytes: identity.otaDataBytes,
    app0Offset: identity.app0Offset,
    app1Offset: identity.app1Offset,
    partitionBytes: identity.partitionBytes,
    journalBytes: identity.journalBytes,
    maximumImageBytes: identity.maximumImageBytes,
    spiffsOffset: identity.spiffsOffset,
    spiffsBytes: identity.spiffsBytes,
    connectivityOffset: identity.connectivityOffset,
    connectivityBytes: identity.connectivityBytes,
    coredumpOffset: identity.coredumpOffset,
    coredumpBytes: identity.coredumpBytes,
    firmwareIdentityCrc32: identity.identityCrc32,
    imageBytes,
    imageSha256: value.image_sha256,
    manifestBytes,
    manifestSha256: sha256(manifest),
    signatureSha256: sha256(signature),
    packageBytes: bundle.length,
    packageSha256: sha256(bundle),
  };
}

function exclusiveWrite(path, bytes) {
  const output = resolve(path);
  const temporary = resolve(dirname(output), `.${basename(output)}.${process.pid}.tmp`);
  try {
    writeFileSync(temporary, bytes, { flag: "wx", mode: 0o644 });
    if (lstatSync(output, { throwIfNoEntry: false })) fail("output already exists");
    renameSync(temporary, output);
  } finally {
    rmSync(temporary, { force: true });
  }
}

function required(values, name) {
  const value = values[name];
  if (typeof value !== "string" || value.length === 0) fail(`--${name} is required`);
  return value;
}

function cli() {
  const [command, ...args] = process.argv.slice(2);
  if (!command || !["prepare", "assemble", "inspect"].includes(command)) {
    fail("usage: package_kitsu_ble_firmware.mjs <prepare|assemble|inspect> [options]");
  }
  const { values } = parseArgs({
    args,
    strict: true,
    options: {
      image: { type: "string" },
      "release-id": { type: "string" },
      "firmware-version": { type: "string" },
      manifest: { type: "string" },
      signature: { type: "string" },
      "public-key": { type: "string" },
      bundle: { type: "string" },
      output: { type: "string" },
    },
  });
  if (command === "prepare") {
    const imagePath = regularFile(required(values, "image"), "application", MAX_IMAGE_BYTES);
    const image = readFileSync(imagePath.absolute);
    const manifest = prepareManifest({
      image,
      releaseId: required(values, "release-id"),
      firmwareVersion: required(values, "firmware-version"),
    });
    const identity = validateEsp32S3Application(image);
    exclusiveWrite(required(values, "output"), manifest);
    process.stdout.write(`${JSON.stringify({
      manifest_bytes: manifest.length,
      manifest_sha256: sha256(manifest),
      image_bytes: image.length,
      image_sha256: sha256(image),
      application_version: identity.firmwareVersion,
      firmware_layout: identity.layout,
    })}\n`);
    return;
  }
  const keyPath = regularFile(required(values, "public-key"), "public key", 16 * 1024);
  const publicKeyPem = readFileSync(keyPath.absolute);
  if (command === "assemble") {
    const manifestPath = regularFile(required(values, "manifest"), "manifest", MAX_MANIFEST_BYTES);
    const signaturePath = regularFile(required(values, "signature"), "signature", SIGNATURE_BYTES);
    const imagePath = regularFile(required(values, "image"), "application", MAX_IMAGE_BYTES);
    const bundle = assembleBundle({
      manifest: readFileSync(manifestPath.absolute),
      signature: readFileSync(signaturePath.absolute),
      image: readFileSync(imagePath.absolute),
      publicKeyPem,
    });
    exclusiveWrite(required(values, "output"), bundle);
    process.stdout.write(`${JSON.stringify(inspectBundle({ bundle, publicKeyPem }))}\n`);
    return;
  }
  const bundlePath = regularFile(
    required(values, "bundle"),
    "firmware package",
    HEADER_BYTES + MAX_MANIFEST_BYTES + SIGNATURE_BYTES + MAX_IMAGE_BYTES,
  );
  process.stdout.write(`${JSON.stringify(inspectBundle({
    bundle: readFileSync(bundlePath.absolute),
    publicKeyPem,
  }))}\n`);
}

if (process.argv[1] && resolve(process.argv[1]) === resolve(fileURLToPath(import.meta.url))) {
  try {
    cli();
  } catch (error) {
    process.stderr.write(`ERROR ${error instanceof Error ? error.message : String(error)}\n`);
    process.exitCode = 1;
  }
}
