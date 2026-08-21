#pragma once

#if defined(ARDUINO_ARCH_ESP32)

#include <Preferences.h>

#include "kitsu_gateway_action_runtime.h"

namespace kitsu868 {
namespace connectivity {

// Dedicated NVS namespace with two read-back-verified generations. The
// persisted values contain action UUIDs/deadlines/outcome tokens only; no
// HMAC key, message text, target, or signed body is stored here.
class Esp32GatewayLanReplayStorage final : public GatewayLanReplayStorage {
 public:
  ~Esp32GatewayLanReplayStorage() override;
  bool begin(const char* partitionLabel = nullptr);
  void end();

  GatewayLanReplayStorageResult readSlot(
      uint8_t slot, uint8_t* output, size_t outputCapacity,
      size_t& outputBytes) override;
  bool writeSlot(uint8_t slot, const uint8_t* input,
                 size_t inputBytes) override;

 private:
  Preferences preferences_{};
  bool begun_ = false;
};

}  // namespace connectivity
}  // namespace kitsu868

#endif  // ARDUINO_ARCH_ESP32
