import { CURRENT_FLASH_PLAN } from "./current-release.js";
import { HELTEC_FACTORY_PARTITION_TABLE_SHA256 } from "./layout-gate.js";
import { sha256Hex } from "./release.js";

export const FACTORY_INIT_PLAN = Object.freeze({
  sourcePartitionSha256: HELTEC_FACTORY_PARTITION_TABLE_SHA256,
  targetPartitionUrl:
    "/downloads/kitsu-current-partitions-3337f0ec25e653d8c0bf9534abeb147a7505f41b1c2e25b53bb6cc74d395b532.kitsu-layout",
  targetPartitionBytes: 0x0c00,
  targetPartitionSha256: "3337f0ec25e653d8c0bf9534abeb147a7505f41b1c2e25b53bb6cc74d395b532",
  nvsOffset: 0x009000,
  nvsBytes: 0x040000,
  lowerGapOffset: 0x04b000,
  lowerGapBytes: 0x005000,
  upperGapOffset: 0x650000,
  upperGapBytes: 0x020000,
  connectivityOffset: 0x7b0000,
  connectivityBytes: 0x040000,
  coredumpOffset: 0x7f0000,
  coredumpBytes: 0x010000,
  bootApp0Sha256: "f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0",
});

function fail(message) {
  throw new Error(message);
}

function erased(bytes) {
  return new Uint8Array(bytes).fill(0xff);
}

export function buildFactoryOtaData() {
  const bytes = erased(CURRENT_FLASH_PLAN.otaDataBytes);
  bytes.set([0x01, 0x00, 0x00, 0x00], 0);
  bytes.set([0x9a, 0x98, 0x43, 0x47], 28);
  bytes.set([0x00, 0x00, 0x00, 0x00], 0x1000);
  return bytes;
}

export function buildFactoryApplicationSlot(application) {
  if (!(application instanceof Uint8Array) || application.byteLength < 1) {
    fail("factory application image is missing");
  }
  if (
    application.byteLength
    > CURRENT_FLASH_PLAN.applicationSlotBytes - CURRENT_FLASH_PLAN.otaJournalBytes
  ) {
    fail("factory application overlaps the private OTA journal");
  }
  const slot = erased(CURRENT_FLASH_PLAN.applicationSlotBytes);
  slot.set(application, 0);
  return slot;
}

async function readExact(response, bytes, label) {
  if (!response.ok) fail(`${label} returned HTTP ${response.status}`);
  const declared = response.headers.get("content-length");
  if (declared !== null && (!/^[0-9]+$/.test(declared) || Number(declared) !== bytes)) {
    fail(`${label} response length is wrong`);
  }
  const value = new Uint8Array(await response.arrayBuffer());
  if (value.byteLength !== bytes) fail(`${label} download is incomplete`);
  return value;
}

export async function fetchVerifiedFactoryPartitionTable() {
  const response = await fetch(FACTORY_INIT_PLAN.targetPartitionUrl, {
    cache: "no-store",
    credentials: "omit",
    redirect: "error",
    referrerPolicy: "no-referrer",
    signal: AbortSignal.timeout(20000),
  });
  const bytes = await readExact(
    response,
    FACTORY_INIT_PLAN.targetPartitionBytes,
    "current partition table",
  );
  if (await sha256Hex(bytes) !== FACTORY_INIT_PLAN.targetPartitionSha256) {
    fail("current partition table SHA-256 is wrong");
  }
  return bytes;
}

export async function prepareFactoryInitialization(application) {
  const [partitionTable, otaData] = await Promise.all([
    fetchVerifiedFactoryPartitionTable(),
    Promise.resolve(buildFactoryOtaData()),
  ]);
  if (await sha256Hex(otaData) !== FACTORY_INIT_PLAN.bootApp0Sha256) {
    fail("factory OTA selection bytes are wrong");
  }
  return {
    partitionTable,
    otaData,
    applicationSlot: buildFactoryApplicationSlot(application),
    nvs: erased(FACTORY_INIT_PLAN.nvsBytes),
    lowerGap: erased(FACTORY_INIT_PLAN.lowerGapBytes),
    upperGap: erased(FACTORY_INIT_PLAN.upperGapBytes),
    connectivity: erased(FACTORY_INIT_PLAN.connectivityBytes),
    coredump: erased(FACTORY_INIT_PLAN.coredumpBytes),
  };
}
