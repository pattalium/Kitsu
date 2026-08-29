import assert from "node:assert/strict";
import { createHash, createPublicKey, generateKeyPairSync, sign, webcrypto } from "node:crypto";
import { readFile } from "node:fs/promises";
import path from "node:path";
import test from "node:test";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const firmware = await import(pathToFileURL(path.join(root, "firmware-release.js")));
const productionPackagePath = path.join(
  root,
  "downloads",
  "kitsu-firmware-0.20.3-022e01c0106007c6bb86ef3854a8ebd3c7fb41a2bdeda9a9285474eebe91af51.kitsu-fw",
);
const { privateKey, publicKey } = generateKeyPairSync("ed25519");
const authorityRaw = Buffer.from(publicKey.export({ format: "jwk" }).x, "base64url");
const releaseId = "kitsu-0.20.3-test-1";

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

function identityMarker({
  version = "0.20.3",
  app1 = "00350000",
  crcOverride = null,
} = {}) {
  const withLength = (length) =>
    `KITSU-ID1|schema=1|length=${String(length).padStart(4, "0")}|version=${version}|` +
    "device_class=heltec-v3.2|layout=kitsu-8m-dual-ota-3m-v1|flash=00800000|" +
    "nvs=00009000/00040000|otadata=00049000/00002000|app0=00050000|" +
    `app1=${app1}|slot=00300000|journal=00001000|max=002ff000|` +
    "spiffs=00670000/00140000|conn=007b0000/00040000|" +
    "coredump=007f0000/00010000";
  const provisional = `${withLength(0)}|crc32=00000000|end\0`;
  const prefix = Buffer.from(withLength(Buffer.byteLength(provisional, "ascii")), "ascii");
  const digest = crcOverride ?? crc32(prefix).toString(16).padStart(8, "0");
  return Buffer.from(`${prefix.toString("ascii")}|crc32=${digest}|end\0`, "ascii");
}

function application({ markers = [identityMarker()] } = {}) {
  const segmentBytes = 1024;
  const segmentEnd = 24 + 8 + segmentBytes;
  const checksumOffset = segmentEnd + (15 - (segmentEnd % 16));
  const digestOffset = checksumOffset + 1;
  const bytes = Buffer.alloc(digestOffset + 32);
  bytes[0] = 0xe9;
  bytes[1] = 1;
  bytes.writeUInt16LE(0x0009, 12);
  bytes[23] = 1;
  bytes.writeUInt32LE(0x3c000020, 24);
  bytes.writeUInt32LE(segmentBytes, 28);
  const offsets = [32, 432];
  markers.forEach((marker, index) => marker.copy(bytes, offsets[index]));
  let checksum = 0xef;
  for (let index = 32; index < segmentEnd; index += 1) checksum ^= bytes[index];
  bytes[checksumOffset] = checksum;
  createHash("sha256").update(bytes.subarray(0, digestOffset)).digest().copy(bytes, digestOffset);
  return bytes;
}

function manifestBytes(image, overrides = {}) {
  const value = {
    releaseId,
    firmwareVersion: "0.20.3",
    deviceClass: "heltec-wifi-lora-32-v3-esp32s3-8mb",
    imageFormat: "esp32s3-app",
    imageBytes: image.byteLength,
    imageSha256: createHash("sha256").update(image).digest("hex"),
    partitionBytes: 3145728,
    chunkBytes: 4096,
    rollback: true,
    ...overrides,
  };
  return Buffer.from(
    `{"schema":"kitsu.ble-firmware.v1","release_id":"${value.releaseId}",` +
      `"firmware_version":"${value.firmwareVersion}","device_class":"${value.deviceClass}",` +
      `"image_format":"${value.imageFormat}","image_bytes":${value.imageBytes},` +
      `"image_sha256":"${value.imageSha256}","partition_bytes":${value.partitionBytes},` +
      `"chunk_bytes":${value.chunkBytes},"rollback":${value.rollback}}`,
    "ascii",
  );
}

function bundle({ image = application(), manifest = null, signature = null } = {}) {
  const signedManifest = manifest ?? manifestBytes(image);
  const signedSignature = signature ?? sign(null, signedManifest, privateKey);
  const header = Buffer.alloc(20);
  header.write("KITSUFW1", 0, "ascii");
  header.writeUInt32BE(signedManifest.byteLength, 8);
  header.writeUInt16BE(signedSignature.byteLength, 12);
  header.writeUInt16BE(0, 14);
  header.writeUInt32BE(image.byteLength, 16);
  return Buffer.concat([header, signedManifest, signedSignature, image]);
}

