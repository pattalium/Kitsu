#include "companion_replacement_intent.h"

#include <string.h>

#if defined(ARDUINO_ARCH_ESP32)
#include "kitsu_legacy_connectivity_retirement.h"
#endif

namespace kitsu868 {
namespace {

constexpr char kIntentMagic[8] = {'K', '8', '6', '8', 'R', 'P', '1', '\0'};

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t length) {
  while (length--) {
    crc ^= *data++;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xedb88320UL & (0U - (crc & 1U)));
    }
  }
  return crc;
}

bool validRecord(const CompanionReplacementIntent& intent) {
  if (memcmp(intent.magic, kIntentMagic, sizeof(kIntentMagic)) != 0 ||
      intent.schema != KITSU_REPLACEMENT_INTENT_SCHEMA ||
      intent.recordBytes != sizeof(CompanionReplacementIntent) ||
      intent.sourcePackId == 0U ||
      intent.targetPackId == 0U || intent.targetRevision == 0U ||
      intent.targetBytes == 0U ||
      intent.targetBytes > KITSU_COMPANION_PACK_MAX_BYTES ||
      intent.sourcePackId == intent.targetPackId) {
    return false;
  }
  return companionReplacementIntentCrc32(intent) == intent.recordCrc32;
}

bool targetMatches(const CompanionReplacementIntent& intent,
                   uint32_t targetPackId,
                   uint32_t targetRevision,
                   uint32_t targetBytes,
                   uint32_t targetPayloadCrc32,
                   uint32_t targetHeaderCrc32) {
  return intent.targetPackId == targetPackId &&
      intent.targetRevision == targetRevision &&
      intent.targetBytes == targetBytes &&
      intent.targetPayloadCrc32 == targetPayloadCrc32 &&
      intent.targetHeaderCrc32 == targetHeaderCrc32;
}

}  // namespace

uint32_t companionReplacementIntentCrc32(
    const CompanionReplacementIntent& intent) {
  CompanionReplacementIntent copy = intent;
  copy.recordCrc32 = 0U;
  uint32_t crc = crc32Update(
      0xffffffffUL,
      reinterpret_cast<const uint8_t*>(&copy) + sizeof(copy.magic),
      sizeof(copy) - sizeof(copy.magic));
  return ~crc;
}

bool companionReplacementIntentValid(
    const CompanionReplacementIntent& intent) {
  return validRecord(intent);
}

bool companionReplacementTransactionValid(
    const CompanionReplacementTransaction& transaction) {
  return validRecord(transaction.prepared) &&
      validRecord(transaction.committed) &&
      memcmp(&transaction.prepared, &transaction.committed,
             sizeof(transaction.prepared)) == 0;
}

bool companionReplacementIntentAuthorizes(
    const CompanionReplacementIntent& intent,
    uint32_t storedPackId,
    uint32_t targetPackId,
    uint32_t targetRevision,
    uint32_t targetBytes,
    uint32_t targetPayloadCrc32,
    uint32_t targetHeaderCrc32) {
  return validRecord(intent) && intent.sourcePackId == storedPackId &&
      targetMatches(intent, targetPackId, targetRevision, targetBytes,
                    targetPayloadCrc32, targetHeaderCrc32);
}

bool companionReplacementTransactionAuthorizes(
    const CompanionReplacementTransaction& transaction,
    uint32_t storedPackId,
    uint32_t targetPackId,
    uint32_t targetRevision,
    uint32_t targetBytes,
    uint32_t targetPayloadCrc32,
    uint32_t targetHeaderCrc32) {
  return companionReplacementTransactionValid(transaction) &&
      companionReplacementIntentAuthorizes(
          transaction.committed, storedPackId, targetPackId, targetRevision,
          targetBytes, targetPayloadCrc32, targetHeaderCrc32);
}

}  // namespace kitsu868

#if defined(ARDUINO_ARCH_ESP32)

