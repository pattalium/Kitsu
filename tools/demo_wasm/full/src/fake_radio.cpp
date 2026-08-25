#include "helpers/radiolib/CustomSX1262Wrapper.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Arduino.h"

extern "C" void attachInterrupt(uint8_t pin, void (*handler)(void), int mode);
extern "C" void detachInterrupt(uint8_t pin);
extern "C" void __attachInterrupt(uint8_t, void (*)(void), int) {}
extern "C" void __detachInterrupt(uint8_t) {}

namespace {

constexpr uint8_t kDio1Pin = 14U;
constexpr size_t kFrameBytes = 255U;
constexpr size_t kQueueCapacity = 16U;
constexpr size_t kIoCapacity = 512U;

struct RadioFrame {
  uint8_t bytes[kFrameBytes]{};
  uint16_t length = 0U;
  float rssi = 0.0f;
  float snr = 0.0f;
};

RadioFrame rxQueue[kQueueCapacity]{};
size_t rxRead = 0U;
size_t rxWrite = 0U;
size_t rxCount = 0U;
RadioFrame txQueue[kQueueCapacity]{};
size_t txRead = 0U;
size_t txWrite = 0U;
size_t txCount = 0U;
uint32_t txTotal = 0U;
uint32_t rxDropped = 0U;
uint32_t txDropped = 0U;
uint8_t radioIo[kIoCapacity]{};
bool txDonePending = false;

void noOpIrqHandler() {}

void updateDio() {
  digitalWrite(kDio1Pin, (txDonePending || rxCount != 0U) ? HIGH : LOW);
}

void incrementSaturating(uint32_t& value) {
  if (value != UINT32_MAX) ++value;
}

void recordBit(CustomSX1262IrqBitObservations& output, uint32_t flags) {
  if ((flags & RADIOLIB_SX126X_IRQ_RX_DONE) != 0U) {
    incrementSaturating(output.rxDone);
  }
  if ((flags & RADIOLIB_SX126X_IRQ_CRC_ERR) != 0U) {
    incrementSaturating(output.crcError);
  }
  if ((flags & RADIOLIB_SX126X_IRQ_HEADER_ERR) != 0U) {
    incrementSaturating(output.headerError);
  }
  if ((flags & RADIOLIB_SX126X_IRQ_TIMEOUT) != 0U) {
    incrementSaturating(output.timeout);
  }
  if ((flags & RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED) != 0U) {
    incrementSaturating(output.preamble);
  }
  if ((flags & RADIOLIB_SX126X_IRQ_HEADER_VALID) != 0U) {
    incrementSaturating(output.headerValid);
  }
  if ((flags & RADIOLIB_SX126X_IRQ_SYNC_WORD_VALID) != 0U) {
    incrementSaturating(output.syncWordValid);
  }
}

uint32_t pendingIrqFlags() {
  uint32_t flags = 0U;
  if (txDonePending) flags |= RADIOLIB_SX126X_IRQ_TX_DONE;
  if (rxCount != 0U) flags |= RADIOLIB_SX126X_IRQ_RX_DONE;
  return flags;
}

}  // namespace

int16_t CustomSX1262::begin(float frequency, float bandwidth,
                            uint8_t spreadingFactor, uint8_t codingRate,
                            uint8_t syncWord, int8_t outputPower,
                            uint16_t preambleLength, float tcxoVoltage) {
  frequency_ = frequency;
  bandwidth_ = bandwidth;
  spreadingFactor_ = spreadingFactor;
  codingRate_ = codingRate;
  syncWord_ = syncWord;
  outputPower_ = outputPower;
  preambleLength_ = preambleLength;
  tcxoVoltage_ = tcxoVoltage;
  sleeping_ = false;
  receiving_ = false;
  updateDio();
  return RADIOLIB_ERR_NONE;
}

int16_t CustomSX1262::setCRC(uint8_t) { return RADIOLIB_ERR_NONE; }
int16_t CustomSX1262::setCurrentLimit(float) { return RADIOLIB_ERR_NONE; }
int16_t CustomSX1262::setDio2AsRfSwitch(bool) { return RADIOLIB_ERR_NONE; }
int16_t CustomSX1262::setRxBoostedGainMode(bool) {
  return RADIOLIB_ERR_NONE;
}

