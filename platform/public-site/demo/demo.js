export const FIRMWARE_WASM_SHA256 =
  "5fbff5cd63d949d0ba72d6a02e7a2f134697f279a36cf0d7c91471e9b0d337b6";
export const FIRMWARE_WASM_URL =
  "/demo/kitsu-firmware-full." + FIRMWARE_WASM_SHA256 + ".wasm";
export const FOX_PACK_SHA256 =
  "c868386770b6083dcd8f7c01ec7fe455faec476a96c724ab62f09770fdcdab38";
export const FOX_PACK_URL =
  "/demo/assets/fox." + FOX_PACK_SHA256 + ".k868";
export const FIRMWARE_ABI_VERSION = 2;
export const FIRMWARE_FRAMEBUFFER_BYTES = 64 * 128;
export const FOX_PACK_BYTES = 24976;
export const DEFAULT_OLED_TONE = "cyan";
export const OLED_TONES = Object.freeze(["cyan", "mono"]);

const OLED_TONE_STORAGE_KEY = "kitsu-demo-oled-tone-v1";
const INSTALLED_STORAGE_KEY = "kitsu-full-firmware-demo-installed-v1";
const PERSISTENCE_STORAGE_KEY = "kitsu-full-firmware-demo-kps2-v1";
const PEER_PERSISTENCE_STORAGE_KEY =
  "kitsu-full-firmware-demo-peer-kps2-v1";
const DEVICE_ID_STORAGE_KEY = "kitsu-full-firmware-demo-device-id-v1";
const LEGACY_STORAGE_KEYS = Object.freeze([
  "kitsu-fox-demo-v5",
  "kitsu-fox-demo-v4",
  "kitsu-fox-demo-v3",
]);
const REQUIRED_IMPORTS = Object.freeze([
  "wasi_snapshot_preview1.fd_close",
  "wasi_snapshot_preview1.fd_write",
  "wasi_snapshot_preview1.fd_seek",
]);
const REQUIRED_EXPORTS = Object.freeze([
  "_initialize",
  "kitsu_emulator_abi_version",
  "kitsu_emulator_boot",
  "kitsu_emulator_booted",
  "kitsu_emulator_step",
  "kitsu_emulator_set_device_id",
  "kitsu_emulator_entropy_buffer",
  "kitsu_emulator_entropy_capacity",
  "kitsu_emulator_entropy_commit",
  "kitsu_emulator_entropy_ready",
  "kitsu_emulator_set_prg",
  "kitsu_emulator_framebuffer",
  "kitsu_emulator_framebuffer_bytes",
  "kitsu_emulator_framebuffer_width",
  "kitsu_emulator_framebuffer_height",
  "kitsu_emulator_display_on",
  "kitsu_emulator_framebuffer_revision",
  "kitsu_emulator_frame_history_count",
  "kitsu_emulator_frame_history_frame",
  "kitsu_emulator_frame_history_millis",
  "kitsu_emulator_frame_history_display_on",
  "kitsu_emulator_frame_history_clear",
  "kitsu_emulator_pack_buffer",
  "kitsu_emulator_pack_capacity",
  "kitsu_emulator_pack_commit",
  "kitsu_emulator_pack_bytes",
  "kitsu_emulator_serial_buffer",
  "kitsu_emulator_serial_bytes",
  "kitsu_emulator_serial_clear",
  "kitsu_emulator_serial_input_buffer",
  "kitsu_emulator_serial_input_capacity",
  "kitsu_emulator_serial_input_commit",
  "kitsu_emulator_serial_input_queued",
  "kitsu_emulator_persistence_buffer",
  "kitsu_emulator_persistence_capacity",
  "kitsu_emulator_persistence_schema_version",
  "kitsu_emulator_persistence_export",
  "kitsu_emulator_persistence_import",
  "kitsu_emulator_ota_partition_buffer",
  "kitsu_emulator_ota_partition_bytes",
  "kitsu_emulator_ota_active_boot_slot",
  "kitsu_emulator_ota_restore_active_boot_slot",
  "kitsu_emulator_debug_view",
  "kitsu_emulator_debug_view_bytes",
  "kitsu_emulator_ble_rx_chunk_buffer",
  "kitsu_emulator_ble_rx_chunk_capacity",
  "kitsu_emulator_ble_rx_chunk_commit",
  "kitsu_emulator_ble_tx_chunk_buffer",
  "kitsu_emulator_ble_tx_chunk_bytes",
  "kitsu_emulator_ble_tx_chunk_consume",
  "kitsu_emulator_ble_connect",
  "kitsu_emulator_ble_disconnect",
  "kitsu_emulator_ble_status_view",
  "kitsu_emulator_ble_status_view_bytes",
  "kitsu_emulator_radio_io_buffer",
  "kitsu_emulator_radio_io_capacity",
  "kitsu_emulator_radio_inject_rx",
  "kitsu_emulator_radio_tx_count",
  "kitsu_emulator_radio_tx_peek_length",
  "kitsu_emulator_radio_tx_copy",
  "kitsu_emulator_radio_tx_consume",
]);
const SCREEN_NAMES = Object.freeze([
  "Portrait",
  "Menu",
  "Connect",
  "Inbox",
  "Games",
  "Game",
  "Listen",
  "Sleep",
  "Status",
  "Pair phone",
  "Controllers",
  "Confirm controller",
  "Controller result",
]);
const MENU_ITEMS = Object.freeze([
  "CONNECT",
  "FEED",
  "PLAY",
  "GAMES",
  "INBOX",
  "RADIO",
  "SLEEP",
  "INFO",
  "BACK",
]);
const GAME_ITEMS = Object.freeze(["SIGNAL", "POUNCE", "BACK"]);
const CONNECTION_ITEMS = Object.freeze(["BLUETOOTH", "CONTROLLERS", "BACK"]);
const GAME_PHASES = Object.freeze(["Inactive", "Playing", "Result", "Finished"]);
const GAME_RESULTS = Object.freeze([
  "",
  "Perfect",
  "Good",
  "Hit",
  "Caught",
  "Too early",
  "Too late",
  "Miss",
]);
const INK = Object.freeze({
  cyan: Object.freeze([112, 245, 255]),
  mono: Object.freeze([244, 244, 242]),
});
const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8", { fatal: true });

export function normalizeOledTone(value) {
  return OLED_TONES.includes(value) ? value : DEFAULT_OLED_TONE;
}

export async function sha256Hex(bytes, cryptoProvider = globalThis.crypto) {
  if (!cryptoProvider || !cryptoProvider.subtle) {
    throw new Error("Web Crypto is unavailable.");
  }
  const digest = await cryptoProvider.subtle.digest("SHA-256", bytes);
  return Array.from(new Uint8Array(digest), (value) =>
    value.toString(16).padStart(2, "0")).join("");
}

export function parseFirmwareRecords(text, prefix) {
  const marker = prefix + " ";
  const output = [];
  for (const line of String(text).split(/\r?\n/)) {
    if (!line.startsWith(marker)) continue;
    try {
      output.push(JSON.parse(line.slice(marker.length)));
    } catch {
      // A malformed firmware record is ignored by the presentation shell.
    }
  }
  return output;
}

