import { ESPLoader, Transport } from "esptool-js";
import {
  fetchVerifiedRelease,
  FLASH_PLAN,
  replacementRetryCoreArtifacts,
  reverifyArtifacts,
  sha256Hex,
} from "./release.js";
import {
  buildReplacementIntent,
  companionPackTransition,
  fetchOfficialPack,
  inspectInstalledPack,
  inspectReplacementTransaction,
  loadUnlockedPack,
  PACK_SLOT,
  REPLACEMENT_TRANSACTION,
  replacementTransactionTargets,
  reverifyPack,
  UNLOCKED_PACK_ID,
} from "./packs.js";
import "./styles.css";

const connectButton = document.querySelector("#connect");
const disconnectButton = document.querySelector("#disconnect");
const installButton = document.querySelector("#install");
const packSelect = document.querySelector("#pack-select");
const unlockedPackField = document.querySelector("#unlocked-pack-field");
const unlockedPackInput = document.querySelector("#unlocked-pack-file");
const packDetail = document.querySelector("#pack-detail");
const refreshButton = document.querySelector("#refresh");
const browserDetail = document.querySelector("#browser-detail");
const releaseDetail = document.querySelector("#release-detail");
const progress = document.querySelector("#progress");
const progressDetail = document.querySelector("#progress-detail");
const log = document.querySelector("#log");

let serialPort;
let transport;
let loader;
let detectedChip;
let detectedFlashSize;
let verifiedRelease;
let verifiedPack;
let installedPack;
let installedReplacementTransaction;
let busy = false;
const serialSupported = "serial" in navigator && window.isSecureContext;

function errorMessage(error) {
  return error instanceof Error ? error.message : String(error);
}

function append(message) {
  log.textContent += `\n${new Date().toLocaleTimeString()}  ${message}`;
  log.scrollTop = log.scrollHeight;
}

function packMatchesSelection(packId, pack = verifiedPack) {
  if (packId === "preserve") return true;
  if (packId === UNLOCKED_PACK_ID) {
    return pack?.definition.id === UNLOCKED_PACK_ID
      && pack.definition.source === "unlocked_file";
  }
  return pack?.definition.id === packId && pack.definition.source !== "unlocked_file";
}

function packIntegrityDescription(pack) {
  return pack.definition.source === "unlocked_file"
    ? "K868PK1 structure, bounds, CRC32, and SHA-256 verified"
    : "exact official bundle and SHA-256 verified";
}

function installedPackDescription(pack = installedPack) {
  if (pack?.status === "valid") {
    return `${pack.name} (ID ${pack.packId.toString(16).padStart(8, "0").toUpperCase()}, revision ${pack.revision})`;
  }
  if (pack?.status === "empty") return "an empty companion slot";
  return "an unreadable or invalid companion slot";
}

function installedPackMatches(left, right) {
  if (!left || !right || left.status !== right.status) return false;
  if (left.status === "empty") return true;
  if (left.status !== "valid") return false;
  return left.packId === right.packId
    && left.revision === right.revision
    && left.bytes === right.bytes
    && left.sha256 === right.sha256;
}

function replacementTransactionPending(transaction = installedReplacementTransaction) {
  return transaction?.status === "prepared" || transaction?.status === "committed";
}

function replacementTransactionDescription(transaction = installedReplacementTransaction) {
  if (transaction?.status === "prepared") {
    return `PREPARED replacement from ID ${transaction.sourcePackId.toString(16).padStart(8, "0").toUpperCase()} to ID ${transaction.targetPackId.toString(16).padStart(8, "0").toUpperCase()}`;
  }
  if (transaction?.status === "committed") {
    return `COMMITTED replacement from ID ${transaction.sourcePackId.toString(16).padStart(8, "0").toUpperCase()} to ID ${transaction.targetPackId.toString(16).padStart(8, "0").toUpperCase()}`;
  }
  if (transaction?.status === "invalid") return "an invalid replacement transaction";
  return "no pending replacement transaction";
}

function byteArraysMatch(left, right) {
  if (!(left instanceof Uint8Array) || !(right instanceof Uint8Array)
    || left.byteLength !== right.byteLength) return false;
  return left.every((value, index) => value === right[index]);
}

function replacementTransactionsMatch(left, right) {
  if (!left || !right || left.status !== right.status) return false;
  if (left.status === "empty") return true;
  if (!replacementTransactionPending(left)) return false;
  return left.sourcePackId === right.sourcePackId
    && left.committedState === right.committedState
    && left.targetPackId === right.targetPackId
    && left.targetRevision === right.targetRevision
    && left.targetBytes === right.targetBytes
    && left.targetPayloadCrc32 === right.targetPayloadCrc32
    && left.targetHeaderCrc32 === right.targetHeaderCrc32
    && byteArraysMatch(left.preparedBytes, right.preparedBytes)
    && (left.status !== "committed"
      || byteArraysMatch(left.committedBytes, right.committedBytes));
}

