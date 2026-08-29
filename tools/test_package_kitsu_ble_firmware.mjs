#!/usr/bin/env node

import assert from "node:assert/strict";
import {
  createHash,
  createPublicKey,
  generateKeyPairSync,
  sign,
} from "node:crypto";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import test from "node:test";

import {
  CHUNK_BYTES,
  ESP32S3_CHIP_ID,
  ESP_IMAGE_CHECKSUM_SEED,
  ESP_IMAGE_HEADER_BYTES,
  FIRMWARE_APP0_OFFSET,
  FIRMWARE_APP1_OFFSET,
  FIRMWARE_CONNECTIVITY_BYTES,
  FIRMWARE_CONNECTIVITY_OFFSET,
  FIRMWARE_COREDUMP_BYTES,
  FIRMWARE_COREDUMP_OFFSET,
  FIRMWARE_DEVICE_CLASS,
  FIRMWARE_FLASH_BYTES,
  FIRMWARE_LAYOUT_ID,
  FIRMWARE_NVS_BYTES,
  FIRMWARE_NVS_OFFSET,
  FIRMWARE_OTA_DATA_BYTES,
  FIRMWARE_OTA_DATA_OFFSET,
  FIRMWARE_SPIFFS_BYTES,
  FIRMWARE_SPIFFS_OFFSET,
  HEADER_BYTES,
  MAGIC,
  MAX_IMAGE_BYTES,
  PARTITION_BYTES,
  UPDATE_AUTHORITY_SPKI_SHA256,
  assembleBundle,
  inspectBundle,
  parseCanonicalManifest,
  parseFirmwareIdentity,
  prepareManifest,
  sha256,
  validateEsp32S3Application,
} from "./package_kitsu_ble_firmware.mjs";

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

function esp32S3Image(
  dataBytes = 70_000,
  firmwareVersion = "0.12.0",
  { duplicateIdentity = false, layout = FIRMWARE_LAYOUT_ID } = {},
) {
  assert.equal(dataBytes % 4, 0);
  const header = Buffer.alloc(ESP_IMAGE_HEADER_BYTES);
  header[0] = 0xe9;
  header[1] = 1;
  header[2] = 2;
  header[3] = 0x3f;
  header.writeUInt32LE(0x4037_0000, 4);
  header[8] = 0xee;
  header.writeUInt16LE(ESP32S3_CHIP_ID, 12);
  header[23] = 1;
  const segmentHeader = Buffer.alloc(8);
  segmentHeader.writeUInt32LE(0x3fc9_0000, 0);
  segmentHeader.writeUInt32LE(dataBytes, 4);
  const data = Buffer.alloc(dataBytes);
  for (let index = 0; index < data.length; index += 1) data[index] = index & 0xff;
  let prefix =
    `KITSU-ID1|schema=1|length=0000|version=${firmwareVersion}|` +
    `device_class=${FIRMWARE_DEVICE_CLASS}|layout=${layout}|` +
    `flash=00800000|nvs=00009000/00040000|` +
    `otadata=00049000/00002000|` +
    `app0=00050000|app1=00350000|slot=00300000|` +
    `journal=00001000|max=002ff000|` +
    `spiffs=00670000/00140000|conn=007b0000/00040000|` +
    `coredump=007f0000/00010000`;
  const total = Buffer.byteLength(`${prefix}|crc32=00000000|end\0`, "ascii");
  prefix = prefix.replace("length=0000", `length=${String(total).padStart(4, "0")}`);
  const prefixBytes = Buffer.from(prefix, "ascii");
  const identity = Buffer.from(
    `${prefix}|crc32=${crc32(prefixBytes).toString(16).padStart(8, "0")}|end\0`,
    "ascii",
  );
  identity.copy(data, 256);
  if (duplicateIdentity) identity.copy(data, 512);
  const body = Buffer.concat([header, segmentHeader, data]);
  let checksum = ESP_IMAGE_CHECKSUM_SEED;
  for (const value of data) checksum ^= value;
  const checksumOffset = body.length + (15 - (body.length % 16));
  const checked = Buffer.alloc(checksumOffset + 1);
  body.copy(checked);
  checked[checksumOffset] = checksum;
  const digest = createHash("sha256").update(checked).digest();
  return Buffer.concat([checked, digest]);
}