function contractFor(bytes, overrides = {}) {
  const digest = createHash("sha256").update(bytes).digest("hex");
  return {
    url: `/downloads/kitsu-firmware-0.20.3-${digest}.kitsu-fw`,
    bytes: bytes.byteLength,
    sha256: digest,
    releaseId,
    firmwareVersion: "0.20.3",
    ...overrides,
  };
}

function fakeDocument() {
  const elements = new Map();
  for (const id of [
    "firmware-status",
    "firmware-title",
    "firmware-detail",
    "firmware-download",
    "firmware-digest",
  ]) {
    const attributes = new Map();
    const classes = new Set(id === "firmware-download" ? ["disabled"] : []);
    elements.set(`#${id}`, {
      textContent: "",
      classList: {
        add: (name) => classes.add(name),
        remove: (name) => classes.delete(name),
        contains: (name) => classes.has(name),
      },
      setAttribute: (name, value) => attributes.set(name, value),
      removeAttribute: (name) => attributes.delete(name),
      hasAttribute: (name) => attributes.has(name),
      getAttribute: (name) => attributes.get(name),
    });
  }
  return {
    querySelector: (selector) => elements.get(selector) ?? null,
    element: (id) => elements.get(`#${id}`),
  };
}

function response(bytes) {
  const body = Uint8Array.from(bytes);
  return {
    ok: true,
    headers: { get: (name) => name === "Content-Length" ? String(body.byteLength) : null },
    async arrayBuffer() {
      return body.buffer.slice(body.byteOffset, body.byteOffset + body.byteLength);
    },
  };
}

test("ships the exact five-field production package contract", async () => {
  assert.equal(Object.isFrozen(firmware.publishedFirmwareRelease), true);
  assert.deepEqual(
    Object.keys(firmware.publishedFirmwareRelease),
    ["url", "bytes", "sha256", "releaseId", "firmwareVersion"],
  );
  assert.deepEqual(firmware.publishedFirmwareRelease, {
    url: "/downloads/kitsu-firmware-0.20.3-022e01c0106007c6bb86ef3854a8ebd3c7fb41a2bdeda9a9285474eebe91af51.kitsu-fw",
    bytes: 1_228_050,
    sha256: "022e01c0106007c6bb86ef3854a8ebd3c7fb41a2bdeda9a9285474eebe91af51",
    releaseId: "kitsu-0.20.3-reflashable-1",
    firmwareVersion: "0.20.3",
  });
  const packageBytes = await readFile(productionPackagePath);
  assert.equal(packageBytes.byteLength, firmware.publishedFirmwareRelease.bytes);
  assert.equal(
    createHash("sha256").update(packageBytes).digest("hex"),
    firmware.publishedFirmwareRelease.sha256,
  );
});

test("pins the same Ed25519 authority as the published PEM", async () => {
  const pem = await readFile(path.join(root, "downloads", "update-ed25519-public.pem"));
  const key = createPublicKey(pem);
  assert.equal(key.asymmetricKeyType, "ed25519");
  assert.equal(key.export({ format: "jwk" }).x, firmware.updateAuthorityRawBase64Url);
  assert.equal(
    createHash("sha256").update(key.export({ format: "der", type: "spki" })).digest("hex"),
    "df530766fbc4fc93e82cdbd354ebe4a17a453c83e9bb7fe2af30ca2d202494ab",
  );
});

