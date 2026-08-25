import { sha256Hex } from "./release.js";

export const PACK_SLOT = Object.freeze({
  offset: 0x670000,
  bytes: 0x140000,
});

export const PACK_CATALOG = Object.freeze({
  fox: Object.freeze({
    id: "fox",
    name: "Fox",
    filename: "Kitsu868-v0.16.5-fox.k868",
    assetUrl: new URL("../../../assets/packs/fox.k868", import.meta.url).href,
    bytes: 24976,
    sha256: "c868386770b6083dcd8f7c01ec7fe455faec476a96c724ab62f09770fdcdab38",
  }),
  cat: Object.freeze({
    id: "cat",
    name: "Cat",
    filename: "Kitsu868-v0.16.5-cat.k868",
    assetUrl: new URL("../../../assets/packs/cat.k868", import.meta.url).href,
    bytes: 24976,
    sha256: "8d19d6b8bc584d9aaee5a6867504fd23c1862c907bbeb1affd9611e35bf2a6d7",
  }),
  dog: Object.freeze({
    id: "dog",
    name: "Dog",
    filename: "Kitsu868-v0.16.5-dog.k868",
    assetUrl: new URL("../../../assets/packs/dog.k868", import.meta.url).href,
    bytes: 24976,
    sha256: "8652aad28816d52fca334766ebefb5c38aec1b09dcc72783414998d17a46e261",
  }),
});

function fail(message) {
  throw new Error(message);
}

export function packDefinition(packId) {
  if (packId === "preserve") return null;
  const definition = PACK_CATALOG[packId];
  if (!definition) fail("selected companion pack is not supported");
  if (definition.bytes < 1 || definition.bytes > PACK_SLOT.bytes) {
    fail(`${definition.name} companion pack exceeds the dedicated pack slot`);
  }
  return definition;
}

export async function verifyPackBytes(definition, bytes) {
  if (!(bytes instanceof Uint8Array) || bytes.byteLength !== definition.bytes) {
    fail(`${definition.name} companion pack size does not match the installed catalog`);
  }
  if (await sha256Hex(bytes) !== definition.sha256) {
    fail(`${definition.name} companion pack SHA-256 does not match the installed catalog`);
  }
}

export async function fetchOfficialPack(packId) {
  const definition = packDefinition(packId);
  if (!definition) return null;
  const url = new URL(definition.assetUrl, window.location.href);
  if (url.origin !== window.location.origin) fail(`${definition.name} companion pack leaves this flasher origin`);
  const response = await fetch(url, {
    cache: "no-store",
    credentials: "omit",
    mode: "same-origin",
    referrerPolicy: "no-referrer",
    signal: AbortSignal.timeout(15000),
  });
  if (!response.ok) fail(`${definition.name} companion pack returned HTTP ${response.status}`);
  const declaredLength = response.headers.get("content-length");
  if (declaredLength !== null && Number(declaredLength) !== definition.bytes) {
    fail(`${definition.name} companion pack response has an unexpected size`);
  }
  const bytes = new Uint8Array(await response.arrayBuffer());
  await verifyPackBytes(definition, bytes);
  return Object.freeze({
    definition,
    record: Object.freeze({
      role: `companion_pack_${definition.id}`,
      offset: PACK_SLOT.offset,
      bytes: definition.bytes,
      sha256: definition.sha256,
    }),
    bytes,
  });
}

export async function reverifyPack(pack) {
  if (!pack?.definition || !pack?.record) fail("selected companion pack is not loaded");
  const expected = packDefinition(pack.definition.id);
  if (
    pack.record.offset !== PACK_SLOT.offset
    || pack.record.bytes !== expected.bytes
    || pack.record.sha256 !== expected.sha256
  ) {
    fail("selected companion pack metadata changed after verification");
  }
  await verifyPackBytes(expected, pack.bytes);
}
