#include "kitsu_nvs_headroom.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <nvs.h>
#endif

namespace kitsu868 {
namespace connectivity {

NvsHeadroomStatus KitsuNvsHeadroom::preparePairing(
    NvsHeadroomPlatform& platform) {
  NvsHeadroomStatus status{};
  if (!platform.eraseRetiredActionReplay(status.retiredActionReplay)) {
    status.result =
        NvsHeadroomResult::RetiredActionReplayCleanupFailed;
    return status;
  }
  if (!platform.eraseRetiredBluedroidConfig(status.retiredBluedroid)) {
    status.result = NvsHeadroomResult::RetiredBluedroidCleanupFailed;
    return status;
  }
  if (!platform.readStats(status.stats)) {
    status.result = NvsHeadroomResult::StatsFailed;
    return status;
  }
  if (status.stats.freeEntries < kPairingRequiredFreeEntries) {
    status.result = NvsHeadroomResult::Insufficient;
    return status;
  }
  status.result = status.retiredActionReplay || status.retiredBluedroid
      ? NvsHeadroomResult::ReadyReclaimed
      : NvsHeadroomResult::ReadyAlreadyClean;
  return status;
}

bool pairingHeadroomReady(const NvsHeadroomStatus& status) {
  return status.result == NvsHeadroomResult::ReadyAlreadyClean ||
      status.result == NvsHeadroomResult::ReadyReclaimed;
}

size_t pairingUsableEntries(const NvsHeadroomStatus& status) {
  return status.stats.freeEntries > kNvsEntriesPerPage
      ? status.stats.freeEntries - kNvsEntriesPerPage
      : 0U;
}

const char* nvsHeadroomResultName(NvsHeadroomResult result) {
  switch (result) {
    case NvsHeadroomResult::ReadyAlreadyClean:
      return "ready-already-clean";
    case NvsHeadroomResult::ReadyReclaimed:
      return "ready-reclaimed";
    case NvsHeadroomResult::RetiredActionReplayCleanupFailed:
      return "retired-action-replay-cleanup-failed";
    case NvsHeadroomResult::RetiredBluedroidCleanupFailed:
      return "retired-bluedroid-cleanup-failed";
    case NvsHeadroomResult::StatsFailed:
      return "stats-failed";
    case NvsHeadroomResult::Insufficient:
      return "insufficient";
  }
  return "unknown";
}

#if defined(ARDUINO_ARCH_ESP32)
namespace {

bool eraseExactRetiredBlob(const char* nvsNamespace, const char* key,
                           bool& changed) {
  changed = false;

  // A read-only open proves the namespace already exists; it cannot create
  // new NVS state while checking an already-clean device.
  nvs_handle_t readHandle = 0U;
  const esp_err_t opened =
      nvs_open(nvsNamespace, NVS_READONLY, &readHandle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return true;
  if (opened != ESP_OK) return false;
  size_t storedBytes = 0U;
  const esp_err_t found = nvs_get_blob(readHandle, key, nullptr, &storedBytes);
  nvs_close(readHandle);
  if (found == ESP_ERR_NVS_NOT_FOUND) return true;
  // A type mismatch is not the known retired blob and therefore fails closed.
  if (found != ESP_OK) return false;

  nvs_handle_t writeHandle = 0U;
  if (nvs_open(nvsNamespace, NVS_READWRITE, &writeHandle) != ESP_OK) {
    return false;
  }
  const esp_err_t erased = nvs_erase_key(writeHandle, key);
  const esp_err_t committed =
      erased == ESP_OK ? nvs_commit(writeHandle) : erased;
  size_t remainingBytes = 0U;
  const esp_err_t verified = committed == ESP_OK
      ? nvs_get_blob(writeHandle, key, nullptr, &remainingBytes)
      : committed;
  nvs_close(writeHandle);
  if (erased != ESP_OK || committed != ESP_OK ||
      verified != ESP_ERR_NVS_NOT_FOUND) {
    return false;
  }
  changed = true;
  return true;
}

}  // namespace

bool Esp32NvsHeadroomPlatform::eraseRetiredActionReplay(bool& changed) {
  return eraseExactRetiredBlob(kRetiredActionReplayNamespace,
                               kRetiredActionReplayKey, changed);
}

bool Esp32NvsHeadroomPlatform::eraseRetiredBluedroidConfig(bool& changed) {
  return eraseExactRetiredBlob(kRetiredBluedroidNamespace,
                               kRetiredBluedroidKey, changed);
}

bool Esp32NvsHeadroomPlatform::readStats(NvsHeadroomStats& output) {
  nvs_stats_t stats{};
  if (nvs_get_stats(nullptr, &stats) != ESP_OK) return false;
  output.usedEntries = stats.used_entries;
  output.freeEntries = stats.free_entries;
  output.totalEntries = stats.total_entries;
  return true;
}

#endif  // ARDUINO_ARCH_ESP32

}  // namespace connectivity
}  // namespace kitsu868
