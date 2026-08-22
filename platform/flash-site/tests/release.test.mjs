import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import {
  FLASH_PLAN,
  pemToSpki,
  reverifyArtifacts,
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
    schema: "kitsu.firmware-update.v2",
    release_id: releaseId,
    firmware_version: "0.11.1",
    release_channel: "stable",
    artifact_status: "available",
    published_at: "2026-08-18T07:00:00Z",
    device_class: "heltec-wifi-lora-32-v3-esp32s3-8mb",
    chip: "esp32s3",
    physical_acceptance: {
      schema: "kitsu.firmware-publication-authorization.v2",
      status: "passed",
      evidence_sha256: acceptedSha,
      bootloader_sha256: FLASH_PLAN.bootloaderSha256,
      application_sha256: appSha,
      partition_table_sha256: FLASH_PLAN.partitionSha256,
      ota_journal_clear_sha256: FLASH_PLAN.otaJournalSha256,
      legacy_connectivity_clear_sha256: FLASH_PLAN.legacyConnectivitySha256,
      accepted_at: "2026-08-18T06:30:00Z",
    },
    writes: [
      {
        role: "bootloader",
        path: `firmware/${releaseId}/kitsu868-bootloader.bin`,
        offset: FLASH_PLAN.bootloaderOffset,
        bytes: FLASH_PLAN.bootloaderBytes,
        sha256: FLASH_PLAN.bootloaderSha256,
        encrypted: false,
        secure_boot_signed: false,
      },
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
        role: "application_app0",
        path: `firmware/${releaseId}/kitsu868-app.bin`,
        offset: 65536,
        bytes: 1380288,
        sha256: appSha,
        encrypted: false,
        secure_boot_signed: false,
      },
      {
        role: "ota_journal_app0_clear",
        path: `firmware/${releaseId}/kitsu868-ota-journal-clear.bin`,
        offset: FLASH_PLAN.app0JournalOffset,
        bytes: FLASH_PLAN.otaJournalBytes,
        sha256: FLASH_PLAN.otaJournalSha256,
        encrypted: false,
        secure_boot_signed: false,
      },
      {
        role: "application_app1",
        path: `firmware/${releaseId}/kitsu868-app.bin`,
        offset: 3407872,
        bytes: 1380288,
        sha256: appSha,
        encrypted: false,
        secure_boot_signed: false,
      },
      {
        role: "ota_journal_app1_clear",
        path: `firmware/${releaseId}/kitsu868-ota-journal-clear.bin`,
        offset: FLASH_PLAN.app1JournalOffset,
        bytes: FLASH_PLAN.otaJournalBytes,
        sha256: FLASH_PLAN.otaJournalSha256,
        encrypted: false,
        secure_boot_signed: false,
      },
      {
        role: "legacy_connectivity_clear",
        path: `firmware/${releaseId}/kitsu868-legacy-connectivity-clear.bin`,
        offset: FLASH_PLAN.legacyConnectivityOffset,
        bytes: FLASH_PLAN.legacyConnectivityBytes,
        sha256: FLASH_PLAN.legacyConnectivitySha256,
        encrypted: false,
        secure_boot_signed: false,
      },
    ],
    operations: {
      erase_flash: false,
      retire_legacy_connectivity: true,
      retire_legacy_lan_action_state: true,
    },
    preserves: {
      ota_data: true,
      companion_state: true,
      companion_pack: true,
      controller_store: true,
      meshcore_state: true,
      coredump: true,
    },
    capabilities: {
      full_chip_erase_available: true,
      rollback_bootloader: true,
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

test("accepts exactly the fixed seven-write local-only A/B serial recovery contract", () => {
  const release = validateReleaseManifest(releaseFixture());
  assert.equal(release.artifacts.length, 7);
  assert.deepEqual(
    release.artifacts.map((item) => item.offset),
    [0x000000, 0x008000, 0x010000, 0x33f000, 0x340000, 0x66f000, 0x7b0000],
  );
  assert.equal(release.artifacts[2].path, release.artifacts[4].path);
  assert.equal(release.artifacts[2].sha256, release.artifacts[4].sha256);
  assert.equal(release.artifacts[3].path, release.artifacts[5].path);
  assert.equal(release.artifacts[3].sha256, release.artifacts[5].sha256);
  assert.deepEqual(
    release.artifacts.map((item) => new URL(item.url).origin),
    Array(7).fill("https://updates.k32.run"),
  );
});

test("keeps every write inside its one fixed boot, app, journal, or retired-connectivity region", () => {
  const release = validateReleaseManifest(releaseFixture());
  const [bootloader, partition, app0, app0Journal, app1, app1Journal, legacyConnectivity] = release.artifacts;
  assert.equal(bootloader.offset, 0);
  assert.equal(bootloader.offset + bootloader.bytes <= 0x008000, true);
  assert.equal(partition.offset + partition.bytes <= 0x009000, true);
  assert.equal(app0.offset, FLASH_PLAN.app0Offset);
  assert.equal(app1.offset, FLASH_PLAN.app1Offset);
  assert.equal(app0.bytes <= FLASH_PLAN.applicationSlotBytes - FLASH_PLAN.otaJournalBytes, true);
  assert.equal(app1.bytes <= FLASH_PLAN.applicationSlotBytes - FLASH_PLAN.otaJournalBytes, true);
  assert.equal(app0Journal.offset, app0.offset + FLASH_PLAN.applicationSlotBytes - FLASH_PLAN.otaJournalBytes);
  assert.equal(app1Journal.offset, app1.offset + FLASH_PLAN.applicationSlotBytes - FLASH_PLAN.otaJournalBytes);
  assert.equal(app0Journal.offset + app0Journal.bytes, app1.offset);
  assert.equal(app1Journal.offset + app1Journal.bytes, 0x670000);
  assert.equal(app0.offset + FLASH_PLAN.applicationSlotBytes, app1.offset);
  assert.equal(app1.offset + FLASH_PLAN.applicationSlotBytes, 0x670000);
  assert.equal(legacyConnectivity.offset, 0x7b0000);
  assert.equal(legacyConnectivity.bytes, 0x40000);
  assert.equal(legacyConnectivity.offset + legacyConnectivity.bytes, 0x7f0000);
});

test("rejects the legacy two-write contract so preserved OTA data cannot select stale app1", () => {
  const legacy = mutated((item) => {
    item.schema = "kitsu.firmware-update.v1";
    item.writes = item.writes.slice(1, 3);
  });
  assert.throws(() => validateReleaseManifest(legacy), /schema is not supported/);
});

test("rejects every expansion of the flash authority", () => {
  const unsafe = [
    mutated((item) => item.writes.push(structuredClone(item.writes[5]))),
    mutated((item) => { item.writes[0].offset = 0x1000; }),
    mutated((item) => { item.writes[0].sha256 = "0".repeat(64); }),
    mutated((item) => { item.writes[1].offset = 0; }),
    mutated((item) => { item.writes[2].offset = 0xe000; }),
    mutated((item) => { item.writes[3].offset -= 0x1000; }),
    mutated((item) => { item.writes[4].offset = 0x350000; }),
    mutated((item) => { item.writes[4].role = "application_app0"; }),
    mutated((item) => { item.writes[4].path = `firmware/${item.release_id}/different.bin`; }),
    mutated((item) => { item.writes[4].sha256 = "2".repeat(64); }),
    mutated((item) => { item.writes[4].bytes -= 1; }),
    mutated((item) => { item.writes[5].path = `firmware/${item.release_id}/different-journal.bin`; }),
    mutated((item) => { item.writes[5].sha256 = "3".repeat(64); }),
    mutated((item) => { item.writes[6].offset -= 0x1000; }),
    mutated((item) => { item.writes[6].bytes -= 1; }),
    mutated((item) => { item.writes[6].sha256 = "6".repeat(64); }),
    mutated((item) => { item.writes[6].path = `firmware/${item.release_id}/different-clear.bin`; }),
    mutated((item) => { item.operations.erase_flash = true; }),
    mutated((item) => { item.operations.retire_legacy_connectivity = false; }),
    mutated((item) => { item.operations.retire_legacy_lan_action_state = false; }),
    mutated((item) => { item.security.efuse_writes = true; }),
    mutated((item) => { item.security.secure_boot = true; }),
    mutated((item) => { item.security.flash_encryption = true; }),
    mutated((item) => { item.writes[1].sha256 = "0".repeat(64); }),
    mutated((item) => { item.writes[2].bytes += 1; }),
    mutated((item) => { item.writes[2].path = "../application.bin"; }),
    mutated((item) => { item.physical_acceptance.bootloader_sha256 = "4".repeat(64); }),
    mutated((item) => { item.physical_acceptance.ota_journal_clear_sha256 = "5".repeat(64); }),
    mutated((item) => { item.physical_acceptance.legacy_connectivity_clear_sha256 = "7".repeat(64); }),
    mutated((item) => { item.capabilities.rollback_bootloader = false; }),
    mutated((item) => { item.firmware_version = "00.11.1"; }),
    mutated((item) => { item.preserves.companion_state = false; }),
    mutated((item) => { item.preserves.companion_pack = false; }),
    mutated((item) => { item.preserves.controller_store = false; }),
    mutated((item) => { item.preserves.meshcore_state = false; }),
    mutated((item) => { item.preserves.coredump = false; }),
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

test("pre-write reverification requires the complete seven-region release", async () => {
  await assert.rejects(
    reverifyArtifacts({ artifacts: Array(3).fill({}) }),
    /verified release is not loaded/,
  );
  await assert.rejects(
    reverifyArtifacts({
      artifacts: Array.from({ length: 7 }, () => ({
        record: { role: "test", bytes: 1, sha256: "0".repeat(64) },
        bytes: new Uint8Array(),
      })),
    }),
    /artifact changed in memory/,
  );
  assert.equal(
    await sha256Hex(new Uint8Array(FLASH_PLAN.legacyConnectivityBytes).fill(0xff)),
    FLASH_PLAN.legacyConnectivitySha256,
  );
});
