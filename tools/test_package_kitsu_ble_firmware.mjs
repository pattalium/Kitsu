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
  HEADER_BYTES,
  MAGIC,
  MAX_IMAGE_BYTES,
  PARTITION_BYTES,
  UPDATE_AUTHORITY_SPKI_SHA256,
  assembleBundle,
  inspectBundle,
  parseCanonicalManifest,
  prepareManifest,
  sha256,
  validateEsp32S3Application,
} from "./package_kitsu_ble_firmware.mjs";

function esp32S3Image(dataBytes = 70_000) {
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

function fixture() {
  const { publicKey, privateKey } = generateKeyPairSync("ed25519");
  const publicKeyPem = publicKey.export({ type: "spki", format: "pem" });
  const spki = publicKey.export({ type: "spki", format: "der" });
  const image = esp32S3Image();
  const manifest = prepareManifest({
    image,
    releaseId: "kitsu-0.12.0-ble-1",
    firmwareVersion: "0.12.0",
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
  assert.equal(manifest.device_class, "heltec-wifi-lora-32-v3-esp32s3-8mb");
  assert.equal(manifest.image_format, "esp32s3-app");
  assert.equal(manifest.partition_bytes, PARTITION_BYTES);
  assert.equal(manifest.chunk_bytes, CHUNK_BYTES);
  assert.equal(manifest.rollback, true);
  assert.equal(manifest.image_bytes, input.image.length);
  assert.equal(manifest.image_sha256, sha256(input.image));

  const buildMetadata = prepareManifest({
    image: input.image,
    releaseId: "Recovery_1",
    firmwareVersion: "0.12.0+qa.1",
  });
  assert.equal(parseCanonicalManifest(buildMetadata).firmware_version, "0.12.0+qa.1");

  expectFailure(
    () => parseCanonicalManifest(Buffer.concat([input.manifest, Buffer.from("\n")])),
    /canonical|JSON/,
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

test("validates the complete ESP32-S3 app image, checksum, digest, and EOF", () => {
  const image = esp32S3Image(4096);
  assert.deepEqual(validateEsp32S3Application(image), {
    chipId: ESP32S3_CHIP_ID,
    segmentCount: 1,
    imageBytes: image.length,
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
