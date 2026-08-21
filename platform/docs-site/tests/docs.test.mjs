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
  "Wi-Fi and remote access",
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

test("owner account guide explains scope, issuance, sign-in, and recovery", async () => {
  const [android, connectivity, home] = await Promise.all([
    read("android/index.html"),
    read("connectivity/index.html"),
    read("index.html"),
  ]);
  assert.match(android, /id="owner-account"/);
  assert.match(android, /No account needed[\s\S]*Direct Bluetooth/);
  assert.match(android, /Owner account needed[\s\S]*Remote access/);
  assert.match(android, /no public registration button/i);
  assert.match(android, /private bootstrap handoff/i);
  assert.match(android, /temporary password/i);
  assert.match(android, /first successful login requires/i);
  assert.match(android, /delivery over an authenticated private channel/i);
  assert.match(android, /readable only by the operator's service account/i);
  assert.match(android, /mode <code>0600<\/code>/);
  assert.match(android, /system browser/i);
  assert.match(android, /no self-service email or public password reset/i);
  assert.match(android, /operator-run recovery procedure/i);
  assert.match(android, /invalidates the old password, terminates its existing server-side sessions, and issues a new temporary password/i);
  assert.match(android, /Never include a password/i);
  assert.match(connectivity, /Owner sign-in is for the remote path/);
  assert.match(connectivity, /Owner sign-in is not required for this local step/);
  assert.match(home, /href="\/android\/#owner-account"/);
});

test("Wi-Fi guide distinguishes radio association from Bluetooth-owned control", async () => {
  const [android, connectivity] = await Promise.all([
    read("android/index.html"),
    read("connectivity/index.html"),
  ]);
  assert.match(connectivity, /keep its Wi-Fi station associated/i);
  assert.match(connectivity, /Gateway enrollment, LAN commands, and remote actions remain stopped until Bluetooth closes/i);
  assert.match(connectivity, /Check storage and link separately/i);
  assert.match(connectivity, /no Wi-Fi-only erase command or supported public factory-reset flow/i);
  assert.match(connectivity, /do not use one for Wi-Fi troubleshooting/i);
  assert.doesNotMatch(connectivity, /release's explicitly destructive factory-reset procedure/i);
  assert.doesNotMatch(connectivity, /keeps Wi-Fi stopped/i);
  assert.match(android, /Use Wi-Fi remote access/);
  assert.match(android, /explicit owner choice/i);
});

test("troubleshooting reflects the live Wi-Fi and Bluetooth coexistence policy", async () => {
  const troubleshooting = await read("troubleshooting/index.html");
  assert.match(troubleshooting, /credentials stored/i);
  assert.match(troubleshooting, /Wi-Fi connected/i);
  assert.match(troubleshooting, /keep Direct Bluetooth open/i);
  assert.match(troubleshooting, /keeps the Wi-Fi station warm/i);
  assert.match(troubleshooting, /normal Connect flow prefers Direct Bluetooth/i);
  assert.match(troubleshooting, /Use Wi-Fi remote access.*explicit owner-selected override/i);
  assert.doesNotMatch(troubleshooting, /Do not leave an authenticated Bluetooth session open/i);
  assert.doesNotMatch(troubleshooting, /initial grace period/i);
});

test("operator security terms are explained without implying user configuration", async () => {
  const security = await read("security/index.html");
  assert.match(security, /<dt>Private CA \(PCA\)<\/dt>/);
  assert.match(security, /issues and verifies device and gateway identities/);
  assert.match(security, /not a public website certificate authority/);
  assert.match(security, /<dt>Envoy<\/dt>/);
  assert.match(security, /backend service-edge proxy/);
  assert.match(security, /routes authenticated traffic, and enforces network policy/);
  assert.match(security, /not an account, an Android setting, or something a normal Kitsu user configures/);
});

test("release guidance uses the signed Android release and preserves device state", async () => {
  const [home, android, gettingStarted, updates] = await Promise.all([
    read("index.html"),
    read("android/index.html"),
    read("getting-started/index.html"),
    read("updates/index.html"),
  ]);
  assert.match(home, /Download Android/);
  assert.match(home, /Install Kitsu for Android/);
  assert.match(android, /website APK is a signed Android release/i);
  assert.match(android, /Install the signed Android APK/i);
  assert.match(gettingStarted, /current signed Android APK/i);
  assert.match(updates, /NVS and companion pack<\/td><td>Preserved/);
  assert.match(updates, /Keeps device identity, companion state, and installed visual pack/);
  const personalCompanionName = String.fromCharCode(70, 111, 120, 32, 71, 105, 114, 108);
  assert.equal(updates.includes(personalCompanionName), false);
  assert.doesNotMatch(`${home}${android}${gettingStarted}${updates}`, /test APK|test build|coming soon|placeholder|logo explanation/i);
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
