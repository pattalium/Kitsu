const UPDATE_ORIGIN = "https://updates.k32.run";
const MANIFEST_URL = `${UPDATE_ORIGIN}/latest.json`;
const SIGNATURE_URL = `${UPDATE_ORIGIN}/latest.json.sig`;
const PUBLIC_KEY_URL = `${UPDATE_ORIGIN}/update-ed25519-public.pem`;

export const UPDATE_AUTHORITY_SPKI_SHA256 =
  "df530766fbc4fc93e82cdbd354ebe4a17a453c83e9bb7fe2af30ca2d202494ab";

export const RELEASE_ENDPOINTS = Object.freeze({
  manifest: MANIFEST_URL,
  signature: SIGNATURE_URL,
  publicKey: PUBLIC_KEY_URL,
});

export const FLASH_PLAN = Object.freeze({
  chip: "esp32s3",
  flashSize: "8MB",
  bootloaderOffset: 0x000000,
  bootloaderBytes: 15104,
  bootloaderSha256: "1776e4dd896a69d0a5c2e79957b0e2a88aa4129b1381d6478683515a1f6af343",
  partitionOffset: 0x008000,
  partitionBytes: 3072,
  partitionSha256: "f9b22e16fcfb701520dd6c7e0791582ececbbd44c317c8d519e3d6b2b9ce8b7a",
  app0Offset: 0x010000,
  app1Offset: 0x340000,
  applicationSlotBytes: 0x330000,
  otaJournalBytes: 0x001000,
  app0JournalOffset: 0x33f000,
  app1JournalOffset: 0x66f000,
  otaJournalSha256: "f47a8ec3e9aff2318d896942282ad4fe37d6391c82914f54a5da8a37de1300c6",
  legacyConnectivityOffset: 0x7b0000,
  legacyConnectivityBytes: 0x040000,
  legacyConnectivitySha256: "3b874d3ba46c638fc3094f8e92fb744ca974893873f8885f54e23760f9b6311b",
  applicationBytes: 1380288,
  applicationSha256: "106ecd2f2013f13997bfb1994a4ba4589b3e9fa9bbf07153ccf7ce3611ee6d67",
});

const TOP_LEVEL_KEYS = [
  "schema",
  "release_id",
  "firmware_version",
  "release_channel",
  "artifact_status",
  "published_at",
  "device_class",
  "chip",
  "physical_acceptance",
  "writes",
  "operations",
  "preserves",
  "capabilities",
  "security",
  "flash",
];

function fail(message) {
  throw new Error(message);
}

function isObject(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function exactKeys(value, required, label) {
  if (!isObject(value)) fail(`${label} must be an object`);
  const actual = Object.keys(value).sort();
  const expected = [...required].sort();
  if (actual.length !== expected.length || actual.some((key, index) => key !== expected[index])) {
    fail(`${label} has missing or unrecognized fields`);
  }
}

function exactBoolean(value, expected, label) {
  if (value !== expected) fail(`${label} must be ${expected}`);
}

function boundedText(value, pattern, label, maximum = 128) {
  if (typeof value !== "string" || value.length < 1 || value.length > maximum || !pattern.test(value)) {
    fail(`${label} is invalid`);
  }
  return value;
}

function sha256Text(value, label) {
  return boundedText(value, /^[0-9a-f]{64}$/, label, 64);
}

function utcTimestamp(value, label) {
  boundedText(
    value,
    /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,6})?Z$/,
    label,
    40,
  );
  if (!Number.isFinite(Date.parse(value))) fail(`${label} is invalid`);
  return value;
}

function exactWriteKeys(write, label) {
  exactKeys(
    write,
    ["role", "path", "offset", "bytes", "sha256", "encrypted", "secure_boot_signed"],
    label,
  );
  exactBoolean(write.encrypted, false, `${label}.encrypted`);
  exactBoolean(write.secure_boot_signed, false, `${label}.secure_boot_signed`);
  sha256Text(write.sha256, `${label}.sha256`);
}