export function validateMeshMessage(value) {
  const text = String(value).trim();
  if (!text) return { ok: false, error: "Enter a message first.", text: "" };
  if (/[\u0000-\u001f\u007f]/.test(text)) {
    return {
      ok: false,
      error: "Messages cannot contain control characters.",
      text: "",
    };
  }
  if (encoder.encode(text).length > 128) {
    return {
      ok: false,
      error: "The message is longer than the firmware Public-channel limit.",
      text: "",
    };
  }
  return { ok: true, error: "", text };
}

export function decodeDebugView(words) {
  if (!(words instanceof Uint32Array) || words.length !== 40) {
    throw new Error("The firmware debug view has the wrong shape.");
  }
  if (words[0] !== 1 || words[1] !== 160) {
    throw new Error("The firmware debug view ABI does not match this page.");
  }
  return {
    firmwareMillis: words[2],
    screen: words[3],
    menuIndex: words[4],
    gameMenuIndex: words[5],
    statusPage: words[6],
    connectionAction: words[7],
    activeGame: words[8],
    gamePhase: words[9],
    gameResult: words[10],
    gameRound: words[11],
    gameTotalRounds: words[12],
    gameScore: words[13],
    gameRewarded: words[14] !== 0,
    radioListening: words[15] !== 0,
    listenRemainingMs: words[16],
    sleeping: words[17] !== 0,
    inboxSelection: words[18],
    inboxCount: words[19],
    unreadCount: words[20],
    controllerSelection: words[21],
    controllerTargetSlot: words[22],
    controllerResult: words[23],
    rawButton: words[24] !== 0,
    stableButton: words[25] !== 0,
    buttonHoldConsumed: words[26] !== 0,
    energy: words[27],
    curiosity: words[28],
    affection: words[29],
    bondLevel: words[30],
    bondXp: words[31],
    packValid: words[32] !== 0,
    packId: words[33],
    packRevision: words[34],
    displaySleeping: words[35] !== 0,
    meshEnabled: words[36] !== 0,
    meshActive: words[37] !== 0,
    radioReady: words[38] !== 0,
    bootCount: words[39],
  };
}

function bytesToBase64(bytes) {
  let binary = "";
  const chunkSize = 0x8000;
  for (let offset = 0; offset < bytes.length; offset += chunkSize) {
    binary += String.fromCharCode(...bytes.subarray(offset, offset + chunkSize));
  }
  return btoa(binary);
}

function base64ToBytes(value) {
  const binary = atob(value);
  const output = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; ++index) {
    output[index] = binary.charCodeAt(index);
  }
  return output;
}

function readBytes(api, pointer, length) {
  return Uint8Array.from(new Uint8Array(api.memory.buffer, pointer, length));
}

function writeBytes(api, pointer, capacity, bytes) {
  if (bytes.length > capacity) throw new Error("A firmware input is too large.");
  new Uint8Array(api.memory.buffer, pointer, bytes.length).set(bytes);
}

function createWasiImports(memoryRef) {
  const writeU32 = (address, value) => {
    if (memoryRef.current && address) {
      new DataView(memoryRef.current.buffer).setUint32(address, value, true);
    }
  };
  return {
    wasi_snapshot_preview1: {
      fd_close: () => 0,
      fd_write: (_fd, _iov, _count, written) => {
        writeU32(written, 0);
        return 0;
      },
      fd_seek: (_fd, _low, _high, _whence, result) => {
        if (memoryRef.current && result) {
          const view = new DataView(memoryRef.current.buffer);
          view.setUint32(result, 0, true);
          view.setUint32(result + 4, 0, true);
        }
        return 0;
      },
    },
  };
}

function assertModuleContract(module) {
  const imports = WebAssembly.Module.imports(module).map((entry) =>
    entry.module + "." + entry.name);
  if (imports.length !== REQUIRED_IMPORTS.length ||
      imports.some((entry, index) => entry !== REQUIRED_IMPORTS[index])) {
    throw new Error("The firmware runtime imports do not match the pinned browser HAL.");
  }
}

function assertApiContract(api) {
  if (!(api.memory instanceof WebAssembly.Memory)) {
    throw new Error("The firmware runtime did not export linear memory.");
  }
  for (const name of REQUIRED_EXPORTS) {
    if (typeof api[name] !== "function") {
      throw new Error("The firmware runtime is missing export " + name + ".");
    }
  }
  if (api.kitsu_emulator_abi_version() !== FIRMWARE_ABI_VERSION) {
    throw new Error("The firmware runtime ABI does not match this page.");
  }
  if (api.kitsu_emulator_framebuffer_width() !== 64 ||
      api.kitsu_emulator_framebuffer_height() !== 128 ||
      api.kitsu_emulator_framebuffer_bytes() !== FIRMWARE_FRAMEBUFFER_BYTES) {
    throw new Error("The firmware OLED framebuffer contract is invalid.");
  }
  if (api.kitsu_emulator_debug_view_bytes() !== 160) {
    throw new Error("The firmware diagnostic view contract is invalid.");
  }
}

function randomDeviceId(storage) {
  try {
    const saved = storage.getItem(DEVICE_ID_STORAGE_KEY);
    if (/^[0-9a-f]{16}$/i.test(saved || "")) {
      return {
        high: Number.parseInt(saved.slice(0, 8), 16) >>> 0,
        low: Number.parseInt(saved.slice(8), 16) >>> 0,
      };
    }
  } catch {
    // A temporary identity is still safe inside this isolated demo.
  }
  const words = new Uint32Array(2);
  globalThis.crypto.getRandomValues(words);
  if (words[0] === 0 && words[1] === 0) words[0] = 1;
  const text = words[1].toString(16).padStart(8, "0") +
    words[0].toString(16).padStart(8, "0");
  try {
    storage.setItem(DEVICE_ID_STORAGE_KEY, text);
  } catch {
    // The runtime can continue without durable browser storage.
  }
  return { low: words[0], high: words[1] };
}

