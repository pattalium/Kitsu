#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_partition.h>
#endif

namespace kitsu868 {
namespace connectivity {

constexpr char kLegacyConnectivityPartitionLabel[] = "kitsu_conn";
constexpr char kLegacyLanReplayNamespace[] = "kitsu_lan_act";
constexpr uint8_t kLegacyConnectivityPartitionType = 0x01U;
constexpr uint8_t kLegacyConnectivityPartitionSubtype = 0x40U;
constexpr uint32_t kLegacyConnectivityPartitionAddress = 0x7b0000UL;
constexpr size_t kLegacyConnectivityPartitionBytes = 0x40000U;

enum class LegacyConnectivityRetirementResult : uint8_t {
  OkAlreadyClean = 0,
  OkRetired,
  InvalidPartition,
  PartitionReadFailed,
  PartitionEraseFailed,
  PartitionReadbackFailed,
  ReplayNamespaceFailed,
};

const char* legacyConnectivityRetirementResultName(
    LegacyConnectivityRetirementResult result);
bool legacyConnectivityRetirementSucceeded(
    LegacyConnectivityRetirementResult result);

struct LegacyConnectivityPartition {
  char label[17]{};
  uint8_t type = 0U;
  uint8_t subtype = 0U;
  uint32_t address = 0U;
  size_t size = 0U;
};

// Deliberately exposes no arbitrary erase primitive: an implementation can
// erase only the one retired partition that it previously inspected.
class LegacyConnectivityRetirementPlatform {
 public:
  virtual ~LegacyConnectivityRetirementPlatform() = default;
  virtual bool inspectPartition(LegacyConnectivityPartition& output) = 0;
  virtual bool readPartition(size_t offset, uint8_t* output,
                             size_t outputBytes) = 0;
  virtual bool eraseEntirePartition() = 0;
  virtual bool clearLegacyReplayNamespace(bool& changed) = 0;
};

class KitsuLegacyConnectivityRetirement {
 public:
  static LegacyConnectivityRetirementResult run(
      LegacyConnectivityRetirementPlatform& platform);
};

#if defined(ARDUINO_ARCH_ESP32)

class Esp32LegacyConnectivityRetirementPlatform final
    : public LegacyConnectivityRetirementPlatform {
 public:
  bool inspectPartition(LegacyConnectivityPartition& output) override;
  bool readPartition(size_t offset, uint8_t* output,
                     size_t outputBytes) override;
  bool eraseEntirePartition() override;
  bool clearLegacyReplayNamespace(bool& changed) override;

 private:
  const esp_partition_t* partition_ = nullptr;
};

#endif  // ARDUINO_ARCH_ESP32

}  // namespace connectivity
}  // namespace kitsu868
