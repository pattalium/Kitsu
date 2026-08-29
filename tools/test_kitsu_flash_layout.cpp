#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "../src/kitsu_flash_layout.h"

using namespace kitsu868::connectivity;

namespace {

FlashPartitionRecord record(const char* label, uint8_t type,
                            uint8_t subtype, uint32_t address,
                            uint32_t size) {
  FlashPartitionRecord output{};
  const size_t bytes = strlen(label);
  assert(bytes < sizeof(output.label));
  memcpy(output.label, label, bytes + 1U);
  output.type = type;
  output.subtype = subtype;
  output.address = address;
  output.size = size;
  return output;
}

std::vector<FlashPartitionRecord> exactPartitions() {
  return {
      record("nvs", 1U, 2U, kKitsuNvsOffset, kKitsuNvsBytes),
      record("otadata", 1U, 0U, kKitsuOtaDataOffset, kKitsuOtaDataBytes),
      record("app0", 0U, 0x10U, kKitsuApp0Offset,
             kKitsuAppPartitionBytes),
      record("app1", 0U, 0x11U, kKitsuApp1Offset,
             kKitsuAppPartitionBytes),
      record("spiffs", 1U, 0x82U, kKitsuSpiffsOffset,
             kKitsuSpiffsBytes),
      record("kitsu_conn", 1U, 0x40U, kKitsuConnectivityOffset,
             kKitsuConnectivityBytes),
      record("coredump", 1U, 3U, kKitsuCoredumpOffset,
             kKitsuCoredumpBytes),
  };
}

class FakePlatform final : public FlashLayoutPlatform {
 public:
  std::vector<FlashPartitionRecord> partitions = exactPartitions();
  FlashPartitionRecord running = partitions[2];
  bool failRead = false;
  bool failRunning = false;

  size_t partitionCount() override { return partitions.size(); }
  bool partitionAt(size_t index, FlashPartitionRecord& output) override {
    if (failRead || index >= partitions.size()) return false;
    output = partitions[index];
    return true;
  }
  bool runningPartition(FlashPartitionRecord& output) override {
    if (failRunning) return false;
    output = running;
    return true;
  }
};

}  // namespace

int main() {
  FakePlatform platform;
  assert(validateKitsuFlashLayout(platform) == FlashLayoutResult::Ready);
  std::reverse(platform.partitions.begin(), platform.partitions.end());
  assert(validateKitsuFlashLayout(platform) == FlashLayoutResult::Ready);
  platform.running = exactPartitions()[3];
  assert(validateKitsuFlashLayout(platform) == FlashLayoutResult::Ready);

  platform.partitions.push_back(record("extra", 1U, 0x99U, 0x650000U,
                                       0x10000U));
  assert(validateKitsuFlashLayout(platform) ==
         FlashLayoutResult::PartitionCountMismatch);
  platform.partitions.pop_back();

  platform.partitions[0].size -= 0x1000U;
  assert(validateKitsuFlashLayout(platform) ==
         FlashLayoutResult::PartitionMismatch);
  platform.partitions = exactPartitions();
  platform.running = record("app0", 0U, 0x10U, 0x10000U,
                            kKitsuAppPartitionBytes);
  assert(validateKitsuFlashLayout(platform) ==
         FlashLayoutResult::RunningPartitionMismatch);
  platform.running = exactPartitions()[2];
  platform.failRead = true;
  assert(validateKitsuFlashLayout(platform) ==
         FlashLayoutResult::PartitionReadFailed);
  platform.failRead = false;
  platform.failRunning = true;
  assert(validateKitsuFlashLayout(platform) ==
         FlashLayoutResult::RunningPartitionUnavailable);

  puts("Kitsu flash layout tests passed.");
  return 0;
}
