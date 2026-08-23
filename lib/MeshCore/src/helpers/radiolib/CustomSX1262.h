#pragma once

#include <RadioLib.h>
#include "MeshCore.h"

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

class CustomSX1262 : public SX1262 {
  uint32_t _preambleMillis = 66;
  uint32_t _maxPayloadMillis = 3934;
  uint32_t _activityAt = 0;
  bool _headerSeen = false;
  CustomSX1262IrqDiagnostics _irqDiagnostics{};

  static void incrementSaturating(uint32_t& value) {
    if (value != UINT32_MAX) ++value;
  }

  static void recordBitObservations(
      CustomSX1262IrqBitObservations& output, uint32_t irq) {
    if ((irq & RADIOLIB_SX126X_IRQ_RX_DONE) != 0U) {
      incrementSaturating(output.rxDone);
    }
    if ((irq & RADIOLIB_SX126X_IRQ_CRC_ERR) != 0U) {
      incrementSaturating(output.crcError);
    }
    if ((irq & RADIOLIB_SX126X_IRQ_HEADER_ERR) != 0U) {
      incrementSaturating(output.headerError);
    }
    if ((irq & RADIOLIB_SX126X_IRQ_TIMEOUT) != 0U) {
      incrementSaturating(output.timeout);
    }
    if ((irq & RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED) != 0U) {
      incrementSaturating(output.preamble);
    }
    if ((irq & RADIOLIB_SX126X_IRQ_HEADER_VALID) != 0U) {
      incrementSaturating(output.headerValid);
    }
    if ((irq & RADIOLIB_SX126X_IRQ_SYNC_WORD_VALID) != 0U) {
      incrementSaturating(output.syncWordValid);
    }
  }

  void recordIrqFlags(uint32_t irq, bool dioAsserted, bool lowRate) {
    incrementSaturating(_irqDiagnostics.samples);
    if (dioAsserted) {
      incrementSaturating(_irqDiagnostics.dioAssertedSamples);
      _irqDiagnostics.lastDioAssertedFlags = static_cast<uint16_t>(irq);
      recordBitObservations(_irqDiagnostics.dio, irq);
    }
    if (lowRate) {
      incrementSaturating(_irqDiagnostics.lowRateSamples);
      _irqDiagnostics.lastLowRateFlags = static_cast<uint16_t>(irq);
      recordBitObservations(_irqDiagnostics.lowRate, irq);
    }
    _irqDiagnostics.lastFlags = static_cast<uint16_t>(irq);
    if ((irq & RADIOLIB_SX126X_IRQ_RX_DONE) != 0U) {
      incrementSaturating(_irqDiagnostics.rxDoneObservations);
    }
    if ((irq & RADIOLIB_SX126X_IRQ_CRC_ERR) != 0U) {
      incrementSaturating(_irqDiagnostics.crcErrorObservations);
    }
    if ((irq & RADIOLIB_SX126X_IRQ_HEADER_ERR) != 0U) {
      incrementSaturating(_irqDiagnostics.headerErrorObservations);
    }
    if ((irq & RADIOLIB_SX126X_IRQ_TIMEOUT) != 0U) {
      incrementSaturating(_irqDiagnostics.timeoutObservations);
    }
    if ((irq & RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED) != 0U) {
      incrementSaturating(_irqDiagnostics.preambleObservations);
    }
    if ((irq & RADIOLIB_SX126X_IRQ_HEADER_VALID) != 0U) {
      incrementSaturating(_irqDiagnostics.headerValidObservations);
    }
    if ((irq & RADIOLIB_SX126X_IRQ_SYNC_WORD_VALID) != 0U) {
      incrementSaturating(_irqDiagnostics.syncWordValidObservations);
    }
  }

  public:
    CustomSX1262(Module *mod) : SX1262(mod) { }

  #ifdef RP2040_PLATFORM
    bool std_init(SPIClassRP2040* spi = NULL)
  #else
    bool std_init(SPIClass* spi = NULL)
  #endif
    {
  #ifdef SX126X_DIO3_TCXO_VOLTAGE
      float tcxo = SX126X_DIO3_TCXO_VOLTAGE;
  #else
      float tcxo = 1.6f;
  #endif

  #ifdef LORA_CR
      uint8_t cr = LORA_CR;
  #else
      uint8_t cr = 5;
  #endif

  #ifdef SX126X_USE_REGULATOR_LDO
      constexpr bool useRegulatorLDO = SX126X_USE_REGULATOR_LDO;
  #else
      constexpr bool useRegulatorLDO = false;
  #endif

      MESH_DEBUG_PRINTLN("SX1262 regulator requested: %s", useRegulatorLDO ? "LDO" : "DC-DC");

  #if defined(P_LORA_SCLK)
    #ifdef NRF52_PLATFORM
      if (spi) { spi->setPins(P_LORA_MISO, P_LORA_SCLK, P_LORA_MOSI); spi->begin(); }
    #elif defined(RP2040_PLATFORM)
      if (spi) {
        spi->setMISO(P_LORA_MISO);
        //spi->setCS(P_LORA_NSS); // Setting CS results in freeze
        spi->setSCK(P_LORA_SCLK);
        spi->setMOSI(P_LORA_MOSI);
        spi->begin();
      }
    #else
      if (spi) spi->begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI);
    #endif
  #endif
      int status = begin(LORA_FREQ, LORA_BW, LORA_SF, cr, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, LORA_TX_POWER, 16, tcxo, useRegulatorLDO);
      // if radio init fails with -707/-706, try again with tcxo voltage set to 0.0f
      if (status == RADIOLIB_ERR_SPI_CMD_FAILED || status == RADIOLIB_ERR_SPI_CMD_INVALID) {
        MESH_DEBUG_PRINTLN("SX1262 init failed with error %d, retrying with TCXO at 0.0V", status);
        tcxo = 0.0f;
        status = begin(LORA_FREQ, LORA_BW, LORA_SF, cr, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, LORA_TX_POWER, 16, tcxo, useRegulatorLDO);
      }
      if (status != RADIOLIB_ERR_NONE) {
        Serial.print("ERROR: radio init failed: ");
        Serial.println(status);
        return false;  // fail
      }
    