function pendingReplacementCanBeCancelled(
  transaction = installedReplacementTransaction,
  physicalPack = installedPack,
) {
  return replacementTransactionPending(transaction)
    && physicalPack?.status === "valid"
    && physicalPack.packId === transaction.sourcePackId;
}

function replacementSelectionReady(pack = verifiedPack) {
  if (packSelect.value === "preserve") {
    return !replacementTransactionPending() || pendingReplacementCanBeCancelled();
  }
  if (installedReplacementTransaction?.status === "invalid") return false;
  if (replacementTransactionPending()) {
    try {
      return replacementTransactionTargets(installedReplacementTransaction, pack);
    } catch {
      return false;
    }
  }
  return installedPack?.status === "valid"
    || installedPack?.status === "empty"
    || (installedReplacementTransaction?.status === "empty"
      && installedPack?.status === "invalid");
}

function updateControls() {
  const connected = Boolean(loader)
    && loader.chip?.CHIP_NAME === "ESP32-S3"
    && detectedFlashSize === FLASH_PLAN.flashSize;
  connectButton.disabled = !serialSupported || busy || Boolean(transport);
  disconnectButton.disabled = busy || !transport;
  refreshButton.disabled = busy;
  packSelect.disabled = busy;
  const unlockedSelected = packSelect.value === UNLOCKED_PACK_ID;
  unlockedPackField.hidden = !unlockedSelected;
  unlockedPackInput.disabled = busy || !unlockedSelected;
  const packReady = packMatchesSelection(packSelect.value);
  const replacementSourceReady = replacementSelectionReady();
  installButton.disabled = busy || !connected || !verifiedRelease || !packReady || !replacementSourceReady;
  if (busy) installButton.textContent = "Install in progress";
  else if (connected && verifiedRelease && packReady) {
    installButton.textContent = verifiedPack
      ? `Install Kitsu + ${verifiedPack.definition.name}`
      : `Install ${verifiedRelease.manifest.firmware_version}`;
  }
  else installButton.textContent = "Install unavailable";
}

function setProgress(value, detail) {
  progress.hidden = false;
  progress.value = Math.max(0, Math.min(100, value));
  progressDetail.textContent = detail;
}

async function closeTransport({ reset = true, announce = true } = {}) {
  const activeLoader = loader;
  const activeTransport = transport;
  loader = undefined;
  transport = undefined;
  serialPort = undefined;
  detectedChip = undefined;
  detectedFlashSize = undefined;
  installedPack = undefined;
  installedReplacementTransaction = undefined;
  updateControls();
  if (!activeTransport) return;
  if (reset && activeLoader) {
    try {
      await activeLoader.after("hard_reset");
    } catch (error) {
      if (announce) append(`Reset warning: ${errorMessage(error)}`);
    }
  }
  try {
    await activeTransport.disconnect();
  } catch (error) {
    if (announce) append(`Disconnect warning: ${errorMessage(error)}`);
  }
  if (announce) append("Serial port disconnected.");
}

