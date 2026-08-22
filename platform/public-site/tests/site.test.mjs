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
  assert.match(html, /https:\/\/github\.com\/pattalium\/Kitsu/);
  assert.match(html, /does not need an account, gateway, or Internet connection/i);
  assert.match(html, /reviewed rollback bootloader, partition table, both A\/B application slots, both clean private update journals, and an exact clear over the isolated legacy connectivity partition/i);
  assert.match(html, /browser flasher stays unavailable until a physically accepted release/i);
  assert.match(html, /Check firmware availability/i);
  assert.match(html, /installer fails closed until a physically accepted signed release is available/i);
  assert.doesNotMatch(html, /https:\/\/(?:app|api|auth|gateway)\.k32\.run/i);
  assert.equal(config.repositoryUrl, "https://github.com/pattalium/Kitsu");
  assert.doesNotMatch(html, /link pending|verification pending|not exposed prematurely|placeholder/i);
  assert.doesNotMatch(html, /private machine|private address/i);
});

test("source landing instructions match the local-only device controls", async () => {
  const readme = await readFile(path.join(projectRoot, "README.md"), "utf8");
  assert.match(readme, /has no Internet permission/i);
  assert.match(readme, /hold PRG from Home[\s\S]*CONNECT[\s\S]*BLUETOOTH[\s\S]*PAIR PHONE/i);
  assert.match(readme, /Pair this phone/);
  assert.match(readme, /no online service is involved/i);
  assert.match(readme, /signed local-first Android 2\.0\.0 release/i);
  assert.match(readme, /firmware installer remains fail-closed until the matching local-only firmware\s+finishes physical acceptance/i);
  assert.doesNotMatch(readme, /open `PHONE`|Connect to public gateway|owner sign-in|Configure Wi-Fi/i);
});

test("fails closed for a signed Android manifest older than the local-first release", async () => {
  const [html, script, readme] = await Promise.all([
    readFile(path.join(root, "index.html"), "utf8"),
    readFile(path.join(root, "site.js"), "utf8"),
    readFile(path.join(root, "README.md"), "utf8"),
  ]);
  assert.match(html, /Install the signed Android app/i);
  assert.match(html, /Local-first Android 2\.0\.0 or newer/i);
  assert.match(html, /eligible local-first manifest/i);
  assert.match(script, /Download Android \$\{release\.version\}/);
  assert.match(script, /signed Android release could not be verified/i);
  assert.match(script, /MIN_LOCAL_FIRST_VERSION_CODE = 13/);
  assert.match(script, /MIN_LOCAL_FIRST_MAJOR_VERSION = 2/);
  assert.match(script, /showReleaseNotPromoted/);
  assert.match(readme, /refuses to link any build older than Android 2\.0\.0/i);
  assert.doesNotMatch(html, /href=["'][^"']+\.apk["']/i);
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

test("keeps signed Android release bytes outside text conversion", async () => {
  const attributes = await readFile(path.join(projectRoot, ".gitattributes"), "utf8");
  const lines = new Set(attributes.split(/\r?\n/));
  for (const rule of [
    "platform/public-site/downloads/latest.json -text",
    "platform/public-site/downloads/latest.json.sig binary",
    "platform/public-site/downloads/*.apk binary",
  ]) assert.ok(lines.has(rule), `missing binary attribute: ${rule}`);
});

test("publishes the byte-exact signed local-first Android 2.0.0 release", async () => {
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
    version: "2.0.0",
    versionCode: 13,
    minimumAndroidApi: 26,
    url: "/downloads/kitsu-k32-android-2.0.0.apk",
    bytes: 1693558,
    sha256: "87499a391944e92b76fa158621b2b59718d15a8e60a133c39fb9f5cf24f9ab2a",
    signingCertificateSha256: "a5a3cddb0d2c103630c6e622ac7f2051085a4c082db37aefdbadfc75d0a2d7fc",
    publishedAt: "2026-08-22T12:39:28Z",
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
  assert.match(script, /release\.versionCode < MIN_LOCAL_FIRST_VERSION_CODE/);
  assert.match(script, /majorVersion < MIN_LOCAL_FIRST_MAJOR_VERSION/);
  assert.match(script, /predates the local-first release/i);
  assert.match(script, /showReleaseFailure/);

  const apk = path.join(root, release.url.slice(1));
  const apkStat = await stat(apk);
  assert.equal(apkStat.isFile(), true);
  assert.equal(apkStat.size, release.bytes);
  assert.equal(await sha256(apk), release.sha256);
  assert.deepEqual([...await readFile(apk).then((bytes) => bytes.subarray(0, 4))], [0x50, 0x4b, 0x03, 0x04]);

  const downloadEntries = await readdir(path.join(root, "downloads"));
  assert.deepEqual(downloadEntries.filter((entry) => entry.toLowerCase().endsWith(".apk")), ["kitsu-k32-android-2.0.0.apk"]);
  assert.equal(downloadEntries.some((entry) => /private|keystore|\.jks$/i.test(entry)), false);
});

test("ships every referenced local release asset", async () => {
  const files = [
    "styles.css",
    "site.js",
    "config.json",
    "assets/kitsu-app-icon.png",
    "assets/kitsu-k32-social-card-v1.png",
    "downloads/latest.json",
    "downloads/latest.json.sig",
    "downloads/update-ed25519-public.pem",
    "downloads/kitsu-k32-android-2.0.0.apk",
  ];
  await Promise.all(files.map((file) => access(path.join(root, file))));
});

test("publishes factual local-first policies without a runtime-service form", async () => {
  const [home, privacy, terms, security, contact] = await Promise.all([
    readFile(path.join(root, "index.html"), "utf8"),
    readFile(path.join(root, "privacy", "index.html"), "utf8"),
    readFile(path.join(root, "terms", "index.html"), "utf8"),
    readFile(path.join(root, "security", "index.html"), "utf8"),
    readFile(path.join(root, "contact", "index.html"), "utf8"),
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
  assert.doesNotMatch(contact, /<form\b|api\.k32\.run|policy\.js/i);
  assert.doesNotMatch(contact, /<script>(?:.|\n)*<\/script>/);
  assert.doesNotMatch(`${privacy}${terms}${security}${contact}`, /private machine|private address|mailto:|placeholder|app\.k32\.run|auth\.k32\.run/i);
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
  const paths = ["index.html", "styles.css", "site.js", "README.md", "privacy/index.html", "terms/index.html", "security/index.html", "contact/index.html"];
  for (const relative of paths) assert.doesNotMatch(await readFile(path.join(root, relative), "utf8"), /(?:Ã‚|Ã¢â‚¬|Ã¢â‚¬â„¢|Ã¢â€ |Ã¢â€¡|Ã¢â„¢|Ã¢â€”|Ã¢Å“|Ã¢Å’|Ãƒ|ï¿½)/, relative);
});
