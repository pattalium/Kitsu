#include "kitsu_esp32_gateway_action.h"

#if defined(ARDUINO_ARCH_ESP32)

#include <string.h>

namespace kitsu868 {
namespace connectivity {
namespace {

constexpr char kReplayNamespace[] = "kitsu_lan_act";
constexpr const char* kReplayKeys[2] = {"rx0", "rx1"};

void secureZero(void* memory, size_t bytes) {
  volatile uint8_t* output = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *output++ = 0U;
}

}  // namespace

Esp32GatewayLanReplayStorage::~Esp32GatewayLanReplayStorage() { end(); }

bool Esp32GatewayLanReplayStorage::begin(const char* partitionLabel) {
  end();
  begun_ = preferences_.begin(kReplayNamespace, false, partitionLabel);
  return begun_;
}

void Esp32GatewayLanReplayStorage::end() {
  if (begun_) preferences_.end();
  begun_ = false;
}

GatewayLanReplayStorageResult Esp32GatewayLanReplayStorage::readSlot(
    uint8_t slot, uint8_t* output, size_t outputCapacity,
    size_t& outputBytes) {
  outputBytes = 0U;
  if (!begun_ || slot >= 2U || !output || outputCapacity == 0U) {
    return GatewayLanReplayStorageResult::Failed;
  }
  memset(output, 0, outputCapacity);
  const size_t storedBytes =
      preferences_.getBytesLength(kReplayKeys[slot]);
  if (storedBytes == 0U) return GatewayLanReplayStorageResult::Missing;
  if (storedBytes != outputCapacity) {
    return GatewayLanReplayStorageResult::Corrupt;
  }
  const size_t read = preferences_.getBytes(
      kReplayKeys[slot], output, outputCapacity);
  if (read != outputCapacity) {
    secureZero(output, outputCapacity);
    return GatewayLanReplayStorageResult::Failed;
  }
  outputBytes = read;
  return GatewayLanReplayStorageResult::Ok;
}

bool Esp32GatewayLanReplayStorage::writeSlot(
    uint8_t slot, const uint8_t* input, size_t inputBytes) {
  if (!begun_ || slot >= 2U || !input || inputBytes == 0U) return false;
  return preferences_.putBytes(kReplayKeys[slot], input, inputBytes) ==
      inputBytes;
}

}  // namespace connectivity
}  // namespace kitsu868

#endif  // ARDUINO_ARCH_ESP32