int16_t CustomSX1262::sleep(bool) {
  sleeping_ = true;
  receiving_ = false;
  return RADIOLIB_ERR_NONE;
}

int16_t CustomSX1262::standby() {
  sleeping_ = false;
  receiving_ = false;
  return RADIOLIB_ERR_NONE;
}

int16_t CustomSX1262::startReceive() {
  sleeping_ = false;
  receiving_ = true;
  updateDio();
  return RADIOLIB_ERR_NONE;
}

void CustomSX1262::setRadioParams(float frequency, float bandwidth,
                                  uint8_t spreadingFactor,
                                  uint8_t codingRate) {
  frequency_ = frequency;
  bandwidth_ = bandwidth;
  spreadingFactor_ = spreadingFactor;
  codingRate_ = codingRate;
}

uint32_t CustomSX1262::observeIrqFlags(bool dioAsserted, bool lowRate) {
  const uint32_t flags = pendingIrqFlags();
  incrementSaturating(irqDiagnostics_.samples);
  irqDiagnostics_.lastFlags = static_cast<uint16_t>(flags);
  if (dioAsserted) {
    incrementSaturating(irqDiagnostics_.dioAssertedSamples);
    irqDiagnostics_.lastDioAssertedFlags = static_cast<uint16_t>(flags);
    recordBit(irqDiagnostics_.dio, flags);
  }
  if (lowRate) {
    incrementSaturating(irqDiagnostics_.lowRateSamples);
    irqDiagnostics_.lastLowRateFlags = static_cast<uint16_t>(flags);
    recordBit(irqDiagnostics_.lowRate, flags);
  }
  if ((flags & RADIOLIB_SX126X_IRQ_RX_DONE) != 0U) {
    incrementSaturating(irqDiagnostics_.rxDoneObservations);
  }
  if ((flags & RADIOLIB_SX126X_IRQ_CRC_ERR) != 0U) {
    incrementSaturating(irqDiagnostics_.crcErrorObservations);
  }
  if ((flags & RADIOLIB_SX126X_IRQ_HEADER_ERR) != 0U) {
    incrementSaturating(irqDiagnostics_.headerErrorObservations);
  }
  if ((flags & RADIOLIB_SX126X_IRQ_TIMEOUT) != 0U) {
    incrementSaturating(irqDiagnostics_.timeoutObservations);
  }
  if ((flags & RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED) != 0U) {
    incrementSaturating(irqDiagnostics_.preambleObservations);
  }
  if ((flags & RADIOLIB_SX126X_IRQ_HEADER_VALID) != 0U) {
    incrementSaturating(irqDiagnostics_.headerValidObservations);
  }
  if ((flags & RADIOLIB_SX126X_IRQ_SYNC_WORD_VALID) != 0U) {
    incrementSaturating(irqDiagnostics_.syncWordValidObservations);
  }
  return flags;
}

CustomSX1262IrqDiagnostics CustomSX1262::irqDiagnostics() const {
  return irqDiagnostics_;
}

void CustomSX1262::clearIrqDiagnostics() {
  irqDiagnostics_ = CustomSX1262IrqDiagnostics{};
}

CustomSX1262Wrapper::CustomSX1262Wrapper(CustomSX1262& radio,
                                         mesh::MainBoard& board)
    : radio_(&radio), board_(&board) {}

void CustomSX1262Wrapper::begin() {
  receiving_ = false;
  txDone_ = false;
  txDonePending = false;
  if (radio_ && radio_->module()) {
    attachInterrupt(static_cast<uint8_t>(radio_->module()->interruptPin()),
                    noOpIrqHandler, RISING);
  }
  (void)startRecvWithStatus();
}

void CustomSX1262Wrapper::loop() {}

