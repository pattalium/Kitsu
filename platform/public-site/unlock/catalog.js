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
  publishedPack("5CAC86A3", "frog", "Frog", "common", "d038ed7db4153e726b577fc67a3b5606ed7457f3c258277e48fb917596807309", "9c375e2c8602e568f5a66f60e3b5f22430b6ef85662987f3bd508026a3ef0745"),
  publishedPack("13793DC7", "hamster", "Hamster", "common", "59c8ece111f08cac2778da5cd9ef0de612e91ef3957325d1d4526194ff8c6369", "4463931e7840d0a095b4d35edb8d30586cd43bf9ca23cbf872934e3b6bb9d282"),
  publishedPack("7495DBFB", "turtle", "Turtle", "common", "bd86805e06ea45701c41dde0b00f7126339dd8e498569daa7c46476ffb5cdecd", "1c45af359cb56c0b2d85dd057d098a03e5d559cde7ceb66ecf7272761e378c1c"),
  publishedPack("68D9554E", "rabbit", "Rabbit", "uncommon", "db95c112d0ba6e30226fe8df4a1427855ee21dc9b2274fd3cf5dcc5dbdf11ee5", "2bfc7968592cb9732b3543abdaf1c961dded6ec3c939df07fc1fec1b6cd0c758"),
  publishedPack("5DF6BE74", "hedgehog", "Hedgehog", "uncommon", "c9980a81b81bcf2c2b942321d00d8856e681606c4efc31bfffca368ac518e1f6", "9670c03b7ed8c51ab8241f558bd528f3262466ac1c28cffb4a341f5304a6ffd2"),
  publishedPack("E59408E0", "ferret", "Ferret", "uncommon", "7b76897601eb218262d6e3f13fbb4fd5374225ef76682ad6d58541a3476c536c", "7d78a6c0a829ebc402f23e1fd04887634f9d7404e24fdbd0e8eb3c5caec65d2e"),
  publishedPack("29B4B2F7", "otter", "Otter", "rare", "ef90cc5b762b69e995037a92adfb875977593d55d0cea7e55ed032bbd7b98cff", "93a892941bf0523f34cb18c2b05d3e34b1fc3400964fc773f45b77ed647b4668"),
  publishedPack("69276D0C", "axolotl", "Axolotl", "rare", "25cd68ced10c15ad16b67f84b1189ab4717f99d4cd1876e60ab18f82da45b2b0", "714094c8abdcc4c0bdf210dc9d6e209778f22fcd8406c90e465b867001f170fd"),
  publishedPack("2DFB0797", "chinchilla", "Chinchilla", "rare", "1c4d7ec38092124b3997b133c7138283ea800c46c05f85df45a66fe31784bb85", "6122570198b1df5c1bd9ed221870b029f168808829ded0c3acd1608e3ace3347"),
  publishedPack("C163EFED", "raccoon", "Raccoon", "very_rare", "c6ce4ed3bf915933a8a977ea338380da6761df22d0cb715cd56693272a973af0", "9eea4b97d01f2a3b46affd0c53ee1d597a2f8ea2b56da641199155440e462cc3"),
  publishedPack("374D2540", "capybara", "Capybara", "very_rare", "b0b7ff183323ddc89f9eda710a8bc729181ae89fb34a4163b10161c9eeec3719", "41baf69b0129f63fd9ca2888066fcc5ec59f3701bbbe50750500496761f636f1"),
  publishedPack("39FC5B1A", "sugar-glider", "Sugar Glider", "very_rare", "a3cda7b4cde7bbc6625ad47abc730b833ff3bf6404190516b8c9abb3194a41fa", "ff827a3be976702c4e8899b89d4efd24ccd38556154872576d254b7f88bcbafa"),
  publishedPack("91A2DE7B", "red-panda", "Red Panda", "epic", "44c2690e1dc85c70e64a0f90704d6e0b7cb2d5e71b40c379b2581e570cecfda3", "2a20f00bd15f3a9cd66585b49f25410ef48377bf7fa41ce102da09628dbf4f99"),
  publishedPack("E04EC405", "pangolin", "Pangolin", "epic", "b77485c1d8e74f950808ec61fb73de8f1c893b5d67d362b1f5fc5eb17b4a78d4", "dc3df08c7463ec916ae8c5a6a2d3f3823e6ed07956fe10df7545874fe1e7f98d"),
  publishedPack("8E0E1B03", "tasmanian-devil", "Tasmanian Devil", "epic", "bfda2e08174f50db6fd5284e5dcbc54ce6f6a01c9f59072e50647ffbfa5c67fc", "20085b99a42ff8a5440a74e12e4f9bd44c8bf9d508d7f280fe10768befeb3380"),
  publishedPack("533B9B30", "snow-leopard", "Snow Leopard", "legendary", "cd18552091ca202b03c4b9db383df9276e3002aa52ac88ed3bbbd438eee72074", "bc080e455570af0e6d10c54731fe40f88b93fd681a7775026a9ce7ea6b4cf57c"),
  publishedPack("86F3BB5D", "okapi", "Okapi", "legendary", "8f6b26c1a0d64f1c9d3880606d42a2576f4fd110cf6ed1bc92cc567d785f54b9", "13ff8262314812e6ce5ecdb15aacf7bdec54487e75671b06b1474a3a1c7cf11d"),
  publishedPack("2D1D89AF", "shoebill", "Shoebill", "legendary", "9c8204f2125fa77530de96c78e4adbcfc277ef05b4d6be070d6d2b5048ed5ab6", "26d27bb750fdddad0242795cee8c246ebe46752f6f22c0392a060ace5f16d1de"),
  publishedPack("A52160C5", "cat-girl", "Cat Girl", "mythical", "3603495a9bda098df941f123b0110a9a8bf72c83a527f0a0c774cf110248a452", "72764fe2ff8269d6c6add7fac2b4e0a466e6feeb4e5791f6668bd6a51c890a60"),
  publishedPack("F0F750BD", "rabbit-girl", "Rabbit Girl", "mythical", "f8bb31771b1c424959565c4c8ce0eca8883fbd4ed52ebd849d7da4c727213335", "c2a5e2fe0800427e63d0f9dd4db84a975b9300d43be3b37d3db18b60eeac0a52"),
  publishedPack("52A1C03A", "deer-girl", "Deer Girl", "mythical", "0daeb2b30b4b0657fe31d3291679efb56264061e5298ff519713a9b451c9aaca", "9c28f15a0e7ac9c837257914644b007ee1d0bb8b617888cf79d61b176751fb94"),
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
