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
  publishedPack("5CAC86A3", "frog", "Frog", "common", "e12ce976db429c6abade6fec7546c13b22671c53cf8e8d4790131582b8b6a94f", "79b1214eb0f5737501f9fdc14d1b53ae80151cb5a3853da8d18d2aa16d22296e"),
  publishedPack("13793DC7", "hamster", "Hamster", "common", "2a29295da21179fabb7da53f5b5ab9329360d2b740b87521ce453cf1e6447e6f", "595ef7401db2fc07790c52333cacbd6d51d345078fa9d65a48fc69e392f32580"),
  publishedPack("7495DBFB", "turtle", "Turtle", "common", "bdfd2b4fba09ee2b342374385b1688856c0607768fbf26ffe12657065df2bb26", "99681a5d153d0480d1540a3283dbb0ab88f65e1acf3245bf0edd983b24baf720"),
  publishedPack("68D9554E", "rabbit", "Rabbit", "uncommon", "ccf3e341eff60b05e8a749729f5b13807351bd94f6c3f0b47a336ff2cf95917e", "6db8fea2132d6af4df37d7dddc30432f80ccf8cde19c5f8a71de7cd529febc20"),
  publishedPack("5DF6BE74", "hedgehog", "Hedgehog", "uncommon", "4620dc3284b3420b887dfe950537c6c573173c478fabcbb5c25dc25c80c1abb4", "b70a49bcf33b45ee691ca7db9a0f6cb38d84c90f6f3b3225f54ba540945c4f6c"),
  publishedPack("E59408E0", "ferret", "Ferret", "uncommon", "1b67378e850eea9b538d365a635192fbff58be0490fdb5035896022658b64a90", "2e7bf12bdfe25a0c24b800f65c7b9a948bde2d7c0b6014c1f7ba8dd3dac981e0"),
  publishedPack("29B4B2F7", "otter", "Otter", "rare", "8b00554ccfefefd9b468379520b5b718b7dcba3aedca62ee4d60eccefdea7bdd", "5f5442306f8d9f81c51ab4b01e67601bd38021767e5c3764a4b76d6a0285e31e"),
  publishedPack("69276D0C", "axolotl", "Axolotl", "rare", "0181206f720dec9f9894da696fc0f3c3809e909b2743d7f25e3ab3012c7a3798", "4210b4a3fe6c8a71db1139fc4257ed433dfe0f4b301d06dd96daa08b59c626fd"),
  publishedPack("2DFB0797", "chinchilla", "Chinchilla", "rare", "e838b8ab5548af651af96985fad2071ae2e74533a0cce0250d0822b45d359870", "2b7e8a24d48e32f71ad6b8b4c4b20f8562751708c0e8e5fe2592babddbf0f2bf"),
  publishedPack("C163EFED", "raccoon", "Raccoon", "very_rare", "4976ea7b25285e0d3a9af08414fe7e130cddaa66cbaf7d63f699524ea8ac14b8", "f9b4bc24d448dc97bf4bf6c6fe44ad0a093d71b965653511731a3189b21f582b"),
  publishedPack("374D2540", "capybara", "Capybara", "very_rare", "9ac74c8df0d9c3cc10612efbf17ede96b4c3011888f9b6b11aa532ce94ef4735", "95eaaaf3ab86211a898f1ba790fc958cad87c94d0cf3c32cde7a38969827fb1e"),
  publishedPack("39FC5B1A", "sugar-glider", "Sugar Glider", "very_rare", "1cfe812f12f29804e3acca72d202e5c1c23f2d75c497f8c94d21e35990d0c224", "b47bee6ab3dd881a626ce2a1cec87a5aee383a375db243a926fe9808c529f81a"),
  publishedPack("91A2DE7B", "red-panda", "Red Panda", "epic", "853eaf31de02aac33a53662effd58aaa539b6b2d0de77ed9bd4cbcf94f63a5ed", "1b97ce0a4d8109850f7b3ce32fc0f2885d74df5f0d61b342fcb4ef5cc8e38293"),
  publishedPack("E04EC405", "pangolin", "Pangolin", "epic", "e6e85ddeb4a2cac04370e10fb512d28546fb47040838001b1573e44931b4618b", "ffad15c85107a410172a82fcb48c22fe25905b6b99300898bb39c2dfb631450c"),
  publishedPack("8E0E1B03", "tasmanian-devil", "Tasmanian Devil", "epic", "720dd6da86ddc0c53eec3ee24131fcc2ad9200ace9a125cb6adcfa3e423a9789", "529d280b6ce8c63619bcbb0861c0ffc99655666151a60405a4e5ec46fe4d76a3"),
  publishedPack("533B9B30", "snow-leopard", "Snow Leopard", "legendary", "ff69b31b4623989182f7f5f1afdc61ce6169ce4ef6d1f4a8c5f679946ebb07f2", "e57c327e2b9d5a7d717901fa635a76b20c4fc85dc8665bca7afd853760f99252"),
  publishedPack("86F3BB5D", "okapi", "Okapi", "legendary", "51abd3f6cd8a3a8397c5a8082d0f11102bed0884f0c0b521644e1cfd876da28d", "870a90ca7ab59ace3504ff1f3ed5b5c408f6de79edf8b5415b7b25bbfb2374bc"),
  publishedPack("2D1D89AF", "shoebill", "Shoebill", "legendary", "ac5463fb0775f4098c2e642f8c3876a4ec0b775067f768be82c9c0b1efea6733", "50f45b450d7b47352f2b089bc250c3f5775b455595dc84a32a42ab81bd650fb4"),
  publishedPack("A52160C5", "cat-girl", "Cat Girl", "mythical", "829fbf1d9d94b6765158d82a063771320d6c311b150b7b820377bc3ccd3cfe60", "4217f38d5e0669f686a507490b1acc7191178ecee3c01a5cd0fce363002bc124"),
  publishedPack("F0F750BD", "rabbit-girl", "Rabbit Girl", "mythical", "f5dee83f9a292bbafd942784c78ba55e797c9cbb767fe2c7e63fd6bc690871b5", "2026ecf7a3063fb0eb4a33b86ead7d292ec654dd44c093c0d5a521bc154b4775"),
  publishedPack("52A1C03A", "deer-girl", "Deer Girl", "mythical", "99d5e4fad27a0fbd0b2c76d3ee19a81856866e0c2b064aba7780904c23cbcad5", "2a3a80a5bfdd29dee0e237043c989e5c300226a4f19ebd50b035cc738237d29e"),
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
