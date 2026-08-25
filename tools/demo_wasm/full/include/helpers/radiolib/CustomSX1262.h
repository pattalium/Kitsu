#pragma once

#include <cstdint>

#include "Arduino.h"
#include "RadioLib.h"

struct CustomSX1262IrqBitObservations {
  uint32_t rxDone = 0U;
  uint32_t crcError = 0U;
  uint32_t headerError = 0U;
  uint32_t timeout = 0U;
  uint32_t preamble = 0U;
  uint32_t headerValid = 0U;
  uint32_t syncWordValid = 0U;
};

struct CustomSX1262IrqDiagnostics {
  uint32_t samples = 0U;
  uint32_t dioAssertedSamples = 0U;
  uint32_t lowRateSamples = 0U;
  uint16_t lastFlags = 0U;
  uint16_t lastDioAssertedFlags = 0U;
  uint16_t lastLowRateFlags = 0U;
  uint32_t rxDoneObservations = 0U;
  uint32_t crcErrorObservations = 0U;
  uint32_t headerErrorObservations = 0U;
  uint32_t timeoutObservations = 0U;
  uint32_t preambleObservations = 0U;
  uint32_t headerValidObservations = 0U;
  uint32_t syncWordValidObservations = 0U;
  CustomSX1262IrqBitObservations dio{};
  CustomSX1262IrqBitObservations lowRate{};
};

class CustomSX1262 {
 public:
  explicit CustomSX1262(Module* module) : module_(module) {}

  int16_t begin(float frequency, float bandwidth, uint8_t spreadingFactor,
                uint8_t codingRate, uint8_t syncWord, int8_t outputPower,
                uint16_t preambleLength, float tcxoVoltage);
  int16_t setCRC(uint8_t enabled);
  int16_t setCurrentLimit(float milliamps);
  int16_t setDio2AsRfSwitch(bool enabled);
  int16_t setRxBoostedGainMode(bool enabled);
  int16_t sleep(bool retainConfiguration = true);
  int16_t standby();
  int16_t startReceive();

  uint32_t observeIrqFlags(bool dioAsserted = false,
                           bool lowRate = false);
  CustomSX1262IrqDiagnostics irqDiagnostics() const;
  void clearIrqDiagnostics();

  Module* module() const { return module_; }
  uint8_t spreadingFactor() const { return spreadingFactor_; }
  float bandwidth() const { return bandwidth_; }
  uint8_t codingRate() const { return codingRate_; }
  uint16_t preambleLength() const { return preambleLength_; }
  void setRadioParams(float frequency, float bandwidth,
                      uint8_t spreadingFactor, uint8_t codingRate);
  void setOutputPower(int8_t value) { outputPower_ = value; }
  void setLastSignal(float rssi, float snr) {
    lastRssi_ = rssi;
    lastSnr_ = snr;
  }
  float lastRssi() const { return lastRssi_; }
  float lastSnr() const { return lastSnr_; }

 private:
  Module* module_ = nullptr;
  float frequency_ = 0.0f;
  float bandwidth_ = 125.0f;
  uint8_t spreadingFactor_ = 7U;
  uint8_t codingRate_ = 5U;
  uint8_t syncWord_ = 0U;
  int8_t outputPower_ = 0;
  uint16_t preambleLength_ = 16U;
  float tcxoVoltage_ = 0.0f;
  float lastRssi_ = 0.0f;
  float lastSnr_ = 0.0f;
  bool receiving_ = false;
  bool sleeping_ = false;
  CustomSX1262IrqDiagnostics irqDiagnostics_{};
};
