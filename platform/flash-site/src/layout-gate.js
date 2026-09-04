import { FLASH_PLAN, sha256Hex } from "./release.js";

export const PARTITION_TABLE_OFFSET = 0x008000;
export const PARTITION_TABLE_SECTOR_BYTES = 0x001000;
export const PARTITION_TABLE_IMAGE_BYTES = 0x000c00;
export const MIGRATED_PARTITION_TABLE_SHA256 =
  "3337f0ec25e653d8c0bf9534abeb147a7505f41b1c2e25b53bb6cc74d395b532";
export const HELTEC_FACTORY_PARTITION_TABLE_SHA256 =
  "1d9cca96de0fe07ad7fc0648b9878ddecd9ce565e38b589ad20fea698ed4c80c";

function fail(message) {
  throw new Error(message);
}

/**
 * Classify the complete physical partition-table sector without accepting
 * prefixes, padded/truncated reads, or a table with non-erased trailing bytes.
 */
export async function classifyPartitionTableSector(sector) {
  if (!(sector instanceof Uint8Array) || sector.byteLength !== PARTITION_TABLE_SECTOR_BYTES) {
    return Object.freeze({ kind: "unknown", reason: "partition-table sector read is incomplete" });
  }
  const image = sector.subarray(0, PARTITION_TABLE_IMAGE_BYTES);
  const trailing = sector.subarray(PARTITION_TABLE_IMAGE_BYTES);
  if (!trailing.every((value) => value === 0xff)) {
    return Object.freeze({ kind: "unknown", reason: "partition-table sector has unexpected trailing data" });
  }
  const sha256 = await sha256Hex(image);
  return classifyPartitionTableDigest(sha256);
}

export function classifyPartitionTableDigest(sha256) {
  if (sha256 === FLASH_PLAN.partitionSha256) {
    return Object.freeze({ kind: "legacy", sha256 });
  }
  if (sha256 === MIGRATED_PARTITION_TABLE_SHA256) {
    return Object.freeze({ kind: "migrated", sha256 });
  }
  if (sha256 === HELTEC_FACTORY_PARTITION_TABLE_SHA256) {
    return Object.freeze({ kind: "factory", sha256 });
  }
  return Object.freeze({ kind: "unknown", reason: "partition-table image is not an approved layout", sha256 });
}

export async function inspectInstalledFlashLayout(loader) {
  if (!loader || typeof loader.readFlash !== "function") {
    fail("partition-table inspection requires an active ROM loader");
  }
  const sector = await loader.readFlash(PARTITION_TABLE_OFFSET, PARTITION_TABLE_SECTOR_BYTES);
  return classifyPartitionTableSector(sector);
}

export function requireLegacyLayoutClassification(layout) {
  if (layout?.kind === "legacy") return layout;
  if (layout?.kind === "migrated") {
    fail(
      "this Heltec already uses the Kitsu 0.20.3 expanded-NVS layout; "
      + "legacy Web Serial writes are blocked—use Android's signed .kitsu-fw updater",
    );
  }
  if (layout?.kind === "factory") {
    fail("this Heltec has the approved factory layout; use the new-board initializer");
  }
  fail(
    `partition-table safety gate rejected an unknown or partial layout (${layout?.reason ?? "invalid result"}); `
    + "nothing was written—use the dedicated Kitsu recovery ceremony",
  );
}

/**
 * This historical flasher is intentionally authorized for one exact legacy
 * source layout only. The 0.20.3 migration has a separate, table-last serial
 * ceremony and all later firmware updates use signed .kitsu-fw packages.
 */
export async function requireLegacyFlashLayout(loader) {
  const layout = await inspectInstalledFlashLayout(loader);
  return requireLegacyLayoutClassification(layout);
}