function fixture({
  releaseId = "kitsu-0.12.0-ble-1",
  firmwareVersion = "0.12.0",
} = {}) {
  const { publicKey, privateKey } = generateKeyPairSync("ed25519");
  const publicKeyPem = publicKey.export({ type: "spki", format: "pem" });
  const spki = publicKey.export({ type: "spki", format: "der" });
  const image = esp32S3Image(70_000, firmwareVersion);
  const manifest = prepareManifest({
    image,
    releaseId,
    firmwareVersion,
  });
  const signature = sign(null, manifest, privateKey);
  const expectedSpkiSha256 = sha256(spki);
  return { image, manifest, signature, publicKeyPem, expectedSpkiSha256 };
}

function expectFailure(action, pattern) {
  assert.throws(action, pattern);
}

test("builds and round-trips the exact signed offline container", () => {
  const input = fixture();
  const bundle = assembleBundle(input);
  assert(bundle.subarray(0, 8).equals(MAGIC));
  assert.equal(bundle.readUInt32BE(8), input.manifest.length);
  assert.equal(bundle.readUInt16BE(12), 64);
  assert.equal(bundle.readUInt16BE(14), 0);
  assert.equal(bundle.readUInt32BE(16), input.image.length);
  assert.equal(bundle.length, HEADER_BYTES + input.manifest.length + 64 + input.image.length);
  const report = inspectBundle({
    bundle,
    publicKeyPem: input.publicKeyPem,
    expectedSpkiSha256: input.expectedSpkiSha256,
  });
  assert.deepEqual(report, {
    releaseId: "kitsu-0.12.0-ble-1",
    firmwareVersion: "0.12.0",
    applicationVersion: "0.12.0",
    firmwareDeviceClass: FIRMWARE_DEVICE_CLASS,
    firmwareLayout: FIRMWARE_LAYOUT_ID,
    flashBytes: FIRMWARE_FLASH_BYTES,
    nvsOffset: FIRMWARE_NVS_OFFSET,
    nvsBytes: FIRMWARE_NVS_BYTES,
    otaDataOffset: FIRMWARE_OTA_DATA_OFFSET,
    otaDataBytes: FIRMWARE_OTA_DATA_BYTES,
    app0Offset: FIRMWARE_APP0_OFFSET,
    app1Offset: FIRMWARE_APP1_OFFSET,
    partitionBytes: PARTITION_BYTES,
    journalBytes: 0x1000,
    maximumImageBytes: MAX_IMAGE_BYTES,
    spiffsOffset: FIRMWARE_SPIFFS_OFFSET,
    spiffsBytes: FIRMWARE_SPIFFS_BYTES,
    connectivityOffset: FIRMWARE_CONNECTIVITY_OFFSET,
    connectivityBytes: FIRMWARE_CONNECTIVITY_BYTES,
    coredumpOffset: FIRMWARE_COREDUMP_OFFSET,
    coredumpBytes: FIRMWARE_COREDUMP_BYTES,
    firmwareIdentityCrc32: parseFirmwareIdentity(input.image).identityCrc32,
    imageBytes: input.image.length,
    imageSha256: sha256(input.image),
    manifestBytes: input.manifest.length,
    manifestSha256: sha256(input.manifest),
    signatureSha256: sha256(input.signature),
    packageBytes: bundle.length,
    packageSha256: sha256(bundle),
  });
});

test("canonical manifest pins the device, partition, chunk, rollback, and exact image", () => {
  const input = fixture();
  const manifest = parseCanonicalManifest(input.manifest);
  assert.equal(PARTITION_BYTES, 0x300000);
  assert.equal(MAX_IMAGE_BYTES, 0x2ff000);
  assert.equal(manifest.device_class, "heltec-wifi-lora-32-v3-esp32s3-8mb");
  assert.equal(manifest.image_format, "esp32s3-app");
  assert.equal(manifest.partition_bytes, PARTITION_BYTES);
  assert.equal(manifest.chunk_bytes, CHUNK_BYTES);
  assert.equal(manifest.rollback, true);
  assert.equal(manifest.image_bytes, input.image.length);
  assert.equal(manifest.image_sha256, sha256(input.image));

  const buildMetadataImage = esp32S3Image(70_000, "0.12.0+qa.1");
  const buildMetadata = prepareManifest({
    image: buildMetadataImage,
    releaseId: "Recovery_1",
    firmwareVersion: "0.12.0+qa.1",
  });
  assert.equal(parseCanonicalManifest(buildMetadata).firmware_version, "0.12.0+qa.1");

  expectFailure(
    () => parseCanonicalManifest(Buffer.concat([input.manifest, Buffer.from("\n")])),
    /canonical|JSON/,
  );
  const legacySlotManifest = Buffer.from(
    input.manifest.toString("ascii").replace(
      `\"partition_bytes\":${PARTITION_BYTES}`,
      `\"partition_bytes\":${0x330000}`,
    ),
    "ascii",
  );
  expectFailure(
    () => parseCanonicalManifest(legacySlotManifest),
    /unsupported/,
  );
  expectFailure(
    () => prepareManifest({ image: input.image, releaseId: "-bad", firmwareVersion: "0.12.0" }),
    /release id/,
  );
  expectFailure(
    () => prepareManifest({ image: input.image, releaseId: "valid", firmwareVersion: "00.12.0" }),
    /firmware version/,
  );
  const oversized = Buffer.alloc(MAX_IMAGE_BYTES + 1, 0xff);
  oversized[0] = 0xe9;
  expectFailure(
    () => prepareManifest({ image: oversized, releaseId: "valid", firmwareVersion: "0.12.0" }),
    /byte length/,
  );
});

