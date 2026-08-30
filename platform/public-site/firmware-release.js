const MAGIC = ascii("KITSUFW1");
const HEADER_BYTES = 20;
const MAX_MANIFEST_BYTES = 1024;
const SIGNATURE_BYTES = 64;
const PARTITION_BYTES = 0x300000;
const JOURNAL_BYTES = 0x1000;
const MAX_IMAGE_BYTES = PARTITION_BYTES - JOURNAL_BYTES;
const CHUNK_BYTES = 4096;
const MANIFEST_SCHEMA = "kitsu.ble-firmware.v1";
const DEVICE_CLASS = "heltec-wifi-lora-32-v3-esp32s3-8mb";
const IMAGE_FORMAT = "esp32s3-app";
const REQUIRED_FIRMWARE_VERSION = "0.20.5";
const IDENTITY_MAGIC = ascii("KITSU-ID1|");
const IDENTITY_MAX_BYTES = 384;
const IDENTITY_SCHEMA = 1;
const IDENTITY_DEVICE_CLASS = "heltec-v3.2";
const IDENTITY_LAYOUT = "kitsu-8m-dual-ota-3m-v1";
const IDENTITY_GEOMETRY = Object.freeze({
  flashBytes: 0x800000,
  nvsOffset: 0x009000,
  nvsBytes: 0x040000,
  otaDataOffset: 0x049000,
  otaDataBytes: 0x002000,
  app0Offset: 0x050000,
  app1Offset: 0x350000,
  partitionBytes: PARTITION_BYTES,
  journalBytes: JOURNAL_BYTES,
  maximumImageBytes: MAX_IMAGE_BYTES,
  spiffsOffset: 0x670000,
  spiffsBytes: 0x140000,
  connectivityOffset: 0x7b0000,
  connectivityBytes: 0x040000,
  coredumpOffset: 0x7f0000,
  coredumpBytes: 0x010000,
});
export const updateAuthorityRawBase64Url = "JAAR8Unpz7n7h_q02cpFc8HH_7OHF3ZYAAXsQa7lE4I";
const AUTHORITY_RAW = base64UrlBytes(updateAuthorityRawBase64Url);
const DIGEST_PATTERN = /^[0-9a-f]{64}$/;
const RELEASE_ID_PATTERN = /^[0-9A-Za-z][0-9A-Za-z._-]{0,63}$/;
const NUMERIC_IDENTIFIER = String.raw`(?:0|[1-9][0-9]*)`;
const NON_NUMERIC_IDENTIFIER = String.raw`(?:[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)`;
const PRERELEASE_IDENTIFIER = String.raw`(?:${NUMERIC_IDENTIFIER}|${NON_NUMERIC_IDENTIFIER})`;
const BUILD_IDENTIFIER = String.raw`(?:[0-9A-Za-z-]+)`;
const SEMVER_PATTERN = new RegExp(
  String.raw`^${NUMERIC_IDENTIFIER}\.${NUMERIC_IDENTIFIER}\.${NUMERIC_IDENTIFIER}` +
    String.raw`(?:-${PRERELEASE_IDENTIFIER}(?:\.${PRERELEASE_IDENTIFIER})*)?` +
    String.raw`(?:\+${BUILD_IDENTIFIER}(?:\.${BUILD_IDENTIFIER})*)?$`,
);
const MAXIMUM_SEMVER_CORE = 9_223_372_036_854_775_807n;

// Exact production-signed package accepted for the current migrated layout.
// The content-addressed path and all four remaining fields are independently
// enforced before verified bytes can be offered through a Blob URL.
export const publishedFirmwareRelease = Object.freeze({
  url: "/downloads/kitsu-firmware-0.20.5-9b8652be49f3fbe0084b5cd7f374939b39df710b2cc7cffbaff58d98bdf312c9.kitsu-fw",
  bytes: 1267730,
  sha256: "9b8652be49f3fbe0084b5cd7f374939b39df710b2cc7cffbaff58d98bdf312c9",
  releaseId: "kitsu-0.20.5-reflashable-1",
  firmwareVersion: "0.20.5",
});
let activeFirmwareObjectUrl = null;

function fail(code) {
  const error = new Error(code);
  error.code = code;
  throw error;
}

