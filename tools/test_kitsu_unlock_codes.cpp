#include "../src/kitsu_unlock_codes.h"

#include <cstring>
#include <iostream>
#include <vector>

namespace {

int failures = 0;
constexpr uint32_t kStoreMagic = 0x4b554331UL;

#pragma pack(push, 1)
struct PersistedHeader {
  uint32_t magic;
  uint16_t schema;
  uint16_t bytes;
  uint32_t generation;
  uint8_t count;
  uint8_t reserved[3];
};
#pragma pack(pop)

static_assert(sizeof(PersistedHeader) + sizeof(uint32_t) ==
                  kitsu868::unlocks::kPersistedEnvelopeBytes,
              "test fixture header must match the v1 wire format");

void expect(bool condition, const char* message) {
  if (condition) return;
  ++failures;
  std::cerr << "FAIL " << message << '\n';
}

uint32_t crc32(const uint8_t* input, size_t bytes) {
  uint32_t value = 0xffffffffUL;
  for (size_t index = 0U; index < bytes; ++index) {
    value ^= input[index];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      value = (value >> 1U) ^
          (0xedb88320UL & (0U - static_cast<uint32_t>(value & 1U)));
    }
  }
  return ~value;
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

std::vector<uint8_t> makeLegacyBlob(
    const kitsu868::unlocks::CodeStore& source) {
  using namespace kitsu868::unlocks;
  expect(source.count() <= kLegacyCodeCapacity,
         "legacy fixture fits v1 capacity");
  std::vector<uint8_t> current(source.serializedBytes());
  size_t currentBytes = 0U;
  expect(source.serialize(current.data(), current.size(), currentBytes) &&
             currentBytes == kStoreSerializedBytes,
         "encode records for legacy fixture");

  std::vector<uint8_t> legacy(kLegacyStoreSerializedBytes, 0U);
  const size_t recordsBytes = kPersistedRecordBytes * source.count();
  std::memcpy(legacy.data() + sizeof(PersistedHeader),
              current.data() + sizeof(PersistedHeader), recordsBytes);
  PersistedHeader header{};
  header.magic = kStoreMagic;
  header.schema = kLegacyStoreSchema;
  header.bytes = static_cast<uint16_t>(legacy.size());
  header.generation = source.generation();
  header.count = static_cast<uint8_t>(source.count());
  std::memcpy(legacy.data(), &header, sizeof(header));
  const uint32_t checksum =
      crc32(legacy.data(), legacy.size() - sizeof(uint32_t));
  std::memcpy(legacy.data() + legacy.size() - sizeof(checksum), &checksum,
              sizeof(checksum));
  return legacy;
}

}  // namespace

int main() {
  using namespace kitsu868::unlocks;
  expect(kCodeCapacity == 32U && kCodeCapacity >= 21U,
         "v2 has room for the complete public roster");
  expect(kLegacyCodeCapacity == 12U &&
             kLegacyStoreSerializedBytes == 956U &&
             kStoreSerializedBytes == 2516U,
         "v1 and v2 wire sizes remain fixed");
  CodeStore store;
  expect(store.count() == 0U && store.generation() == 0U &&
             !store.migrationRequired(),
         "fresh state");

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

  for (uint8_t i = 2U; i <= kLegacyCodeCapacity; ++i) {
    const uint32_t packId = i == 2U ? first.packId : 0x11223344U + i;
    expect(store.add(makeRecord(i, packId)) == AddResult::Added,
           "fill legacy-sized source");
  }

  const uint32_t legacyGeneration = store.generation();
  std::vector<uint8_t> legacyBlob = makeLegacyBlob(store);
  CodeStore migrated;
  expect(migrated.load(legacyBlob.data(), legacyBlob.size()),
         "load sealed v1 store");
  expect(migrated.migrationRequired() && migrated.count() == store.count() &&
             migrated.generation() == legacyGeneration,
         "v1 load preserves records and requests migration");
  expect(migrated.findById(first.codeId, found) && found.redeemed &&
             found.installed && found.packId == first.packId,
         "v1 migration preserves code identity and flags");
  expect(migrated.findByPackId(first.packId, found) &&
             found.codeId == first.codeId,
         "pack lookup returns the first record without rejecting legacy duplicates");
  for (size_t index = 0U; index < store.count(); ++index) {
    CodeRecord before{};
    CodeRecord after{};
    expect(store.at(index, before) && migrated.find(before.code, after) &&
               after.codeId == before.codeId &&
               after.packId == before.packId &&
               std::strcmp(after.code, before.code) == 0 &&
               std::strcmp(after.creatureName, before.creatureName) == 0 &&
               after.rarity == before.rarity &&
               std::strcmp(after.source, before.source) == 0 &&
               after.acquiredAtEpoch == before.acquiredAtEpoch &&
               after.redeemed == before.redeemed &&
               after.installed == before.installed,
           "every v1 code remains verifiable after migration");
  }

  for (uint8_t i = static_cast<uint8_t>(kLegacyCodeCapacity + 1U);
       i <= kCodeCapacity; ++i) {
    expect(migrated.add(makeRecord(i, 0x11223344U + i)) == AddResult::Added,
           "fill expanded v2 capacity");
  }
  expect(migrated.count() == kCodeCapacity &&
             migrated.add(makeRecord(42U, 0x99887766U)) == AddResult::Full,
         "full v2 store preserves earned codes");

  std::vector<uint8_t> blob(migrated.serializedBytes());
  size_t blobBytes = 0U;
  expect(migrated.serialize(blob.data(), blob.size(), blobBytes) &&
             blobBytes == blob.size(),
         "serialize v2");
  CodeStore restored;
  expect(restored.load(blob.data(), blob.size()), "load sealed v2 store");
  expect(restored.count() == migrated.count() &&
             restored.generation() == migrated.generation() &&
             !restored.migrationRequired(),
         "v2 roundtrip clears migration requirement");

  CodeStore guarded;
  const CodeRecord guard = makeRecord(80U, 0x01020304U);
  expect(guarded.add(guard) == AddResult::Added, "create transaction guard");
  const uint32_t guardGeneration = guarded.generation();
  legacyBlob[legacyBlob.size() / 2U] ^= 0x40U;
  expect(!guarded.load(legacyBlob.data(), legacyBlob.size()),
         "v1 crc corruption blocked");
  expect(guarded.count() == 1U && guarded.generation() == guardGeneration &&
             guarded.findById(guard.codeId, found),
         "failed v1 migration preserves live store");
  blob[blob.size() / 2U] ^= 0x80U;
  expect(!restored.load(blob.data(), blob.size()), "crc corruption blocked");
  expect(restored.count() == migrated.count() &&
             restored.generation() == migrated.generation(),
         "failed v2 load is transactional");

  Rarity rarity{};
  expect(parseRarity("mythical", rarity) && rarity == Rarity::Mythical,
         "rarity parse");
  expect(!parseRarity("legendary_plus", rarity), "unknown rarity blocked");

  if (failures != 0) {
    std::cerr << "TEST_FAIL kitsu_unlock_codes failures=" << failures << '\n';
    return 1;
  }
  std::cout << "TEST_PASS kitsu_unlock_codes capacity=" << kCodeCapacity
            << " v1_bytes=" << kLegacyStoreSerializedBytes
            << " v2_bytes=" << migrated.serializedBytes()
            << " migration=preserved\n";
  return 0;
}
