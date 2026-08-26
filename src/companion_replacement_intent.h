#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_partition.h>
#endif

namespace kitsu868 {

// The transaction uses two sectors of the already-retired kitsu_conn
// partition. PREPARED is durable before the pack write; COMMITTED is written
// only after exact pack readback. Separate sectors mean a failed COMMITTED
// write cannot erase the source identity needed to retry. Neither sector
// consumes companion-partition bytes.
constexpr uint32_t KITSU_REPLACEMENT_PREPARED_FLASH_OFFSET = 0x7b0000U;
constexpr uint32_t KITSU_REPLACEMENT_COMMITTED_FLASH_OFFSET = 0x7b1000U;
constexpr size_t KITSU_REPLACEMENT_INTENT_SECTOR_BYTES = 0x1000U;
constexpr size_t KITSU_REPLACEMENT_TRANSACTION_BYTES = 0x2000U;
constexpr size_t KITSU_COMPANION_PACK_MAX_BYTES = 0x140000U;
constexpr uint16_t KITSU_REPLACEMENT_INTENT_SCHEMA = 1U;

#pragma pack(push, 1)
struct CompanionReplacementIntent {
  char magic[8];
  uint16_t schema;
  uint16_t recordBytes;
  uint32_t sourcePackId;
  uint32_t targetPackId;
  uint32_t targetRevision;
  uint32_t targetBytes;
  uint32_t targetPayloadCrc32;
  uint32_t targetHeaderCrc32;
  uint32_t recordCrc32;
};
#pragma pack(pop)

static_assert(sizeof(CompanionReplacementIntent) == 40,
              "Replacement intent wire record must remain 40 bytes");

struct CompanionReplacementTransaction {
  CompanionReplacementIntent prepared{};
  CompanionReplacementIntent committed{};
};
static_assert(sizeof(CompanionReplacementTransaction) == 80,
              "Replacement transaction must contain exactly two records");

uint32_t companionReplacementIntentCrc32(
    const CompanionReplacementIntent& intent);

bool companionReplacementIntentValid(
    const CompanionReplacementIntent& intent);

bool companionReplacementTransactionValid(
    const CompanionReplacementTransaction& transaction);

// Validates the complete record and binds it to both the state being replaced
// and the exact structurally validated target pack.  A same-pack revision
// update is deliberately not destructive and therefore never needs intent.
bool companionReplacementIntentAuthorizes(
    const CompanionReplacementIntent& intent,
    uint32_t storedPackId,
    uint32_t targetPackId,
    uint32_t targetRevision,
    uint32_t targetBytes,
    uint32_t targetPayloadCrc32,
    uint32_t targetHeaderCrc32);

bool companionReplacementTransactionAuthorizes(
    const CompanionReplacementTransaction& transaction,
    uint32_t storedPackId,
    uint32_t targetPackId,
    uint32_t targetRevision,
    uint32_t targetBytes,
    uint32_t targetPayloadCrc32,
    uint32_t targetHeaderCrc32);

#if defined(ARDUINO_ARCH_ESP32)

// Provides access only to the fixed first sector of the exact, retired
// kitsu_conn partition.  It deliberately exposes no arbitrary flash address.
class CompanionReplacementTransactionStorage {
 public:
  bool beginAndRead(CompanionReplacementTransaction& transaction);
  bool consume();
  bool preparedSectorCanonical() const {
    return preparedSectorCanonical_;
  }
  bool committedSectorCanonical() const {
    return committedSectorCanonical_;
  }

 private:
  bool exactPartition() const;
  bool sectorPaddingErased(size_t sectorOffset) const;
  const esp_partition_t* partition_ = nullptr;
  bool preparedSectorCanonical_ = false;
  bool committedSectorCanonical_ = false;
};

#endif  // ARDUINO_ARCH_ESP32

}  // namespace kitsu868
