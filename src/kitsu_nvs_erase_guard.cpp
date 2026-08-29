#include "kitsu_nvs_erase_guard.h"

#include <atomic>
#include <cstring>

namespace kitsu868 {
namespace connectivity {
namespace {

std::atomic<bool> blocked{false};

#if defined(ARDUINO_ARCH_ESP32) || \
    defined(KITSU_NVS_ERASE_GUARD_HOST_TEST)
bool isWholeNvsErase(const esp_partition_t* partition, size_t offset,
                     size_t size) {
  return partition != nullptr &&
      partition->type == ESP_PARTITION_TYPE_DATA &&
      partition->subtype == ESP_PARTITION_SUBTYPE_DATA_NVS &&
      std::strcmp(partition->label, "nvs") == 0 &&
      partition->address == 0x009000U &&
      (partition->size == 0x005000U || partition->size == 0x040000U) &&
      offset == 0U && size == partition->size;
}
#endif

}  // namespace

bool destructiveNvsEraseBlocked() {
  return blocked.load(std::memory_order_acquire);
}

}  // namespace connectivity
}  // namespace kitsu868

#if defined(ARDUINO_ARCH_ESP32) || \
    defined(KITSU_NVS_ERASE_GUARD_HOST_TEST)
extern "C" esp_err_t __real_esp_partition_erase_range(
    const esp_partition_t* partition, size_t offset, size_t size);

extern "C" esp_err_t __wrap_esp_partition_erase_range(
    const esp_partition_t* partition, size_t offset, size_t size) {
  if (kitsu868::connectivity::isWholeNvsErase(partition, offset, size)) {
    kitsu868::connectivity::blocked.store(true, std::memory_order_release);
    return ESP_ERR_INVALID_STATE;
  }
  return __real_esp_partition_erase_range(partition, offset, size);
}
#endif
