#pragma once

#include <Mesh.h>
#include <RadioLib.h>

#ifdef USE_CC310_HW_CRYPTO
#include <Adafruit_nRFCrypto.h>
#endif
struct PacketMillis {
  uint32_t preambleMillis;  // preamble-detect -> header-valid deadline
  uint32_t payloadMillis;   // header-valid   -> rx-done deadline
};

// Task-context receive observability only. No payload bytes are retained.
// All counters saturate so a long-running receiver cannot wrap into a false
// zero during acceptance testing.
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

class RadioLibWrapper : public mesh::Radio {
protected:
  PhysicalLayer* _radio;
  mesh::MainBoard* _board;
  uint32_t n_recv, n_sent, n_recv_errors;
  int16_t _noise_floor, _threshold;
  bool _cad_enabled;
  uint16_t _num_floor_samples;
  int32_t _floor_sample_sum;
  uint8_t _preamble_sf;
  RadioLibReceiveDiagnostics _receiveDiagnostics{};

  static void incrementSaturating(uint32_t& value) {
    if (value != UINT32_MAX) ++value;
  }

  void idle();
  int16_t startRecvWithStatus();
  void startRecv();
  // Pinned RadioLib 7.7.1-43-g6d893's SX126x::getStatus() asks
  // SPIreadStream() for zero data bytes, so it always returns the
  // zero-initialized local byte. Read the
  // actual GetStatus response without bypassing Module's SPI/HAL ownership.
  // False means no trustworthy status byte is available; output is then 0.
#if !RADIOLIB_EXCLUDE_SX126X
  bool readSx126xStatus(SX126x& radio, uint8_t& output);
#endif
  float packetScoreInt(float snr, int sf, int packet_len);
  virtual bool isReceivingPacket() =0;
  virtual void doResetAGC();

public:
  RadioLibWrapper(PhysicalLayer& radio, mesh::MainBoard& board) : _radio(&radio), _board(&board), _preamble_sf(0) { n_recv = n_sent = n_recv_errors = 0; }

  void begin() override;
  virtual void powerOff() { _radio->sleep(); }
  int recvRaw(uint8_t* bytes, int sz) override;
  uint32_t getEstAirtimeFor(int len_bytes) override;
  bool startSendRaw(const uint8_t* bytes, int len) override;
  bool isSendComplete() override;
  void onSendFinished() override;
  bool isInRecvMode() const override;
  bool isChannelActive();

  bool isReceiving() override {
    if (isReceivingPacket()) return true;

    return isChannelActive();
  }

  virtual void setParams(float freq, float bw, uint8_t sf, uint8_t cr) = 0;
  uint32_t getRngSeed();
  void setTxPower(int8_t dbm);

  virtual float getCurrentRSSI() =0;
  virtual uint8_t getSpreadingFactor() const { return LORA_SF; }
  static uint16_t preambleLengthForSF(uint8_t sf) { return sf <= 8 ? 32 : 16; }
  void updatePreamble(uint8_t sf) { _preamble_sf = sf; _radio->setPreambleLength(preambleLengthForSF(sf)); }
  PacketMillis calcMaxPacketMillis(uint8_t sf, float bw, uint8_t cr, uint8_t preambleSymbols);
  virtual int16_t performChannelScan();

  int getNoiseFloor() const override { return _noise_floor; }
  void triggerNoiseFloorCalibrate(int threshold) override;
  void setCADEnabled(bool enable) override { _cad_enabled = enable; }
  void resetAGC() override;

  void loop() override;

  uint32_t getPacketsRecv() const { return n_recv; }
  uint32_t getPacketsRecvErrors() const { return n_recv_errors; }
  uint32_t getPacketsSent() const { return n_sent; }
  RadioLibReceiveDiagnostics receiveDiagnostics() const {
    return _receiveDiagnostics;
  }
  void resetReceiveDiagnostics() {
    _receiveDiagnostics = RadioLibReceiveDiagnostics{};
  }
  void resetStats() {
    n_recv = n_sent = n_recv_errors = 0;
    resetReceiveDiagnostics();
  }

  virtual float getLastRSSI() const override;
  virtual float getLastSNR() const override;

  float packetScore(float snr, int packet_len) override { return packetScoreInt(snr, 10, packet_len); }  // assume sf=10

  virtual bool setRxBoostedGainMode(bool) { return false; }
  virtual bool getRxBoostedGainMode() const { return false; }
  
  virtual bool configSideDetectors(const uint8_t sideDetSFs[], uint8_t num, float bw) { return false; }
};

/**
 * \brief  an RNG impl using the noise from the LoRa radio as entropy.
 *         NOTE: this is VERY SLOW!  Use only for things like creating new LocalIdentity
*/
class RadioNoiseListener : public mesh::RNG {
  PhysicalLayer* _radio;
public:
  RadioNoiseListener(PhysicalLayer& radio): _radio(&radio) { }

  void random(uint8_t* dest, size_t sz) override {
#ifdef USE_CC310_HW_CRYPTO
    nRFCrypto.Random.generate(dest, (uint16_t)sz);
    for (int i = 0; i < sz; i++) {
      dest[i] ^= _radio->randomByte() ^ (::random(0, 256) & 0xFF); // combine with Radio's entropy
    }
#else
    for (int i = 0; i < sz; i++) {
      dest[i] = _radio->randomByte() ^ (::random(0, 256) & 0xFF);
    }
#endif
  }
};
