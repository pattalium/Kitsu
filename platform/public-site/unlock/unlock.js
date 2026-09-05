"use strict";

import {
  isPublishedPackEntry,
  publishedPackFor,
  rarityLabel,
} from "./catalog.js?sha256=4357b46ff6165929b776d2cb17f12bbd295be7dedd3bb2436ea4ee57d968f229";

export const VERIFY_MARKER = "KITSU_CODE_VERIFY_V1 ";
export const VERIFY_SCHEMA = "kitsu.code-verification.v1";
export const MAX_SERIAL_LINE_CHARS = 2_048;
export const MAX_SERIAL_RESPONSE_BYTES = 65_536;
export const DEFAULT_VERIFY_TIMEOUT_MS = 8_000;
export const REDEMPTION_SCHEMA = "kitsu.pet-pack-redemption.v1";
export const REDEMPTION_ENDPOINT = "https://api.k32.run/v1/pet-packs/redeem";
export const MAX_PACK_RESPONSE_BYTES = 2_097_152;

const K868_MAGIC = Object.freeze([0x4b, 0x38, 0x36, 0x38, 0x50, 0x4b, 0x31, 0x00]);
const K868 = Object.freeze({
  headerBytes: 64,
  clipBytes: 12,
  stepBytes: 4,
  slotBytes: 0x140000,
  versions: Object.freeze({
    1: Object.freeze({ width: 64, height: 64, frameBytes: 512 }),
    2: Object.freeze({ width: 64, height: 80, frameBytes: 640 }),
  }),
  frames: 48,
  clips: 12,
  steps: 48,
});

const CRC32_TABLE = Uint32Array.from({ length: 256 }, (_, value) => {
  let crc = value;
  for (let bit = 0; bit < 8; bit += 1) {
    crc = (crc & 1) === 1 ? (crc >>> 1) ^ 0xedb88320 : crc >>> 1;
  }
  return crc >>> 0;
});

const unlockCodeAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

const validRarities = new Set([
  "common",
  "uncommon",
  "rare",
  "very_rare",
  "epic",
  "legendary",
  "mythical",
]);

const invalidResponseKeys = Object.freeze([
  "deviceId",
  "requestId",
  "schema",
  "status",
]);

const validResponseKeys = Object.freeze([
  "boundDeviceId",
  "codeId",
  "deviceId",
  "packId",
  "rarity",
  "requestId",
  "schema",
  "status",
]);

export class UnlockProtocolError extends Error {
  constructor(code, message) {
    super(message);
    this.name = "UnlockProtocolError";
    this.code = code;
  }
}

function protocolError(code, message) {
  return new UnlockProtocolError(code, message);
}

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc = CRC32_TABLE[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function hexadecimal32(value) {
  return value.toString(16).padStart(8, "0").toUpperCase();
}

function isPrivateNoStore(value) {
  if (typeof value !== "string") return false;
  const directives = new Set(value.toLowerCase().split(",").map((part) => part.trim()));
  return directives.has("private") && directives.has("no-store");
}

async function sha256Hex(bytes, cryptoProvider = globalThis.crypto) {
  if (!cryptoProvider?.subtle || typeof cryptoProvider.subtle.digest !== "function") {
    throw protocolError("integrity_unavailable", "This browser cannot verify the downloaded pack.");
  }
  const digest = await cryptoProvider.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(digest)]
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join("");
}

async function boundedResponseBytes(response, expectedBytes) {
  const declaredLength = response.headers.get("content-length");
  if (declaredLength !== null) {
    if (!/^(0|[1-9][0-9]*)$/.test(declaredLength)) {
      throw protocolError("invalid_download", "The pack response declared an invalid size.");
    }
    const parsedLength = Number(declaredLength);
    if (
      !Number.isSafeInteger(parsedLength)
      || parsedLength > MAX_PACK_RESPONSE_BYTES
      || parsedLength !== expectedBytes
    ) {
      throw protocolError("invalid_download", "The pack response size does not match its catalog record.");
    }
  }

  if (response.body && typeof response.body.getReader === "function") {
    const reader = response.body.getReader();
    const chunks = [];
    let total = 0;
    try {
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        if (!(value instanceof Uint8Array)) {
          throw protocolError("invalid_download", "The pack response contained invalid bytes.");
        }
        total += value.byteLength;
        if (total > MAX_PACK_RESPONSE_BYTES || total > expectedBytes) {
          await Promise.resolve(reader.cancel()).catch(() => {});
          throw protocolError("invalid_download", "The pack response exceeded its allowed size.");
        }
        chunks.push(value.slice());
      }
    } finally {
      if (typeof reader.releaseLock === "function") reader.releaseLock();
    }
    if (total !== expectedBytes) {
      throw protocolError("invalid_download", "The pack response size does not match its catalog record.");
    }
    const bytes = new Uint8Array(total);
    let offset = 0;
    for (const chunk of chunks) {
      bytes.set(chunk, offset);
      offset += chunk.byteLength;
    }
    return bytes;
  }

  const buffer = await response.arrayBuffer();
  if (!(buffer instanceof ArrayBuffer)) {
    throw protocolError("invalid_download", "The pack response could not be read safely.");
  }
  if (buffer.byteLength > MAX_PACK_RESPONSE_BYTES || buffer.byteLength !== expectedBytes) {
    throw protocolError("invalid_download", "The pack response size does not match its catalog record.");
  }
  return new Uint8Array(buffer).slice();
}

