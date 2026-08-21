import assert from "node:assert/strict";
import { readFile, readdir } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const dist = path.join(root, "dist");
const entries = await readdir(dist, { recursive: true });
assert(entries.includes("index.html"), "dist/index.html is missing");
assert.equal(entries.some((entry) => /(?:^|[\\/])node_modules(?:[\\/]|$)/.test(entry)), false);
assert.equal(entries.some((entry) => /\.(?:map|bin|pem|key)$/i.test(entry)), false);

const [html, appSource, releaseSource, packageJson] = await Promise.all([
  readFile(path.join(dist, "index.html"), "utf8"),
  readFile(path.join(root, "src", "app.js"), "utf8"),
  readFile(path.join(root, "src", "release.js"), "utf8"),
  readFile(path.join(root, "package.json"), "utf8"),
]);
assert.doesNotMatch(html, /https?:\/\/(?:unpkg|cdn\.jsdelivr|esm\.sh|cdnjs)/i);
assert.doesNotMatch(html, /managed site platform|workers\.dev|chat interface/i);
assert.match(appSource, /new ESPLoader/);
assert.match(appSource, /loader\.chip\?\.CHIP_NAME !== "ESP32-S3"/);
assert.match(appSource, /eraseAll: false/);
assert.doesNotMatch(appSource, /\.eraseFlash\s*\(|\.eraseRegion\s*\(|eraseAll:\s*true/);
assert.match(releaseSource, /partitionOffset: 0x008000/);
assert.match(releaseSource, /applicationOffset: 0x010000/);
assert.match(releaseSource, /writes\.length !== 2/);
assert.match(releaseSource, /UPDATE_AUTHORITY_SPKI_SHA256/);
assert.equal(JSON.parse(packageJson).dependencies["esptool-js"], "0.6.1");

const bundles = entries.filter((entry) => entry.endsWith(".js"));
assert(bundles.length > 0, "bundled JavaScript is missing");
const bundledText = (await Promise.all(bundles.map((entry) => readFile(path.join(dist, entry), "utf8")))).join("\n");
assert.match(bundledText, /df530766fbc4fc93e82cdbd354ebe4a17a453c83e9bb7fe2af30ca2d202494ab/);
assert.doesNotMatch(bundledText, /https?:\/\/(?:unpkg|cdn\.jsdelivr|esm\.sh|cdnjs)/i);

console.log(`audited ${entries.length} locally bundled flash-site files`);
