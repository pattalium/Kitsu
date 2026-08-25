#pragma once

#include <cstdint>

constexpr int16_t RADIOLIB_ERR_NONE = 0;
constexpr int16_t RADIOLIB_ERR_UNKNOWN = -1;
constexpr uint8_t RADIOLIB_SX126X_STATUS_MODE_RX = 0x50U;

constexpr uint32_t RADIOLIB_SX126X_IRQ_TX_DONE = 1U << 0U;
constexpr uint32_t RADIOLIB_SX126X_IRQ_RX_DONE = 1U << 1U;
constexpr uint32_t RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED = 1U << 2U;
constexpr uint32_t RADIOLIB_SX126X_IRQ_SYNC_WORD_VALID = 1U << 3U;
constexpr uint32_t RADIOLIB_SX126X_IRQ_HEADER_VALID = 1U << 4U;
constexpr uint32_t RADIOLIB_SX126X_IRQ_HEADER_ERR = 1U << 5U;
constexpr uint32_t RADIOLIB_SX126X_IRQ_CRC_ERR = 1U << 6U;
constexpr uint32_t RADIOLIB_SX126X_IRQ_TIMEOUT = 1U << 9U;

class Module {
 public:
  Module(int16_t chipSelect, int16_t interrupt, int16_t reset,
         int16_t busy)
      : chipSelect_(chipSelect), interrupt_(interrupt), reset_(reset),
        busy_(busy) {}

  int16_t interruptPin() const { return interrupt_; }

 private:
  int16_t chipSelect_;
  int16_t interrupt_;
  int16_t reset_;
  int16_t busy_;
};