function exactArtifactPath(value, releaseId, filename, label) {
  const expected = `firmware/${releaseId}/${filename}`;
  if (value !== expected) fail(`${label} must be ${expected}`);
  const parsed = new URL(value, `${UPDATE_ORIGIN}/`);
  if (
    parsed.origin !== UPDATE_ORIGIN
    || parsed.pathname !== `/${expected}`
    || parsed.search
    || parsed.hash
  ) {
    fail(`${label} leaves the firmware update origin`);
  }
  return parsed.toString();
}

/** Validate the exact safety-bearing firmware update schema. */
export function validateReleaseManifest(value) {
  exactKeys(value, TOP_LEVEL_KEYS, "manifest");
  if (value.schema !== "kitsu.firmware-update.v2") fail("manifest schema is not supported");
  const releaseId = boundedText(
    value.release_id,
    /^[0-9A-Za-z][0-9A-Za-z._-]{0,63}$/,
    "manifest.release_id",
    64,
  );
  boundedText(
    value.firmware_version,
    /^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:[-+][0-9A-Za-z.-]+)?$/,
    "manifest.firmware_version",
    64,
  );
  if (value.release_channel !== "stable") fail("manifest release channel is not stable");
  if (value.artifact_status !== "available") fail("manifest artifacts are not available");
  utcTimestamp(value.published_at, "manifest.published_at");
  if (value.device_class !== "heltec-wifi-lora-32-v3-esp32s3-8mb") {
    fail("manifest device class is not the Heltec V3 ESP32-S3 8 MiB target");
  }
  if (value.chip !== FLASH_PLAN.chip) fail("manifest chip is not ESP32-S3");

  exactKeys(
    value.physical_acceptance,
    [
      "schema",
      "status",
      "evidence_sha256",
      "accepted_at",
      "bootloader_sha256",
      "application_sha256",
      "partition_table_sha256",
      "ota_journal_clear_sha256",
      "legacy_connectivity_clear_sha256",
    ],
    "manifest.physical_acceptance",
  );
  if (value.physical_acceptance.schema !== "kitsu.firmware-publication-authorization.v2") {
    fail("physical acceptance schema is not supported");
  }
  if (value.physical_acceptance.status !== "passed") fail("physical acceptance has not passed");
  sha256Text(value.physical_acceptance.evidence_sha256, "physical acceptance evidence digest");
  utcTimestamp(value.physical_acceptance.accepted_at, "physical acceptance timestamp");
  if (value.physical_acceptance.bootloader_sha256 !== FLASH_PLAN.bootloaderSha256) {
    fail("physical acceptance does not bind the reviewed rollback bootloader");
  }
  if (value.physical_acceptance.partition_table_sha256 !== FLASH_PLAN.partitionSha256) {
    fail("physical acceptance does not bind the reviewed partition table");
  }
  if (value.physical_acceptance.application_sha256 !== FLASH_PLAN.applicationSha256) {
    fail("physical acceptance does not bind the reviewed application");
  }
  if (value.physical_acceptance.ota_journal_clear_sha256 !== FLASH_PLAN.otaJournalSha256) {
    fail("physical acceptance does not bind the erased OTA journals");
  }
  if (
    value.physical_acceptance.legacy_connectivity_clear_sha256
    !== FLASH_PLAN.legacyConnectivitySha256
  ) {
    fail("physical acceptance does not bind retirement of legacy connectivity secrets");
  }

  if (!Array.isArray(value.writes) || value.writes.length !== 7) {
    fail("manifest must contain exactly seven flash writes");
  }
  const [bootloader, partition, app0, app0Journal, app1, app1Journal, legacyConnectivity] = value.writes;
  exactWriteKeys(bootloader, "bootloader write");
  if (
    bootloader.role !== "bootloader"
    || bootloader.offset !== FLASH_PLAN.bootloaderOffset
    || bootloader.bytes !== FLASH_PLAN.bootloaderBytes
    || bootloader.sha256 !== FLASH_PLAN.bootloaderSha256
  ) {
    fail("bootloader write does not match the reviewed rollback-enabled Kitsu bootloader");
  }
  const bootloaderUrl = exactArtifactPath(
    bootloader.path,
    releaseId,
    "kitsu868-bootloader.bin",
    "bootloader write path",
  );

  exactWriteKeys(partition, "partition write");
  if (
    partition.role !== "partition_table"
    || partition.offset !== FLASH_PLAN.partitionOffset
    || partition.bytes !== FLASH_PLAN.partitionBytes
    || partition.sha256 !== FLASH_PLAN.partitionSha256
  ) {
    fail("partition write does not match the reviewed Kitsu 8 MiB layout");
  }
  const partitionUrl = exactArtifactPath(
    partition.path,
    releaseId,
    "kitsu868-partitions.bin",
    "partition write path",
  );

  exactWriteKeys(app0, "app0 write");
  if (
    app0.role !== "application_app0"
    || app0.offset !== FLASH_PLAN.app0Offset
    || app0.bytes !== FLASH_PLAN.applicationBytes
    || app0.sha256 !== FLASH_PLAN.applicationSha256
  ) {
    fail("app0 write does not match the reviewed Kitsu application");
  }
  const app0Url = exactArtifactPath(
    app0.path,
    releaseId,
    "kitsu868-app.bin",
    "app0 write path",
  );

  if (FLASH_PLAN.applicationBytes > FLASH_PLAN.applicationSlotBytes - FLASH_PLAN.otaJournalBytes) {
    fail("reviewed application overlaps its private OTA journal");
  }
  exactWriteKeys(app0Journal, "app0 journal write");
  if (
    app0Journal.role !== "ota_journal_app0_clear"
    || app0Journal.offset !== FLASH_PLAN.app0JournalOffset
    || app0Journal.bytes !== FLASH_PLAN.otaJournalBytes
    || app0Journal.sha256 !== FLASH_PLAN.otaJournalSha256
  ) {
    fail("app0 journal write is not the reviewed erased OTA journal");
  }
  const app0JournalUrl = exactArtifactPath(
    app0Journal.path,
    releaseId,
    "kitsu868-ota-journal-clear.bin",
    "app0 journal write path",
  );

  exactWriteKeys(app1, "app1 write");
  if (
    app1.role !== "application_app1"
    || app1.offset !== FLASH_PLAN.app1Offset
    || app1.bytes !== FLASH_PLAN.applicationBytes
    || app1.sha256 !== FLASH_PLAN.applicationSha256
    || app1.path !== app0.path
  ) {
    fail("app1 write must install the exact reviewed app0 application in the fixed app1 slot");
  }
  const app1Url = exactArtifactPath(
    app1.path,
    releaseId,
    "kitsu868-app.bin",
    "app1 write path",
  );

  exactWriteKeys(app1Journal, "app1 journal write");
  if (
    app1Journal.role !== "ota_journal_app1_clear"
    || app1Journal.offset !== FLASH_PLAN.app1JournalOffset
    || app1Journal.bytes !== FLASH_PLAN.otaJournalBytes
    || app1Journal.sha256 !== FLASH_PLAN.otaJournalSha256
    || app1Journal.path !== app0Journal.path
  ) {
    fail("app1 journal write must use the exact reviewed clear bytes in the fixed app1 journal");
  }
  const app1JournalUrl = exactArtifactPath(
    app1Journal.path,
    releaseId,
    "kitsu868-ota-journal-clear.bin",
    "app1 journal write path",
  );

  exactWriteKeys(legacyConnectivity, "legacy connectivity retirement write");
  if (
    legacyConnectivity.role !== "legacy_connectivity_clear"
    || legacyConnectivity.offset !== FLASH_PLAN.legacyConnectivityOffset
    || legacyConnectivity.bytes !== FLASH_PLAN.legacyConnectivityBytes
    || legacyConnectivity.sha256 !== FLASH_PLAN.legacyConnectivitySha256
  ) {
    fail("legacy connectivity retirement does not exactly clear the isolated retired partition");
  }
  const legacyConnectivityUrl = exactArtifactPath(
    legacyConnectivity.path,
    releaseId,
    "kitsu868-legacy-connectivity-clear.bin",
    "legacy connectivity retirement write path",
  );

  exactKeys(
    value.operations,
    ["erase_flash", "retire_legacy_connectivity", "retire_legacy_lan_action_state"],
    "manifest.operations",
  );
  exactBoolean(value.operations.erase_flash, false, "manifest.operations.erase_flash");
  exactBoolean(
    value.operations.retire_legacy_connectivity,
    true,
    "manifest.operations.retire_legacy_connectivity",
  );
  exactBoolean(
    value.operations.retire_legacy_lan_action_state,
    true,
    "manifest.operations.retire_legacy_lan_action_state",
  );
  exactKeys(
    value.preserves,
    [
      "ota_data",
      "companion_state",
      "companion_pack",
      "controller_store",
      "meshcore_state",
      "coredump",
    ],
    "manifest.preserves",
  );
  for (const key of Object.keys(value.preserves)) {
    exactBoolean(value.preserves[key], true, `manifest.preserves.${key}`);
  }
  exactKeys(
    value.capabilities,
    ["full_chip_erase_available", "rollback_bootloader", "stock_meshcore_restore_available"],
    "manifest.capabilities",
  );
  exactBoolean(value.capabilities.full_chip_erase_available, true, "full-chip recovery capability");
  exactBoolean(value.capabilities.rollback_bootloader, true, "rollback bootloader capability");
  exactBoolean(value.capabilities.stock_meshcore_restore_available, true, "stock MeshCore recovery capability");
  exactKeys(
    value.security,
    ["mode", "efuse_writes", "secure_boot", "flash_encryption"],
    "manifest.security",
  );
  if (value.security.mode !== "reflashable") fail("manifest security mode is not reflashable");
  exactBoolean(value.security.efuse_writes, false, "manifest.security.efuse_writes");
  exactBoolean(value.security.secure_boot, false, "manifest.security.secure_boot");
  exactBoolean(value.security.flash_encryption, false, "manifest.security.flash_encryption");
  exactKeys(value.flash, ["flash_mode", "flash_frequency", "flash_size", "readback_verify"], "manifest.flash");
  if (
    value.flash.flash_mode !== "dio"
    || value.flash.flash_frequency !== "80m"
    || value.flash.flash_size !== "8MB"
  ) {
    fail("manifest flash parameters are not the reviewed Heltec V3 settings");
  }
  exactBoolean(value.flash.readback_verify, true, "manifest.flash.readback_verify");

  return Object.freeze({
    manifest: value,
    artifacts: Object.freeze([
      Object.freeze({ ...bootloader, url: bootloaderUrl }),
      Object.freeze({ ...partition, url: partitionUrl }),
      Object.freeze({ ...app0, url: app0Url }),
      Object.freeze({ ...app0Journal, url: app0JournalUrl }),
      Object.freeze({ ...app1, url: app1Url }),
      Object.freeze({ ...app1Journal, url: app1JournalUrl }),
      Object.freeze({ ...legacyConnectivity, url: legacyConnectivityUrl }),
    ]),
  });
}