async function connect() {
  busy = true;
  updateControls();
  try {
    serialPort = await navigator.serial.requestPort();
    const info = serialPort.getInfo();
    const usb = [info.usbVendorId, info.usbProductId]
      .map((value) => value === undefined ? "unknown" : `0x${value.toString(16).padStart(4, "0")}`)
      .join(":");
    append(`Selected USB serial device ${usb}. Entering the ROM loader for identity checks.`);

    transport = new Transport(serialPort, false);
    transport.setDeviceLostCallback(() => {
      append("The USB serial device was removed.");
      loader = undefined;
      transport = undefined;
      serialPort = undefined;
      detectedChip = undefined;
      detectedFlashSize = undefined;
      installedPack = undefined;
      installedReplacementTransaction = undefined;
      updateControls();
    });
    loader = new ESPLoader({ transport, baudrate: 460800, debugLogging: false });
    detectedChip = await loader.main("default_reset");
    if (loader.chip?.CHIP_NAME !== "ESP32-S3") {
      throw new Error(`unsupported chip ${loader.chip?.CHIP_NAME ?? detectedChip ?? "unknown"}; ESP32-S3 required`);
    }
    detectedFlashSize = await loader.detectFlashSize();
    if (detectedFlashSize !== FLASH_PLAN.flashSize) {
      throw new Error(`unsupported flash size ${detectedFlashSize}; 8MB required`);
    }
    try {
      installedReplacementTransaction = await inspectReplacementTransaction(loader);
      append(`Replacement transaction gate: ${replacementTransactionDescription()}.`);
      if (replacementTransactionPending()) {
        append("Recovery mode: only the exact target bound by PREPARED can be retried. The physical companion slot is not used as the source identity.");
        if (installedReplacementTransaction.committedState === "invalid") {
          append(`COMMITTED is invalid and cannot authorize replacement: ${installedReplacementTransaction.committedError}. PREPARED remains valid for exact-target recovery.`);
        }
      }
    } catch (error) {
      installedReplacementTransaction = Object.freeze({ status: "invalid", error: errorMessage(error) });
      append(`Replacement transaction is invalid: ${errorMessage(error)}. Pet writes are blocked; Keep current pet remains non-destructive.`);
    }
    try {
      installedPack = await inspectInstalledPack(loader);
      append(`Current pet verified before writing: ${installedPackDescription()}.`);
    } catch (error) {
      installedPack = Object.freeze({ status: "invalid", error: errorMessage(error) });
      append(
        `Current pet could not be validated: ${errorMessage(error)}. `
        + (replacementTransactionPending()
          ? "PREPARED preserves the original identity; Keep current pet is blocked and only its exact target may retry."
          : installedReplacementTransaction?.status === "empty"
            ? "Keep current pet remains available. An explicitly selected verified pack can repair physical pack bytes, but no species-reset authorization will be written."
            : "Firmware-only preserve mode remains available; pet replacement is blocked."),
      );
    }
    if (packSelect.value === "preserve") await loadSelectedPack();
    browserDetail.textContent = `${detectedChip} with ${detectedFlashSize} flash verified through Espressif ROM loader. Current physical pack: ${installedPackDescription()}; ${replacementTransactionDescription()}.`;
    append(`Device gate passed: ${detectedChip}; chip family ESP32-S3; flash ${detectedFlashSize}.`);
  } catch (error) {
    if (error instanceof DOMException && error.name === "NotFoundError") {
      append("Port selection was cancelled. Nothing was written.");
    } else {
      append(`Device gate closed: ${errorMessage(error)}. Nothing was written.`);
    }
    await closeTransport({
      reset: !replacementTransactionPending(installedReplacementTransaction),
      announce: false,
    });
  } finally {
    busy = false;
    updateControls();
  }
}

async function loadSelectedPack({ allowMissingUnlockedFile = false } = {}) {
  const packId = packSelect.value;
  verifiedPack = undefined;
  if (packId === "preserve") {
    if (replacementTransactionPending() && !pendingReplacementCanBeCancelled()) {
      packDetail.textContent = `Keep current pet cannot clear the pending PREPARED record while the physical slot is ${installedPackDescription()}. Choose the exact bound target ID ${installedReplacementTransaction.targetPackId.toString(16).padStart(8, "0").toUpperCase()} to recover without losing the saved source identity.`;
    } else if (pendingReplacementCanBeCancelled()) {
      packDetail.textContent = `Keep current pet safely cancels stale ${installedReplacementTransaction.status.toUpperCase()} because the physical pack still matches its saved source ID. The companion slot and pet progress will not be written.`;
    } else {
      packDetail.textContent = `Keep current pet selected. ${installedPackDescription()} and all pet progress will be preserved; the companion-pack slot will not be written.`;
    }
    return null;
  }
  if (packId === UNLOCKED_PACK_ID && !unlockedPackInput.files?.[0]) {
    packDetail.textContent = "Choose the unlocked .k868 file you downloaded. It stays on this device and must pass every local format and integrity check.";
    if (allowMissingUnlockedFile) return null;
  }
  const pack = packId === UNLOCKED_PACK_ID
    ? await loadUnlockedPack(unlockedPackInput.files?.[0])
    : await fetchOfficialPack(packId);
  verifiedPack = pack;
  if (installedReplacementTransaction?.status === "invalid") {
    packDetail.textContent = `${pack.definition.name} is verified, but companion writes are blocked because the on-device replacement transaction is malformed. Keep current pet remains available.`;
  } else if (replacementTransactionPending()) {
    if (replacementTransactionTargets(installedReplacementTransaction, pack)) {
      packDetail.textContent = `${pack.definition.name} exactly matches the pending ${installedReplacementTransaction.status.toUpperCase()} replacement target. Retrying will use the preserved source ID ${installedReplacementTransaction.sourcePackId.toString(16).padStart(8, "0").toUpperCase()}, never the current physical pack, and requires destructive confirmation again.`;
    } else {
      packDetail.textContent = `${pack.definition.name} is verified but blocked. The pending replacement is bound to target ID ${installedReplacementTransaction.targetPackId.toString(16).padStart(8, "0").toUpperCase()}; only that exact pack can resume it.`;
    }
  } else if (installedPack?.status === "valid" && installedPack.packId !== pack.definition.packId) {
    packDetail.textContent = `${pack.definition.name} is ready: ${packIntegrityDescription(pack)}. This is a different species from ${installedPackDescription()}; replacement is destructive and requires a separate confirmation before any write.`;
  } else if (installedPack?.status === "valid") {
    packDetail.textContent = `${pack.definition.name} is ready: ${packIntegrityDescription(pack)}. Its pack ID matches the installed pet, so care and bond progress will be preserved.`;
  } else if (installedPack?.status === "empty") {
    packDetail.textContent = `${pack.definition.name} is ready: ${packIntegrityDescription(pack)}. The companion slot is empty: firmware will preserve legacy vitals, establish this pack's brain identity, and clear pack-specific traits and gifts.`;
  } else if (installedReplacementTransaction?.status === "empty" && installedPack?.status === "invalid") {
    packDetail.textContent = `${pack.definition.name} is ready for explicit physical-pack repair. No PREPARED or COMMITTED record will be written. Firmware activates it only if its ID matches durable companion state or the device is legacy packless; otherwise it quarantines the pack without resetting species state.`;
  } else {
    packDetail.textContent = `${pack.definition.name} is verified, but replacement stays blocked until the current companion slot can be validated.`;
  }
  return pack;
}

