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

test("static product, manual, and historical pre-0.20.3 USB surfaces link safe destinations", async () => {
  const [product, manual, flasher, historicalWeb] = await Promise.all([
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
  assert.match(flasher, /seven verified core writes/);
  assert.match(flasher, /no intermediate starter is booted/);
  assert.match(flasher, /One final reset/);
  assert.match(flasher, /rollback-enabled Kitsu bootloader/);
  assert.match(flasher, /app0 and app1/);
  assert.match(flasher, /clean private OTA journal in each slot/);
  assert.match(flasher, /isolated retired connectivity partition/);
  assert.match(historicalWeb, /immutable historical pre-0\.20\.3 Web Serial installer/i);
  assert.match(historicalWeb, /noncurrent and unsupported for migrated or unknown-layout boards/i);
  assert.doesNotMatch(`${product}\n${manual}\n${flasher}`, /link pending|placeholder|app\.k32\.run/i);
});

test("flasher keeps its firmware controls while exposing a persistent accessible theme choice", async () => {
  const [html, theme, styles, app, packs] = await Promise.all([
    text("flash-site/index.html"),
    text("flash-site/src/theme.js"),
    text("flash-site/src/styles.css"),
    text("flash-site/src/app.js"),
    text("flash-site/src/packs.js"),
  ]);
  for (const id of ["connect", "disconnect", "refresh", "pack-select", "unlocked-pack-field", "unlocked-pack-file", "pack-detail", "install", "progress", "progress-detail", "log"]) {
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
  assert.match(html, /<option value="preserve" selected>Keep current pet<\/option>/u);
  for (const pet of ["fox", "cat", "dog"]) assert.match(html, new RegExp(`<option value="${pet}">`, "u"));
  assert.match(html, /<option value="unlocked">Replace with unlocked \.k868 file<\/option>/u);
  assert.match(html, /<input id="unlocked-pack-file"[^>]+type="file"[^>]+accept="\.k868,application\/octet-stream"[^>]+aria-describedby="pack-detail"/iu);
  assert.doesNotMatch(html, /<option value="(?:frog|hamster|turtle|rabbit|hedgehog|ferret|otter|axolotl|chinchilla|raccoon|capybara|sugar_glider|red_panda|pangolin|tasmanian_devil|snow_leopard|okapi|shoebill|cat_girl|rabbit_girl|deer_girl)">/iu);
  assert.match(styles, /\.pack-choice select/u);
  assert.match(styles, /\.unlocked-pack-field input/u);
  assert.match(app, /loadUnlockedPack/u);
  assert.match(app, /packSelect\.value = "preserve"/u);
  assert.match(app, /window\.addEventListener\("pageshow"[\s\S]*event\.persisted[\s\S]*packSelect\.value = "preserve"/u);
  assert.match(app, /inspectInstalledPack\(loader\)/u);
  assert.match(app, /inspectReplacementTransaction\(loader\)/u);
  assert.match(app, /DESTRUCTIVE PET REPLACEMENT/u);
  assert.match(app, /currentTransaction\.preparedBytes\.slice\(\)[\s\S]*buildReplacementIntent\(transition\.sourcePackId, latestPack\)/u);
  assert.match(app, /companionPackTransition\(currentPack, latestPack, currentTransaction\)/u);
  assert.match(app, /if \(\["empty", "prepared", "committed"\]\.includes\(installedReplacementTransaction\?\.status\)[\s\S]*replacementTransactionsMatch/u);
  assert.match(app, /const explicitRepair = latestPack[\s\S]*currentTransaction\.status === "empty"[\s\S]*installedPack\?\.status === "invalid"/u);
  assert.match(packs, /if \(!targetPack\)[\s\S]*current\?\.status === "valid"[\s\S]*current\.packId === transaction\.sourcePackId/u);
  assert.match(packs, /current\?\.status === "invalid" && transaction\?\.status === "empty"/u);
  assert.match(packs, /replacementTransactionTargets\(transaction, targetPack\)/u);
  assert.match(app, /replacementRetryCoreArtifacts\(verifiedRelease\)/u);
  assert.match(app, /PREPARED\/COMMITTED remain untouched/u);
  assert.match(app, /packForInstall\(selectedPackId, selectedPack\)/u);
  assert.match(app, /fileArray: \[\{ data: latestPack\.bytes, address: latestPack\.record\.offset \}\]/u);
  assert.match(app, /let packVerified = !packRequested/u);
  assert.match(app, /resetAttempted = true;\s+await loader\.after\("hard_reset"\)/u);
  assert.match(app, /Every selected region passed SHA-256 readback[\s\S]*No second automatic reset was attempted/u);
  assert.match(app, /const holdInLoader = destructiveReplacement[\s\S]*replacementRetry \|\| packWriteStarted/u);
  assert.match(app, /closeTransport\(\{ reset: !resetAttempted && !holdInLoader, announce: true \}\)/u);
  assert.ok(
    app.indexOf("data: replacementPrepared.bytes")
      < app.indexOf("fileArray: [{ data: latestPack.bytes"),
    "PREPARED must be written before any target pack byte",
  );
  assert.ok(
    app.indexOf("await verifyReadback(latestPack")
      < app.indexOf("data: replacementCommitted.bytes"),
    "COMMITTED must be written only after exact target pack readback",
  );
  assert.match(app, /if \(!replacementRetry\)[\s\S]*data: replacementPrepared\.bytes/u);
  assert.match(app, /await verifyReadback\(replacementPrepared[\s\S]*packWriteStarted = true/u);
  assert.match(app, /Keep current pet cannot clear the pending PREPARED/u);
  assert.match(app, /No PREPARED or COMMITTED record will be written/u);
  assert.match(app, /preserves legacy vitals[\s\S]*clears pack-specific traits and gifts/u);
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
