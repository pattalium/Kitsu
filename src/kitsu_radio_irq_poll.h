#pragma once

#include <stdint.h>

namespace kitsu868 {
namespace mesh {

// Heltec V3's SX1262 DIO1 interrupt is level-latched until RadioLib clears the
// radio IRQ status.  MeshCore's callback only ORs a flag; packet reads and TX
// completion already happen from the Arduino loop.  Polling that latched line
// therefore preserves the existing state machine while avoiding the ESP32-S3
// Arduino 2.0.x gpio_isr_register() path, whose 1,024-byte ipc1 stack can
// overflow intermittently during interrupt allocation.
constexpr uint8_t kKitsuRadioDio1Pin = 14U;
constexpr int kKitsuRadioDio1RisingMode = 1;

using RadioIrqCallback = void (*)(void);

struct RadioIrqPollDiagnostics {
  uint32_t polls = 0U;
  uint32_t highPolls = 0U;
  uint32_t highEdges = 0U;
  uint32_t callbacks = 0U;
};

class LatchedRadioIrqPoll {
 public:
  bool claim(uint8_t pin, RadioIrqCallback callback, int mode) {
    if (pin != kKitsuRadioDio1Pin ||
        mode != kKitsuRadioDio1RisingMode || !callback) {
      return false;
    }
    callback_ = callback;
    return true;
  }

  bool release(uint8_t pin) {
    if (pin != kKitsuRadioDio1Pin) return false;
    callback_ = nullptr;
    return true;
  }

  bool poll(bool asserted) {
    incrementSaturating(diagnostics_.polls);
    if (asserted) {
      incrementSaturating(diagnostics_.highPolls);
      if (!wasAsserted_) incrementSaturating(diagnostics_.highEdges);
    }
    wasAsserted_ = asserted;
    RadioIrqCallback callback = callback_;
    if (!asserted || !callback) return false;
    callback();
    incrementSaturating(diagnostics_.callbacks);
    return true;
  }

  bool claimed() const { return callback_ != nullptr; }

  RadioIrqPollDiagnostics diagnostics() const { return diagnostics_; }

  void clearDiagnostics() {
    diagnostics_ = RadioIrqPollDiagnostics{};
    wasAsserted_ = false;
  }

 private:
  static void incrementSaturating(uint32_t& value) {
    if (value != UINT32_MAX) ++value;
  }

  RadioIrqCallback callback_ = nullptr;
  RadioIrqPollDiagnostics diagnostics_{};
  bool wasAsserted_ = false;
};

}  // namespace mesh
}  // namespace kitsu868
