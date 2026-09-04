import assert from "node:assert/strict";
import { inflateSync } from "node:zlib";
import test from "node:test";
import {
  classifyPartitionTableDigest,
  classifyPartitionTableSector,
  HELTEC_FACTORY_PARTITION_TABLE_SHA256,
  inspectInstalledFlashLayout,
  MIGRATED_PARTITION_TABLE_SHA256,
  PARTITION_TABLE_IMAGE_BYTES,
  PARTITION_TABLE_OFFSET,
  PARTITION_TABLE_SECTOR_BYTES,
  requireLegacyFlashLayout,
  requireLegacyLayoutClassification,
} from "../src/layout-gate.js";
import { FLASH_PLAN } from "../src/release.js";

const LEGACY_PARTITIONS_ZLIB_BASE64 =
  "eNpbFcDIxDCBgYEhgIEhr6yYAR2sCmBkYHgAZCgwMOSXJKYkliSiyTMIMDAA1TAYMyQWFBhg6mcQZGAwgckbYjG/iYEhHcgSYSguyExLK8aQd2BgqAayWBiyM0uKS+OT8/PyUOSZGRjqGUBuSM4vSk0pzS1A1v/69X8UYFohf2mDuabR+9ltcyrnLEn8PwpGwSgYBaNgFIyCUTAKRsEIAgCe7CoL";
const MIGRATED_PARTITIONS_ZLIB_BASE64 =
  "eNpbFcDIxDCBAQhYGPLKihnQwaoARgaGCSwMDAoMDPkliSmJJYlo8gwCDAysQJYBQ2JBgQGmfgZBBgZTmLwhFvObGBjSgSwRhuKCzLS0Ygx5BwaGarD7sjNLikvjk/Pz8lDkmRkY6oEsRobk/KLUlNLcAmT9r1//RwF8eg97Vi+bPPd6H+tPaf3pK/6PglEwCkbBKBgFo2AUjIJRMIIAAFjiKeI=";
const HELTEC_FACTORY_PARTITIONS_ZLIB_BASE64 =
  "eJxbFcDIxDCBgYEhgIEhr6yYAR2sCmBkYHjAwMCgwMCQX5KYkliSiCbPIMDAwMjAwGDMkFhQYICpn0GQgcEEJm+IxfwmBoZ0BgYGCYbigsy0tGIMeWYGhnoGkB3J+UWpKaW5Bcjyr1//RwFudTumPZortdLiaPiXs398ov+PglEwCkbBKBgFo2AUjIJRMArgAAAj4kVe";

function reviewedLegacyImage() {
  const image = new Uint8Array(inflateSync(Buffer.from(LEGACY_PARTITIONS_ZLIB_BASE64, "base64")));
  assert.equal(image.byteLength, PARTITION_TABLE_IMAGE_BYTES);
  return image;
}

function reviewedMigratedImage() {
  const image = new Uint8Array(inflateSync(Buffer.from(MIGRATED_PARTITIONS_ZLIB_BASE64, "base64")));
  assert.equal(image.byteLength, PARTITION_TABLE_IMAGE_BYTES);
  return image;
}

function heltecFactoryImage() {
  const image = new Uint8Array(inflateSync(Buffer.from(HELTEC_FACTORY_PARTITIONS_ZLIB_BASE64, "base64")));
  assert.equal(image.byteLength, PARTITION_TABLE_IMAGE_BYTES);
  return image;
}

function sectorWithImage(image) {
  const sector = new Uint8Array(PARTITION_TABLE_SECTOR_BYTES).fill(0xff);
  sector.set(image, 0);
  return sector;
}

test("accepts only the exact reviewed legacy partition-table digest", () => {
  const legacy = classifyPartitionTableDigest(FLASH_PLAN.partitionSha256);
  assert.deepEqual(legacy, {
    kind: "legacy",
    sha256: FLASH_PLAN.partitionSha256,
  });
  assert.equal(requireLegacyLayoutClassification(legacy).kind, "legacy");
});

