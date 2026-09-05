import { ESPLoader, Transport } from "esptool-js";
import {
  fetchVerifiedRelease,
  FLASH_PLAN,
  reverifyArtifacts,
  sha256Hex,
} from "./release.js";
import { inspectInstalledFlashLayout } from "./layout-gate.js";
import {
  CURRENT_FLASH_PLAN,
  fetchVerifiedCurrentRelease,
  inspectCurrentOtaSelection,
} from "./current-release.js";
import {
  FACTORY_INIT_PLAN,
  fetchVerifiedFactoryPartitionTable,
  prepareFactoryInitialization,
} from "./factory-init.js";
import {
  buildReplacementIntent,
  companionPackTransition,
  fetchOfficialPack,
  inspectInstalledPack,
  inspectReplacementTransaction,
  loadUnlockedPack,
  PACK_SLOT,
  REPLACEMENT_TRANSACTION,
  reverifyPack,
  UNLOCKED_PACK_ID,
} from "./packs.js";
import "./styles.css";

const LEGACY_OTA_DATA = Object.freeze({ offset: 0x00e000, bytes: 0x002000 });
const COMPANION_PACK = Object.freeze({
  offset: CURRENT_FLASH_PLAN.companionPackOffset,
  bytes: CURRENT_FLASH_PLAN.companionPackBytes,
});

const connectButton = document.querySelector("#connect");
const disconnectButton = document.querySelector("#disconnect");
const installButton = document.querySelector("#install");
const installPackButton = document.querySelector("#install-pack");
const refreshButton = document.querySelector("#refresh");
const browserDetail = document.querySelector("#browser-detail");
const releaseDetail = document.querySelector("#release-detail");
const deviceDetail = document.querySelector("#device-detail");
const installedPackDetail = document.querySelector("#installed-pack-detail");
const packSelect = document.querySelector("#pack-select");
const unlockedPackField = document.querySelector("#unlocked-pack-field");
const unlockedPackInput = document.querySelector("#unlocked-pack-file");
const packDetail = document.querySelector("#pack-detail");
const progress = document.querySelector("#progress");
const progressDetail = document.querySelector("#progress-detail");
const log = document.querySelector("#log");

let serialPort;
let transport;
let loader;
let detectedChip;
let detectedFlashSize;
let installedFlashLayout;
let installedOtaSelection;
let installedPack;
let replacementTransaction;
let replacementTransactionError;
let verifiedLegacyRelease;
let verifiedCurrentRelease;
let verifiedPack;
let factoryInitializerReady = false;
let busy = false;
const serialSupported = "serial" in navigator && window.isSecureContext;

function errorMessage(error) {
  return error instanceof Error ? error.message : String(error);
}

function append(message) {
  log.textContent += `\n${new Date().toLocaleTimeString()}  ${message}`;
  log.scrollTop = log.scrollHeight;
}

function setProgress(value, detail) {
  progress.hidden = false;
  progress.value = Math.max(0, Math.min(100, value));
  progressDetail.textContent = detail;
}

function packMatchesSelection(packId, pack = verifiedPack) {
  if (packId === "preserve") return true;
  if (packId === UNLOCKED_PACK_ID) {
    return pack?.definition.id === UNLOCKED_PACK_ID
      && pack.definition.source === "unlocked_file";
  }
  return pack?.definition.id === packId && pack.definition.source !== "unlocked_file";
}

function selectedPackName() {
  return packSelect.value === "preserve" ? null : verifiedPack?.definition.name;
}

function packIntegrityDescription(pack) {
  return pack.definition.source === "unlocked_file"
    ? "format, animation bounds, CRC32, and SHA-256 verified locally"
    : "built-in pack and SHA-256 verified";
}

function describeInstalledPack() {
  if (installedFlashLayout?.kind === "factory") {
    installedPackDetail.textContent = selectedPackName()
      ? `${selectedPackName()} will be installed as this board's starter companion during initialization.`
      : "Choose Fox, Cat, Dog, or your own .k868 pack to install a starter during initialization. Keep installed companion leaves the region untouched.";
    return;
  }
  if (installedFlashLayout?.kind === "legacy") {
    installedPackDetail.textContent = "This legacy layout preserves its companion. Install the current layout before changing packs.";
    return;
  }
  if (installedFlashLayout?.kind !== "migrated") {
    installedPackDetail.textContent = "Connect a current-layout Heltec to inspect its installed companion. On a stock board, your selected pack is installed as the starter during initialization.";
    return;
  }
  if (replacementTransactionError) {
    installedPackDetail.textContent = `Companion changes are blocked: ${replacementTransactionError}`;
    return;
  }
  const pending = ["prepared", "committed"].includes(replacementTransaction?.status)
    ? " An interrupted replacement is waiting for the same target pack to be selected and retried."
    : "";
  if (installedPack?.status === "valid") {
    installedPackDetail.textContent = `Installed companion: ${installedPack.name}, revision ${installedPack.revision}.${pending}`;
  } else if (installedPack?.status === "empty") {
    installedPackDetail.textContent = `No companion is installed yet. Choose a starter below.${pending}`;
  } else {
    installedPackDetail.textContent = `The companion slot needs repair before it can load a pet.${pending}`;
  }
}

function releaseForInstalledLayout() {
  if (installedFlashLayout?.kind === "migrated") return verifiedCurrentRelease;
  if (installedFlashLayout?.kind === "legacy") return verifiedLegacyRelease;
  if (installedFlashLayout?.kind === "factory"
    && verifiedCurrentRelease && verifiedLegacyRelease && factoryInitializerReady) {
    return verifiedCurrentRelease;
  }
  return undefined;
}

