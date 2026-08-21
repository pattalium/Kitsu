import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFile, readdir } from "node:fs/promises";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const APPROVED_APP_ICON_SHA256 = "4f850b551e8fc242b0b31577ab76407cf1ade0e1a59bfaaf21edde3653b0ef42";
const APPROVED_SOCIAL_CARD_SHA256 = "b9795d97ba74f60cac471e20bc4a4d699b0bd50c3f9d2ff4d1d46c6363d10aa1";
const PUBLIC_COMPANION_ANIMATIONS = ["feed", "idle", "listen", "pet", "play"];
const PUBLIC_COMPANION_SPECIES = ["cat", "dog", "fox"];

async function sha256(file) {
  return createHash("sha256").update(await readFile(file)).digest("hex");
}

test("builds a standalone K32 browser application", async () => {
  const html = await readFile(new URL("../dist/index.html", import.meta.url), "utf8");
  assert.match(html, /K32 · Kitsu Companion/i);
  assert.match(html, /kitsu-app-icon\.png/i);
  assert.match(html, /type="module"/i);
  assert.doesNotMatch(html, /managed site platform|workers\.dev|starter/i);
});

test("uses the production owner API without local persistence", async () => {
  const [api, consoleSource, vite, worker, packageJson] = await Promise.all([
    readFile(new URL("../app/lib/kitsu-api.ts", import.meta.url), "utf8"),
    readFile(new URL("../app/components/CompanionConsole.tsx", import.meta.url), "utf8"),
    readFile(new URL("../vite.config.ts", import.meta.url), "utf8"),
    readFile(new URL("../index.html", import.meta.url), "utf8"),
    readFile(new URL("../package.json", import.meta.url), "utf8"),
  ]);

  assert.match(api, /\/v1\/companions/);
  assert.match(api, /action_type: careActionTypes\[action\]/);
  assert.match(api, /"X-CSRF-Token"/);
  assert.match(api, /"Idempotency-Key"/);
  assert.match(api, /VITE_KITSU_SERVER_REPOSITORY_URL/);
  assert.match(api, /configuredHttpsUrl\(import\.meta\.env\.VITE_KITSU_API_BASE\)/);
  assert.match(api, /url\.protocol !== "https:"/);
  assert.match(api, /problem\?\.error\?\.message/);
  assert.match(consoleSource, /choices\.length > 1/);
  assert.match(consoleSource, /Who are we visiting\?/);
  assert.doesNotMatch(api, /list\.items\[0\]/);
  assert.doesNotMatch(api, /getCompanionSnapshot\("primary"\)/);
  assert.doesNotMatch(`${vite}\n${worker}\n${packageJson}`, /drizzle|sqlite|D1Database/);
  assert.doesNotMatch(`${vite}\n${worker}\n${packageJson}`, /sites-vite-plugin|@cloudflare\/vite-plugin|wrangler|vinext|next\/image/);
  assert.match(consoleSource, /kitsu-app-icon\.png/);
  assert.match(consoleSource, /https:\/\/k32\.run\/#download/);
  assert.match(consoleSource, /signed native Android app/);
  assert.match(consoleSource, /20260821-approved-v2/);
  assert.match(consoleSource, /<meter className="vital-track"/);
  assert.doesNotMatch(consoleSource, /style=\{\{/);
});

test("ships only static runtime files and byte-exact K32 brand assets", async () => {
  const distRoot = path.join(root, "dist");
  const entries = await readdir(distRoot, { recursive: true });
  const forbidden = entries.filter((entry) =>
    /(^|[\\/])(\.git|\.env(?:\.|$)|node_modules|\.wrangler|\.vinext)([\\/]|$)|\.apk$/i.test(entry),
  );
  assert.deepEqual(forbidden, []);

  assert.equal(
    await sha256(path.join(distRoot, "brand", "kitsu-app-icon.png")),
    APPROVED_APP_ICON_SHA256,
  );
  assert.equal(await sha256(path.join(distRoot, "og.png")), APPROVED_SOCIAL_CARD_SHA256);
});

test("ships only approved Cat, Dog, and Fox animation previews", async () => {
  const expected = PUBLIC_COMPANION_SPECIES.flatMap((species) =>
    PUBLIC_COMPANION_ANIMATIONS.map((animation) => `${species}-${animation}.gif`),
  ).sort();
  const sourceRoot = path.join(root, "public", "companion");
  const evidenceRoot = path.resolve(root, "..", "..", "assets", "pack-evidence");
  const distRoot = path.join(root, "dist", "companion");

  assert.deepEqual((await readdir(sourceRoot)).sort(), expected);
  assert.deepEqual((await readdir(distRoot)).sort(), expected);
  for (const name of expected) {
    const approved = await sha256(path.join(evidenceRoot, name));
    assert.equal(await sha256(path.join(sourceRoot, name)), approved, `${name} source drifted`);
    assert.equal(await sha256(path.join(distRoot, name)), approved, `${name} build drifted`);
  }
});
