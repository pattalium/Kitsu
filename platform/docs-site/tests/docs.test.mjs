import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFile, stat } from "node:fs/promises";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const projectRoot = path.resolve(root, "..", "..");
const pages = [
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
const pageTitles = [
  "Manual home",
  "Getting started",
  "Device controls",
  "Android app",
  "Messaging",
  "Bluetooth and offline use",
  "Firmware updates",
  "Troubleshooting",
  "Security and privacy",
];

const read = (relative) => readFile(path.join(root, relative), "utf8");
const sha256 = async (file) => createHash("sha256").update(await readFile(file)).digest("hex");

function localTarget(value) {
  const [withoutFragment, fragment = ""] = value.split("#", 2);
  const pathname = withoutFragment.split("?", 1)[0];
  if (!pathname.startsWith("/")) return null;
  if (pathname === "/") return { relative: "index.html", fragment };
  if (pathname.endsWith("/")) return { relative: `${pathname.slice(1)}index.html`, fragment };
  return { relative: pathname.slice(1), fragment };
}

test("every manual page has working local branding and compact page navigation", async () => {
  for (const [index, page] of pages.entries()) {
    const html = await read(page);
    assert.match(html, /rel="icon" type="image\/png" href="\/assets\/kitsu-app-icon\.png\?v=4f850b55"/, page);
    assert.match(html, /<img src="\/assets\/kitsu-app-icon\.png\?v=4f850b55" alt="" width="43" height="43"/, page);
    assert.match(html, /<link rel="stylesheet" href="\/styles\.css\?v=c0dcc352"/, page);
    assert.doesNotMatch(html, /https:\/\/k32\.run\/assets\//, page);
    assert.match(html, /<aside class="sidebar" aria-label="Manual pages">/, page);
    assert.equal((html.match(/<details class="mobile-manual">/g) ?? []).length, 1, page);
    assert.match(html, new RegExp(`<summary><span>Manual<\\/span><b>${pageTitles[index]}<\\/b><\\/summary>`), page);
    const mobileNavigation = html.match(/<nav aria-label="Mobile manual pages">([\s\S]*?)<\/nav>/)?.[1];
    assert.ok(mobileNavigation, page);
    assert.equal((mobileNavigation.match(/<a(?: aria-current="page")? href=/g) ?? []).length, pages.length, page);
    assert.equal((mobileNavigation.match(/aria-current="page"/g) ?? []).length, 1, page);
    assert.equal((html.match(/aria-current="page"/g) ?? []).length, 2, page);
    assert.match(html, new RegExp(`<nav class="next-links" aria-label="Page navigation" data-page="${index + 1} of ${pages.length}">`), page);
    assert.doesNotMatch(html, /(?:placeholder|coming soon|todo\b)/i, page);
  }
});

test("all local links, fragments, styles, and images resolve", async () => {
  for (const page of pages) {
    const html = await read(page);
    const references = [...html.matchAll(/(?:href|src)="([^"]+)"/g)].map((match) => match[1]);
    for (const reference of references) {
      if (reference.startsWith("#")) {
        assert.match(html, new RegExp(`id=["']${reference.slice(1)}["']`), `${page}: ${reference}`);
        continue;
      }
      if (/^https:\/\//.test(reference)) continue;
      assert.doesNotMatch(reference, /^http:\/\//, `${page}: ${reference}`);
      const target = localTarget(reference);
      if (!target) continue;
      const destination = path.join(root, target.relative);
      assert.equal((await stat(destination)).isFile(), true, `${page}: ${reference}`);
      if (target.fragment) {
        const targetHtml = await read(target.relative);
        assert.match(targetHtml, new RegExp(`id=["']${target.fragment}["']`), `${page}: ${reference}`);
      }
    }
  }
});

test("documentation publishes the authoritative app icon bytes", async () => {
  const canonical = path.join(projectRoot, "assets", "brand", "kitsu-app-icon.png");
  const published = path.join(root, "assets", "kitsu-app-icon.png");
  assert.equal(await sha256(published), await sha256(canonical));
  assert.equal(await sha256(published), "4f850b551e8fc242b0b31577ab76407cf1ade0e1a59bfaaf21edde3653b0ef42");
  assert.deepEqual([...await readFile(published).then((bytes) => bytes.subarray(0, 8))], [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
});

test("documents a complete Bluetooth-only product with no runtime-service controls", async () => {
  const [android, connectivity, home] = await Promise.all([
    read("android/index.html"),
    read("connectivity/index.html"),
    read("index.html"),
  ]);
  assert.match(android, /has no sign-in screen, account, gateway control, Wi-Fi setup, background relay, or Internet permission/i);
  assert.match(connectivity, /Put the phone in airplane mode, turn Bluetooth back on/i);
  assert.match(connectivity, /Internet permission is absent/i);
  assert.match(home, /No account, Wi-Fi, gateway, or Internet connection is required/i);
  assert.doesNotMatch(`${android}${connectivity}${home}`, /app\.k32\.run|owner-account|Sign in|Public gateway|Use Wi-Fi remote access/i);
});

test("saved-device controls and controller revocation are explicit", async () => {
  const [android, connectivity, gettingStarted] = await Promise.all([
    read("android/index.html"),
    read("connectivity/index.html"),
    read("getting-started/index.html"),
  ]);
  assert.match(android, /stores up to three Kitsu authorizations/i);
  assert.match(android, /Select[\s\S]*Connect[\s\S]*Disconnect/i);
  assert.match(android, /controller\.forget/);
  assert.match(connectivity, /Disconnect versus Forget authorization/i);
  assert.match(connectivity, /durably revoke this phone's controller root/i);
  assert.match(gettingStarted, /airplane mode[\s\S]*turn Bluetooth back on/i);
});

test("setup instructions and messaging controls match the real device and Android labels", async () => {
  const [home, device, gettingStarted, android, messaging, styles] = await Promise.all([
    read("index.html"),
    read("device/index.html"),
    read("getting-started/index.html"),
    read("android/index.html"),
    read("messaging/index.html"),
    read("styles.css"),
  ]);
  assert.match(home, /leave CONNECT selected and hold[\s\S]*leave BLUETOOTH selected and hold again/i);
  assert.match(device, /<dt>CONNECT<\/dt><dd>Opens the local connection menu/);
  assert.match(device, /<dt>BLUETOOTH<\/dt>/);
  assert.doesNotMatch(device, /<dt>PHONE<\/dt>/);
  assert.match(gettingStarted, /open Settings and find Pair another Kitsu/i);
  assert.match(gettingStarted, /Pair this phone/);
  assert.match(gettingStarted, /Connected directly over authenticated Bluetooth/);
  assert.doesNotMatch(gettingStarted, /open More|Pair nearby Kitsu|refresh the inbox/i);
  assert.match(android, /Settings, enter a phone label and tap Pair this phone/i);
  assert.match(messaging, /Choose a nearby peer/);
  assert.match(messaging, /compact key reference/);
  assert.match(messaging, /Public · slot 0/);
  assert.match(messaging, /Send over mesh/);
  assert.match(messaging, /tap <strong>Refresh<\/strong> in the Connection card/i);
  assert.match(messaging, /complete 24-message device ring/i);
  assert.doesNotMatch(messaging, /Open the recipient selector|delivery details/i);
  assert.doesNotMatch(styles, /account-scope|scope-label/);
});

test("release guidance explains signed resumable A/B OTA and recovery", async () => {
  const [home, android, gettingStarted, updates, troubleshooting, security] = await Promise.all([
    read("index.html"),
    read("android/index.html"),
    read("getting-started/index.html"),
    read("updates/index.html"),
    read("troubleshooting/index.html"),
    read("security/index.html"),
  ]);
  assert.match(home, /Download Android/);
  assert.match(home, /Install Kitsu for Android/);
  assert.match(android, /signed APK/i);
  assert.match(gettingStarted, /eligible local-first Android APK/i);
  assert.match(updates, /inactive application slot/i);
  assert.match(updates, /64 KiB checkpoint/i);
  assert.match(updates, /30 seconds/i);
  assert.match(updates, /controller store, MeshCore state, and coredump<\/td><td>Preserved/i);
  assert.match(gettingStarted, /physical-acceptance record bind the exact SHA-256/i);
  assert.match(gettingStarted, /intentionally writes the reviewed rollback-enabled Kitsu bootloader/i);
  assert.doesNotMatch(gettingStarted, /preserves the bootloader/i);
  assert.match(updates, /one clean 4 KiB OTA-journal artifact/i);
  assert.match(updates, /journal artifact to <code>0x33f000<\/code> and <code>0x66f000<\/code>/i);
  assert.match(updates, /legacy-connectivity clear artifact/i);
  assert.match(updates, /isolated retired region at <code>0x7b0000<\/code>/i);
  assert.match(updates, /all seven regions/i);
  assert.match(updates, /Unlike normal Bluetooth OTA, this USB bootstrap intentionally writes the bootloader and partition table/i);
  assert.match(troubleshooting, /Reset interrupted update/i);
  assert.match(security, /Ed25519 update authority/i);
  assert.match(security, /A\/B update/i);
  const personalCompanionName = String.fromCharCode(70, 111, 120, 32, 71, 105, 114, 108);
  assert.equal(updates.includes(personalCompanionName), false);
  assert.doesNotMatch(`${home}${android}${gettingStarted}${updates}${troubleshooting}${security}`, /test APK|test build|coming soon|placeholder|logo explanation/i);
});

test("public manual contains no retired runtime-service instructions", async () => {
  const text = (await Promise.all(pages.map(read))).join("\n");
  assert.doesNotMatch(text, /app\.k32\.run|api\.k32\.run|auth\.k32\.run|Owner account|Public gateway|Connect to public gateway|Use Wi-Fi remote access|gateway enrollment|OIDC|PKCE|mTLS|Envoy/i);
});

test("mobile manual navigation is compact, keyboard ordered, and touch sized", async () => {
  const css = await read("styles.css");
  assert.match(css, /@media \(max-width: 820px\)[\s\S]*\.sidebar \{ display: none; \}/);
  assert.match(css, /@media \(max-width: 820px\)[\s\S]*\.mobile-manual \{[^}]*position: sticky/);
  assert.match(css, /\.mobile-manual summary \{[^}]*min-height: 52px/);
  assert.match(css, /\.mobile-manual nav a \{[^}]*min-height: 44px/);
  assert.doesNotMatch(css, /aria-current="page"\][^{]*\{[^}]*order:/);
  assert.match(css, /\.next-links::before \{[^}]*content: "Page " attr\(data-page\)/);
  assert.match(css, /@media \(max-width: 560px\)[\s\S]*\.next-links[^}]*grid-template-columns: repeat\(2, minmax\(0, 1fr\)\)/);
  assert.match(css, /\.next-links a \{[^}]*min-height: 86px/);
});

test("public documentation contains no deployment-specific identifiers", async () => {
  const text = (await Promise.all(pages.map(read))).join("\n");
  assert.doesNotMatch(text, /\b(?:10|127)\.\d{1,3}\.\d{1,3}\.\d{1,3}\b/);
  assert.doesNotMatch(text, /localhost|\.lan\b|\.local\b/i);
  assert.doesNotMatch(text, /cloudflare|tunnel token/i);
});