export async function sha256Hex(bytes) {
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  return [...digest].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

export function pemToSpki(pem) {
  if (typeof pem !== "string") fail("update authority is not text");
  const match = pem.trim().match(
    /^-----BEGIN PUBLIC KEY-----\s+([A-Za-z0-9+/=\s]+)\s+-----END PUBLIC KEY-----$/,
  );
  if (!match) fail("update authority is not an SPKI public key");
  const encoded = match[1].replace(/\s+/g, "");
  let decoded;
  try {
    decoded = atob(encoded);
  } catch {
    fail("update authority contains invalid base64");
  }
  const der = Uint8Array.from(decoded, (character) => character.charCodeAt(0));
  if (der.byteLength < 32 || der.byteLength > 256) fail("update authority has an invalid size");
  return der;
}

/** Verify raw detached Ed25519 bytes without reserializing the manifest. */
export async function verifyDetachedManifest(manifestBytes, signatureBytes, publicKeyPem) {
  if (!(manifestBytes instanceof Uint8Array) || manifestBytes.byteLength < 2 || manifestBytes.byteLength > 65536) {
    fail("release manifest has an invalid size");
  }
  if (!(signatureBytes instanceof Uint8Array) || signatureBytes.byteLength !== 64) {
    fail("release signature must be exactly 64 bytes");
  }
  const spki = pemToSpki(publicKeyPem);
  if (await sha256Hex(spki) !== UPDATE_AUTHORITY_SPKI_SHA256) {
    fail("release public key is not the installed Kitsu update authority");
  }
  const key = await crypto.subtle.importKey(
    "spki",
    spki,
    { name: "Ed25519" },
    false,
    ["verify"],
  );
  const valid = await crypto.subtle.verify(
    { name: "Ed25519" },
    key,
    signatureBytes,
    manifestBytes,
  );
  if (!valid) fail("release manifest signature is invalid");
}

export async function verifyReleaseEnvelope(manifestBytes, signatureBytes, publicKeyPem) {
  await verifyDetachedManifest(manifestBytes, signatureBytes, publicKeyPem);
  let value;
  try {
    const text = new TextDecoder("utf-8", { fatal: true }).decode(manifestBytes);
    value = JSON.parse(text);
  } catch {
    fail("signed release manifest is not valid UTF-8 JSON");
  }
  return validateReleaseManifest(value);
}

async function requiredResponse(url, label, maximumBytes) {
  const response = await fetch(url, {
    cache: "no-store",
    credentials: "omit",
    mode: "cors",
    referrerPolicy: "no-referrer",
    signal: AbortSignal.timeout(15000),
  });
  if (!response.ok) fail(`${label} returned HTTP ${response.status}`);
  const declaredLength = Number(response.headers.get("content-length"));
  if (Number.isFinite(declaredLength) && declaredLength > maximumBytes) {
    fail(`${label} exceeds its size limit`);
  }
  const bytes = new Uint8Array(await response.arrayBuffer());
  if (bytes.byteLength > maximumBytes) fail(`${label} exceeds its size limit`);
  return bytes;
}

export async function fetchVerifiedRelease() {
  const [manifestBytes, signatureBytes, publicKeyBytes] = await Promise.all([
    requiredResponse(MANIFEST_URL, "release manifest", 65536),
    requiredResponse(SIGNATURE_URL, "release signature", 64),
    requiredResponse(PUBLIC_KEY_URL, "release public key", 4096),
  ]);
  const publicKeyPem = new TextDecoder("utf-8", { fatal: true }).decode(publicKeyBytes);
  const release = await verifyReleaseEnvelope(manifestBytes, signatureBytes, publicKeyPem);
  const downloads = new Map();
  const artifacts = await Promise.all(release.artifacts.map(async (record) => {
    const cacheKey = `${record.url}\u0000${record.bytes}\u0000${record.sha256}`;
    if (!downloads.has(cacheKey)) {
      downloads.set(cacheKey, requiredResponse(record.url, `${record.role} artifact`, record.bytes));
    }
    const bytes = await downloads.get(cacheKey);
    if (bytes.byteLength !== record.bytes) fail(`${record.role} artifact size does not match its signed manifest`);
    if (await sha256Hex(bytes) !== record.sha256) {
      fail(`${record.role} artifact hash does not match its signed manifest`);
    }
    return Object.freeze({ record, bytes });
  }));
  return Object.freeze({ ...release, artifacts: Object.freeze(artifacts) });
}

export async function reverifyArtifacts(release) {
  if (!release || !Array.isArray(release.artifacts) || release.artifacts.length !== 7) {
    fail("verified release is not loaded");
  }
  for (const artifact of release.artifacts) {
    if (!(artifact.bytes instanceof Uint8Array) || artifact.bytes.byteLength !== artifact.record.bytes) {
      fail(`${artifact.record.role} artifact changed in memory`);
    }
    if (await sha256Hex(artifact.bytes) !== artifact.record.sha256) {
      fail(`${artifact.record.role} artifact changed after verification`);
    }
  }
}