test("the shipped production package passes the enabled browser verifier", async () => {
  const packageBytes = await readFile(productionPackagePath);
  const verified = await firmware.verifyFirmwarePackage({
    bytes: packageBytes,
    contract: firmware.publishedFirmwareRelease,
    cryptoObject: webcrypto,
  });
  assert.equal(verified.packageDigest, firmware.publishedFirmwareRelease.sha256);
  assert.equal(verified.manifest.release_id, "kitsu-0.20.3-reflashable-1");
  assert.equal(verified.manifest.firmware_version, "0.20.3");
  assert.equal(verified.manifest.image_bytes, 1_227_616);
  assert.equal(
    verified.imageDigest,
    "d148442c4f5ca737056a471f981fbacb6d39be045e6f629ad5d37d75cfb260b2",
  );
  assert.equal(verified.identity.markerOffset, 0xddb4);
  assert.equal(verified.identity.markerBytes, 331);
  assert.equal(verified.identity.identityCrc32, "068e9051");

  const documentObject = fakeDocument();
  let fetches = 0;
  let verifiedBlob;
  const revoked = [];
  const urlApi = {
    createObjectURL(blob) {
      verifiedBlob = blob;
      return "blob:https://k32.run/production-firmware";
    },
    revokeObjectURL(url) { revoked.push(url); },
  };
  const initialized = await firmware.initializeFirmwareRelease({
    documentObject,
    cryptoObject: webcrypto,
    blobConstructor: Blob,
    urlApi,
    windowObject: { addEventListener() {} },
    fetchImpl: async (url) => {
      fetches += 1;
      assert.equal(url, firmware.publishedFirmwareRelease.url);
      return response(packageBytes);
    },
  });
  assert.equal(initialized.state, "verified");
  assert.equal(fetches, 1);
  assert.equal(
    documentObject.element("firmware-download").download,
    path.basename(productionPackagePath),
  );
  assert.deepEqual(Buffer.from(await verifiedBlob.arrayBuffer()), packageBytes);
  await firmware.initializeFirmwareRelease({ documentObject, contract: null, urlApi });
  assert.deepEqual(revoked, ["blob:https://k32.run/production-firmware"]);
});

test("mirrors the frozen 0.20.3 identity bytes exactly", () => {
  const expected =
    "KITSU-ID1|schema=1|length=0331|version=0.20.3|device_class=heltec-v3.2|" +
    "layout=kitsu-8m-dual-ota-3m-v1|flash=00800000|nvs=00009000/00040000|" +
    "otadata=00049000/00002000|app0=00050000|app1=00350000|slot=00300000|" +
    "journal=00001000|max=002ff000|spiffs=00670000/00140000|" +
    "conn=007b0000/00040000|coredump=007f0000/00010000|crc32=068e9051|end\0";
  assert.equal(identityMarker().toString("ascii"), expected);
  assert.equal(Buffer.byteLength(expected, "ascii"), 331);
});

test("accepts one exact content-addressed contract and rejects path boundary tricks", () => {
  const bytes = bundle();
  const contract = contractFor(bytes);
  assert.equal(firmware.validateFirmwareReleaseContract(contract), contract);
  const digest = contract.sha256;
  for (const url of [
    `/downloads/kitsu-${digest.slice(1)}.kitsu-fw`,
    `/downloads/kitsu-0${digest}.kitsu-fw`,
    `/downloads/kitsu-${digest}.kitsu-fw?download=1`,
    `https://k32.run/downloads/kitsu-${digest}.kitsu-fw`,
    `/downloads/kitsu-${"0".repeat(64)}.kitsu-fw`,
  ]) {
    assert.throws(
      () => firmware.validateFirmwareReleaseContract({ ...contract, url }),
      (error) => ["release_path_invalid", "release_digest_invalid"].includes(error.code),
      url,
    );
  }
  assert.throws(
    () => firmware.validateFirmwareReleaseContract({ ...contract, sha256: "0".repeat(64) }),
    (error) => error.code === "release_digest_invalid",
  );
});

test("does not fetch or expose a link while the release gate is disabled", async () => {
  const documentObject = fakeDocument();
  let fetches = 0;
  const result = await firmware.initializeFirmwareRelease({
    documentObject,
    contract: null,
    fetchImpl: async () => { fetches += 1; },
    cryptoObject: webcrypto,
  });
  const download = documentObject.element("firmware-download");
  assert.equal(result.state, "unavailable");
  assert.equal(fetches, 0);
  assert.equal(download.hasAttribute("href"), false);
  assert.equal(download.hasAttribute("download"), false);
  assert.equal(download.getAttribute("aria-disabled"), "true");
  assert.equal(download.classList.contains("disabled"), true);
  assert.match(documentObject.element("firmware-detail").textContent, /No physically accepted signed firmware package/i);
});