namespace kitsu868 {

static_assert(KITSU_REPLACEMENT_PREPARED_FLASH_OFFSET ==
                  connectivity::kLegacyConnectivityPartitionAddress,
              "PREPARED must start at the retired partition boundary");
static_assert(KITSU_REPLACEMENT_COMMITTED_FLASH_OFFSET ==
                  KITSU_REPLACEMENT_PREPARED_FLASH_OFFSET +
                      KITSU_REPLACEMENT_INTENT_SECTOR_BYTES,
              "COMMITTED must use the next independent flash sector");
static_assert(KITSU_REPLACEMENT_TRANSACTION_BYTES <=
                  connectivity::kLegacyConnectivityPartitionBytes,
              "replacement transaction must stay in the retired partition");

bool CompanionReplacementTransactionStorage::exactPartition() const {
  return partition_ &&
      strcmp(partition_->label,
             connectivity::kLegacyConnectivityPartitionLabel) == 0 &&
      partition_->type == ESP_PARTITION_TYPE_DATA &&
      static_cast<uint8_t>(partition_->subtype) ==
          connectivity::kLegacyConnectivityPartitionSubtype &&
      partition_->address == KITSU_REPLACEMENT_PREPARED_FLASH_OFFSET &&
      partition_->size ==
          connectivity::kLegacyConnectivityPartitionBytes;
}

bool CompanionReplacementTransactionStorage::sectorPaddingErased(
    size_t sectorOffset) const {
  uint8_t scratch[256]{};
  size_t offset = sectorOffset + sizeof(CompanionReplacementIntent);
  const size_t end = sectorOffset + KITSU_REPLACEMENT_INTENT_SECTOR_BYTES;
  while (offset < end) {
    const size_t amount = end - offset < sizeof(scratch)
        ? end - offset
        : sizeof(scratch);
    if (esp_partition_read(partition_, offset, scratch, amount) != ESP_OK) {
      return false;
    }
    for (size_t index = 0U; index < amount; ++index) {
      if (scratch[index] != 0xffU) return false;
    }
    offset += amount;
  }
  return true;
}

bool CompanionReplacementTransactionStorage::beginAndRead(
    CompanionReplacementTransaction& transaction) {
  transaction = CompanionReplacementTransaction{};
  preparedSectorCanonical_ = false;
  committedSectorCanonical_ = false;
  partition_ = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA,
      static_cast<esp_partition_subtype_t>(
          connectivity::kLegacyConnectivityPartitionSubtype),
      connectivity::kLegacyConnectivityPartitionLabel);
  if (!exactPartition()) {
    partition_ = nullptr;
    return false;
  }
  const bool read = esp_partition_read(
      partition_, 0U, &transaction.prepared,
      sizeof(transaction.prepared)) == ESP_OK &&
      esp_partition_read(partition_, KITSU_REPLACEMENT_INTENT_SECTOR_BYTES,
                         &transaction.committed,
                         sizeof(transaction.committed)) == ESP_OK;
  if (!read) return false;
  preparedSectorCanonical_ = sectorPaddingErased(0U);
  committedSectorCanonical_ = sectorPaddingErased(
      KITSU_REPLACEMENT_INTENT_SECTOR_BYTES);
  return true;
}

bool CompanionReplacementTransactionStorage::consume() {
  if (!exactPartition() ||
      esp_partition_erase_range(partition_, 0U,
                                KITSU_REPLACEMENT_TRANSACTION_BYTES) !=
          ESP_OK) {
    return false;
  }

  uint8_t verification[256]{};
  for (size_t offset = 0U; offset < KITSU_REPLACEMENT_TRANSACTION_BYTES;
       offset += sizeof(verification)) {
    if (esp_partition_read(partition_, offset, verification,
                           sizeof(verification)) != ESP_OK) {
      return false;
    }
    for (uint8_t value : verification) {
      if (value != 0xffU) return false;
    }
  }
  return true;
}

}  // namespace kitsu868

#endif  // ARDUINO_ARCH_ESP32
