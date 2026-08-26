"use strict";

export const PUBLISHED_PACK_SCHEMA = "kitsu.published-wild-pack.v2";

export const RARITIES = Object.freeze([
  "common",
  "uncommon",
  "rare",
  "very_rare",
  "epic",
  "legendary",
  "mythical",
]);

const raritySet = new Set(RARITIES);

// Starter companions remain normal flasher choices, never encounter-code rewards.
const reservedStarterPackIds = new Set([
  "FDC79D6F",
  "6C393E21",
  "E2B5E7BA",
]);

const requiredKeys = Object.freeze([
  "bytes",
  "displayName",
  "packId",
  "portraitSha256",
  "portraitUrl",
  "rarity",
  "schema",
  "sha256",
  "slug",
]);

function hasExactKeys(value, expected) {
  const actual = Object.keys(value).sort();
  return actual.length === expected.length
    && actual.every((key, index) => key === expected[index]);
}

export function normalizePackId(value) {
  if (typeof value !== "string") return null;
  const normalized = value.trim().toUpperCase();
  return /^[A-F0-9]{8}$/.test(normalized) ? normalized : null;
}

export function isPublishedPackEntry(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return false;
  if (!hasExactKeys(value, requiredKeys)) return false;
  if (value.schema !== PUBLISHED_PACK_SCHEMA) return false;

  const packId = normalizePackId(value.packId);
  if (!packId || reservedStarterPackIds.has(packId)) return false;
  if (!raritySet.has(value.rarity)) return false;
  if (!Number.isSafeInteger(value.bytes) || value.bytes < 64 || value.bytes > 2_097_152) return false;
  if (typeof value.sha256 !== "string" || !/^[a-f0-9]{64}$/.test(value.sha256)) return false;
  if (
    typeof value.displayName !== "string"
    || value.displayName.length < 1
    || value.displayName.length > 64
    || !/^[A-Za-z0-9][A-Za-z0-9 '().-]*$/.test(value.displayName)
  ) return false;
  if (typeof value.slug !== "string" || !/^[a-z0-9][a-z0-9-]{0,63}$/.test(value.slug)) return false;
  if (typeof value.portraitSha256 !== "string" || !/^[a-f0-9]{64}$/.test(value.portraitSha256)) return false;
  if (typeof value.portraitUrl !== "string") return false;

  const portraitMatch = value.portraitUrl.match(
    /^\.\/portraits\/([a-z0-9][a-z0-9-]{0,63})\.([a-f0-9]{64})\.png$/,
  );
  return Boolean(
    portraitMatch
    && portraitMatch[1] === value.slug
    && portraitMatch[2] === value.portraitSha256
  );
}

function publishedPack(packId, slug, displayName, rarity, sha256, portraitSha256) {
  return Object.freeze({
    schema: PUBLISHED_PACK_SCHEMA,
    packId,
    slug,
    displayName,
    rarity,
    bytes: 24_976,
    sha256,
    portraitSha256,
    portraitUrl: `./portraits/${slug}.${portraitSha256}.png`,
  });
}

// Publication is explicit. A record contains public metadata and one static
// portrait only. Pack bytes are served by the gated redemption endpoint and
// never have a stable public URL.
export const PUBLISHED_WILD_PACKS = Object.freeze([
  publishedPack("5CAC86A3", "frog", "Frog", "common", "0805b1edef3ddd5da1f91d72ed28fed26faa8666b81fa07a06f51229601c843e", "79b1214eb0f5737501f9fdc14d1b53ae80151cb5a3853da8d18d2aa16d22296e"),
  publishedPack("13793DC7", "hamster", "Hamster", "common", "2a29295da21179fabb7da53f5b5ab9329360d2b740b87521ce453cf1e6447e6f", "595ef7401db2fc07790c52333cacbd6d51d345078fa9d65a48fc69e392f32580"),
  publishedPack("7495DBFB", "turtle", "Turtle", "common", "ff6a91905c73c76cec88f10578fcc1f32c8f76dab90123c5b2100be945a06456", "99681a5d153d0480d1540a3283dbb0ab88f65e1acf3245bf0edd983b24baf720"),
  publishedPack("68D9554E", "rabbit", "Rabbit", "uncommon", "82d27ef8127cb7fb11e2325a070b97d8303f72b281c0b81af037be65c4a4a9ee", "6db8fea2132d6af4df37d7dddc30432f80ccf8cde19c5f8a71de7cd529febc20"),
  publishedPack("5DF6BE74", "hedgehog", "Hedgehog", "uncommon", "4620dc3284b3420b887dfe950537c6c573173c478fabcbb5c25dc25c80c1abb4", "b70a49bcf33b45ee691ca7db9a0f6cb38d84c90f6f3b3225f54ba540945c4f6c"),
  publishedPack("E59408E0", "ferret", "Ferret", "uncommon", "64681e4f7bb93adb170f724a35dc6cd403943cff518405f5e48c2a1593a5e387", "167e4f23ee17992c0fb8e093547c45cc7aa5fd4fd20dc1c1beb7c925f3798f9f"),
  publishedPack("29B4B2F7", "otter", "Otter", "rare", "8b00554ccfefefd9b468379520b5b718b7dcba3aedca62ee4d60eccefdea7bdd", "5f5442306f8d9f81c51ab4b01e67601bd38021767e5c3764a4b76d6a0285e31e"),
  publishedPack("69276D0C", "axolotl", "Axolotl", "rare", "e1106e349853b167f7f67ab25607ee1d9612a37bd6d0e4f4e7dfeb587a9dee12", "5a890fda295960bf5ab40ae76139ba664b58d5c5081635fec05ae65b8666a724"),
  publishedPack("2DFB0797", "chinchilla", "Chinchilla", "rare", "e838b8ab5548af651af96985fad2071ae2e74533a0cce0250d0822b45d359870", "2b7e8a24d48e32f71ad6b8b4c4b20f8562751708c0e8e5fe2592babddbf0f2bf"),
  publishedPack("C163EFED", "raccoon", "Raccoon", "very_rare", "71d14834923c7ba510e055cd3e2daee77c3479f1e4995626df3f888fe5a36915", "f9b4bc24d448dc97bf4bf6c6fe44ad0a093d71b965653511731a3189b21f582b"),
  publishedPack("374D2540", "capybara", "Capybara", "very_rare", "723b70bbcdf9ad552fbada270892b3d28e9207b992b9ca25af219aa5c46921bd", "95eaaaf3ab86211a898f1ba790fc958cad87c94d0cf3c32cde7a38969827fb1e"),
  publishedPack("39FC5B1A", "sugar-glider", "Sugar Glider", "very_rare", "afa00ae6a804a72e2d839384e63916e54b696b0c5e797985b58b229b9aecf234", "b47bee6ab3dd881a626ce2a1cec87a5aee383a375db243a926fe9808c529f81a"),
  publishedPack("91A2DE7B", "red-panda", "Red Panda", "epic", "346aacba5cede6a43ee2dde553b16ee250babcc138f57f2986576bec9d4538a3", "1b97ce0a4d8109850f7b3ce32fc0f2885d74df5f0d61b342fcb4ef5cc8e38293"),
  publishedPack("E04EC405", "pangolin", "Pangolin", "epic", "581010c35dd0d7043de67fb8b3baee375af37280a6b8ccf712353dde25c69428", "09e9bf69658d60b15c03c7951986f01cbe0c677de7973ec5a600b8eec3fa637c"),
  publishedPack("8E0E1B03", "tasmanian-devil", "Tasmanian Devil", "epic", "720dd6da86ddc0c53eec3ee24131fcc2ad9200ace9a125cb6adcfa3e423a9789", "529d280b6ce8c63619bcbb0861c0ffc99655666151a60405a4e5ec46fe4d76a3"),
  publishedPack("533B9B30", "snow-leopard", "Snow Leopard", "legendary", "5ac0efb34286993f5d32afb71268a24b0129aa25cc3e392e1e2a52c8540ad5d5", "e57c327e2b9d5a7d717901fa635a76b20c4fc85dc8665bca7afd853760f99252"),
  publishedPack("86F3BB5D", "okapi", "Okapi", "legendary", "6f68048429fc1e2ece5cc77c486d3264093d5fa8070a866dd267703aea2696db", "870a90ca7ab59ace3504ff1f3ed5b5c408f6de79edf8b5415b7b25bbfb2374bc"),
  publishedPack("2D1D89AF", "shoebill", "Shoebill", "legendary", "3b3564617923cb0574c215d865402db34dc750d3cc982fd079a3fe8873865b26", "50f45b450d7b47352f2b089bc250c3f5775b455595dc84a32a42ab81bd650fb4"),
  publishedPack("A52160C5", "cat-girl", "Cat Girl", "mythical", "a1100187a0e5a4ef7ee96bdc3f64ebd8754f7df36c11e01ec63f8376a63d18b5", "93cb53d50bd1ed995b057c9cb4901499f58c00baa21459963fb13974e4e84b09"),
  publishedPack("F0F750BD", "rabbit-girl", "Rabbit Girl", "mythical", "b4977b377f068b67a9872a10593c1166097b6dd444123e8cc9f9650362cab7c4", "2b529073ff4283ff611745ae254e9e54c7e141a4ae688ec8239543f41f16a7f8"),
  publishedPack("52A1C03A", "deer-girl", "Deer Girl", "mythical", "cb397b84ae528999c68b1ec283f8256c9c0f61c19b1877b47153bde99c8d54bf", "36ff3f3776b891f66624aa361c4a998d0d98d3f66e0b704b0171b71883c1b7d3"),
]);

for (const entry of PUBLISHED_WILD_PACKS) {
  if (!isPublishedPackEntry(entry)) {
    throw new TypeError("The published wild-pack catalog contains an invalid entry.");
  }
}

export function publishedPackFor(packId, catalog = PUBLISHED_WILD_PACKS) {
  const normalized = normalizePackId(packId);
  if (!normalized || !Array.isArray(catalog)) return null;
  for (const entry of catalog) {
    if (isPublishedPackEntry(entry) && entry.packId === normalized) return entry;
  }
  return null;
}

export function rarityLabel(rarity) {
  if (!raritySet.has(rarity)) return "Unknown";
  return rarity === "very_rare"
    ? "Very Rare"
    : rarity.charAt(0).toUpperCase() + rarity.slice(1);
}