test("enables only after the exact package, signature, image, and identity verify", async () => {
  const bytes = bundle();
  const contract = contractFor(bytes);
  const verified = await firmware.verifyFirmwarePackage({
    bytes,
    contract,
    cryptoObject: webcrypto,
    authorityRaw,
  });
  assert.equal(verified.manifest.firmware_version, "0.20.3");
  assert.equal(verified.identity.layout, "kitsu-8m-dual-ota-3m-v1");
  assert.equal(verified.identity.partitionBytes, 0x300000);
  assert.equal(verified.identity.journalBytes, 0x1000);

  const documentObject = fakeDocument();
  let captured;
  let verifiedBlob;
  let fetches = 0;
  const revoked = [];
  const pagehideListeners = [];
  const urlApi = {
    createObjectURL(blob) {
      verifiedBlob = blob;
      return "blob:https://k32.run/verified-firmware";
    },
    revokeObjectURL(url) { revoked.push(url); },
  };
  const result = await firmware.initializeFirmwareRelease({
    documentObject,
    contract,
    authorityRaw,
    cryptoObject: webcrypto,
    blobConstructor: Blob,
    urlApi,
    windowObject: {
      addEventListener(name, listener, options) {
        pagehideListeners.push({ name, listener, options });
      },
    },
    fetchImpl: async (url, init) => {
      fetches += 1;
      captured = { url, init };
      return response(bytes);
    },
  });
  const download = documentObject.element("firmware-download");
  assert.equal(result.state, "verified");
  assert.equal(fetches, 1);
  assert.equal(captured.url, contract.url);
  assert.deepEqual(captured.init, {
    cache: "no-store",
    credentials: "omit",
    redirect: "error",
    referrerPolicy: "no-referrer",
  });
  assert.equal(download.href, "blob:https://k32.run/verified-firmware");
  assert.equal(download.download, contract.url.split("/").at(-1));
  assert.equal(download.hasAttribute("aria-disabled"), false);
  assert.equal(download.classList.contains("disabled"), false);
  assert.match(documentObject.element("firmware-digest").textContent, new RegExp(contract.sha256));
  assert.deepEqual(
    Buffer.from(await verifiedBlob.arrayBuffer()),
    bytes,
    "the download Blob must contain the exact bytes that passed verification",
  );
  assert.deepEqual(pagehideListeners.map(({ name, options }) => ({ name, options })), [
    { name: "pagehide", options: undefined },
  ]);

  await firmware.initializeFirmwareRelease({
    documentObject,
    contract: null,
    urlApi,
  });
  assert.deepEqual(revoked, ["blob:https://k32.run/verified-firmware"]);
  assert.equal(fetches, 1, "replacing the release state must not fetch again");
});

test("rejects truncated, trailing, wrong-size, and wrong-package-digest bytes", async () => {
  const original = bundle();
  const variants = [original.subarray(0, -1), Buffer.concat([original, Buffer.from([0])])];
  for (const bytes of variants) {
    await assert.rejects(
      firmware.verifyFirmwarePackage({
        bytes,
        contract: contractFor(bytes),
        cryptoObject: webcrypto,
        authorityRaw,
      }),
      (error) => error.code === "package_boundaries_invalid",
    );
  }
  await assert.rejects(
    firmware.verifyFirmwarePackage({
      bytes: original,
      contract: { ...contractFor(original), bytes: original.byteLength + 1 },
      cryptoObject: webcrypto,
      authorityRaw,
    }),
    (error) => error.code === "package_size_mismatch",
  );
  const digestContract = contractFor(original);
  digestContract.sha256 = "0".repeat(64);
  digestContract.url = `/downloads/kitsu-firmware-0.20.3-${digestContract.sha256}.kitsu-fw`;
  await assert.rejects(
    firmware.verifyFirmwarePackage({
      bytes: original,
      contract: digestContract,
      cryptoObject: webcrypto,
      authorityRaw,
    }),
    (error) => error.code === "package_digest_mismatch",
  );
});

test("keeps the verified Blob URL through BFCache and revokes it on real exit", async () => {
  const bytes = bundle();
  const listeners = [];
  const revoked = [];
  const result = await firmware.initializeFirmwareRelease({
    documentObject: fakeDocument(),
    contract: contractFor(bytes),
    authorityRaw,
    cryptoObject: webcrypto,
    blobConstructor: Blob,
    fetchImpl: async () => response(bytes),
    urlApi: {
      createObjectURL: () => "blob:https://k32.run/pagehide-firmware",
      revokeObjectURL: (url) => revoked.push(url),
    },
    windowObject: {
      addEventListener: (name, listener, options) => listeners.push({ name, listener, options }),
    },
  });
  assert.equal(result.state, "verified");
  assert.equal(listeners.length, 1);
  assert.equal(listeners[0].name, "pagehide");
  assert.equal(listeners[0].options, undefined);
  listeners[0].listener({ persisted: true });
  assert.deepEqual(revoked, []);
  listeners[0].listener({ persisted: false });
  assert.deepEqual(revoked, ["blob:https://k32.run/pagehide-firmware"]);
  listeners[0].listener({ persisted: false });
  assert.equal(revoked.length, 1);
});

