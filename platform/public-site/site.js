const DOWNLOAD_MANIFEST = "/downloads/latest.json";
const DOWNLOAD_SIGNATURE = "/downloads/latest.json.sig";
const RELEASE_PUBLIC_KEY_B64URL = "JAAR8Unpz7n7h_q02cpFc8HH_7OHF3ZYAAXsQa7lE4I";
const ANDROID_SIGNING_CERTIFICATE_SHA256 = "a5a3cddb0d2c103630c6e622ac7f2051085a4c082db37aefdbadfc75d0a2d7fc";
const REQUIRED_PACKAGE_ID = "ptl.kitsu.app";
const REQUIRED_VERSION = "2.2.11";
const REQUIRED_VERSION_CODE = 32;
const RELEASE_FIELDS = Object.freeze([
  "schema",
  "status",
  "channel",
  "buildType",
  "packageId",
  "version",
  "versionCode",
  "minimumAndroidApi",
  "url",
  "bytes",
  "sha256",
  "signingCertificateSha256",
  "publishedAt",
]);

function validHttpsUrl(value) {
  try {
    const url = new URL(value);
    return url.protocol === "https:" && !url.username && !url.password ? url : null;
  } catch {
    return null;
  }
}

function validDownloadUrl(value) {
  const url = validHttpsUrl(value);
  if (!url || url.origin !== window.location.origin || url.search || url.hash) return null;
  return /^\/downloads\/kitsu-android-[0-9]+\.[0-9]+\.[0-9]+-[a-f0-9]{32}\.apk$/.test(url.pathname) ? url : null;
}

function validUtcTimestamp(value) {
  if (typeof value !== "string" || !/^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$/.test(value)) return false;
  const parsed = new Date(value);
  return !Number.isNaN(parsed.valueOf()) && parsed.toISOString() === value.replace("Z", ".000Z");
}

function decodeBase64Url(value) {
  if (!/^[A-Za-z0-9_-]+$/.test(value)) throw new Error("invalid base64url");
  const standard = value.replaceAll("-", "+").replaceAll("_", "/");
  const padded = standard + "=".repeat((4 - (standard.length % 4)) % 4);
  return Uint8Array.from(atob(padded), (character) => character.charCodeAt(0));
}

async function verifiedManifest(manifestResponse, signatureResponse) {
  if (!manifestResponse.ok || !signatureResponse.ok || !globalThis.crypto?.subtle) return null;
  const manifest = new Uint8Array(await manifestResponse.arrayBuffer());
  const signature = new Uint8Array(await signatureResponse.arrayBuffer());
  if (manifest.length < 2 || manifest.length > 16_384 || signature.length !== 64) return null;
  const key = await crypto.subtle.importKey(
    "raw",
    decodeBase64Url(RELEASE_PUBLIC_KEY_B64URL),
    { name: "Ed25519" },
    false,
    ["verify"],
  );
  if (!await crypto.subtle.verify({ name: "Ed25519" }, key, signature, manifest)) return null;
  return JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(manifest));
}

function showReleaseFailure() {
  document.querySelector("#android-status").textContent = "Android release unavailable";
  document.querySelector("#android-detail").textContent = "The signed Android release could not be verified. No download has been exposed.";
  document.querySelector("#android-download").textContent = "Verification failed";
  document.querySelector("#android-digest").textContent = "Try again later or check the public status page.";
}

async function loadAndroidRelease() {
  try {
    const [manifestResponse, signatureResponse] = await Promise.all([
      fetch(DOWNLOAD_MANIFEST, { cache: "no-store" }),
      fetch(DOWNLOAD_SIGNATURE, { cache: "no-store" }),
    ]);
    const release = await verifiedManifest(manifestResponse, signatureResponse);
    if (!release) return showReleaseFailure();
    if (
      typeof release !== "object"
      || Array.isArray(release)
      || Object.keys(release).length !== RELEASE_FIELDS.length
      || !RELEASE_FIELDS.every((field, index) => Object.keys(release)[index] === field)
    ) return showReleaseFailure();
    const download = validDownloadUrl(new URL(release.url, window.location.origin).toString());
    if (
      !download
      || release.schema !== "kitsu.android-release.v1"
      || release.status !== "available"
      || release.channel !== "stable"
      || release.buildType !== "release"
      || release.packageId !== REQUIRED_PACKAGE_ID
      || release.version !== REQUIRED_VERSION
      || !Number.isSafeInteger(release.versionCode)
      || release.versionCode !== REQUIRED_VERSION_CODE
      || !Number.isSafeInteger(release.minimumAndroidApi)
      || release.minimumAndroidApi !== 26
      || !Number.isSafeInteger(release.bytes)
      || release.bytes < 1
      || typeof release.sha256 !== "string"
      || !/^[a-f0-9]{64}$/.test(release.sha256)
      || download.pathname !== `/downloads/kitsu-android-${release.version}-${release.sha256.slice(0, 32)}.apk`
      || typeof release.signingCertificateSha256 !== "string"
      || !/^[a-f0-9]{64}$/.test(release.signingCertificateSha256)
      || release.signingCertificateSha256 !== ANDROID_SIGNING_CERTIFICATE_SHA256
      || !validUtcTimestamp(release.publishedAt)
    ) return showReleaseFailure();

    const link = document.querySelector("#android-download");
    link.href = download.toString();
    link.download = `kitsu-k32-android-${release.version}.apk`;
    link.textContent = `Download Android ${release.version}`;
    link.classList.remove("disabled");
    link.removeAttribute("aria-disabled");
    document.querySelector("#android-status").textContent = `Verified local-first Android manifest · version ${release.version}`;
    document.querySelector("#android-title").textContent = `Kitsu ${release.version} Android`;
    document.querySelector("#android-detail").textContent = "Signed local-first Android release with authenticated Bluetooth, resilient clock-sync recovery, saved-device controls, messages, offline firmware updates, and no account or Internet requirement.";
    document.querySelector("#android-digest").textContent = `SHA-256 ${release.sha256.toUpperCase()}`;
  } catch {
    showReleaseFailure();
  }
}

void loadAndroidRelease();
