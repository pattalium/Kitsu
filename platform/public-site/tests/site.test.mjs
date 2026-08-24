import assert from "node:assert/strict";
import { createHash, createPublicKey, generateKeyPairSync, sign, verify, webcrypto } from "node:crypto";
import { access, readFile, readdir, stat } from "node:fs/promises";
import path from "node:path";
import test from "node:test";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const projectRoot = path.resolve(root, "..", "..");
const previewModule = await import(pathToFileURL(path.join(root, "preview-release.js")));

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

test("fails closed outside the exact Android 2.1.6 production contract", async () => {
  const [html, script, readme] = await Promise.all([
    readFile(path.join(root, "index.html"), "utf8"),
    readFile(path.join(root, "site.js"), "utf8"),
    readFile(path.join(root, "README.md"), "utf8"),
  ]);
  assert.match(html, /Install the signed Android app/i);
  assert.match(html, /Kitsu Android 2\.1\.6 · version code 20/i);
  assert.match(html, /Changing app tracks is a clean install/i);
  assert.match(html, /Forget authorization/i);
  assert.match(html, /Direct and Play builds cannot update one another/i);
  assert.match(script, /Download Android \$\{release\.version\}/);
  assert.match(script, /signed Android release could not be verified/i);
  assert.match(script, /REQUIRED_PACKAGE_ID = "ptl\.kitsu\.app"/);
  assert.match(script, /REQUIRED_VERSION = "2\.1\.6"/);
  assert.match(script, /REQUIRED_VERSION_CODE = 20/);
  assert.match(readme, /exact Android 2\.1\.6 \/ version-code 20/i);
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

test("keeps device companion packs out of the static website", async () => {
  const readme = await readFile(path.join(root, "README.md"), "utf8");
  const personalCompanionName = String.fromCharCode(70, 111, 120, 32, 71, 105, 114, 108);
  assert.equal(readme.includes(personalCompanionName), false);
  const publicFiles = await listFiles(root);
  assert.deepEqual(publicFiles.filter((file) => file.toLowerCase().endsWith(".k868")), []);
});

test("keeps signed Android release bytes outside text conversion", async () => {
  const attributes = await readFile(path.join(projectRoot, ".gitattributes"), "utf8");
  const lines = new Set(attributes.split(/\r?\n/));
  for (const rule of [
    "platform/public-site/downloads/*.json -text",
    "platform/public-site/downloads/*.json.sig binary",
    "platform/public-site/downloads/*.apk binary",
  ]) assert.ok(lines.has(rule), `missing binary attribute: ${rule}`);
});

test("publishes the byte-exact signed local-first Android 2.1.6 release", async () => {
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
    version: "2.1.6",
    versionCode: 20,
    minimumAndroidApi: 26,
    url: "/downloads/kitsu-android-2.1.6-da081f9d09e6e4cd5747cbc53c655344.apk",
    bytes: 1839311,
    sha256: "da081f9d09e6e4cd5747cbc53c6553445546618b4b1ab6d2450c2048daa4f527",
    signingCertificateSha256: "a5a3cddb0d2c103630c6e622ac7f2051085a4c082db37aefdbadfc75d0a2d7fc",
    publishedAt: "2026-08-24T18:41:20Z",
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

test("ships every referenced local release asset", async () => {
  const files = [
    "styles.css",
    "theme.js",
    "site.js",
    "preview-release.js",
    "config.json",
    "assets/kitsu-app-icon.png",
    "assets/kitsu-k32-social-card-v1.png",
    "downloads/latest.json",
    "downloads/latest.json.sig",
    "downloads/update-ed25519-public.pem",
    "downloads/kitsu-android-2.1.5-72cd273f7e44402267ccd7a9bbbec9f2.apk",
    "downloads/kitsu-android-2.1.6-da081f9d09e6e4cd5747cbc53c655344.apk",
    "downloads/kitsu-k32-android-2.0.0.apk",
    "downloads/android-stable-2.0.0-20260822t123928z.json",
    "downloads/android-stable-2.0.0-20260822t123928z.json.sig",
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
  const pages = ["index.html", "privacy/index.html", "terms/index.html", "security/index.html", "contact/index.html"];
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
    "index.html", "privacy/index.html", "terms/index.html", "security/index.html", "contact/index.html",
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
  const paths = ["index.html", "styles.css", "theme.js", "site.js", "preview-release.js", "README.md", "privacy/index.html", "terms/index.html", "security/index.html", "contact/index.html"];
  for (const relative of paths) assert.doesNotMatch(await readFile(path.join(root, relative), "utf8"), /(?:Ã‚|Ã¢â‚¬|Ã¢â‚¬â„¢|Ã¢â€ |Ã¢â€¡|Ã¢â„¢|Ã¢â€”|Ã¢Å“|Ã¢Å’|Ãƒ|ï¿½)/, relative);
});
