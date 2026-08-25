#pragma once

#include <cstddef>
#include <cstdint>

#include "Dispatcher.h"
#include "MeshCore.h"
#include "helpers/radiolib/CustomSX1262.h"

struct RadioLibReceiveDiagnostics {
  uint32_t recvRawAttempts = 0U;
  uint32_t interruptReadyAttempts = 0U;
  uint32_t packetLengthSamples = 0U;
  uint32_t packetLengthZero = 0U;
  bool lastPacketLengthAvailable = false;
  uint16_t lastPacketLength = 0U;
  uint32_t readDataAttempts = 0U;
  uint32_t successfulReads = 0U;
  uint32_t readDataErrors = 0U;
  bool lastReadDataErrorAvailable = false;
  int16_t lastReadDataError = 0;
  uint32_t rxRestartAttempts = 0U;
  uint32_t rxRestartSuccesses = 0U;
  uint32_t rxRestartErrors = 0U;
  bool lastRxRestartResultAvailable = false;
  int16_t lastRxRestartResult = 0;
  bool lastRxRestartErrorAvailable = false;
  int16_t lastRxRestartError = 0;
};

class CustomSX1262Wrapper : public mesh::Radio {
 public:
  CustomSX1262Wrapper(CustomSX1262& radio, mesh::MainBoard& board);

  void begin() override;
  void loop() override;
  int recvRaw(uint8_t* bytes, int capacity) override;
  uint32_t getEstAirtimeFor(int bytes) override;
  float packetScore(float snr, int packetLength) override;
  bool startSendRaw(const uint8_t* bytes, int length) override;
  bool isSendComplete() override;
  void onSendFinished() override;
  bool isInRecvMode() const override;
  bool isReceiving() override;
  float getLastRSSI() const override;
  float getLastSNR() const override;

  void setParams(float frequency, float bandwidth, uint8_t spreadingFactor,
                 uint8_t codingRate);
  void setTxPower(int8_t dbm);
  RadioLibReceiveDiagnostics receiveDiagnostics() const;
  void resetReceiveDiagnostics();

 protected:
  int16_t startRecvWithStatus();
  bool readSx126xStatus(CustomSX1262& radio, uint8_t& output);

 private:
  CustomSX1262* radio_ = nullptr;
  mesh::MainBoard* board_ = nullptr;
  bool receiving_ = false;
  bool txDone_ = false;
  RadioLibReceiveDiagnostics receiveDiagnostics_{};
};