async function checkSelectedPack() {
  busy = true;
  updateControls();
  try {
    unlockedPackInput.removeAttribute("aria-invalid");
    const pack = await loadSelectedPack();
    if (pack) append(`Pet gate passed: ${pack.definition.name}; ${packIntegrityDescription(pack)} for the dedicated slot at 0x670000.`);
    else append("Pet choice: preserve the current companion-pack slot.");
  } catch (error) {
    if (packSelect.value === UNLOCKED_PACK_ID) unlockedPackInput.setAttribute("aria-invalid", "true");
    packDetail.textContent = "This file was rejected. Choose the downloaded .k868 file again, or download a fresh unlocked copy and retry.";
    append(`Pet gate closed: ${errorMessage(error)}`);
  } finally {
    busy = false;
    updateControls();
  }
}

async function checkUnlockedPackFile() {
  if (packSelect.value !== UNLOCKED_PACK_ID) return;
  verifiedPack = undefined;
  if (!unlockedPackInput.files?.[0]) {
    unlockedPackInput.removeAttribute("aria-invalid");
    packDetail.textContent = "Choose the unlocked .k868 file you downloaded. It stays on this device and must pass every local format and integrity check.";
    updateControls();
    return;
  }
  await checkSelectedPack();
}

async function packForInstall(packId, selectedPack) {
  if (packId === "preserve") return null;
  if (packId === UNLOCKED_PACK_ID) {
    if (!packMatchesSelection(packId, selectedPack)) {
      throw new Error("selected unlocked companion pack is not loaded");
    }
    await reverifyPack(selectedPack);
    return selectedPack;
  }
  return fetchOfficialPack(packId);
}

async function checkRelease() {
  busy = true;
  verifiedRelease = undefined;
  verifiedPack = undefined;
  releaseDetail.textContent = "Checking the Ed25519 manifest authority and every bootstrap artifact…";
  updateControls();
  try {
    const [release] = await Promise.all([
      fetchVerifiedRelease(),
      loadSelectedPack({ allowMissingUnlockedFile: true }),
    ]);
    verifiedRelease = release;
    const totalBytes = release.artifacts.reduce((total, artifact) => total + artifact.bytes.byteLength, 0);
    releaseDetail.textContent = `Kitsu ${release.manifest.firmware_version} is signed, physically accepted, and ready for seven bounded core writes (${totalBytes.toLocaleString()} bytes).`;
    append(`Release gate passed: ${release.manifest.release_id}; exact manifest signature and all seven SHA-256 write images verified.`);
  } catch (error) {
    releaseDetail.textContent = "No installable production release passed every signature, schema, acceptance, and artifact gate.";
    append(`Release gate closed: ${errorMessage(error)}`);
  } finally {
    busy = false;
    updateControls();
  }
}

