import assert from "node:assert/strict";
import { createHash, createPublicKey, verify } from "node:crypto";
import { access, readFile, readdir, stat } from "node:fs/promises";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const projectRoot = path.resolve(root, "..", "..");

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
  assert.match(html, /https:\/\/app\.k32\.run/);
  assert.match(html, /https:\/\/github\.com\/pattalium\/Kitsu/);
  assert.equal(config.repositoryUrl, "https://github.com/pattalium/Kitsu");
  assert.doesNotMatch(html, /link pending|verification pending|not exposed prematurely|placeholder/i);
  assert.doesNotMatch(html, /private machine|private address/i);
});

test("publishes the signed Android release without placeholder distribution copy", async () => {
  const [html, script, readme] = await Promise.all([
    readFile(path.join(root, "index.html"), "utf8"),
    readFile(path.join(root, "site.js"), "utf8"),
    readFile(path.join(root, "README.md"), "utf8"),
  ]);
  assert.match(html, /Install the signed Android app/i);
  assert.match(html, /current signed Android APK/i);
  assert.match(script, /Download Android \$\{release\.version\}/);
  assert.match(script, /signed Android release could not be verified/i);
  assert.match(readme, /APK on the website is the signed Android release/i);
  assert.doesNotMatch(`${html}${script}${readme}`, /test APK|test build|coming soon|placeholder/i);
  assert.doesNotMatch(`${html}${script}${readme}`, /https?:\/\/play\.google\.com/i);
});

test("keeps device companion packs out of the static website", async () => {
  const readme = await readFile(path.join(root, "README.md"), "utf8");
  const personalCompanionName = String.fromCharCode(70, 111, 120, 32, 71, 105, 114, 108);
  assert.equal(readme.includes(personalCompanionName), false);
  const publicFiles = await listFiles(root);
  assert.deepEqual(publicFiles.filter((file) => file.toLowerCase().endsWith(".k868")), []);
});

test("publishes the detached-signature-verified Android 1.1.1 release", async () => {
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
    packageId: "app.kitsu.mobile",
    version: "1.1.1",
    versionCode: 7,
    minimumAndroidApi: 26,
    url: "/downloads/kitsu-k32-android-1.1.1.apk",
    bytes: 2596934,
    sha256: "e0c39119a9187eb2fe86878cdcd55009fe69be87724d44c3f7ae706c5fb65ae0",
    signingCertificateSha256: "a5a3cddb0d2c103630c6e622ac7f2051085a4c082db37aefdbadfc75d0a2d7fc",
    publishedAt: "2026-08-21T21:19:19Z",
  });

  const publicJWK = publicKey.export({ format: "jwk" });
  assert.equal(publicJWK.x, "JAAR8Unpz7n7h_q02cpFc8HH_7OHF3ZYAAXsQa7lE4I");
  assert.match(script, /crypto\.subtle\.verify/);
  assert.match(script, /signature\.length !== 64/);
  assert.match(script, /release\.channel !== "stable"/);
  assert.match(script, /release\.buildType !== "release"/);
  assert.match(script, /release\.packageId !== "app\.kitsu\.mobile"/);
  assert.match(script, /url\.origin !== window\.location\.origin/);
  assert.match(script, /ANDROID_SIGNING_CERTIFICATE_SHA256/);
  assert.match(script, /showReleaseFailure/);

  const apk = path.join(root, release.url.slice(1));
  const apkStat = await stat(apk);
  assert.equal(apkStat.isFile(), true);
  assert.equal(apkStat.size, release.bytes);
  assert.equal(await sha256(apk), release.sha256);
  assert.deepEqual([...await readFile(apk).then((bytes) => bytes.subarray(0, 4))], [0x50, 0x4b, 0x03, 0x04]);

  const downloadEntries = await readdir(path.join(root, "downloads"));
  assert.deepEqual(downloadEntries.filter((entry) => entry.toLowerCase().endsWith(".apk")), ["kitsu-k32-android-1.1.1.apk"]);
  assert.equal(downloadEntries.some((entry) => /private|keystore|\.jks$/i.test(entry)), false);
});

test("ships every referenced local release asset", async () => {
  const files = [
    "styles.css",
    "site.js",
    "policy.js",
    "config.json",
    "assets/kitsu-app-icon.png",
    "assets/kitsu-k32-social-card-v1.png",
    "downloads/latest.json",
    "downloads/latest.json.sig",
    "downloads/update-ed25519-public.pem",
    "downloads/kitsu-k32-android-1.1.1.apk",
  ];
  await Promise.all(files.map((file) => access(path.join(root, file))));
});

test("publishes factual policies without private deployment identifiers", async () => {
  const [home, privacy, terms, security, contact, policyScript] = await Promise.all([
    readFile(path.join(root, "index.html"), "utf8"),
    readFile(path.join(root, "privacy", "index.html"), "utf8"),
    readFile(path.join(root, "terms", "index.html"), "utf8"),
    readFile(path.join(root, "security", "index.html"), "utf8"),
    readFile(path.join(root, "contact", "index.html"), "utf8"),
    readFile(path.join(root, "policy.js"), "utf8"),
  ]);
  for (const href of ["/privacy/", "/terms/", "/security/", "/contact/"]) assert.match(home, new RegExp(`href=["']${href.replaceAll("/", "\\/")}`));
  assert.match(privacy, /90 days/);
  assert.match(privacy, /365 days/);
  assert.match(privacy, /request deletion/i);
  assert.match(security, /web contact route/i);
  assert.match(contact, /action="https:\/\/api\.k32\.run\/v1\/contact"/);
  assert.match(contact, /src="\/policy\.js"/);
  assert.doesNotMatch(contact, /<script>(?:.|\n)*<\/script>/);
  assert.match(policyScript, /URLSearchParams/);
  assert.doesNotMatch(`${privacy}${terms}${security}${contact}`, /private machine|private address|mailto:|placeholder/i);
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
  const paths = ["index.html", "styles.css", "site.js", "README.md", "privacy/index.html", "terms/index.html", "security/index.html", "contact/index.html"];
  for (const relative of paths) assert.doesNotMatch(await readFile(path.join(root, relative), "utf8"), /(?:Ã‚|Ã¢â‚¬|Ã¢â‚¬â„¢|Ã¢â€ |Ã¢â€¡|Ã¢â„¢|Ã¢â€”|Ã¢Å“|Ã¢Å’|Ãƒ|ï¿½)/, relative);
});
