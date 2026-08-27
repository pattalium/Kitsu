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
    bytes: 31_120,
    sha256,
    portraitSha256,
    portraitUrl: `./portraits/${slug}.${portraitSha256}.png`,
  });
}

// Publication is explicit. A record contains public metadata and one static
// portrait only. Pack bytes are served by the gated redemption endpoint and
// never have a stable public URL.
export const PUBLISHED_WILD_PACKS = Object.freeze([
  publishedPack("5CAC86A3", "frog", "Frog", "common", "d038ed7db4153e726b577fc67a3b5606ed7457f3c258277e48fb917596807309", "79b1214eb0f5737501f9fdc14d1b53ae80151cb5a3853da8d18d2aa16d22296e"),
  publishedPack("13793DC7", "hamster", "Hamster", "common", "59c8ece111f08cac2778da5cd9ef0de612e91ef3957325d1d4526194ff8c6369", "595ef7401db2fc07790c52333cacbd6d51d345078fa9d65a48fc69e392f32580"),
  publishedPack("7495DBFB", "turtle", "Turtle", "common", "bd86805e06ea45701c41dde0b00f7126339dd8e498569daa7c46476ffb5cdecd", "99681a5d153d0480d1540a3283dbb0ab88f65e1acf3245bf0edd983b24baf720"),
  publishedPack("68D9554E", "rabbit", "Rabbit", "uncommon", "db95c112d0ba6e30226fe8df4a1427855ee21dc9b2274fd3cf5dcc5dbdf11ee5", "6db8fea2132d6af4df37d7dddc30432f80ccf8cde19c5f8a71de7cd529febc20"),
  publishedPack("5DF6BE74", "hedgehog", "Hedgehog", "uncommon", "c9980a81b81bcf2c2b942321d00d8856e681606c4efc31bfffca368ac518e1f6", "b70a49bcf33b45ee691ca7db9a0f6cb38d84c90f6f3b3225f54ba540945c4f6c"),
  publishedPack("E59408E0", "ferret", "Ferret", "uncommon", "7b76897601eb218262d6e3f13fbb4fd5374225ef76682ad6d58541a3476c536c", "167e4f23ee17992c0fb8e093547c45cc7aa5fd4fd20dc1c1beb7c925f3798f9f"),
  publishedPack("29B4B2F7", "otter", "Otter", "rare", "ef90cc5b762b69e995037a92adfb875977593d55d0cea7e55ed032bbd7b98cff", "5f5442306f8d9f81c51ab4b01e67601bd38021767e5c3764a4b76d6a0285e31e"),
  publishedPack("69276D0C", "axolotl", "Axolotl", "rare", "25cd68ced10c15ad16b67f84b1189ab4717f99d4cd1876e60ab18f82da45b2b0", "5a890fda295960bf5ab40ae76139ba664b58d5c5081635fec05ae65b8666a724"),
  publishedPack("2DFB0797", "chinchilla", "Chinchilla", "rare", "1c4d7ec38092124b3997b133c7138283ea800c46c05f85df45a66fe31784bb85", "2b7e8a24d48e32f71ad6b8b4c4b20f8562751708c0e8e5fe2592babddbf0f2bf"),
  publishedPack("C163EFED", "raccoon", "Raccoon", "very_rare", "c6ce4ed3bf915933a8a977ea338380da6761df22d0cb715cd56693272a973af0", "f9b4bc24d448dc97bf4bf6c6fe44ad0a093d71b965653511731a3189b21f582b"),
  publishedPack("374D2540", "capybara", "Capybara", "very_rare", "b0b7ff183323ddc89f9eda710a8bc729181ae89fb34a4163b10161c9eeec3719", "95eaaaf3ab86211a898f1ba790fc958cad87c94d0cf3c32cde7a38969827fb1e"),
  publishedPack("39FC5B1A", "sugar-glider", "Sugar Glider", "very_rare", "a3cda7b4cde7bbc6625ad47abc730b833ff3bf6404190516b8c9abb3194a41fa", "b47bee6ab3dd881a626ce2a1cec87a5aee383a375db243a926fe9808c529f81a"),
  publishedPack("91A2DE7B", "red-panda", "Red Panda", "epic", "44c2690e1dc85c70e64a0f90704d6e0b7cb2d5e71b40c379b2581e570cecfda3", "1b97ce0a4d8109850f7b3ce32fc0f2885d74df5f0d61b342fcb4ef5cc8e38293"),
  publishedPack("E04EC405", "pangolin", "Pangolin", "epic", "b77485c1d8e74f950808ec61fb73de8f1c893b5d67d362b1f5fc5eb17b4a78d4", "09e9bf69658d60b15c03c7951986f01cbe0c677de7973ec5a600b8eec3fa637c"),
  publishedPack("8E0E1B03", "tasmanian-devil", "Tasmanian Devil", "epic", "bfda2e08174f50db6fd5284e5dcbc54ce6f6a01c9f59072e50647ffbfa5c67fc", "529d280b6ce8c63619bcbb0861c0ffc99655666151a60405a4e5ec46fe4d76a3"),
  publishedPack("533B9B30", "snow-leopard", "Snow Leopard", "legendary", "cd18552091ca202b03c4b9db383df9276e3002aa52ac88ed3bbbd438eee72074", "e57c327e2b9d5a7d717901fa635a76b20c4fc85dc8665bca7afd853760f99252"),
  publishedPack("86F3BB5D", "okapi", "Okapi", "legendary", "8f6b26c1a0d64f1c9d3880606d42a2576f4fd110cf6ed1bc92cc567d785f54b9", "870a90ca7ab59ace3504ff1f3ed5b5c408f6de79edf8b5415b7b25bbfb2374bc"),
  publishedPack("2D1D89AF", "shoebill", "Shoebill", "legendary", "9c8204f2125fa77530de96c78e4adbcfc277ef05b4d6be070d6d2b5048ed5ab6", "50f45b450d7b47352f2b089bc250c3f5775b455595dc84a32a42ab81bd650fb4"),
  publishedPack("A52160C5", "cat-girl", "Cat Girl", "mythical", "3603495a9bda098df941f123b0110a9a8bf72c83a527f0a0c774cf110248a452", "93cb53d50bd1ed995b057c9cb4901499f58c00baa21459963fb13974e4e84b09"),
  publishedPack("F0F750BD", "rabbit-girl", "Rabbit Girl", "mythical", "f8bb31771b1c424959565c4c8ce0eca8883fbd4ed52ebd849d7da4c727213335", "2b529073ff4283ff611745ae254e9e54c7e141a4ae688ec8239543f41f16a7f8"),
  publishedPack("52A1C03A", "deer-girl", "Deer Girl", "mythical", "0daeb2b30b4b0657fe31d3291679efb56264061e5298ff519713a9b451c9aaca", "36ff3f3776b891f66624aa361c4a998d0d98d3f66e0b704b0171b71883c1b7d3"),
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