test("rejects noncanonical, reordered, wrong-device, and wrong-version manifests", () => {
  const image = application();
  const canonical = manifestBytes(image);
  assert.equal(firmware.parseCanonicalFirmwareManifest(canonical).firmware_version, "0.20.3");
  for (const manifest of [
    Buffer.concat([canonical, Buffer.from("\n")]),
    Buffer.from(canonical.toString("ascii").replace(
      '{"schema":"kitsu.ble-firmware.v1","release_id":',
      '{"release_id":',
    )),
    manifestBytes(image, { deviceClass: "heltec-v3" }),
    manifestBytes(image, { firmwareVersion: "0.20.4" }),
  ]) {
    assert.throws(
      () => firmware.parseCanonicalFirmwareManifest(manifest),
      (error) => [
        "manifest_json_invalid",
        "manifest_contract_invalid",
        "manifest_canonical_form_invalid",
      ].includes(error.code),
    );
  }
});

test("rejects an invalid signature and an image that differs from its signed manifest", async () => {
  const image = application();
  const signedManifest = manifestBytes(image);
  const invalidSignature = Buffer.alloc(64, 0x5a);
  const wrongSignatureBundle = bundle({ image, manifest: signedManifest, signature: invalidSignature });
  await assert.rejects(
    firmware.verifyFirmwarePackage({
      bytes: wrongSignatureBundle,
      contract: contractFor(wrongSignatureBundle),
      cryptoObject: webcrypto,
      authorityRaw,
    }),
    (error) => error.code === "signature_invalid",
  );

  const alteredImage = Buffer.from(image);
  alteredImage[400] ^= 1;
  const alteredBundle = bundle({ image: alteredImage, manifest: signedManifest });
  await assert.rejects(
    firmware.verifyFirmwarePackage({
      bytes: alteredBundle,
      contract: contractFor(alteredBundle),
      cryptoObject: webcrypto,
      authorityRaw,
    }),
    (error) => error.code === "image_manifest_mismatch",
  );
});

test("requires exactly one intact current-layout identity marker", async () => {
  const cases = [
    { image: application({ markers: [] }), code: "identity_count_invalid" },
    { image: application({ markers: [identityMarker(), identityMarker()] }), code: "identity_count_invalid" },
    { image: application({ markers: [identityMarker({ crcOverride: "00000000" })] }), code: "identity_integrity_invalid" },
    { image: application({ markers: [identityMarker({ version: "0.20.4" })] }), code: null },
    { image: application({ markers: [identityMarker({ app1: "00360000" })] }), code: "identity_geometry_invalid" },
  ];
  for (const entry of cases) {
    if (entry.code === null) {
      const parsed = await firmware.validateEsp32S3Application(entry.image, webcrypto);
      assert.equal(parsed.firmwareVersion, "0.20.4");
    } else {
      await assert.rejects(
        firmware.validateEsp32S3Application(entry.image, webcrypto),
        (error) => error.code === entry.code,
      );
    }
  }

  const wrongVersionImage = application({ markers: [identityMarker({ version: "0.20.4" })] });
  const wrongVersionBundle = bundle({ image: wrongVersionImage });
  await assert.rejects(
    firmware.verifyFirmwarePackage({
      bytes: wrongVersionBundle,
      contract: contractFor(wrongVersionBundle),
      cryptoObject: webcrypto,
      authorityRaw,
    }),
    (error) => error.code === "image_identity_mismatch",
  );
});