function updateControls() {
  const connected = Boolean(loader)
    && loader.chip?.CHIP_NAME === "ESP32-S3"
    && detectedFlashSize === FLASH_PLAN.flashSize
    && ["factory", "legacy", "migrated"].includes(installedFlashLayout?.kind);
  const release = releaseForInstalledLayout();
  const selectedPackReady = packMatchesSelection(packSelect.value);
  connectButton.disabled = !serialSupported || busy || Boolean(transport);
  disconnectButton.disabled = busy || !transport;
  refreshButton.disabled = busy;
  packSelect.disabled = busy;
  const unlockedSelected = packSelect.value === UNLOCKED_PACK_ID;
  unlockedPackField.hidden = !unlockedSelected;
  unlockedPackInput.disabled = busy || !unlockedSelected;
  installButton.disabled = busy || !connected || !release
    || (installedFlashLayout?.kind === "factory" && !selectedPackReady);
  installPackButton.disabled = busy
    || !connected
    || installedFlashLayout?.kind !== "migrated"
    || packSelect.value === "preserve"
    || !selectedPackReady
    || Boolean(replacementTransactionError);
  if (busy) installButton.textContent = "Install in progress";
  else if (connected && installedFlashLayout.kind === "migrated" && release) {
    installButton.textContent = `Install latest ${release.manifest.firmware_version}`;
  } else if (connected && installedFlashLayout.kind === "legacy" && release) {
    installButton.textContent = `Install legacy recovery ${release.manifest.firmware_version}`;
  } else if (connected && installedFlashLayout.kind === "factory" && release) {
    installButton.textContent = selectedPackName()
      ? `Initialize Kitsu ${release.manifest.firmware_version} + ${selectedPackName()}`
      : `Initialize with Kitsu ${release.manifest.firmware_version}`;
  } else {
    installButton.textContent = "Install unavailable";
  }
  if (busy) installPackButton.textContent = "Companion action in progress";
  else if (connected && installedFlashLayout?.kind === "factory" && selectedPackName()) {
    installPackButton.textContent = `Starter selected: ${selectedPackName()}`;
  } else if (connected && installedFlashLayout?.kind === "migrated" && selectedPackName()) {
    installPackButton.textContent = `Install ${selectedPackName()}`;
  } else {
    installPackButton.textContent = "Install companion unavailable";
  }
  describeInstalledPack();
}