int CustomSX1262Wrapper::recvRaw(uint8_t* bytes, int capacity) {
  incrementSaturating(receiveDiagnostics_.recvRawAttempts);
  if (rxCount == 0U) {
    if (!receiving_) (void)startRecvWithStatus();
    updateDio();
    return 0;
  }
  incrementSaturating(receiveDiagnostics_.interruptReadyAttempts);
  incrementSaturating(receiveDiagnostics_.packetLengthSamples);
  const RadioFrame& frame = rxQueue[rxRead];
  receiveDiagnostics_.lastPacketLengthAvailable = true;
  receiveDiagnostics_.lastPacketLength = frame.length;
  if (!bytes || capacity <= 0) {
    incrementSaturating(receiveDiagnostics_.readDataErrors);
    receiveDiagnostics_.lastReadDataErrorAvailable = true;
    receiveDiagnostics_.lastReadDataError = RADIOLIB_ERR_UNKNOWN;
    return 0;
  }
  incrementSaturating(receiveDiagnostics_.readDataAttempts);
  const int copying = std::min<int>(capacity, frame.length);
  std::memcpy(bytes, frame.bytes, static_cast<size_t>(copying));
  radio_->setLastSignal(frame.rssi, frame.snr);
  rxQueue[rxRead] = RadioFrame{};
  rxRead = (rxRead + 1U) % kQueueCapacity;
  --rxCount;
  incrementSaturating(receiveDiagnostics_.successfulReads);
  receiving_ = true;
  updateDio();
  return copying;
}

uint32_t CustomSX1262Wrapper::getEstAirtimeFor(int bytes) {
  if (!radio_ || bytes <= 0) return 0U;
  const int sf = radio_->spreadingFactor();
  const double bandwidthHz = radio_->bandwidth() * 1000.0;
  if (sf < 5 || sf > 12 || bandwidthHz <= 0.0) return 0U;
  const bool lowDataRate = sf >= 11 && radio_->bandwidth() <= 125.0f;
  const double denominator = 4.0 * (sf - (lowDataRate ? 2 : 0));
  const double numerator = 8.0 * bytes - 4.0 * sf + 28.0 + 16.0;
  const double coded = std::max(0.0, std::ceil(numerator / denominator)) *
      radio_->codingRate();
  const double payloadSymbols = 8.0 + coded;
  const double symbolSeconds = static_cast<double>(1U << sf) / bandwidthHz;
  const double totalSeconds =
      (radio_->preambleLength() + 4.25 + payloadSymbols) * symbolSeconds;
  return static_cast<uint32_t>(std::ceil(totalSeconds * 1000.0));
}

float CustomSX1262Wrapper::packetScore(float snr, int packetLength) {
  static constexpr float thresholds[] = {-7.5f, -10.0f, -12.5f,
                                         -15.0f, -17.5f, -20.0f};
  const int sf = radio_ ? radio_->spreadingFactor() : 10;
  if (sf < 7 || sf > 12 || snr < thresholds[sf - 7]) return 0.0f;
  const float snrSuccess = (snr - thresholds[sf - 7]) / 10.0f;
  const float collisionPenalty = 1.0f - packetLength / 256.0f;
  return std::max(0.0f, std::min(1.0f, snrSuccess * collisionPenalty));
}

bool CustomSX1262Wrapper::startSendRaw(const uint8_t* bytes, int length) {
  if (!bytes || length <= 0 || length > static_cast<int>(kFrameBytes) ||
      txCount >= kQueueCapacity || txDonePending) {
    incrementSaturating(txDropped);
    return false;
  }
  if (board_) board_->onBeforeTransmit();
  RadioFrame& frame = txQueue[txWrite];
  frame = RadioFrame{};
  frame.length = static_cast<uint16_t>(length);
  std::memcpy(frame.bytes, bytes, static_cast<size_t>(length));
  txWrite = (txWrite + 1U) % kQueueCapacity;
  ++txCount;
  incrementSaturating(txTotal);
  receiving_ = false;
  const uint32_t airtime = std::max<uint32_t>(1U, getEstAirtimeFor(length));
  kitsu_hal_advance_millis(airtime);
  txDone_ = true;
  txDonePending = true;
  updateDio();
  return true;
}

bool CustomSX1262Wrapper::isSendComplete() {
  if (!txDone_) return false;
  txDone_ = false;
  txDonePending = false;
  updateDio();
  return true;
}