export function validateK868Pack(bytes, expectedPackId = null) {
  if (!(bytes instanceof Uint8Array)) {
    throw protocolError("invalid_download", "The downloaded pack is not a byte array.");
  }
  if (bytes.byteLength < K868.headerBytes || bytes.byteLength > K868.slotBytes) {
    throw protocolError("invalid_download", "The downloaded pack has an invalid size.");
  }
  if (K868_MAGIC.some((value, index) => bytes[index] !== value)) {
    throw protocolError("invalid_download", "The downloaded file is not a K868PK1 pack.");
  }

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const version = view.getUint16(0x08, true);
  const headerBytes = view.getUint16(0x0a, true);
  const totalBytes = view.getUint32(0x0c, true);
  const payloadCrc = view.getUint32(0x10, true);
  const headerCrc = view.getUint32(0x14, true);
  const packId = hexadecimal32(view.getUint32(0x18, true));
  const revision = view.getUint32(0x1c, true);
  const width = view.getUint16(0x20, true);
  const height = view.getUint16(0x22, true);
  const frameCount = view.getUint16(0x24, true);
  const clipCount = view.getUint16(0x26, true);
  const stepCount = view.getUint32(0x28, true);
  const flags = view.getUint32(0x2c, true);
  const frameFormat = K868.versions[version];

  if (
    !frameFormat
    || headerBytes !== K868.headerBytes
    || totalBytes !== bytes.byteLength
    || packId === "00000000"
    || revision === 0
    || width !== frameFormat?.width
    || height !== frameFormat?.height
    || frameCount !== K868.frames
    || clipCount !== K868.clips
    || stepCount !== K868.steps
    || flags !== 0
  ) {
    throw protocolError("invalid_download", "The downloaded pack has an unsupported K868PK1 layout.");
  }
  const normalizedExpectedId = expectedPackId === null ? null : normalizedPackId(expectedPackId);
  if (normalizedExpectedId === null && expectedPackId !== null) {
    throw protocolError("invalid_download", "The expected pack identity is invalid.");
  }
  if (normalizedExpectedId !== null && packId !== normalizedExpectedId) {
    throw protocolError("invalid_download", "The downloaded pack identity does not match the verified code.");
  }
  const expectedTotal = K868.headerBytes
    + clipCount * K868.clipBytes
    + stepCount * K868.stepBytes
    + frameCount * frameFormat.frameBytes;
  if (expectedTotal !== bytes.byteLength || crc32(bytes.subarray(K868.headerBytes)) !== payloadCrc) {
    throw protocolError("invalid_download", "The downloaded pack failed its payload CRC check.");
  }
  const headerForCrc = bytes.slice(0x08, K868.headerBytes);
  headerForCrc.fill(0, 0x14 - 0x08, 0x18 - 0x08);
  if (crc32(headerForCrc) !== headerCrc) {
    throw protocolError("invalid_download", "The downloaded pack failed its header CRC check.");
  }

  const displayName = bytes.subarray(0x30, K868.headerBytes);
  const terminator = displayName.indexOf(0);
  const nameBytes = terminator < 0 ? displayName : displayName.subarray(0, terminator);
  if (
    nameBytes.byteLength === 0
    || nameBytes.some((value) => value < 0x20 || value > 0x7e)
    || (terminator >= 0 && displayName.subarray(terminator).some((value) => value !== 0))
  ) {
    throw protocolError("invalid_download", "The downloaded pack has an invalid display name.");
  }

  const clipsOffset = K868.headerBytes;
  const stepsOffset = clipsOffset + clipCount * K868.clipBytes;
  let hasBaseIdle = false;
  for (let index = 0; index < clipCount; index += 1) {
    const offset = clipsOffset + index * K868.clipBytes;
    const role = view.getUint8(offset);
    const variant = view.getUint8(offset + 1);
    const mode = view.getUint8(offset + 2);
    const weight = view.getUint8(offset + 3);
    const firstStep = view.getUint32(offset + 4, true);
    const count = view.getUint16(offset + 8, true);
    const reserved = view.getUint16(offset + 10, true);
    if (
      role > 11
      || mode > 3
      || weight === 0
      || count === 0
      || count > 256
      || firstStep + count > stepCount
      || (mode === 0 && count !== 1)
      || reserved !== 0
    ) {
      throw protocolError("invalid_download", "The downloaded pack has an invalid animation clip.");
    }
    hasBaseIdle ||= role === 0 && variant === 0;
  }
  if (!hasBaseIdle) {
    throw protocolError("invalid_download", "The downloaded pack has no base Idle animation.");
  }

  for (let index = 0; index < stepCount; index += 1) {
    const offset = stepsOffset + index * K868.stepBytes;
    const frameIndex = view.getUint16(offset, true);
    const durationMs = view.getUint16(offset + 2, true);
    if (frameIndex >= frameCount || durationMs < 100 || durationMs > 60_000) {
      throw protocolError("invalid_download", "The downloaded pack has an invalid animation step.");
    }
  }
  return Object.freeze({
    bytes: totalBytes,
    packId,
    revision,
    version,
    width,
    height,
  });
}