async function instantiateRuntime(module, options) {
  const memoryRef = { current: null };
  const instance = await WebAssembly.instantiate(
    module,
    createWasiImports(memoryRef),
  );
  const api = instance.exports;
  memoryRef.current = api.memory;
  if (typeof api._initialize !== "function") {
    throw new Error("The firmware runtime cannot initialize.");
  }
  api._initialize();
  assertApiContract(api);
  if (api.kitsu_emulator_set_device_id(
    options.deviceId.low,
    options.deviceId.high,
  ) !== 1) {
    throw new Error("The firmware hardware identity could not be installed.");
  }

  const entropyBytes = Math.min(48, api.kitsu_emulator_entropy_capacity());
  if (entropyBytes < 32) {
    throw new Error("The firmware entropy boundary is too small.");
  }
  const entropy = new Uint8Array(entropyBytes);
  globalThis.crypto.getRandomValues(entropy);
  writeBytes(
    api,
    api.kitsu_emulator_entropy_buffer(),
    api.kitsu_emulator_entropy_capacity(),
    entropy,
  );
  entropy.fill(0);
  if (api.kitsu_emulator_entropy_commit(entropyBytes) !== 1 ||
      api.kitsu_emulator_entropy_ready() !== 1) {
    throw new Error("The firmware entropy source did not initialize.");
  }

  if (options.persistence) {
    writeBytes(
      api,
      api.kitsu_emulator_persistence_buffer(),
      api.kitsu_emulator_persistence_capacity(),
      options.persistence,
    );
    if (api.kitsu_emulator_persistence_import(options.persistence.length) !== 1) {
      throw new Error("Saved firmware state failed its integrity check.");
    }
  } else {
    writeBytes(
      api,
      api.kitsu_emulator_pack_buffer(),
      api.kitsu_emulator_pack_capacity(),
      options.pack,
    );
    if (api.kitsu_emulator_pack_commit(options.pack.length) !== 1) {
      throw new Error("The Fox package did not fit the emulated companion slot.");
    }
  }

  if (options.ota) {
    const partitionBytes = api.kitsu_emulator_ota_partition_bytes();
    if (options.ota.partitions.length !== 2 ||
        options.ota.partitions.some((entry) => entry.length !== partitionBytes)) {
      throw new Error("The virtual application partitions have the wrong size.");
    }
    for (let slot = 0; slot < 2; ++slot) {
      writeBytes(
        api,
        api.kitsu_emulator_ota_partition_buffer(slot),
        partitionBytes,
        options.ota.partitions[slot],
      );
    }
    if (api.kitsu_emulator_ota_restore_active_boot_slot(
      options.ota.activeBootSlot,
    ) !== 1) {
      throw new Error("The virtual boot slot could not be restored.");
    }
  }

  if (api.kitsu_emulator_boot() !== 1) {
    throw new Error("The source-built firmware setup did not start.");
  }
  for (let index = 0; index < 10; ++index) api.kitsu_emulator_step(16);

  const runtime = {
    api,
    step(milliseconds) {
      return api.kitsu_emulator_step(milliseconds >>> 0) === 1;
    },
    debug() {
      const pointer = api.kitsu_emulator_debug_view();
      const words = Uint32Array.from(new Uint32Array(
        api.memory.buffer,
        pointer,
        api.kitsu_emulator_debug_view_bytes() / 4,
      ));
      return decodeDebugView(words);
    },
    framebuffer() {
      return new Uint8Array(
        api.memory.buffer,
        api.kitsu_emulator_framebuffer(),
        api.kitsu_emulator_framebuffer_bytes(),
      );
    },
    serial(commands) {
      api.kitsu_emulator_serial_clear();
      const commandText = String(commands).endsWith("\n")
        ? String(commands)
        : String(commands) + "\n";
      const bytes = encoder.encode(commandText);
      writeBytes(
        api,
        api.kitsu_emulator_serial_input_buffer(),
        api.kitsu_emulator_serial_input_capacity(),
        bytes,
      );
      if (api.kitsu_emulator_serial_input_commit(bytes.length) !== 1) {
        throw new Error("The firmware serial command queue is full.");
      }
      for (let index = 0;
        index < 5000 && api.kitsu_emulator_serial_input_queued() !== 0;
        ++index) {
        api.kitsu_emulator_step(2);
      }
      if (api.kitsu_emulator_serial_input_queued() !== 0) {
        throw new Error("The firmware serial parser did not drain its input.");
      }
      for (let index = 0; index < 6; ++index) api.kitsu_emulator_step(4);
      return decoder.decode(new Uint8Array(
        api.memory.buffer,
        api.kitsu_emulator_serial_buffer(),
        api.kitsu_emulator_serial_bytes(),
      ));
    },
    history() {
      const frames = [];
      const count = api.kitsu_emulator_frame_history_count();
      for (let index = 0; index < count; ++index) {
        frames.push({
          pixels: readBytes(
            api,
            api.kitsu_emulator_frame_history_frame(index),
            api.kitsu_emulator_framebuffer_bytes(),
          ),
          millis: api.kitsu_emulator_frame_history_millis(index),
          displayOn: api.kitsu_emulator_frame_history_display_on(index) !== 0,
        });
      }
      api.kitsu_emulator_frame_history_clear();
      return frames;
    },
    snapshot(includeOta = false) {
      const length = api.kitsu_emulator_persistence_export();
      if (length === 0 || length > api.kitsu_emulator_persistence_capacity()) {
        throw new Error("The firmware persistence snapshot failed.");
      }
      const snapshot = {
        persistence: readBytes(
          api,
          api.kitsu_emulator_persistence_buffer(),
          length,
        ),
        ota: null,
      };
      if (includeOta) {
        const partitionBytes = api.kitsu_emulator_ota_partition_bytes();
        snapshot.ota = {
          partitions: [0, 1].map((slot) => readBytes(
            api,
            api.kitsu_emulator_ota_partition_buffer(slot),
            partitionBytes,
          )),
          activeBootSlot: api.kitsu_emulator_ota_active_boot_slot(),
        };
      }
      return snapshot;
    },
    copyNextRadioFrame() {
      const length = api.kitsu_emulator_radio_tx_peek_length();
      if (length === 0) return null;
      if (api.kitsu_emulator_radio_tx_copy() !== length) {
        throw new Error("The in-memory radio frame could not be copied.");
      }
      return readBytes(api, api.kitsu_emulator_radio_io_buffer(), length);
    },
    consumeRadioFrame() {
      return api.kitsu_emulator_radio_tx_consume() === 1;
    },
    injectRadioFrame(bytes, rssiX100 = -7000, snrX100 = 1200) {
      writeBytes(
        api,
        api.kitsu_emulator_radio_io_buffer(),
        api.kitsu_emulator_radio_io_capacity(),
        bytes,
      );
      return api.kitsu_emulator_radio_inject_rx(
        bytes.length,
        rssiX100,
        snrX100,
      ) === 1;
    },
  };
  return runtime;
}

async function fetchPinnedBytes(url, digest, expectedBytes = 0) {
  const response = await fetch(url, {
    credentials: "same-origin",
    cache: "force-cache",
  });
  if (!response.ok) {
    throw new Error("A required demo asset returned HTTP " + response.status + ".");
  }
  const bytes = new Uint8Array(await response.arrayBuffer());
  if (expectedBytes && bytes.length !== expectedBytes) {
    throw new Error("A required demo asset has the wrong size.");
  }
  const actual = await sha256Hex(bytes);
  if (actual !== digest) {
    throw new Error("A required demo asset failed its SHA-256 check.");
  }
  return bytes;
}

export async function loadFullFirmwareAssets() {
  const [wasmBytes, pack] = await Promise.all([
    fetchPinnedBytes(FIRMWARE_WASM_URL, FIRMWARE_WASM_SHA256),
    fetchPinnedBytes(FOX_PACK_URL, FOX_PACK_SHA256, FOX_PACK_BYTES),
  ]);
  const module = await WebAssembly.compile(wasmBytes);
  assertModuleContract(module);
  return { module, pack };
}

function delay(milliseconds) {
  return new Promise((resolve) => {
    window.setTimeout(resolve, milliseconds);
  });
}

