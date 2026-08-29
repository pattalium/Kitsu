import assert from "node:assert/strict";
import { createHash, createPublicKey, generateKeyPairSync, sign, verify, webcrypto } from "node:crypto";
import { access, readFile, readdir, stat } from "node:fs/promises";
import path from "node:path";
import test from "node:test";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const projectRoot = path.resolve(root, "..", "..");
const previewModule = await import(pathToFileURL(path.join(root, "preview-release.js")));
const demoModule = await import(pathToFileURL(path.join(root, "demo", "demo.js")));
const unlockModule = await import(pathToFileURL(path.join(root, "unlock", "unlock.js")));
const unlockCatalogModule = await import(pathToFileURL(path.join(root, "unlock", "catalog.js")));

const canonicalPreview = Object.freeze({
  ...previewModule.previewReleaseContract.release,
  publishedAt: "2026-08-22T19:00:00Z",
});

function bodyResponse(bytes, ok = true) {
  const body = Uint8Array.from(bytes);
  return {
    ok,
    async arrayBuffer() {
      return body.buffer.slice(body.byteOffset, body.byteOffset + body.byteLength);
    },
  };
}

function testCrc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = (crc & 1) === 1 ? (crc >>> 1) ^ 0xedb88320 : crc >>> 1;
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function syntheticK868(packId = "A1B2C3D4", formatVersion = 1) {
  const frameHeight = formatVersion === 2 ? 80 : 64;
  const frameBytes = 64 * frameHeight / 8;
  const bytes = new Uint8Array(64 + (12 * 12) + (48 * 4) + (48 * frameBytes));
  bytes.set([0x4b, 0x38, 0x36, 0x38, 0x50, 0x4b, 0x31, 0x00]);
  const view = new DataView(bytes.buffer);
  view.setUint16(0x08, formatVersion, true);
  view.setUint16(0x0a, 64, true);
  view.setUint32(0x0c, bytes.byteLength, true);
  view.setUint32(0x18, Number.parseInt(packId, 16), true);
  view.setUint32(0x1c, 3, true);
  view.setUint16(0x20, 64, true);
  view.setUint16(0x22, frameHeight, true);
  view.setUint16(0x24, 48, true);
  view.setUint16(0x26, 12, true);
  view.setUint32(0x28, 48, true);
  bytes.set(Buffer.from("TEST PACK", "ascii"), 0x30);
  for (let role = 0; role < 12; role += 1) {
    const offset = 64 + role * 12;
    view.setUint8(offset, role);
    view.setUint8(offset + 1, 0);
    view.setUint8(offset + 2, role === 0 ? 2 : 1);
    view.setUint8(offset + 3, 1);
    view.setUint32(offset + 4, role * 4, true);
    view.setUint16(offset + 8, 4, true);
  }
  const stepsOffset = 64 + 12 * 12;
  for (let step = 0; step < 48; step += 1) {
    view.setUint16(stepsOffset + step * 4, step, true);
    view.setUint16(stepsOffset + step * 4 + 2, 500, true);
  }
  view.setUint32(0x10, testCrc32(bytes.subarray(64)), true);
  const headerForCrc = bytes.slice(0x08, 64);
  headerForCrc.fill(0, 0x14 - 0x08, 0x18 - 0x08);
  view.setUint32(0x14, testCrc32(headerForCrc), true);
  return bytes;
}

function encodedPreview(value = canonicalPreview) {
  return Buffer.from(`${JSON.stringify(value, null, 2)}\n`, "utf8");
}

