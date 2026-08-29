#include "kitsu_flash_layout.h"

#include <string.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_ota_ops.h>
#include <esp_partition.h>
#endif

namespace kitsu868 {
namespace connectivity {
namespace {

constexpr uint8_t kPartitionTypeApp = 0x00U;
constexpr uint8_t kPartitionTypeData = 0x01U;
constexpr uint8_t kPartitionSubtypeOtaData = 0x00U;
constexpr uint8_t kPartitionSubtypeNvs = 0x02U;
constexpr uint8_t kPartitionSubtypeCoredump = 0x03U;
constexpr uint8_t kPartitionSubtypeApp0 = 0x10U;
constexpr uint8_t kPartitionSubtypeApp1 = 0x11U;
constexpr uint8_t kPartitionSubtypeConnectivity = 0x40U;
constexpr uint8_t kPartitionSubtypeSpiffs = 0x82U;

struct ExpectedPartition {
  const char* label;
  uint8_t type;
  uint8_t subtype;
  uint32_t address;
  uint32_t size;
};

constexpr ExpectedPartition kExpected[kKitsuExpectedPartitionCount] = {
    {"nvs", kPartitionTypeData, kPartitionSubtypeNvs,
     kKitsuNvsOffset, kKitsuNvsBytes},
    {"otadata", kPartitionTypeData, kPartitionSubtypeOtaData,
     kKitsuOtaDataOffset, kKitsuOtaDataBytes},
    {"app0", kPartitionTypeApp, kPartitionSubtypeApp0,
     kKitsuApp0Offset, kKitsuAppPartitionBytes},
    {"app1", kPartitionTypeApp, kPartitionSubtypeApp1,
     kKitsuApp1Offset, kKitsuAppPartitionBytes},
    {"spiffs", kPartitionTypeData, kPartitionSubtypeSpiffs,
     kKitsuSpiffsOffset, kKitsuSpiffsBytes},
    {"kitsu_conn", kPartitionTypeData, kPartitionSubtypeConnectivity,
     kKitsuConnectivityOffset, kKitsuConnectivityBytes},
    {"coredump", kPartitionTypeData, kPartitionSubtypeCoredump,
     kKitsuCoredumpOffset, kKitsuCoredumpBytes},
};

bool matches(const FlashPartitionRecord& actual,
             const ExpectedPartition& expected) {
  return strcmp(actual.label, expected.label) == 0 &&
      actual.type == expected.type && actual.subtype == expected.subtype &&
      actual.address == expected.address && actual.size == expected.size;
}

#if defined(ARDUINO_ARCH_ESP32)
void copyPartition(const esp_partition_t& input,
                   FlashPartitionRecord& output) {
  output = FlashPartitionRecord{};
  memcpy(output.label, input.label, sizeof(input.label));
  output.label[sizeof(output.label) - 1U] = '\0';
  output.type = input.type;
  output.subtype = input.subtype;
  output.address = input.address;
  output.size = input.size;
}
#endif

}  // namespace

FlashLayoutResult validateKitsuFlashLayout(FlashLayoutPlatform& platform) {
  if (platform.partitionCount() != kKitsuExpectedPartitionCount) {
    return FlashLayoutResult::PartitionCountMismatch;
  }
  bool found[kKitsuExpectedPartitionCount]{};
  for (size_t index = 0U; index < kKitsuExpectedPartitionCount; ++index) {
    FlashPartitionRecord actual{};
    if (!platform.partitionAt(index, actual)) {
      return FlashLayoutResult::PartitionReadFailed;
    }
    bool matched = false;
    for (size_t expected = 0U; expected < kKitsuExpectedPartitionCount;
         ++expected) {
      if (!found[expected] && matches(actual, kExpected[expected])) {
        found[expected] = true;
        matched = true;
        break;
      }
    }
    if (!matched) return FlashLayoutResult::PartitionMismatch;
  }
  FlashPartitionRecord running{};
  if (!platform.runningPartition(running)) {
    return FlashLayoutResult::RunningPartitionUnavailable;
  }
  return matches(running, kExpected[2]) || matches(running, kExpected[3])
             ? FlashLayoutResult::Ready
             : FlashLayoutResult::RunningPartitionMismatch;
}

const char* flashLayoutResultName(FlashLayoutResult result) {
  switch (result) {
    case FlashLayoutResult::Ready: return "ready";
    case FlashLayoutResult::PartitionCountMismatch:
      return "partition-count-mismatch";
    case FlashLayoutResult::PartitionReadFailed:
      return "partition-read-failed";
    case FlashLayoutResult::PartitionMismatch: return "partition-mismatch";
    case FlashLayoutResult::RunningPartitionUnavailable:
      return "running-partition-unavailable";
    case FlashLayoutResult::RunningPartitionMismatch:
      return "running-partition-mismatch";
  }
  return "unknown";
}

#if defined(ARDUINO_ARCH_ESP32)

size_t Esp32FlashLayoutPlatform::partitionCount() {
  size_t count = 0U;
  esp_partition_iterator_t iterator = esp_partition_find(
      ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (iterator) {
    ++count;
    iterator = esp_partition_next(iterator);
  }
  return count;
}

bool Esp32FlashLayoutPlatform::partitionAt(
    size_t index, FlashPartitionRecord& output) {
  esp_partition_iterator_t iterator = esp_partition_find(
      ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  size_t current = 0U;
  while (iterator && current < index) {
    iterator = esp_partition_next(iterator);
    ++current;
  }
  if (!iterator) return false;
  const esp_partition_t* partition = esp_partition_get(iterator);
  if (!partition) {
    esp_partition_iterator_release(iterator);
    return false;
  }
  copyPartition(*partition, output);
  esp_partition_iterator_release(iterator);
  return true;
}

bool Esp32FlashLayoutPlatform::runningPartition(
    FlashPartitionRecord& output) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running) return false;
  copyPartition(*running, output);
  return true;
}

#endif

}  // namespace connectivity
}  // namespace kitsu868
