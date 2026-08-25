#include "../src/kitsu_unlock_codes.h"

#include <cstring>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
  if (condition) return;
  ++failures;
  std::cerr << "FAIL " << message << '\n';
}

kitsu868::unlocks::CodeRecord makeRecord(uint8_t seed, uint32_t packId) {
  uint8_t entropy[10]{};
  for (size_t i = 0U; i < sizeof(entropy); ++i) {
    entropy[i] = static_cast<uint8_t>(seed + i * 7U + 1U);
  }
  kitsu868::unlocks::CodeRecord record{};
  expect(kitsu868::unlocks::CodeStore::generateCode(entropy, record.code),
         "generate valid code");
  record.codeId = kitsu868::unlocks::CodeStore::codeId(record.code);
  record.packId = packId;
  const char* const creature = seed == 1U ? "Frog" : "Test Creature";
  std::memcpy(record.creatureName, creature, std::strlen(creature) + 1U);
  record.rarity = seed == 1U ? kitsu868::unlocks::Rarity::Common
                             : kitsu868::unlocks::Rarity::Mythical;
  constexpr char source[] = "mesh_repeater";
  std::memcpy(record.source, source, sizeof(source));
  record.acquiredAtEpoch = 1800000000U + seed;
  return record;
}

}  // namespace

int main() {
  using namespace kitsu868::unlocks;
  CodeStore store;
  expect(store.count() == 0U && store.generation() == 0U, "fresh state");

  uint8_t zero[10]{};
  char rejected[kFormattedCodeBytes]{};
  expect(!CodeStore::generateCode(zero, rejected), "reject zero entropy");

  CodeRecord first = makeRecord(1U, 0x11223344U);
  expect(std::strlen(first.code) == kFormattedCodeBytes - 1U,
         "formatted length");
  char normalized[kFormattedCodeBytes]{};
  expect(CodeStore::normalizeCode(first.code, normalized) &&
             std::strcmp(first.code, normalized) == 0,
         "canonical normalization");
  char loose[kFormattedCodeBytes]{};
  std::memcpy(loose, first.code, sizeof(loose));
  for (char& c : loose) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  }
  expect(CodeStore::normalizeCode(loose, normalized) &&
             std::strcmp(first.code, normalized) == 0,
         "case normalization");

  expect(store.add(first) == AddResult::Added, "first add");
  expect(store.add(first) == AddResult::Duplicate, "duplicate blocked");
  CodeRecord found{};
  expect(store.find(first.code, found) && found.packId == first.packId,
         "find exact");
  expect(store.setRedeemed(first.codeId, true) &&
             store.setInstalled(first.codeId, true),
         "update flags");
  expect(store.findById(first.codeId, found) && found.redeemed &&
             found.installed,
         "updated record");

  for (uint8_t i = 2U; i <= kCodeCapacity; ++i) {
    expect(store.add(makeRecord(i, 0x11223344U + i)) == AddResult::Added,
           "fill store");
  }
  expect(store.add(makeRecord(42U, 0x99887766U)) == AddResult::Full,
         "full store preserves earned codes");

  std::vector<uint8_t> blob(store.serializedBytes());
  size_t blobBytes = 0U;
  expect(store.serialize(blob.data(), blob.size(), blobBytes) &&
             blobBytes == blob.size(),
         "serialize");
  CodeStore restored;
  expect(restored.load(blob.data(), blob.size()), "load sealed store");
  expect(restored.count() == store.count() &&
             restored.generation() == store.generation(),
         "roundtrip identity");
  blob[blob.size() / 2U] ^= 0x80U;
  expect(!restored.load(blob.data(), blob.size()), "crc corruption blocked");

  Rarity rarity{};
  expect(parseRarity("mythical", rarity) && rarity == Rarity::Mythical,
         "rarity parse");
  expect(!parseRarity("legendary_plus", rarity), "unknown rarity blocked");

  if (failures != 0) {
    std::cerr << "TEST_FAIL kitsu_unlock_codes failures=" << failures << '\n';
    return 1;
  }
  std::cout << "TEST_PASS kitsu_unlock_codes capacity=" << kCodeCapacity
            << " serialized_bytes=" << store.serializedBytes() << '\n';
  return 0;
}
