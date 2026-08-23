const PREVIEW_MANIFEST = "/downloads/android-testing-preview-2.1.4-debug-20260823t084114z.json";
const PREVIEW_SIGNATURE = "/downloads/android-testing-preview-2.1.4-debug-20260823t084114z.json.sig";
const RELEASE_PUBLIC_KEY_B64URL = "JAAR8Unpz7n7h_q02cpFc8HH_7OHF3ZYAAXsQa7lE4I";

const PREVIEW_FIELDS = Object.freeze([
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

const PREVIEW_RELEASE = Object.freeze({
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
  bytes: 17_639_476,
  sha256: "16684de9063b6e5a76ac2c7f517e8219db4b1c908c1d8fea1fd85cd42768823d",
  signingCertificateSha256: "68892ab5f40f5b8b01834be4ba2fcc4fd9038293d9bdf97ab48c9dc0bb534298",
  internetPermissionDeclared: false,
  foregroundServicesDeclared: false,
  controllerAuthorizationScope: "separate-install",
});

function decodeBase64Url(value) {
  if (!/^[A-Za-z0-9_-]+$/.test(value)) throw new Error("invalid base64url");
  const standard = value.replaceAll("-", "+").replaceAll("_", "/");
  const padded = standard + "=".repeat((4 - (standard.length % 4)) % 4);
  return Uint8Array.from(atob(padded), (character) => character.charCodeAt(0));
}

function canonicalUtcTimestamp(value) {
  if (typeof value !== "string" || !/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$/.test(value)) return false;
  const parsed = Date.parse(value);
  return Number.isFinite(parsed) && new Date(parsed).toISOString().replace(".000Z", "Z") === value;
}

export function validPreviewManifest(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return false;
  const fields = Object.keys(value);
  if (fields.length !== PREVIEW_FIELDS.length || fields.some((field, index) => field !== PREVIEW_FIELDS[index])) return false;
  if (!canonicalUtcTimestamp(value.publishedAt)) return false;
  return Object.entries(PREVIEW_RELEASE).every(([field, expected]) => value[field] === expected);
}

export async function verifiedPreviewManifest(
  manifestResponse,
  signatureResponse,
  publicKeyB64Url = RELEASE_PUBLIC_KEY_B64URL,
  subtle = globalThis.crypto?.subtle,
) {
  try {
    if (!manifestResponse?.ok || !signatureResponse?.ok || !subtle) return null;
    const manifest = new Uint8Array(await manifestResponse.arrayBuffer());
    const signature = new Uint8Array(await signatureResponse.arrayBuffer());
    if (manifest.length < 2 || manifest.length > 16_384 || signature.length !== 64) return null;
    const keyBytes = decodeBase64Url(publicKeyB64Url);
    if (keyBytes.length !== 32) return null;
    const key = await subtle.importKey("raw", keyBytes, { name: "Ed25519" }, false, ["verify"]);
    if (!await subtle.verify({ name: "Ed25519" }, key, signature, manifest)) return null;
    const release = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(manifest));
    return validPreviewManifest(release) ? release : null;
  } catch {
    return null;
  }
}

function previewDownloadUrl(origin) {
  try {
    const originUrl = new URL(origin);
    const download = new URL(PREVIEW_RELEASE.url, originUrl);
    if (
      download.origin !== originUrl.origin
      || download.pathname !== PREVIEW_RELEASE.url
      || download.search
      || download.hash
      || download.username
      || download.password
    ) return null;
    return download;
  } catch {
    return null;
  }
}

function previewElements(documentObject) {
  return {
    status: documentObject.querySelector("#android-preview-status"),
    title: documentObject.querySelector("#android-preview-title"),
    detail: documentObject.querySelector("#android-preview-detail"),
    link: documentObject.querySelector("#android-preview-download"),
    digest: documentObject.querySelector("#android-preview-digest"),
  };
}

export function showPreviewFailure(documentObject = document) {
  const elements = previewElements(documentObject);
  if (Object.values(elements).some((element) => !element)) return;
  elements.link.removeAttribute("href");
  elements.link.removeAttribute("download");
  elements.link.classList.add("disabled");
  elements.link.setAttribute("aria-disabled", "true");
  elements.status.textContent = "Testing preview not published";
  elements.title.textContent = "Kitsu Android testing preview";
  elements.detail.textContent = "No signed preview matching this exact test build has been published. No test APK link has been exposed.";
  elements.link.textContent = "Testing preview unavailable";
  elements.digest.textContent = "The accepted stable Android release channel remains separate and unchanged.";
}

function showPreviewRelease(documentObject, release, download) {
  const elements = previewElements(documentObject);
  if (Object.values(elements).some((element) => !element)) return;
  elements.link.href = download.toString();
  elements.link.download = "kitsu-android-2.1.4-debug.apk";
  elements.link.classList.remove("disabled");
  elements.link.removeAttribute("aria-disabled");
  elements.status.textContent = `Verified test-only manifest · ${release.version}`;
  elements.title.textContent = "Kitsu Android 2.1.4 testing preview";
  elements.detail.textContent = "Debug-signed build for end-to-end Heltec testing. It is not an accepted or stable Android release.";
  elements.link.textContent = "Download testing preview · 16.8 MiB";
  elements.digest.textContent = `${release.bytes.toLocaleString("en-US")} bytes · SHA-256 ${release.sha256.toUpperCase()} · debug certificate ${release.signingCertificateSha256.toUpperCase()}`;
}

export async function loadAndroidPreview({
  documentObject = document,
  fetchFunction = fetch,
  origin = window.location.origin,
} = {}) {
  try {
    const [manifestResponse, signatureResponse] = await Promise.all([
      fetchFunction(PREVIEW_MANIFEST, { cache: "no-store" }),
      fetchFunction(PREVIEW_SIGNATURE, { cache: "no-store" }),
    ]);
    const release = await verifiedPreviewManifest(manifestResponse, signatureResponse);
    const download = release ? previewDownloadUrl(origin) : null;
    if (!release || !download) return showPreviewFailure(documentObject);
    showPreviewRelease(documentObject, release, download);
  } catch {
    showPreviewFailure(documentObject);
  }
}

export const previewReleaseContract = Object.freeze({
  manifestPath: PREVIEW_MANIFEST,
  signaturePath: PREVIEW_SIGNATURE,
  publicKeyB64Url: RELEASE_PUBLIC_KEY_B64URL,
  fields: PREVIEW_FIELDS,
  release: PREVIEW_RELEASE,
});

if (typeof document !== "undefined" && typeof window !== "undefined") void loadAndroidPreview();