export function buildRedemptionRequest(code, verification) {
  const normalizedCode = normalizeUnlockCode(code);
  if (!normalizedCode || verification?.status !== "valid") {
    throw protocolError("invalid_redemption", "A valid code verification is required before download.");
  }
  const requestId = verification.requestId;
  if (
    !validRequestId(requestId)
    || !normalizedHardwareId(verification.deviceId)
    || verification.boundDeviceId !== verification.deviceId
    || !normalizedCodeId(verification.codeId)
    || !normalizedPackId(verification.packId)
    || !validRarities.has(verification.rarity)
  ) {
    throw protocolError("invalid_redemption", "The code verification record is incomplete.");
  }
  return Object.freeze({
    schema: REDEMPTION_SCHEMA,
    code: normalizedCode,
    verification: Object.freeze({
      schema: VERIFY_SCHEMA,
      boundDeviceId: verification.boundDeviceId,
      codeId: verification.codeId,
      deviceId: verification.deviceId,
      packId: verification.packId,
      rarity: verification.rarity,
      requestId,
      status: "valid",
    }),
  });
}

function redemptionFailure(status) {
  if (status === 403) {
    return protocolError("redemption_denied", "The K32 service did not accept this code and device proof.");
  }
  if (status === 404) {
    return protocolError("pack_unpublished", "This valid pack has not been published yet.");
  }
  if (status === 429) {
    return protocolError("redemption_rate_limited", "Too many attempts were made. Wait a moment and try again.");
  }
  if (status >= 500) {
    return protocolError("redemption_unavailable", "Pack delivery is temporarily unavailable. Try again later.");
  }
  return protocolError("redemption_failed", "The pack could not be delivered.");
}

