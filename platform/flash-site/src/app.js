import { ESPLoader, Transport } from "esptool-js";
import {
  fetchVerifiedRelease,
  FLASH_PLAN,
  reverifyArtifacts,
  sha256Hex,
} from "./release.js";
import { fetchOfficialPack, packDefinition, reverifyPack } from "./packs.js";
import "./styles.css";

const connectButton = document.querySelector("#connect");
const disconnectButton = document.querySelector("#disconnect");
const installButton = document.querySelector("#install");
const packSelect = document.querySelector("#pack-select");
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
let busy = false;
const serialSupported = "serial" in navigator && window.isSecureContext;

function errorMessage(error) {
  return error instanceof Error ? error.message : String(error);
}

function append(message) {
  log.textContent += `\n${new Date().toLocaleTimeString()}  ${message}`;
  log.scrollTop = log.scrollHeight;
}

function updateControls() {
  const connected = Boolean(loader)
    && loader.chip?.CHIP_NAME === "ESP32-S3"
    && detectedFlashSize === FLASH_PLAN.flashSize;
  connectButton.disabled = !serialSupported || busy || Boolean(transport);
  disconnectButton.disabled = busy || !transport;
  refreshButton.disabled = busy;
  packSelect.disabled = busy;
  const packReady = packSelect.value === "preserve"
    || verifiedPack?.definition.id === packSelect.value;
  installButton.disabled = busy || !connected || !verifiedRelease || !packReady;
  if (busy) installButton.textContent = "Install in progress";
  else if (connected && verifiedRelease && packReady) {
    const pack = packDefinition(packSelect.value);
    installButton.textContent = pack
      ? `Install Kitsu + ${pack.name}`
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
    browserDetail.textContent = `${detectedChip} with ${detectedFlashSize} flash verified through Espressif ROM loader.`;
    append(`Device gate passed: ${detectedChip}; chip family ESP32-S3; flash ${detectedFlashSize}.`);
  } catch (error) {
    if (error instanceof DOMException && error.name === "NotFoundError") {
      append("Port selection was cancelled. Nothing was written.");
    } else {
      append(`Device gate closed: ${errorMessage(error)}. Nothing was written.`);
    }
    await closeTransport({ reset: true, announce: false });
  } finally {
    busy = false;
    updateControls();
  }
}

async function loadSelectedPack() {
  const packId = packSelect.value;
  verifiedPack = undefined;
  if (packId === "preserve") {
    packDetail.textContent = "Keep current pet selected. The companion-pack slot will not be written.";
    return null;
  }
  const pack = await fetchOfficialPack(packId);
  verifiedPack = pack;
  packDetail.textContent = `${pack.definition.name} is ready: exact official bundle and SHA-256 verified. Installing a different pet resets that pet's care and bond progress.`;
  return pack;
}

