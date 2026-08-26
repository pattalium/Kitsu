#include "kitsu_legacy_connectivity_retirement.h"

#include "companion_replacement_intent.h"

#include <string.h>

namespace kitsu868 {
namespace connectivity {
namespace {

constexpr size_t kVerificationChunkBytes = 512U;

bool exactPartition(const LegacyConnectivityPartition& partition) {
  return strcmp(partition.label, kLegacyConnectivityPartitionLabel) == 0 &&
      partition.type == kLegacyConnectivityPartitionType &&
      partition.subtype == kLegacyConnectivityPartitionSubtype &&
      partition.address == kLegacyConnectivityPartitionAddress &&
      partition.size == kLegacyConnectivityPartitionBytes;
}

bool verifyErased(LegacyConnectivityRetirementPlatform& platform,
                  size_t firstByte,
                  bool& erased) {
  erased = true;
  uint8_t chunk[kVerificationChunkBytes]{};
  for (size_t offset = firstByte; offset < kLegacyConnectivityPartitionBytes;
       offset += sizeof(chunk)) {
    if (!platform.readPartition(offset, chunk, sizeof(chunk))) return false;
    for (size_t index = 0U; index < sizeof(chunk); ++index) {
      if (chunk[index] != 0xffU) erased = false;
    }
  }
  return true;
}

bool preservedPrefixBytes(LegacyConnectivityPreservation preservation,
                          size_t& bytes) {
  switch (preservation) {
    case LegacyConnectivityPreservation::None:
      bytes = 0U;
      return true;
    case LegacyConnectivityPreservation::Prepared:
      bytes = KITSU_REPLACEMENT_INTENT_SECTOR_BYTES;
      return true;
    case LegacyConnectivityPreservation::Transaction:
      bytes = KITSU_REPLACEMENT_TRANSACTION_BYTES;
      return true;
  }
  return false;
}

}  // namespace

const char* legacyConnectivityRetirementResultName(
    LegacyConnectivityRetirementResult result) {
  switch (result) {
    case LegacyConnectivityRetirementResult::OkAlreadyClean:
      return "already_clean";
    case LegacyConnectivityRetirementResult::OkRetired: return "retired";
    case LegacyConnectivityRetirementResult::InvalidPartition:
      return "invalid_partition";
    case LegacyConnectivityRetirementResult::PartitionReadFailed:
      return "partition_read_failed";
    case LegacyConnectivityRetirementResult::PartitionEraseFailed:
      return "partition_erase_failed";
    case LegacyConnectivityRetirementResult::PartitionReadbackFailed:
      return "partition_readback_failed";
    case LegacyConnectivityRetirementResult::ReplayNamespaceFailed:
      return "replay_namespace_failed";
  }
  return "unknown";
}

bool legacyConnectivityRetirementSucceeded(
    LegacyConnectivityRetirementResult result) {
  return result == LegacyConnectivityRetirementResult::OkAlreadyClean ||
      result == LegacyConnectivityRetirementResult::OkRetired;
}

LegacyConnectivityRetirementResult KitsuLegacyConnectivityRetirement::run(
    LegacyConnectivityRetirementPlatform& platform,
    LegacyConnectivityPreservation preservation) {
  LegacyConnectivityPartition partition{};
  if (!platform.inspectPartition(partition) || !exactPartition(partition)) {
    return LegacyConnectivityRetirementResult::InvalidPartition;
  }

  size_t firstRetiredByte = 0U;
  if (!preservedPrefixBytes(preservation, firstRetiredByte)) {
    return LegacyConnectivityRetirementResult::InvalidPartition;
  }
  bool partitionErased = false;
  if (!verifyErased(platform, firstRetiredByte, partitionErased)) {
    return LegacyConnectivityRetirementResult::PartitionReadFailed;
  }
  bool retired = false;
  if (!partitionErased) {
    bool erased = false;
    switch (preservation) {
      case LegacyConnectivityPreservation::None:
        erased = platform.eraseEntirePartition();
        break;
      case LegacyConnectivityPreservation::Prepared:
        erased = platform.eraseAfterReplacementPrepared();
        break;
      case LegacyConnectivityPreservation::Transaction:
        erased = platform.eraseAfterReplacementTransaction();
        break;
    }
    if (!erased) {
      return LegacyConnectivityRetirementResult::PartitionEraseFailed;
    }
    bool readbackErased = false;
    if (!verifyErased(platform, firstRetiredByte, readbackErased) ||
        !readbackErased) {
      return LegacyConnectivityRetirementResult::PartitionReadbackFailed;
    }
    retired = true;
  }

  bool replayNamespaceChanged = false;
  if (!platform.clearLegacyReplayNamespace(replayNamespaceChanged)) {
    return LegacyConnectivityRetirementResult::ReplayNamespaceFailed;
  }
  retired = retired || replayNamespaceChanged;
  return retired ? LegacyConnectivityRetirementResult::OkRetired
                 : LegacyConnectivityRetirementResult::OkAlreadyClean;
}

}  // namespace connectivity
}  // namespace kitsu868

