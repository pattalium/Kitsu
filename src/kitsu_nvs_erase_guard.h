#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(KITSU_NVS_ERASE_GUARD_HOST_TEST)
typedef int32_t esp_err_t;
typedef uint8_t esp_partition_type_t;
typedef uint8_t esp_partition_subtype_t;
struct esp_partition_t {
  void* flash_chip;
  esp_partition_type_t type;
  esp_partition_subtype_t subtype;
  uint32_t address;
  uint32_t size;
  char label[17];
  bool encrypted;
};
constexpr esp_partition_type_t ESP_PARTITION_TYPE_DATA = 0x01U;
constexpr esp_partition_subtype_t ESP_PARTITION_SUBTYPE_DATA_NVS = 0x02U;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;
#elif defined(ARDUINO_ARCH_ESP32)
#include <esp_err.h>
#include <esp_partition.h>
#endif

namespace kitsu868 {
namespace connectivity {

// Arduino-ESP32 attempts to format the complete main NVS partition before
// setup() when nvs_flash_init reports NO_FREE_PAGES or NEW_VERSION_FOUND.
// Kitsu must preserve that partition for serial recovery, so the linker wraps
// the erase primitive and latches that exact destructive attempt.
bool destructiveNvsEraseBlocked();

}  // namespace connectivity
}  // namespace kitsu868

#if defined(ARDUINO_ARCH_ESP32) || \
    defined(KITSU_NVS_ERASE_GUARD_HOST_TEST)
extern "C" esp_err_t __wrap_esp_partition_erase_range(
    const esp_partition_t* partition, size_t offset, size_t size);
#endif