async function checkSelectedPack() {
  busy = true;
  updateControls();
  try {
    const pack = await loadSelectedPack();
    if (pack) append(`Pet gate passed: official ${pack.definition.name} bundle verified for the dedicated slot at 0x670000.`);
    else append("Pet choice: preserve the current companion-pack slot.");
  } catch (error) {
    packDetail.textContent = "The selected pet bundle did not pass its exact size and SHA-256 gates.";
    append(`Pet gate closed: ${errorMessage(error)}`);
  } finally {
    busy = false;
    updateControls();
  }
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
      loadSelectedPack(),
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
  const selectedDefinition = packDefinition(selectedPackId);
  if (selectedDefinition && verifiedPack?.definition.id !== selectedPackId) return;
  const packPlan = selectedDefinition
    ? `After the seven core regions pass readback, the same USB session writes the official ${selectedDefinition.name} pet to the dedicated companion slot at 0x670000 and reads it back before one final reset. Replacing a different species starts that companion fresh. `
    : "The current companion-pack slot and pet progress stay untouched. ";
  const confirmed = window.confirm(
    `${selectedDefinition ? `Install Kitsu and ${selectedDefinition.name}` : "Install Kitsu"} now?\n\n`
    + "The installer will check the update service again immediately before writing. "
    + "This writes the reviewed rollback-enabled bootloader, partition table, the same application in app0 and app1, an empty OTA journal at the end of each application slot, and an exact clear image over the retired connectivity partition. Every region is read back. "
    + packPlan
    + "It does not call full-chip erase or write OTA data, companion state, controller records, MeshCore state, coredump, or eFuses. The local-only firmware also removes only the retired LAN-action NVS namespace after boot.",
  );
  if (!confirmed) {
    append("Install cancelled. Nothing was written.");
    return;
  }

  busy = true;
  updateControls();
  setProgress(0, "Checking the latest signed stable release");
  let coreVerified = false;
  let packVerified = !selectedDefinition;
  let resetAttempted = false;
  try {
    if (loader.chip?.CHIP_NAME !== "ESP32-S3" || detectedFlashSize !== FLASH_PLAN.flashSize) {
      throw new Error("device identity gate is no longer valid");
    }
    const [latestRelease, latestPack] = await Promise.all([
      fetchVerifiedRelease(),
      fetchOfficialPack(selectedPackId),
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
    setProgress(4, "Latest release verified; starting seven bounded writes");
    append(`Install authorized for latest release ${verifiedRelease.manifest.release_id}. Starting the exact seven-write signed core phase; erase-all remains disabled.`);
    if (latestPack) append(`Selected pet phase: official ${latestPack.definition.name}, ${latestPack.record.bytes.toLocaleString()} bytes at 0x670000, exact SHA-256 pinned.`);
    else append("Selected pet phase: preserve the current companion-pack slot without writing it.");

    const totalWriteBytes = verifiedRelease.artifacts.reduce((total, artifact) => total + artifact.record.bytes, 0);
    const precedingWriteBytes = verifiedRelease.artifacts.map((_, index) => verifiedRelease.artifacts
      .slice(0, index)
      .reduce((total, artifact) => total + artifact.record.bytes, 0));
    await loader.writeFlash({
      fileArray: verifiedRelease.artifacts.map((artifact) => ({
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
        const artifact = verifiedRelease.artifacts[fileIndex];
        const completedBytes = precedingWriteBytes[fileIndex] + (artifact.record.bytes * ratio);
        setProgress(4 + (completedBytes / totalWriteBytes) * 54, `Writing signed core ${artifact.record.role}: ${written.toLocaleString()} / ${total.toLocaleString()} transfer bytes`);
      },
    });

    for (const [index, artifact] of verifiedRelease.artifacts.entries()) {
      await verifyReadback(artifact, index, verifiedRelease.artifacts, 58, latestPack ? 88 : 99);
    }
    coreVerified = true;
    append("Signed core phase complete: all seven regions passed SHA-256 readback.");

    if (latestPack) {
      setProgress(88, `Core verified; writing ${latestPack.definition.name} to the companion slot`);
      await reverifyPack(latestPack);
      await loader.writeFlash({
        fileArray: [{ data: latestPack.bytes, address: latestPack.record.offset }],
        flashMode: "dio",
        flashFreq: "80m",
        flashSize: "8MB",
        eraseAll: false,
        compress: true,
        reportProgress(_fileIndex, written, total) {
          const ratio = total === 0 ? 0 : Math.min(1, written / total);
          setProgress(88 + ratio * 6, `Writing ${latestPack.definition.name} pet: ${written.toLocaleString()} / ${total.toLocaleString()} transfer bytes`);
        },
      });
      await verifyReadback(latestPack, 0, [latestPack], 94, 99);
      packVerified = true;
      packDetail.textContent = `${latestPack.definition.name} installed with Kitsu in the same USB session and verified by SHA-256 readback.`;
      append(`${latestPack.definition.name} pet phase complete: dedicated companion slot read back exactly.`);
    }

    setProgress(99, "All selected readback hashes passed; resetting the Heltec once");
    resetAttempted = true;
    await loader.after("hard_reset");
    setProgress(100, "Install and SHA-256 readback complete");
    append(
      `Kitsu ${verifiedRelease.manifest.firmware_version}${latestPack ? ` and ${latestPack.definition.name}` : ""} installed and read back successfully. `
      + "The Heltec was reset once; releasing the serial port.",
    );
    await closeTransport({ reset: false, announce: true });
  } catch (error) {
    if (resetAttempted && coreVerified && packVerified) {
      setProgress(progress.value, "Installation verified; automatic reset could not be confirmed");
      append(`Every selected region passed SHA-256 readback, but the final automatic reset could not be confirmed: ${errorMessage(error)}. Press RST once on the Heltec. No second automatic reset was attempted.`);
    } else if (coreVerified && selectedDefinition && !packVerified) {
      setProgress(progress.value, `${selectedDefinition.name} install stopped after the signed core passed`);
      append(`Signed core firmware is verified, but the ${selectedDefinition.name} pet phase failed closed: ${errorMessage(error)}. Kitsu may show NO PACK until the pet phase succeeds.`);
    } else {
      setProgress(progress.value, "Install stopped; no automatic retry was attempted");
      append(`Install failed closed: ${errorMessage(error)}. Inspect the log and reconnect before retrying.`);
    }
    await closeTransport({ reset: !resetAttempted, announce: true });
  } finally {
    busy = false;
    updateControls();
  }
}

if (serialSupported) {
  browserDetail.textContent = "Web Serial is available in this secure context. Device access starts only after Connect Heltec is clicked.";
  connectButton.addEventListener("click", () => { void connect(); });
  disconnectButton.addEventListener("click", () => { void closeTransport(); });
} else {
  browserDetail.textContent = "Web Serial is unavailable. Use current desktop Chrome or Edge over HTTPS.";
  connectButton.disabled = true;
}

refreshButton.addEventListener("click", () => { void checkRelease(); });
packSelect.addEventListener("change", () => { void checkSelectedPack(); });
installButton.addEventListener("click", () => { void install(); });
window.addEventListener("pagehide", () => { void closeTransport({ reset: false, announce: false }); });
void checkRelease();