export async function redeemPublishedPack(
  fetchImpl,
  code,
  verification,
  publishedPack,
  cryptoProvider = globalThis.crypto,
) {
  if (typeof fetchImpl !== "function" || !isPublishedPackEntry(publishedPack)) {
    throw protocolError("invalid_redemption", "The requested pack is not in the published catalog.");
  }
  if (verification?.packId !== publishedPack.packId || verification?.rarity !== publishedPack.rarity) {
    throw protocolError("invalid_redemption", "The verified result does not match the published pack.");
  }
  const body = buildRedemptionRequest(code, verification);
  let response;
  try {
    response = await fetchImpl(REDEMPTION_ENDPOINT, {
      method: "POST",
      headers: Object.freeze({
        Accept: "application/octet-stream",
        "Content-Type": "application/json",
      }),
      body: JSON.stringify(body),
      cache: "no-store",
      credentials: "omit",
      mode: "cors",
      redirect: "error",
      referrerPolicy: "no-referrer",
    });
  } catch {
    throw protocolError("redemption_unavailable", "The K32 pack service could not be reached.");
  }
  if (!response || typeof response.status !== "number" || !response.ok) {
    throw redemptionFailure(response?.status ?? 503);
  }
  if (!response.headers || typeof response.headers.get !== "function") {
    throw protocolError("invalid_download", "The pack response did not include verifiable headers.");
  }

  const contentType = response.headers.get("content-type")?.toLowerCase().trim();
  const disposition = response.headers.get("content-disposition");
  const responsePackId = response.headers.get("x-kitsu-pack-id")?.toUpperCase();
  const responseSha256 = response.headers.get("x-kitsu-pack-sha256")?.toLowerCase();
  if (
    contentType !== "application/octet-stream"
    || disposition !== `attachment; filename="kitsu-${publishedPack.slug}.k868"`
    || responsePackId !== publishedPack.packId
    || responseSha256 !== publishedPack.sha256
    || !isPrivateNoStore(response.headers.get("cache-control"))
  ) {
    throw protocolError("invalid_download", "The pack response failed its delivery-policy checks.");
  }

  let bytes;
  try {
    bytes = await boundedResponseBytes(response, publishedPack.bytes);
    validateK868Pack(bytes, publishedPack.packId);
    if (await sha256Hex(bytes, cryptoProvider) !== publishedPack.sha256) {
      throw protocolError("invalid_download", "The downloaded pack failed its SHA-256 check.");
    }
  } catch (error) {
    if (error instanceof UnlockProtocolError) throw error;
    throw protocolError("invalid_download", "The pack response could not be verified safely.");
  }
  return Object.freeze({
    bytes,
    filename: `kitsu-${publishedPack.slug}.k868`,
    mimeType: "application/octet-stream",
  });
}

function hasExactKeys(value, expected) {
  const actual = Object.keys(value).sort();
  return actual.length === expected.length
    && actual.every((key, index) => key === expected[index]);
}

function validRequestId(value) {
  return typeof value === "string" && /^[a-f0-9]{32}$/.test(value);
}

function normalizedHardwareId(value) {
  if (typeof value !== "string") return null;
  return /^KT[A-F0-9]{4}$/.test(value) ? value : null;
}

function normalizedPackId(value) {
  return typeof value === "string" && /^[A-F0-9]{8}$/.test(value)
    ? value
    : null;
}

function normalizedCodeId(value) {
  return typeof value === "string" && /^[A-F0-9]{8}$/.test(value)
    ? value
    : null;
}

export function normalizeUnlockCode(value) {
  if (typeof value !== "string") return null;
  const candidate = value.normalize("NFKC").trim();
  if (!candidate || /[^\x20-\x7e]/.test(candidate)) return null;
  if (/[^A-Za-z0-9 -]/.test(candidate)) return null;
  const compact = candidate.replace(/[ -]+/g, "").toUpperCase();
  const characters = compact.startsWith("K8") ? compact.slice(2) : compact;
  if (characters.length !== 15) return null;
  if (![...characters].every((character) => unlockCodeAlphabet.includes(character))) return null;
  return `K8-${characters.slice(0, 5)}-${characters.slice(5, 10)}-${characters.slice(10)}`;
}

export function unlockCodeFromFragment(hash) {
  if (typeof hash !== "string" || hash.length > 86) return null;
  const match = /^#code=([A-Z0-9-]{8,80})$/.exec(hash);
  if (!match || !normalizeUnlockCode(match[1])) return null;
  return match[1];
}