      setCRC(1);
  
  #ifdef SX126X_CURRENT_LIMIT
      setCurrentLimit(SX126X_CURRENT_LIMIT);
  #endif
  #ifdef SX126X_DIO2_AS_RF_SWITCH
      setDio2AsRfSwitch(SX126X_DIO2_AS_RF_SWITCH);
  #endif
  #ifdef SX126X_RX_BOOSTED_GAIN
      setRxBoostedGainMode(SX126X_RX_BOOSTED_GAIN);
  #endif
  #if defined(SX126X_RXEN) || defined(SX126X_TXEN)
    #ifndef SX126X_RXEN
      #define SX126X_RXEN RADIOLIB_NC
    #endif
    #ifndef SX126X_TXEN
      #define SX126X_TXEN RADIOLIB_NC
    #endif
      setRfSwitchPins(SX126X_RXEN, SX126X_TXEN);
  #endif 

  // for improved RX with Heltec v4
  #ifdef SX126X_REGISTER_PATCH
    uint8_t r_data = 0;
    readRegister(0x8B5, &r_data, 1);
    r_data |= 0x01;
    writeRegister(0x8B5, &r_data, 1);
  #endif

      MESH_DEBUG_PRINTLN("SX1262 status=0x%02X device_errors=0x%04X", getStatus(), getDeviceErrors());

      return true;  // success
    }

    int16_t startReceive() override {
      // include the PREAMBLE_DETECTED irq bit in reported flags
      return SX1262::startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF, RADIOLIB_IRQ_RX_DEFAULT_FLAGS | (1UL << RADIOLIB_IRQ_PREAMBLE_DETECTED), RADIOLIB_IRQ_RX_DEFAULT_MASK, 0);
    }

    // getIrqFlags() issues SX1262 GetIrqStatus (0x12) and does not clear any
    // radio IRQ. The normal receive-activity path already performs this read;
    // recording it adds no SPI traffic. Kitsu additionally calls this once
    // when task-context DIO1 polling observes an asserted completion line.
    uint32_t observeIrqFlags(bool dioAsserted = false,
                             bool lowRate = false) {
      const uint32_t irq = getIrqFlags();
      recordIrqFlags(irq, dioAsserted, lowRate);
      return irq;
    }

    CustomSX1262IrqDiagnostics irqDiagnostics() const {
      return _irqDiagnostics;
    }

    void clearIrqDiagnostics() {
      _irqDiagnostics = CustomSX1262IrqDiagnostics{};
    }

    bool isReceiving() {
      uint32_t irq = observeIrqFlags();
      bool preamble = irq & RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED; // bit 2
      bool header   = irq & RADIOLIB_SX126X_IRQ_HEADER_VALID;      // bit 4
      bool hdrErr   = irq & RADIOLIB_SX126X_IRQ_HEADER_ERR;        // bit 5
      uint32_t now  = millis();
      if (hdrErr) {
        clearIrqFlags(RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED | RADIOLIB_SX126X_IRQ_HEADER_VALID | RADIOLIB_SX126X_IRQ_HEADER_ERR | RADIOLIB_SX126X_IRQ_SYNC_WORD_VALID);
        _activityAt = 0;
        _headerSeen = false;
        return false;
      }
      if (!header && _headerSeen) {
        // something cleared the header flag, reset our state.
        _activityAt = 0; _headerSeen = false;
        return false;
      }

      if (header) {
        if (!_headerSeen) { _headerSeen = true; _activityAt = now; };
        if (now - _activityAt > _maxPayloadMillis) {
          MESH_DEBUG_PRINTLN("Clearing header IRQ after %ums", _maxPayloadMillis);
          clearIrqFlags(RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED | RADIOLIB_SX126X_IRQ_HEADER_VALID | RADIOLIB_SX126X_IRQ_HEADER_ERR | RADIOLIB_SX126X_IRQ_SYNC_WORD_VALID);
          _activityAt = 0; _headerSeen = false;
          return false;
        }
        return true;
      }
      if (preamble) {
        if (_activityAt == 0) _activityAt = now;
        if (now - _activityAt > _preambleMillis) {
          clearIrqFlags(RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED);
          _activityAt = 0;
          MESH_DEBUG_PRINTLN("Clearing preamble IRQ after %ums", _preambleMillis);

          return false;
        }
        return true;
      }
      _activityAt = 0; _headerSeen = false;
      return false;
    }

    void setPreambleMillis(uint32_t preambleMillis) {
      _preambleMillis = preambleMillis;
      MESH_DEBUG_PRINTLN("Set _preambleMillis=%u", _preambleMillis);
    }
    void setMaxPayloadMillis(uint32_t payloadMillis) {
      _maxPayloadMillis = payloadMillis;
      MESH_DEBUG_PRINTLN("Set _maxPayloadMillis=%u", _maxPayloadMillis);
    }

    bool getRxBoostedGainMode() {
      uint8_t rxGain = 0;
      readRegister(RADIOLIB_SX126X_REG_RX_GAIN, &rxGain, 1);
      return (rxGain == RADIOLIB_SX126X_RX_GAIN_BOOSTED);
    }
};
