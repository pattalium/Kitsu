"use strict";

export const PUBLISHED_PACK_SCHEMA = "kitsu.published-wild-pack.v1";

export const RARITIES = Object.freeze([
  "common",
  "uncommon",
  "rare",
  "very_rare",
  "epic",
  "legendary",
  "mythical",
]);

const raritySet = new Set(RARITIES);

// Existing starter companions are install choices, not wild-code rewards.
const reservedStarterPackIds = new Set([
  "FDC79D6F",
  "6C393E21",
  "E2B5E7BA",
]);

const requiredKeys = Object.freeze([
  "bytes",
  "displayName",
  "downloadUrl",
  "packId",
  "rarity",
  "schema",
  "sha256",
]);

function hasExactKeys(value, expected) {
  const actual = Object.keys(value).sort();
  return actual.length === expected.length
    && actual.every((key, index) => key === expected[index]);
}

export function normalizePackId(value) {
  if (typeof value !== "string") return null;
  const normalized = value.trim().toUpperCase();
  return /^[A-F0-9]{8}$/.test(normalized) ? normalized : null;
}

export function isPublishedPackEntry(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return false;
  if (!hasExactKeys(value, requiredKeys)) return false;
  if (value.schema !== PUBLISHED_PACK_SCHEMA) return false;

  const packId = normalizePackId(value.packId);
  if (!packId || reservedStarterPackIds.has(packId)) return false;
  if (!raritySet.has(value.rarity)) return false;
  if (!Number.isSafeInteger(value.bytes) || value.bytes < 1 || value.bytes > 2_097_152) return false;
  if (typeof value.sha256 !== "string" || !/^[a-f0-9]{64}$/.test(value.sha256)) return false;
  if (
    typeof value.displayName !== "string"
    || value.displayName.length < 1
    || value.displayName.length > 64
    || !/^[A-Za-z0-9][A-Za-z0-9 '().-]*$/.test(value.displayName)
  ) return false;
  if (typeof value.downloadUrl !== "string") return false;

  const pathMatch = value.downloadUrl.match(
    /^\.\/assets\/[a-z0-9][a-z0-9-]{0,63}\.([a-f0-9]{64})\.k868$/,
  );
  return Boolean(pathMatch && pathMatch[1] === value.sha256);
}

// Publication is explicit. Add a record only when its ordinary .k868 bytes
// exist at the exact same-origin, content-addressed path declared by the record.
export const PUBLISHED_WILD_PACKS = Object.freeze([
  Object.freeze({
    bytes: 24_976,
    displayName: "Frog",
    downloadUrl: "./assets/frog.06461beb8ad592a120a95bfb238a44458060cacf6494e194d7b2fe0dd8d862c8.k868",
    packId: "5CAC86A3",
    rarity: "common",
    schema: PUBLISHED_PACK_SCHEMA,
    sha256: "06461beb8ad592a120a95bfb238a44458060cacf6494e194d7b2fe0dd8d862c8",
  }),
]);

for (const entry of PUBLISHED_WILD_PACKS) {
  if (!isPublishedPackEntry(entry)) {
    throw new TypeError("The published wild-pack catalog contains an invalid entry.");
  }
}

export function publishedPackFor(packId, catalog = PUBLISHED_WILD_PACKS) {
  const normalized = normalizePackId(packId);
  if (!normalized || !Array.isArray(catalog)) return null;
  for (const entry of catalog) {
    if (isPublishedPackEntry(entry) && entry.packId === normalized) return entry;
  }
  return null;
}

export function rarityLabel(rarity) {
  if (!raritySet.has(rarity)) return "Unknown";
  return rarity === "very_rare"
    ? "Very Rare"
    : rarity.charAt(0).toUpperCase() + rarity.slice(1);
}
