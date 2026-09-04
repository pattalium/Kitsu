import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFile, readdir } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const dist = path.join(root, "dist");
const entries = await readdir(dist, { recursive: true });
const files = entries.map((entry) => entry.replaceAll("\\", "/"));

assert(files.includes("index.html"), "dist/index.html is missing");
assert.equal(files.some((entry) => /(?:^|\/)node_modules(?:\/|$)/.test(entry)), false);
assert.equal(files.some((entry) => /\.(?:map|bin|pem|key|aab|idsig|jks|keystore|p12|pfx)$/i.test(entry)), false);

const [html, appSource, currentReleaseSource, legacyReleaseSource, layoutGateSource, packageJson] = await Promise.all([
  readFile(path.join(dist, "index.html"), "utf8"),
  readFile(path.join(root, "src", "app.js"), "utf8"),
  readFile(path.join(root, "src", "current-release.js"), "utf8"),
  readFile(path.join(root, "src", "release.js"), "utf8"),
  readFile(path.join(root, "src", "layout-gate.js"), "utf8"),
  readFile(path.join(root, "package.json"), "utf8"),
]);

assert.doesNotMatch(html, /https?:\/\/(?:unpkg|cdn\.jsdelivr|esm\.sh|cdnjs)/i);
assert.match(html, /Install the latest Kitsu/);
assert.match(html, /firmware 0\.20\.5/);
assert.match(html, /id="device-detail"/);
assert.doesNotMatch(html, /type=["']file["']|pack-select|Replace with (?:Cat|Fox|Dog)|unlocked \.k868/i);

assert.match(appSource, /new ESPLoader/);
assert.match(appSource, /inspectInstalledFlashLayout\(loader\)/);
assert.match(appSource, /inspectCurrentOtaSelection\(loader\)/);
assert.match(appSource, /fetchVerifiedCurrentRelease\(\)/);
assert.match(appSource, /fileArray: \[\{ data: artifact\.bytes, address: artifact\.record\.offset \}\]/);
assert.match(appSource, /flashMode: "keep"/);
assert.match(appSource, /flashFreq: "keep"/);
assert.match(appSource, /flashSize: "keep"/);
assert.match(appSource, /eraseAll: false/);
assert.match(appSource, /custom companion-pack bytes changed during firmware install/);
assert.match(appSource, /OTA metadata changed during firmware install/);
assert.match(appSource, /partition table changed during firmware install/);
assert.match(appSource, /The page never performs a full-chip erase/);
assert.doesNotMatch(appSource, /from "\.\/packs\.js"|latestPack|packSelect|\.eraseFlash\s*\(|\.eraseRegion\s*\(|eraseAll:\s*true/);

const currentInstallStart = appSource.indexOf("async function installCurrent()");
const currentConfirmation = appSource.indexOf("const confirmed = window.confirm(", currentInstallStart);
const currentFinalLayoutGate = appSource.indexOf("const finalLayout = await inspectInstalledFlashLayout(loader)", currentConfirmation);
const currentFirstWrite = appSource.indexOf("await loader.writeFlash({", currentFinalLayoutGate);
assert(currentInstallStart >= 0 && currentConfirmation > currentInstallStart);
assert(currentFinalLayoutGate > currentConfirmation, "current layout must be rechecked after confirmation");
assert(currentFirstWrite > currentFinalLayoutGate, "current layout and slot must be rechecked before writing");

for (const marker of [
  "otaDataOffset: 0x049000",
  "app0Offset: 0x050000",
  "app1Offset: 0x350000",
  "companionPackOffset: 0x670000",
  "companionPackBytes: 0x140000",
]) assert.match(currentReleaseSource, new RegExp(marker.replace("0x", "0x")));
assert.match(currentReleaseSource, /publishedFirmwareRelease/);
assert.match(currentReleaseSource, /parseFirmwarePackage/);
assert.match(currentReleaseSource, /verifyFirmwarePackage/);
assert.match(currentReleaseSource, /OTA_STATE_PENDING_VERIFY/);

assert.match(layoutGateSource, /3337f0ec25e653d8c0bf9534abeb147a7505f41b1c2e25b53bb6cc74d395b532/);
assert.match(legacyReleaseSource, /writes\.length !== 7/);
assert.match(legacyReleaseSource, /retire_legacy_connectivity/);
assert.equal(JSON.parse(packageJson).dependencies["esptool-js"], "0.6.1");

const firmwarePath = "downloads/kitsu-firmware-0.20.5-9b8652be49f3fbe0084b5cd7f374939b39df710b2cc7cffbaff58d98bdf312c9.kitsu-fw";
assert.deepEqual(files.filter((entry) => entry.endsWith(".kitsu-fw")), [firmwarePath]);
assert.equal(files.some((entry) => entry.endsWith(".pet") || entry.endsWith(".k868")), false);
const firmware = await readFile(path.join(dist, ...firmwarePath.split("/")));
assert.equal(firmware.byteLength, 1_267_730);
assert.equal(createHash("sha256").update(firmware).digest("hex"), "9b8652be49f3fbe0084b5cd7f374939b39df710b2cc7cffbaff58d98bdf312c9");

const bundles = files.filter((entry) => entry.endsWith(".js"));
assert(bundles.length > 0, "bundled JavaScript is missing");
const bundledText = (await Promise.all(bundles.map((entry) => readFile(path.join(dist, ...entry.split("/")), "utf8")))).join("\n");
for (const marker of [
  "0.20.5",
  "9b8652be49f3fbe0084b5cd7f374939b39df710b2cc7cffbaff58d98bdf312c9",
  "3337f0ec25e653d8c0bf9534abeb147a7505f41b1c2e25b53bb6cc74d395b532",
  "df530766fbc4fc93e82cdbd354ebe4a17a453c83e9bb7fe2af30ca2d202494ab",
]) assert.match(bundledText, new RegExp(marker.replaceAll(".", "\\.")));
assert.doesNotMatch(bundledText, /https?:\/\/(?:unpkg|cdn\.jsdelivr|esm\.sh|cdnjs)/i);

console.log(`audited ${files.length} locally bundled flash-site files`);
