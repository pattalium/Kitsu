#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kitsu868 {
namespace unlocks {

constexpr size_t kCodeCapacity = 12U;
constexpr size_t kCodeCharacters = 15U;
constexpr size_t kFormattedCodeBytes = 21U;  // K8-XXXXX-XXXXX-XXXXX + NUL
constexpr size_t kCreatureNameBytes = 24U;
constexpr size_t kSourceNameBytes = 16U;
constexpr uint16_t kStoreSchema = 1U;

enum class Rarity : uint8_t {
  Common = 0,
  Uncommon,
  Rare,
  VeryRare,
  Epic,
  Legendary,
  Mythical,
};

const char* rarityName(Rarity rarity);
bool parseRarity(const char* value, Rarity& output);

struct CodeRecord {
  uint32_t codeId = 0U;
  char code[kFormattedCodeBytes]{};
  uint32_t packId = 0U;
  char creatureName[kCreatureNameBytes + 1U]{};
  Rarity rarity = Rarity::Common;
  char source[kSourceNameBytes + 1U]{};
  uint32_t acquiredAtEpoch = 0U;
  bool redeemed = false;
  bool installed = false;
};

enum class AddResult : uint8_t {
  Added = 0,
  Duplicate,
  Full,
  Invalid,
};

// A small device-owned ledger. A website may reveal a normal .k868 only after
// the connected Kitsu confirms an exact code from this ledger. The pack format
// itself remains unchanged and intentionally contains no entitlement data.
class CodeStore {
 public:
  CodeStore();

  void reset();
  size_t count() const;
  uint32_t generation() const;
  bool at(size_t index, CodeRecord& output) const;
  bool find(const char* code, CodeRecord& output) const;
  bool findById(uint32_t codeId, CodeRecord& output) const;
  AddResult add(const CodeRecord& record);
  bool setRedeemed(uint32_t codeId, bool redeemed);
  bool setInstalled(uint32_t codeId, bool installed);

  // Uses exactly 10 caller-provided entropy bytes. The human-readable code is
  // Crockford-style base32 with a device-local ledger check; raw codes are
  // never printed or derived from a public device identifier.
  static bool generateCode(const uint8_t entropy[10],
                           char output[kFormattedCodeBytes]);
  static bool normalizeCode(const char* input,
                            char output[kFormattedCodeBytes]);
  static uint32_t codeId(const char* normalizedCode);

  // Versioned, CRC-protected persistence. This is integrity for local flash,
  // not a signature and not protection for a downloaded .k868 file.
  size_t serializedBytes() const;
  bool serialize(uint8_t* output, size_t capacity, size_t& outputBytes) const;
  bool load(const uint8_t* input, size_t inputBytes);

 private:
  CodeRecord records_[kCodeCapacity]{};
  uint8_t count_ = 0U;
  uint32_t generation_ = 0U;
};

}  // namespace unlocks
}  // namespace kitsu868