void CustomSX1262Wrapper::onSendFinished() {
  if (board_) board_->onAfterTransmit();
  receiving_ = false;
}

bool CustomSX1262Wrapper::isInRecvMode() const { return receiving_; }
bool CustomSX1262Wrapper::isReceiving() { return false; }
float CustomSX1262Wrapper::getLastRSSI() const {
  return radio_ ? radio_->lastRssi() : 0.0f;
}
float CustomSX1262Wrapper::getLastSNR() const {
  return radio_ ? radio_->lastSnr() : 0.0f;
}

void CustomSX1262Wrapper::setParams(float frequency, float bandwidth,
                                    uint8_t spreadingFactor,
                                    uint8_t codingRate) {
  if (radio_) {
    radio_->setRadioParams(frequency, bandwidth, spreadingFactor, codingRate);
  }
}

void CustomSX1262Wrapper::setTxPower(int8_t dbm) {
  if (radio_) radio_->setOutputPower(dbm);
}

RadioLibReceiveDiagnostics CustomSX1262Wrapper::receiveDiagnostics() const {
  return receiveDiagnostics_;
}

void CustomSX1262Wrapper::resetReceiveDiagnostics() {
  receiveDiagnostics_ = RadioLibReceiveDiagnostics{};
}

int16_t CustomSX1262Wrapper::startRecvWithStatus() {
  if (!radio_) {
    receiving_ = false;
    return RADIOLIB_ERR_UNKNOWN;
  }
  const int16_t result = radio_->startReceive();
  receiving_ = result == RADIOLIB_ERR_NONE;
  return result;
}

bool CustomSX1262Wrapper::readSx126xStatus(CustomSX1262&,
                                           uint8_t& output) {
  output = receiving_ ? RADIOLIB_SX126X_STATUS_MODE_RX : 0x20U;
  return true;
}

extern "C" uint8_t* kitsu_emulator_radio_io_buffer() { return radioIo; }
extern "C" uint32_t kitsu_emulator_radio_io_capacity() {
  return sizeof(radioIo);
}
extern "C" uint32_t kitsu_emulator_radio_inject_rx(
    uint32_t bytes, int32_t rssiX100, int32_t snrX100) {
  if (bytes == 0U || bytes > kFrameBytes || bytes > sizeof(radioIo) ||
      rxCount >= kQueueCapacity) {
    incrementSaturating(rxDropped);
    return 0U;
  }
  RadioFrame& frame = rxQueue[rxWrite];
  frame = RadioFrame{};
  frame.length = static_cast<uint16_t>(bytes);
  frame.rssi = static_cast<float>(rssiX100) / 100.0f;
  frame.snr = static_cast<float>(snrX100) / 100.0f;
  std::memcpy(frame.bytes, radioIo, bytes);
  rxWrite = (rxWrite + 1U) % kQueueCapacity;
  ++rxCount;
  updateDio();
  return 1U;
}
extern "C" uint32_t kitsu_emulator_radio_tx_count() {
  return static_cast<uint32_t>(txCount);
}
extern "C" uint32_t kitsu_emulator_radio_tx_total() { return txTotal; }
extern "C" uint32_t kitsu_emulator_radio_tx_peek_length() {
  return txCount == 0U ? 0U : txQueue[txRead].length;
}
extern "C" uint32_t kitsu_emulator_radio_tx_copy() {
  if (txCount == 0U) return 0U;
  const RadioFrame& frame = txQueue[txRead];
  std::memcpy(radioIo, frame.bytes, frame.length);
  return frame.length;
}
extern "C" uint32_t kitsu_emulator_radio_tx_consume() {
  if (txCount == 0U) return 0U;
  txQueue[txRead] = RadioFrame{};
  txRead = (txRead + 1U) % kQueueCapacity;
  --txCount;
  return 1U;
}
extern "C" uint32_t kitsu_emulator_radio_rx_queued() {
  return static_cast<uint32_t>(rxCount);
}
extern "C" uint32_t kitsu_emulator_radio_rx_dropped() { return rxDropped; }
extern "C" uint32_t kitsu_emulator_radio_tx_dropped() { return txDropped; }