function initializeDemo() {
  const element = (selector) => document.querySelector(selector);
  const elements = {
    workbench: element(".demo-workbench"),
    canvas: element("#firmware-framebuffer"),
    display: element(".oled-display"),
    screenDescription: element("#screen-description"),
    runtimeStatus: element("#firmware-runtime-status"),
    status: element("#demo-status"),
    flashButton: element("#flash-demo"),
    bootloaderAssist: element("#bootloader-assist"),
    flashProgress: element("#flash-progress"),
    flashProgressBox: element(".flash-progress"),
    flashStage: element("#flash-stage"),
    firmwareSteps: [...document.querySelectorAll("[data-firmware-step]")],
    viewButtons: [...document.querySelectorAll("[data-demo-view]")],
    panels: [...document.querySelectorAll("[data-demo-panel]")],
    actionButtons: [...document.querySelectorAll("[data-demo-action]")],
    toneInputs: [...document.querySelectorAll("input[name='oled-tone']")],
    prgButton: element("#prg-button"),
    prgHoldButton: element("#prg-hold-button"),
    rstButton: element("#rst-button"),
    resetButton: element("#reset-demo"),
    deviceTitle: element("#device-title"),
    deviceScreen: element("#device-screen-name"),
    deviceShort: element("#device-short-action"),
    deviceLong: element("#device-long-action"),
    energy: element("#energy-meter"),
    curiosity: element("#curiosity-meter"),
    affection: element("#affection-meter"),
    sleepToggle: element("#sleep-toggle"),
    scanButton: element("#scan-mesh"),
    replayButton: element("#replay-encounter"),
    peerList: element("#mesh-peer-list"),
    messageForm: element("#demo-message-form"),
    messageInput: element("#demo-message"),
    messageError: element("#demo-message-error"),
    messageSubmit: element("#demo-message-form button[type='submit']"),
    messageLog: element("#mesh-message-log"),
    memoryList: element("#memory-list"),
  };
  const missing = Object.entries(elements)
    .filter(([, value]) => value === null)
    .map(([name]) => name);
  if (missing.length) {
    throw new Error("The demo page is missing required controls: " +
      missing.join(", ") + ".");
  }

  const context = elements.canvas.getContext("2d", { alpha: false });
  if (!context) throw new Error("The browser cannot render the OLED canvas.");
  const image = context.createImageData(64, 128);
  const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)");
  let oledTone = DEFAULT_OLED_TONE;
  let assets = null;
  let target = null;
  let peer = null;
  let peerPersistence = null;
  let targetDeviceId = randomDeviceId(window.localStorage);
  const peerDeviceId = Object.freeze({ low: 0x5a17c0de, high: 0x4b333252 });
  let installed = false;
  let busy = true;
  let meshWorking = false;
  let meshReady = false;
  let activeView = "flash";
  let downloadMode = false;
  let manualDownloadPrepared = false;
  let volatileSnapshot = null;
  let operationGeneration = 0;
  let replayingBoot = false;
  let prgDown = false;
  let pointerId = null;
  let keyboardDown = false;
  let suppressNextClick = false;
  let lastWallTime = performance.now();
  let lastFramebufferRevision = -1;
  let lastSemanticSignature = "";
  let lastPersistentSignature = "";
  let persistenceDirty = false;
  let persistenceDueAt = 0;
  let persistenceWritable = true;
  let resetInProgress = false;
  let resetArmedUntil = 0;
  let lastContacts = [];
  let lastMessages = [];
  let lastMemories = [];

  function setStatus(text) {
    elements.status.textContent = text;
  }

  function setRuntimeStatus(text) {
    elements.runtimeStatus.textContent = text;
  }

  function setBusy(value) {
    busy = Boolean(value);
    elements.workbench.setAttribute("aria-busy", busy ? "true" : "false");
    updateAvailability();
  }

  function applyTone(value, persist = false) {
    oledTone = normalizeOledTone(value);
    elements.display.dataset.oledTone = oledTone;
    for (const input of elements.toneInputs) {
      input.checked = input.value === oledTone;
    }
    if (persist) {
      try {
        window.localStorage.setItem(OLED_TONE_STORAGE_KEY, oledTone);
      } catch {
        // The visual preference remains active for this page load.
      }
    }
    renderFramebuffer(true);
  }

  function loadTone() {
    try {
      return normalizeOledTone(
        window.localStorage.getItem(OLED_TONE_STORAGE_KEY),
      );
    } catch {
      return DEFAULT_OLED_TONE;
    }
  }

  function drawPixels(pixels, displayOn = true) {
    const ink = INK[oledTone];
    for (let index = 0; index < FIRMWARE_FRAMEBUFFER_BYTES; ++index) {
      const offset = index * 4;
      const on = displayOn && pixels && pixels[index] !== 0;
      image.data[offset] = on ? ink[0] : 0;
      image.data[offset + 1] = on ? ink[1] : 0;
      image.data[offset + 2] = on ? ink[2] : 0;
      image.data[offset + 3] = 255;
    }
    context.putImageData(image, 0, 0);
  }

  function renderFramebuffer(force = false) {
    if (!target || downloadMode) {
      drawPixels(null, false);
      return;
    }
    const revision = target.api.kitsu_emulator_framebuffer_revision();
    if (!force && revision === lastFramebufferRevision) return;
    lastFramebufferRevision = revision;
    drawPixels(
      target.framebuffer(),
      target.api.kitsu_emulator_display_on() !== 0,
    );
  }

  function setView(view, focusHeading = false) {
    const allowed = view === "flash" || (installed && target);
    activeView = allowed ? view : "flash";
    for (const button of elements.viewButtons) {
      button.setAttribute(
        "aria-pressed",
        button.dataset.demoView === activeView ? "true" : "false",
      );
    }
    for (const panel of elements.panels) {
      panel.hidden = panel.dataset.demoPanel !== activeView;
    }
    if (focusHeading && activeView === "device") {
      elements.deviceTitle.focus({ preventScroll: true });
    }
  }

  function markProcedure(current, completed) {
    for (const item of elements.firmwareSteps) {
      const name = item.dataset.firmwareStep;
      if (name === current) item.setAttribute("aria-current", "step");
      else item.removeAttribute("aria-current");
      if (completed.includes(name)) item.dataset.complete = "true";
      else delete item.dataset.complete;
    }
  }

  function setProgress(percent, stage, current, completed) {
    const value = Math.max(0, Math.min(100, percent));
    elements.flashProgress.style.width = value + "%";
    elements.flashProgressBox.setAttribute("aria-valuenow", String(value));
    elements.flashStage.textContent = stage;
    markProcedure(current, completed);
  }

  function currentDebug() {
    return target ? target.debug() : null;
  }

  function screenActions(debug) {
    if (!debug) return ["Unavailable", "Unavailable"];
    switch (debug.screen) {
      case 0: return ["Pet Fox", "Open menu"];
      case 1:
        return ["Next item", "Select " +
          (MENU_ITEMS[debug.menuIndex] || "item")];
      case 2:
        return ["Next item", "Select " +
          (CONNECTION_ITEMS[debug.connectionAction] || "item")];
      case 3: return ["Next message", "Return home"];
      case 4:
        return ["Next game", "Select " +
          (GAME_ITEMS[debug.gameMenuIndex] || "game")];
      case 5:
        return debug.gamePhase === 3
          ? ["Return home", "Return home"]
          : ["Tap game", "Quit game"];
      case 6: return ["Stop listening", "Stop listening"];
      case 7: return ["Wake Fox", "Wake Fox"];
      case 8: return ["Next status page", "Return home"];
      case 9: return ["Cancel or close", "Confirm pairing"];
      case 10: return ["Next controller", "Select controller"];
      case 11: return ["Cancel", "Keep holding for 5 seconds"];
      case 12: return ["Continue", "Continue"];
      default: return ["Unavailable", "Unavailable"];
    }
  }

  function describeScreen(debug) {
    if (!debug) {
      return downloadMode
        ? "The emulated board is in ESP32-S3 download mode. The OLED is off."
        : "Kitsu is not installed in the browser emulator.";
    }
    const name = SCREEN_NAMES[debug.screen] || "Firmware screen";
    let detail = "";
    if (debug.screen === 1) {
      detail = " Selected item " + (MENU_ITEMS[debug.menuIndex] || "") + ".";
    } else if (debug.screen === 2) {
      detail = " Selected item " +
        (CONNECTION_ITEMS[debug.connectionAction] || "") + ".";
    } else if (debug.screen === 4) {
      detail = " Selected game " +
        (GAME_ITEMS[debug.gameMenuIndex] || "") + ".";
    } else if (debug.screen === 5) {
      const phase = GAME_PHASES[debug.gamePhase] || "Unknown";
      const result = GAME_RESULTS[debug.gameResult] || "";
      detail = " " + phase + (result ? ", " + result : "") +
        ". Score " + debug.gameScore + ".";
    } else if (debug.screen === 6) {
      detail = " Listening with " +
        Math.ceil(debug.listenRemainingMs / 1000) + " seconds remaining.";
    } else if (debug.screen === 8) {
      detail = " Status page " + (debug.statusPage + 1) + ".";
    }
    return name + "." + detail + " Energy " + debug.energy +
      ", curiosity " + debug.curiosity + ", affection " + debug.affection + ".";
  }

  function renderDeviceGuide(debug) {
    if (!debug) {
      elements.deviceScreen.textContent = downloadMode
        ? "ESP32-S3 download mode"
        : "Not flashed";
      elements.deviceShort.textContent = "Unavailable";
      elements.deviceLong.textContent = "Unavailable";
      elements.screenDescription.textContent = describeScreen(null);
      return;
    }
    const name = SCREEN_NAMES[debug.screen] || "Firmware screen";
    elements.deviceScreen.textContent = name;
    const actions = screenActions(debug);
    elements.deviceShort.textContent = actions[0];
    elements.deviceLong.textContent = actions[1];
    const semantic = [
      debug.screen,
      debug.menuIndex,
      debug.gameMenuIndex,
      debug.statusPage,
      debug.gamePhase,
      debug.gameResult,
      debug.gameScore,
      Math.ceil(debug.listenRemainingMs / 1000),
      debug.energy,
      debug.curiosity,
      debug.affection,
      debug.displaySleeping ? 1 : 0,
    ].join(":");
    if (semantic !== lastSemanticSignature) {
      lastSemanticSignature = semantic;
      elements.screenDescription.textContent = describeScreen(debug);
    }
  }

  function renderNeeds(debug) {
    if (!debug) return;
    for (const [meter, value] of [
      [elements.energy, debug.energy],
      [elements.curiosity, debug.curiosity],
      [elements.affection, debug.affection],
    ]) {
      meter.value = value;
      meter.textContent = value + " out of 100";
    }
    elements.sleepToggle.textContent = debug.sleeping ? "Wake" : "Sleep";
    const signature = [
      debug.energy,
      debug.curiosity,
      debug.affection,
      debug.bondLevel,
      debug.bondXp,
      debug.bootCount,
      debug.meshEnabled ? 1 : 0,
    ].join(":");
    if (lastPersistentSignature && signature !== lastPersistentSignature) {
      schedulePersistence();
    }
    lastPersistentSignature = signature;
  }

  function renderMemories() {
    if (!lastMemories.length) {
      const empty = document.createElement("li");
      const label = document.createElement("strong");
      const text = document.createElement("span");
      label.textContent = "Waiting";
      text.textContent = "Fox has not recorded a firmware memory yet.";
      empty.append(label, text);
      elements.memoryList.replaceChildren(empty);
      return;
    }
    elements.memoryList.replaceChildren(...lastMemories.slice(0, 8).map(
      (memory) => {
        const item = document.createElement("li");
        const label = document.createElement("strong");
        const text = document.createElement("span");
        label.textContent = [memory.line1, memory.line2]
          .filter(Boolean).join(" ");
        text.textContent = "Firmware journal event " + memory.event + ".";
        item.append(label, text);
        return item;
      },
    ));
  }

  function renderContacts() {
    if (!lastContacts.length) {
      const item = document.createElement("li");
      const name = document.createElement("strong");
      const detail = document.createElement("span");
      name.textContent = "No injected peers";
      detail.textContent = "Inject a signed nearby advert to populate the real contact table.";
      item.append(name, detail);
      elements.peerList.replaceChildren(item);
      return;
    }
    elements.peerList.replaceChildren(...lastContacts.map((contact) => {
      const item = document.createElement("li");
      const name = document.createElement("strong");
      const detail = document.createElement("span");
      name.textContent = contact.name || contact.id || "MeshCore peer";
      detail.textContent = String(contact.role || "unknown").toUpperCase() +
        " - " + String(contact.route_hint || "flood").toUpperCase();
      item.append(name, detail);
      return item;
    }));
  }

  function renderMessages() {
    if (!lastMessages.length) {
      elements.messageLog.replaceChildren();
      return;
    }
    elements.messageLog.replaceChildren(...lastMessages.slice(-8).map(
      (message) => {
        const item = document.createElement("li");
        const sender = document.createElement("strong");
        const text = document.createElement("span");
        const who = message.direction === "out"
          ? "You"
          : (message.sender || "Demo peer");
        sender.textContent = who + " - " +
          String(message.state || "received").toUpperCase();
        text.textContent = message.text;
        item.append(sender, text);
        return item;
      },
    ));
  }

  function updateAvailability() {
    const runtimeReady = installed && target && !downloadMode;
    for (const button of elements.viewButtons) {
      button.disabled = button.dataset.demoView !== "flash" && !runtimeReady;
    }
    for (const button of elements.actionButtons) {
      button.disabled = !runtimeReady || busy;
    }
    elements.prgButton.disabled = !runtimeReady || busy;
    elements.prgHoldButton.disabled = !runtimeReady || busy;
    elements.bootloaderAssist.disabled = busy;
    elements.flashButton.disabled = !assets || busy || installed;
    elements.scanButton.disabled = !runtimeReady || busy || meshWorking;
    elements.replayButton.disabled =
      !runtimeReady || busy || meshWorking || !meshReady;
    elements.messageInput.disabled =
      !runtimeReady || busy || meshWorking || !meshReady;
    elements.messageSubmit.disabled =
      !runtimeReady || busy || meshWorking || !meshReady;
    elements.sleepToggle.disabled = !runtimeReady || busy;
  }

  function readSerialState(text) {
    const sync = parseFirmwareRecords(text, "KITSU_SYNC").at(-1);
    if (sync) {
      elements.energy.value = sync.energy;
      elements.curiosity.value = sync.curiosity;
      elements.affection.value = sync.affection;
    }
    lastMemories = parseFirmwareRecords(text, "KITSU_MEMORY");
    lastContacts = parseFirmwareRecords(text, "KITSU_CONTACT");
    lastMessages = parseFirmwareRecords(text, "KITSU_MESSAGE");
    renderMemories();
    renderContacts();
    renderMessages();
  }

  function firmwareFailure(text) {
    const errorLine = String(text).split(/\r?\n/)
      .find((line) => line.startsWith("KITSU_ERROR "));
    if (errorLine) return errorLine.slice("KITSU_ERROR ".length);
    for (const prefix of ["KITSU_CHAT_RESULT", "KITSU_MESH_RESULT"]) {
      const record = parseFirmwareRecords(text, prefix).at(-1);
      if (record && record.status === "rejected") {
        return record.error || "rejected";
      }
    }
    return "";
  }

  function refreshFirmwarePanels() {
    if (!target) return;
    try {
      const output = target.serial(
        "sync\njournal\nchat contacts\nchat inbox 0",
      );
      readSerialState(output);
      const debug = currentDebug();
      renderNeeds(debug);
      renderDeviceGuide(debug);
      meshReady = Boolean(
        debug && debug.meshEnabled && debug.meshActive && peer,
      );
      updateAvailability();
    } catch (error) {
      setStatus(error instanceof Error ? error.message :
        "The firmware state could not be read.");
    }
  }

  function schedulePersistence() {
    if (!installed || !target || !persistenceWritable) return;
    persistenceDirty = true;
    persistenceDueAt = performance.now() + 350;
  }

  function savePersistence() {
    if (!installed || !target || !persistenceWritable) return;
    try {
      const snapshot = target.snapshot(false);
      window.localStorage.setItem(
        PERSISTENCE_STORAGE_KEY,
        bytesToBase64(snapshot.persistence),
      );
      window.localStorage.setItem(INSTALLED_STORAGE_KEY, "true");
      persistenceDirty = false;
    } catch {
      persistenceDirty = false;
      persistenceWritable = false;
      setStatus("Kitsu is running, but this browser could not save its local state.");
    }
  }

  function loadPersistence() {
    try {
      if (window.localStorage.getItem(INSTALLED_STORAGE_KEY) !== "true") {
        return null;
      }
      const encoded = window.localStorage.getItem(PERSISTENCE_STORAGE_KEY);
      return encoded ? base64ToBytes(encoded) : null;
    } catch {
      return null;
    }
  }

  function loadPeerPersistence() {
    try {
      const encoded = window.localStorage.getItem(
        PEER_PERSISTENCE_STORAGE_KEY,
      );
      return encoded ? base64ToBytes(encoded) : null;
    } catch {
      return null;
    }
  }

  function capturePeerPersistence(runtime = peer) {
    if (!runtime) return;
    try {
      peerPersistence = runtime.snapshot(false).persistence;
      window.localStorage.setItem(
        PEER_PERSISTENCE_STORAGE_KEY,
        bytesToBase64(peerPersistence),
      );
    } catch {
      // The peer identity remains stable for this page load.
    }
  }

  function pumpOneDirection(source, destination) {
    let moved = 0;
    while (moved < 16 && source &&
      destination && source.api.kitsu_emulator_radio_tx_count() !== 0) {
      const frame = source.copyNextRadioFrame();
      if (!frame) break;
      if (!destination.injectRadioFrame(frame)) break;
      if (!source.consumeRadioFrame()) break;
      ++moved;
    }
    return moved;
  }

  function pumpRadios() {
    if (!target || !peer) return 0;
    return pumpOneDirection(target, peer) +
      pumpOneDirection(peer, target);
  }

  function advanceRuntime(milliseconds) {
    if (!target) return;
    let remaining = Math.max(0, Math.round(milliseconds));
    while (remaining > 0) {
      const step = Math.min(16, remaining);
      target.step(step);
      if (peer) peer.step(step);
      pumpRadios();
      remaining -= step;
    }
  }

  function settleRadio(milliseconds = 1800) {
    advanceRuntime(milliseconds);
    for (let index = 0; index < 12 && pumpRadios() !== 0; ++index) {
      advanceRuntime(32);
    }
  }

  async function replayBootFrames(runtime, generation) {
    const frames = runtime.history();
    if (!frames.length) return;
    replayingBoot = true;
    for (let index = 0; index < frames.length; ++index) {
      if (generation !== operationGeneration || target !== runtime) return;
      const frame = frames[index];
      drawPixels(frame.pixels, frame.displayOn);
      if (index + 1 < frames.length) {
        const interval = Math.max(
          40,
          Math.min(900, frames[index + 1].millis - frame.millis),
        );
        await delay(reducedMotion.matches ? Math.min(70, interval) : interval);
      }
    }
    replayingBoot = false;
    lastFramebufferRevision = -1;
    renderFramebuffer(true);
  }

  async function createPeerRuntime() {
    const nextPeer = await instantiateRuntime(assets.module, {
      deviceId: peerDeviceId,
      pack: assets.pack,
      persistence: peerPersistence || loadPeerPersistence(),
      ota: null,
    });
    capturePeerPersistence(nextPeer);
    return nextPeer;
  }

  async function activateRuntime(options = {}) {
    const generation = ++operationGeneration;
    setBusy(true);
    setRuntimeStatus("Starting source-built 0.17.0 firmware");
    const nextTarget = await instantiateRuntime(assets.module, {
      deviceId: targetDeviceId,
      pack: assets.pack,
      persistence: options.persistence || null,
      ota: options.ota || null,
    });
    const nextPeer = await createPeerRuntime();
    if (generation !== operationGeneration) return false;
    target = nextTarget;
    peer = nextPeer;
    installed = true;
    downloadMode = false;
    volatileSnapshot = null;
    meshReady = false;
    lastPersistentSignature = "";
    setRuntimeStatus("Kitsu 0.17.0 running");
    await replayBootFrames(target, generation);
    if (generation !== operationGeneration || target !== nextTarget) return false;
    setBusy(false);
    schedulePersistence();
    refreshFirmwarePanels();
    setView(options.keepView ? activeView : "device", !options.keepView);
    setStatus(options.restored
      ? "Kitsu restarted from its emulated persistent storage."
      : "Kitsu 0.17.0 and Fox are running in the browser.");
    return true;
  }

  async function performFlash() {
    if (!assets || busy || installed) return;
    const generation = ++operationGeneration;
    setBusy(true);
    setProgress(8, "The demo USB connection is ready.", "connect", []);
    await delay(280);
    if (generation !== operationGeneration) return;
    setProgress(
      20,
      manualDownloadPrepared
        ? "Manual download mode is ready."
        : "Automatic download-mode entry is ready. PRG + RST remains available as a fallback.",
      "download",
      ["connect"],
    );
    await delay(320);
    if (generation !== operationGeneration) return;
    setProgress(
      48,
      "Loading the source-built 0.17.0 runtime into emulated application storage.",
      "write",
      ["connect", "download"],
    );
    await delay(450);
    if (generation !== operationGeneration) return;
    setProgress(
      74,
      "Validating and writing the official Fox data to the emulated companion slot.",
      "fox",
      ["connect", "download", "write"],
    );
    await delay(420);
    if (generation !== operationGeneration) return;
    setProgress(
      92,
      "Verifying both emulated regions and starting the firmware.",
      "verify",
      ["connect", "download", "write", "fox"],
    );
    try {
      const activated = await activateRuntime();
      if (!activated) return;
      setProgress(
        100,
        "Kitsu 0.17.0 and Fox verified. The firmware is running.",
        "",
        ["connect", "download", "write", "fox", "verify"],
      );
      savePersistence();
    } catch (error) {
      target = null;
      peer = null;
      installed = false;
      setBusy(false);
      setRuntimeStatus("Firmware runtime unavailable");
      setStatus(error instanceof Error ? error.message :
        "The firmware runtime failed to start.");
      setProgress(0, "Installation stopped before the firmware booted.", "", []);
    }
  }

  function simulateManualDownloadMode() {
    if (busy) return;
    manualDownloadPrepared = true;
    setProgress(
      20,
      "Manual PRG + RST download-mode fallback completed in the browser.",
      "download",
      ["connect"],
    );
    setStatus("Manual download mode simulated. No USB or device was accessed.");
    updateAvailability();
  }

  async function enterDownloadMode() {
    ++operationGeneration;
    if (target) {
      try {
        volatileSnapshot = target.snapshot(true);
        savePersistence();
      } catch {
        volatileSnapshot = null;
      }
    }
    target = null;
    peer = null;
    downloadMode = true;
    meshReady = false;
    replayingBoot = false;
    setBusy(false);
    setView("flash");
    renderFramebuffer(true);
    renderDeviceGuide(null);
    setRuntimeStatus("ESP32-S3 download mode");
    setStatus(
      "Simulated ESP32-S3 download mode. No device or serial port is connected. Press RST without PRG to boot Kitsu again.",
    );
  }

  async function handleReset() {
    if (resetInProgress) return;
    resetInProgress = true;
    try {
      if (busy && !target) {
        ++operationGeneration;
        setBusy(false);
        setProgress(0, "Installation cancelled before firmware state was committed.", "", []);
        setStatus("The demo installation was cancelled. Nothing was committed.");
        return;
      }
      if (prgDown) {
        await enterDownloadMode();
        return;
      }
      if (downloadMode && installed) {
        const persistence = volatileSnapshot
          ? volatileSnapshot.persistence
          : loadPersistence();
        const ota = volatileSnapshot ? volatileSnapshot.ota : null;
        await activateRuntime({
          persistence,
          ota,
          restored: true,
          keepView: true,
        });
        return;
      }
      if (!target) {
        setStatus(
          "The empty demo Heltec restarted. Install Kitsu and Fox to boot the firmware.",
        );
        return;
      }
      const snapshot = target.snapshot(true);
      savePersistence();
      target = null;
      peer = null;
      renderFramebuffer(true);
      await activateRuntime({
        persistence: snapshot.persistence,
        ota: snapshot.ota,
        restored: true,
        keepView: true,
      });
    } catch (error) {
      setBusy(false);
      setStatus(error instanceof Error ? error.message :
        "The firmware did not restart.");
    } finally {
      resetInProgress = false;
    }
  }

  function pressPrg() {
    if (!target || busy || prgDown) return false;
    prgDown = true;
    elements.prgButton.dataset.pressed = "true";
    target.api.kitsu_emulator_set_prg(1);
    target.step(1);
    return true;
  }

  function releasePrg() {
    if (!prgDown) return false;
    prgDown = false;
    delete elements.prgButton.dataset.pressed;
    if (target) {
      target.api.kitsu_emulator_set_prg(0);
      target.step(1);
      schedulePersistence();
    }
    return true;
  }

  function pulsePrg(milliseconds) {
    if (!pressPrg()) return;
    advanceRuntime(milliseconds);
    releasePrg();
    advanceRuntime(40);
    refreshFirmwarePanels();
  }

  function runCareAction(action) {
    if (!target || busy) return;
    try {
      const output = target.serial(action);
      advanceRuntime(120);
      const failure = firmwareFailure(output);
      if (failure) {
        setStatus("The firmware rejected " + action + ": " + failure + ".");
      } else if (action === "listen") {
        setStatus(target.debug().radioListening
          ? "The firmware is listening. Meet the nearby demo Kitsu from the radio panel."
          : "The radio profile is off. Start an injected radio scenario first.");
      } else {
        setStatus(action.charAt(0).toUpperCase() + action.slice(1) +
          " ran inside the firmware.");
      }
      schedulePersistence();
      refreshFirmwarePanels();
    } catch (error) {
      setStatus(error instanceof Error ? error.message :
        "The firmware action failed.");
    }
  }

  function toggleSleep() {
    if (!target || busy) return;
    const command = target.debug().sleeping ? "wake" : "sleep";
    try {
      const output = target.serial(command);
      const failure = firmwareFailure(output);
      if (failure) setStatus("The firmware rejected " + command + ": " + failure + ".");
      else setStatus(command === "sleep" ? "Fox is dreaming." : "Fox woke up.");
      schedulePersistence();
      refreshFirmwarePanels();
    } catch (error) {
      setStatus(error instanceof Error ? error.message :
        "The firmware sleep state could not change.");
    }
  }

  function prepareMeshScenario() {
    if (!target || !peer) return false;
    const epoch = Math.floor(Date.now() / 1000);
    const targetOutput = target.serial("mesh config on\nmesh time " + epoch);
    const peerOutput = peer.serial(
      "mesh config on\nmesh time " + epoch + "\nmesh tx unlock",
    );
    const failure = firmwareFailure(targetOutput) || firmwareFailure(peerOutput);
    if (failure) throw new Error("Mesh setup was rejected: " + failure + ".");
    meshReady = true;
    schedulePersistence();
    updateAvailability();
    return true;
  }

  function emitPeerAdvert(scope) {
    const command = scope === "nearby"
      ? "mesh introduce nearby"
      : "mesh introduce mesh";
    advanceRuntime(1100);
    const output = peer.serial("mesh tx unlock\n" + command);
    const failure = firmwareFailure(output);
    if (failure) throw new Error("The demo peer advert was rejected: " + failure + ".");
    settleRadio(2200);
    peer.serial("mesh tx lock");
    capturePeerPersistence();
  }

  async function scanMesh() {
    if (!target || meshWorking) return;
    meshWorking = true;
    updateAvailability();
    try {
      prepareMeshScenario();
      target.serial("stop");
      emitPeerAdvert("nearby");
      refreshFirmwarePanels();
      setStatus(lastContacts.length
        ? "A valid signed peer advert passed through the firmware contact path."
        : "The raw advert was injected, but the firmware did not add a contact.");
    } catch (error) {
      setStatus(error instanceof Error ? error.message :
        "The injected mesh scenario failed.");
    } finally {
      meshWorking = false;
      updateAvailability();
    }
  }

  async function injectEncounter() {
    if (!target || meshWorking) return;
    meshWorking = true;
    updateAvailability();
    try {
      prepareMeshScenario();
      let output = target.serial("mesh tx unlock\nstop\nlisten");
      let failure = firmwareFailure(output);
      if (failure) throw new Error("The Kitsu listener was rejected: " + failure + ".");
      output = peer.serial("stop\nlisten");
      failure = firmwareFailure(output);
      if (failure) throw new Error("The demo Kitsu listener was rejected: " + failure + ".");
      settleRadio(2200);
      target.serial("mesh tx lock");
      peer.serial("mesh tx lock");
      capturePeerPersistence();
      refreshFirmwarePanels();
      setStatus(
        "Two direct Kitsu presence frames reached the real nearby-pet protocol. They did not enter MeshCore, and no RF was transmitted.",
      );
    } catch (error) {
      setStatus(error instanceof Error ? error.message :
        "The injected encounter failed.");
    } finally {
      meshWorking = false;
      updateAvailability();
    }
  }

  async function submitMeshMessage(event) {
    event.preventDefault();
    if (!target || meshWorking) return;
    const validation = validateMeshMessage(elements.messageInput.value);
    if (!validation.ok) {
      elements.messageError.textContent = validation.error;
      setStatus(validation.error);
      elements.messageInput.focus();
      return;
    }
    elements.messageError.textContent = "";
    meshWorking = true;
    updateAvailability();
    try {
      prepareMeshScenario();
      let output = target.serial(
        "mesh tx unlock\nchat send ch 0 " + validation.text,
      );
      let failure = firmwareFailure(output);
      if (failure) throw new Error("The firmware rejected the message: " + failure + ".");
      settleRadio(2600);
      target.serial("mesh tx lock");

      advanceRuntime(1100);
      output = peer.serial(
        "mesh tx unlock\nchat send ch 0 Received in demo mode.",
      );
      failure = firmwareFailure(output);
      if (!failure) settleRadio(2600);
      peer.serial("mesh tx lock");
      capturePeerPersistence();

      elements.messageInput.value = "";
      schedulePersistence();
      refreshFirmwarePanels();
      setStatus(
        "The firmware serialized, encrypted, and captured the Public-channel packet. A demo peer reply was injected locally.",
      );
    } catch (error) {
      const message = error instanceof Error ? error.message :
        "The in-memory message exchange failed.";
      elements.messageError.textContent = message;
      setStatus(message);
    } finally {
      meshWorking = false;
      updateAvailability();
    }
  }

  function clearDemoStorage() {
    try {
      window.localStorage.removeItem(INSTALLED_STORAGE_KEY);
      window.localStorage.removeItem(PERSISTENCE_STORAGE_KEY);
      window.localStorage.removeItem(PEER_PERSISTENCE_STORAGE_KEY);
      for (const key of LEGACY_STORAGE_KEYS) {
        window.localStorage.removeItem(key);
      }
    } catch {
      // In-memory reset still proceeds.
    }
  }

  function resetDemo() {
    const now = performance.now();
    if (now > resetArmedUntil) {
      resetArmedUntil = now + 5000;
      elements.resetButton.textContent = "Press again to erase";
      setStatus("Press Reset demo again to erase the emulated firmware state.");
      return;
    }
    ++operationGeneration;
    releasePrg();
    target = null;
    peer = null;
    installed = false;
    busy = false;
    meshWorking = false;
    meshReady = false;
    downloadMode = false;
    manualDownloadPrepared = false;
    volatileSnapshot = null;
    persistenceDirty = false;
    persistenceWritable = true;
    peerPersistence = null;
    lastContacts = [];
    lastMessages = [];
    lastMemories = [];
    clearDemoStorage();
    resetArmedUntil = 0;
    elements.resetButton.textContent = "Reset demo";
    setProgress(0, "Ready to load the Kitsu runtime and Fox into the emulator.", "", []);
    setRuntimeStatus(assets ? "Ready to install" : "Loading firmware runtime");
    setStatus(
      "The browser emulator was reset. Its hardware identity and OLED appearance were kept.",
    );
    renderFramebuffer(true);
    renderDeviceGuide(null);
    renderMemories();
    renderContacts();
    renderMessages();
    setView("flash");
    updateAvailability();
  }

  function frame(now) {
    const elapsed = Math.max(1, Math.min(100, Math.round(now - lastWallTime)));
    lastWallTime = now;
    if (target && !busy && !downloadMode) {
      if (target.api.kitsu_emulator_restart_pending() !== 0) {
        void handleReset();
      } else {
        target.step(elapsed);
        if (peer) peer.step(elapsed);
        pumpRadios();
        if (!replayingBoot) renderFramebuffer();
        const debug = currentDebug();
        renderDeviceGuide(debug);
        renderNeeds(debug);
        if (target &&
            target.api.kitsu_emulator_restart_pending() !== 0) {
          void handleReset();
        }
      }
    }
    if (persistenceDirty && now >= persistenceDueAt) savePersistence();
    window.requestAnimationFrame(frame);
  }

  function bindPrgControls() {
    elements.prgButton.addEventListener("pointerdown", (event) => {
      if (event.button !== 0 || !pressPrg()) return;
      pointerId = event.pointerId;
      suppressNextClick = true;
      elements.prgButton.setPointerCapture(event.pointerId);
      event.preventDefault();
    });
    const releasePointer = (event) => {
      if (pointerId !== null && event.pointerId !== pointerId) return;
      releasePrg();
      pointerId = null;
      window.setTimeout(refreshFirmwarePanels, 60);
    };
    elements.prgButton.addEventListener("pointerup", releasePointer);
    elements.prgButton.addEventListener("pointercancel", releasePointer);
    elements.prgButton.addEventListener("lostpointercapture", releasePointer);
    elements.prgButton.addEventListener("keydown", (event) => {
      if ((event.key !== " " && event.key !== "Enter") ||
          event.repeat || keyboardDown) return;
      keyboardDown = true;
      suppressNextClick = true;
      pressPrg();
      if (event.key === " ") event.preventDefault();
    });
    elements.prgButton.addEventListener("keyup", (event) => {
      if ((event.key !== " " && event.key !== "Enter") || !keyboardDown) return;
      keyboardDown = false;
      releasePrg();
      window.setTimeout(refreshFirmwarePanels, 60);
      if (event.key === " ") event.preventDefault();
    });
    elements.prgButton.addEventListener("click", (event) => {
      if (suppressNextClick) {
        suppressNextClick = false;
        event.preventDefault();
        return;
      }
      pulsePrg(100);
    });
    elements.prgHoldButton.addEventListener("click", () => {
      pulsePrg(800);
    });
    const safetyRelease = () => {
      keyboardDown = false;
      pointerId = null;
      releasePrg();
    };
    window.addEventListener("blur", safetyRelease);
    document.addEventListener("visibilitychange", () => {
      if (document.hidden) safetyRelease();
    });
  }

  async function loadAndRestore() {
    setBusy(true);
    setRuntimeStatus("Loading and verifying source-built 0.17.0 firmware");
    setStatus("Loading the source-built firmware runtime and Fox data.");
    try {
      assets = await loadFullFirmwareAssets();
      setRuntimeStatus("Ready to install");
      const persistence = loadPersistence();
      if (persistence) {
        await activateRuntime({
          persistence,
          restored: true,
          keepView: true,
        });
      } else {
        setBusy(false);
        setStatus("The emulator is ready to load Kitsu 0.17.0 and Fox.");
        updateAvailability();
      }
    } catch (error) {
      setBusy(false);
      setRuntimeStatus("Firmware runtime unavailable");
      setStatus(error instanceof Error ? error.message :
        "The source-built firmware runtime could not be loaded.");
      updateAvailability();
    }
  }

  applyTone(loadTone());
  renderFramebuffer(true);
  renderDeviceGuide(null);
  renderMemories();
  renderContacts();
  renderMessages();
  setView("flash");
  updateAvailability();
  bindPrgControls();

  for (const input of elements.toneInputs) {
    input.addEventListener("change", () => applyTone(input.value, true));
  }
  for (const button of elements.viewButtons) {
    button.addEventListener("click", () => setView(button.dataset.demoView));
  }
  for (const button of elements.actionButtons) {
    button.addEventListener("click", () => runCareAction(button.dataset.demoAction));
  }
  elements.flashButton.addEventListener("click", () => void performFlash());
  elements.bootloaderAssist.addEventListener("click", simulateManualDownloadMode);
  elements.rstButton.addEventListener("click", () => void handleReset());
  elements.sleepToggle.addEventListener("click", toggleSleep);
  elements.scanButton.addEventListener("click", () => void scanMesh());
  elements.replayButton.addEventListener("click", () => void injectEncounter());
  elements.messageForm.addEventListener("submit", (event) =>
    void submitMeshMessage(event));
  elements.messageInput.addEventListener("input", () => {
    elements.messageError.textContent = "";
  });
  elements.resetButton.addEventListener("click", resetDemo);
  reducedMotion.addEventListener("change", () => renderFramebuffer(true));

  window.requestAnimationFrame(frame);
  void loadAndRestore();
}

if (typeof document !== "undefined") {
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", initializeDemo, { once: true });
  } else {
    initializeDemo();
  }
}
