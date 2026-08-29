#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kitsu868 {
namespace connectivity {

constexpr uint32_t kKitsuFlashBytes = 0x800000UL;
constexpr uint32_t kKitsuNvsOffset = 0x009000UL;
constexpr uint32_t kKitsuNvsBytes = 0x040000UL;
constexpr uint32_t kKitsuLegacyNvsBytes = 0x005000UL;
constexpr uint32_t kKitsuNvsExtensionOffset = 0x00e000UL;
constexpr uint32_t kKitsuNvsExtensionBytes = 0x03b000UL;
constexpr uint32_t kKitsuOtaDataOffset = 0x049000UL;
constexpr uint32_t kKitsuOtaDataBytes = 0x002000UL;
constexpr uint32_t kKitsuApp0Offset = 0x050000UL;
constexpr uint32_t kKitsuApp1Offset = 0x350000UL;
constexpr uint32_t kKitsuAppPartitionBytes = 0x300000UL;
constexpr uint32_t kKitsuSpiffsOffset = 0x670000UL;
constexpr uint32_t kKitsuSpiffsBytes = 0x140000UL;
constexpr uint32_t kKitsuConnectivityOffset = 0x7b0000UL;
constexpr uint32_t kKitsuConnectivityBytes = 0x040000UL;
constexpr uint32_t kKitsuCoredumpOffset = 0x7f0000UL;
constexpr uint32_t kKitsuCoredumpBytes = 0x010000UL;
constexpr size_t kKitsuExpectedPartitionCount = 7U;

static_assert(kKitsuNvsOffset + kKitsuNvsBytes == kKitsuOtaDataOffset,
              "NVS must end exactly at OTA data");
static_assert(kKitsuNvsExtensionOffset ==
                  kKitsuNvsOffset + kKitsuLegacyNvsBytes,
              "legacy NVS prefix must remain in place");
static_assert(kKitsuNvsExtensionOffset + kKitsuNvsExtensionBytes ==
                  kKitsuOtaDataOffset,
              "NVS extension must end exactly at OTA data");
static_assert(kKitsuOtaDataOffset + kKitsuOtaDataBytes == 0x04b000UL,
              "OTA data layout changed");
static_assert(kKitsuApp0Offset + kKitsuAppPartitionBytes ==
                  kKitsuApp1Offset,
              "OTA slots must be contiguous");
static_assert(kKitsuApp1Offset + kKitsuAppPartitionBytes == 0x650000UL,
              "second OTA slot layout changed");
static_assert(kKitsuSpiffsOffset + kKitsuSpiffsBytes ==
                  kKitsuConnectivityOffset,
              "SPIFFS and connectivity layout changed");
static_assert(kKitsuConnectivityOffset + kKitsuConnectivityBytes ==
                  kKitsuCoredumpOffset,
              "connectivity and coredump layout changed");
static_assert(kKitsuCoredumpOffset + kKitsuCoredumpBytes ==
                  kKitsuFlashBytes,
              "partition layout must end at 8 MiB");

struct FlashPartitionRecord {
  char label[17]{};
  uint8_t type = 0xffU;
  uint8_t subtype = 0xffU;
  uint32_t address = 0U;
  uint32_t size = 0U;
};

class FlashLayoutPlatform {
 public:
  virtual ~FlashLayoutPlatform() = default;
  virtual size_t partitionCount() = 0;
  virtual bool partitionAt(size_t index, FlashPartitionRecord& output) = 0;
  virtual bool runningPartition(FlashPartitionRecord& output) = 0;
};

enum class FlashLayoutResult : uint8_t {
  Ready = 0,
  PartitionCountMismatch,
  PartitionReadFailed,
  PartitionMismatch,
  RunningPartitionUnavailable,
  RunningPartitionMismatch,
};

FlashLayoutResult validateKitsuFlashLayout(FlashLayoutPlatform& platform);
const char* flashLayoutResultName(FlashLayoutResult result);

#if defined(ARDUINO_ARCH_ESP32)

class Esp32FlashLayoutPlatform final : public FlashLayoutPlatform {
 public:
  size_t partitionCount() override;
  bool partitionAt(size_t index, FlashPartitionRecord& output) override;
  bool runningPartition(FlashPartitionRecord& output) override;
};

#endif

}  // namespace connectivity
}  // namespace kitsu868
