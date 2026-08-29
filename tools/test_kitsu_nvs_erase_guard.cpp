#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/kitsu_nvs_erase_guard.h"

namespace {

size_t calls = 0U;
const esp_partition_t* lastPartition = nullptr;
size_t lastOffset = 0U;
size_t lastSize = 0U;
constexpr esp_err_t kForwardedResult = 77;

esp_partition_t partition(uint8_t type, uint8_t subtype, uint32_t address,
                          uint32_t size, const char* label) {
  esp_partition_t value{};
  value.type = type;
  value.subtype = subtype;
  value.address = address;
  value.size = size;
  memcpy(value.label, label, strlen(label) + 1U);
  return value;
}

void expectForwarded(const esp_partition_t* value, size_t offset,
                     size_t size) {
  const size_t before = calls;
  assert(__wrap_esp_partition_erase_range(value, offset, size) ==
         kForwardedResult);
  assert(calls == before + 1U);
  assert(lastPartition == value);
  assert(lastOffset == offset);
  assert(lastSize == size);
}

}  // namespace

extern "C" esp_err_t __real_esp_partition_erase_range(
    const esp_partition_t* value, size_t offset, size_t size) {
  ++calls;
  lastPartition = value;
  lastOffset = offset;
  lastSize = size;
  return kForwardedResult;
}

int main() {
  esp_partition_t expanded = partition(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS,
      0x009000U, 0x040000U, "nvs");
  esp_partition_t legacy = expanded;
  legacy.size = 0x005000U;
  esp_partition_t app = partition(0x00U, 0x10U, 0x050000U,
                                  0x300000U, "app0");
  esp_partition_t nvsKeys = partition(
      ESP_PARTITION_TYPE_DATA, 0x04U, 0x050000U, 0x001000U, "nvs_keys");
  esp_partition_t otherAddress = expanded;
  otherAddress.address = 0x100000U;
  esp_partition_t otherLabel = expanded;
  memcpy(otherLabel.label, "other\0", 6U);
  esp_partition_t otherSize = expanded;
  otherSize.size = 0x010000U;

  expectForwarded(nullptr, 0U, 0x1000U);
  expectForwarded(&expanded, 0U, 0x1000U);
  expectForwarded(&expanded, 0x1000U, expanded.size - 0x1000U);
  expectForwarded(&app, 0U, app.size);
  expectForwarded(&nvsKeys, 0U, nvsKeys.size);
  expectForwarded(&otherAddress, 0U, otherAddress.size);
  expectForwarded(&otherLabel, 0U, otherLabel.size);
  expectForwarded(&otherSize, 0U, otherSize.size);
  assert(!kitsu868::connectivity::destructiveNvsEraseBlocked());

  const size_t before = calls;
  assert(__wrap_esp_partition_erase_range(
             &legacy, 0U, legacy.size) == ESP_ERR_INVALID_STATE);
  assert(calls == before);
  assert(kitsu868::connectivity::destructiveNvsEraseBlocked());
  assert(__wrap_esp_partition_erase_range(
             &expanded, 0U, expanded.size) == ESP_ERR_INVALID_STATE);
  assert(calls == before);

  puts("Kitsu NVS erase guard tests passed.");
  return 0;
}
