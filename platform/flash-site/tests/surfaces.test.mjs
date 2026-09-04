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

test("static product, manual, and USB flasher surfaces link safe destinations", async () => {
  const [product, manual, flasher, webReadme] = await Promise.all([
    text("public-site/index.html"),
    text("docs-site/index.html"),
    text("flash-site/index.html"),
    text("web/README.md"),
  ]);
  assert.match(product, /https:\/\/docs\.k32\.run\/connectivity\//);
  assert.match(product, /href="#firmware"/);
  assert.match(product, /Checking signed firmware package/);
  assert.match(product, /Verifying the package signature, application image, and flash layout/);
  assert.doesNotMatch(product, /https:\/\/flash\.k32\.run/);
  assert.match(manual, /https:\/\/github\.com\/pattalium\/Kitsu/);
  assert.match(flasher, /Install the latest Kitsu/);
  assert.match(flasher, /signed firmware 0\.20\.5/i);
  assert.match(flasher, /writes only the application slot selected by the bootloader/i);
  assert.match(flasher, /custom companion pack is never written/i);
  assert.doesNotMatch(flasher, /href="[^"]+\.kitsu-fw/i);
  assert.match(webReadme, /immutable historical pre-0\.20\.3 Web Serial installer/i);
  assert.doesNotMatch(`${product}\n${manual}\n${flasher}`, /link pending|placeholder|app\.k32\.run/i);
});

test("flasher offers latest firmware without a companion-pack write surface", async () => {
  const [html, theme, styles, app, currentRelease] = await Promise.all([
    text("flash-site/index.html"),
    text("flash-site/src/theme.js"),
    text("flash-site/src/styles.css"),
    text("flash-site/src/app.js"),
    text("flash-site/src/current-release.js"),
  ]);
  for (const id of ["connect", "disconnect", "refresh", "device-detail", "install", "progress", "progress-detail", "log"]) {
    assert.match(html, new RegExp(`id="${id}"`), id);
  }
  assert.match(html, /type="button" data-theme-toggle/u);
  assert.match(html, /aria-pressed="false"/u);
  assert.match(theme, /const storageKey = "kitsu-theme"/u);
  assert.match(theme, /window\.localStorage\.setItem\(storageKey, theme\)/u);
  assert.match(theme, /prefers-color-scheme: dark/u);
  assert.match(theme, /setAttribute\("aria-pressed"/u);
  assert.match(styles, /--canvas: #f3efe5/u);
  assert.match(styles, /html\[data-theme="dark"\][\s\S]*--canvas: #12110f/u);
  assert.match(styles, /--display: Georgia/u);
  assert.doesNotMatch(`${html}\n${app}`, /pack-select|unlocked-pack|loadUnlockedPack|DESTRUCTIVE PET REPLACEMENT/u);
  assert.match(app, /inspectCurrentOtaSelection\(loader\)/u);
  assert.match(app, /fileArray: \[\{ data: artifact\.bytes, address: artifact\.record\.offset \}\]/u);
  assert.match(app, /custom companion-pack bytes changed during firmware install/u);
  assert.match(app, /eraseAll: false/u);
  assert.doesNotMatch(app, /from "\.\/packs\.js"|eraseAll:\s*true/u);
  assert.match(currentRelease, /app0Offset: 0x050000/u);
  assert.match(currentRelease, /app1Offset: 0x350000/u);
  assert.match(currentRelease, /companionPackOffset: 0x670000/u);
  assert.match(currentRelease, /companionPackBytes: 0x140000/u);
  assert.doesNotMatch(html, /class="step">\d/u);
});

test("physical acceptance keeps migration private and evidence external to the signed manifest", async () => {
  const acceptance = await text("mobile/android/qa/PHYSICAL-RELEASE-ACCEPTANCE.md");
  assert.match(acceptance, /two\s+deliberately separate retained records/i);
  assert.match(acceptance, /Private capture, double backup, and fresh NVS oracle/i);
  assert.match(acceptance, /two private copies[\s\S]*different physical volumes/i);
  assert.match(acceptance, /IDF 4\.4\.7[\s\S]*zero\s+writes and zero\s+erases/i);
  assert.match(acceptance, /partition-table sector[\s\S]*final flash mutation/i);
  assert.match(acceptance, /restore[\s\S]*legacy table last/i);
  assert.match(acceptance, /signed post-migration Bluetooth firmware update/i);
  assert.match(acceptance, /Final public Android and `\.kitsu-fw` delivery smoke/i);
  assert.match(acceptance, /exact `kitsu\.ble-firmware\.v1` manifest has no physical-evidence field/i);
  assert.match(acceptance, /final promotion record externally binds record 1[\s\S]*record 2/i);
  assert.match(acceptance, /public package must be byte-for-byte\s+identical to the record-1 package/i);
  assert.doesNotMatch(acceptance, /0x10000(?![0-9a-f])|0x340000/i);
  assert.doesNotMatch(acceptance, /final signed BLE manifest may bind[\s\S]*physical_acceptance\.evidence_sha256/i);
  assert.doesNotMatch(acceptance, /Web Serial[^\n]*(?:install|migrat|repair|recover)[^\n]*`0\.20\.3`/i);
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