test("firmware versions use strict bounded SemVer 2.0 grammar", () => {
  const accepted = [
    "0.20.3",
    "0.20.3-rc.1+build.7",
    "1.0.0-alpha.0",
    "1.0.0-x-y-z.--",
    "1.0.0+001",
  ];
  for (const firmwareVersion of accepted) {
    const input = fixture({ firmwareVersion });
    assert.equal(parseCanonicalManifest(input.manifest).firmware_version, firmwareVersion);
  }

  const rejected = [
    "0.20.3+", "0.20.3-", "0.20.3-rc.1+", "0.20.3-rc..1",
    "0.20.3+build..7", "0.20.3-01", "0.20.3-rc.01", "01.20.3",
    "0.020.3", "0.20.03", "0.20", "0.20.3_rc1", "0.20.3++build",
    "0.20.3-rc+build+again", "999999999999999999999.0.0",
    "9223372036854775808.0.0",
  ];
  for (const firmwareVersion of rejected) {
    expectFailure(
      () => fixture({ firmwareVersion }),
      /firmware version is invalid/,
    );
  }
});

test("validates the complete ESP32-S3 app image, checksum, digest, and EOF", () => {
  const image = esp32S3Image(4096, "0.20.3");
  const validation = validateEsp32S3Application(image);
  assert.equal(validation.chipId, ESP32S3_CHIP_ID);
  assert.equal(validation.segmentCount, 1);
  assert.equal(validation.imageBytes, image.length);
  assert.equal(validation.firmwareVersion, "0.20.3");
  assert.equal(validation.layout, FIRMWARE_LAYOUT_ID);
  assert.equal(validation.partitionBytes, 0x300000);
  assert.equal(validation.journalBytes, 0x1000);
  assert.equal(validation.maximumImageBytes, 0x2ff000);
  assert.deepEqual(parseFirmwareIdentity(image), {
    schema: 1,
    firmwareVersion: "0.20.3",
    deviceClass: FIRMWARE_DEVICE_CLASS,
    layout: FIRMWARE_LAYOUT_ID,
    flashBytes: FIRMWARE_FLASH_BYTES,
    nvsOffset: FIRMWARE_NVS_OFFSET,
    nvsBytes: FIRMWARE_NVS_BYTES,
    otaDataOffset: FIRMWARE_OTA_DATA_OFFSET,
    otaDataBytes: FIRMWARE_OTA_DATA_BYTES,
    app0Offset: FIRMWARE_APP0_OFFSET,
    app1Offset: FIRMWARE_APP1_OFFSET,
    partitionBytes: 0x300000,
    journalBytes: 0x1000,
    maximumImageBytes: 0x2ff000,
    spiffsOffset: FIRMWARE_SPIFFS_OFFSET,
    spiffsBytes: FIRMWARE_SPIFFS_BYTES,
    connectivityOffset: FIRMWARE_CONNECTIVITY_OFFSET,
    connectivityBytes: FIRMWARE_CONNECTIVITY_BYTES,
    coredumpOffset: FIRMWARE_COREDUMP_OFFSET,
    coredumpBytes: FIRMWARE_COREDUMP_BYTES,
    markerOffset: validation.markerOffset,
    markerBytes: validation.markerBytes,
    identityCrc32: validation.identityCrc32,
  });

  const wrongChip = Buffer.from(image);
  wrongChip.writeUInt16LE(0x0005, 12);
  expectFailure(
    () => validateEsp32S3Application(wrongChip),
    /not an ESP32-S3 image/,
  );

  const badSegmentLength = Buffer.from(image);
  badSegmentLength.writeUInt32LE(0xffff_ffff, ESP_IMAGE_HEADER_BYTES + 4);
  expectFailure(
    () => validateEsp32S3Application(badSegmentLength),
    /segment range/,
  );

  const badChecksum = Buffer.from(image);
  badChecksum[badChecksum.length - 33] ^= 1;
  expectFailure(
    () => validateEsp32S3Application(badChecksum),
    /checksum is invalid/,
  );

  const badDigest = Buffer.from(image);
  badDigest[badDigest.length - 1] ^= 1;
  expectFailure(
    () => validateEsp32S3Application(badDigest),
    /validation digest is invalid/,
  );

  const noDigestFlag = Buffer.from(image);
  noDigestFlag[23] = 0;
  expectFailure(
    () => validateEsp32S3Application(noDigestFlag),
    /must contain an appended validation digest/,
  );

  expectFailure(
    () => validateEsp32S3Application(Buffer.concat([image, Buffer.from([0])])),
    /truncated or trailing ESP image data/,
  );

  expectFailure(
    () => validateEsp32S3Application(
      esp32S3Image(4096, "0.20.3", { duplicateIdentity: true }),
    ),
    /exactly one Kitsu identity/,
  );
  expectFailure(
    () => validateEsp32S3Application(
      esp32S3Image(4096, "0.20.3", { layout: "legacy-layout" }),
    ),
    /schema.*layout/,
  );
  expectFailure(
    () => prepareManifest({
      image: esp32S3Image(4096, "0.20.4"),
      releaseId: "identity-mismatch",
      firmwareVersion: "0.20.3",
    }),
    /identity does not match/,
  );
});