function ascii(value) {
  return Uint8Array.from(value, (character) => character.charCodeAt(0));
}

function base64UrlBytes(value) {
  const normalized = value.replaceAll("-", "+").replaceAll("_", "/");
  const padded = normalized + "=".repeat((4 - (normalized.length % 4)) % 4);
  if (typeof Buffer !== "undefined") return Uint8Array.from(Buffer.from(padded, "base64"));
  return Uint8Array.from(atob(padded), (character) => character.charCodeAt(0));
}

function equalBytes(left, right) {
  if (left.byteLength !== right.byteLength) return false;
  let difference = 0;
  for (let index = 0; index < left.byteLength; index += 1) {
    difference |= left[index] ^ right[index];
  }
  return difference === 0;
}

function hex(bytes) {
  return [...bytes].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

function validSemver(value) {
  if (typeof value !== "string" || value.length > 64 || !SEMVER_PATTERN.test(value)) {
    return false;
  }
  return value.split(/[+-]/, 1)[0].split(".")
    .every((identifier) => BigInt(identifier) <= MAXIMUM_SEMVER_CORE);
}

function readAscii(bytes, code) {
  if ([...bytes].some((byte) => byte > 0x7f)) fail(code);
  return String.fromCharCode(...bytes);
}

function readUint32BE(bytes, offset) {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(offset, false);
}

function readUint32LE(bytes, offset) {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(offset, true);
}

function readUint16BE(bytes, offset) {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint16(offset, false);
}

function readUint16LE(bytes, offset) {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint16(offset, true);
}

function findAll(bytes, needle) {
  const offsets = [];
  outer: for (let index = 0; index <= bytes.byteLength - needle.byteLength; index += 1) {
    for (let offset = 0; offset < needle.byteLength; offset += 1) {
      if (bytes[index + offset] !== needle[offset]) continue outer;
    }
    offsets.push(index);
  }
  return offsets;
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

async function sha256(bytes, cryptoObject) {
  if (!cryptoObject?.subtle) fail("crypto_unavailable");
  return new Uint8Array(await cryptoObject.subtle.digest("SHA-256", bytes));
}

export function validateFirmwareReleaseContract(contract) {
  if (!contract || typeof contract !== "object" || Array.isArray(contract)) {
    fail("release_unavailable");
  }
  const keys = ["url", "bytes", "sha256", "releaseId", "firmwareVersion"];
  if (Object.keys(contract).join("\0") !== keys.join("\0")) fail("release_contract_invalid");
  const pathMatch = typeof contract.url === "string" && contract.url.length <= 180
    ? contract.url.match(/^\/downloads\/[a-z0-9][a-z0-9._-]{0,96}-([0-9a-f]{64})\.kitsu-fw$/)
    : null;
  if (!pathMatch) {
    fail("release_path_invalid");
  }
  if (!DIGEST_PATTERN.test(contract.sha256) || pathMatch[1] !== contract.sha256) {
    fail("release_digest_invalid");
  }
  if (!Number.isSafeInteger(contract.bytes) ||
      contract.bytes < HEADER_BYTES + SIGNATURE_BYTES + 1 ||
      contract.bytes > HEADER_BYTES + MAX_MANIFEST_BYTES + SIGNATURE_BYTES + MAX_IMAGE_BYTES) {
    fail("release_bytes_invalid");
  }
  if (!RELEASE_ID_PATTERN.test(contract.releaseId) ||
      contract.firmwareVersion !== REQUIRED_FIRMWARE_VERSION) {
    fail("release_identity_invalid");
  }
  return contract;
}

function canonicalManifest(value) {
  return `{"schema":"${MANIFEST_SCHEMA}","release_id":"${value.release_id}",` +
    `"firmware_version":"${value.firmware_version}","device_class":"${DEVICE_CLASS}",` +
    `"image_format":"${IMAGE_FORMAT}","image_bytes":${value.image_bytes},` +
    `"image_sha256":"${value.image_sha256}","partition_bytes":${PARTITION_BYTES},` +
    `"chunk_bytes":${CHUNK_BYTES},"rollback":true}`;
}

export function parseCanonicalFirmwareManifest(bytes) {
  if (!(bytes instanceof Uint8Array) || bytes.byteLength < 1 ||
      bytes.byteLength > MAX_MANIFEST_BYTES) {
    fail("manifest_bytes_invalid");
  }
  const text = readAscii(bytes, "manifest_encoding_invalid");
  let value;
  try {
    value = JSON.parse(text);
  } catch {
    fail("manifest_json_invalid");
  }
  if (!value || typeof value !== "object" || Array.isArray(value)) fail("manifest_invalid");
  const keys = [
    "schema", "release_id", "firmware_version", "device_class", "image_format",
    "image_bytes", "image_sha256", "partition_bytes", "chunk_bytes", "rollback",
  ];
  if (Object.keys(value).join("\0") !== keys.join("\0") ||
      value.schema !== MANIFEST_SCHEMA || value.device_class !== DEVICE_CLASS ||
      value.image_format !== IMAGE_FORMAT || value.partition_bytes !== PARTITION_BYTES ||
      value.chunk_bytes !== CHUNK_BYTES || value.rollback !== true ||
      !RELEASE_ID_PATTERN.test(value.release_id) || !validSemver(value.firmware_version) ||
      value.firmware_version !== REQUIRED_FIRMWARE_VERSION ||
      !Number.isSafeInteger(value.image_bytes) || value.image_bytes < 1 ||
      value.image_bytes > MAX_IMAGE_BYTES || !DIGEST_PATTERN.test(value.image_sha256)) {
    fail("manifest_contract_invalid");
  }
  if (text !== canonicalManifest(value)) fail("manifest_canonical_form_invalid");
  return value;
}

export function parseFirmwarePackage(bytes) {
  if (!(bytes instanceof Uint8Array) ||
      bytes.byteLength < HEADER_BYTES + SIGNATURE_BYTES + 1) {
    fail("package_truncated");
  }
  if (!equalBytes(bytes.subarray(0, MAGIC.byteLength), MAGIC)) fail("package_magic_invalid");
  const manifestBytes = readUint32BE(bytes, 8);
  const signatureBytes = readUint16BE(bytes, 12);
  const flags = readUint16BE(bytes, 14);
  const imageBytes = readUint32BE(bytes, 16);
  if (manifestBytes < 1 || manifestBytes > MAX_MANIFEST_BYTES ||
      signatureBytes !== SIGNATURE_BYTES || flags !== 0 ||
      imageBytes < 1 || imageBytes > MAX_IMAGE_BYTES) {
    fail("package_header_invalid");
  }
  const manifestEnd = HEADER_BYTES + manifestBytes;
  const signatureEnd = manifestEnd + signatureBytes;
  const packageEnd = signatureEnd + imageBytes;
  if (packageEnd !== bytes.byteLength) fail("package_boundaries_invalid");
  return {
    manifest: bytes.subarray(HEADER_BYTES, manifestEnd),
    signature: bytes.subarray(manifestEnd, signatureEnd),
    image: bytes.subarray(signatureEnd),
  };
}

export function parseFirmwareIdentity(image) {
  if (!(image instanceof Uint8Array)) fail("image_invalid");
  const starts = findAll(image, IDENTITY_MAGIC);
  if (starts.length !== 1) fail("identity_count_invalid");
  const start = starts[0];
  const limit = Math.min(image.byteLength, start + IDENTITY_MAX_BYTES + 1);
  let end = -1;
  for (let index = start; index < limit; index += 1) {
    if (image[index] === 0) {
      end = index;
      break;
    }
  }
  if (end < 0) fail("identity_termination_invalid");
  const raw = image.subarray(start, end);
  const text = readAscii(raw, "identity_encoding_invalid");
  const match = text.match(
    /^KITSU-ID1\|schema=([0-9]+)\|length=([0-9]{4})\|version=([^|]+)\|device_class=([^|]+)\|layout=([^|]+)\|flash=([0-9a-f]{8})\|nvs=([0-9a-f]{8})\/([0-9a-f]{8})\|otadata=([0-9a-f]{8})\/([0-9a-f]{8})\|app0=([0-9a-f]{8})\|app1=([0-9a-f]{8})\|slot=([0-9a-f]{8})\|journal=([0-9a-f]{8})\|max=([0-9a-f]{8})\|spiffs=([0-9a-f]{8})\/([0-9a-f]{8})\|conn=([0-9a-f]{8})\/([0-9a-f]{8})\|coredump=([0-9a-f]{8})\/([0-9a-f]{8})\|crc32=([0-9a-f]{8})\|end$/,
  );
  if (!match) fail("identity_canonical_form_invalid");
  const identity = {
    schema: Number(match[1]),
    length: Number(match[2]),
    firmwareVersion: match[3],
    deviceClass: match[4],
    layout: match[5],
    flashBytes: Number.parseInt(match[6], 16),
    nvsOffset: Number.parseInt(match[7], 16),
    nvsBytes: Number.parseInt(match[8], 16),
    otaDataOffset: Number.parseInt(match[9], 16),
    otaDataBytes: Number.parseInt(match[10], 16),
    app0Offset: Number.parseInt(match[11], 16),
    app1Offset: Number.parseInt(match[12], 16),
    partitionBytes: Number.parseInt(match[13], 16),
    journalBytes: Number.parseInt(match[14], 16),
    maximumImageBytes: Number.parseInt(match[15], 16),
    spiffsOffset: Number.parseInt(match[16], 16),
    spiffsBytes: Number.parseInt(match[17], 16),
    connectivityOffset: Number.parseInt(match[18], 16),
    connectivityBytes: Number.parseInt(match[19], 16),
    coredumpOffset: Number.parseInt(match[20], 16),
    coredumpBytes: Number.parseInt(match[21], 16),
    identityCrc32: match[22],
    markerOffset: start,
    markerBytes: raw.byteLength + 1,
  };
  const crcBoundary = text.indexOf("|crc32=");
  if (identity.length !== identity.markerBytes ||
      crc32(raw.subarray(0, crcBoundary)).toString(16).padStart(8, "0") !==
        identity.identityCrc32) {
    fail("identity_integrity_invalid");
  }
  if (identity.schema !== IDENTITY_SCHEMA || !validSemver(identity.firmwareVersion) ||
      identity.deviceClass !== IDENTITY_DEVICE_CLASS || identity.layout !== IDENTITY_LAYOUT) {
    fail("identity_contract_invalid");
  }
  for (const [field, expected] of Object.entries(IDENTITY_GEOMETRY)) {
    if (identity[field] !== expected) fail("identity_geometry_invalid");
  }
  return identity;
}

export async function validateEsp32S3Application(image, cryptoObject = globalThis.crypto) {
  if (!(image instanceof Uint8Array) || image.byteLength < 24 ||
      image.byteLength > MAX_IMAGE_BYTES) {
    fail("image_bytes_invalid");
  }
  const segmentCount = image[1];
  if (image[0] !== 0xe9 || segmentCount < 1 || segmentCount > 16) {
    fail("image_header_invalid");
  }
  if (readUint16LE(image, 12) !== 0x0009) fail("image_chip_invalid");
  if (image[23] !== 1) fail("image_digest_flag_invalid");
  let cursor = 24;
  let checksum = 0xef;
  for (let segment = 0; segment < segmentCount; segment += 1) {
    if (cursor + 8 > image.byteLength) fail("image_segment_header_invalid");
    const loadAddress = readUint32LE(image, cursor);
    const dataBytes = readUint32LE(image, cursor + 4);
    cursor += 8;
    if (dataBytes < 1 || dataBytes % 4 !== 0 ||
        loadAddress + dataBytes > 0x1_0000_0000 ||
        dataBytes > image.byteLength - cursor) {
      fail("image_segment_range_invalid");
    }
    const end = cursor + dataBytes;
    for (let index = cursor; index < end; index += 1) checksum ^= image[index];
    cursor = end;
  }
  const checksumOffset = cursor + (15 - (cursor % 16));
  const digestOffset = checksumOffset + 1;
  if (digestOffset + 32 !== image.byteLength) fail("image_boundaries_invalid");
  if (image[checksumOffset] !== checksum) fail("image_checksum_invalid");
  const digest = await sha256(image.subarray(0, digestOffset), cryptoObject);
  if (!equalBytes(digest, image.subarray(digestOffset))) fail("image_digest_invalid");
  return { segmentCount, ...parseFirmwareIdentity(image) };
}

export async function verifyFirmwarePackage({
  bytes,
  contract,
  cryptoObject = globalThis.crypto,
  authorityRaw = AUTHORITY_RAW,
}) {
  const release = validateFirmwareReleaseContract(contract);
  if (!(bytes instanceof Uint8Array) || bytes.byteLength !== release.bytes) {
    fail("package_size_mismatch");
  }
  const packageDigest = hex(await sha256(bytes, cryptoObject));
  if (packageDigest !== release.sha256) fail("package_digest_mismatch");
  const parts = parseFirmwarePackage(bytes);
  const manifest = parseCanonicalFirmwareManifest(parts.manifest);
  if (manifest.release_id !== release.releaseId ||
      manifest.firmware_version !== release.firmwareVersion) {
    fail("package_release_mismatch");
  }
  let key;
  try {
    key = await cryptoObject.subtle.importKey(
      "raw",
      authorityRaw,
      { name: "Ed25519" },
      false,
      ["verify"],
    );
  } catch {
    fail("authority_import_failed");
  }
  let signatureValid = false;
  try {
    signatureValid = await cryptoObject.subtle.verify(
      { name: "Ed25519" },
      key,
      parts.signature,
      parts.manifest,
    );
  } catch {
    fail("signature_verification_failed");
  }
  if (!signatureValid) fail("signature_invalid");
  const imageDigest = hex(await sha256(parts.image, cryptoObject));
  if (parts.image.byteLength !== manifest.image_bytes ||
      imageDigest !== manifest.image_sha256) {
    fail("image_manifest_mismatch");
  }
  const identity = await validateEsp32S3Application(parts.image, cryptoObject);
  if (identity.firmwareVersion !== manifest.firmware_version ||
      identity.layout !== IDENTITY_LAYOUT || identity.deviceClass !== IDENTITY_DEVICE_CLASS) {
    fail("image_identity_mismatch");
  }
  return { contract: release, manifest, identity, packageDigest, imageDigest };
}

function firmwareElements(documentObject) {
  const elements = {
    status: documentObject?.querySelector?.("#firmware-status"),
    title: documentObject?.querySelector?.("#firmware-title"),
    detail: documentObject?.querySelector?.("#firmware-detail"),
    download: documentObject?.querySelector?.("#firmware-download"),
    digest: documentObject?.querySelector?.("#firmware-digest"),
  };
  return Object.values(elements).every(Boolean) ? elements : null;
}

function disableDownload(elements) {
  elements.download.removeAttribute("href");
  elements.download.removeAttribute("download");
  elements.download.setAttribute("aria-disabled", "true");
  elements.download.classList.add("disabled");
}

function revokeActiveFirmwareObjectUrl() {
  if (!activeFirmwareObjectUrl) return;
  activeFirmwareObjectUrl.urlApi.revokeObjectURL(activeFirmwareObjectUrl.href);
  activeFirmwareObjectUrl = null;
}

function handleFirmwarePageHide(event) {
  // A persisted pagehide enters the browser back/forward cache. Its DOM and
  // verified download control will be restored without rerunning this module,
  // so the Blob URL must remain valid until a real page teardown.
  if (!event?.persisted) revokeActiveFirmwareObjectUrl();
}

async function readExactResponseBytes(response, expectedBytes) {
  const contentLength = response.headers?.get?.("Content-Length");
  if (contentLength !== null && contentLength !== undefined &&
      (!/^[0-9]+$/.test(contentLength) || Number(contentLength) !== expectedBytes)) {
    fail("package_content_length_mismatch");
  }
  if (typeof response.body?.getReader === "function") {
    const output = new Uint8Array(expectedBytes);
    const reader = response.body.getReader();
    let offset = 0;
    try {
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        if (!(value instanceof Uint8Array) || value.byteLength > expectedBytes - offset) {
          fail("package_stream_size_mismatch");
        }
        output.set(value, offset);
        offset += value.byteLength;
      }
    } catch (error) {
      try {
        await reader.cancel();
      } catch {
        // The verification error remains authoritative if cancellation fails.
      }
      throw error;
    } finally {
      reader.releaseLock?.();
    }
    if (offset !== expectedBytes) fail("package_stream_size_mismatch");
    return output;
  }
  const buffer = await response.arrayBuffer();
  const bytes = new Uint8Array(buffer);
  if (bytes.byteLength !== expectedBytes) fail("package_size_mismatch");
  return bytes;
}

export async function initializeFirmwareRelease({
  documentObject = globalThis.document,
  fetchImpl = globalThis.fetch,
  cryptoObject = globalThis.crypto,
  contract = publishedFirmwareRelease,
  authorityRaw = AUTHORITY_RAW,
  urlApi = globalThis.URL,
  blobConstructor = globalThis.Blob,
  windowObject = globalThis.window,
} = {}) {
  const elements = firmwareElements(documentObject);
  if (!elements) return { state: "absent" };
  revokeActiveFirmwareObjectUrl();
  disableDownload(elements);
  if (contract === null) {
    elements.status.textContent = "Firmware package unavailable";
    elements.title.textContent = "Kitsu firmware 0.20.5";
    elements.detail.textContent = "No physically accepted signed firmware package is published yet.";
    elements.download.textContent = "Download unavailable";
    elements.digest.textContent = "This control remains disabled until one exact signed package passes release acceptance.";
    return { state: "unavailable" };
  }
  elements.status.textContent = "Checking signed firmware package";
  elements.title.textContent = `Kitsu firmware ${REQUIRED_FIRMWARE_VERSION}`;
  elements.detail.textContent = "Verifying the package signature, application image, and flash layout in this browser.";
  elements.download.textContent = "Verifying package...";
  try {
    const release = validateFirmwareReleaseContract(contract);
    if (typeof fetchImpl !== "function") fail("fetch_unavailable");
    const response = await fetchImpl(release.url, {
      cache: "no-store",
      credentials: "omit",
      redirect: "error",
      referrerPolicy: "no-referrer",
    });
    if (!response?.ok) fail("package_fetch_failed");
    const bytes = await readExactResponseBytes(response, release.bytes);
    const verified = await verifyFirmwarePackage({ bytes, contract: release, cryptoObject, authorityRaw });
    if (typeof blobConstructor !== "function" ||
        typeof urlApi?.createObjectURL !== "function" ||
        typeof urlApi?.revokeObjectURL !== "function") {
      fail("verified_download_url_unavailable");
    }
    const blob = new blobConstructor([bytes], { type: "application/octet-stream" });
    if (blob.size !== bytes.byteLength) fail("verified_download_blob_invalid");
    const objectUrl = urlApi.createObjectURL(blob);
    if (typeof objectUrl !== "string" || !objectUrl.startsWith("blob:")) {
      fail("verified_download_url_invalid");
    }
    activeFirmwareObjectUrl = { href: objectUrl, urlApi };
    windowObject?.addEventListener?.("pagehide", handleFirmwarePageHide);
    elements.status.textContent = "Signed firmware verified";
    elements.title.textContent = `Kitsu firmware ${verified.manifest.firmware_version}`;
    elements.detail.textContent = "For already migrated Heltec V3 boards. Install through Kitsu Android for signed A/B update and rollback handling.";
    elements.download.href = objectUrl;
    elements.download.download = release.url.split("/").at(-1);
    elements.download.removeAttribute("aria-disabled");
    elements.download.classList.remove("disabled");
    elements.download.textContent = `Download firmware ${verified.manifest.firmware_version}`;
    elements.digest.textContent = `Package SHA-256 ${verified.packageDigest}`;
    return { state: "verified", verified };
  } catch {
    revokeActiveFirmwareObjectUrl();
    disableDownload(elements);
    elements.status.textContent = "Firmware verification failed";
    elements.title.textContent = "Kitsu firmware unavailable";
    elements.detail.textContent = "The signed firmware package could not be verified. It is not offered, saved, or installed.";
    elements.download.textContent = "Download unavailable";
    elements.digest.textContent = "Use only a package that this page verifies and enables.";
    return { state: "failed" };
  }
}

if (typeof document !== "undefined") {
  void initializeFirmwareRelease();
}
