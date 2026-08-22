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
    "LOCAL-FIRST ANDROID APP",
    "MESHCORE MESSAGES",
    "LOCAL-FIRST CONNECTIVITY",
    "SIGNED A/B BLUETOOTH UPDATE",
    "DIAGNOSE BEFORE RESETTING",
    "LOCAL SECURITY MODEL",
  ]) assert.match(docs, new RegExp(marker));
  assert.match(docs, /stock MeshCore/i);
  assert.match(docs, /authenticated Bluetooth/i);
  assert.match(docs, /controller store, MeshCore state, and coredump/i);
  assert.doesNotMatch(docs, /app\.k32\.run|api\.k32\.run|auth\.k32\.run/i);
  assert.doesNotMatch(docs, /private machine|private address|private signing ceremony/i);
});

test("status checks only the retained static release surfaces", async () => {
  const [html, script, responseScript] = await Promise.all([
    text("status-site/index.html"),
    text("status-site/status.js"),
    text("status-site/status-response.js"),
  ]);
  for (const check of ["public", "flash", "docs", "updates"]) {
    assert.match(html, new RegExp(`data-url="/checks/${check}"`));
  }
  assert.doesNotMatch(html, /data-url="\/checks\/(?:app|api|auth|gateway)"/);
  assert.doesNotMatch(html, /data-url="https:\/\/(?:k32|flash|docs|updates)\.k32\.run/);
  assert.doesNotMatch(html, /private machine|private address/i);
  assert.match(script, /mode: "cors"/);
  assert.match(script, /validateHealthResponse/);
  assert.match(responseScript, /if \(!response\.ok\)/);
  assert.match(responseScript, /body !== "ok"/);
  assert.doesNotMatch(responseScript, /status === 503|gated/);
  assert.doesNotMatch(script, /issuer|oidc|auth\.k32\.run|kind === "ready"/i);
});

test("static product, manual, and USB recovery surfaces link real destinations", async () => {
  const [product, manual, flasher] = await Promise.all([
    text("public-site/index.html"),
    text("docs-site/index.html"),
    text("flash-site/index.html"),
  ]);
  assert.match(product, /https:\/\/docs\.k32\.run\/connectivity\//);
  assert.match(product, /https:\/\/flash\.k32\.run/);
  assert.match(manual, /https:\/\/github\.com\/pattalium\/Kitsu/);
  assert.match(flasher, /seven verified writes/);
  assert.match(flasher, /rollback-enabled Kitsu bootloader/);
  assert.match(flasher, /app0 and app1/);
  assert.match(flasher, /clean private OTA journal in each slot/);
  assert.match(flasher, /isolated retired connectivity partition/);
  assert.doesNotMatch(`${product}\n${manual}\n${flasher}`, /link pending|placeholder|app\.k32\.run/i);
});

test("physical acceptance cannot authorize its own signed manifest", async () => {
  const acceptance = await text("mobile/android/qa/PHYSICAL-RELEASE-ACCEPTANCE.md");
  assert.match(acceptance, /two\s+deliberately separate records/i);
  assert.match(acceptance, /candidate hardware evidence/i);
  assert.match(acceptance, /must exclude the final manifest, final signature, and final\s+public URL/i);
  assert.match(acceptance, /physical_acceptance\.evidence_sha256/);
  assert.match(acceptance, /Final public-delivery smoke/i);
  assert.match(acceptance, /None of those values are fed back into record 1 or the\s+manifest/i);
  assert.match(acceptance, /external retained record that\s+binds both evidence hashes/i);
  assert.doesNotMatch(acceptance, /completed evidence.*final manifest.*evidence_sha256/is);
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
    "status-site/status-response.js",
    "public-site/index.html",
    "public-site/site.js",
  ];
  for (const file of files) {
    assert.doesNotMatch(await text(file), /(?:Ã‚|Ã¢â‚¬|Ã¢â‚¬â„¢|Ã¢â€ |Ã¢â€¡|Ã¢â„¢|Ã¢â€”|Ã¢Å“|Ã¢Å’|Ãƒ|ï¿½)/, file);
  }
});
