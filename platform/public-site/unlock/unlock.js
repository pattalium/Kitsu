"use strict";

import {
  publishedPackFor,
  rarityLabel,
} from "./catalog.js?sha256=3cfb9d21941d59b5730c0808b24170a1c4e93c39ac0a8b1c1bcc2777a7f3c8c5";

export const VERIFY_MARKER = "KITSU_CODE_VERIFY_V1 ";
export const VERIFY_SCHEMA = "kitsu.code-verification.v1";
export const MAX_SERIAL_LINE_CHARS = 2_048;
export const MAX_SERIAL_RESPONSE_BYTES = 65_536;
export const DEFAULT_VERIFY_TIMEOUT_MS = 8_000;

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
  if (/^KT[A-F0-9]{4}$/.test(value)) return value;
  if (/^[A-F0-9]{32}$/.test(value)) return value;
  return null;
}

function normalizedPackId(value) {
  return typeof value === "string" && /^[A-F0-9]{8}$/.test(value)
    ? value
    : null;
}

function normalizedCodeId(value) {
  return typeof value === "string" && /^[A-Z0-9]{8,64}$/.test(value)
    ? value
    : null;
}

export function normalizeUnlockCode(value) {
  if (typeof value !== "string") return null;
  const candidate = value.normalize("NFKC").trim();
  if (!candidate || /[^\x20-\x7e]/.test(candidate)) return null;
  if (/[^A-Za-z0-9 -]/.test(candidate)) return null;
  const normalized = candidate.replace(/[ -]+/g, "").toUpperCase();
  return /^[A-Z0-9]{8,64}$/.test(normalized) ? normalized : null;
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
  const packDownload = byId("pack-download");
  const packIntegrity = byId("pack-integrity");

  let currentPort = null;
  let connected = false;
  let busy = false;

  function resetPackResult() {
    packResult.hidden = true;
    packTitle.textContent = "";
    packDetail.textContent = "";
    packIntegrity.textContent = "";
    packDownload.hidden = true;
    packDownload.removeAttribute("href");
    packDownload.removeAttribute("download");
    packDownload.setAttribute("aria-disabled", "true");
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
      setFieldError("Use 8 to 64 letters or numbers. Spaces and hyphens are optional.");
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

      packTitle.textContent = publishedPack.displayName;
      packDetail.textContent = `${rarityLabel(publishedPack.rarity)} pet pack. The downloaded file is an ordinary .k868 package.`;
      packIntegrity.textContent = `SHA-256 ${publishedPack.sha256.toUpperCase()}`;
      packDownload.href = publishedPack.downloadUrl;
      packDownload.download = publishedPack.downloadUrl.split("/").at(-1);
      packDownload.hidden = false;
      packDownload.removeAttribute("aria-disabled");
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