test("tampering, the wrong authority, and trailing bytes fail closed", () => {
  const input = fixture();
  const bundle = assembleBundle(input);

  const tamperedSignature = Buffer.from(input.signature);
  tamperedSignature[0] ^= 1;
  expectFailure(
    () => assembleBundle({ ...input, signature: tamperedSignature }),
    /signature is invalid/,
  );

  const tamperedImage = Buffer.from(input.image);
  tamperedImage[ESP_IMAGE_HEADER_BYTES + 8 + 100] ^= 1;
  let repairedChecksum = ESP_IMAGE_CHECKSUM_SEED;
  const segmentBytes = tamperedImage.readUInt32LE(ESP_IMAGE_HEADER_BYTES + 4);
  const segmentStart = ESP_IMAGE_HEADER_BYTES + 8;
  for (let index = segmentStart; index < segmentStart + segmentBytes; index += 1) {
    repairedChecksum ^= tamperedImage[index];
  }
  const checksumOffset = tamperedImage.length - 33;
  tamperedImage[checksumOffset] = repairedChecksum;
  createHash("sha256")
    .update(tamperedImage.subarray(0, checksumOffset + 1))
    .digest()
    .copy(tamperedImage, checksumOffset + 1);
  expectFailure(
    () => assembleBundle({ ...input, image: tamperedImage }),
    /does not bind/,
  );

  const other = generateKeyPairSync("ed25519").publicKey;
  const otherPem = other.export({ type: "spki", format: "pem" });
  expectFailure(
    () => assembleBundle({ ...input, publicKeyPem: otherPem }),
    /SPKI is not pinned/,
  );

  expectFailure(
    () => inspectBundle({
      bundle: Buffer.concat([bundle, Buffer.from([0])]),
      publicKeyPem: input.publicKeyPem,
      expectedSpkiSha256: input.expectedSpkiSha256,
    }),
    /truncated or trailing/,
  );
  const badFlags = Buffer.from(bundle);
  badFlags.writeUInt16BE(1, 14);
  expectFailure(
    () => inspectBundle({
      bundle: badFlags,
      publicKeyPem: input.publicKeyPem,
      expectedSpkiSha256: input.expectedSpkiSha256,
    }),
    /header is invalid/,
  );
});

test("tracked production public key matches the firmware pin", () => {
  const pem = readFileSync(resolve(
    "platform/public-site/downloads/update-ed25519-public.pem",
  ));
  const key = createPublicKey(pem);
  const spki = key.export({ type: "spki", format: "der" });
  assert.equal(sha256(spki), UPDATE_AUTHORITY_SPKI_SHA256);
});