async function closeTransport({ reset = true, announce = true } = {}) {
  const activeLoader = loader;
  const activeTransport = transport;
  loader = undefined;
  transport = undefined;
  serialPort = undefined;
  detectedChip = undefined;
  detectedFlashSize = undefined;
  installedFlashLayout = undefined;
  installedOtaSelection = undefined;
  installedPack = undefined;
  replacementTransaction = undefined;
  replacementTransactionError = undefined;
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
    append(`Selected USB serial device ${usb}. Entering the ROM loader for read-only checks.`);

    transport = new Transport(serialPort, false);
    transport.setDeviceLostCallback(() => {
      append("The USB serial device was removed.");
      loader = undefined;
      transport = undefined;
      serialPort = undefined;
      detectedChip = undefined;
      detectedFlashSize = undefined;
      installedFlashLayout = undefined;
      installedOtaSelection = undefined;
      installedPack = undefined;
      replacementTransaction = undefined;
      replacementTransactionError = undefined;
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

    installedFlashLayout = await inspectInstalledFlashLayout(loader);
    if (installedFlashLayout.kind === "unknown") {
      const digest = installedFlashLayout.sha256 ? `; SHA-256 ${installedFlashLayout.sha256}` : "";
      throw new Error(`partition-table check failed: ${installedFlashLayout.reason}${digest}`);
    }
    if (installedFlashLayout.kind === "migrated") {
      installedOtaSelection = await inspectCurrentOtaSelection(loader);
      installedPack = await inspectInstalledPack(loader);
      try {
        replacementTransaction = await inspectReplacementTransaction(loader);
      } catch (error) {
        replacementTransaction = undefined;
        replacementTransactionError = errorMessage(error);
      }
      browserDetail.textContent = `${detectedChip} with ${detectedFlashSize} flash. Current Kitsu layout verified; ${installedOtaSelection.label} is selected for a firmware-only reinstall.`;
      deviceDetail.textContent = `The signed latest application will be written only to ${installedOtaSelection.label} at 0x${installedOtaSelection.offset.toString(16).padStart(6, "0")}. OTA metadata and the complete companion-pack region are hashed before and after.`;
      append(`Current layout verified. Boot selection: ${installedOtaSelection.label}, sequence ${installedOtaSelection.sequence ?? "initial"}, state ${installedOtaSelection.stateName}.`);
      if (installedPack.status === "valid") {
        append(`Installed companion verified: ${installedPack.name}, revision ${installedPack.revision}, SHA-256 ${installedPack.sha256}.`);
      } else if (installedPack.status === "empty") {
        append("Companion slot is empty; a starter pack can be installed.");
      } else {
        append(`Companion slot needs repair: ${installedPack.reason}.`);
      }
      if (replacementTransactionError) {
        append(`Companion replacement gate closed: ${replacementTransactionError}`);
      } else if (["prepared", "committed"].includes(replacementTransaction.status)) {
        append("An interrupted companion replacement was found. Only the exact saved target pack can continue it.");
      }
    } else if (installedFlashLayout.kind === "factory") {
      browserDetail.textContent = `${detectedChip} with ${detectedFlashSize} flash. The stock Heltec factory layout is verified for first-time Kitsu initialization.`;
      deviceDetail.textContent = "This new-board path installs the current Kitsu layout and signed latest firmware in one pass. Choose a starter companion below, or keep the pack region untouched.";
      append(`Stock Heltec factory layout verified: ${installedFlashLayout.sha256}.`);
    } else {
      browserDetail.textContent = `${detectedChip} with ${detectedFlashSize} flash. Exact legacy Kitsu layout verified for the signed recovery release.`;
      deviceDetail.textContent = "This board still uses the legacy layout. The historical signed recovery remains available and keeps the companion-pack region byte-for-byte unchanged.";
      append(`Legacy layout verified: ${installedFlashLayout.sha256}.`);
    }
    append("Firmware protection is active: firmware-only installs preserve the complete companion-pack region. Companion writes require a separate explicit selection and confirmation.");
  } catch (error) {
    if (error instanceof DOMException && error.name === "NotFoundError") {
      append("Port selection was cancelled. Nothing was written.");
    } else {
      append(`Device check stopped: ${errorMessage(error)}. Nothing was written.`);
    }
    await closeTransport({ reset: true, announce: false });
  } finally {
    busy = false;
    updateControls();
  }
}

async function loadSelectedPack({ allowMissingFile = false } = {}) {
  const packId = packSelect.value;
  verifiedPack = undefined;
  if (packId === "preserve") {
    packDetail.textContent = "The installed companion will be kept. Choose a pet only when you want to install or replace it.";
    return null;
  }
  if (packId === UNLOCKED_PACK_ID && !unlockedPackInput.files?.[0]) {
    packDetail.textContent = "Choose a .k868 companion pack from this computer. It is read locally and must pass every format and integrity check.";
    if (allowMissingFile) return null;
  }
  const pack = packId === UNLOCKED_PACK_ID
    ? await loadUnlockedPack(unlockedPackInput.files?.[0])
    : await fetchOfficialPack(packId);
  verifiedPack = pack;
  packDetail.textContent = `${pack.definition.name} is ready: ${packIntegrityDescription(pack)}.`;
  return pack;
}

async function checkSelectedPack() {
  busy = true;
  updateControls();
  try {
    unlockedPackInput.removeAttribute("aria-invalid");
    const pack = await loadSelectedPack();
    if (pack) {
      append(`Companion verified: ${pack.definition.name}, revision ${pack.definition.revision}, ${pack.record.bytes.toLocaleString()} bytes.`);
    } else {
      append("Companion selection: keep the installed pack; no companion write is enabled.");
    }
  } catch (error) {
    verifiedPack = undefined;
    if (packSelect.value === UNLOCKED_PACK_ID) {
      unlockedPackInput.setAttribute("aria-invalid", "true");
    }
    packDetail.textContent = `Companion rejected: ${errorMessage(error)}`;
    append(`Companion gate closed: ${errorMessage(error)}.`);
  } finally {
    busy = false;
    updateControls();
  }
}

async function selectedPackForWrite() {
  if (!packMatchesSelection(packSelect.value) || packSelect.value === "preserve") {
    throw new Error("choose and verify a companion pack first");
  }
  if (packSelect.value === UNLOCKED_PACK_ID) {
    await reverifyPack(verifiedPack);
    return verifiedPack;
  }
  const current = await fetchOfficialPack(packSelect.value);
  verifiedPack = current;
  return current;
}

async function checkRelease() {
  busy = true;
  verifiedLegacyRelease = undefined;
  verifiedCurrentRelease = undefined;
  factoryInitializerReady = false;
  releaseDetail.textContent = "Verifying the latest signed firmware and the legacy recovery release…";
  updateControls();
  try {
    const [currentResult, legacyResult, factoryResult] = await Promise.allSettled([
      fetchVerifiedCurrentRelease(),
      fetchVerifiedRelease(),
      fetchVerifiedFactoryPartitionTable(),
    ]);
    if (currentResult.status === "fulfilled") {
      verifiedCurrentRelease = currentResult.value;
      append(`Latest firmware verified: ${verifiedCurrentRelease.manifest.release_id}; package and ESP32-S3 application signatures/hashes passed.`);
    } else {
      append(`Latest firmware unavailable: ${errorMessage(currentResult.reason)}`);
    }
    if (legacyResult.status === "fulfilled") {
      verifiedLegacyRelease = legacyResult.value;
      append(`Legacy recovery verified: ${verifiedLegacyRelease.manifest.release_id}; all seven signed write images passed.`);
    } else {
      append(`Legacy recovery unavailable: ${errorMessage(legacyResult.reason)}`);
    }
    if (factoryResult.status === "fulfilled") {
      factoryInitializerReady = true;
      append("New-board initializer verified: the current partition table passed its exact hash check.");
    } else {
      append(`New-board initializer unavailable: ${errorMessage(factoryResult.reason)}`);
    }
    if (!verifiedCurrentRelease && !verifiedLegacyRelease) {
      throw new Error("no signed firmware release passed verification");
    }
    releaseDetail.textContent = verifiedCurrentRelease
      ? `Latest Kitsu ${verifiedCurrentRelease.manifest.firmware_version} is signed and byte-verified.${verifiedLegacyRelease && factoryInitializerReady ? " Factory-new Heltec initialization is ready." : ""}${verifiedLegacyRelease ? ` Legacy recovery ${verifiedLegacyRelease.manifest.firmware_version} is also available.` : ""}`
      : `Latest firmware is unavailable. Legacy recovery ${verifiedLegacyRelease.manifest.firmware_version} is verified.`;
  } catch (error) {
    releaseDetail.textContent = "No installable firmware passed the signature, image, and hash checks.";
    append(`Release check stopped: ${errorMessage(error)}`);
  } finally {
    busy = false;
    updateControls();
  }
}

async function readRegionDigest(offset, bytes, label, start, end) {
  const data = await loader.readFlash(offset, bytes, (_packet, received, total) => {
    const ratio = total === 0 ? 0 : Math.min(1, received / total);
    setProgress(start + ratio * (end - start), `Reading ${label}: ${received.toLocaleString()} / ${total.toLocaleString()} bytes`);
  });
  if (data.byteLength !== bytes) throw new Error(`${label} read was incomplete`);
  return sha256Hex(data);
}

function otaRegion(layout) {
  return layout.kind === "migrated"
    ? { offset: CURRENT_FLASH_PLAN.otaDataOffset, bytes: CURRENT_FLASH_PLAN.otaDataBytes }
    : LEGACY_OTA_DATA;
}

async function capturePreservedRegions(layout, start = 2, end = 18) {
  const ota = otaRegion(layout);
  const middle = start + (end - start) * 0.15;
  const otaSha256 = await readRegionDigest(ota.offset, ota.bytes, "OTA metadata", start, middle);
  const packSha256 = await readRegionDigest(
    COMPANION_PACK.offset,
    COMPANION_PACK.bytes,
    "custom companion pack",
    middle,
    end,
  );
  append(`Preservation baseline captured: OTA metadata ${otaSha256}; companion pack ${packSha256}.`);
  return Object.freeze({ otaSha256, packSha256 });
}

async function verifyPreservedRegions(layout, baseline, start = 82, end = 98) {
  const ota = otaRegion(layout);
  const middle = start + (end - start) * 0.15;
  const otaSha256 = await readRegionDigest(ota.offset, ota.bytes, "preserved OTA metadata", start, middle);
  const packSha256 = await readRegionDigest(
    COMPANION_PACK.offset,
    COMPANION_PACK.bytes,
    "preserved custom companion pack",
    middle,
    end,
  );
  if (otaSha256 !== baseline.otaSha256) throw new Error("OTA metadata changed during firmware install");
  if (packSha256 !== baseline.packSha256) throw new Error("custom companion-pack bytes changed during firmware install");
  append("Preservation verified: OTA metadata and the complete custom companion-pack region are byte-for-byte unchanged.");
}

async function verifyArtifactReadback(artifact, start, end) {
  const readback = await loader.readFlash(
    artifact.record.offset,
    artifact.record.bytes,
    (_packet, received, total) => {
      const ratio = total === 0 ? 0 : Math.min(1, received / total);
      setProgress(start + ratio * (end - start), `Reading back ${artifact.record.role}: ${received.toLocaleString()} / ${total.toLocaleString()} bytes`);
    },
  );
  if (readback.byteLength !== artifact.record.bytes) {
    throw new Error(`${artifact.record.role} readback length is wrong`);
  }
  if (await sha256Hex(readback) !== artifact.record.sha256) {
    throw new Error(`${artifact.record.role} readback SHA-256 is wrong`);
  }
  append(`Readback verified: ${artifact.record.role} at 0x${artifact.record.offset.toString(16).padStart(6, "0")}.`);
}

async function writeCompanionArtifact(artifact, start, end) {
  const midpoint = start + (end - start) * 0.58;
  await loader.writeFlash({
    fileArray: [{ data: artifact.bytes, address: artifact.record.offset }],
    flashMode: "keep",
    flashFreq: "keep",
    flashSize: "keep",
    eraseAll: false,
    compress: true,
    reportProgress(_fileIndex, written, total) {
      const ratio = total === 0 ? 0 : Math.min(1, written / total);
      setProgress(
        start + ratio * (midpoint - start),
        `Writing ${artifact.record.role}: ${written.toLocaleString()} / ${total.toLocaleString()} transfer bytes`,
      );
    },
  });
  await verifyArtifactReadback(artifact, midpoint, end);
}

async function replacementArtifact(role, offset, bytes) {
  return Object.freeze({
    record: Object.freeze({ role, offset, bytes: bytes.byteLength, sha256: await sha256Hex(bytes) }),
    bytes,
  });
}

function installedPackMatches(pack, expected) {
  return pack?.status === "valid"
    && pack.packId === expected.definition.packId
    && pack.revision === expected.definition.revision
    && pack.bytes === expected.record.bytes
    && pack.sha256 === expected.record.sha256;
}

function sameTransition(left, right) {
  return left.destructive === right.destructive
    && left.repair === right.repair
    && left.retry === right.retry
    && left.sourcePackId === right.sourcePackId;
}

async function installCompanion() {
  if (busy || !loader || packSelect.value === "preserve" || !packMatchesSelection(packSelect.value)) return;
  const selectedId = packSelect.value;
  const selected = verifiedPack;
  busy = true;
  updateControls();
  setProgress(0, "Rechecking the selected companion and device");
  let writeStarted = false;
  let completeVerified = false;
  let resetAttempted = false;
  let targetName = selected.definition.name;
  try {
    if (loader.chip?.CHIP_NAME !== "ESP32-S3" || detectedFlashSize !== CURRENT_FLASH_PLAN.flashSize) {
      throw new Error("device identity check is no longer valid");
    }
    if (selectedId !== packSelect.value || selected !== verifiedPack) {
      throw new Error("companion selection changed; choose it again");
    }
    const target = await selectedPackForWrite();
    await reverifyPack(target);
    targetName = target.definition.name;
    const layout = await inspectInstalledFlashLayout(loader);
    if (layout.kind !== "migrated" || layout.sha256 !== installedFlashLayout.sha256) {
      throw new Error("current partition layout changed after connection; reconnect before installing the companion");
    }
    const current = await inspectInstalledPack(loader);
    const transaction = await inspectReplacementTransaction(loader);
    const transition = companionPackTransition(current, target, transaction);

    let title;
    let consequence;
    if (transition.retry) {
      title = `Finish installing ${targetName}?`;
      consequence = "An earlier replacement was interrupted. This retries only the exact pack saved in that replacement record.";
    } else if (transition.destructive) {
      title = `Replace ${current.name} with ${targetName}?`;
      consequence = "Changing to a different companion resets that pet's care and bond progress on the next boot.";
    } else if (transition.repair) {
      title = `Repair the companion slot with ${targetName}?`;
      consequence = "The current slot is unreadable. This replaces only its companion bytes.";
    } else if (current.status === "empty") {
      title = `Install ${targetName} as the starter companion?`;
      consequence = "This is the first companion assignment and does not erase firmware or settings.";
    } else {
      title = `Update the installed ${targetName} pack?`;
      consequence = "The companion identity stays the same, so its existing care and bond progress is retained.";
    }
    const confirmed = window.confirm(
      `${title}\n\n${consequence} `
      + `Only the verified pack at 0x${PACK_SLOT.offset.toString(16)}${transition.destructive || transition.retry ? " and its two fixed replacement-record sectors" : ""} will be written and read back. `
      + "Firmware, partition table, NVS, OTA data, private journals, and the other flash regions are not written.",
    );
    if (!confirmed) {
      append("Companion install cancelled. Nothing was written.");
      return;
    }

    const finalLayout = await inspectInstalledFlashLayout(loader);
    if (finalLayout.kind !== "migrated" || finalLayout.sha256 !== layout.sha256) {
      throw new Error("partition layout changed before the first companion write; nothing was written");
    }
    const finalCurrent = await inspectInstalledPack(loader);
    const finalTransaction = await inspectReplacementTransaction(loader);
    const finalTransition = companionPackTransition(finalCurrent, target, finalTransaction);
    if (!sameTransition(transition, finalTransition)) {
      throw new Error("installed companion state changed before the first write; reconnect and review it again");
    }

    let intent;
    if (finalTransition.destructive) {
      intent = finalTransition.retry
        ? finalTransaction.preparedBytes
        : buildReplacementIntent(finalTransition.sourcePackId, target);
      if (!finalTransition.retry) {
        const prepared = await replacementArtifact(
          "companion_replacement_prepared",
          REPLACEMENT_TRANSACTION.prepared.offset,
          intent,
        );
        append(`Authorizing the one-time replacement from ${finalCurrent.name} to ${targetName}.`);
        writeStarted = true;
        await writeCompanionArtifact(prepared, 4, 24);
      } else {
        append(`Retrying the exact interrupted ${targetName} replacement.`);
      }
    }

    writeStarted = true;
    await writeCompanionArtifact(target, finalTransition.destructive ? 24 : 8, finalTransition.destructive ? 78 : 92);
    const installedTarget = await inspectInstalledPack(loader);
    if (!installedPackMatches(installedTarget, target)) {
      throw new Error("installed companion does not match the selected pack after readback");
    }

    if (finalTransition.destructive) {
      const committed = await replacementArtifact(
        "companion_replacement_committed",
        REPLACEMENT_TRANSACTION.committed.offset,
        intent,
      );
      await writeCompanionArtifact(committed, 78, 96);
      const completeTransaction = await inspectReplacementTransaction(loader);
      if (completeTransaction.status !== "committed") {
        throw new Error("companion replacement commit did not verify");
      }
    }
    const postLayout = await inspectInstalledFlashLayout(loader);
    if (postLayout.kind !== "migrated" || postLayout.sha256 !== finalLayout.sha256) {
      throw new Error("partition table changed during companion installation");
    }

    completeVerified = true;
    setProgress(99, `${targetName} verified; resetting once`);
    resetAttempted = true;
    await loader.after("hard_reset");
    setProgress(100, `${targetName} installed`);
    append(`${targetName} installed and read back exactly. The Heltec was reset once to load it.`);
    await closeTransport({ reset: false, announce: true });
  } catch (error) {
    if (resetAttempted && completeVerified) {
      setProgress(progress.value, "Companion verified; automatic reset could not be confirmed");
      append(`${targetName} passed readback, but reset could not be confirmed: ${errorMessage(error)}. Press RST once.`);
      await closeTransport({ reset: false, announce: true });
    } else if (writeStarted) {
      setProgress(progress.value, "Companion install stopped; Heltec left in ROM loader");
      append(`Companion install stopped after writing began: ${errorMessage(error)}. Reconnect and select the same pack to retry; no automatic reset was attempted.`);
      await closeTransport({ reset: false, announce: true });
    } else {
      setProgress(progress.value, "Companion install stopped before writing");
      append(`Companion install stopped before any write: ${errorMessage(error)}.`);
      await closeTransport({ reset: true, announce: true });
    }
  } finally {
    busy = false;
    updateControls();
  }
}

async function factoryArtifact(role, offset, bytes) {
  return Object.freeze({
    record: Object.freeze({ role, offset, bytes: bytes.byteLength, sha256: await sha256Hex(bytes) }),
    bytes,
  });
}

async function buildFactoryArtifacts(initialization, bootloader) {
  const slotSha256 = await sha256Hex(initialization.applicationSlot);
  const slot = (role, offset) => Object.freeze({
    record: Object.freeze({
      role,
      offset,
      bytes: initialization.applicationSlot.byteLength,
      sha256: slotSha256,
    }),
    bytes: initialization.applicationSlot,
  });
  return [
    bootloader,
    slot("latest_firmware_app1", CURRENT_FLASH_PLAN.app1Offset),
    await factoryArtifact("factory_nvs_reset", FACTORY_INIT_PLAN.nvsOffset, initialization.nvs),
    await factoryArtifact("factory_ota_selection", CURRENT_FLASH_PLAN.otaDataOffset, initialization.otaData),
    await factoryArtifact("factory_lower_gap_clear", FACTORY_INIT_PLAN.lowerGapOffset, initialization.lowerGap),
    slot("latest_firmware_app0", CURRENT_FLASH_PLAN.app0Offset),
    await factoryArtifact("factory_upper_gap_clear", FACTORY_INIT_PLAN.upperGapOffset, initialization.upperGap),
    await factoryArtifact(
      "factory_connectivity_reset",
      FACTORY_INIT_PLAN.connectivityOffset,
      initialization.connectivity,
    ),
    await factoryArtifact("factory_coredump_reset", FACTORY_INIT_PLAN.coredumpOffset, initialization.coredump),
  ];
}

async function writeFactoryArtifact(artifact, start, end) {
  const midpoint = start + (end - start) * 0.55;
  await loader.writeFlash({
    fileArray: [{ data: artifact.bytes, address: artifact.record.offset }],
    flashMode: "dio",
    flashFreq: "80m",
    flashSize: "8MB",
    eraseAll: false,
    compress: true,
    reportProgress(_fileIndex, written, total) {
      const ratio = total === 0 ? 0 : Math.min(1, written / total);
      setProgress(
        start + ratio * (midpoint - start),
        `Writing ${artifact.record.role}: ${written.toLocaleString()} / ${total.toLocaleString()} transfer bytes`,
      );
    },
  });
  await verifyArtifactReadback(artifact, midpoint, end);
}

async function readFactoryPackDigest(start, end, label) {
  return readRegionDigest(
    CURRENT_FLASH_PLAN.companionPackOffset,
    CURRENT_FLASH_PLAN.companionPackBytes,
    label,
    start,
    end,
  );
}

function sameOtaSelection(left, right) {
  return left?.label === right?.label
    && left?.offset === right?.offset
    && left?.sequence === right?.sequence
    && left?.state === right?.state
    && left?.sha256 === right?.sha256;
}

async function installCurrent() {
  if (!verifiedCurrentRelease) return;
  busy = true;
  updateControls();
  setProgress(0, "Rechecking signed latest firmware");
  let writeStarted = false;
  let completeVerified = false;
  let resetAttempted = false;
  try {
    if (loader.chip?.CHIP_NAME !== "ESP32-S3" || detectedFlashSize !== CURRENT_FLASH_PLAN.flashSize) {
      throw new Error("device identity check is no longer valid");
    }
    const latest = await fetchVerifiedCurrentRelease();
    if (latest.packageSha256 !== verifiedCurrentRelease.packageSha256) {
      append(`The signed latest package changed from ${verifiedCurrentRelease.manifest.release_id} to ${latest.manifest.release_id}; the new verified package will be used.`);
    }
    verifiedCurrentRelease = latest;
    const layout = await inspectInstalledFlashLayout(loader);
    if (layout.kind !== "migrated" || layout.sha256 !== installedFlashLayout.sha256) {
      throw new Error("current partition layout changed after connection; reconnect before installing");
    }
    const selection = await inspectCurrentOtaSelection(loader);
    if (!sameOtaSelection(installedOtaSelection, selection)) {
      throw new Error("OTA boot selection changed after connection; reconnect before installing");
    }
    const confirmed = window.confirm(
      `Install signed Kitsu ${latest.manifest.firmware_version} to ${selection.label} now?\n\n`
      + `Only the ${selection.label} application at 0x${selection.offset.toString(16).padStart(6, "0")} will be written and read back. `
      + "The page never performs a full-chip erase and never writes the partition table, bootloader, NVS, OTA metadata, other application slot, private journals, custom companion pack, connectivity data, or coredump.",
    );
    if (!confirmed) {
      append("Install cancelled. Nothing was written.");
      return;
    }

    const finalLayout = await inspectInstalledFlashLayout(loader);
    const finalSelection = await inspectCurrentOtaSelection(loader);
    if (finalLayout.kind !== "migrated" || finalLayout.sha256 !== layout.sha256
      || !sameOtaSelection(selection, finalSelection)) {
      throw new Error("layout or OTA selection changed before the first write; nothing was written");
    }
    const preserved = await capturePreservedRegions(finalLayout);
    const artifact = Object.freeze({
      record: Object.freeze({
        role: `latest_firmware_${selection.label}`,
        offset: selection.offset,
        bytes: latest.image.byteLength,
        sha256: latest.imageDigest,
      }),
      bytes: latest.image,
    });
    setProgress(18, `Writing signed Kitsu ${latest.manifest.firmware_version} to ${selection.label}`);
    append(`Writing one bounded application image to ${selection.label}; no companion-pack write exists in this plan.`);
    writeStarted = true;
    await loader.writeFlash({
      fileArray: [{ data: artifact.bytes, address: artifact.record.offset }],
      flashMode: "keep",
      flashFreq: "keep",
      flashSize: "keep",
      eraseAll: false,
      compress: true,
      reportProgress(_fileIndex, written, total) {
        const ratio = total === 0 ? 0 : Math.min(1, written / total);
        setProgress(18 + ratio * 42, `Writing ${selection.label}: ${written.toLocaleString()} / ${total.toLocaleString()} transfer bytes`);
      },
    });
    await verifyArtifactReadback(artifact, 60, 82);
    await verifyPreservedRegions(finalLayout, preserved);
    const postLayout = await inspectInstalledFlashLayout(loader);
    if (postLayout.kind !== "migrated" || postLayout.sha256 !== finalLayout.sha256) {
      throw new Error("partition table changed during firmware install");
    }
    completeVerified = true;
    setProgress(99, "Application and preserved regions verified; resetting once");
    resetAttempted = true;
    await loader.after("hard_reset");
    setProgress(100, `Kitsu ${latest.manifest.firmware_version} installed`);
    append(`Kitsu ${latest.manifest.firmware_version} installed to ${selection.label}. Application readback, OTA metadata, and custom pack verification all passed.`);
    await closeTransport({ reset: false, announce: true });
  } catch (error) {
    if (resetAttempted && completeVerified) {
      setProgress(progress.value, "Install verified; automatic reset could not be confirmed");
      append(`The application and preserved regions passed, but reset could not be confirmed: ${errorMessage(error)}. Press RST once.`);
      await closeTransport({ reset: false, announce: true });
    } else if (writeStarted) {
      setProgress(progress.value, "Install stopped; Heltec left in ROM loader");
      append(`Install stopped after writing began: ${errorMessage(error)}. The Heltec is left in the ROM loader for a clean retry; no automatic reset was attempted.`);
      await closeTransport({ reset: false, announce: true });
    } else {
      setProgress(progress.value, "Install stopped before writing");
      append(`Install stopped before any write: ${errorMessage(error)}.`);
      await closeTransport({ reset: true, announce: true });
    }
  } finally {
    busy = false;
    updateControls();
  }
}

async function installFactory() {
  if (!verifiedCurrentRelease || !verifiedLegacyRelease || !factoryInitializerReady) return;
  const selectedPackId = packSelect.value;
  const packRequested = selectedPackId !== "preserve";
  const selectedPack = verifiedPack;
  if (packRequested && !packMatchesSelection(selectedPackId, selectedPack)) return;
  busy = true;
  updateControls();
  setProgress(0, "Rechecking the signed firmware and new-board initializer");
  let writeStarted = false;
  let completeVerified = false;
  let resetAttempted = false;
  try {
    if (loader.chip?.CHIP_NAME !== "ESP32-S3" || detectedFlashSize !== CURRENT_FLASH_PLAN.flashSize) {
      throw new Error("device identity check is no longer valid");
    }
    if (selectedPackId !== packSelect.value || selectedPack !== verifiedPack) {
      throw new Error("companion selection changed; choose it again");
    }
    const [latest, legacy, starterPack] = await Promise.all([
      fetchVerifiedCurrentRelease(),
      fetchVerifiedRelease(),
      packRequested ? selectedPackForWrite() : Promise.resolve(null),
    ]);
    await reverifyArtifacts(legacy);
    if (starterPack) await reverifyPack(starterPack);
    const bootloader = legacy.artifacts.find((artifact) => artifact.record.role === "bootloader");
    if (!bootloader) throw new Error("signed Kitsu bootloader is unavailable");
    const initialization = await prepareFactoryInitialization(latest.image);
    const artifacts = await buildFactoryArtifacts(initialization, bootloader);
    const partitionTable = await factoryArtifact(
      "current_partition_table_commit_last",
      0x008000,
      initialization.partitionTable,
    );
    verifiedCurrentRelease = latest;
    verifiedLegacyRelease = legacy;

    const layout = await inspectInstalledFlashLayout(loader);
    if (layout.kind !== "factory" || layout.sha256 !== installedFlashLayout.sha256) {
      throw new Error("factory partition layout changed after connection; reconnect before installing");
    }
    const confirmed = window.confirm(
      `Initialize this stock Heltec with signed Kitsu ${latest.manifest.firmware_version}${starterPack ? ` and ${starterPack.definition.name}` : ""}?\n\n`
      + "This replaces the stock firmware, partition layout, NVS settings, connectivity state, and coredump. "
      + (starterPack
        ? `The verified ${starterPack.definition.name} pack will be written only to the dedicated companion region and loaded as the starter. `
        : "The companion-pack region will not be written. ")
      + "It does not erase the whole chip. Every selected region is read back before the partition table is committed last.",
    );
    if (!confirmed) {
      append("New-board initialization cancelled. Nothing was written.");
      return;
    }

    const finalFactoryLayout = await inspectInstalledFlashLayout(loader);
    if (
      finalFactoryLayout.kind !== "factory"
      || finalFactoryLayout.sha256 !== FACTORY_INIT_PLAN.sourcePartitionSha256
      || finalFactoryLayout.sha256 !== layout.sha256
    ) {
      throw new Error("factory layout changed before the first write; nothing was written");
    }
    const packBaseline = await readFactoryPackDigest(2, 10, "custom companion pack baseline");
    append(`Custom companion-pack baseline captured: ${packBaseline}.`);
    append(`Initializing the stock Heltec directly with signed Kitsu ${latest.manifest.firmware_version}${starterPack ? ` and ${starterPack.definition.name}` : ""}; the partition table will be committed last.`);

    writeStarted = true;
    const totalBytes = artifacts.reduce((sum, artifact) => sum + artifact.record.bytes, 0);
    let completedBytes = 0;
    for (const artifact of artifacts) {
      const start = 10 + (completedBytes / totalBytes) * (starterPack ? 64 : 76);
      completedBytes += artifact.record.bytes;
      const end = 10 + (completedBytes / totalBytes) * (starterPack ? 64 : 76);
      await writeFactoryArtifact(artifact, start, end);
    }

    if (starterPack) {
      append(`All core initialization writes passed. Writing ${starterPack.definition.name} to the dedicated companion region.`);
      await writeCompanionArtifact(starterPack, 74, 88);
      const installedStarter = await inspectInstalledPack(loader);
      if (!installedPackMatches(installedStarter, starterPack)) {
        throw new Error("starter companion did not match the selected pack after readback");
      }
    }

    const precommitLayout = await inspectInstalledFlashLayout(loader);
    if (precommitLayout.kind !== "factory" || precommitLayout.sha256 !== layout.sha256) {
      throw new Error("factory partition table changed before its final commit");
    }
    if (starterPack) {
      const precommitStarter = await inspectInstalledPack(loader);
      if (!installedPackMatches(precommitStarter, starterPack)) {
        throw new Error("starter companion changed before partition-table commit");
      }
    } else {
      const precommitPack = await readFactoryPackDigest(86, 90, "custom companion pack before commit");
      if (precommitPack !== packBaseline) {
        throw new Error("custom companion-pack bytes changed before partition-table commit");
      }
    }

    append("All initialization writes passed readback. Committing the current partition table as the final flash mutation.");
    await writeFactoryArtifact(partitionTable, 90, 95);
    const migratedLayout = await inspectInstalledFlashLayout(loader);
    if (
      migratedLayout.kind !== "migrated"
      || migratedLayout.sha256 !== FACTORY_INIT_PLAN.targetPartitionSha256
    ) {
      throw new Error("current partition table did not pass final verification");
    }
    const selection = await inspectCurrentOtaSelection(loader);
    if (selection.label !== "app0" || selection.sequence !== 1) {
      throw new Error("new-board OTA selection did not resolve to app0");
    }
    if (starterPack) {
      const finalStarter = await inspectInstalledPack(loader);
      if (!installedPackMatches(finalStarter, starterPack)) {
        throw new Error("starter companion changed during new-board initialization");
      }
    } else {
      const finalPack = await readFactoryPackDigest(95, 99, "preserved custom companion pack");
      if (finalPack !== packBaseline) {
        throw new Error("custom companion-pack bytes changed during new-board initialization");
      }
    }

    completeVerified = true;
    installedFlashLayout = migratedLayout;
    installedOtaSelection = selection;
    setProgress(99, "Initialization verified; resetting once");
    resetAttempted = true;
    await loader.after("hard_reset");
    setProgress(100, `Kitsu ${latest.manifest.firmware_version} initialized`);
    append(`New Heltec initialized with Kitsu ${latest.manifest.firmware_version}${starterPack ? ` and starter ${starterPack.definition.name}` : ""}. Both application slots, current layout, and ${starterPack ? "starter companion" : "custom-pack preservation"} passed readback.`);
    await closeTransport({ reset: false, announce: true });
  } catch (error) {
    if (resetAttempted && completeVerified) {
      setProgress(progress.value, "Initialization verified; automatic reset could not be confirmed");
      append(`Initialization and preservation passed, but reset could not be confirmed: ${errorMessage(error)}. Press RST once.`);
      await closeTransport({ reset: false, announce: true });
    } else if (writeStarted) {
      setProgress(progress.value, "Initialization stopped; Heltec left in ROM loader");
      append(`New-board initialization stopped after writing began: ${errorMessage(error)}. Reconnect here and retry; no automatic reset was attempted.`);
      await closeTransport({ reset: false, announce: true });
    } else {
      setProgress(progress.value, "Initialization stopped before writing");
      append(`New-board initialization stopped before any write: ${errorMessage(error)}.`);
      await closeTransport({ reset: true, announce: true });
    }
  } finally {
    busy = false;
    updateControls();
  }
}

async function installLegacy() {
  if (!verifiedLegacyRelease) return;
  busy = true;
  updateControls();
  setProgress(0, "Rechecking signed legacy recovery");
  let writeStarted = false;
  let completeVerified = false;
  let resetAttempted = false;
  try {
    if (loader.chip?.CHIP_NAME !== "ESP32-S3" || detectedFlashSize !== FLASH_PLAN.flashSize) {
      throw new Error("device identity check is no longer valid");
    }
    const latest = await fetchVerifiedRelease();
    verifiedLegacyRelease = latest;
    await reverifyArtifacts(latest);
    const layout = await inspectInstalledFlashLayout(loader);
    if (layout.kind !== "legacy" || layout.sha256 !== installedFlashLayout.sha256) {
      throw new Error("legacy partition layout changed after connection; reconnect before installing");
    }
    const confirmed = window.confirm(
      `Install signed legacy recovery ${latest.manifest.firmware_version} now?\n\n`
      + "This performs the seven reviewed recovery writes and reads each one back. It never performs a full-chip erase. OTA metadata and the complete custom companion-pack region are hashed before and after and must remain identical.",
    );
    if (!confirmed) {
      append("Install cancelled. Nothing was written.");
      return;
    }
    const finalLayout = await inspectInstalledFlashLayout(loader);
    if (finalLayout.kind !== "legacy" || finalLayout.sha256 !== layout.sha256) {
      throw new Error("partition layout changed before the first write; nothing was written");
    }
    const preserved = await capturePreservedRegions(finalLayout);
    const artifacts = latest.artifacts;
    const totalWriteBytes = artifacts.reduce((total, artifact) => total + artifact.record.bytes, 0);
    const precedingWriteBytes = artifacts.map((_, index) => artifacts
      .slice(0, index)
      .reduce((total, artifact) => total + artifact.record.bytes, 0));
    writeStarted = true;
    await loader.writeFlash({
      fileArray: artifacts.map((artifact) => ({
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
        const completed = precedingWriteBytes[fileIndex] + artifacts[fileIndex].record.bytes * ratio;
        setProgress(18 + (completed / totalWriteBytes) * 42, `Writing ${artifacts[fileIndex].record.role}: ${written.toLocaleString()} / ${total.toLocaleString()} transfer bytes`);
      },
    });
    for (const [index, artifact] of artifacts.entries()) {
      const start = 60 + (index / artifacts.length) * 22;
      const end = 60 + ((index + 1) / artifacts.length) * 22;
      await verifyArtifactReadback(artifact, start, end);
    }
    await verifyPreservedRegions(finalLayout, preserved);
    completeVerified = true;
    setProgress(99, "Recovery and preserved regions verified; resetting once");
    resetAttempted = true;
    await loader.after("hard_reset");
    setProgress(100, `Legacy recovery ${latest.manifest.firmware_version} installed`);
    append(`Legacy recovery ${latest.manifest.firmware_version} installed. All seven write readbacks plus OTA metadata and custom pack verification passed.`);
    await closeTransport({ reset: false, announce: true });
  } catch (error) {
    if (resetAttempted && completeVerified) {
      setProgress(progress.value, "Recovery verified; automatic reset could not be confirmed");
      append(`Every write and preserved region passed, but reset could not be confirmed: ${errorMessage(error)}. Press RST once.`);
      await closeTransport({ reset: false, announce: true });
    } else if (writeStarted) {
      setProgress(progress.value, "Recovery stopped; Heltec left in ROM loader");
      append(`Recovery stopped after writing began: ${errorMessage(error)}. The Heltec is left in the ROM loader for a clean retry; no automatic reset was attempted.`);
      await closeTransport({ reset: false, announce: true });
    } else {
      setProgress(progress.value, "Recovery stopped before writing");
      append(`Recovery stopped before any write: ${errorMessage(error)}.`);
      await closeTransport({ reset: true, announce: true });
    }
  } finally {
    busy = false;
    updateControls();
  }
}

async function install() {
  if (busy || !loader) return;
  if (installedFlashLayout?.kind === "migrated") await installCurrent();
  else if (installedFlashLayout?.kind === "legacy") await installLegacy();
  else if (installedFlashLayout?.kind === "factory") await installFactory();
}

if (serialSupported) {
  browserDetail.textContent = "Web Serial is available. Device access starts only after Connect Heltec is clicked.";
  connectButton.addEventListener("click", () => { void connect(); });
  disconnectButton.addEventListener("click", () => { void closeTransport(); });
} else {
  browserDetail.textContent = "Web Serial is unavailable. Use current desktop Chrome or Edge over HTTPS.";
  connectButton.disabled = true;
}

refreshButton.addEventListener("click", () => { void checkRelease(); });
packSelect.addEventListener("change", () => {
  verifiedPack = undefined;
  unlockedPackInput.removeAttribute("aria-invalid");
  if (packSelect.value === UNLOCKED_PACK_ID) {
    unlockedPackInput.value = "";
    packDetail.textContent = "Choose a .k868 companion pack from this computer. It is read locally and must pass every format and integrity check.";
    append("Companion selection: waiting for a local .k868 file. Nothing has been accepted or written.");
    updateControls();
    return;
  }
  unlockedPackInput.value = "";
  void checkSelectedPack();
});
unlockedPackInput.addEventListener("change", () => {
  verifiedPack = undefined;
  void checkSelectedPack();
});
installButton.addEventListener("click", () => { void install(); });
installPackButton.addEventListener("click", () => { void installCompanion(); });
window.addEventListener("pagehide", () => { void closeTransport({ reset: false, announce: false }); });
void checkRelease();