test("accepts the independently generated reviewed legacy table bytes", async () => {
  const sector = sectorWithImage(reviewedLegacyImage());
  assert.deepEqual(await classifyPartitionTableSector(sector), {
    kind: "legacy",
    sha256: FLASH_PLAN.partitionSha256,
  });
  assert.equal(
    (await requireLegacyFlashLayout({ async readFlash() { return sector.slice(); } })).kind,
    "legacy",
  );
});

test("identifies and blocks the exact migrated 256 KiB NVS layout", () => {
  const migrated = classifyPartitionTableDigest(MIGRATED_PARTITION_TABLE_SHA256);
  assert.equal(migrated.kind, "migrated");
  assert.throws(
    () => requireLegacyLayoutClassification(migrated),
    /expanded-NVS layout.*legacy Web Serial writes are blocked/s,
  );
});

test("derives the migrated layout digest from the reviewed table bytes", async () => {
  const sector = sectorWithImage(reviewedMigratedImage());
  assert.deepEqual(await classifyPartitionTableSector(sector), {
    kind: "migrated",
    sha256: MIGRATED_PARTITION_TABLE_SHA256,
  });
  await assert.rejects(
    requireLegacyFlashLayout({ async readFlash() { return sector.slice(); } }),
    /expanded-NVS layout.*legacy Web Serial writes are blocked/s,
  );
});

test("recognizes the stock Heltec V3 factory partition table", async () => {
  const sector = sectorWithImage(heltecFactoryImage());
  assert.deepEqual(await classifyPartitionTableSector(sector), {
    kind: "factory",
    sha256: HELTEC_FACTORY_PARTITION_TABLE_SHA256,
  });
  assert.throws(
    () => requireLegacyLayoutClassification({
      kind: "factory",
      sha256: HELTEC_FACTORY_PARTITION_TABLE_SHA256,
    }),
    /approved factory layout.*new-board initializer/i,
  );
});

test("fails closed for truncated, modified, and non-erased partition sectors", async () => {
  const image = new Uint8Array(PARTITION_TABLE_IMAGE_BYTES).fill(0xa5);
  const valid = sectorWithImage(image);
  const truncated = valid.subarray(0, valid.byteLength - 1);
  const modified = valid.slice();
  modified[17] ^= 1;
  const trailing = valid.slice();
  trailing[PARTITION_TABLE_IMAGE_BYTES] = 0;

  assert.equal((await classifyPartitionTableSector(truncated)).kind, "unknown");
  assert.equal((await classifyPartitionTableSector(modified)).kind, "unknown");
  assert.equal((await classifyPartitionTableSector(trailing)).kind, "unknown");
  for (const sector of [truncated, modified, trailing, valid]) {
    await assert.rejects(
      requireLegacyFlashLayout({ async readFlash() { return sector; } }),
      /safety gate rejected.*nothing was written/s,
    );
  }
  await assert.rejects(inspectInstalledFlashLayout({}), /active ROM loader/);
});

test("physical inspection reads exactly one complete table sector", async () => {
  const reads = [];
  const loader = {
    async readFlash(offset, bytes) {
      reads.push([offset, bytes]);
      return new Uint8Array(PARTITION_TABLE_SECTOR_BYTES).fill(0xff);
    },
  };
  assert.equal((await inspectInstalledFlashLayout(loader)).kind, "unknown");
  assert.deepEqual(reads, [[PARTITION_TABLE_OFFSET, PARTITION_TABLE_SECTOR_BYTES]]);
});

test("a changed or short final table read prevents the first write", async () => {
  const legacy = sectorWithImage(reviewedLegacyImage());
  for (const rejected of [
    legacy.subarray(0, legacy.byteLength - 1),
    (() => { const changed = legacy.slice(); changed[3] ^= 1; return changed; })(),
  ]) {
    const writes = [];
    const reads = [legacy.slice(), rejected];
    const loader = {
      async readFlash() { return reads.shift(); },
      async writeFlash(plan) { writes.push(plan); },
    };
    const connected = await requireLegacyFlashLayout(loader);
    await assert.rejects(async () => {
      const final = await requireLegacyFlashLayout(loader);
      if (final.sha256 !== connected.sha256) throw new Error("partition table changed");
      await loader.writeFlash({ unsafe: true });
    }, /safety gate rejected/);
    assert.deepEqual(writes, []);
  }
});