test("rejects independent ESP32-S3 header, segment, checksum, digest, and EOF corruption", async () => {
  const valid = application();
  const corruptions = [
    ["image_header_invalid", (bytes) => { bytes[0] = 0xea; }],
    ["image_header_invalid", (bytes) => { bytes[1] = 0; }],
    ["image_chip_invalid", (bytes) => { bytes.writeUInt16LE(0x0008, 12); }],
    ["image_digest_flag_invalid", (bytes) => { bytes[23] = 0; }],
    ["image_segment_range_invalid", (bytes) => { bytes.writeUInt32LE(1025, 28); }],
    ["image_checksum_invalid", (bytes) => { bytes[bytes.byteLength - 33] ^= 1; }],
    ["image_digest_invalid", (bytes) => { bytes[bytes.byteLength - 1] ^= 1; }],
  ];
  for (const [code, mutate] of corruptions) {
    const bytes = Buffer.from(valid);
    mutate(bytes);
    await assert.rejects(
      firmware.validateEsp32S3Application(bytes, webcrypto),
      (error) => error.code === code,
      code,
    );
  }
  await assert.rejects(
    firmware.validateEsp32S3Application(Buffer.concat([valid, Buffer.from([0])]), webcrypto),
    (error) => error.code === "image_boundaries_invalid",
  );
});

test("fails closed for fetch, length, bounded-stream, crypto, Blob, and Object URL failures", async () => {
  const bytes = bundle();
  const contract = contractFor(bytes);
  let streamCancelled = false;
  const failures = [
    {
      name: "fetch rejection",
      fetchImpl: async () => { throw new Error("offline"); },
    },
    {
      name: "HTTP rejection",
      fetchImpl: async () => ({ ok: false }),
    },
    {
      name: "Content-Length mismatch",
      fetchImpl: async () => ({
        ...response(bytes),
        headers: { get: () => String(bytes.byteLength + 1) },
      }),
    },
    {
      name: "bounded stream overflow",
      fetchImpl: async () => ({
        ok: true,
        headers: { get: () => null },
        body: {
          getReader() {
            let read = false;
            return {
              async read() {
                if (read) return { done: true };
                read = true;
                return { done: false, value: new Uint8Array(bytes.byteLength + 1) };
              },
              async cancel() { streamCancelled = true; },
              releaseLock() {},
            };
          },
        },
      }),
    },
    {
      name: "crypto unavailable",
      fetchImpl: async () => response(bytes),
      cryptoObject: {},
    },
    {
      name: "Blob unavailable",
      fetchImpl: async () => response(bytes),
      blobConstructor: null,
    },
    {
      name: "Object URL unavailable",
      fetchImpl: async () => response(bytes),
      urlApi: {},
    },
    {
      name: "non-Blob Object URL",
      fetchImpl: async () => response(bytes),
      urlApi: { createObjectURL: () => "https://k32.run/downloads/unverified", revokeObjectURL() {} },
    },
  ];
  for (const entry of failures) {
    const documentObject = fakeDocument();
    const result = await firmware.initializeFirmwareRelease({
      documentObject,
      contract,
      authorityRaw,
      cryptoObject: entry.cryptoObject ?? webcrypto,
      fetchImpl: entry.fetchImpl,
      blobConstructor: Object.hasOwn(entry, "blobConstructor") ? entry.blobConstructor : Blob,
      urlApi: entry.urlApi ?? {
        createObjectURL: () => "blob:https://k32.run/failure-test",
        revokeObjectURL() {},
      },
    });
    const download = documentObject.element("firmware-download");
    assert.equal(result.state, "failed", entry.name);
    assert.equal(download.hasAttribute("href"), false, entry.name);
    assert.equal(download.hasAttribute("download"), false, entry.name);
    assert.equal(download.getAttribute("aria-disabled"), "true", entry.name);
    assert.equal(download.classList.contains("disabled"), true, entry.name);
  }
  assert.equal(streamCancelled, true);
});

test("keeps the UI disabled on every verification failure", async () => {
  const bytes = bundle();
  const documentObject = fakeDocument();
  const result = await firmware.initializeFirmwareRelease({
    documentObject,
    contract: contractFor(bytes),
    authorityRaw: Buffer.alloc(32),
    cryptoObject: webcrypto,
    fetchImpl: async () => response(bytes),
  });
  const download = documentObject.element("firmware-download");
  assert.equal(result.state, "failed");
  assert.equal(download.hasAttribute("href"), false);
  assert.equal(download.hasAttribute("download"), false);
  assert.equal(download.getAttribute("aria-disabled"), "true");
  assert.equal(download.classList.contains("disabled"), true);
  assert.match(documentObject.element("firmware-detail").textContent, /could not be verified/i);
});