#if defined(ARDUINO_ARCH_ESP32)

#include <nvs.h>

namespace kitsu868 {
namespace connectivity {
namespace {

bool exactEsp32Partition(const esp_partition_t* partition) {
  return partition &&
      strcmp(partition->label, kLegacyConnectivityPartitionLabel) == 0 &&
      partition->type == ESP_PARTITION_TYPE_DATA &&
      static_cast<uint8_t>(partition->subtype) ==
          kLegacyConnectivityPartitionSubtype &&
      partition->address == kLegacyConnectivityPartitionAddress &&
      partition->size == kLegacyConnectivityPartitionBytes;
}

}  // namespace

bool Esp32LegacyConnectivityRetirementPlatform::inspectPartition(
    LegacyConnectivityPartition& output) {
  output = LegacyConnectivityPartition{};
  partition_ = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA,
      static_cast<esp_partition_subtype_t>(
          kLegacyConnectivityPartitionSubtype),
      kLegacyConnectivityPartitionLabel);
  if (!partition_) return false;
  memcpy(output.label, partition_->label, sizeof(output.label) - 1U);
  output.label[sizeof(output.label) - 1U] = '\0';
  output.type = static_cast<uint8_t>(partition_->type);
  output.subtype = static_cast<uint8_t>(partition_->subtype);
  output.address = partition_->address;
  output.size = partition_->size;
  return true;
}

bool Esp32LegacyConnectivityRetirementPlatform::readPartition(
    size_t offset, uint8_t* output, size_t outputBytes) {
  if (!exactEsp32Partition(partition_) ||
      (!output && outputBytes != 0U) ||
      offset > kLegacyConnectivityPartitionBytes ||
      outputBytes > kLegacyConnectivityPartitionBytes - offset) {
    return false;
  }
  return esp_partition_read(partition_, offset, output, outputBytes) == ESP_OK;
}

bool Esp32LegacyConnectivityRetirementPlatform::eraseEntirePartition() {
  return exactEsp32Partition(partition_) &&
      esp_partition_erase_range(partition_, 0U,
                                kLegacyConnectivityPartitionBytes) == ESP_OK;
}

bool Esp32LegacyConnectivityRetirementPlatform::
    eraseAfterReplacementPrepared() {
  return exactEsp32Partition(partition_) &&
      esp_partition_erase_range(
          partition_, KITSU_REPLACEMENT_INTENT_SECTOR_BYTES,
          kLegacyConnectivityPartitionBytes -
              KITSU_REPLACEMENT_INTENT_SECTOR_BYTES) == ESP_OK;
}

bool Esp32LegacyConnectivityRetirementPlatform::
    eraseAfterReplacementTransaction() {
  return exactEsp32Partition(partition_) &&
      esp_partition_erase_range(
          partition_, KITSU_REPLACEMENT_TRANSACTION_BYTES,
          kLegacyConnectivityPartitionBytes -
              KITSU_REPLACEMENT_TRANSACTION_BYTES) == ESP_OK;
}

bool Esp32LegacyConnectivityRetirementPlatform::clearLegacyReplayNamespace(
    bool& changed) {
  changed = false;
  nvs_handle_t handle = 0U;
  esp_err_t opened = nvs_open(kLegacyLanReplayNamespace, NVS_READONLY,
                              &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return true;
  if (opened != ESP_OK) return false;
  size_t usedEntries = 0U;
  const esp_err_t counted = nvs_get_used_entry_count(handle, &usedEntries);
  nvs_close(handle);
  if (counted != ESP_OK) return false;
  if (usedEntries == 0U) return true;

  opened = nvs_open(kLegacyLanReplayNamespace, NVS_READWRITE, &handle);
  if (opened != ESP_OK) return false;
  const esp_err_t erased = nvs_erase_all(handle);
  const esp_err_t committed = erased == ESP_OK ? nvs_commit(handle) : erased;
  size_t remainingEntries = 0U;
  const esp_err_t verified = committed == ESP_OK
      ? nvs_get_used_entry_count(handle, &remainingEntries)
      : committed;
  nvs_close(handle);
  if (erased != ESP_OK || committed != ESP_OK || verified != ESP_OK ||
      remainingEntries != 0U) {
    return false;
  }
  changed = true;
  return true;
}

}  // namespace connectivity
}  // namespace kitsu868

#endif  // ARDUINO_ARCH_ESP32