function fakePreviewDocument() {
  const elements = new Map();
  for (const id of [
    "android-preview-status",
    "android-preview-title",
    "android-preview-detail",
    "android-preview-download",
    "android-preview-digest",
  ]) {
    const attributes = new Map();
    const classes = new Set();
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

async function sha256(file) {
  return createHash("sha256").update(await readFile(file)).digest("hex");
}

async function listFiles(directory, prefix = "") {
  const entries = await readdir(directory, { withFileTypes: true });
  const files = [];
  for (const entry of entries) {
    const relative = path.join(prefix, entry.name);
    if (entry.isDirectory()) files.push(...await listFiles(path.join(directory, entry.name), relative));
    if (entry.isFile()) files.push(relative);
  }
  return files;
}

test("publishes a complete product surface with real destinations", async () => {
  const [html, config] = await Promise.all([
    readFile(path.join(root, "index.html"), "utf8"),
    readFile(path.join(root, "config.json"), "utf8").then(JSON.parse),
  ]);
  assert.match(html, /A radio companion you can care for/);
  assert.match(html, /Portrait companion/);
  assert.match(html, /Download Android/);
  assert.match(html, /https:\/\/docs\.k32\.run/);
  assert.match(html, /https:\/\/docs\.k32\.run\/connectivity\//);
  assert.match(html, /https:\/\/flash\.k32\.run/);
  assert.match(html, /https:\/\/github\.com\/pattalium\/Kitsu/);
  assert.match(html, /does not need an account, gateway, or Internet connection/i);
  assert.match(html, /reviewed rollback bootloader, partition table, both A\/B application slots, both clean private update journals, and an exact clear over the isolated legacy connectivity partition/i);
  assert.match(html, /browser flasher stays unavailable until a physically accepted release/i);
  assert.match(html, /Check firmware availability/i);
  assert.match(html, /installer fails closed until a physically accepted signed release is available/i);
  assert.doesNotMatch(html, /hero-signal|signal-trace|signal-orbit|label-near|label-far|label-home/);
  assert.doesNotMatch(html, /[✦◇⌁]/u);
  assert.equal((html.match(/class="feature-icon" aria-hidden="true"><svg/g) ?? []).length, 4);
  assert.doesNotMatch(html, /https:\/\/(?:app|api|auth|gateway)\.k32\.run/i);
  assert.equal(config.repositoryUrl, "https://github.com/pattalium/Kitsu");
  assert.doesNotMatch(html, /link pending|verification pending|not exposed prematurely|placeholder/i);
  assert.doesNotMatch(html, /private machine|private address/i);
});

test("offers only a voluntary no-benefit Ko-fi support action", async () => {
  const [html, script, readme, privacy, funding] = await Promise.all([
    readFile(path.join(root, "index.html"), "utf8"),
    readFile(path.join(root, "site.js"), "utf8"),
    readFile(path.join(root, "README.md"), "utf8"),
    readFile(path.join(root, "privacy", "index.html"), "utf8"),
    readFile(path.join(projectRoot, ".github", "FUNDING.yml"), "utf8"),
  ]);
  const supportDestinations = [...html.matchAll(/href=["'](https:\/\/ko-fi\.com\/[^"']*)["']/gi)]
    .map((match) => match[1]);
  assert.deepEqual(supportDestinations, ["https://ko-fi.com/pattalium"]);
  assert.match(html, /Support is voluntary\. It grants no app feature, content, badge, or other benefit\./i);
  assert.match(html, /Support Kitsu on Ko-fi[^<]*<span[^>]*>→<\/span>/i);
  assert.match(readme, /plain outbound HTTPS link to `https:\/\/ko-fi\.com\/pattalium`/i);
  assert.match(readme, /Support is voluntary and grants no app feature, content, badge, or other\s+benefit/i);
  assert.match(privacy, /Following the Ko-fi support link transfers your visit to Ko-fi/i);
  assert.match(privacy, /any payment interaction then takes place on Ko-fi under its own terms and privacy notice/i);
  assert.match(privacy, /Kitsu's static site does not embed payment code or collect payment details/i);
  assert.equal(funding, "ko_fi: pattalium\n");
  assert.doesNotMatch(`${html}\n${script}\n${privacy}`, /<iframe\b|ko-fi\.com\/Home\/ButtonWidget|ko-fi\.com\/widgets|kofi(?:Widget|Button)/i);
  assert.doesNotMatch(script, /ko-fi|kofi/i);
});

test("source landing instructions match the local-only device controls", async () => {
  const readme = await readFile(path.join(projectRoot, "README.md"), "utf8");
  assert.match(readme, /has no Internet permission/i);
  assert.match(readme, /hold PRG from Home[\s\S]*CONNECT[\s\S]*BLUETOOTH[\s\S]*PAIR PHONE/i);
  assert.match(readme, /Pair this phone/);
  assert.match(readme, /no online service is involved/i);
  assert.match(readme, /only the currently accepted, signed, local-first\s+Android release/i);
  assert.match(readme, /firmware installer remains fail-closed until the matching\s+local-only firmware\s+finishes physical acceptance/i);
  assert.doesNotMatch(readme, /open `PHONE`|Connect to public gateway|owner sign-in|Configure Wi-Fi/i);
});

test("fails closed outside the exact Android 2.2.6 production contract", async () => {
  const [html, script, readme] = await Promise.all([
    readFile(path.join(root, "index.html"), "utf8"),
    readFile(path.join(root, "site.js"), "utf8"),
    readFile(path.join(root, "README.md"), "utf8"),
  ]);
  assert.match(html, /Install the signed Android app/i);
  assert.match(html, /Kitsu Android 2\.2\.6 · version code 27/i);
  assert.match(html, /Changing app tracks is a clean install/i);
  assert.match(html, /Forget authorization/i);
  assert.match(html, /Direct and Play builds cannot update one another/i);
  assert.match(script, /Download Android \$\{release\.version\}/);
  assert.match(script, /signed Android release could not be verified/i);
  assert.match(script, /REQUIRED_PACKAGE_ID = "ptl\.kitsu\.app"/);
  assert.match(script, /REQUIRED_VERSION = "2\.2\.6"/);
  assert.match(script, /REQUIRED_VERSION_CODE = 27/);
  assert.match(readme, /exact Android 2\.2\.6 \/ version-code 27/i);
  assert.match(readme, /do not cross-update/i);
  assert.doesNotMatch(html, /href=["'][^"']+\.apk["']/i);
  assert.doesNotMatch(`${html}${script}${readme}`, /https?:\/\/play\.google\.com/i);
});

test("preserves historical testing-preview bytes without advertising them", async () => {
  const [html, styles, stableScript] = await Promise.all([
    readFile(path.join(root, "index.html"), "utf8"),
    readFile(path.join(root, "styles.css"), "utf8"),
    readFile(path.join(root, "site.js"), "utf8"),
  ]);
  assert.doesNotMatch(html, /android-preview-|Kitsu Android 2\.1\.4 testing preview/i);
  assert.doesNotMatch(html, /src="\/preview-release\.js/);
  await access(path.join(root, "preview-release.js"));
  const stylesDigest = html.match(/href="\/styles\.css\?sha256=([a-f0-9]{64})"/)?.[1];
  assert.equal(stylesDigest, await sha256(path.join(root, "styles.css")));
  assert.match(styles, /\.download-cards\s*\{[^}]*grid-template-columns:\s*minmax\(0, 1fr\)/);
  assert.match(styles, /@media \(max-width: 680px\)[\s\S]*\.download-cards\s*\{\s*grid-template-columns:\s*1fr;/);

  // The stable verifier remains its independent, production-only channel.
  const stableScriptDigest = html.match(/src="\/site\.js\?sha256=([a-f0-9]{64})"/)?.[1];
  assert.equal(stableScriptDigest, await sha256(path.join(root, "site.js")));
  assert.match(stableScript, /DOWNLOAD_MANIFEST = "\/downloads\/latest\.json"/);
  assert.match(stableScript, /release\.channel !== "stable"/);
  assert.match(stableScript, /release\.buildType !== "release"/);
  assert.match(stableScript, /REQUIRED_PACKAGE_ID = "ptl\.kitsu\.app"/);
});

test("pins an exact and strictly ordered Android testing-preview contract", async () => {
  const publicKey = createPublicKey(await readFile(path.join(root, "downloads", "update-ed25519-public.pem")));
  assert.deepEqual(previewModule.previewReleaseContract.fields, [
    "schema",
    "status",
    "channel",
    "acceptance",
    "buildType",
    "packageId",
    "version",
    "versionCode",
    "minimumAndroidApi",
    "targetAndroidApi",
    "url",
    "bytes",
    "sha256",
    "signingCertificateSha256",
    "internetPermissionDeclared",
    "foregroundServicesDeclared",
    "controllerAuthorizationScope",
    "publishedAt",
  ]);
  assert.deepEqual(previewModule.previewReleaseContract.release, {
    schema: "kitsu.android-testing-preview.v1",
    status: "available",
    channel: "testing-preview",
    acceptance: "not-stable",
    buildType: "debug",
    packageId: "ptl.kitsu.app.debug",
    version: "2.1.4-debug",
    versionCode: 18,
    minimumAndroidApi: 26,
    targetAndroidApi: 36,
    url: "/downloads/kitsu-android-2.1.4-debug-16684de9063b6e5a76ac2c7f517e8219.apk",
    bytes: 17639476,
    sha256: "16684de9063b6e5a76ac2c7f517e8219db4b1c908c1d8fea1fd85cd42768823d",
    signingCertificateSha256: "68892ab5f40f5b8b01834be4ba2fcc4fd9038293d9bdf97ab48c9dc0bb534298",
    internetPermissionDeclared: false,
    foregroundServicesDeclared: false,
    controllerAuthorizationScope: "separate-install",
  });
  assert.equal(previewModule.previewReleaseContract.manifestPath, "/downloads/android-testing-preview-2.1.4-debug-20260823t084114z.json");
  assert.equal(previewModule.previewReleaseContract.signaturePath, "/downloads/android-testing-preview-2.1.4-debug-20260823t084114z.json.sig");
  assert.equal(previewModule.previewReleaseContract.publicKeyB64Url, publicKey.export({ format: "jwk" }).x);
  assert.equal(previewModule.validPreviewManifest(canonicalPreview), true);
});

test("verifies preview bytes before parsing or accepting their schema", async () => {
  const { publicKey, privateKey } = generateKeyPairSync("ed25519");
  const publicKeyB64Url = publicKey.export({ format: "jwk" }).x;
  const manifest = encodedPreview();
  const signature = sign(null, manifest, privateKey);
  const verified = await previewModule.verifiedPreviewManifest(
    bodyResponse(manifest),
    bodyResponse(signature),
    publicKeyB64Url,
    webcrypto.subtle,
  );
  assert.deepEqual(verified, canonicalPreview);

  const tamperedManifest = encodedPreview({ ...canonicalPreview, bytes: canonicalPreview.bytes + 1 });
  assert.equal(await previewModule.verifiedPreviewManifest(
    bodyResponse(tamperedManifest),
    bodyResponse(signature),
    publicKeyB64Url,
    webcrypto.subtle,
  ), null);

  const tamperedSignature = Buffer.from(signature);
  tamperedSignature[0] ^= 0x80;
  assert.equal(await previewModule.verifiedPreviewManifest(
    bodyResponse(manifest),
    bodyResponse(tamperedSignature),
    publicKeyB64Url,
    webcrypto.subtle,
  ), null);
  assert.equal(await previewModule.verifiedPreviewManifest(
    bodyResponse(manifest),
    bodyResponse(signature.subarray(0, 63)),
    publicKeyB64Url,
    webcrypto.subtle,
  ), null);
  assert.equal(await previewModule.verifiedPreviewManifest(
    bodyResponse(manifest, false),
    bodyResponse(signature),
    publicKeyB64Url,
    webcrypto.subtle,
  ), null);
  assert.equal(await previewModule.verifiedPreviewManifest(
    bodyResponse(manifest),
    bodyResponse(signature, false),
    publicKeyB64Url,
    webcrypto.subtle,
  ), null);
});

test("rejects preview field tampering, legacy schemas, and production-package confusion", () => {
  const invalidVariants = [
    ["legacy stable schema", { schema: "kitsu.android-release.v1" }],
    ["wrong status", { status: "withdrawn" }],
    ["stable channel", { channel: "stable" }],
    ["accepted release", { acceptance: "stable" }],
    ["release build", { buildType: "release" }],
    ["Play package", { packageId: "ptl.kitsu.app" }],
    ["legacy production package", { packageId: "app.kitsu.mobile" }],
    ["production version", { version: "2.1.4" }],
    ["previous preview version", { version: "2.1.3-debug" }],
    ["wrong version code", { versionCode: 17 }],
    ["wrong minimum API", { minimumAndroidApi: 25 }],
    ["wrong target API", { targetAndroidApi: 35 }],
    ["stable APK path", { url: "/downloads/kitsu-k32-android-2.0.0.apk" }],
    ["previous preview path", { url: "/downloads/kitsu-android-2.1.3-debug-557d61af86d1f46fbeb4e4439de94ca9.apk" }],
    ["non-content-addressed path", { url: "/downloads/kitsu-android-2.1.4-debug.apk" }],
    ["absolute APK URL", { url: "https://k32.run/downloads/kitsu-android-2.1.4-debug-16684de9063b6e5a76ac2c7f517e8219.apk" }],
    ["path traversal", { url: "/downloads/../kitsu-android-2.1.4-debug-16684de9063b6e5a76ac2c7f517e8219.apk" }],
    ["wrong byte count", { bytes: 17639475 }],
    ["wrong APK digest", { sha256: "0".repeat(64) }],
    ["uppercase APK digest", { sha256: canonicalPreview.sha256.toUpperCase() }],
    ["wrong debug certificate", { signingCertificateSha256: "0".repeat(64) }],
    ["Internet permission", { internetPermissionDeclared: true }],
    ["foreground service", { foregroundServicesDeclared: true }],
    ["shared authorization", { controllerAuthorizationScope: "production" }],
    ["noncanonical timestamp", { publishedAt: "2026-08-22T19:00:00.000Z" }],
    ["invalid timestamp", { publishedAt: "2026-02-31T19:00:00Z" }],
  ];
  for (const [label, changed] of invalidVariants) {
    assert.equal(previewModule.validPreviewManifest({ ...canonicalPreview, ...changed }), false, label);
  }

  const missingHash = { ...canonicalPreview };
  delete missingHash.sha256;
  assert.equal(previewModule.validPreviewManifest(missingHash), false);
  assert.equal(previewModule.validPreviewManifest({ ...canonicalPreview, extra: true }), false);
  const reordered = { status: canonicalPreview.status, ...canonicalPreview };
  assert.equal(previewModule.validPreviewManifest(reordered), false);
});

test("leaves the preview download fail-closed when either detached file is unavailable", async () => {
  for (const unavailablePath of [
    previewModule.previewReleaseContract.manifestPath,
    previewModule.previewReleaseContract.signaturePath,
  ]) {
    const fakeDocument = fakePreviewDocument();
    const link = fakeDocument.element("android-preview-download");
    link.setAttribute("href", "https://attacker.invalid/untrusted.apk");
    await previewModule.loadAndroidPreview({
      documentObject: fakeDocument,
      fetchFunction: async (request) => bodyResponse(new Uint8Array(), request !== unavailablePath),
      origin: "https://k32.run",
    });
    assert.equal(link.hasAttribute("href"), false, unavailablePath);
    assert.equal(link.hasAttribute("download"), false, unavailablePath);
    assert.equal(link.classList.contains("disabled"), true, unavailablePath);
    assert.equal(link.getAttribute("aria-disabled"), "true", unavailablePath);
    assert.match(fakeDocument.element("android-preview-status").textContent, /not published/i);
    assert.match(fakeDocument.element("android-preview-detail").textContent, /No test APK link has been exposed/i);
  }
});

test("publishes only the explicitly approved Fox demo companion bundle", async () => {
  const readme = await readFile(path.join(root, "README.md"), "utf8");
  const personalCompanionName = String.fromCharCode(70, 111, 120, 32, 71, 105, 114, 108);
  assert.equal(readme.includes(personalCompanionName), false);
  const demoFiles = await listFiles(path.join(root, "demo"));
  const bundles = demoFiles
    .filter((file) => file.toLowerCase().endsWith(".k868"))
    .map((file) => `demo/${file.replaceAll("\\", "/")}`);
  assert.deepEqual(bundles, [
    "demo/assets/fox.c868386770b6083dcd8f7c01ec7fe455faec476a96c724ab62f09770fdcdab38.k868",
  ]);
  const publicFox = path.join(root, bundles[0]);
  const canonicalFox = path.join(projectRoot, "assets", "packs", "fox.k868");
  assert.equal((await stat(publicFox)).size, 24976);
  assert.equal(
    await sha256(publicFox),
    "c868386770b6083dcd8f7c01ec7fe455faec476a96c724ab62f09770fdcdab38",
  );
  assert.equal(await sha256(publicFox), await sha256(canonicalFox));
  assert.equal(bundles.some((file) => /(?:cat|dog)/i.test(file)), false);
});

test("ships the full source-built firmware demo over a browser hardware layer", async () => {
  const [home, html, styles, script] = await Promise.all([
    readFile(path.join(root, "index.html"), "utf8"),
    readFile(path.join(root, "demo", "index.html"), "utf8"),
    readFile(path.join(root, "demo", "demo.css"), "utf8"),
    readFile(path.join(root, "demo", "demo.js"), "utf8"),
  ]);
  assert.match(home, /href="\/demo\/">Demo</);
  assert.match(home, /href="\/demo\/">Try the demo/);
  assert.match(html, /<strong>Demo mode<\/strong>/);
  assert.match(html, /Everything presented here is for demonstration only/i);
  assert.match(html, /does not connect to a Heltec, Bluetooth, USB, or MeshCore network/i);
  for (const control of ["Install Kitsu + Fox", "Heltec cyan", "Black &amp; white", "PRG", "RST", "Hold PRG", "Pet", "Feed", "Play", "Listen", "Sleep", "Inject nearby nodes", "Meet nearby Kitsu", "Inject message", "Reset demo"]) {
    const escapedControl = control.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    assert.match(html, new RegExp(`>${escapedControl}<`), control);
  }
  for (const section of ["Flash", "Device", "Care", "Mesh"]) assert.match(html, new RegExp(`>${section}<`));
  assert.doesNotMatch(html, /data-demo-(?:view|panel)="pack"|id="pack-(?:next|bootloader|progress|stage|error)"/);
  assert.match(html, /Hold it for 750 ms to select/);
  assert.match(html, /same core-and-pet sequence as the real flasher/i);
  assert.match(html, /automatic reset first/i);
  assert.match(html, /PRG \+ RST is available as a manual fallback/i);
  assert.match(html, /real flasher writes and verifies the signed core, then installs your selected official pet in the same USB session/i);
  assert.match(html, /href="https:\/\/flash\.k32\.run">Open the real flasher/);
  assert.match(html, /emulated pet partition/i);
  assert.match(html, /do not flash a physical board, request USB, accept files or codes/i);
  assert.doesNotMatch(html, /<input[^>]+type=["']file["']/i);
  assert.match(html, /role="status" aria-live="polite"/);
  assert.match(html, /aria-label="Simulated Kitsu and Fox installation progress"[^>]*aria-describedby="flash-stage"[^>]*role="progressbar"[^>]*aria-valuemin="0"[^>]*aria-valuemax="100"/);
  assert.match(html, /id="device-title" tabindex="-1"/);
  assert.match(html, /class="oled-display" data-oled-tone="cyan" role="img" aria-label="Kitsu firmware OLED display" aria-describedby="screen-description"/);
  assert.match(html, /<canvas class="oled-framebuffer" id="firmware-framebuffer" data-firmware-framebuffer width="64" height="128" aria-hidden="true"><\/canvas>/);
  assert.equal((html.match(/data-firmware-framebuffer/g) ?? []).length, 1);
  assert.match(html, /<fieldset class="oled-tone-picker" aria-describedby="oled-tone-help">[\s\S]*?<legend>OLED appearance<\/legend>/);
  assert.match(html, /id="oled-tone-cyan" name="oled-tone" type="radio" value="cyan" checked/);
  assert.match(html, /id="oled-tone-mono" name="oled-tone" type="radio" value="mono"/);
  assert.match(html, /Visual tint only\. Saved in this browser and kept when demo progress is reset\./);
  for (const meter of ["energy", "curiosity", "affection"]) {
    assert.match(html, new RegExp(`<label id="${meter}-label" for="${meter}-meter">[^<]+<\\/label><meter id="${meter}-meter" aria-labelledby="${meter}-label"`));
  }
  assert.match(html, /source-built 0\.17\.4 setup and loop own the display, PRG timing, menus, care, games, persistence, and reset behavior/i);
  assert.doesNotMatch(`${html}\n${script}`, /0\.17\.[123]/, "demo copy must not retain an earlier firmware version");
  assert.match(html, /Incoming raw packets exist only in memory/i);
  assert.match(html, /This is not Xtensa binary or CPU emulation/i);
  assert.match(styles, /aspect-ratio:\s*1\s*\/\s*2/);
  assert.match(styles, /\.oled-display\s*\{[^}]*--oled-ink:\s*#[a-f0-9]+;/s);
  assert.match(styles, /\.oled-display\[data-oled-tone="mono"\]\s*\{[^}]*--oled-ink:/s);
  assert.match(styles, /\.oled-framebuffer\s*\{[^}]*image-rendering:\s*pixelated/s);
  assert.doesNotMatch(styles, /\.fox-sprite|\.oled-(?:mood|energy|countdown|system|firmware)/);
  assert.match(styles, /\.oled-tone-option\s*\{[^}]*min-height:\s*2\.75rem/s);
  assert.match(styles, /\.oled-tone-option:focus-within\s*\{[^}]*outline:\s*3px solid var\(--focus\)/s);
  assert.match(styles, /@media \(max-width: 680px\)[\s\S]*\.oled-tone-picker\s*\{\s*width:\s*min\(100%, 14rem\)/);
  assert.match(styles, /\.demo-mode-switch\s*\{[^}]*grid-template-columns:\s*repeat\(4,/s);
  assert.doesNotMatch(styles, /\.pack-(?:procedure|actions|progress|stage|cli|error)/);
  assert.match(styles, /@media \(prefers-reduced-motion: reduce\)/);
  assert.match(script, /const OLED_TONE_STORAGE_KEY = "kitsu-demo-oled-tone-v1"/);
  assert.match(script, /export const FIRMWARE_ABI_VERSION = 2/);
  assert.match(script, /cdf1830000232c2e1f4492bd70fdb40b06f6a406ce4a63e1aa79c419ccdcaf73/);
  assert.match(script, /c868386770b6083dcd8f7c01ec7fe455faec476a96c724ab62f09770fdcdab38/);
  assert.match(script, /WebAssembly\.Module\.imports/);
  assert.match(script, /WebAssembly\.compile\(wasmBytes\)/);
  assert.match(script, /crypto\.getRandomValues|cryptoProvider\.subtle/);
  assert.match(script, /kitsu_emulator_entropy_commit/);
  assert.match(script, /kitsu_emulator_set_prg\(1\)/);
  assert.match(script, /kitsu_emulator_set_prg\(0\)/);
  assert.match(script, /kitsu_emulator_persistence_export/);
  assert.match(script, /kitsu_emulator_radio_inject_rx/);
  assert.match(script, /mesh introduce mesh/);
  assert.match(script, /chat send ch 0/);
  assert.match(script, /display\.dataset\.oledTone = oledTone/);
  assert.match(script, /localStorage\.setItem\(OLED_TONE_STORAGE_KEY, oledTone\)/);
  const resetDemoBody = script.match(/function resetDemo\(\) \{([\s\S]*?)\n  \}\n\n  function frame/)?.[1] ?? "";
  assert.doesNotMatch(resetDemoBody, /removeItem\(OLED_TONE_STORAGE_KEY\)/, "Reset demo preserves the independent OLED preference");
  assert.doesNotMatch(script, /applyDemoAction|applyDeviceInput|completeDemoInstall|applyFirmwareGameReward|CLIP_MOODS|FIRMWARE_GAME_WASM/);
  assert.doesNotMatch(script, /navigator\.(?:bluetooth|serial|usb)|WebSocket|EventSource|https?:\/\//i);
  assert.match(script, /reducedMotion\.matches/);
  assert.match(script, /pointercancel", releasePointer/);
  assert.match(script, /lostpointercapture", releasePointer/);
  assert.match(script, /screenDescription\.textContent = describeScreen/);

  const cssDigest = html.match(/href="\/demo\/demo\.css\?sha256=([a-f0-9]{64})"/)?.[1];
  const scriptDigest = html.match(/src="\/demo\/demo\.js\?sha256=([a-f0-9]{64})"/)?.[1];
  assert.equal(cssDigest, await sha256(path.join(root, "demo", "demo.css")));
  assert.equal(scriptDigest, await sha256(path.join(root, "demo", "demo.js")));

  const canonicalFox = path.join(projectRoot, "assets", "pack-evidence", "fox-48-frame-contact.png");
  const publicFox = path.join(root, "demo", "assets", "fox-48-frame-contact.png");
  assert.equal((await stat(publicFox)).size, 17580);
  assert.equal(await sha256(publicFox), await sha256(canonicalFox));

  const wasmPath = path.join(
    root,
    "demo",
    "kitsu-firmware-full.cdf1830000232c2e1f4492bd70fdb40b06f6a406ce4a63e1aa79c419ccdcaf73.wasm",
  );
  assert.equal((await stat(wasmPath)).size, 356142);
  assert.equal(
    await sha256(wasmPath),
    "cdf1830000232c2e1f4492bd70fdb40b06f6a406ce4a63e1aa79c419ccdcaf73",
  );
  const wasmModule = await WebAssembly.compile(await readFile(wasmPath));
  assert.deepEqual(
    WebAssembly.Module.imports(wasmModule).map((entry) =>
      entry.module + "." + entry.name),
    [
      "wasi_snapshot_preview1.fd_close",
      "wasi_snapshot_preview1.fd_write",
      "wasi_snapshot_preview1.fd_seek",
    ],
  );
  const wasmExports = new Set(
    WebAssembly.Module.exports(wasmModule).map((entry) => entry.name),
  );
  for (const required of [
    "kitsu_emulator_boot",
    "kitsu_emulator_step",
    "kitsu_emulator_set_prg",
    "kitsu_emulator_framebuffer",
    "kitsu_emulator_persistence_export",
    "kitsu_emulator_ble_rx_chunk_commit",
    "kitsu_emulator_radio_inject_rx",
  ]) assert.equal(wasmExports.has(required), true, required);
});

test("full firmware demo helpers and raw PRG path fail closed", async () => {
  assert.equal(demoModule.DEFAULT_OLED_TONE, "cyan");
  assert.deepEqual(demoModule.OLED_TONES, ["cyan", "mono"]);
  assert.equal(demoModule.normalizeOledTone("cyan"), "cyan");
  assert.equal(demoModule.normalizeOledTone("mono"), "mono");
  assert.equal(demoModule.normalizeOledTone("sepia"), "cyan");
  assert.equal(demoModule.normalizeOledTone(null), "cyan");
  assert.equal(demoModule.FIRMWARE_ABI_VERSION, 2);
  assert.equal(demoModule.FIRMWARE_FRAMEBUFFER_BYTES, 8192);
  assert.equal(demoModule.FOX_PACK_BYTES, 24976);
  assert.deepEqual(demoModule.parseFirmwareRecords(
    'noise\nKITSU_SYNC {"status":"ok"}\nKITSU_SYNC not-json\n',
    "KITSU_SYNC",
  ), [{ status: "ok" }]);
  assert.deepEqual(demoModule.validateMeshMessage("  Hello Kitsu  "), {
    ok: true,
    error: "",
    text: "Hello Kitsu",
  });
  assert.equal(demoModule.validateMeshMessage("   ").ok, false);
  assert.equal(demoModule.validateMeshMessage("line\nbreak").ok, false);
  assert.equal(demoModule.validateMeshMessage("x".repeat(129)).ok, false);
  assert.throws(() => demoModule.decodeDebugView(new Uint32Array(39)), /wrong shape/i);

  const wasmPath = path.join(
    root,
    "demo",
    "kitsu-firmware-full.cdf1830000232c2e1f4492bd70fdb40b06f6a406ce4a63e1aa79c419ccdcaf73.wasm",
  );
  const foxPath = path.join(
    root,
    "demo",
    "assets",
    "fox.c868386770b6083dcd8f7c01ec7fe455faec476a96c724ab62f09770fdcdab38.k868",
  );
  const [wasmBytes, foxBytes] = await Promise.all([
    readFile(wasmPath),
    readFile(foxPath),
  ]);
  const module = await WebAssembly.compile(wasmBytes);
  let memory;
  const writeU32 = (pointer, value) => {
    if (memory && pointer) new DataView(memory.buffer).setUint32(pointer, value, true);
  };
  const instance = await WebAssembly.instantiate(module, {
    wasi_snapshot_preview1: {
      fd_close: () => 0,
      fd_write: (_fd, _iov, _count, written) => {
        writeU32(written, 0);
        return 0;
      },
      fd_seek: (_fd, _low, _high, _whence, result) => {
        if (memory && result) {
          const view = new DataView(memory.buffer);
          view.setUint32(result, 0, true);
          view.setUint32(result + 4, 0, true);
        }
        return 0;
      },
    },
  });
  const api = instance.exports;
  memory = api.memory;
  api._initialize();

  assert.equal(api.kitsu_emulator_abi_version(), 2);
  assert.equal(api.kitsu_emulator_set_device_id(0x55667788, 0x11223344), 1);
  assert.equal(api.kitsu_emulator_boot(), 0, "boot fails closed before host entropy");
  const entropy = Uint8Array.from({ length: 48 }, (_value, index) =>
    (index * 37 + 11) & 0xff);
  new Uint8Array(
    memory.buffer,
    api.kitsu_emulator_entropy_buffer(),
    entropy.length,
  ).set(entropy);
  assert.ok(entropy.length <= api.kitsu_emulator_entropy_capacity());
  assert.equal(api.kitsu_emulator_entropy_commit(entropy.length), 1);
  assert.ok(foxBytes.length <= api.kitsu_emulator_pack_capacity());
  new Uint8Array(
    memory.buffer,
    api.kitsu_emulator_pack_buffer(),
    foxBytes.length,
  ).set(foxBytes);
  assert.equal(api.kitsu_emulator_pack_commit(foxBytes.length), 1);
  assert.equal(api.kitsu_emulator_boot(), 1);
  for (let index = 0; index < 10; ++index) {
    assert.equal(api.kitsu_emulator_step(16), 1);
  }

  const readDebug = () => demoModule.decodeDebugView(Uint32Array.from(
    new Uint32Array(
      memory.buffer,
      api.kitsu_emulator_debug_view(),
      api.kitsu_emulator_debug_view_bytes() / Uint32Array.BYTES_PER_ELEMENT,
    ),
  ));
  const booted = readDebug();
  assert.deepEqual(
    [booted.screen, booted.energy, booted.curiosity, booted.affection],
    [0, 72, 14, 5],
  );
  assert.equal(booted.packValid, true);
  assert.equal(api.kitsu_emulator_framebuffer_width(), 64);
  assert.equal(api.kitsu_emulator_framebuffer_height(), 128);
  assert.equal(api.kitsu_emulator_framebuffer_bytes(), 8192);
  const framebuffer = new Uint8Array(
    memory.buffer,
    api.kitsu_emulator_framebuffer(),
    api.kitsu_emulator_framebuffer_bytes(),
  );
  assert.ok(framebuffer.some((pixel) => pixel !== 0), "real firmware renders the Fox portrait");

  api.kitsu_emulator_set_prg(1);
  assert.equal(api.kitsu_emulator_step(1), 1);
  assert.equal(api.kitsu_emulator_step(31), 1);
  assert.equal(api.kitsu_emulator_step(100), 1);
  api.kitsu_emulator_set_prg(0);
  assert.equal(api.kitsu_emulator_step(1), 1);
  assert.equal(api.kitsu_emulator_step(31), 1);
  const tapped = readDebug();
  assert.deepEqual(
    [tapped.screen, tapped.energy, tapped.curiosity, tapped.affection],
    [0, 76, 15, 8],
    "raw PRG timing reaches the production portrait tap action",
  );

  api.kitsu_emulator_set_prg(1);
  assert.equal(api.kitsu_emulator_step(1), 1);
  assert.equal(api.kitsu_emulator_step(31), 1);
  assert.equal(api.kitsu_emulator_step(800), 1);
  api.kitsu_emulator_set_prg(0);
  assert.equal(api.kitsu_emulator_step(1), 1);
  assert.equal(api.kitsu_emulator_step(31), 1);
  assert.equal(readDebug().screen, 1, "raw PRG hold enters the production menu");
});

test("keeps public release binaries outside text conversion", async () => {
  const attributes = await readFile(path.join(projectRoot, ".gitattributes"), "utf8");
  const lines = new Set(attributes.split(/\r?\n/));
  for (const rule of [
    "platform/public-site/downloads/*.json -text",
    "platform/public-site/downloads/*.json.sig binary",
    "platform/public-site/downloads/*.apk binary",
    "platform/public-site/demo/*.wasm binary",
    "platform/public-site/demo/assets/*.k868 binary",
    "platform/public-site/unlock/assets/*.k868 binary",
  ]) assert.ok(lines.has(rule), `missing binary attribute: ${rule}`);
});

test("publishes the byte-exact signed local-first Android 2.2.6 release", async () => {
  const manifestBytes = await readFile(path.join(root, "downloads", "latest.json"));
  const signature = await readFile(path.join(root, "downloads", "latest.json.sig"));
  const publicKeyPEM = await readFile(path.join(root, "downloads", "update-ed25519-public.pem"));
  const publicKey = createPublicKey(publicKeyPEM);
  const release = JSON.parse(manifestBytes);
  const script = await readFile(path.join(root, "site.js"), "utf8");

  assert.equal(signature.length, 64);
  assert.equal(verify(null, manifestBytes, publicKey, signature), true);
  assert.deepEqual(release, {
    schema: "kitsu.android-release.v1",
    status: "available",
    channel: "stable",
    buildType: "release",
    packageId: "ptl.kitsu.app",
    version: "2.2.6",
    versionCode: 27,
    minimumAndroidApi: 26,
    url: "/downloads/kitsu-android-2.2.6-12f462cd3d938ef9c32c5a9e4709d4c2.apk",
    bytes: 1921231,
    sha256: "12f462cd3d938ef9c32c5a9e4709d4c27120c44d357c15a4a4b5adc6f414ca47",
    signingCertificateSha256: "a5a3cddb0d2c103630c6e622ac7f2051085a4c082db37aefdbadfc75d0a2d7fc",
    publishedAt: "2026-08-29T01:58:05Z",
  });

  const publicJWK = publicKey.export({ format: "jwk" });
  assert.equal(publicJWK.x, "JAAR8Unpz7n7h_q02cpFc8HH_7OHF3ZYAAXsQa7lE4I");
  assert.match(script, /crypto\.subtle\.verify/);
  assert.match(script, /signature\.length !== 64/);
  assert.match(script, /release\.channel !== "stable"/);
  assert.match(script, /release\.buildType !== "release"/);
  assert.match(script, /release\.packageId !== REQUIRED_PACKAGE_ID/);
  assert.match(script, /url\.origin !== window\.location\.origin/);
  assert.match(script, /ANDROID_SIGNING_CERTIFICATE_SHA256/);
  assert.match(script, /release\.versionCode !== REQUIRED_VERSION_CODE/);
  assert.match(script, /release\.version !== REQUIRED_VERSION/);
  assert.match(script, /showReleaseFailure/);

  const apk = path.join(root, release.url.slice(1));
  const apkStat = await stat(apk);
  assert.equal(apkStat.isFile(), true);
  assert.equal(apkStat.size, release.bytes);
  assert.equal(await sha256(apk), release.sha256);
  assert.deepEqual([...await readFile(apk).then((bytes) => bytes.subarray(0, 4))], [0x50, 0x4b, 0x03, 0x04]);

  const downloadEntries = await readdir(path.join(root, "downloads"));
  assert.deepEqual(downloadEntries.filter((entry) => entry.toLowerCase().endsWith(".apk")).sort(), [
    "kitsu-android-2.1.5-72cd273f7e44402267ccd7a9bbbec9f2.apk",
    "kitsu-android-2.1.6-da081f9d09e6e4cd5747cbc53c655344.apk",
    "kitsu-android-2.2.0-36fe0a87aba10939ea9f6d6ae9b58242.apk",
    "kitsu-android-2.2.1-6e521a5f4ff1db1c21ab6997436ec1a0.apk",
    "kitsu-android-2.2.3-5575ce664d9fd262ea705cb7b02676d5.apk",
    "kitsu-android-2.2.4-b23727da3603fdc5b35b3146fb8d3eb1.apk",
    "kitsu-android-2.2.5-4f0cc3bb5fd1059c9d6096a41350aa8f.apk",
    "kitsu-android-2.2.6-12f462cd3d938ef9c32c5a9e4709d4c2.apk",
    "kitsu-k32-android-2.0.0.apk",
  ]);
  assert.equal(downloadEntries.some((entry) => /private|keystore|\.jks$/i.test(entry)), false);
});

test("archives the previous stable manifest and signature under immutable 2.0.0 names", async () => {
  const downloads = path.join(root, "downloads");
  const archivedManifest = path.join(downloads, "android-stable-2.0.0-20260822t123928z.json");
  const archivedSignature = path.join(downloads, "android-stable-2.0.0-20260822t123928z.json.sig");
  const publicKey = createPublicKey(await readFile(path.join(downloads, "update-ed25519-public.pem")));
  const manifestBytes = await readFile(archivedManifest);
  const signatureBytes = await readFile(archivedSignature);

  assert.equal(manifestBytes.length, 512);
  assert.equal(await sha256(archivedManifest), "a9b846ab16ef6534d13316ba7ecc7f5ce354209e0deeb2f71bec211190db868d");
  assert.equal(signatureBytes.length, 64);
  assert.equal(await sha256(archivedSignature), "56828bffdaed27cca0cac4ca73a403a31b1448c6937131aa43aa0ab5bf399d98");
  assert.equal(verify(null, manifestBytes, publicKey, signatureBytes), true);
});

test("archives the previous stable manifest and signature under immutable 2.2.0 names", async () => {
  const downloads = path.join(root, "downloads");
  const archivedManifest = path.join(downloads, "android-stable-2.2.0-20260825t170821z.json");
  const archivedSignature = path.join(downloads, "android-stable-2.2.0-20260825t170821z.json.sig");
  const publicKey = createPublicKey(await readFile(path.join(downloads, "update-ed25519-public.pem")));
  const manifestBytes = await readFile(archivedManifest);
  const signatureBytes = await readFile(archivedSignature);

  assert.equal(manifestBytes.length, 538);
  assert.equal(await sha256(archivedManifest), "74a27b896fa5099cf8d094cf23da999b591d067a5b5dcfe031efe23b5bc19e46");
  assert.equal(signatureBytes.length, 64);
  assert.equal(await sha256(archivedSignature), "7a4614473ec74cbe3dd19870499db73e4dfdcc93b0ff980d211dd78ae7c98e54");
  assert.equal(verify(null, manifestBytes, publicKey, signatureBytes), true);
});

test("archives the previous stable manifest and signature under immutable 2.2.1 names", async () => {
  const downloads = path.join(root, "downloads");
  const archivedManifest = path.join(downloads, "android-stable-2.2.1-20260826t002057z.json");
  const archivedSignature = path.join(downloads, "android-stable-2.2.1-20260826t002057z.json.sig");
  const publicKey = createPublicKey(await readFile(path.join(downloads, "update-ed25519-public.pem")));
  const manifestBytes = await readFile(archivedManifest);
  const signatureBytes = await readFile(archivedSignature);

  assert.equal(manifestBytes.length, 538);
  assert.equal(await sha256(archivedManifest), "08a0be9c73345144fd65e878e8cad00741129e3578ae5a6cad4c567eed0c8264");
  assert.equal(signatureBytes.length, 64);
  assert.equal(await sha256(archivedSignature), "29e98dccf17819a8971d166e07cde59b13c18e01a5d1f30a3e5a27f42e16640f");
  assert.equal(verify(null, manifestBytes, publicKey, signatureBytes), true);
});

test("archives the previous stable manifest and signature under immutable 2.2.3 names", async () => {
  const downloads = path.join(root, "downloads");
  const archivedManifest = path.join(downloads, "android-stable-2.2.3-20260826t132716z.json");
  const archivedSignature = path.join(downloads, "android-stable-2.2.3-20260826t132716z.json.sig");
  const publicKey = createPublicKey(await readFile(path.join(downloads, "update-ed25519-public.pem")));
  const manifestBytes = await readFile(archivedManifest);
  const signatureBytes = await readFile(archivedSignature);

  assert.equal(manifestBytes.length, 538);
  assert.equal(await sha256(archivedManifest), "08c6f428cdd80628e8ed6f2f4c448b1d732748ad7f4211a38e1db2c5ff73d9de");
  assert.equal(signatureBytes.length, 64);
  assert.equal(await sha256(archivedSignature), "06a60d266a9c84006757813bf09e9d84eeccfd7964fce5e5da68ea8d1dd6436f");
  assert.equal(verify(null, manifestBytes, publicKey, signatureBytes), true);
});

test("archives the previous stable manifest and signature under immutable 2.2.4 names", async () => {
  const downloads = path.join(root, "downloads");
  const archivedManifest = path.join(downloads, "android-stable-2.2.4-20260828t092529z.json");
  const archivedSignature = path.join(downloads, "android-stable-2.2.4-20260828t092529z.json.sig");
  const publicKey = createPublicKey(await readFile(path.join(downloads, "update-ed25519-public.pem")));
  const manifestBytes = await readFile(archivedManifest);
  const signatureBytes = await readFile(archivedSignature);

  assert.equal(manifestBytes.length, 538);
  assert.equal(await sha256(archivedManifest), "5e9310748dc2ace692b30f6a7e056043ddda442e9f414ec429b5a67d117ae1c5");
  assert.equal(signatureBytes.length, 64);
  assert.equal(await sha256(archivedSignature), "45b8b701f8a45df7913ddc514f3ff62118e4bfe3586d6deb6cd524c6c1619efb");
  assert.equal(verify(null, manifestBytes, publicKey, signatureBytes), true);
});

test("archives the previous stable manifest and signature under immutable 2.2.5 names", async () => {
  const downloads = path.join(root, "downloads");
  const archivedManifest = path.join(downloads, "android-stable-2.2.5-20260828t143256z.json");
  const archivedSignature = path.join(downloads, "android-stable-2.2.5-20260828t143256z.json.sig");
  const publicKey = createPublicKey(await readFile(path.join(downloads, "update-ed25519-public.pem")));
  const manifestBytes = await readFile(archivedManifest);
  const signatureBytes = await readFile(archivedSignature);

  assert.equal(manifestBytes.length, 538);
  assert.equal(await sha256(archivedManifest), "53dd92658cb8136c463774005763341373acfa4bf37edddbf3cc101462134a25");
  assert.equal(signatureBytes.length, 64);
  assert.equal(await sha256(archivedSignature), "c573259bb72cd213e507d58f71b8b4feb6c55b88e4bb27754cb52cd9b129e7d2");
  assert.equal(verify(null, manifestBytes, publicKey, signatureBytes), true);
});

test("ships every referenced local release asset", async () => {
  const files = [
    "styles.css",
    "theme.js",
    "site.js",
    "preview-release.js",
    "demo/index.html",
    "demo/demo.css",
    "demo/demo.js",
    "demo/kitsu-firmware-full.cdf1830000232c2e1f4492bd70fdb40b06f6a406ce4a63e1aa79c419ccdcaf73.wasm",
    "demo/assets/fox-48-frame-contact.png",
    "demo/assets/fox.c868386770b6083dcd8f7c01ec7fe455faec476a96c724ab62f09770fdcdab38.k868",
    "config.json",
    "assets/kitsu-app-icon.png",
    "assets/kitsu-k32-social-card-v1.png",
    "downloads/latest.json",
    "downloads/latest.json.sig",
    "downloads/update-ed25519-public.pem",
    "downloads/kitsu-android-2.1.5-72cd273f7e44402267ccd7a9bbbec9f2.apk",
    "downloads/kitsu-android-2.1.6-da081f9d09e6e4cd5747cbc53c655344.apk",
    "downloads/kitsu-android-2.2.0-36fe0a87aba10939ea9f6d6ae9b58242.apk",
    "downloads/kitsu-android-2.2.1-6e521a5f4ff1db1c21ab6997436ec1a0.apk",
    "downloads/kitsu-android-2.2.3-5575ce664d9fd262ea705cb7b02676d5.apk",
    "downloads/kitsu-android-2.2.4-b23727da3603fdc5b35b3146fb8d3eb1.apk",
    "downloads/kitsu-android-2.2.5-4f0cc3bb5fd1059c9d6096a41350aa8f.apk",
    "downloads/kitsu-android-2.2.6-12f462cd3d938ef9c32c5a9e4709d4c2.apk",
    "downloads/kitsu-k32-android-2.0.0.apk",
    "downloads/android-stable-2.0.0-20260822t123928z.json",
    "downloads/android-stable-2.0.0-20260822t123928z.json.sig",
    "downloads/android-stable-2.2.0-20260825t170821z.json",
    "downloads/android-stable-2.2.0-20260825t170821z.json.sig",
    "downloads/android-stable-2.2.1-20260826t002057z.json",
    "downloads/android-stable-2.2.1-20260826t002057z.json.sig",
    "downloads/android-stable-2.2.3-20260826t132716z.json",
    "downloads/android-stable-2.2.3-20260826t132716z.json.sig",
    "downloads/android-stable-2.2.4-20260828t092529z.json",
    "downloads/android-stable-2.2.4-20260828t092529z.json.sig",
    "downloads/android-stable-2.2.5-20260828t143256z.json",
    "downloads/android-stable-2.2.5-20260828t143256z.json.sig",
  ];
  await Promise.all(files.map((file) => access(path.join(root, file))));
});

test("publishes factual local-first policies without a runtime-service form", async () => {
  const [home, privacy, terms, security, contact, rootSecurity] = await Promise.all([
    readFile(path.join(root, "index.html"), "utf8"),
    readFile(path.join(root, "privacy", "index.html"), "utf8"),
    readFile(path.join(root, "terms", "index.html"), "utf8"),
    readFile(path.join(root, "security", "index.html"), "utf8"),
    readFile(path.join(root, "contact", "index.html"), "utf8"),
    readFile(path.join(projectRoot, "SECURITY.md"), "utf8"),
  ]);
  for (const href of ["/privacy/", "/terms/", "/security/", "/contact/"]) assert.match(home, new RegExp(`href=["']${href.replaceAll("/", "\\/")}`));
  assert.match(privacy, /does not have permission to access the Internet/i);
  assert.match(privacy, /does not upload analytics/i);
  assert.match(privacy, /signed Bluetooth firmware updates/i);
  assert.match(security, /private vulnerability reporting/i);
  assert.match(contact, /github\.com\/pattalium\/Kitsu\/issues/i);
  assert.match(contact, /New public issues are currently restricted/i);
  assert.match(contact, /github\.com\/pattalium\/Kitsu\/pulls/i);
  assert.match(contact, /github\.com\/pattalium\/Kitsu\/security\/advisories\/new/i);
  assert.match(rootSecurity, /github\.com\/pattalium\/Kitsu\/security\/advisories\/new/i);
  assert.match(rootSecurity, /owner-reflashable/i);
  assert.match(rootSecurity, /does not promise a bug bounty or a fixed response deadline/i);
  assert.doesNotMatch(contact, /<form\b|api\.k32\.run|policy\.js/i);
  assert.doesNotMatch(contact, /<script>(?:.|\n)*<\/script>/);
  assert.doesNotMatch(`${privacy}${terms}${security}${contact}`, /private machine|private address|mailto:|placeholder|app\.k32\.run|auth\.k32\.run/i);
  assert.doesNotMatch(rootSecurity, /guaranteed response|guaranteed bounty|tamper[- ]proof/i);
});

test("all local page, asset, and fragment links resolve", async () => {
  const pages = ["index.html", "demo/index.html", "unlock/index.html", "privacy/index.html", "terms/index.html", "security/index.html", "contact/index.html"];
  for (const page of pages) {
    const html = await readFile(path.join(root, page), "utf8");
    for (const [, reference] of html.matchAll(/(?:href|src)=["']([^"']+)["']/g)) {
      const target = new URL(reference, `https://k32.run/${page}`);
      if (target.origin !== "https://k32.run") continue;
      const relative = target.pathname === "/"
        ? "index.html"
        : target.pathname.endsWith("/")
          ? `${target.pathname.slice(1)}index.html`
          : target.pathname.slice(1);
      const destination = path.join(root, relative);
      assert.equal((await stat(destination)).isFile(), true, `${page}: ${reference}`);
      if (target.hash) {
        const targetText = await readFile(destination, "utf8");
        assert.match(targetText, new RegExp(`id=["']${target.hash.slice(1)}["']`), `${page}: ${reference}`);
      }
    }
  }
});

test("public pages contain no retired runtime-service actions or origins", async () => {
  const text = (await Promise.all([
    "index.html", "demo/index.html", "privacy/index.html", "terms/index.html", "security/index.html", "contact/index.html",
  ].map((relative) => readFile(path.join(root, relative), "utf8")))).join("\n");
  assert.doesNotMatch(text, /https:\/\/(?:app|api|auth|gateway)\.k32\.run/i);
  assert.doesNotMatch(text, /Connect to public gateway|Use Wi-Fi remote access|gateway enrollment|owner sign-in|contact form/i);
});

test("uses byte-exact authoritative K32 brand assets", async () => {
  const pairs = [
    ["kitsu-app-icon.png", "kitsu-app-icon.png"],
    ["kitsu-k32-social-card-v1.png", "kitsu-k32-social-card-v1.png"],
  ];
  for (const [canonicalName, publicName] of pairs) {
    const canonical = path.join(projectRoot, "assets", "brand", canonicalName);
    const published = path.join(root, "assets", publicName);
    assert.equal(await sha256(published), await sha256(canonical));
  }
});

test("all public text assets are valid UTF-8 without mojibake", async () => {
  const paths = ["index.html", "styles.css", "theme.js", "site.js", "preview-release.js", "README.md", "demo/index.html", "demo/demo.css", "demo/demo.js", "unlock/index.html", "unlock/unlock.css", "unlock/unlock.js", "unlock/catalog.js", "privacy/index.html", "terms/index.html", "security/index.html", "contact/index.html"];
  for (const relative of paths) assert.doesNotMatch(await readFile(path.join(root, relative), "utf8"), /(?:Ã‚|Ã¢â‚¬|Ã¢â‚¬â„¢|Ã¢â€ |Ã¢â€¡|Ã¢â„¢|Ã¢â€”|Ã¢Å“|Ã¢Å’|Ãƒ|ï¿½)/, relative);
});

test("keeps the unlock flow discoverable and accepts the Android deep-link shape", async () => {
  const pages = [
    "index.html", "demo/index.html", "unlock/index.html", "privacy/index.html",
    "terms/index.html", "security/index.html", "contact/index.html",
  ];
  const styleDigest = await sha256(path.join(root, "styles.css"));
  for (const page of pages) {
    const html = await readFile(path.join(root, page), "utf8");
    assert.match(html, /href="\/unlock\/"/, `${page}: unlock link`);
    assert.match(
      html,
      new RegExp(`href="\\/styles\\.css\\?sha256=${styleDigest}"`),
      `${page}: current shared stylesheet digest`,
    );
  }

  const deepLink = new URL("https://k32.run/unlock/#code=K8-ABCDE-FGHJK-MNPQR");
  assert.equal(deepLink.pathname, "/unlock/");
  assert.equal(deepLink.search, "");
  assert.equal(deepLink.hash, "#code=K8-ABCDE-FGHJK-MNPQR");
  assert.equal(
    unlockModule.unlockCodeFromFragment(deepLink.hash),
    "K8-ABCDE-FGHJK-MNPQR",
  );
  assert.equal(unlockModule.unlockCodeFromFragment("?code=K8-ABCDE-FGHJK-MNPQR"), null);
  assert.equal(unlockModule.unlockCodeFromFragment("#code=K8-ABCDE-FGHJK-MNPQR&extra=1"), null);
  assert.equal(unlockModule.unlockCodeFromFragment("#code=k8-abcde-fghjk-mnpqr"), null);

  const [unlockScript, activity, models] = await Promise.all([
    readFile(path.join(root, "unlock", "unlock.js"), "utf8"),
    readFile(path.join(projectRoot, "platform", "mobile", "android", "app", "src", "main", "java", "ptl", "kitsu", "app", "MainActivity.kt"), "utf8"),
    readFile(path.join(projectRoot, "platform", "mobile", "android", "app", "src", "main", "java", "ptl", "kitsu", "app", "model", "EncounterModels.kt"), "utf8"),
  ]);
  assert.doesNotMatch(unlockScript, /searchParams\.get\("code"\)/);
  assert.match(unlockScript, /unlockCodeFromFragment\(url\.hash\)/);
  assert.match(unlockScript, /url\.hash = ""/);
  assert.match(unlockScript, /history\.replaceState\(null, "", `\$\{url\.pathname\}\$\{url\.search\}`\)/);
  assert.match(activity, /KITSU_UNLOCK_URL = "https:\/\/k32\.run\/unlock\/"/);
  assert.match(activity, /encodedFragment\("code=\$\{Uri\.encode\(code\)\}"\)/);
  assert.doesNotMatch(activity, /appendQueryParameter\("code", code\)/);
  assert.match(activity, /require\(EncounterCodePolicy\.validCode\(code\)\)/);
  assert.ok(models.includes(
    'Regex("^K8-[0123456789ABCDEFGHJKMNPQRSTVWXYZ]{5}-[0123456789ABCDEFGHJKMNPQRSTVWXYZ]{5}-[0123456789ABCDEFGHJKMNPQRSTVWXYZ]{5}$")',
  ));
  assert.match(models, /fun validCode\(value: String\): Boolean = unlockCode\.matches\(value\)/);
});

test("ships a pinned, rollback-capable atomic unlock deployment script", async () => {
  const deploy = await readFile(path.join(projectRoot, "tools", "deploy_public_unlock_atomic.sh"), "utf8");
  assert.match(deploy, /^#!\/usr\/bin\/env bash\nset -Eeuo pipefail/);
  assert.match(deploy, /flock -n 9/);
  assert.match(deploy, /source_digest_mismatch/);
  assert.match(deploy, /\.aab.*\.idsig.*\.jks.*\.keystore/);
  assert.match(deploy, /PRIVATE KEY/);
  assert.match(deploy, /openssl pkeyutl -verify -pubin/);
  assert.match(deploy, /mv -Tf -- "\$next_link" "\$current"/);
  assert.match(deploy, /KITSU_PUBLIC_UNLOCK_ROLLBACK_OK/);
  assert.match(deploy, /\/unlock\/#code=K8-ABCDE-FGHJK-MNPQR/);
  assert.doesNotMatch(deploy, /\/unlock\/\?code=/);
  assert.doesNotMatch(deploy, /sshpass|password\s*=|-----BEGIN (?:RSA |EC )?PRIVATE KEY-----/i);
});

test("publishes an accessible, fail-closed local unlock surface", async () => {
  const unlockRoot = path.join(root, "unlock");
  const [html, styles, script, catalog] = await Promise.all([
    readFile(path.join(unlockRoot, "index.html"), "utf8"),
    readFile(path.join(unlockRoot, "unlock.css"), "utf8"),
    readFile(path.join(unlockRoot, "unlock.js"), "utf8"),
    readFile(path.join(unlockRoot, "catalog.js"), "utf8"),
  ]);

  assert.match(html, /<label for="unlock-code">Encounter code<\/label>/);
  assert.match(html, /id="unlock-code"[^>]*aria-describedby="unlock-code-help unlock-code-error"/s);
  assert.match(html, /id="connect-kitsu" type="button"/);
  assert.match(html, /id="verify-code" type="submit" disabled/);
  assert.match(html, /id="unlock-status" role="status" aria-live="polite" aria-atomic="true" tabindex="-1"/);
  assert.match(html, /id="unlock-result"[^>]*aria-busy="false"/);
  assert.match(styles, /\.unlock-action\s*\{[^}]*min-width:[^}]*white-space:\s*nowrap/s);
  assert.match(styles, /@media \(max-width: 680px\)[\s\S]*\.unlock-action[\s\S]*width:\s*100%/);
  assert.match(styles, /\.unlock-status\[data-state="unsupported"\]/);

  for (const stateCopy of [
    "Web Serial is not available in this browser. Use Chrome or Edge on a desktop computer.",
    "Connection permission was not granted. Choose Connect Kitsu to try again.",
    "Kitsu is disconnected. Reconnect it before verifying the code.",
    "This code was not accepted by the connected Kitsu. Check the saved code or connect the Heltec that received it.",
    "Code accepted for this Kitsu. The matching pet pack has not been published yet. Keep the code saved and check again after publication.",
    "Code accepted. The matching pet pack is ready to download.",
  ]) assert.ok(script.includes(stateCopy), stateCopy);

  const scriptTags = [...html.matchAll(/<script\b([^>]*)>([\s\S]*?)<\/script>/gi)];
  assert.ok(scriptTags.length >= 2);
  for (const [, attributes, body] of scriptTags) {
    assert.match(attributes, /\bsrc="[^"]+"/);
    assert.equal(body.trim(), "");
  }
  assert.doesNotMatch(html, /<style\b|\sstyle=/i);
  assert.doesNotMatch(script, /\.innerHTML\b|insertAdjacentHTML|\beval\s*\(|new Function\b/);
  const ownerPrivateName = ["fox", "girl"].join("\\s+");
  assert.doesNotMatch(`${html}\n${script}\n${catalog}`, new RegExp(ownerPrivateName, "i"));
  assert.equal(
    [...`${html}\n${script}\n${catalog}`.matchAll(/https?:\/\/[^\s"'`;,)]+/gi)]
      .map((match) => match[0])
      .filter((url) => url.startsWith("https://api.k32.run"))
      .every((url) => url === "https://api.k32.run/v1/pet-packs/redeem" || url === "https://api.k32.run"),
    true,
  );
  assert.doesNotMatch(`${script}\n${catalog}`, /\.downloadUrl\b|["']downloadUrl["']\s*:|(?:\.\/|\/unlock\/)assets\/[^\s"']+\.k868/i);
  assert.match(html, /Content-Security-Policy/);
  assert.match(html, /connect-src https:\/\/api\.k32\.run/);
  assert.match(html, /No account is required/);
  assert.match(html, /firmware verification record are sent only to the K32 API/);
  assert.match(html, /maxlength="20"/);
  assert.match(html, /placeholder="K8-XXXXX-XXXXX-XXXXX"/);
  assert.match(html, /href="https:\/\/flash\.k32\.run"/);

  const cssDigest = html.match(/href="\/unlock\/unlock\.css\?sha256=([a-f0-9]{64})"/)?.[1];
  const scriptDigest = html.match(/src="\/unlock\/unlock\.js\?sha256=([a-f0-9]{64})"/)?.[1];
  const catalogDigest = script.match(/\.\/catalog\.js\?sha256=([a-f0-9]{64})/)?.[1];
  assert.equal(cssDigest, await sha256(path.join(unlockRoot, "unlock.css")));
  assert.equal(scriptDigest, await sha256(path.join(unlockRoot, "unlock.js")));
  assert.equal(catalogDigest, await sha256(path.join(unlockRoot, "catalog.js")));

  const unlockFiles = await listFiles(unlockRoot);
  const publishedPacks = unlockFiles.filter((file) => file.toLowerCase().endsWith(".k868"));
  assert.deepEqual(publishedPacks, []);
});

test("normalizes unlock codes without allowing command injection", () => {
  const requestId = "0123456789abcdef0123456789abcdef";
  assert.equal(
    unlockModule.normalizeUnlockCode("  k8 abcde-fghjk mnpqr  "),
    "K8-ABCDE-FGHJK-MNPQR",
  );
  assert.equal(
    unlockModule.normalizeUnlockCode("ABCDE-FGHJK-MNPQR"),
    "K8-ABCDE-FGHJK-MNPQR",
  );
  assert.equal(unlockModule.normalizeUnlockCode("ABCD<script>"), null);
  assert.equal(unlockModule.normalizeUnlockCode("ABCD\nEFGH"), null);
  assert.equal(unlockModule.normalizeUnlockCode("ABCDE-FGHJI-MNPQR"), null);
  assert.equal(unlockModule.normalizeUnlockCode("A".repeat(14)), null);
  assert.equal(unlockModule.normalizeUnlockCode("A".repeat(16)), null);
  assert.equal(
    unlockModule.buildVerificationCommand("k8 abcde-fghjk mnpqr", requestId),
    `codes verify K8-ABCDE-FGHJK-MNPQR ${requestId}\n`,
  );
  assert.throws(
    () => unlockModule.buildVerificationCommand("ABCD;reset", requestId),
    (error) => error.code === "invalid_code",
  );

  const generated = unlockModule.createRequestId({
    getRandomValues(bytes) {
      bytes.forEach((_value, index) => { bytes[index] = index; });
      return bytes;
    },
  });
  assert.equal(generated, "000102030405060708090a0b0c0d0e0f");
});

test("parses only bounded, versioned verification records for the active request", () => {
  const requestId = "0123456789abcdef0123456789abcdef";
  const valid = {
    schema: unlockModule.VERIFY_SCHEMA,
    requestId,
    status: "valid",
    deviceId: "KT12AF",
    boundDeviceId: "KT12AF",
    codeId: "C0DE1234",
    packId: "A1B2C3D4",
    rarity: "rare",
  };
  const parsed = unlockModule.parseVerificationLine(
    `${unlockModule.VERIFY_MARKER}${JSON.stringify(valid)}\r`,
    requestId,
  );
  assert.deepEqual(parsed, {
    boundDeviceId: "KT12AF",
    codeId: "C0DE1234",
    deviceId: "KT12AF",
    packId: "A1B2C3D4",
    rarity: "rare",
    requestId,
    status: "valid",
  });

  const invalid = {
    schema: unlockModule.VERIFY_SCHEMA,
    requestId,
    status: "invalid",
    deviceId: "KT12AF",
  };
  assert.deepEqual(
    unlockModule.parseVerificationLine(
      `${unlockModule.VERIFY_MARKER}${JSON.stringify(invalid)}`,
      requestId,
    ),
    { deviceId: "KT12AF", requestId, status: "invalid" },
  );
  assert.equal(unlockModule.parseVerificationLine("ordinary firmware log", requestId), null);
  assert.throws(
    () => unlockModule.parseVerificationLine(
      `${unlockModule.VERIFY_MARKER}${"x".repeat(unlockModule.MAX_SERIAL_LINE_CHARS)}`,
      requestId,
    ),
    (error) => error.code === "response_too_large",
  );
  assert.throws(
    () => unlockModule.parseVerificationLine(
      `${unlockModule.VERIFY_MARKER}${JSON.stringify({ ...valid, extra: true })}`,
      requestId,
    ),
    (error) => error.code === "malformed_response",
  );
  assert.throws(
    () => unlockModule.parseVerificationLine(
      `${unlockModule.VERIFY_MARKER}${JSON.stringify({ ...valid, requestId: "f".repeat(32) })}`,
      requestId,
    ),
    (error) => error.code === "request_mismatch",
  );
  assert.throws(
    () => unlockModule.parseVerificationLine(`${unlockModule.VERIFY_MARKER}{`, requestId),
    (error) => error.code === "malformed_response",
  );
});

test("rejects a valid-code response bound to a different Kitsu", () => {
  const requestId = "0123456789abcdef0123456789abcdef";
  const response = {
    schema: unlockModule.VERIFY_SCHEMA,
    requestId,
    status: "valid",
    deviceId: "KT12AF",
    boundDeviceId: "KT98BC",
    codeId: "C0DE1234",
    packId: "A1B2C3D4",
    rarity: "mythical",
  };
  assert.throws(
    () => unlockModule.parseVerificationLine(
      `${unlockModule.VERIFY_MARKER}${JSON.stringify(response)}`,
      requestId,
    ),
    (error) => error.code === "device_mismatch",
  );
});

test("publishes the exact accepted 21-creature portrait metadata for gated wild packs", async () => {
  const portraitManifest = JSON.parse(
    await readFile(path.join(projectRoot, "assets", "wild-portraits-manifest.json"), "utf8"),
  );
  const expectedCatalog = portraitManifest.creatures.map((creature) => ({
    schema: unlockCatalogModule.PUBLISHED_PACK_SCHEMA,
    packId: creature.pack_id,
    displayName: creature.display_name,
    rarity: creature.rarity,
    slug: creature.slug,
    portraitUrl: `./portraits/${creature.portrait_png}`,
    portraitSha256: creature.portrait_png_sha256,
    bytes: creature.pack_bytes,
    sha256: creature.pack_sha256,
  }));

  assert.equal(portraitManifest.schema, "kitsu-wild-static-portraits-v1");
  assert.equal(portraitManifest.creatures.length, 21);
  assert.equal(unlockCatalogModule.PUBLISHED_WILD_PACKS.length, 21);
  assert.deepEqual(unlockCatalogModule.PUBLISHED_WILD_PACKS, expectedCatalog);
  for (const rarity of unlockCatalogModule.RARITIES) {
    assert.equal(portraitManifest.creatures.filter((entry) => entry.rarity === rarity).length, 3, rarity);
  }

  for (const entry of unlockCatalogModule.PUBLISHED_WILD_PACKS) {
    assert.equal(unlockCatalogModule.isPublishedPackEntry(entry), true);
    assert.equal(unlockCatalogModule.publishedPackFor(entry.packId), entry);
    assert.equal("downloadUrl" in entry, false);
    assert.match(entry.portraitUrl, /^\.\/portraits\//);
    assert.equal(
      await sha256(path.join(root, "unlock", entry.portraitUrl.slice(2))),
      entry.portraitSha256,
    );
  }
  const starterPackIds = {
    Cat: "FDC79D6F",
    Fox: "6C393E21",
    Dog: "E2B5E7BA",
  };
  for (const [name, reservedId] of Object.entries(starterPackIds)) {
    assert.equal(unlockCatalogModule.publishedPackFor(reservedId), null, name);
  }

  const digest = "a".repeat(64);
  const entry = {
    schema: unlockCatalogModule.PUBLISHED_PACK_SCHEMA,
    packId: "A1B2C3D4",
    displayName: "Published pet pack",
    rarity: "rare",
    slug: "published-pet-pack",
    portraitUrl: `./portraits/published-pet-pack.${digest}.png`,
    portraitSha256: digest,
    bytes: 24_976,
    sha256: digest,
  };
  assert.equal(unlockCatalogModule.isPublishedPackEntry(entry), true);
  assert.deepEqual(
    unlockCatalogModule.publishedPackFor("a1b2c3d4", [entry]),
    entry,
  );
  assert.equal(
    unlockCatalogModule.isPublishedPackEntry({ ...entry, packId: "FDC79D6F" }),
    false,
  );
  assert.equal(
    unlockCatalogModule.isPublishedPackEntry({ ...entry, portraitUrl: "https://example.test/pet.png" }),
    false,
  );
  assert.equal(
    unlockCatalogModule.isPublishedPackEntry({ ...entry, downloadUrl: "./assets/unsafe.k868" }),
    false,
  );
  assert.equal(unlockCatalogModule.publishedPackFor("FDC79D6F"), null, "Cat is not a wild unlock");
  assert.equal(unlockCatalogModule.publishedPackFor("E2B5E7BA"), null, "Dog is not a wild unlock");
});

test("redeems a verified pack only through the bounded gated API response", async () => {
  const bytes = syntheticK868();
  const digest = createHash("sha256").update(bytes).digest("hex");
  const entry = Object.freeze({
    schema: unlockCatalogModule.PUBLISHED_PACK_SCHEMA,
    packId: "A1B2C3D4",
    displayName: "Published pet pack",
    rarity: "rare",
    slug: "published-pet-pack",
    portraitUrl: `./portraits/published-pet-pack.${"b".repeat(64)}.png`,
    portraitSha256: "b".repeat(64),
    bytes: bytes.byteLength,
    sha256: digest,
  });
  const verification = Object.freeze({
    boundDeviceId: "KT12AF",
    codeId: "C0DE1234",
    deviceId: "KT12AF",
    packId: "A1B2C3D4",
    rarity: "rare",
    requestId: "0123456789abcdef0123456789abcdef",
    status: "valid",
  });
  let captured;
  const result = await unlockModule.redeemPublishedPack(
    async (url, init) => {
      captured = { url, init };
      return new Response(bytes, {
        status: 200,
        headers: {
          "Cache-Control": "private, no-store",
          "Content-Disposition": "attachment; filename=\"kitsu-published-pet-pack.k868\"",
          "Content-Length": String(bytes.byteLength),
          "Content-Type": "application/octet-stream",
          "X-Kitsu-Pack-Id": "A1B2C3D4",
          "X-Kitsu-Pack-Sha256": digest,
        },
      });
    },
    "k8 abcde-fghjk mnpqr",
    verification,
    entry,
    webcrypto,
  );

  assert.equal(captured.url, "https://api.k32.run/v1/pet-packs/redeem");
  assert.equal(captured.init.method, "POST");
  assert.equal(captured.init.cache, "no-store");
  assert.equal(captured.init.credentials, "omit");
  assert.equal(captured.init.referrerPolicy, "no-referrer");
  assert.deepEqual(JSON.parse(captured.init.body), {
    schema: unlockModule.REDEMPTION_SCHEMA,
    code: "K8-ABCDE-FGHJK-MNPQR",
    verification: {
      schema: unlockModule.VERIFY_SCHEMA,
      ...verification,
    },
  });
  assert.equal(result.filename, "kitsu-published-pet-pack.k868");
  assert.equal(result.bytes.byteLength, bytes.byteLength);
  assert.equal(createHash("sha256").update(result.bytes).digest("hex"), digest);

  await assert.rejects(
    unlockModule.redeemPublishedPack(
      async () => new Response(JSON.stringify({ error: { code: "not_published" } }), { status: 404 }),
      "K8-ABCDE-FGHJK-MNPQR",
      verification,
      entry,
      webcrypto,
    ),
    (error) => error.code === "pack_unpublished",
  );
  await assert.rejects(
    unlockModule.redeemPublishedPack(
      async () => new Response(bytes, {
        status: 200,
        headers: {
          "Cache-Control": "private, no-store",
          "Content-Disposition": "attachment; filename=\"kitsu-published-pet-pack.k868\"",
          "Content-Length": String(unlockModule.MAX_PACK_RESPONSE_BYTES + 1),
          "Content-Type": "application/octet-stream",
          "X-Kitsu-Pack-Id": "A1B2C3D4",
          "X-Kitsu-Pack-Sha256": digest,
        },
      }),
      "K8-ABCDE-FGHJK-MNPQR",
      verification,
      entry,
      webcrypto,
    ),
    (error) => error.code === "invalid_download",
  );
  await assert.rejects(
    unlockModule.redeemPublishedPack(
      async () => new Response(bytes, {
        status: 200,
        headers: {
          "Cache-Control": "private, no-store",
          "Content-Disposition": "attachment; filename=\"kitsu-published-pet-pack.k868\"",
          "Content-Type": "application/octet-stream",
          "X-Kitsu-Pack-Id": "DEADBEEF",
          "X-Kitsu-Pack-Sha256": digest,
        },
      }),
      "K8-ABCDE-FGHJK-MNPQR",
      verification,
      entry,
      webcrypto,
    ),
    (error) => error.code === "invalid_download",
  );
});

test("accepts native 64x80 K868PK1 v2 downloads without weakening v1", () => {
  const v1 = syntheticK868("A1B2C3D4", 1);
  const v2 = syntheticK868("A1B2C3D4", 2);
  assert.equal(v1.byteLength, 24_976);
  assert.equal(v2.byteLength, 31_120);
  assert.equal(unlockModule.validateK868Pack(v1, "A1B2C3D4").version, 1);
  assert.equal(unlockModule.validateK868Pack(v2, "A1B2C3D4").version, 2);

  const mismatched = v2.slice();
  new DataView(mismatched.buffer).setUint16(0x22, 64, true);
  assert.throws(
    () => unlockModule.validateK868Pack(mismatched, "A1B2C3D4"),
    (error) => error.code === "invalid_download",
  );
});

test("reports Web Serial absence before any device request", () => {
  assert.equal(unlockModule.supportsWebSerial(undefined), false);
  assert.equal(unlockModule.supportsWebSerial({}), false);
  assert.equal(unlockModule.supportsWebSerial({ serial: {} }), false);
  assert.equal(
    unlockModule.supportsWebSerial({ serial: { requestPort() {} } }),
    true,
  );
  assert.equal(
    unlockModule.supportsWebSerial({ serial: { requestPort() {} } }, false),
    false,
  );
});
