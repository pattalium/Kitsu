import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import {
  FLASH_PLAN,
  pemToSpki,
  sha256Hex,
  UPDATE_AUTHORITY_SPKI_SHA256,
  validateReleaseManifest,
  verifyDetachedManifest,
} from "../src/release.js";

const acceptedSha = "1".repeat(64);
const appSha = "106ecd2f2013f13997bfb1994a4ba4589b3e9fa9bbf07153ccf7ce3611ee6d67";

function releaseFixture() {
  const releaseId = "kitsu-0.11.1-reflashable-1";
  return {
    schema: "kitsu.firmware-update.v1",
    release_id: releaseId,
    firmware_version: "0.11.1",
    release_channel: "stable",
    artifact_status: "available",
    published_at: "2026-08-18T07:00:00Z",
    device_class: "heltec-wifi-lora-32-v3-esp32s3-8mb",
    chip: "esp32s3",
    physical_acceptance: {
      schema: "kitsu.firmware-publication-authorization.v1",
      status: "passed",
      evidence_sha256: acceptedSha,
      application_sha256: appSha,
      partition_table_sha256: FLASH_PLAN.partitionSha256,
      accepted_at: "2026-08-18T06:30:00Z",
    },
    writes: [
      {
        role: "partition_table",
        path: `firmware/${releaseId}/kitsu868-partitions.bin`,
        offset: 32768,
        bytes: 3072,
        sha256: FLASH_PLAN.partitionSha256,
        encrypted: false,
        secure_boot_signed: false,
      },
      {
        role: "application",
        path: `firmware/${releaseId}/kitsu868-app.bin`,
        offset: 65536,
        bytes: 1380288,
        sha256: appSha,
        encrypted: false,
        secure_boot_signed: false,
      },
    ],
    operations: { erase_flash: false },
    preserves: {
      bootloader: true,
      ota_data: true,
      nvs: true,
      companion_pack: true,
      kitsu_connectivity: true,
    },
    capabilities: {
      full_chip_erase_available: true,
      stock_meshcore_restore_available: true,
    },
    security: {
      mode: "reflashable",
      efuse_writes: false,
      secure_boot: false,
      flash_encryption: false,
    },
    flash: { flash_mode: "dio", flash_frequency: "80m", flash_size: "8MB", readback_verify: true },
  };
}

function mutated(change) {
  const fixture = structuredClone(releaseFixture());
  change(fixture);
  return fixture;
}

test("accepts exactly the frozen two-write owner-reflashable contract", () => {
  const release = validateReleaseManifest(releaseFixture());
  assert.equal(release.artifacts.length, 2);
  assert.deepEqual(release.artifacts.map((item) => item.offset), [0x008000, 0x010000]);
  assert.deepEqual(
    release.artifacts.map((item) => new URL(item.url).origin),
    ["https://updates.k32.run", "https://updates.k32.run"],
  );
});

test("rejects every expansion of the flash authority", () => {
  const unsafe = [
    mutated((item) => item.writes.push(structuredClone(item.writes[1]))),
    mutated((item) => { item.writes[0].offset = 0; }),
    mutated((item) => { item.writes[1].offset = 0xe000; }),
    mutated((item) => { item.operations.erase_flash = true; }),
    mutated((item) => { item.security.efuse_writes = true; }),
    mutated((item) => { item.security.secure_boot = true; }),
    mutated((item) => { item.security.flash_encryption = true; }),
    mutated((item) => { item.writes[0].sha256 = "0".repeat(64); }),
    mutated((item) => { item.writes[1].bytes += 1; }),
    mutated((item) => { item.writes[1].path = "../bootloader.bin"; }),
    mutated((item) => { item.unrecognized = true; }),
  ];
  for (const manifest of unsafe) {
    assert.throws(() => validateReleaseManifest(manifest));
  }
});

test("pins the installed Kitsu Ed25519 SPKI and verifies exact signed bytes", async () => {
  const fixtures = new URL("./fixtures/", import.meta.url);
  const [manifest, signatureBase64, pem] = await Promise.all([
    readFile(new URL("known-signed-message.json", fixtures)),
    readFile(new URL("known-signed-message.sig.b64", fixtures), "utf8"),
    readFile(new URL("update-authority-public.txt", fixtures), "utf8"),
  ]);
  const signature = Buffer.from(signatureBase64.trim(), "base64");
  assert.equal(signature.byteLength, 64);
  const spki = pemToSpki(pem);
  assert.equal(await sha256Hex(spki), UPDATE_AUTHORITY_SPKI_SHA256);
  await verifyDetachedManifest(new Uint8Array(manifest), new Uint8Array(signature), pem);

  const tampered = new Uint8Array(manifest);
  tampered[tampered.length - 1] ^= 1;
  await assert.rejects(
    verifyDetachedManifest(tampered, new Uint8Array(signature), pem),
    /signature is invalid/,
  );
});

test("computes byte-exact SHA-256 digests", async () => {
  assert.equal(
    await sha256Hex(new TextEncoder().encode("K32 Kitsu")),
    "162962d70b45919009e29a470e55b650b0b25220c20258ab21e288204cc342a5",
  );
});