export function createRequestId(cryptoProvider = globalThis.crypto) {
  if (!cryptoProvider || typeof cryptoProvider.getRandomValues !== "function") {
    throw protocolError("entropy_unavailable", "Secure request entropy is unavailable.");
  }
  const bytes = new Uint8Array(16);
  cryptoProvider.getRandomValues(bytes);
  return [...bytes].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

export function buildVerificationCommand(code, requestId) {
  const normalizedCode = normalizeUnlockCode(code);
  if (!normalizedCode) {
    throw protocolError("invalid_code", "The unlock code has an invalid format.");
  }
  if (!validRequestId(requestId)) {
    throw protocolError("invalid_request", "The verification request ID is invalid.");
  }
  return `codes verify ${normalizedCode} ${requestId}\n`;
}

export function parseVerificationLine(line, expectedRequestId) {
  if (!validRequestId(expectedRequestId)) {
    throw protocolError("invalid_request", "The expected request ID is invalid.");
  }
  if (typeof line !== "string") return null;

  const candidate = line.endsWith("\r") ? line.slice(0, -1) : line;
  if (!candidate.startsWith(VERIFY_MARKER)) return null;
  if (candidate.length > MAX_SERIAL_LINE_CHARS) {
    throw protocolError("response_too_large", "The verification response exceeded its size limit.");
  }

  const payload = candidate.slice(VERIFY_MARKER.length);
  let record;
  try {
    record = JSON.parse(payload);
  } catch {
    throw protocolError("malformed_response", "The Kitsu returned malformed verification data.");
  }

  if (!record || typeof record !== "object" || Array.isArray(record)) {
    throw protocolError("malformed_response", "The Kitsu returned the wrong verification shape.");
  }
  if (record.schema !== VERIFY_SCHEMA) {
    throw protocolError("unsupported_schema", "The Kitsu returned an unsupported verification schema.");
  }
  if (record.requestId !== expectedRequestId) {
    throw protocolError("request_mismatch", "The Kitsu response does not match this request.");
  }
  if (!normalizedHardwareId(record.deviceId)) {
    throw protocolError("invalid_device", "The Kitsu returned an invalid hardware identity.");
  }

  if (record.status === "invalid") {
    if (!hasExactKeys(record, invalidResponseKeys)) {
      throw protocolError("malformed_response", "The rejected response has unexpected fields.");
    }
    return Object.freeze({
      deviceId: record.deviceId,
      requestId: record.requestId,
      status: "invalid",
    });
  }

  if (record.status !== "valid" || !hasExactKeys(record, validResponseKeys)) {
    throw protocolError("malformed_response", "The accepted response has unexpected fields.");
  }
  if (!normalizedHardwareId(record.boundDeviceId)) {
    throw protocolError("invalid_binding", "The Kitsu returned an invalid hardware binding.");
  }
  if (record.deviceId !== record.boundDeviceId) {
    throw protocolError("device_mismatch", "The code belongs to different hardware.");
  }
  if (!normalizedCodeId(record.codeId)) {
    throw protocolError("invalid_code_id", "The Kitsu returned an invalid code identity.");
  }
  if (!normalizedPackId(record.packId)) {
    throw protocolError("invalid_pack_id", "The Kitsu returned an invalid pack identity.");
  }
  if (!validRarities.has(record.rarity)) {
    throw protocolError("invalid_rarity", "The Kitsu returned an invalid rarity.");
  }

  return Object.freeze({
    boundDeviceId: record.boundDeviceId,
    codeId: record.codeId,
    deviceId: record.deviceId,
    packId: record.packId,
    rarity: record.rarity,
    requestId: record.requestId,
    status: "valid",
  });
}

export function supportsWebSerial(navigatorLike, secureContext = true) {
  return Boolean(
    secureContext
    && navigatorLike
    && navigatorLike.serial
    && typeof navigatorLike.serial.requestPort === "function",
  );
}

export async function readVerificationFromPort(
  port,
  expectedRequestId,
  timeoutMs = DEFAULT_VERIFY_TIMEOUT_MS,
) {
  if (!port?.readable || typeof port.readable.getReader !== "function") {
    throw protocolError("disconnected", "The Kitsu serial connection is not readable.");
  }
  if (!Number.isSafeInteger(timeoutMs) || timeoutMs < 250 || timeoutMs > 30_000) {
    throw protocolError("invalid_timeout", "The verification timeout is outside its allowed range.");
  }

  const reader = port.readable.getReader();
  const decoder = new TextDecoder("utf-8", { fatal: true });
  let buffer = "";
  let receivedBytes = 0;
  let timedOut = false;
  const timeout = setTimeout(() => {
    timedOut = true;
    Promise.resolve(reader.cancel()).catch(() => {});
  }, timeoutMs);

  try {
    while (true) {
      const { done, value } = await reader.read();
      if (timedOut) {
        throw protocolError("verification_timeout", "The Kitsu did not answer in time.");
      }
      if (done) {
        throw protocolError("disconnected", "The Kitsu disconnected before verification completed.");
      }
      if (!(value instanceof Uint8Array)) {
        throw protocolError("malformed_serial_data", "The Kitsu returned invalid serial bytes.");
      }

      receivedBytes += value.byteLength;
      if (receivedBytes > MAX_SERIAL_RESPONSE_BYTES) {
        throw protocolError("response_too_large", "The serial response exceeded its total size limit.");
      }

      try {
        buffer += decoder.decode(value, { stream: true });
      } catch {
        throw protocolError("malformed_serial_data", "The Kitsu returned invalid UTF-8 serial data.");
      }

      let newline = buffer.indexOf("\n");
      while (newline !== -1) {
        const line = buffer.slice(0, newline);
        buffer = buffer.slice(newline + 1);
        const verification = parseVerificationLine(line, expectedRequestId);
        if (verification) return verification;
        newline = buffer.indexOf("\n");
      }

      if (buffer.length > MAX_SERIAL_LINE_CHARS) {
        throw protocolError("response_too_large", "A serial line exceeded its size limit.");
      }
    }
  } finally {
    clearTimeout(timeout);
    reader.releaseLock();
  }
}

export async function verifyCodeWithPort(port, code, requestId) {
  if (!port?.writable || typeof port.writable.getWriter !== "function") {
    throw protocolError("disconnected", "The Kitsu serial connection is not writable.");
  }

  const writer = port.writable.getWriter();
  try {
    await writer.write(new TextEncoder().encode(buildVerificationCommand(code, requestId)));
  } catch {
    throw protocolError("disconnected", "The verification request could not reach the Kitsu.");
  } finally {
    writer.releaseLock();
  }
  return readVerificationFromPort(port, requestId);
}

function initializeUnlockPage(windowObject, documentObject) {
  const url = new URL(windowObject.location.href);
  const hasCodeFragment = url.hash.startsWith("#code=");
  const providedCode = unlockCodeFromFragment(url.hash);
  const hasLegacyQueryCode = url.searchParams.has("code");
  if (hasCodeFragment || hasLegacyQueryCode) {
    url.hash = "";
    url.searchParams.delete("code");
    windowObject.history.replaceState(null, "", `${url.pathname}${url.search}`);
  }

  const byId = (id) => {
    const element = documentObject.getElementById(id);
    if (!element) throw new Error(`Missing unlock-page element: ${id}`);
    return element;
  };

  const connectButton = byId("connect-kitsu");
  const deviceTitle = byId("device-title");
  const deviceDetail = byId("device-detail");
  const form = byId("unlock-form");
  const input = byId("unlock-code");
  const inputError = byId("unlock-code-error");
  const verifyButton = byId("verify-code");
  const resultRegion = byId("unlock-status");
  const resultPanel = byId("unlock-result");
  const packResult = byId("pack-result");
  const packTitle = byId("pack-title");
  const packDetail = byId("pack-detail");
  const packPortrait = byId("pack-portrait");
  const packDownload = byId("pack-download");
  const packInstall = byId("pack-install");
  const packIntegrity = byId("pack-integrity");

  let currentPort = null;
  let connected = false;
  let busy = false;
  let currentDownloadUrl = null;

  function revokeDownloadUrl() {
    if (currentDownloadUrl !== null) {
      windowObject.URL.revokeObjectURL(currentDownloadUrl);
      currentDownloadUrl = null;
    }
  }

  function resetPackResult() {
    revokeDownloadUrl();
    packResult.hidden = true;
    packTitle.textContent = "";
    packDetail.textContent = "";
    packIntegrity.textContent = "";
    packPortrait.hidden = true;
    packPortrait.removeAttribute("src");
    packPortrait.alt = "";
    packDownload.hidden = true;
    packDownload.removeAttribute("href");
    packDownload.removeAttribute("download");
    packDownload.setAttribute("aria-disabled", "true");
    packInstall.hidden = true;
  }

  function setResult(state, message, moveFocus = false) {
    resultRegion.dataset.state = state;
    resultRegion.textContent = message;
    if (moveFocus) resultRegion.focus({ preventScroll: true });
  }

  function setFieldError(message) {
    const hasError = Boolean(message);
    inputError.hidden = !hasError;
    inputError.textContent = message;
    input.setAttribute("aria-invalid", String(hasError));
  }

  function syncControls() {
    form.setAttribute("aria-busy", String(busy));
    resultPanel.setAttribute("aria-busy", String(busy));
    connectButton.disabled = busy || !supportsWebSerial(
      windowObject.navigator,
      windowObject.isSecureContext,
    );
    input.disabled = busy;
    verifyButton.disabled = busy || !connected;
  }

  function showDisconnected(message) {
    connected = false;
    currentPort = null;
    deviceTitle.textContent = "Kitsu not connected";
    deviceDetail.textContent = message;
    connectButton.textContent = "Connect Kitsu";
    resetPackResult();
    setResult("disconnected", "Kitsu is disconnected. Reconnect it before verifying the code.");
    syncControls();
  }

  async function closeCurrentPort() {
    const port = currentPort;
    currentPort = null;
    connected = false;
    if (!port || typeof port.close !== "function") return;
    try {
      await port.close();
    } catch {
      // A physically disconnected port may already be closed.
    }
  }

  async function connectKitsu() {
    setFieldError("");
    resetPackResult();
    busy = true;
    setResult("loading", "Waiting for permission to use a USB serial device…");
    syncControls();

    try {
      const selectedPort = await windowObject.navigator.serial.requestPort();
      await closeCurrentPort();
      await selectedPort.open({ baudRate: 115_200, bufferSize: 1_024 });
      currentPort = selectedPort;
      connected = true;
      deviceTitle.textContent = "Kitsu connected";
      deviceDetail.textContent = "USB serial is open at 115200 baud. The browser has not sent a code yet.";
      connectButton.textContent = "Change Kitsu";
      setResult("ready", "Kitsu connected. Enter the saved code and verify it against this device.");
      input.focus();
    } catch (error) {
      const denied = error?.name === "NotFoundError"
        || error?.name === "SecurityError"
        || error?.name === "NotAllowedError";
      if (denied && connected && currentPort) {
        deviceTitle.textContent = "Kitsu connected";
        deviceDetail.textContent = "Permission to choose a different USB serial device was not granted. The current connection remains open.";
        connectButton.textContent = "Change Kitsu";
        setResult("permission", "Connection permission was not granted. Choose Change Kitsu to try again.", true);
      } else if (denied) {
        deviceTitle.textContent = "Kitsu not connected";
        connectButton.textContent = "Connect Kitsu";
        deviceDetail.textContent = "Connection permission was not granted. Choose Connect Kitsu to try again.";
        setResult("permission", "Connection permission was not granted. Choose Connect Kitsu to try again.", true);
      } else {
        deviceTitle.textContent = "Kitsu not connected";
        connectButton.textContent = "Connect Kitsu";
        deviceDetail.textContent = "The USB serial connection could not be opened.";
        setResult("disconnected", "Kitsu is disconnected. Reconnect it before verifying the code.", true);
      }
    } finally {
      busy = false;
      syncControls();
    }
  }

  async function verifyCode(event) {
    event.preventDefault();
    setFieldError("");
    resetPackResult();

    const normalizedCode = normalizeUnlockCode(input.value);
    if (!normalizedCode) {
      setFieldError("Use K8 followed by three groups of five letters or numbers.");
      input.focus();
      return;
    }
    if (!connected || !currentPort) {
      showDisconnected("Connect the Heltec that received the code.");
      connectButton.focus();
      return;
    }

    busy = true;
    setResult("loading", "Checking the code and its hardware binding on the connected Kitsu…");
    syncControls();

    try {
      const requestId = createRequestId(windowObject.crypto);
      const verification = await verifyCodeWithPort(
        currentPort,
        normalizedCode,
        requestId,
      );

      if (verification.status === "invalid") {
        setResult(
          "invalid",
          "This code was not accepted by the connected Kitsu. Check the saved code or connect the Heltec that received it.",
          true,
        );
        return;
      }

      const publishedPack = publishedPackFor(verification.packId);
      packResult.hidden = false;
      if (!publishedPack) {
        packTitle.textContent = "Pack Not Published";
        packDetail.textContent = `${rarityLabel(verification.rarity)} pack ${verification.packId} is valid for this Kitsu, but no approved download is in the public catalog.`;
        setResult(
          "unpublished",
          "Code accepted for this Kitsu. The matching pet pack has not been published yet. Keep the code saved and check again after publication.",
          true,
        );
        return;
      }

      packPortrait.src = publishedPack.portraitUrl;
      packPortrait.alt = `${publishedPack.displayName} portrait`;
      packPortrait.hidden = false;
      packTitle.textContent = publishedPack.displayName;
      packDetail.textContent = `${rarityLabel(publishedPack.rarity)} pet pack. Requesting the ordinary .k868 package from the gated K32 service…`;
      setResult("loading", "Code accepted. Checking the gated pack delivery now…");

      const redeemed = await redeemPublishedPack(
        windowObject.fetch.bind(windowObject),
        normalizedCode,
        verification,
        publishedPack,
        windowObject.crypto,
      );
      try {
        const blob = new Blob([redeemed.bytes], { type: redeemed.mimeType });
        currentDownloadUrl = windowObject.URL.createObjectURL(blob);
      } catch {
        throw protocolError("download_unavailable", "This browser could not prepare the verified pack download.");
      }
      packDetail.textContent = `${rarityLabel(publishedPack.rarity)} pet pack. Keep this verified .k868 file private, then load it from the Companion section of the Kitsu flasher.`;
      packIntegrity.textContent = `SHA-256 ${publishedPack.sha256.toUpperCase()}`;
      packDownload.href = currentDownloadUrl;
      packDownload.download = redeemed.filename;
      packDownload.hidden = false;
      packDownload.removeAttribute("aria-disabled");
      packInstall.hidden = false;
      setResult("available", "Code accepted. The matching pet pack is ready to download.", true);
    } catch (error) {
      if (error?.code === "device_mismatch") {
        setResult(
          "invalid",
          "This code is bound to different hardware. Connect the Heltec that received it.",
          true,
        );
      } else if (error?.code === "verification_timeout") {
        await closeCurrentPort();
        deviceTitle.textContent = "Kitsu not connected";
        deviceDetail.textContent = "The serial port opened, but the firmware did not return a verification record.";
        connectButton.textContent = "Connect Kitsu";
        setResult(
          "disconnected",
          "The connected Kitsu did not answer. Check its cable, restart it, and try again.",
          true,
        );
      } else if (error?.code === "pack_unpublished") {
        packResult.hidden = false;
        packTitle.textContent = "Pack Not Published";
        packDetail.textContent = "The code and hardware proof were accepted, but the matching pack is not available yet.";
        setResult(
          "unpublished",
          "Code accepted for this Kitsu. Keep the code saved and check again after the pack is published.",
          true,
        );
      } else if (typeof error?.code === "string" && (
        error.code.startsWith("redemption_")
        || error.code === "invalid_download"
        || error.code === "integrity_unavailable"
        || error.code === "invalid_redemption"
        || error.code === "download_unavailable"
      )) {
        resetPackResult();
        const state = error.code === "redemption_denied" ? "invalid" : "service";
        setResult(state, error.message, true);
      } else {
        await closeCurrentPort();
        showDisconnected("The serial verification session ended before a valid response arrived.");
        resultRegion.focus({ preventScroll: true });
      }
    } finally {
      busy = false;
      syncControls();
    }
  }

  const supported = supportsWebSerial(
    windowObject.navigator,
    windowObject.isSecureContext,
  );
  if (!supported) {
    connectButton.textContent = "Web Serial Unavailable";
    deviceTitle.textContent = "Browser not supported";
    deviceDetail.textContent = "Use Chrome or Edge on a desktop computer with this page served over HTTPS.";
    setResult(
      "unsupported",
      "Web Serial is not available in this browser. Use Chrome or Edge on a desktop computer.",
    );
  } else {
    setResult("idle", "Connect a Kitsu to check a saved code against its hardware.");
  }

  if (providedCode !== null) {
    input.value = providedCode;
  } else if (hasCodeFragment) {
    setFieldError("The private link contained an invalid encounter code. Paste the saved code to continue.");
  } else if (hasLegacyQueryCode) {
    setFieldError("For privacy, query-string codes are not accepted. Open the saved code from Android again or paste it here.");
  }

  connectButton.addEventListener("click", connectKitsu);
  form.addEventListener("submit", verifyCode);
  input.addEventListener("input", () => {
    setFieldError("");
    resetPackResult();
    if (connected) {
      setResult("ready", "Kitsu connected. Verify the code when it is ready.");
    }
  });

  if (typeof windowObject.navigator.serial?.addEventListener === "function") {
    windowObject.navigator.serial.addEventListener("disconnect", (event) => {
      const disconnectedPort = event.port
        || (event.target !== windowObject.navigator.serial ? event.target : null);
      if (!currentPort || (disconnectedPort && disconnectedPort !== currentPort)) return;
      showDisconnected("The USB serial device was unplugged or reset.");
    });
  }

  windowObject.addEventListener("pagehide", () => {
    revokeDownloadUrl();
    void closeCurrentPort();
  }, { once: true });

  syncControls();
}

if (typeof window !== "undefined" && typeof document !== "undefined") {
  if (document.readyState === "loading") {
    document.addEventListener(
      "DOMContentLoaded",
      () => initializeUnlockPage(window, document),
      { once: true },
    );
  } else {
    initializeUnlockPage(window, document);
  }
}
