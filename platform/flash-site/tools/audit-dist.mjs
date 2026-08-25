import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFile, readdir } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const dist = path.join(root, "dist");
const entries = await readdir(dist, { recursive: true });
assert(entries.includes("index.html"), "dist/index.html is missing");
assert.equal(entries.some((entry) => /(?:^|[\\/])node_modules(?:[\\/]|$)/.test(entry)), false);
assert.equal(entries.some((entry) => /\.(?:map|bin|pem|key)$/i.test(entry)), false);
assert.equal(entries.some((entry) => /\.(?:aab|idsig|jks|keystore|p12|pfx)$/i.test(entry)), false);

const [html, appSource, releaseSource, packSource, packageJson] = await Promise.all([
  readFile(path.join(dist, "index.html"), "utf8"),
  readFile(path.join(root, "src", "app.js"), "utf8"),
  readFile(path.join(root, "src", "release.js"), "utf8"),
  readFile(path.join(root, "src", "packs.js"), "utf8"),
  readFile(path.join(root, "package.json"), "utf8"),
]);
assert.doesNotMatch(html, /https?:\/\/(?:unpkg|cdn\.jsdelivr|esm\.sh|cdnjs)/i);
assert.doesNotMatch(html, /managed site platform|workers\.dev|chat interface/i);
assert.match(appSource, /new ESPLoader/);
assert.match(appSource, /loader\.chip\?\.CHIP_NAME !== "ESP32-S3"/);
assert.match(appSource, /eraseAll: false/);
assert.match(appSource, /Signed core phase complete/);
assert.match(appSource, /same USB session writes the official/);
assert.doesNotMatch(appSource, /\.eraseFlash\s*\(|\.eraseRegion\s*\(|eraseAll:\s*true/);
assert.match(packSource, /offset: 0x670000/);
assert.match(packSource, /bytes: 0x140000/);
assert.match(packSource, /PACK_CATALOG/);
assert.doesNotMatch(html, /<input[^>]+type=["']file["']/i);
assert.match(releaseSource, /bootloaderOffset: 0x000000/);
assert.match(releaseSource, /partitionOffset: 0x008000/);
assert.match(releaseSource, /app0Offset: 0x010000/);
assert.match(releaseSource, /app1Offset: 0x340000/);
assert.match(releaseSource, /app0JournalOffset: 0x33f000/);
assert.match(releaseSource, /app1JournalOffset: 0x66f000/);
assert.match(releaseSource, /legacyConnectivityOffset: 0x7b0000/);
assert.match(releaseSource, /legacyConnectivityBytes: 0x040000/);
assert.match(releaseSource, /writes\.length !== 7/);
assert.match(releaseSource, /retire_legacy_connectivity/);
assert.match(releaseSource, /kitsu\.firmware-update\.v2/);
assert.match(releaseSource, /controller_store/);
assert.match(releaseSource, /meshcore_state/);
assert.match(releaseSource, /coredump/);
assert.match(releaseSource, /UPDATE_AUTHORITY_SPKI_SHA256/);
assert.equal(JSON.parse(packageJson).dependencies["esptool-js"], "0.6.1");

const expectedPackHashes = new Set([
  "8d19d6b8bc584d9aaee5a6867504fd23c1862c907bbeb1affd9611e35bf2a6d7",
  "c868386770b6083dcd8f7c01ec7fe455faec476a96c724ab62f09770fdcdab38",
  "8652aad28816d52fca334766ebefb5c38aec1b09dcc72783414998d17a46e261",
]);
assert.equal(entries.some((entry) => entry.endsWith(".k868")), false, "source-format pack paths must not collide with the server's protected extension denylist");
const packs = entries.filter((entry) => entry.endsWith(".pet"));
assert.equal(packs.length, 3, "dist must contain exactly the three public companion bundles");
for (const entry of packs) {
  const bytes = await readFile(path.join(dist, entry));
  const digest = createHash("sha256").update(bytes).digest("hex");
  assert(expectedPackHashes.delete(digest), `${entry} is not an approved public companion bundle`);
}
assert.equal(expectedPackHashes.size, 0, "one or more approved companion bundles are missing");

const bundles = entries.filter((entry) => entry.endsWith(".js"));
assert(bundles.length > 0, "bundled JavaScript is missing");
const bundledText = (await Promise.all(bundles.map((entry) => readFile(path.join(dist, entry), "utf8")))).join("\n");
assert.match(bundledText, /df530766fbc4fc93e82cdbd354ebe4a17a453c83e9bb7fe2af30ca2d202494ab/);
assert.doesNotMatch(bundledText, /https?:\/\/(?:unpkg|cdn\.jsdelivr|esm\.sh|cdnjs)/i);

console.log(`audited ${entries.length} locally bundled flash-site files`);