async function verifyReadback(artifact, index, artifacts, start = 65, end = 99) {
  const totalBytes = artifacts.reduce((total, item) => total + item.record.bytes, 0);
  const precedingBytes = artifacts
    .slice(0, index)
    .reduce((total, item) => total + item.record.bytes, 0);
  const readback = await loader.readFlash(
    artifact.record.offset,
    artifact.record.bytes,
    (_packet, received, total) => {
      const ratio = Math.min(1, received / total);
      const completedBytes = precedingBytes + (artifact.record.bytes * ratio);
      setProgress(start + (completedBytes / totalBytes) * (end - start), `Reading back ${artifact.record.role}: ${received.toLocaleString()} / ${total.toLocaleString()} bytes`);
    },
  );
  if (readback.byteLength !== artifact.record.bytes) {
    throw new Error(`${artifact.record.role} readback length differs from the verified install plan`);
  }
  if (await sha256Hex(readback) !== artifact.record.sha256) {
    throw new Error(`${artifact.record.role} readback SHA-256 differs from the verified artifact`);
  }
  append(`Readback verified: ${artifact.record.role} at 0x${artifact.record.offset.toString(16).padStart(6, "0")}.`);
}

async function install() {
  if (busy || !loader || !verifiedRelease) return;
  const selectedPackId = packSelect.value;
  const packRequested = selectedPackId !== "preserve";
  const selectedPack = verifiedPack;
  if (packRequested && !packMatchesSelection(selectedPackId, selectedPack)) return;
  const selectedPackName = selectedPack?.definition.name;
  busy = true;
  updateControls();
  setProgress(0, "Checking the latest signed stable release");
  let coreVerified = false;
  let packVerified = !packRequested;
  let replacementPreparedVerified = true;
  let replacementCommittedVerified = true;
  let destructiveReplacement = false;
  let replacementRetry = false;
  let packWriteStarted = false;
  let resetAttempted = false;
  try {
    if (loader.chip?.CHIP_NAME !== "ESP32-S3" || detectedFlashSize !== FLASH_PLAN.flashSize) {
      throw new Error("device identity gate is no longer valid");
    }
    const [latestRelease, latestPack] = await Promise.all([
      fetchVerifiedRelease(),
      packForInstall(selectedPackId, selectedPack),
    ]);
    if (latestRelease.manifest.release_id !== verifiedRelease.manifest.release_id) {
      append(
        `A newer signed release is available: ${latestRelease.manifest.release_id}; `
        + "the installer switched to it before writing.",
      );
    }
    verifiedRelease = latestRelease;
    verifiedPack = latestPack ?? undefined;
    releaseDetail.textContent = `Kitsu ${verifiedRelease.manifest.firmware_version} is the latest signed, physically accepted, byte-verified release.`;
    await reverifyArtifacts(verifiedRelease);
    if (latestPack) await reverifyPack(latestPack);

    let currentTransaction;
    try {
      currentTransaction = await inspectReplacementTransaction(loader);
    } catch (error) {
      currentTransaction = Object.freeze({ status: "invalid", error: errorMessage(error) });
    }
    if (latestPack && currentTransaction.status === "invalid") {
      throw new Error(`replacement transaction is invalid: ${currentTransaction.error}`);
    }
    if (["empty", "prepared", "committed"].includes(installedReplacementTransaction?.status)
      && !replacementTransactionsMatch(installedReplacementTransaction, currentTransaction)) {
      throw new Error("replacement transaction changed after connection; reconnect and review it again");
    }
    installedReplacementTransaction = currentTransaction;

    let currentPack;
    try {
      currentPack = await inspectInstalledPack(loader);
    } catch (error) {
      const explicitRepair = latestPack
        && currentTransaction.status === "empty"
        && installedPack?.status === "invalid";
      if (latestPack && !replacementTransactionPending(currentTransaction) && !explicitRepair) {
        throw new Error(`current companion changed or is invalid: ${errorMessage(error)}`);
      }
      currentPack = Object.freeze({ status: "invalid", error: errorMessage(error) });
    }
    if (!replacementTransactionPending(currentTransaction)
      && (installedPack?.status === "valid" || installedPack?.status === "empty")
      && !installedPackMatches(installedPack, currentPack)) {
      throw new Error("current companion changed after connection; reconnect and review it again");
    }
    if (latestPack && installedPack?.status === "invalid" && currentPack.status !== "invalid") {
      throw new Error("the physical companion changed after its failed connection-time inspection; reconnect before any pack write");
    }
    installedPack = currentPack;
    const transition = companionPackTransition(currentPack, latestPack, currentTransaction);
    destructiveReplacement = transition.destructive;
    replacementRetry = Boolean(transition.retry);

    let replacementPrepared;
    let replacementCommitted;
    if (destructiveReplacement) {
      // On retry, retain the exact validated PREPARED bytes read before the
      // core phase. Never rebuild the source identity from a target or partial
      // physical companion slot.
      const intentBytes = replacementRetry
        ? currentTransaction.preparedBytes.slice()
        : buildReplacementIntent(transition.sourcePackId, latestPack);
      const intentSha256 = await sha256Hex(intentBytes);
      replacementPrepared = Object.freeze({
        record: Object.freeze({
          role: "companion_replacement_prepared",
          offset: REPLACEMENT_TRANSACTION.prepared.offset,
          bytes: intentBytes.byteLength,
          sha256: intentSha256,
        }),
        bytes: intentBytes,
      });
      replacementCommitted = Object.freeze({
        record: Object.freeze({
          role: "companion_replacement_committed",
          offset: REPLACEMENT_TRANSACTION.committed.offset,
          bytes: intentBytes.byteLength,
          sha256: intentSha256,
        }),
        bytes: intentBytes,
      });
      replacementPreparedVerified = false;
      replacementCommittedVerified = false;
    }

    const coreArtifacts = replacementRetry
      ? await replacementRetryCoreArtifacts(verifiedRelease)
      : verifiedRelease.artifacts;

    const destructiveSource = replacementRetry
      ? `stored pet ID ${transition.sourcePackId.toString(16).padStart(8, "0").toUpperCase()} recovered from PREPARED (physical slot: ${installedPackDescription(currentPack)})`
      : installedPackDescription(currentPack);
    const packPlan = !latestPack
      ? `The current companion slot (${installedPackDescription(currentPack)}) will not be written, and all pet progress stays untouched. `
      : destructiveReplacement
        ? `The validated ${latestPack.definition.name} pack will replace ${destructiveSource}. A separate destructive confirmation is required. PREPARED is verified before the companion slot is written; matching COMMITTED is written only after exact target SHA-256 readback. `
        : transition.repair
          ? `The physical companion slot is invalid, so the validated ${latestPack.definition.name} pack will be written as an explicit repair with no PREPARED or COMMITTED authorization. Firmware activates it only if its ID matches durable state or this is a legacy packless first assignment; otherwise it quarantines it without a species-state reset. `
        : currentPack.status === "empty"
          ? `The validated ${latestPack.definition.name} pack will be assigned to the empty companion slot. Firmware preserves legacy vitals, establishes the new pack brain identity, and clears pack-specific traits and gifts. `
          : `The validated ${latestPack.definition.name} pack has the same companion ID as ${installedPackDescription(currentPack)}, so pet progress stays untouched. `;
    const confirmed = window.confirm(
      `${latestPack ? `Install Kitsu and ${latestPack.definition.name}` : "Install Kitsu"} now?\n\n`
      + "This writes the reviewed rollback-enabled bootloader, partition table, the same application in app0 and app1, and an empty OTA journal at the end of each application slot. "
      + (replacementRetry
        ? "Because this is an interrupted replacement retry, it preserves the two transaction sectors and rewrites only the signed erased suffix of the retired connectivity partition. "
        : "It writes an exact clear image over the retired connectivity partition. ")
      + "Every selected region is read back. "
      + packPlan
      + "It never calls full-chip erase and does not write OTA data, controller records, MeshCore state, coredump, or eFuses.",
    );
    if (!confirmed) {
      append("Install cancelled. Nothing was written.");
      return;
    }
    if (destructiveReplacement) {
      const destructiveConfirmed = window.confirm(
        `DESTRUCTIVE PET REPLACEMENT\n\n`
        + `Current: ${destructiveSource}\n`
        + `New: ${latestPack.definition.name} (ID ${latestPack.definition.packId.toString(16).padStart(8, "0").toUpperCase()})\n\n`
        + "This permanently resets the current pet's care stats, bond progress, memories, traits, and gifts. Firmware updates do not require this. Choose Cancel to keep the current pet and use Keep current pet instead.",
      );
      if (!destructiveConfirmed) {
        append("Destructive pet replacement cancelled. Nothing was written.");
        return;
      }
    }

    setProgress(4, "Latest release verified; starting seven bounded writes");
    append(
      `Install authorized for latest release ${verifiedRelease.manifest.release_id}. `
      + (replacementRetry
        ? "Starting six exact signed core writes plus the SHA-bound erased connectivity suffix at 0x7b2000; PREPARED/COMMITTED remain untouched."
        : "Starting the exact seven-write signed core phase.")
      + " Erase-all remains disabled.",
    );
    if (latestPack) append(`Selected pet phase: ${latestPack.definition.name}, ${latestPack.record.bytes.toLocaleString()} bytes at 0x670000; ${packIntegrityDescription(latestPack)}.`);
    else append("Selected pet phase: preserve the current companion-pack slot without writing it.");

    const totalWriteBytes = coreArtifacts.reduce((total, artifact) => total + artifact.record.bytes, 0);
    const precedingWriteBytes = coreArtifacts.map((_, index) => coreArtifacts
      .slice(0, index)
      .reduce((total, artifact) => total + artifact.record.bytes, 0));
    await loader.writeFlash({
      fileArray: coreArtifacts.map((artifact) => ({
        data: artifact.bytes,
        address: artifact.record.offset,
      })),
      flashMode: "dio",
      flashFreq: "80m",
      flashSize: "8MB",
      eraseAll: false,
      compress: true,
      reportProgress(fileIndex, written, total) {
        const ratio = total === 0 ? 0 : Math.min(1, written / total);
        const artifact = coreArtifacts[fileIndex];
        const completedBytes = precedingWriteBytes[fileIndex] + (artifact.record.bytes * ratio);
        setProgress(4 + (completedBytes / totalWriteBytes) * 54, `Writing signed core ${artifact.record.role}: ${written.toLocaleString()} / ${total.toLocaleString()} transfer bytes`);
      },
    });

    const coreReadbackEnd = replacementPrepared ? 76 : latestPack ? 82 : 99;
    for (const [index, artifact] of coreArtifacts.entries()) {
      await verifyReadback(artifact, index, coreArtifacts, 58, coreReadbackEnd);
    }
    coreVerified = true;
    append("Signed core phase complete: all seven regions passed SHA-256 readback.");

    if (latestPack) {
      await reverifyPack(latestPack);
      if (replacementPrepared) {
        setProgress(76, replacementRetry
          ? "Core verified; checking retained PREPARED before the companion pack"
          : "Core verified; writing PREPARED before any companion-pack byte");
        if (!replacementRetry) {
          await loader.writeFlash({
            fileArray: [{
              data: replacementPrepared.bytes,
              address: replacementPrepared.record.offset,
            }],
            flashMode: "dio",
            flashFreq: "80m",
            flashSize: "8MB",
            eraseAll: false,
            compress: true,
            reportProgress(_fileIndex, written, total) {
              const ratio = total === 0 ? 0 : Math.min(1, written / total);
              setProgress(76 + ratio * 3, `Writing PREPARED: ${written.toLocaleString()} / ${total.toLocaleString()} transfer bytes`);
            },
          });
        }
        await verifyReadback(replacementPrepared, 0, [replacementPrepared], 79, 82);
        replacementPreparedVerified = true;
        append(`${replacementRetry ? "Retained, never erased" : "New"} PREPARED replacement record passed SHA-256 readback before the target pack write.`);
      }

      setProgress(82, `Core verified; writing ${latestPack.definition.name} to the companion slot`);
      packWriteStarted = true;
      await loader.writeFlash({
        fileArray: [{ data: latestPack.bytes, address: latestPack.record.offset }],
        flashMode: "dio",
        flashFreq: "80m",
        flashSize: "8MB",
        eraseAll: false,
        compress: true,
        reportProgress(_fileIndex, written, total) {
          const ratio = total === 0 ? 0 : Math.min(1, written / total);
          setProgress(82 + ratio * 6, `Writing ${latestPack.definition.name} pet: ${written.toLocaleString()} / ${total.toLocaleString()} transfer bytes`);
        },
      });
      await verifyReadback(latestPack, 0, [latestPack], 88, replacementCommitted ? 94 : 99);
      packVerified = true;
      packDetail.textContent = transition.repair
        ? `${latestPack.definition.name} physical pack bytes were repaired and verified by SHA-256. Firmware will activate them only for a matching durable ID or legacy packless first assignment; otherwise it will quarantine them.`
        : `${latestPack.definition.name} installed with Kitsu in the same USB session and verified by SHA-256 readback.`;
      append(
        transition.repair
          ? `${latestPack.definition.name} physical-pack repair passed exact readback; no species-reset authorization exists.`
          : `${latestPack.definition.name} pet phase complete: dedicated companion slot read back exactly.`,
      );

      if (replacementCommitted) {
        setProgress(94, "Target pack verified; writing COMMITTED in its separate sector");
        await loader.writeFlash({
          fileArray: [{
            data: replacementCommitted.bytes,
            address: replacementCommitted.record.offset,
          }],
          flashMode: "dio",
          flashFreq: "80m",
          flashSize: "8MB",
          eraseAll: false,
          compress: true,
          reportProgress(_fileIndex, written, total) {
            const ratio = total === 0 ? 0 : Math.min(1, written / total);
            setProgress(94 + ratio * 3, `Writing COMMITTED: ${written.toLocaleString()} / ${total.toLocaleString()} transfer bytes`);
          },
        });
        await verifyReadback(replacementCommitted, 0, [replacementCommitted], 97, 99);
        replacementCommittedVerified = true;
        append("Matching COMMITTED replacement record passed SHA-256 readback after the exact target pack. Firmware will require both records after the explicit reset.");
      }
    }

    setProgress(99, "All selected readback hashes passed; resetting the Heltec once");
    resetAttempted = true;
    await loader.after("hard_reset");
    setProgress(100, "Install and SHA-256 readback complete");
    append(
      `Kitsu ${verifiedRelease.manifest.firmware_version}${latestPack
        ? transition.repair
          ? ` with ${latestPack.definition.name} physical-pack repair`
          : ` and ${latestPack.definition.name}`
        : ""} installed and read back successfully. `
      + "The Heltec was reset once; releasing the serial port.",
    );
    await closeTransport({ reset: false, announce: true });
  } catch (error) {
    if (resetAttempted && coreVerified && packVerified && replacementCommittedVerified) {
      setProgress(progress.value, "Installation verified; automatic reset could not be confirmed");
      append(`Every selected region passed SHA-256 readback, but the final automatic reset could not be confirmed: ${errorMessage(error)}. Press RST once on the Heltec. No second automatic reset was attempted.`);
    } else if (coreVerified && destructiveReplacement && !replacementPreparedVerified) {
      setProgress(progress.value, "Replacement stopped before PREPARED was verified");
      append(`The target companion slot was not started because PREPARED did not pass exact readback: ${errorMessage(error)}.`);
    } else if (coreVerified && packRequested && !packVerified) {
      setProgress(progress.value, `${selectedPackName} install stopped after the signed core passed`);
      append(`Signed core and PREPARED are verified, but the ${selectedPackName} target pack failed closed: ${errorMessage(error)}. COMMITTED was not written.`);
    } else if (coreVerified && destructiveReplacement && packVerified && !replacementCommittedVerified) {
      setProgress(progress.value, "Replacement stopped before COMMITTED was verified");
      append(`The target pack passed readback, but COMMITTED did not: ${errorMessage(error)}. The Heltec will remain in the ROM loader for transaction inspection and exact-target retry.`);
    } else {
      setProgress(progress.value, "Install stopped; no automatic retry was attempted");
      append(`Install failed closed: ${errorMessage(error)}. Inspect the log and reconnect before retrying.`);
    }
    const holdInLoader = destructiveReplacement
      && !replacementCommittedVerified
      && !resetAttempted
      && (replacementRetry || packWriteStarted);
    if (holdInLoader) {
      append("The Heltec is being left in the ROM loader so an intermediate unapproved species cannot boot. Reconnect to finish or restore the intended pack; firmware also quarantines any unapproved mismatch after a later power cycle.");
    }
    await closeTransport({ reset: !resetAttempted && !holdInLoader, announce: true });
  } finally {
    busy = false;
    updateControls();
  }
}

