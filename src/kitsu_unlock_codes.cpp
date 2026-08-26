#include "kitsu_unlock_codes.h"

#include <string.h>

namespace kitsu868 {
namespace unlocks {
namespace {

constexpr uint32_t kStoreMagic = 0x4b554331UL;  // KUC1
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

#pragma pack(push, 1)
struct PersistedRecord {
  uint32_t codeId;
  char code[kFormattedCodeBytes];
  uint32_t packId;
  char creatureName[kCreatureNameBytes + 1U];
  uint8_t rarity;
  char source[kSourceNameBytes + 1U];
  uint32_t acquiredAtEpoch;
  uint8_t redeemed;
  uint8_t installed;
};

struct PersistedHeader {
  uint32_t magic;
  uint16_t schema;
  uint16_t bytes;
  uint32_t generation;
  uint8_t count;
  uint8_t reserved[3];
};
#pragma pack(pop)

static_assert(sizeof(PersistedRecord) == kPersistedRecordBytes,
              "unlock record wire size changed");
static_assert(sizeof(PersistedHeader) + sizeof(uint32_t) ==
                  kPersistedEnvelopeBytes,
              "unlock envelope wire size changed");

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

size_t boundedLength(const char* value, size_t capacity) {
  if (!value) return capacity + 1U;
  size_t length = 0U;
  while (length <= capacity && value[length] != '\0') ++length;
  return length;
}

bool validToken(const char* value, size_t capacity, bool allowSpace) {
  const size_t length = boundedLength(value, capacity);
  if (length == 0U || length > capacity) return false;
  for (size_t index = 0U; index < length; ++index) {
    const char c = value[index];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-' ||
        (allowSpace && c == ' ')) {
      continue;
    }
    return false;
  }
  return true;
}

bool validRecord(const CodeRecord& record) {
  char normalized[kFormattedCodeBytes]{};
  return record.codeId != 0U && record.packId != 0U &&
      boundedLength(record.code, kFormattedCodeBytes - 1U) ==
          kFormattedCodeBytes - 1U &&
      CodeStore::normalizeCode(record.code, normalized) &&
      strcmp(normalized, record.code) == 0 &&
      CodeStore::codeId(normalized) == record.codeId &&
      static_cast<uint8_t>(record.rarity) <=
          static_cast<uint8_t>(Rarity::Mythical) &&
      validToken(record.creatureName, kCreatureNameBytes, true) &&
      validToken(record.source, kSourceNameBytes, false);
}

void copyToPersisted(const CodeRecord& source, PersistedRecord& destination) {
  destination = PersistedRecord{};
  destination.codeId = source.codeId;
  memcpy(destination.code, source.code, sizeof(destination.code));
  destination.packId = source.packId;
  memcpy(destination.creatureName, source.creatureName,
         sizeof(destination.creatureName));
  destination.rarity = static_cast<uint8_t>(source.rarity);
  memcpy(destination.source, source.source, sizeof(destination.source));
  destination.acquiredAtEpoch = source.acquiredAtEpoch;
  destination.redeemed = source.redeemed ? 1U : 0U;
  destination.installed = source.installed ? 1U : 0U;
}

void copyFromPersisted(const PersistedRecord& source, CodeRecord& destination) {
  destination = CodeRecord{};
  destination.codeId = source.codeId;
  memcpy(destination.code, source.code, sizeof(destination.code));
  destination.packId = source.packId;
  memcpy(destination.creatureName, source.creatureName,
         sizeof(destination.creatureName));
  destination.rarity = static_cast<Rarity>(source.rarity);
  memcpy(destination.source, source.source, sizeof(destination.source));
  destination.acquiredAtEpoch = source.acquiredAtEpoch;
  destination.redeemed = source.redeemed != 0U;
  destination.installed = source.installed != 0U;
}

}  // namespace

const char* rarityName(Rarity rarity) {
  switch (rarity) {
    case Rarity::Common: return "common";
    case Rarity::Uncommon: return "uncommon";
    case Rarity::Rare: return "rare";
    case Rarity::VeryRare: return "very_rare";
    case Rarity::Epic: return "epic";
    case Rarity::Legendary: return "legendary";
    case Rarity::Mythical: return "mythical";
  }
  return "common";
}

bool parseRarity(const char* value, Rarity& output) {
  if (!value) return false;
  for (uint8_t raw = 0U; raw <= static_cast<uint8_t>(Rarity::Mythical); ++raw) {
    const Rarity candidate = static_cast<Rarity>(raw);
    if (strcmp(value, rarityName(candidate)) == 0) {
      output = candidate;
      return true;
    }
  }
  return false;
}

CodeStore::CodeStore() { reset(); }

void CodeStore::reset() {
  memset(records_, 0, sizeof(records_));
  count_ = 0U;
  generation_ = 0U;
  migrationRequired_ = false;
}

size_t CodeStore::count() const { return count_; }
uint32_t CodeStore::generation() const { return generation_; }

bool CodeStore::at(size_t index, CodeRecord& output) const {
  if (index >= count_) return false;
  output = records_[index];
  return true;
}

bool CodeStore::find(const char* code, CodeRecord& output) const {
  char normalized[kFormattedCodeBytes]{};
  if (!normalizeCode(code, normalized)) return false;
  uint8_t match = 0U;
  size_t found = 0U;
  for (size_t record = 0U; record < count_; ++record) {
    uint8_t difference = 0U;
    for (size_t index = 0U; index < kFormattedCodeBytes; ++index) {
      difference |= static_cast<uint8_t>(normalized[index]) ^
                    static_cast<uint8_t>(records_[record].code[index]);
    }
    const uint8_t equal = static_cast<uint8_t>(difference == 0U);
    if (equal) found = record;
    match |= equal;
  }
  if (!match) return false;
  output = records_[found];
  return true;
}

bool CodeStore::findById(uint32_t value, CodeRecord& output) const {
  for (size_t index = 0U; index < count_; ++index) {
    if (records_[index].codeId == value) {
      output = records_[index];
      return true;
    }
  }
  return false;
}

bool CodeStore::findByPackId(uint32_t value, CodeRecord& output) const {
  if (value == 0U) return false;
  for (size_t index = 0U; index < count_; ++index) {
    if (records_[index].packId == value) {
      output = records_[index];
      return true;
    }
  }
  return false;
}

AddResult CodeStore::add(const CodeRecord& record) {
  if (!validRecord(record)) return AddResult::Invalid;
  CodeRecord existing{};
  if (findById(record.codeId, existing) || find(record.code, existing)) {
    return AddResult::Duplicate;
  }
  if (count_ >= kCodeCapacity) return AddResult::Full;
  records_[count_++] = record;
  if (++generation_ == 0U) ++generation_;
  return AddResult::Added;
}

bool CodeStore::setRedeemed(uint32_t value, bool redeemed) {
  for (size_t index = 0U; index < count_; ++index) {
    if (records_[index].codeId != value) continue;
    if (records_[index].redeemed == redeemed) return true;
    records_[index].redeemed = redeemed;
    if (++generation_ == 0U) ++generation_;
    return true;
  }
  return false;
}

bool CodeStore::setInstalled(uint32_t value, bool installed) {
  for (size_t index = 0U; index < count_; ++index) {
    if (records_[index].codeId != value) continue;
    if (records_[index].installed == installed) return true;
    records_[index].installed = installed;
    if (++generation_ == 0U) ++generation_;
    return true;
  }
  return false;
}

bool CodeStore::generateCode(const uint8_t entropy[10],
                             char output[kFormattedCodeBytes]) {
  if (!entropy || !output) return false;
  uint8_t any = 0U;
  for (size_t index = 0U; index < 10U; ++index) any |= entropy[index];
  if (!any) return false;
  output[0] = 'K';
  output[1] = '8';
  output[2] = '-';
  uint32_t accumulator = 0U;
  uint8_t bits = 0U;
  size_t input = 0U;
  size_t written = 3U;
  size_t characters = 0U;
  while (characters < kCodeCharacters) {
    while (bits < 5U) {
      accumulator = (accumulator << 8U) | entropy[input++];
      bits = static_cast<uint8_t>(bits + 8U);
    }
    bits = static_cast<uint8_t>(bits - 5U);
    output[written++] = kAlphabet[(accumulator >> bits) & 0x1fU];
    ++characters;
    if ((characters == 5U || characters == 10U) &&
        characters < kCodeCharacters) {
      output[written++] = '-';
    }
  }
  output[written] = '\0';
  return written + 1U == kFormattedCodeBytes;
}

bool CodeStore::normalizeCode(const char* input,
                              char output[kFormattedCodeBytes]) {
  if (!input || !output) return false;
  char raw[kCodeCharacters + 3U]{};
  size_t rawBytes = 0U;
  for (size_t index = 0U; input[index] != '\0'; ++index) {
    char c = input[index];
    if (c == '-' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      continue;
    }
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
    if (rawBytes >= sizeof(raw) - 1U) return false;
    raw[rawBytes++] = c;
  }
  size_t start = 0U;
  if (rawBytes == kCodeCharacters + 2U && raw[0] == 'K' && raw[1] == '8') {
    start = 2U;
  } else if (rawBytes != kCodeCharacters) {
    return false;
  }
  for (size_t index = start; index < rawBytes; ++index) {
    if (strchr(kAlphabet, raw[index]) == nullptr) return false;
  }
  output[0] = 'K';
  output[1] = '8';
  output[2] = '-';
  size_t source = start;
  size_t destination = 3U;
  size_t characters = 0U;
  while (characters < kCodeCharacters) {
    output[destination++] = raw[source++];
    ++characters;
    if ((characters == 5U || characters == 10U) &&
        characters < kCodeCharacters) {
      output[destination++] = '-';
    }
  }
  output[destination] = '\0';
  return destination + 1U == kFormattedCodeBytes;
}

uint32_t CodeStore::codeId(const char* normalizedCode) {
  if (boundedLength(normalizedCode, kFormattedCodeBytes - 1U) !=
      kFormattedCodeBytes - 1U) {
    return 0U;
  }
  uint32_t value = crc32(reinterpret_cast<const uint8_t*>(normalizedCode),
                         kFormattedCodeBytes - 1U);
  return value == 0U ? 1U : value;
}

size_t CodeStore::serializedBytes() const { return kStoreSerializedBytes; }

bool CodeStore::migrationRequired() const { return migrationRequired_; }

bool CodeStore::serialize(uint8_t* output, size_t capacity,
                          size_t& outputBytes) const {
  outputBytes = 0U;
  if (!output || capacity < kStoreSerializedBytes) return false;
  memset(output, 0, kStoreSerializedBytes);
  PersistedHeader header{};
  header.magic = kStoreMagic;
  header.schema = kStoreSchema;
  header.bytes = static_cast<uint16_t>(kStoreSerializedBytes);
  header.generation = generation_;
  header.count = count_;
  memcpy(output, &header, sizeof(header));
  for (size_t index = 0U; index < count_; ++index) {
    PersistedRecord record{};
    copyToPersisted(records_[index], record);
    memcpy(output + sizeof(PersistedHeader) +
               index * sizeof(PersistedRecord),
           &record, sizeof(record));
  }
  const uint32_t checksum =
      crc32(output, kStoreSerializedBytes - sizeof(uint32_t));
  memcpy(output + kStoreSerializedBytes - sizeof(checksum), &checksum,
         sizeof(checksum));
  outputBytes = kStoreSerializedBytes;
  return true;
}

bool CodeStore::load(const uint8_t* input, size_t inputBytes) {
  if (!input) return false;

  const bool legacy = inputBytes == kLegacyStoreSerializedBytes;
  if (!legacy && inputBytes != kStoreSerializedBytes) {
    return false;
  }
  PersistedHeader header{};
  memcpy(&header, input, sizeof(header));
  uint32_t persistedChecksum = 0U;
  memcpy(&persistedChecksum, input + inputBytes - sizeof(persistedChecksum),
         sizeof(persistedChecksum));
  const uint16_t expectedSchema =
      legacy ? kLegacyStoreSchema : kStoreSchema;
  const size_t expectedCapacity =
      legacy ? kLegacyCodeCapacity : kCodeCapacity;
  if (header.magic != kStoreMagic || header.schema != expectedSchema ||
      header.bytes != inputBytes || header.count > expectedCapacity ||
      header.reserved[0] != 0U || header.reserved[1] != 0U ||
      header.reserved[2] != 0U ||
      persistedChecksum != crc32(input, inputBytes - sizeof(uint32_t))) {
    return false;
  }

  // Fully validate before mutating this store. This preserves an in-memory
  // ledger if either a legacy or current blob is corrupt.
  for (size_t index = 0U; index < header.count; ++index) {
    PersistedRecord persisted{};
    memcpy(&persisted,
           input + sizeof(PersistedHeader) +
               index * sizeof(PersistedRecord),
           sizeof(persisted));
    CodeRecord candidate{};
    copyFromPersisted(persisted, candidate);
    if (persisted.redeemed > 1U || persisted.installed > 1U ||
        !validRecord(candidate)) {
      return false;
    }
    for (size_t prior = 0U; prior < index; ++prior) {
      PersistedRecord previous{};
      memcpy(&previous,
             input + sizeof(PersistedHeader) +
                 prior * sizeof(PersistedRecord),
             sizeof(previous));
      if (previous.codeId == candidate.codeId ||
          strcmp(previous.code, candidate.code) == 0) {
        return false;
      }
    }
  }

  reset();
  generation_ = header.generation;
  count_ = header.count;
  migrationRequired_ = legacy;
  for (size_t index = 0U; index < header.count; ++index) {
    PersistedRecord persisted{};
    memcpy(&persisted,
           input + sizeof(PersistedHeader) +
               index * sizeof(PersistedRecord),
           sizeof(persisted));
    copyFromPersisted(persisted, records_[index]);
  }
  return true;
}

}  // namespace unlocks
}  // namespace kitsu868
