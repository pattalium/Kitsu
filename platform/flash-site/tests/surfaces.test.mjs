import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const platform = new URL("../../", import.meta.url);

async function text(relative) {
  return readFile(new URL(relative, platform), "utf8");
}

test("documentation is a complete user manual without private deployment identifiers", async () => {
  const files = [
    "index.html",
    "getting-started/index.html",
    "device/index.html",
    "android/index.html",
    "messaging/index.html",
    "connectivity/index.html",
    "updates/index.html",
    "troubleshooting/index.html",
    "security/index.html",
  ];
  const docs = (await Promise.all(files.map((file) => text(`docs-site/${file}`)))).join("\n");
  for (const marker of [
    "FIRST-TIME SETUP",
    "PORTRAIT DEVICE REFERENCE",
    "NATIVE ANDROID APP",
    "MESHCORE MESSAGES",
    "OPTIONAL CONNECTIVITY",
    "SAFE WEB SERIAL INSTALL",
    "DIAGNOSE BEFORE RESETTING",
    "SECURITY MODEL",
  ]) assert.match(docs, new RegExp(marker));
  assert.match(docs, /stock MeshCore/i);
  assert.doesNotMatch(docs, /private machine|private address|private signing ceremony/i);
});

test("status checks distinct public services without exposing a private machine", async () => {
  const [html, script] = await Promise.all([
    text("status-site/index.html"),
    text("status-site/status.js"),
  ]);
  for (const check of ["public", "app", "api", "auth", "flash", "docs", "updates"]) {
    assert.match(html, new RegExp(`data-url="/checks/${check}"`));
  }
  assert.doesNotMatch(html, /data-url="https:\/\/(?:k32|app|api|auth|flash|docs|updates)\.k32\.run/);
  assert.doesNotMatch(html, /private machine|private address/i);
  assert.match(script, /mode: "cors"/);
  assert.match(script, /response\.status === 503/);
  assert.match(script, /issuer !== "https:\/\/auth\.k32\.run\/realms\/kitsu"/);
});

test("browser companion fails closed and links real setup and source", async () => {
  const [consoleSource, apiSource] = await Promise.all([
    text("web/app/components/CompanionConsole.tsx"),
    text("web/app/lib/kitsu-api.ts"),
  ]);
  assert.match(consoleSource, /isApiConfigured \? "loading" : "server-missing"/);
  assert.match(consoleSource, /https:\/\/docs\.k32\.run\/connectivity\//);
  assert.match(apiSource, /https:\/\/github\.com\/pattalium\/Kitsu/);
  assert.doesNotMatch(consoleSource, /link pending|placeholder/i);
  assert.doesNotMatch(`${consoleSource}\n${apiSource}`, /previewSnapshot|preview-fox|mode === "preview"|Interface preview/);
  assert.doesNotMatch(consoleSource, /await new Promise\(\(resolve\).*preview/i);
});

test("all public web surfaces are free of common UTF-8 mojibake", async () => {
  const files = [
    "flash-site/index.html",
    "flash-site/src/app.js",
    "flash-site/src/release.js",
    "docs-site/index.html",
    "docs-site/getting-started/index.html",
    "docs-site/connectivity/index.html",
    "docs-site/troubleshooting/index.html",
    "status-site/index.html",
    "status-site/status.js",
    "web/index.html",
    "web/app/components/CompanionConsole.tsx",
    "web/app/lib/kitsu-api.ts",
  ];
  for (const file of files) {
    assert.doesNotMatch(await text(file), /(?:Ã‚|Ã¢â‚¬|Ã¢â‚¬â„¢|Ã¢â€ |Ã¢â€¡|Ã¢â„¢|Ã¢â€”|Ã¢Å“|Ã¢Å’|Ãƒ|ï¿½)/, file);
  }
});