// Browsers may restore a previous form selection across reloads.  Firmware
// reflashing must always start in preserve mode, never a remembered starter.
packSelect.value = "preserve";
unlockedPackInput.value = "";

if (serialSupported) {
  browserDetail.textContent = "Web Serial is available in this secure context. Device access starts only after Connect Heltec is clicked.";
  connectButton.addEventListener("click", () => { void connect(); });
  disconnectButton.addEventListener("click", () => {
    const holdInLoader = replacementTransactionPending();
    if (holdInLoader) {
      append("Pending replacement detected; disconnecting without reset so PREPARED/COMMITTED can be inspected on the next connection.");
    }
    void closeTransport({ reset: !holdInLoader });
  });
} else {
  browserDetail.textContent = "Web Serial is unavailable. Use current desktop Chrome or Edge over HTTPS.";
  connectButton.disabled = true;
}

refreshButton.addEventListener("click", () => { void checkRelease(); });
packSelect.addEventListener("change", () => {
  verifiedPack = undefined;
  unlockedPackInput.removeAttribute("aria-invalid");
  if (packSelect.value === UNLOCKED_PACK_ID) {
    packDetail.textContent = "Choose the unlocked .k868 file you downloaded. It stays on this device and must pass every local format and integrity check.";
    append("Pet choice: waiting for a local unlocked .k868 file. Nothing has been accepted or written.");
    updateControls();
    return;
  }
  unlockedPackInput.value = "";
  void checkSelectedPack();
});
unlockedPackInput.addEventListener("change", () => { void checkUnlockedPackFile(); });
installButton.addEventListener("click", () => { void install(); });
window.addEventListener("pageshow", (event) => {
  if (!event.persisted) return;
  packSelect.value = "preserve";
  unlockedPackInput.value = "";
  verifiedPack = undefined;
  void checkSelectedPack();
});
window.addEventListener("pagehide", () => { void closeTransport({ reset: false, announce: false }); });
void checkRelease();
