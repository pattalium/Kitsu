#pragma once

#include <cstddef>
#include <cstdint>

struct esp_partition_t {
  uint32_t address = 0U;
  size_t size = 0U;
  uint8_t type = 0U;
  uint8_t subtype = 0U;
  char label[17]{};
};

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr uint8_t ESP_PARTITION_TYPE_DATA = 1U;
constexpr uint8_t ESP_PARTITION_SUBTYPE_ANY = 0xffU;
constexpr uint8_t ESP_PARTITION_SUBTYPE_DATA_SPIFFS = 0x82U;

const esp_partition_t* esp_partition_find_first(uint8_t type,
                                                uint8_t subtype,
                                                const char* label);
esp_err_t esp_partition_read(const esp_partition_t* partition, size_t offset,
                             void* output, size_t bytes);
esp_err_t esp_partition_write(const esp_partition_t* partition, size_t offset,
                              const void* input, size_t bytes);
esp_err_t esp_partition_erase_range(const esp_partition_t* partition,
                                    size_t offset, size_t bytes);
