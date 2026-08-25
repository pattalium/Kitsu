#include "kitsu_mesh_transport.h"
#include "kitsu_nearby_protocol.h"
#include "kitsu_advert_repeat_tracker.h"
#include "kitsu_channel_repeat_tracker.h"
#include "kitsu_endpoint_rx_policy.h"
#include "kitsu_radio_irq_poll.h"
#include "kitsu_repeat_wire.h"
#include "kitsu_rx_rearm_policy.h"
#include "kitsu_transport_scope.h"
#include "kitsu_tx_turnaround.h"

#include <Arduino.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <SPI.h>
#include <esp_system.h>
#include <nvs.h>

#include <Identity.h>
#include <Mesh.h>
#include <Packet.h>
#include <Utils.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/radiolib/CustomSX1262.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>

#include <string.h>

namespace {

kitsu868::mesh::LatchedRadioIrqPoll kitsuRadioIrqPoll;

bool pollKitsuRadioDio1(bool* assertedOutput = nullptr) {
  const bool asserted =
      digitalRead(kitsu868::mesh::kKitsuRadioDio1Pin) == HIGH;
  if (assertedOutput) *assertedOutput = asserted;
  return kitsuRadioIrqPoll.poll(asserted);
}

bool kitsuRadioDio1Claimed() {
  return kitsuRadioIrqPoll.claimed();
}

}  // namespace

// Arduino-ESP32 2.0.17 implements attachInterrupt() as a weak symbol, so this
// board-specific definition can intercept only the SX1262 DIO1 registration.
// All other GPIO users retain the framework implementation.  Do not call the
// framework delegate for DIO1: its gpio_isr_register() performs a blocking IPC
// allocation on the undersized 1,024-byte ipc1 stack on ESP32-S3.
extern "C" void __attachInterrupt(uint8_t pin, void (*handler)(void),
                                   int mode);
extern "C" void __detachInterrupt(uint8_t pin);

extern "C" void attachInterrupt(uint8_t pin, void (*handler)(void), int mode) {
  if (pin == kitsu868::mesh::kKitsuRadioDio1Pin) {
    (void)kitsuRadioIrqPoll.claim(pin, handler, mode);
    return;
  }
  __attachInterrupt(pin, handler, mode);
}

extern "C" void detachInterrupt(uint8_t pin) {
  if (kitsuRadioIrqPoll.release(pin)) return;
  __detachInterrupt(pin);
}

namespace kitsu868 {
namespace mesh {
namespace {

constexpr uint8_t kLoraCs = 8;
constexpr uint8_t kLoraSck = 9;
constexpr uint8_t kLoraMosi = 10;
constexpr uint8_t kLoraMiso = 11;
constexpr uint8_t kLoraReset = 12;
constexpr uint8_t kLoraBusy = 13;
constexpr uint8_t kLoraDio1 = kKitsuRadioDio1Pin;
constexpr size_t kPacketPoolSize = 10;
constexpr size_t kAdvertQueueSize = 8;
constexpr size_t kNearbyRadioQueueSize = 8;
constexpr float kTcxoVoltage = 1.8f;
constexpr float kConservativeAirtimeFactor = 99.0f;  // 1% long-term TX.

static_assert(MAX_HASH_SIZE == kChannelRepeatHashBytes,
              "MeshCore packet-hash width changed");
static_assert(kSx126xChipModeRx == RADIOLIB_SX126X_STATUS_MODE_RX,
              "SX126x RX status mode changed");
static_assert(PAYLOAD_TYPE_GRP_TXT == kChannelGroupTextPayloadType,
              "MeshCore group-text payload type changed");
static_assert(MAX_HASH_SIZE == kAdvertRepeatHashBytes,
              "MeshCore advert packet-hash width changed");
static_assert(kAdvertRepeatHashBytes == kChannelRepeatHashBytes &&
                  kAdvertRepeatDigestBytes == kChannelRepeatDigestBytes,
              "shared RF correlation buffer widths changed");
static_assert(PAYLOAD_TYPE_ADVERT == kAdvertPayloadType,
              "MeshCore advert payload type changed");
static_assert(PH_ROUTE_MASK == kRepeatWireRouteMask &&
                  ROUTE_TYPE_TRANSPORT_FLOOD ==
                      kRepeatWireRouteTransportFlood &&
                  ROUTE_TYPE_FLOOD == kRepeatWireRouteFlood &&
                  ROUTE_TYPE_DIRECT == kRepeatWireRouteDirect &&
                  ROUTE_TYPE_TRANSPORT_DIRECT ==
                      kRepeatWireRouteTransportDirect,
              "MeshCore route encoding changed");
static_assert(PH_TYPE_SHIFT == kRepeatWireTypeShift &&
                  PH_TYPE_MASK == kRepeatWireTypeMask &&
                  PH_VER_SHIFT == kRepeatWireVersionShift &&
                  PH_VER_MASK == kRepeatWireVersionMask &&
                  PAYLOAD_VER_1 == kRepeatWirePayloadVersion1,
              "MeshCore header encoding changed");
static_assert(MAX_PATH_SIZE == kRepeatWireMaximumPathBytes,
              "MeshCore maximum path size changed");

constexpr uint32_t kMinimumEpoch = 1704067200UL;  // 2024-01-01 UTC.
constexpr uint32_t kMaximumEpoch = 4102444800UL;  // 2100-01-01 UTC.

constexpr char kIdentityNamespace[] = "kitsu_mcid";
constexpr char kIdentityKey[] = "identity1";
constexpr uint8_t kIdentityMagic[4] = {'K', 'M', 'I', '1'};
constexpr uint16_t kIdentitySchema = 1;
constexpr size_t kIdentityBytes = 96;
constexpr size_t kIdentityRecordBytes = 108;
constexpr size_t kIdentityOffsetData = 8;
constexpr size_t kIdentityOffsetCrc = 104;

uint16_t readLe16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) |
      static_cast<uint16_t>(bytes[1]) << 8U;
}

uint32_t readLe32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) |
      static_cast<uint32_t>(bytes[1]) << 8U |
      static_cast<uint32_t>(bytes[2]) << 16U |
      static_cast<uint32_t>(bytes[3]) << 24U;
}

void writeLe16(uint8_t* bytes, uint16_t value) {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8U);
}

void writeLe32(uint8_t* bytes, uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8U);
  bytes[2] = static_cast<uint8_t>(value >> 16U);
  bytes[3] = static_cast<uint8_t>(value >> 24U);
}

uint32_t crc32WithZeroedIdentityCrc(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xffffffffUL;
  for (size_t index = 0; index < length; ++index) {
    const uint8_t value =
        index >= kIdentityOffsetCrc && index < kIdentityOffsetCrc + 4U
            ? 0
            : bytes[index];
    crc ^= value;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^
          (0xedb88320UL & (0U - static_cast<uint32_t>(crc & 1U)));
    }
  }
  return ~crc;
}

size_t sanitizeUtf8Name(char* destination, size_t capacity,
                        const uint8_t* source, size_t sourceBytes) {
  if (!destination || capacity == 0) return 0;
  size_t input = 0;
  size_t output = 0;
  while (input < sourceBytes && source[input] != 0) {
    const uint8_t first = source[input];
    size_t sequenceBytes = 1;
    uint32_t codePoint = first;
    bool valid = true;

    if (first < 0x80U) {
      valid = first >= 0x20U && first != 0x7fU;
    } else if (first >= 0xc2U && first <= 0xdfU) {
      sequenceBytes = 2;
    } else if (first >= 0xe0U && first <= 0xefU) {
      sequenceBytes = 3;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      sequenceBytes = 4;
    } else {
      valid = false;
    }

    if (valid && sequenceBytes > 1U) {
      if (input + sequenceBytes > sourceBytes) {
        valid = false;
      } else {
        for (size_t index = 1; index < sequenceBytes; ++index) {
          if ((source[input + index] & 0xc0U) != 0x80U) {
            valid = false;
            break;
          }
        }
      }
      if (valid) {
        if (sequenceBytes == 2U) {
          codePoint = ((first & 0x1fU) << 6U) |
              (source[input + 1] & 0x3fU);
        } else if (sequenceBytes == 3U) {
          codePoint = ((first & 0x0fU) << 12U) |
              ((source[input + 1] & 0x3fU) << 6U) |
              (source[input + 2] & 0x3fU);
          if (codePoint < 0x800U ||
              (codePoint >= 0xd800U && codePoint <= 0xdfffU)) {
            valid = false;
          }
        } else {
          codePoint = ((first & 0x07U) << 18U) |
              ((source[input + 1] & 0x3fU) << 12U) |
              ((source[input + 2] & 0x3fU) << 6U) |
              (source[input + 3] & 0x3fU);
          if (codePoint < 0x10000U || codePoint > 0x10ffffU) valid = false;
        }
        // C1 controls are legal Unicode but unsafe in terminal-facing names.
        if (codePoint >= 0x80U && codePoint <= 0x9fU) valid = false;
      }
    }

    if (!valid) {
      if (output + 1U >= capacity) break;
      destination[output++] = '?';
      ++input;
      continue;
    }
    if (output + sequenceBytes >= capacity) break;
    memcpy(destination + output, source + input, sequenceBytes);
    output += sequenceBytes;
    input += sequenceBytes;
  }
  destination[output] = '\0';
  return output;
}

class EspRng final : public ::mesh::RNG {
 public:
  void random(uint8_t* destination, size_t bytes) override {
    esp_fill_random(destination, bytes);
  }
};

class SessionRtc final : public ::mesh::RTCClock {
 public:
  uint32_t getCurrentTime() override {
    if (!valid_) return 0;
    return baseEpoch_ + (millis() - setAtMillis_) / 1000UL;
  }

  void setCurrentTime(uint32_t epoch) override {
    baseEpoch_ = epoch;
    setAtMillis_ = millis();
    valid_ = true;
  }

  bool valid() const { return valid_; }

 private:
  bool valid_ = false;
  uint32_t baseEpoch_ = 0;
  uint32_t setAtMillis_ = 0;
};

class KitsuBoard final : public ::mesh::MainBoard {
 public:
  uint16_t getBattMilliVolts() override { return 0; }
  const char* getManufacturerName() const override { return "Heltec"; }
  void reboot() override { ESP.restart(); }
  uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }
};

// Last-line TX interlock. Dispatcher callbacks may parse incoming MeshCore
// packets while the owner gate is locked, but only an explicitly armed owner
// packet or a rate-limited reply to authenticated direct TXT can key the
// SX1262. API checks and outbound-queue purge remain useful earlier layers;
// this is the radio boundary that none of them can bypass.
class GatedCustomSX1262Wrapper final : public CustomSX1262Wrapper {
 public:
  GatedCustomSX1262Wrapper(CustomSX1262& radio, ::mesh::MainBoard& board,
                           const TxGate& gate, const Settings& settings)
      : CustomSX1262Wrapper(radio, board), physical_(&radio), gate_(&gate),
        settings_(&settings) {}

  void loop() override {
    // SX1262 DIO1 remains asserted until RadioLib clears the IRQ status.  The
    // original ISR did nothing except set MeshCore's flag, so polling once at
    // the same loop boundary cannot lose RX-done or TX-done notification.
    const bool dioAsserted = pollRadioDio1WithDiagnostics();
    serviceLowRateIrqObservation(dioAsserted);
    CustomSX1262Wrapper::loop();
  }

  bool startSendRaw(const uint8_t* bytes, int length) override {
    // Every physical start owns a fresh completion pair. In particular, a
    // rejected or failed attempt must not expose the prior TX's latch to
    // Dispatcher.
    txTurnaroundCompletion_.reset();
    txDoneRearmTimingPending_ = false;
    txDoneReadyTimingPending_ = false;
    lastTxDoneToStartReceiveMicrosAvailable_ = false;
    lastTxDoneToStartReceiveMicros_ = 0U;
    lastTxDoneToRxConfirmedMicrosAvailable_ = false;
    lastTxDoneToRxConfirmedMicros_ = 0U;
    turnaroundRearmPhysicalRxConfirmed_ = false;
    turnaroundRearmChipStatusAvailable_ = false;
    turnaroundRearmChipStatus_ = 0U;
    const bool sessionAllowed = gate_->allowsTransmit(*settings_);
    bool oneShotAllowed = oneShotArmed_ &&
        static_cast<uint32_t>(millis() - oneShotArmedAt_) <=
            oneShotPermitLifetimeMs_;
    if (oneShotAllowed && oneShotPacketBound_) {
      uint8_t digest[kOneShotPacketDigestBytes]{};
      if (!bytes || length <= 0 || length > MAX_TRANS_UNIT) {
        oneShotAllowed = false;
      } else {
        ::mesh::Utils::sha256(digest, sizeof(digest), bytes, length);
        uint8_t difference = 0U;
        for (size_t index = 0U; index < sizeof(digest); ++index) {
          difference |= digest[index] ^ oneShotPacketDigest_[index];
        }
        oneShotAllowed = difference == 0U;
      }
      memset(digest, 0, sizeof(digest));
    }
    if (!sessionAllowed && !oneShotAllowed) {
      if (oneShotArmed_) revokeOneShot();
      return false;
    }
    // Consume before touching the radio. A failed physical start must never
    // leave a reusable authorization behind for a different packet.
    if (oneShotArmed_) revokeOneShot();
    const uint32_t estimatedAirtimeMs = getEstAirtimeFor(length);
    const uint32_t timeoutMs = txTurnaroundTimeoutMs(estimatedAirtimeMs);
    const TxTurnaroundResult result = runSynchronousTxTurnaround(
        txTurnaroundCompletion_, timeoutMs == 0U ? 1U : timeoutMs,
        [this, bytes, length]() {
          return CustomSX1262Wrapper::startSendRaw(bytes, length);
        },
        []() { return millis(); },
        [this]() { (void)pollRadioDio1WithDiagnostics(); },
        [this]() {
          const bool completed =
              CustomSX1262Wrapper::isSendComplete();
          if (completed) {
            txDoneConsumedAtMicros_ = micros();
            txDoneRearmTimingPending_ = true;
            txDoneReadyTimingPending_ = true;
          }
          return completed;
        },
        [this]() { CustomSX1262Wrapper::onSendFinished(); },
        [this]() {
          turnaroundRearmPhysicalRxConfirmed_ = resumeReceiveNow();
          turnaroundRearmChipStatusAvailable_ =
              lastRxChipStatusAvailable_;
          turnaroundRearmChipStatus_ = lastRxChipStatus_;
        },
        []() { ::yield(); });
    switch (result) {
      case TxTurnaroundResult::Completed:
        incrementSaturating(syncTurnaroundCompleted_);
        openIrqObservationWindow(millis());
        lastCompletedTxPhysicalRxConfirmed_ =
            turnaroundRearmPhysicalRxConfirmed_;
        lastCompletedTxRxStartAttempts_ = lastRxStartAttempts_;
        lastCompletedTxRxStartCodeAvailable_ =
            lastRxStartCodeAvailable_;
        lastCompletedTxRxStartCode_ = lastRxStartCode_;
        lastCompletedTxRxStartSoftwareState_ =
            lastRxStartSoftwareState_;
        lastCompletedTxRxChipStatusAvailable_ =
            turnaroundRearmChipStatusAvailable_;
        lastCompletedTxRxChipStatus_ = turnaroundRearmChipStatus_;
        lastCompletedTxDoneToStartReceiveMicrosAvailable_ =
            lastTxDoneToStartReceiveMicrosAvailable_;
        lastCompletedTxDoneToStartReceiveMicros_ =
            lastTxDoneToStartReceiveMicros_;
        lastCompletedTxDoneToRxConfirmedMicrosAvailable_ =
            lastTxDoneToRxConfirmedMicrosAvailable_;
        lastCompletedTxDoneToRxConfirmedMicros_ =
            lastTxDoneToRxConfirmedMicros_;
        if (lastCompletedTxPhysicalRxConfirmed_) {
          incrementSaturating(physicalRxConfirmedAfterTx_);
        }
        return true;
      case TxTurnaroundResult::StartFailed:
        incrementSaturating(syncTurnaroundStartFailures_);
        return false;
      case TxTurnaroundResult::TimedOut:
        incrementSaturating(syncTurnaroundTimeouts_);
        return false;
    }
    return false;
  }

  bool sendDirectOneShotRaw(const uint8_t* bytes, int length) {
    const bool sent = startSendRaw(bytes, length);
    // startSendRaw already consumed the physical TX completion and rearmed
    // RX. There is no Dispatcher-owned packet for this direct frame, so clear
    // only its synthetic completion latch without invoking onSendFinished a
    // second time.
    txTurnaroundCompletion_.reset();
    return sent;
  }

  // TX_DONE was consumed synchronously before RX was rearmed. Dispatcher
  // must consume only this independent latch: RadioLib's shared interrupt
  // state may already contain a new RX_DONE here.
  bool isSendComplete() override {
    return txTurnaroundCompletion_.takeForDispatcher();
  }

  // The matching physical finishTransmit()/board callback also happened in
  // startSendRaw(). Repeating it here would clear a fast RX_DONE and put the
  // SX1262 back in standby, so Dispatcher's paired callback is a one-shot
  // no-op.
  void onSendFinished() override {
    (void)txTurnaroundCompletion_.consumeReportedFinish();
  }

  bool armOneShot(const Settings& requested, bool explicitUserApproval) {
    revokeOneShot();
    if (!explicitUserApproval || requested.txPolicy != TxPolicy::ExplicitSession ||
        !requested.enabled || validateSettings(requested) != Status::Ok ||
        !settings_->enabled ||
        settings_->txPolicy != TxPolicy::ExplicitSession ||
        !sameRadioProfile(requested.radio, settings_->radio)) {
      return false;
    }
    oneShotArmed_ = true;
    oneShotArmedAt_ = millis();
    oneShotPermitLifetimeMs_ = kOneShotPermitLifetimeMs;
    return true;
  }

  // An owner-approved advert or message can legitimately sit in MeshCore's 1%
  // airtime scheduler longer than the generic five-second permit. Bind the
  // longer permit to the exact serialized packet, so no different outbound
  // frame can consume that action while it waits for its regulatory slot.
  bool armOneShotForPacket(const Settings& requested,
                           bool explicitUserApproval,
                           const uint8_t* bytes, size_t byteCount) {
    if (!bytes || byteCount == 0U || byteCount > MAX_TRANS_UNIT ||
        !armOneShot(requested, explicitUserApproval)) {
      revokeOneShot();
      return false;
    }
    ::mesh::Utils::sha256(oneShotPacketDigest_,
                          sizeof(oneShotPacketDigest_), bytes, byteCount);
    oneShotPacketBound_ = true;
    oneShotPermitLifetimeMs_ = kBoundOneShotPermitLifetimeMs;
    return true;
  }

  // A successfully decrypted direct TXT packet is already authenticated to a
  // known MeshCore contact. When the owner has persisted ExplicitSession for
  // the active profile, permit its single standards-required ACK/PATH reply
  // without opening the general session gate. The global limiter prevents a
  // known-but-hostile contact from turning authenticated traffic into an
  // unbounded transmit oracle; Dispatcher's 1% airtime budget remains the
  // stronger long-term regulatory ceiling.
  bool armAuthenticatedReply(const Settings& requested) {
    const bool sessionAllowed = gate_->allowsTransmit(*settings_);
    if (!requested.enabled ||
        requested.txPolicy != TxPolicy::ExplicitSession ||
        validateSettings(requested) != Status::Ok || !settings_->enabled ||
        settings_->txPolicy != TxPolicy::ExplicitSession ||
        !sameRadioProfile(requested.radio, settings_->radio) ||
        (!sessionAllowed && oneShotArmed_) || !takeProtocolReplyToken()) {
      return false;
    }
    if (!sessionAllowed) {
      oneShotArmed_ = true;
      oneShotArmedAt_ = millis();
      oneShotPermitLifetimeMs_ = kOneShotPermitLifetimeMs;
    }
    return true;
  }

  void revokeOneShot() {
    oneShotArmed_ = false;
    oneShotArmedAt_ = 0U;
    oneShotPermitLifetimeMs_ = 0U;
    oneShotPacketBound_ = false;
    memset(oneShotPacketDigest_, 0, sizeof(oneShotPacketDigest_));
  }

  int recvRaw(uint8_t* bytes, int capacity) override {
    const int length = CustomSX1262Wrapper::recvRaw(bytes, capacity);
    if (length <= 0) return length;
    // Dispatcher::tryParsePacket() reads header, optional transport codes and
    // path_len before doing its first bounds check in v1.17.1.  Drop a short
    // physical frame here so those reads can never touch stale stack bytes.
    if (!bytes || length < 2) {
      recordShortFrameRejected(length);
      return 0;
    }
    const uint8_t route = bytes[0] & PH_ROUTE_MASK;
    const bool hasTransportCodes = route == ROUTE_TYPE_TRANSPORT_FLOOD ||
        route == ROUTE_TYPE_TRANSPORT_DIRECT;
    if (hasTransportCodes && length < 6) {
      recordShortFrameRejected(length);
      return 0;
    }
    return length;
  }

  // Dispatcher normally restarts RX only after subclass logTx bookkeeping.
  // A first-hop repeater is allowed to choose a zero retransmit delay, so put
  // the SX1262 back into continuous receive mode immediately after TX-done and
  // let the remaining correlation/journal work run while the radio listens.
  bool resumeReceiveNow() {
    lastRxStartAttempts_ = 0U;
    lastRxStartCodeAvailable_ = false;
    lastRxStartCode_ = 0;
    lastRxStartSoftwareState_ = isInRecvMode();
    lastRxChipStatusAvailable_ = false;
    lastRxChipStatus_ = 0U;
    lastTxDoneToRxConfirmedMicrosAvailable_ = false;
    lastTxDoneToRxConfirmedMicros_ = 0U;

    RxRearmEvidence evidence{};
    if (!isInRecvMode()) {
      evidence = startAndProbeReceive();
    } else {
      // This path should not occur after the paired onSendFinished(), but it
      // is handled without issuing a destructive nominal duplicate start.
      evidence.softwareRx = true;
      evidence.chipStatusAvailable =
          readSx126xStatus(*physical_, evidence.chipStatus);
    }

    // Retry at most once, only on positive failure evidence and only while
    // DIO1 is low. A high DIO1 may already be RX_DONE. An unavailable status
    // by itself is not evidence and must never trigger another startReceive.
    if (shouldRetryRxRearm(evidence, digitalRead(kLoraDio1) == LOW)) {
      incrementSaturating(rxRearmRetries_);
      evidence = startAndProbeReceive();
    }

    lastRxStartCodeAvailable_ = evidence.startAttempted;
    lastRxStartCode_ = evidence.startCode;
    lastRxStartSoftwareState_ = evidence.softwareRx;
    lastRxChipStatusAvailable_ = evidence.chipStatusAvailable;
    lastRxChipStatus_ =
        evidence.chipStatusAvailable ? evidence.chipStatus : 0U;
    const bool confirmed = rxRearmPhysicallyConfirmed(evidence);
    if (confirmed && txDoneReadyTimingPending_) {
      lastTxDoneToRxConfirmedMicros_ =
          static_cast<uint32_t>(micros() - txDoneConsumedAtMicros_);
      lastTxDoneToRxConfirmedMicrosAvailable_ = true;
    }
    txDoneReadyTimingPending_ = false;
    if (!confirmed) incrementSaturating(rxRearmFailures_);
    return confirmed;
  }

  void currentReceiveSnapshot(bool& softwareRx, bool& statusAvailable,
                              uint8_t& chipStatus) {
    softwareRx = isInRecvMode();
    statusAvailable = readSx126xStatus(*physical_, chipStatus);
    if (!statusAvailable) chipStatus = 0U;
  }

  void receiveObservability(RepeatDiagnostics& output) const {
    const RadioIrqPollDiagnostics poll = kitsuRadioIrqPoll.diagnostics();
    output.dio1Polls = poll.polls;
    output.dio1HighPolls = poll.highPolls;
    output.dio1HighEdges = poll.highEdges;
    output.dio1Callbacks = poll.callbacks;

    const CustomSX1262IrqDiagnostics irq = physical_->irqDiagnostics();
    output.irqSamples = irq.samples;
    output.irqDioAssertedSamples = irq.dioAssertedSamples;
    output.irqLowRateSamples = irq.lowRateSamples;
    const uint32_t now = millis();
    output.irqObservationOpen = irqObservationWindowOpen(now);
    output.irqObservationRemainingMs = output.irqObservationOpen
        ? static_cast<uint32_t>(irqObservationUntilMs_ - now)
        : 0U;
    output.lastIrqFlags = irq.lastFlags;
    output.lastDioIrqFlags = irq.lastDioAssertedFlags;
    output.lastLowRateIrqFlags = irq.lastLowRateFlags;
    output.irqRxDoneObservations = irq.rxDoneObservations;
    output.irqCrcErrorObservations = irq.crcErrorObservations;
    output.irqHeaderErrorObservations = irq.headerErrorObservations;
    output.irqTimeoutObservations = irq.timeoutObservations;
    output.irqPreambleObservations = irq.preambleObservations;
    output.irqHeaderValidObservations = irq.headerValidObservations;
    output.irqSyncWordValidObservations = irq.syncWordValidObservations;
    output.dioIrqRxDoneObservations = irq.dio.rxDone;
    output.dioIrqCrcErrorObservations = irq.dio.crcError;
    output.dioIrqHeaderErrorObservations = irq.dio.headerError;
    output.dioIrqTimeoutObservations = irq.dio.timeout;
    output.dioIrqPreambleObservations = irq.dio.preamble;
    output.dioIrqHeaderValidObservations = irq.dio.headerValid;
    output.dioIrqSyncWordValidObservations = irq.dio.syncWordValid;
    output.lowRateIrqRxDoneObservations = irq.lowRate.rxDone;
    output.lowRateIrqCrcErrorObservations = irq.lowRate.crcError;
    output.lowRateIrqHeaderErrorObservations = irq.lowRate.headerError;
    output.lowRateIrqTimeoutObservations = irq.lowRate.timeout;
    output.lowRateIrqPreambleObservations = irq.lowRate.preamble;
    output.lowRateIrqHeaderValidObservations = irq.lowRate.headerValid;
    output.lowRateIrqSyncWordValidObservations =
        irq.lowRate.syncWordValid;

    const RadioLibReceiveDiagnostics receive = receiveDiagnostics();
    output.recvRawAttempts = receive.recvRawAttempts;
    output.recvInterruptReadyAttempts = receive.interruptReadyAttempts;
    output.recvPacketLengthSamples = receive.packetLengthSamples;
    output.recvPacketLengthZero = receive.packetLengthZero;
    output.lastRecvPacketLengthAvailable =
        receive.lastPacketLengthAvailable;
    output.lastRecvPacketLength = receive.lastPacketLength;
    output.recvReadDataAttempts = receive.readDataAttempts;
    output.recvSuccessfulReads = receive.successfulReads;
    output.recvReadDataErrors = receive.readDataErrors;
    output.lastRecvReadDataErrorAvailable =
        receive.lastReadDataErrorAvailable;
    output.lastRecvReadDataError = receive.lastReadDataError;
    output.recvRxRestartAttempts = receive.rxRestartAttempts;
    output.recvRxRestartSuccesses = receive.rxRestartSuccesses;
    output.recvRxRestartErrors = receive.rxRestartErrors;
    output.lastRecvRxRestartResultAvailable =
        receive.lastRxRestartResultAvailable;
    output.lastRecvRxRestartResult = receive.lastRxRestartResult;
    output.lastRecvRxRestartErrorAvailable =
        receive.lastRxRestartErrorAvailable;
    output.lastRecvRxRestartError = receive.lastRxRestartError;
    output.shortFrameRejected = shortFrameRejected_;
    output.lastShortFrameLengthAvailable =
        lastShortFrameLengthAvailable_;
    output.lastShortFrameLength = lastShortFrameLength_;
  }

  bool lastRxChipStatusAvailable() const {
    return lastRxChipStatusAvailable_;
  }

  uint8_t lastRxChipStatus() const { return lastRxChipStatus_; }

  uint32_t physicalRxConfirmedAfterTx() const {
    return physicalRxConfirmedAfterTx_;
  }

  bool lastCompletedTxPhysicalRxConfirmed() const {
    return lastCompletedTxPhysicalRxConfirmed_;
  }

  bool lastCompletedTxRxChipStatusAvailable() const {
    return lastCompletedTxRxChipStatusAvailable_;
  }

  uint8_t lastCompletedTxRxChipStatus() const {
    return lastCompletedTxRxChipStatus_;
  }

  uint32_t rxRearmAttempts() const { return rxRearmAttempts_; }

  uint32_t rxRearmRetries() const { return rxRearmRetries_; }

  uint32_t rxRearmFailures() const { return rxRearmFailures_; }

  uint8_t lastCompletedTxRxStartAttempts() const {
    return lastCompletedTxRxStartAttempts_;
  }

  bool lastCompletedTxRxStartCodeAvailable() const {
    return lastCompletedTxRxStartCodeAvailable_;
  }

  int16_t lastCompletedTxRxStartCode() const {
    return lastCompletedTxRxStartCode_;
  }

  bool lastCompletedTxRxStartSoftwareState() const {
    return lastCompletedTxRxStartSoftwareState_;
  }

  uint32_t syncTurnaroundCompleted() const {
    return syncTurnaroundCompleted_;
  }

  uint32_t syncTurnaroundStartFailures() const {
    return syncTurnaroundStartFailures_;
  }

  uint32_t syncTurnaroundTimeouts() const {
    return syncTurnaroundTimeouts_;
  }

  bool lastTxDoneToStartReceiveMicrosAvailable() const {
    return lastCompletedTxDoneToStartReceiveMicrosAvailable_;
  }

  uint32_t lastTxDoneToStartReceiveMicros() const {
    return lastCompletedTxDoneToStartReceiveMicros_;
  }

  bool lastCompletedTxDoneToRxConfirmedMicrosAvailable() const {
    return lastCompletedTxDoneToRxConfirmedMicrosAvailable_;
  }

  uint32_t lastCompletedTxDoneToRxConfirmedMicros() const {
    return lastCompletedTxDoneToRxConfirmedMicros_;
  }

  void clearTurnaroundDiagnostics() {
    syncTurnaroundCompleted_ = 0U;
    syncTurnaroundStartFailures_ = 0U;
    syncTurnaroundTimeouts_ = 0U;
    physicalRxConfirmedAfterTx_ = 0U;
    rxRearmAttempts_ = 0U;
    rxRearmRetries_ = 0U;
    rxRearmFailures_ = 0U;
    lastRxStartAttempts_ = 0U;
    lastRxStartCodeAvailable_ = false;
    lastRxStartCode_ = 0;
    lastRxStartSoftwareState_ = false;
    lastRxChipStatusAvailable_ = false;
    lastRxChipStatus_ = 0U;
    lastCompletedTxPhysicalRxConfirmed_ = false;
    lastCompletedTxRxStartAttempts_ = 0U;
    lastCompletedTxRxStartCodeAvailable_ = false;
    lastCompletedTxRxStartCode_ = 0;
    lastCompletedTxRxStartSoftwareState_ = false;
    lastCompletedTxRxChipStatusAvailable_ = false;
    lastCompletedTxRxChipStatus_ = 0U;
    lastTxDoneToStartReceiveMicrosAvailable_ = false;
    lastTxDoneToStartReceiveMicros_ = 0U;
    lastCompletedTxDoneToStartReceiveMicrosAvailable_ = false;
    lastCompletedTxDoneToStartReceiveMicros_ = 0U;
    lastTxDoneToRxConfirmedMicrosAvailable_ = false;
    lastTxDoneToRxConfirmedMicros_ = 0U;
    lastCompletedTxDoneToRxConfirmedMicrosAvailable_ = false;
    lastCompletedTxDoneToRxConfirmedMicros_ = 0U;
    txDoneRearmTimingPending_ = false;
    txDoneReadyTimingPending_ = false;
    kitsuRadioIrqPoll.clearDiagnostics();
    physical_->clearIrqDiagnostics();
    resetReceiveDiagnostics();
    dioIrqWasHigh_ = false;
    dioIrqSampleAtAvailable_ = false;
    lastDioIrqSampleAtMs_ = 0U;
    irqObservationInitialized_ = false;
    irqObservationUntilMs_ = 0U;
    lowRateIrqSampleAtAvailable_ = false;
    lastLowRateIrqSampleAtMs_ = 0U;
    shortFrameRejected_ = 0U;
    lastShortFrameLengthAvailable_ = false;
    lastShortFrameLength_ = 0U;
  }

 private:
  static constexpr uint32_t kOneShotPermitLifetimeMs = 5000UL;
  // UK/EU Narrow's worst-case scheduler refill plus forced-CAD window is
  // below this bound; it also matches action.apply's maximum expiry horizon.
  static constexpr uint32_t kBoundOneShotPermitLifetimeMs = 120000UL;
  static constexpr size_t kOneShotPacketDigestBytes = 32U;
  static constexpr uint8_t kProtocolReplyBurst = 8U;
  static constexpr uint32_t kProtocolReplyRefillMs = 10000UL;
  // Normal RX_DONE is sampled once and then consumed in the same task turn.
  // If DIO1 remains stuck high, re-sample at no more than 10 Hz so diagnostics
  // can show changing flags without high-rate SPI traffic perturbing RX.
  static constexpr uint32_t kStuckDioIrqSampleIntervalMs = 100UL;
  static constexpr uint32_t kLowRateIrqSampleIntervalMs = 100UL;
  static constexpr uint32_t kIrqObservationWindowMs = 120000UL;
  CustomSX1262* physical_;
  const TxGate* gate_;
  const Settings* settings_;
  TxTurnaroundCompletion txTurnaroundCompletion_{};
  uint32_t syncTurnaroundCompleted_ = 0U;
  uint32_t syncTurnaroundStartFailures_ = 0U;
  uint32_t syncTurnaroundTimeouts_ = 0U;
  uint32_t physicalRxConfirmedAfterTx_ = 0U;
  uint32_t rxRearmAttempts_ = 0U;
  uint32_t rxRearmRetries_ = 0U;
  uint32_t rxRearmFailures_ = 0U;
  uint8_t lastRxStartAttempts_ = 0U;
  bool lastRxStartCodeAvailable_ = false;
  int16_t lastRxStartCode_ = 0;
  bool lastRxStartSoftwareState_ = false;
  bool lastRxChipStatusAvailable_ = false;
  uint8_t lastRxChipStatus_ = 0U;
  bool turnaroundRearmPhysicalRxConfirmed_ = false;
  bool turnaroundRearmChipStatusAvailable_ = false;
  uint8_t turnaroundRearmChipStatus_ = 0U;
  bool lastCompletedTxPhysicalRxConfirmed_ = false;
  uint8_t lastCompletedTxRxStartAttempts_ = 0U;
  bool lastCompletedTxRxStartCodeAvailable_ = false;
  int16_t lastCompletedTxRxStartCode_ = 0;
  bool lastCompletedTxRxStartSoftwareState_ = false;
  bool lastCompletedTxRxChipStatusAvailable_ = false;
  uint8_t lastCompletedTxRxChipStatus_ = 0U;
  bool txDoneRearmTimingPending_ = false;
  bool txDoneReadyTimingPending_ = false;
  uint32_t txDoneConsumedAtMicros_ = 0U;
  bool lastTxDoneToStartReceiveMicrosAvailable_ = false;
  uint32_t lastTxDoneToStartReceiveMicros_ = 0U;
  bool lastCompletedTxDoneToStartReceiveMicrosAvailable_ = false;
  uint32_t lastCompletedTxDoneToStartReceiveMicros_ = 0U;
  bool lastTxDoneToRxConfirmedMicrosAvailable_ = false;
  uint32_t lastTxDoneToRxConfirmedMicros_ = 0U;
  bool lastCompletedTxDoneToRxConfirmedMicrosAvailable_ = false;
  uint32_t lastCompletedTxDoneToRxConfirmedMicros_ = 0U;
  bool oneShotArmed_ = false;
  uint32_t oneShotArmedAt_ = 0U;
  uint32_t oneShotPermitLifetimeMs_ = 0U;
  bool oneShotPacketBound_ = false;
  uint8_t oneShotPacketDigest_[kOneShotPacketDigestBytes]{};
  bool protocolReplyRateStarted_ = false;
  uint8_t protocolReplyTokens_ = kProtocolReplyBurst;
  uint32_t protocolReplyRefilledAt_ = 0U;
  bool dioIrqWasHigh_ = false;
  bool dioIrqSampleAtAvailable_ = false;
  uint32_t lastDioIrqSampleAtMs_ = 0U;
  bool irqObservationInitialized_ = false;
  uint32_t irqObservationUntilMs_ = 0U;
  bool lowRateIrqSampleAtAvailable_ = false;
  uint32_t lastLowRateIrqSampleAtMs_ = 0U;
  uint32_t shortFrameRejected_ = 0U;
  bool lastShortFrameLengthAvailable_ = false;
  uint8_t lastShortFrameLength_ = 0U;

  static void incrementSaturating(uint32_t& value) {
    if (value != UINT32_MAX) ++value;
  }

  bool pollRadioDio1WithDiagnostics() {
    bool asserted = false;
    (void)pollKitsuRadioDio1(&asserted);
    const uint32_t now = millis();
    if (!asserted) {
      dioIrqWasHigh_ = false;
      return false;
    }
    const bool newAssertion = !dioIrqWasHigh_;
    dioIrqWasHigh_ = true;
    const bool stuckSampleDue = dioIrqSampleAtAvailable_ &&
        static_cast<uint32_t>(now - lastDioIrqSampleAtMs_) >=
            kStuckDioIrqSampleIntervalMs;
    if (newAssertion || stuckSampleDue) {
      // SX1262 GetIrqStatus is non-clearing. This is task context, after the
      // level read and before Dispatcher can consume RX_DONE/readData().
      (void)physical_->observeIrqFlags(true);
      lastDioIrqSampleAtMs_ = now;
      dioIrqSampleAtAvailable_ = true;
    }
    return true;
  }

  bool irqObservationWindowOpen(uint32_t now) const {
    return irqObservationInitialized_ &&
        static_cast<int32_t>(irqObservationUntilMs_ - now) > 0;
  }

  void openIrqObservationWindow(uint32_t now) {
    irqObservationInitialized_ = true;
    irqObservationUntilMs_ = now + kIrqObservationWindowMs;
    lowRateIrqSampleAtAvailable_ = false;
    lastLowRateIrqSampleAtMs_ = 0U;
  }

  void serviceLowRateIrqObservation(bool dioAsserted) {
    const uint32_t now = millis();
    if (!irqObservationInitialized_) openIrqObservationWindow(now);
    if (dioAsserted || !irqObservationWindowOpen(now) || !isInRecvMode()) {
      return;
    }
    if (lowRateIrqSampleAtAvailable_ &&
        static_cast<uint32_t>(now - lastLowRateIrqSampleAtMs_) <
            kLowRateIrqSampleIntervalMs) {
      return;
    }
    // Ten non-clearing GetIrqStatus reads per second, only during the bounded
    // boot/after-TX evidence window, can catch preamble/header/error flags that
    // are intentionally not mapped to DIO1. Outside the window there is zero
    // diagnostic SPI sampling; asserted DIO1 remains captured separately.
    (void)physical_->observeIrqFlags(false, true);
    lastLowRateIrqSampleAtMs_ = now;
    lowRateIrqSampleAtAvailable_ = true;
  }

  void recordShortFrameRejected(int length) {
    incrementSaturating(shortFrameRejected_);
    lastShortFrameLengthAvailable_ = true;
    if (length <= 0) {
      lastShortFrameLength_ = 0U;
    } else if (length > UINT8_MAX) {
      lastShortFrameLength_ = UINT8_MAX;
    } else {
      lastShortFrameLength_ = static_cast<uint8_t>(length);
    }
  }

  void recordStartReceiveInvocation() {
    if (!txDoneRearmTimingPending_) return;
    lastTxDoneToStartReceiveMicros_ =
        static_cast<uint32_t>(micros() - txDoneConsumedAtMicros_);
    lastTxDoneToStartReceiveMicrosAvailable_ = true;
    txDoneRearmTimingPending_ = false;
  }

  RxRearmEvidence startAndProbeReceive() {
    RxRearmEvidence evidence{};
    evidence.startAttempted = true;
    recordStartReceiveInvocation();
    incrementSaturating(rxRearmAttempts_);
    if (lastRxStartAttempts_ != UINT8_MAX) ++lastRxStartAttempts_;
    evidence.startCode = startRecvWithStatus();
    evidence.softwareRx = isInRecvMode();
    // This GetStatus read is owned by RadioLib's Module and runs only in the
    // Arduino task context, never in the DIO callback/poll interception.
    evidence.chipStatusAvailable =
        readSx126xStatus(*physical_, evidence.chipStatus);
    return evidence;
  }

  bool takeProtocolReplyToken() {
    const uint32_t now = millis();
    if (!protocolReplyRateStarted_) {
      protocolReplyRateStarted_ = true;
      protocolReplyRefilledAt_ = now;
    } else {
      const uint32_t elapsed = now - protocolReplyRefilledAt_;
      const uint32_t refill = elapsed / kProtocolReplyRefillMs;
      if (refill != 0U) {
        const uint32_t replenished =
            static_cast<uint32_t>(protocolReplyTokens_) + refill;
        protocolReplyTokens_ = static_cast<uint8_t>(
            replenished < kProtocolReplyBurst ? replenished
                                              : kProtocolReplyBurst);
        protocolReplyRefilledAt_ =
            now - (elapsed % kProtocolReplyRefillMs);
      }
    }
    if (protocolReplyTokens_ == 0U) return false;
    --protocolReplyTokens_;
    return true;
  }
};

constexpr uint8_t kUnknownPath = 0xffU;
constexpr size_t kMessageQueueSize = 8;
constexpr size_t kDeliveryQueueSize = 4;
constexpr uint32_t kTextAckDelayMillis = 200UL;
constexpr uint32_t kSendTimeoutBaseMillis = 500UL;
constexpr char kMessagingNamespace[] = "kitsu_msg";
constexpr char kMessagingLegacyKey[] = "state1";
constexpr char kMessagingCompactKeyA[] = "state2a";
constexpr char kMessagingCompactKeyB[] = "state2b";
constexpr uint8_t kMessagingMagic[4] = {'K', 'M', 'S', '1'};
constexpr uint16_t kMessagingSchemaV1 = 1U;
constexpr uint16_t kMessagingSchema = 2U;

const uint8_t kPublicChannelSecret[32] = {
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
    0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

struct ContactEntry {
  bool used = false;
  bool pinned = false;
  uint8_t type = 0;
  uint8_t publicKey[PUB_KEY_SIZE]{};
  char name[33]{};
  uint8_t outPathLen = kUnknownPath;
  uint8_t outPath[MAX_PATH_SIZE]{};
  uint32_t lastAdvertTimestamp = 0;
};

struct ChannelEntry {
  bool used = false;
  char name[33]{};
  ::mesh::GroupChannel channel{};
  ChannelRegionScope regionScope = ChannelRegionScope::Legacy;
};

#pragma pack(push, 1)
struct PersistedContactV1 {
  uint8_t used;
  uint8_t pinned;
  uint8_t type;
  uint8_t outPathLen;
  uint8_t publicKey[PUB_KEY_SIZE];
  char name[33];
  uint32_t lastAdvertTimestamp;
  uint8_t outPath[MAX_PATH_SIZE];
};

struct PersistedChannelV1 {
  uint8_t used;
  char name[33];
  uint8_t secret[PUB_KEY_SIZE];
};

struct PersistedMessagingStateV1 {
  uint8_t magic[4];
  uint16_t schema;
  uint16_t recordBytes;
  uint32_t crc32;
  PersistedContactV1 contacts[kMeshContactCapacity];
  PersistedChannelV1 channels[kMeshChannelCapacity];
};

// Runtime route hints are deliberately not durable: v1 decode always restored
// kUnknownPath and v1 save always wrote an unknown/zero path. Omitting those 65
// bytes per contact makes a copy-on-write migration fit the measured NVS peak
// while preserving every field Kitsu actually restores.
struct PersistedContact {
  uint8_t used;
  uint8_t pinned;
  uint8_t type;
  uint8_t publicKey[PUB_KEY_SIZE];
  char name[33];
  uint32_t lastAdvertTimestamp;
};

struct PersistedChannel {
  uint8_t used;
  char name[33];
  uint8_t secret[PUB_KEY_SIZE];
  uint8_t regionScope;
};

struct PersistedMessagingState {
  uint8_t magic[4];
  uint16_t schema;
  uint16_t recordBytes;
  uint32_t crc32;
  uint32_t generation;
  PersistedContact contacts[kMeshContactCapacity];
  PersistedChannel channels[kMeshChannelCapacity];
};
#pragma pack(pop)

static_assert(sizeof(PersistedMessagingStateV1) == 1920U,
              "messaging NVS v1 decoder must remain byte-stable");
static_assert(sizeof(PersistedMessagingState) == 1148U,
              "compact messaging NVS v2 schema must remain byte-stable");

constexpr size_t kNvsEntryBytes = 32U;
constexpr size_t kMessagingCompactSinglePageEntries =
    1U + (sizeof(PersistedMessagingState) + kNvsEntryBytes - 1U) /
             kNvsEntryBytes +
    1U;
// The preserved board had a 608-byte active-page tail. One additional chunk
// header makes the exact transactional target 39 entries, still below the
// independently measured 50-entry peak allowance (504 usable - 454 live).
constexpr size_t kMessagingCompactMeasuredSplitEntries = 39U;
constexpr size_t kMessagingMeasuredOtherLiveEntries = 392U;
constexpr size_t kMessagingLegacyLiveEntries = 62U;
constexpr size_t kMessagingMeasuredUsableEntries = 504U;
static_assert(kMessagingCompactSinglePageEntries == 38U,
              "compact v2 must consume 38 NVS entries on one page");
static_assert(kMessagingCompactMeasuredSplitEntries <= 40U,
              "compact v2 split peak must remain bounded");
static_assert(kMessagingMeasuredOtherLiveEntries +
                      kMessagingLegacyLiveEntries +
                      kMessagingCompactMeasuredSplitEntries <=
                  kMessagingMeasuredUsableEntries,
              "legacy plus compact promotion must fit the proven board peak");
static_assert(kMessagingMeasuredOtherLiveEntries +
                      2U * kMessagingCompactMeasuredSplitEntries <=
                  kMessagingMeasuredUsableEntries,
              "alternating compact A/B update must fit the proven board peak");

union PersistedMessagingScratch {
  PersistedMessagingStateV1 v1;
  PersistedMessagingState v2;
};

bool validChannelRegionScope(ChannelRegionScope scope) {
  return scope == ChannelRegionScope::Legacy ||
      scope == ChannelRegionScope::Eu;
}

uint32_t messagingCrc(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xffffffffUL;
  for (size_t index = 0; index < length; ++index) {
    const uint8_t value = index >= 8U && index < 12U ? 0U : bytes[index];
    crc ^= value;
    for (uint8_t bit = 0; bit < 8U; ++bit) {
      crc = (crc >> 1U) ^
          (0xedb88320UL & (0U - static_cast<uint32_t>(crc & 1U)));
    }
  }
  return ~crc;
}

bool validPublicKey(const uint8_t* publicKey) {
  if (!publicKey) return false;
  bool anyNonzero = false;
  bool anyNotFf = false;
  for (size_t index = 0; index < PUB_KEY_SIZE; ++index) {
    anyNonzero = anyNonzero || publicKey[index] != 0U;
    anyNotFf = anyNotFf || publicKey[index] != 0xffU;
  }
  return anyNonzero && anyNotFf;
}

bool validContactType(uint8_t type) {
  return type >= ADV_TYPE_CHAT && type <= ADV_TYPE_SENSOR;
}

bool validChannelSecret(const uint8_t* secret) {
  if (!secret) return false;
  uint8_t combined = 0;
  for (size_t index = 0; index < PUB_KEY_SIZE; ++index) {
    combined |= secret[index];
  }
  return combined != 0U;
}

bool validPathEncoding(uint8_t encodedLength) {
  if (encodedLength == kUnknownPath) return true;
  if (!::mesh::Packet::isValidPathLen(encodedLength)) return false;
  const size_t hashSize = (encodedLength >> 6U) + 1U;
  const size_t hashCount = encodedLength & 63U;
  return hashSize * hashCount <= MAX_PATH_SIZE;
}

bool validStoredName(const char* name) {
  if (!name) return false;
  const size_t length = strnlen(name, 33U);
  return length > 0U && length < 33U && validMeshTextUtf8(name, length);
}

bool bytesAreZero(const void* value, size_t length) {
  if (!value) return false;
  const uint8_t* bytes = static_cast<const uint8_t*>(value);
  for (size_t index = 0; index < length; ++index) {
    if (bytes[index] != 0U) return false;
  }
  return true;
}

bool validCanonicalStoredName(const char* name) {
  if (!validStoredName(name)) return false;
  const size_t length = strnlen(name, 33U);
  return bytesAreZero(name + length + 1U, 32U - length);
}

void deriveChannelHash(::mesh::GroupChannel& channel) {
  bool upperHalfZero = true;
  for (size_t index = 16U; index < PUB_KEY_SIZE; ++index) {
    upperHalfZero = upperHalfZero && channel.secret[index] == 0U;
  }
  ::mesh::Utils::sha256(channel.hash, sizeof(channel.hash), channel.secret,
                        upperHalfZero ? 16U : PUB_KEY_SIZE);
}

class MessagingState {
 public:
  bool begin() {
    clearRam();
    resetStorageStatus();
    memset(&compactCandidateA_, 0, sizeof(compactCandidateA_));
    memset(&compactCandidateB_, 0, sizeof(compactCandidateB_));

    // Opening read-only is deliberate. Boot must never create a namespace,
    // retry a migration, erase an orphan, or compact NVS as a side effect.
    nvs_handle_t namespaceHandle = 0;
    const esp_err_t namespaceResult = nvs_open(
        kMessagingNamespace, NVS_READONLY, &namespaceHandle);
    if (namespaceResult == ESP_ERR_NVS_NOT_FOUND) {
      installPublicChannel();
      storageUsable_ = true;
      migrationPending_ = true;
      storageReason_ = MessagingStorageReason::FreshInitializationPending;
      return true;
    }
    if (namespaceResult != ESP_OK) {
      installPublicChannel();
      storageReason_ = MessagingStorageReason::NamespaceOpenFailed;
      return false;
    }

    const CompactCandidateState candidateA = readCompactCandidate(
        namespaceHandle, kMessagingCompactKeyA, compactCandidateA_);
    const CompactCandidateState candidateB = readCompactCandidate(
        namespaceHandle, kMessagingCompactKeyB, compactCandidateB_);
    if (candidateA == CompactCandidateState::ReadError ||
        candidateB == CompactCandidateState::ReadError) {
      nvs_close(namespaceHandle);
      installPublicChannel();
      storageReason_ = MessagingStorageReason::ReadFailed;
      return false;
    }
    CompactSlot selected = CompactSlot::None;
    const bool compactSelectionValid = selectCompactCandidate(
        candidateA, compactCandidateA_, candidateB, compactCandidateB_,
        selected);
    if (compactSelectionValid && selected != CompactSlot::None) {
      const PersistedMessagingState& record =
          selected == CompactSlot::A ? compactCandidateA_
                                     : compactCandidateB_;
      const CompactCandidateState peerState =
          selected == CompactSlot::A ? candidateB : candidateA;
      bool legacyStillPresent = false;
      if (!keyPresent(namespaceHandle, kMessagingLegacyKey,
                      legacyStillPresent)) {
        nvs_close(namespaceHandle);
        clearRam();
        installPublicChannel();
        storageReason_ = MessagingStorageReason::ReadFailed;
        return false;
      }
      nvs_close(namespaceHandle);
      if (decode(record)) {
        storageUsable_ = true;
        persistedSchema_ = kMessagingSchema;
        generation_ = record.generation;
        activeCompactSlot_ = selected;
        migrationPending_ = false;
        cleanupPending_ = peerState != CompactCandidateState::Absent ||
            legacyStillPresent;
        storageReason_ = peerState == CompactCandidateState::Invalid
            ? MessagingStorageReason::CompactPeerInvalid
            : cleanupPending_ ? MessagingStorageReason::CleanupPending
                              : MessagingStorageReason::Ready;
        return true;
      }
      clearRam();
      installPublicChannel();
      storageReason_ = MessagingStorageReason::State2Invalid;
      return false;
    }

    const bool compactInvalid =
        candidateA == CompactCandidateState::Invalid ||
        candidateB == CompactCandidateState::Invalid ||
        !compactSelectionValid;
    size_t storedBytes = 0U;
    const esp_err_t legacySizeResult = nvs_get_blob(
        namespaceHandle, kMessagingLegacyKey, nullptr, &storedBytes);
    if (legacySizeResult != ESP_OK &&
        legacySizeResult != ESP_ERR_NVS_NOT_FOUND) {
      nvs_close(namespaceHandle);
      clearRam();
      installPublicChannel();
      storageReason_ = MessagingStorageReason::ReadFailed;
      return false;
    }
    if (legacySizeResult == ESP_ERR_NVS_NOT_FOUND) storedBytes = 0U;
    if (storedBytes == sizeof(PersistedMessagingStateV1)) {
      PersistedMessagingStateV1& record = persistedScratch_.v1;
      memset(&record, 0, sizeof(record));
      size_t bytesRead = sizeof(record);
      const esp_err_t legacyReadResult = nvs_get_blob(
          namespaceHandle, kMessagingLegacyKey, &record, &bytesRead);
      nvs_close(namespaceHandle);
      if (legacyReadResult != ESP_OK || bytesRead != sizeof(record)) {
        clearRam();
        installPublicChannel();
        storageReason_ = legacyReadResult == ESP_ERR_NVS_NOT_FOUND ||
                legacyReadResult == ESP_ERR_NVS_INVALID_LENGTH ||
                bytesRead != sizeof(record)
            ? MessagingStorageReason::OrphanedLegacyRecord
            : MessagingStorageReason::ReadFailed;
        return false;
      }
      if (
          memcmp(record.magic, kMessagingMagic,
                 sizeof(kMessagingMagic)) == 0 &&
          record.schema == kMessagingSchemaV1 &&
          record.recordBytes == sizeof(record) &&
          record.crc32 == messagingCrc(
                              reinterpret_cast<const uint8_t*>(&record),
                              sizeof(record)) &&
          decode(record)) {
        // A valid v1 record is a supported read-only source indefinitely.
        // Promotion is attempted once, and only by an explicit owner mutation.
        storageUsable_ = true;
        persistedSchema_ = kMessagingSchemaV1;
        migrationPending_ = true;
        storageReason_ = compactInvalid
            ? MessagingStorageReason::State2InvalidLegacyUsable
            : MessagingStorageReason::LegacyMigrationPending;
        return true;
      }
    } else {
      nvs_close(namespaceHandle);
    }
    clearRam();
    installPublicChannel();
    storageReason_ = compactInvalid
        ? MessagingStorageReason::State2Invalid
        : storedBytes == 0U ? MessagingStorageReason::MissingRecord
                            : MessagingStorageReason::InvalidRecord;
    return false;
  }

  bool reset() {
    PersistedMessagingState& record = compactCandidateA_;
    if (!encodeDefaultRecord(record, nextGeneration(generation_))) {
      return false;
    }
    const bool saved = storageUsable_ ? persistCompactRecord(record)
                                      : replaceInvalidStorage(record);
    if (!saved) return false;
    clearRam();
    installPublicChannel();
    return true;
  }

  bool storageReady() const { return storageUsable_; }

  MessagingStorageStatus storageStatus() const {
    MessagingStorageStatus output{};
    output.usable = storageUsable_;
    output.persistedSchema = persistedSchema_;
    output.migrationPending = migrationPending_;
    output.cleanupPending = cleanupPending_;
    output.generation = generation_;
    output.lastWriteResult = lastWriteResult_;
    output.reason = storageReason_;
    return output;
  }

  size_t contactCount() const {
    size_t count = 0;
    for (const ContactEntry& entry : contacts_) count += entry.used ? 1U : 0U;
    return count;
  }

  ContactEntry* findContact(const uint8_t* publicKey) {
    for (ContactEntry& entry : contacts_) {
      if (entry.used && memcmp(entry.publicKey, publicKey, PUB_KEY_SIZE) == 0) {
        return &entry;
      }
    }
    return nullptr;
  }

  const ContactEntry* findContact(const uint8_t* publicKey) const {
    for (const ContactEntry& entry : contacts_) {
      if (entry.used && memcmp(entry.publicKey, publicKey, PUB_KEY_SIZE) == 0) {
        return &entry;
      }
    }
    return nullptr;
  }

  ContactEntry* contactAt(size_t ordinal) {
    for (ContactEntry& entry : contacts_) {
      if (!entry.used) continue;
      if (ordinal == 0U) return &entry;
      --ordinal;
    }
    return nullptr;
  }

  const ContactEntry* contactAt(size_t ordinal) const {
    for (const ContactEntry& entry : contacts_) {
      if (!entry.used) continue;
      if (ordinal == 0U) return &entry;
      --ordinal;
    }
    return nullptr;
  }

  TransportStatus upsertContact(const uint8_t* publicKey, const char* name,
                                uint8_t type, bool pinned,
                                uint32_t advertTimestamp = 0U) {
    if (!storageUsable_) return TransportStatus::MessagingStorageFailed;
    if (!validPublicKey(publicKey) || !validContactType(type) ||
        !validStoredName(name)) {
      return TransportStatus::InvalidArgument;
    }
    ContactEntry* entry = findContact(publicKey);
    // Explicit provisioning never evicts a discovered contact behind the
    // phone's back. The owner can list/drop one and retry if all slots are in
    // use.
    if (!entry) entry = allocationSlot();
    if (!entry) return TransportStatus::ContactTableFull;
    const ContactEntry previous = *entry;

    const uint8_t savedPathLen = entry->used ? entry->outPathLen : kUnknownPath;
    uint8_t savedPath[MAX_PATH_SIZE]{};
    if (entry->used) memcpy(savedPath, entry->outPath, sizeof(savedPath));
    const bool wasPinned = entry->used && entry->pinned;
    *entry = ContactEntry{};
    entry->used = true;
    entry->pinned = pinned || wasPinned;
    entry->type = type;
    memcpy(entry->publicKey, publicKey, PUB_KEY_SIZE);
    const size_t nameBytes = strnlen(name, 32U);
    memcpy(entry->name, name, nameBytes);
    entry->name[nameBytes] = '\0';
    entry->outPathLen = savedPathLen;
    memcpy(entry->outPath, savedPath, sizeof(savedPath));
    entry->lastAdvertTimestamp = advertTimestamp != 0U
        ? advertTimestamp
        : previous.lastAdvertTimestamp;
    if (!save()) {
      *entry = previous;
      return TransportStatus::MessagingStorageFailed;
    }
    return TransportStatus::Ok;
  }

  TransportStatus stageObservedContact(const uint8_t* publicKey,
                                       const char* name, uint8_t type,
                                       uint32_t timestamp) {
    // Only normal Client adverts become chat contacts automatically.  Other
    // standard advert roles remain visible in the advert queue/map.
    if (!storageUsable_ || type != ADV_TYPE_CHAT || !validPublicKey(publicKey) ||
        !validStoredName(name)) {
      return !storageUsable_ ? TransportStatus::MessagingStorageFailed
                            : TransportStatus::InvalidArgument;
    }
    ContactEntry* existing = findContact(publicKey);
    if (existing) {
      if (timestamp > existing->lastAdvertTimestamp) {
        existing->lastAdvertTimestamp = timestamp;
      }
      existing->type = type;
      const size_t bytes = strnlen(name, 32U);
      memset(existing->name, 0, sizeof(existing->name));
      memcpy(existing->name, name, bytes);
      return TransportStatus::Ok;
    }
    ContactEntry* entry = allocationSlot();
    if (!entry) return TransportStatus::ContactTableFull;
    *entry = ContactEntry{};
    entry->used = true;
    entry->type = type;
    entry->outPathLen = kUnknownPath;
    entry->lastAdvertTimestamp = timestamp;
    memcpy(entry->publicKey, publicKey, PUB_KEY_SIZE);
    const size_t bytes = strnlen(name, 32U);
    memcpy(entry->name, name, bytes);
    return TransportStatus::Ok;
  }

  void learnAdvert(const uint8_t* publicKey, const char* name, uint8_t type,
                   uint32_t timestamp) {
    (void)stageObservedContact(publicKey, name, type, timestamp);
  }

  TransportStatus removeContact(const uint8_t* publicKey) {
    if (!storageUsable_) return TransportStatus::MessagingStorageFailed;
    ContactEntry* entry = findContact(publicKey);
    if (!entry) return TransportStatus::ContactNotFound;
    const ContactEntry previous = *entry;
    *entry = ContactEntry{};
    entry->outPathLen = kUnknownPath;
    if (save()) return TransportStatus::Ok;
    *entry = previous;
    return TransportStatus::MessagingStorageFailed;
  }

  bool updatePath(ContactEntry& entry, const uint8_t* path,
                  uint8_t pathLen) {
    if (!path || !validPathEncoding(pathLen) || pathLen == kUnknownPath) {
      return false;
    }
    const size_t pathBytes = ((pathLen >> 6U) + 1U) * (pathLen & 63U);
    entry.outPathLen = pathLen;
    memset(entry.outPath, 0, sizeof(entry.outPath));
    memcpy(entry.outPath, path, pathBytes);
    // Route learning is message-driven and intentionally RAM-only.  Rebooting
    // falls back to a standards-compatible flood and learns a fresh path,
    // without turning incoming RF into flash writes.
    return true;
  }

  ChannelEntry* channel(uint8_t slot) {
    return slot < kMeshChannelCapacity && channels_[slot].used
        ? &channels_[slot]
        : nullptr;
  }

  const ChannelEntry* channel(uint8_t slot) const {
    return slot < kMeshChannelCapacity && channels_[slot].used
        ? &channels_[slot]
        : nullptr;
  }

  TransportStatus setChannel(uint8_t slot, const char* name,
                             const uint8_t* secret,
                             ChannelRegionScope regionScope) {
    if (!storageUsable_) return TransportStatus::MessagingStorageFailed;
    if (slot == 0U || slot >= kMeshChannelCapacity ||
        !validStoredName(name) || !validChannelSecret(secret) ||
        !validChannelRegionScope(regionScope)) {
      return TransportStatus::InvalidArgument;
    }
    ChannelEntry next{};
    next.used = true;
    const size_t nameBytes = strnlen(name, 32U);
    memcpy(next.name, name, nameBytes);
    memcpy(next.channel.secret, secret, PUB_KEY_SIZE);
    deriveChannelHash(next.channel);
    next.regionScope = regionScope;
    const ChannelEntry previous = channels_[slot];
    channels_[slot] = next;
    if (save()) return TransportStatus::Ok;
    channels_[slot] = previous;
    return TransportStatus::MessagingStorageFailed;
  }

  TransportStatus clearChannel(uint8_t slot) {
    if (!storageUsable_) return TransportStatus::MessagingStorageFailed;
    if (slot == 0U || slot >= kMeshChannelCapacity) {
      return TransportStatus::InvalidArgument;
    }
    const ChannelEntry previous = channels_[slot];
    channels_[slot] = ChannelEntry{};
    if (save()) return TransportStatus::Ok;
    channels_[slot] = previous;
    return TransportStatus::MessagingStorageFailed;
  }

  int findChannelSlot(const ::mesh::GroupChannel& channel) const {
    for (size_t slot = 0; slot < kMeshChannelCapacity; ++slot) {
      if (channels_[slot].used &&
          memcmp(channels_[slot].channel.secret, channel.secret,
                 PUB_KEY_SIZE) == 0) {
        return static_cast<int>(slot);
      }
    }
    return -1;
  }

  bool enqueueMessage(const ReceivedMessage& event) {
    if (messageCount_ == kMessageQueueSize) {
      messageRead_ = static_cast<uint8_t>(
          (messageRead_ + 1U) % kMessageQueueSize);
      --messageCount_;
      ++droppedMessages_;
    }
    messages_[messageWrite_] = event;
    messageWrite_ = static_cast<uint8_t>(
        (messageWrite_ + 1U) % kMessageQueueSize);
    ++messageCount_;
    return true;
  }

  bool takeMessage(ReceivedMessage& output) {
    if (messageCount_ == 0U) return false;
    output = messages_[messageRead_];
    messageRead_ = static_cast<uint8_t>(
        (messageRead_ + 1U) % kMessageQueueSize);
    --messageCount_;
    return true;
  }

  void beginPending(uint32_t messageTimestamp, uint32_t expectedAck,
                    const ContactEntry& recipient, MessageRoute route,
                    uint32_t timeoutMillis) {
    pending_ = true;
    pendingTimerStarted_ = false;
    pendingMessageTimestamp_ = messageTimestamp;
    pendingAck_ = expectedAck;
    pendingRoute_ = route;
    pendingTimeoutMillis_ = timeoutMillis;
    pendingExpiresAt_ = 0;
    memcpy(pendingRecipient_, recipient.publicKey, PUB_KEY_SIZE);
    pendingRepeaterCountKnown_ =
        route == MessageRoute::Direct &&
        recipient.outPathLen != kUnknownPath;
    pendingRepeaterCount_ = pendingRepeaterCountKnown_
        ? static_cast<uint8_t>(recipient.outPathLen & 63U)
        : 0U;
  }

  bool pending() const { return pending_; }

  void markPendingSent() {
    if (!pending_) return;
    pendingTimerStarted_ = true;
    pendingExpiresAt_ = millis() + pendingTimeoutMillis_;
    DeliveryEvent event{};
    event.kind = MessageKind::Direct;
    event.state = DeliveryState::Sent;
    event.route = pendingRoute_;
    event.messageTimestamp = pendingMessageTimestamp_;
    event.expectedAck = pendingAck_;
    memcpy(event.recipientPublicKey, pendingRecipient_, PUB_KEY_SIZE);
    enqueueDelivery(event);
  }

  void markPendingTxFailed() {
    if (pending_) completePending(DeliveryState::TxFailed);
  }

  bool acceptAck(const uint8_t* ack, size_t ackBytes,
                 bool authenticatedPathCountKnown = false,
                 uint8_t authenticatedPathCount = 0U) {
    if (!pending_ || !ack || ackBytes < sizeof(pendingAck_) ||
        memcmp(&pendingAck_, ack, sizeof(pendingAck_)) != 0) {
      return false;
    }
    // A flood recipient returns the original received path inside the
    // authenticated PATH payload. A direct-routed send already retained the
    // exact outbound path used. Simple ACK packet paths describe the return
    // route and must never be substituted for either source of evidence.
    const bool countKnown = pendingRepeaterCountKnown_ ||
        (pendingRoute_ == MessageRoute::Flood &&
         authenticatedPathCountKnown);
    const uint8_t count = pendingRepeaterCountKnown_
        ? pendingRepeaterCount_
        : authenticatedPathCount;
    completePending(DeliveryState::Delivered, countKnown, count);
    return true;
  }

  void cancelPending(bool sentBecomesUnconfirmed = false) {
    if (!pending_) return;
    // Once logTx() has reported Sent, no state change can retract the RF
    // packet.  Invalidating its ACK tracker is therefore "unconfirmed", not
    // "cancelled".  Work that never reached the radio remains cancellable.
    completePending(sentBecomesUnconfirmed && pendingTimerStarted_
                        ? DeliveryState::TimedOut
                        : DeliveryState::Cancelled);
  }

  void checkTimeout() {
    channelRepeats_.expire(millis());
    if (pending_ && pendingTimerStarted_ &&
        static_cast<int32_t>(millis() - pendingExpiresAt_) >= 0) {
      completePending(DeliveryState::TimedOut);
    }
  }

  bool takeDelivery(DeliveryEvent& output) {
    if (deliveryCount_ != 0U) {
      output = deliveries_[deliveryRead_];
      deliveryRead_ = static_cast<uint8_t>(
          (deliveryRead_ + 1U) % kDeliveryQueueSize);
      --deliveryCount_;
      return true;
    }
    ChannelRepeatObservation observation{};
    if (!channelRepeats_.takeDirty(observation)) return false;
    output = DeliveryEvent{};
    output.kind = MessageKind::Channel;
    output.state = DeliveryState::RepeatObserved;
    output.route = MessageRoute::Flood;
    output.channelSlot = observation.channelSlot;
    output.messageTimestamp = observation.messageTimestamp;
    output.repeatCountKnown = true;
    output.repeatCount = observation.repeatCount;
    output.repeatObservationOpen = observation.observationOpen;
    output.repeatSourceCount = observation.sourceCount;
    memcpy(output.repeatSources, observation.sources,
           sizeof(output.repeatSources));
    output.repeatSourcesTruncated = observation.sourcesTruncated;
    return true;
  }

  void enqueueChannelDelivery(DeliveryState state, uint8_t slot,
                              uint32_t messageTimestamp) {
    DeliveryEvent event{};
    event.kind = MessageKind::Channel;
    event.state = state;
    event.route = MessageRoute::Flood;
    event.channelSlot = slot;
    event.messageTimestamp = messageTimestamp;
    enqueueDelivery(event);
  }

  void markChannelSent(
      uint8_t slot, uint32_t messageTimestamp, uint8_t payloadType,
      const FloodRouteBinding& route,
      const uint8_t hash[kChannelRepeatHashBytes],
      const uint8_t digest[kChannelRepeatDigestBytes], uint32_t nowMs) {
    DeliveryEvent event{};
    event.kind = MessageKind::Channel;
    event.state = DeliveryState::Sent;
    event.route = MessageRoute::Flood;
    event.channelSlot = slot;
    event.messageTimestamp = messageTimestamp;
    event.repeatCountKnown = channelRepeats_.recordSent(
        payloadType, route, hash, digest, slot, messageTimestamp, nowMs);
    event.repeatCount = 0U;
    event.repeatObservationOpen = event.repeatCountKnown;
    enqueueDelivery(event);
  }

  void observeChannelRepeat(
      uint8_t payloadType, const FloodRouteBinding& route,
      uint8_t pathCount,
      const uint8_t hash[kChannelRepeatHashBytes],
      const uint8_t digest[kChannelRepeatDigestBytes], uint32_t nowMs) {
    ChannelRepeatObservation observation{};
    (void)channelRepeats_.observe(payloadType, route, pathCount, hash, digest,
                                  nowMs, observation);
  }

  ChannelRepeatObserveResult observeChannelRepeatDetailed(
      uint8_t payloadType, const FloodRouteBinding& route,
      uint8_t pathCount,
      const uint8_t hash[kChannelRepeatHashBytes],
      const uint8_t digest[kChannelRepeatDigestBytes],
      const uint8_t* lastHopToken, uint8_t lastHopTokenBytes,
      uint32_t nowMs, ChannelRepeatObservation& output) {
    return channelRepeats_.observeDetailed(
        payloadType, route, pathCount, hash, digest, lastHopToken,
        lastHopTokenBytes, nowMs, output);
  }

  void clearChannelRepeatTracking() { channelRepeats_.clear(); }
  void closeChannelRepeatTracking() { channelRepeats_.closeAll(); }

  uint32_t droppedMessageCount() const { return droppedMessages_; }
  uint32_t droppedDeliveryCount() const { return droppedDeliveries_; }

 private:
  ContactEntry contacts_[kMeshContactCapacity]{};
  ChannelEntry channels_[kMeshChannelCapacity]{};
  ReceivedMessage messages_[kMessageQueueSize]{};
  DeliveryEvent deliveries_[kDeliveryQueueSize]{};
  uint8_t messageRead_ = 0;
  uint8_t messageWrite_ = 0;
  uint8_t messageCount_ = 0;
  uint8_t deliveryRead_ = 0;
  uint8_t deliveryWrite_ = 0;
  uint8_t deliveryCount_ = 0;
  uint32_t droppedMessages_ = 0;
  uint32_t droppedDeliveries_ = 0;
  enum class CompactSlot : uint8_t { None = 0, A, B };
  enum class CompactCandidateState : uint8_t {
    Absent = 0,
    Valid,
    Invalid,
    ReadError,
  };

  bool storageUsable_ = false;
  uint16_t persistedSchema_ = 0U;
  bool migrationPending_ = false;
  bool cleanupPending_ = false;
  uint32_t generation_ = 0U;
  CompactSlot activeCompactSlot_ = CompactSlot::None;
  MessagingStorageWriteResult lastWriteResult_ =
      MessagingStorageWriteResult::NotAttempted;
  MessagingStorageReason storageReason_ =
      MessagingStorageReason::MissingRecord;
  bool pending_ = false;
  bool pendingTimerStarted_ = false;
  uint32_t pendingMessageTimestamp_ = 0;
  uint32_t pendingAck_ = 0;
  uint32_t pendingTimeoutMillis_ = 0;
  uint32_t pendingExpiresAt_ = 0;
  MessageRoute pendingRoute_ = MessageRoute::Flood;
  uint8_t pendingRecipient_[PUB_KEY_SIZE]{};
  bool pendingRepeaterCountKnown_ = false;
  uint8_t pendingRepeaterCount_ = 0;
  ChannelRepeatTracker channelRepeats_{};
  // The Arduino loop task has a small stack. Keep NVS records in this
  // heap-owned object. Two compact buffers are required for exact A/B
  // generation selection and exact write/readback byte comparison.
  PersistedMessagingScratch persistedScratch_{};
  PersistedMessagingState compactCandidateA_{};
  PersistedMessagingState compactCandidateB_{};

  void clearRam() {
    memset(contacts_, 0, sizeof(contacts_));
    for (ContactEntry& entry : contacts_) entry.outPathLen = kUnknownPath;
    memset(channels_, 0, sizeof(channels_));
    messageRead_ = messageWrite_ = messageCount_ = 0;
    deliveryRead_ = deliveryWrite_ = deliveryCount_ = 0;
    droppedMessages_ = droppedDeliveries_ = 0;
    pending_ = false;
    pendingTimerStarted_ = false;
    pendingMessageTimestamp_ = 0U;
    pendingRepeaterCountKnown_ = false;
    pendingRepeaterCount_ = 0U;
    channelRepeats_.clear();
  }

  void installPublicChannel() {
    ChannelEntry& channel = channels_[0];
    channel.used = true;
    memcpy(channel.name, "Public", 7U);
    memcpy(channel.channel.secret, kPublicChannelSecret,
           sizeof(kPublicChannelSecret));
    deriveChannelHash(channel.channel);
    channel.regionScope = ChannelRegionScope::Legacy;
  }

  ContactEntry* allocationSlot() {
    for (ContactEntry& entry : contacts_) {
      if (!entry.used) return &entry;
    }
    return nullptr;
  }

  void resetStorageStatus() {
    storageUsable_ = false;
    persistedSchema_ = 0U;
    migrationPending_ = false;
    cleanupPending_ = false;
    generation_ = 0U;
    activeCompactSlot_ = CompactSlot::None;
    lastWriteResult_ = MessagingStorageWriteResult::NotAttempted;
    storageReason_ = MessagingStorageReason::MissingRecord;
  }

  static const char* compactKey(CompactSlot slot) {
    return slot == CompactSlot::A ? kMessagingCompactKeyA
                                 : kMessagingCompactKeyB;
  }

  static uint32_t nextGeneration(uint32_t current) {
    return current == UINT32_MAX ? 1U : current + 1U;
  }

  static int generationOrder(uint32_t a, uint32_t b) {
    const uint32_t delta = a - b;
    if (delta == 0U || delta == 0x80000000UL) return 0;
    return delta < 0x80000000UL ? 1 : -1;
  }

  bool valid(const PersistedMessagingStateV1& record) const {
    if (memcmp(record.magic, kMessagingMagic, sizeof(kMessagingMagic)) != 0 ||
        record.schema != kMessagingSchemaV1 ||
        record.recordBytes != sizeof(record) ||
        record.crc32 != messagingCrc(
                            reinterpret_cast<const uint8_t*>(&record),
                            sizeof(record))) {
      return false;
    }
    for (size_t index = 0; index < kMeshContactCapacity; ++index) {
      const PersistedContactV1& source = record.contacts[index];
      if (source.used > 1U || source.pinned > 1U) return false;
      if (!source.used) continue;
      if (source.pinned == 0U || !validPublicKey(source.publicKey) ||
          !validContactType(source.type) || !validStoredName(source.name) ||
          !validPathEncoding(source.outPathLen)) {
        return false;
      }
    }
    for (size_t index = 0; index < kMeshChannelCapacity; ++index) {
      const PersistedChannelV1& source = record.channels[index];
      if (source.used > 1U) return false;
      if (index == 0U &&
          (!source.used || memcmp(source.name, "Public", 7U) != 0 ||
           memcmp(source.secret, kPublicChannelSecret, PUB_KEY_SIZE) != 0)) {
        return false;
      }
      if (source.used && (!validStoredName(source.name) ||
                          !validChannelSecret(source.secret))) {
        return false;
      }
    }
    return true;
  }

  bool valid(const PersistedMessagingState& record) const {
    if (memcmp(record.magic, kMessagingMagic, sizeof(kMessagingMagic)) != 0 ||
        record.schema != kMessagingSchema ||
        record.recordBytes != sizeof(record) || record.generation == 0U ||
        record.crc32 != messagingCrc(
                            reinterpret_cast<const uint8_t*>(&record),
                            sizeof(record))) {
      return false;
    }
    for (size_t index = 0; index < kMeshContactCapacity; ++index) {
      const PersistedContact& source = record.contacts[index];
      if (source.used > 1U || source.pinned > 1U) return false;
      if (!source.used) {
        if (source.pinned != 0U || source.type != 0U ||
            source.lastAdvertTimestamp != 0U ||
            !bytesAreZero(source.publicKey, sizeof(source.publicKey)) ||
            !bytesAreZero(source.name, sizeof(source.name))) {
          return false;
        }
        continue;
      }
      if (source.pinned != 1U || !validPublicKey(source.publicKey) ||
          !validContactType(source.type) ||
          !validCanonicalStoredName(source.name)) {
        return false;
      }
    }
    for (size_t index = 0; index < kMeshChannelCapacity; ++index) {
      const PersistedChannel& source = record.channels[index];
      const ChannelRegionScope scope =
          static_cast<ChannelRegionScope>(source.regionScope);
      if (source.used > 1U || !validChannelRegionScope(scope) ||
          (!source.used && scope != ChannelRegionScope::Legacy) ||
          (index == 0U && scope != ChannelRegionScope::Legacy)) {
        return false;
      }
      if (!source.used) {
        if (!bytesAreZero(source.name, sizeof(source.name)) ||
            !bytesAreZero(source.secret, sizeof(source.secret))) {
          return false;
        }
        continue;
      }
      if (index == 0U &&
          (!source.used || memcmp(source.name, "Public", 7U) != 0 ||
           memcmp(source.secret, kPublicChannelSecret, PUB_KEY_SIZE) != 0)) {
        return false;
      }
      if (!validCanonicalStoredName(source.name) ||
          !validChannelSecret(source.secret)) {
        return false;
      }
    }
    return true;
  }

  CompactCandidateState readCompactCandidate(
      nvs_handle_t namespaceHandle, const char* key,
      PersistedMessagingState& record) const {
    memset(&record, 0, sizeof(record));
    size_t storedBytes = 0U;
    const esp_err_t sizeResult = nvs_get_blob(
        namespaceHandle, key, nullptr, &storedBytes);
    if (sizeResult == ESP_ERR_NVS_NOT_FOUND) {
      return CompactCandidateState::Absent;
    }
    if (sizeResult != ESP_OK) return CompactCandidateState::ReadError;
    if (storedBytes != sizeof(record)) {
      memset(&record, 0, sizeof(record));
      return CompactCandidateState::Invalid;
    }
    size_t bytesRead = sizeof(record);
    const esp_err_t readResult = nvs_get_blob(
        namespaceHandle, key, &record, &bytesRead);
    if (readResult != ESP_OK) {
      memset(&record, 0, sizeof(record));
      return readResult == ESP_ERR_NVS_NOT_FOUND
          ? CompactCandidateState::Invalid
          : CompactCandidateState::ReadError;
    }
    if (bytesRead != sizeof(record) || !valid(record)) {
      memset(&record, 0, sizeof(record));
      return CompactCandidateState::Invalid;
    }
    return CompactCandidateState::Valid;
  }

  bool selectCompactCandidate(
      CompactCandidateState stateA, const PersistedMessagingState& recordA,
      CompactCandidateState stateB, const PersistedMessagingState& recordB,
      CompactSlot& selected) const {
    selected = CompactSlot::None;
    if (stateA == CompactCandidateState::Valid &&
        stateB == CompactCandidateState::Valid) {
      const int order = generationOrder(recordA.generation,
                                        recordB.generation);
      if (order > 0) selected = CompactSlot::A;
      else if (order < 0) selected = CompactSlot::B;
      else if (recordA.generation == recordB.generation &&
               memcmp(&recordA, &recordB, sizeof(recordA)) == 0) {
        // Byte-identical duplicate commits are deterministic; divergent equal
        // generations and the half-range wrap ambiguity are fail-closed.
        selected = CompactSlot::A;
      } else {
        return false;
      }
      return true;
    }
    if (stateA == CompactCandidateState::Valid) selected = CompactSlot::A;
    else if (stateB == CompactCandidateState::Valid) selected = CompactSlot::B;
    return true;
  }

  bool decode(const PersistedMessagingStateV1& record) {
    if (!valid(record)) return false;
    memset(contacts_, 0, sizeof(contacts_));
    for (ContactEntry& entry : contacts_) entry.outPathLen = kUnknownPath;
    memset(channels_, 0, sizeof(channels_));
    for (size_t index = 0; index < kMeshContactCapacity; ++index) {
      const PersistedContactV1& source = record.contacts[index];
      if (!source.used) continue;
      ContactEntry& destination = contacts_[index];
      destination.used = true;
      destination.pinned = true;
      destination.type = source.type;
      destination.outPathLen = kUnknownPath;
      memcpy(destination.publicKey, source.publicKey, PUB_KEY_SIZE);
      memcpy(destination.name, source.name, sizeof(destination.name));
      destination.name[32] = '\0';
      destination.lastAdvertTimestamp = source.lastAdvertTimestamp;
    }
    for (size_t index = 0; index < kMeshChannelCapacity; ++index) {
      const PersistedChannelV1& source = record.channels[index];
      if (!source.used) continue;
      ChannelEntry& destination = channels_[index];
      destination.used = true;
      memcpy(destination.name, source.name, sizeof(destination.name));
      destination.name[32] = '\0';
      memcpy(destination.channel.secret, source.secret, PUB_KEY_SIZE);
      deriveChannelHash(destination.channel);
      destination.regionScope = ChannelRegionScope::Legacy;
    }
    return true;
  }

  bool decode(const PersistedMessagingState& record) {
    if (!valid(record)) return false;
    memset(contacts_, 0, sizeof(contacts_));
    for (ContactEntry& entry : contacts_) entry.outPathLen = kUnknownPath;
    memset(channels_, 0, sizeof(channels_));
    for (size_t index = 0; index < kMeshContactCapacity; ++index) {
      const PersistedContact& source = record.contacts[index];
      if (!source.used) continue;
      ContactEntry& destination = contacts_[index];
      destination.used = true;
      destination.pinned = true;
      destination.type = source.type;
      destination.outPathLen = kUnknownPath;
      memcpy(destination.publicKey, source.publicKey, PUB_KEY_SIZE);
      memcpy(destination.name, source.name, sizeof(destination.name));
      destination.name[32] = '\0';
      destination.lastAdvertTimestamp = source.lastAdvertTimestamp;
    }
    for (size_t index = 0; index < kMeshChannelCapacity; ++index) {
      const PersistedChannel& source = record.channels[index];
      if (!source.used) continue;
      ChannelEntry& destination = channels_[index];
      destination.used = true;
      memcpy(destination.name, source.name, sizeof(destination.name));
      destination.name[32] = '\0';
      memcpy(destination.channel.secret, source.secret, PUB_KEY_SIZE);
      deriveChannelHash(destination.channel);
      destination.regionScope =
          static_cast<ChannelRegionScope>(source.regionScope);
    }
    return true;
  }

  void initializeRecord(PersistedMessagingState& record,
                        uint32_t generation) const {
    memset(&record, 0, sizeof(record));
    memcpy(record.magic, kMessagingMagic, sizeof(kMessagingMagic));
    record.schema = kMessagingSchema;
    record.recordBytes = sizeof(record);
    record.generation = generation;
  }

  bool finishRecord(PersistedMessagingState& record) const {
    record.crc32 = messagingCrc(
        reinterpret_cast<const uint8_t*>(&record), sizeof(record));
    return valid(record);
  }

  bool encodeDefaultRecord(PersistedMessagingState& record,
                           uint32_t generation) const {
    initializeRecord(record, generation);
    PersistedChannel& channel = record.channels[0];
    channel.used = 1U;
    memcpy(channel.name, "Public", 7U);
    memcpy(channel.secret, kPublicChannelSecret, PUB_KEY_SIZE);
    channel.regionScope = static_cast<uint8_t>(ChannelRegionScope::Legacy);
    return finishRecord(record);
  }

  bool encodeCurrentRecord(PersistedMessagingState& record,
                           uint32_t generation) const {
    initializeRecord(record, generation);
    for (size_t index = 0; index < kMeshContactCapacity; ++index) {
      const ContactEntry& source = contacts_[index];
      PersistedContact& destination = record.contacts[index];
      destination.used = source.used && source.pinned ? 1U : 0U;
      if (!destination.used) continue;
      if (!validPublicKey(source.publicKey) ||
          !validContactType(source.type) || !validStoredName(source.name)) {
        return false;
      }
      destination.pinned = 1U;
      destination.type = source.type;
      memcpy(destination.publicKey, source.publicKey, PUB_KEY_SIZE);
      const size_t nameBytes = strnlen(source.name, 32U);
      memcpy(destination.name, source.name, nameBytes);
      destination.lastAdvertTimestamp = source.lastAdvertTimestamp;
    }
    for (size_t index = 0; index < kMeshChannelCapacity; ++index) {
      const ChannelEntry& source = channels_[index];
      if (!validChannelRegionScope(source.regionScope) ||
          (index == 0U && source.regionScope != ChannelRegionScope::Legacy) ||
          (!source.used && source.regionScope != ChannelRegionScope::Legacy)) {
        return false;
      }
      PersistedChannel& destination = record.channels[index];
      destination.used = source.used ? 1U : 0U;
      if (!source.used) continue;
      if (!validStoredName(source.name) ||
          !validChannelSecret(source.channel.secret)) {
        return false;
      }
      const size_t nameBytes = strnlen(source.name, 32U);
      memcpy(destination.name, source.name, nameBytes);
      memcpy(destination.secret, source.channel.secret, PUB_KEY_SIZE);
      destination.regionScope = static_cast<uint8_t>(source.regionScope);
    }
    return finishRecord(record);
  }

  void writeFailed(MessagingStorageWriteResult result,
                   MessagingStorageReason reason) {
    lastWriteResult_ = result;
    storageReason_ = reason;
  }

  bool keyPresent(nvs_handle_t namespaceHandle, const char* key,
                  bool& present) const {
    present = false;
    size_t storedBytes = 0U;
    const esp_err_t result = nvs_get_blob(
        namespaceHandle, key, nullptr, &storedBytes);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) return false;
    present = true;
    return true;
  }

  bool keyPresentFresh(const char* key, bool& present) const {
    present = false;
    nvs_handle_t readHandle = 0;
    const esp_err_t openResult = nvs_open(
        kMessagingNamespace, NVS_READONLY, &readHandle);
    if (openResult == ESP_ERR_NVS_NOT_FOUND) return true;
    if (openResult != ESP_OK) return false;
    const bool readOk = keyPresent(readHandle, key, present);
    nvs_close(readHandle);
    return readOk;
  }

  bool removeAndConfirm(const char* key) const {
    bool present = false;
    if (!keyPresentFresh(key, present)) return false;
    if (!present) return true;
    nvs_handle_t writeHandle = 0;
    if (nvs_open(kMessagingNamespace, NVS_READWRITE,
                 &writeHandle) != ESP_OK) {
      return false;
    }
    const esp_err_t eraseResult = nvs_erase_key(writeHandle, key);
    const esp_err_t commitResult = eraseResult == ESP_OK
        ? nvs_commit(writeHandle)
        : eraseResult;
    nvs_close(writeHandle);
    if (commitResult != ESP_OK || !keyPresentFresh(key, present)) return false;
    return !present;
  }

  bool persistCompactRecord(PersistedMessagingState& expected) {
    if (!valid(expected)) {
      writeFailed(MessagingStorageWriteResult::VerifyFailed,
                  MessagingStorageReason::VerifyFailed);
      return false;
    }
    const CompactSlot previousActive = activeCompactSlot_;
    const uint16_t previousSchema = persistedSchema_;
    const CompactSlot target = previousActive == CompactSlot::A
        ? CompactSlot::B
        : previousActive == CompactSlot::B ? CompactSlot::A
                                           : CompactSlot::A;
    if (target == CompactSlot::None || target == previousActive) {
      writeFailed(MessagingStorageWriteResult::VerifyFailed,
                  MessagingStorageReason::VerifyFailed);
      return false;
    }
    const char* targetKey = compactKey(target);

    // If an earlier verified compact commit could not retire legacy state1,
    // retire it before allocating the next inactive compact generation. The
    // active compact record already makes this pre-clean safe; retaining all
    // three records would exceed the measured 504-entry usable peak.
    if (previousActive != CompactSlot::None &&
        !removeAndConfirm(kMessagingLegacyKey)) {
      writeFailed(MessagingStorageWriteResult::ClearFailed,
                  MessagingStorageReason::WriteFailed);
      return false;
    }

    // When v1 is authoritative, neither compact key is trusted (the pair may
    // be invalid, equal-generation divergent, or half-range ambiguous). Clear
    // and confirm both inactive candidates before generation 1 is written.
    // Legacy remains intact throughout this pre-clean and preserves rollback.
    if (previousActive == CompactSlot::None &&
        previousSchema == kMessagingSchemaV1 &&
        (!removeAndConfirm(kMessagingCompactKeyA) ||
         !removeAndConfirm(kMessagingCompactKeyB))) {
      writeFailed(MessagingStorageWriteResult::ClearFailed,
                  MessagingStorageReason::WriteFailed);
      return false;
    }

    // The active record is never touched. A stale/invalid inactive key is
    // removed first so a failed same-key replacement cannot consume peak NVS
    // space; the still-valid active or v1 record remains authoritative.
    if (!removeAndConfirm(targetKey)) {
      writeFailed(MessagingStorageWriteResult::ClearFailed,
                  MessagingStorageReason::WriteFailed);
      return false;
    }
    nvs_handle_t writeHandle = 0;
    if (nvs_open(kMessagingNamespace, NVS_READWRITE,
                 &writeHandle) != ESP_OK) {
      writeFailed(MessagingStorageWriteResult::OpenFailed,
                  MessagingStorageReason::WriteFailed);
      return false;
    }
    esp_err_t writeResult = nvs_set_blob(
        writeHandle, targetKey, &expected, sizeof(expected));
    if (writeResult == ESP_OK) writeResult = nvs_commit(writeHandle);
    nvs_close(writeHandle);
    if (writeResult != ESP_OK) {
      // A short/failed blob write may leave an orphaned target index or, in
      // the most conservative case, bytes whose authority cannot be inferred
      // from the return value alone. Remove and confirm the inactive target
      // while the prior active/legacy record is still intact.
      const bool invalidated = removeAndConfirm(targetKey);
      if (!invalidated) {
        storageUsable_ = false;
        writeFailed(MessagingStorageWriteResult::Ambiguous,
                    MessagingStorageReason::CommitAmbiguous);
      } else {
        writeFailed(MessagingStorageWriteResult::WriteFailed,
                    MessagingStorageReason::WriteFailed);
      }
      return false;
    }

    PersistedMessagingState* readbackTarget = &compactCandidateA_;
    if (readbackTarget == &expected) readbackTarget = &compactCandidateB_;
    PersistedMessagingState& readback = *readbackTarget;
    memset(&readback, 0, sizeof(readback));
    nvs_handle_t readbackHandle = 0;
    const esp_err_t readbackOpenResult = nvs_open(
        kMessagingNamespace, NVS_READONLY, &readbackHandle);
    if (readbackOpenResult != ESP_OK) {
      // The commit returned success but a fresh handle cannot establish what
      // is durable. Best-effort target cleanup is insufficient to call this a
      // rollback while the partition is unreadable, so fail closed.
      (void)removeAndConfirm(targetKey);
      storageUsable_ = false;
      writeFailed(MessagingStorageWriteResult::Ambiguous,
                  MessagingStorageReason::CommitAmbiguous);
      return false;
    }
    size_t readbackBytes = sizeof(readback);
    const esp_err_t readbackResult = nvs_get_blob(
        readbackHandle, targetKey, &readback, &readbackBytes);
    nvs_close(readbackHandle);
    const bool verified =
        readbackResult == ESP_OK && readbackBytes == sizeof(readback) &&
        valid(readback) &&
        memcmp(&expected, &readback, sizeof(expected)) == 0;
    if (!verified) {
      // The prior active/legacy record is still authoritative. Best-effort
      // removal prevents a fully written but unverified target from winning
      // generation selection after reboot.
      const bool invalidated = removeAndConfirm(targetKey);
      if (!invalidated) {
        storageUsable_ = false;
        writeFailed(MessagingStorageWriteResult::Ambiguous,
                    MessagingStorageReason::CommitAmbiguous);
      } else {
        writeFailed(MessagingStorageWriteResult::VerifyFailed,
                    MessagingStorageReason::VerifyFailed);
      }
      return false;
    }

    // The new compact generation is now the durable authority. Cleanup is
    // best-effort and happens only after exact readback validation.
    activeCompactSlot_ = target;
    generation_ = expected.generation;
    persistedSchema_ = kMessagingSchema;
    storageUsable_ = true;
    migrationPending_ = false;
    lastWriteResult_ = MessagingStorageWriteResult::Saved;
    bool cleanupOk = true;
    if (previousActive != CompactSlot::None) {
      cleanupOk = removeAndConfirm(compactKey(previousActive)) &&
          cleanupOk;
    }
    if (previousSchema == kMessagingSchemaV1) {
      cleanupOk = removeAndConfirm(kMessagingLegacyKey) &&
          cleanupOk;
    }
    const CompactSlot stale = target == CompactSlot::A ? CompactSlot::B
                                                       : CompactSlot::A;
    if (stale != previousActive) {
      cleanupOk = removeAndConfirm(compactKey(stale)) &&
          cleanupOk;
    }
    cleanupPending_ = !cleanupOk;
    storageReason_ = cleanupPending_
        ? MessagingStorageReason::CleanupPending
        : MessagingStorageReason::Ready;
    return true;
  }

  bool replaceInvalidStorage(PersistedMessagingState& expected) {
    nvs_handle_t namespaceHandle = 0;
    if (nvs_open(kMessagingNamespace, NVS_READWRITE,
                 &namespaceHandle) != ESP_OK) {
      writeFailed(MessagingStorageWriteResult::OpenFailed,
                  MessagingStorageReason::WriteFailed);
      return false;
    }
    const esp_err_t eraseResult = nvs_erase_all(namespaceHandle);
    const esp_err_t commitResult = eraseResult == ESP_OK
        ? nvs_commit(namespaceHandle)
        : eraseResult;
    nvs_close(namespaceHandle);
    bool legacyPresent = true;
    bool compactAPresent = true;
    bool compactBPresent = true;
    const bool cleared = commitResult == ESP_OK &&
        keyPresentFresh(kMessagingLegacyKey, legacyPresent) &&
        keyPresentFresh(kMessagingCompactKeyA, compactAPresent) &&
        keyPresentFresh(kMessagingCompactKeyB, compactBPresent) &&
        !legacyPresent && !compactAPresent && !compactBPresent;
    if (!cleared) {
      writeFailed(MessagingStorageWriteResult::ClearFailed,
                  MessagingStorageReason::WriteFailed);
      return false;
    }
    activeCompactSlot_ = CompactSlot::None;
    persistedSchema_ = 0U;
    generation_ = 0U;
    return persistCompactRecord(expected);
  }

  bool save() {
    PersistedMessagingState& record = persistedScratch_.v2;
    if (!encodeCurrentRecord(record, nextGeneration(generation_))) {
      writeFailed(MessagingStorageWriteResult::VerifyFailed,
                  MessagingStorageReason::VerifyFailed);
      return false;
    }
    return persistCompactRecord(record);
  }

  void completePending(DeliveryState state,
                       bool repeaterCountKnown = false,
                       uint8_t repeaterCount = 0U) {
    DeliveryEvent event{};
    event.kind = MessageKind::Direct;
    event.state = state;
    event.route = pendingRoute_;
    event.messageTimestamp = pendingMessageTimestamp_;
    event.expectedAck = pendingAck_;
    memcpy(event.recipientPublicKey, pendingRecipient_, PUB_KEY_SIZE);
    event.repeaterCountKnown =
        state == DeliveryState::Delivered && repeaterCountKnown;
    event.repeaterCount = event.repeaterCountKnown ? repeaterCount : 0U;
    enqueueDelivery(event);
    pending_ = false;
    pendingTimerStarted_ = false;
    pendingMessageTimestamp_ = 0U;
    pendingAck_ = 0;
    pendingTimeoutMillis_ = 0;
    pendingExpiresAt_ = 0;
    memset(pendingRecipient_, 0, sizeof(pendingRecipient_));
    pendingRepeaterCountKnown_ = false;
    pendingRepeaterCount_ = 0U;
  }

  void enqueueDelivery(const DeliveryEvent& event) {
    if (deliveryCount_ == kDeliveryQueueSize) {
      deliveryRead_ = static_cast<uint8_t>(
          (deliveryRead_ + 1U) % kDeliveryQueueSize);
      --deliveryCount_;
      ++droppedDeliveries_;
    }
    deliveries_[deliveryWrite_] = event;
    deliveryWrite_ = static_cast<uint8_t>(
        (deliveryWrite_ + 1U) % kDeliveryQueueSize);
    ++deliveryCount_;
  }
};

class AdvertSink {
 public:
  virtual ~AdvertSink() = default;
  virtual void captureAdvert(::mesh::Packet* packet,
                             const ::mesh::Identity& identity,
                             uint32_t timestamp,
                             const uint8_t* appData,
                             size_t appDataBytes) = 0;
  virtual void captureNearbyRadio(const uint8_t* bytes, size_t byteCount,
                                  float rssi, float snr) = 0;
};

class KitsuClient final : public ::mesh::Mesh {
 public:
  KitsuClient(GatedCustomSX1262Wrapper& radio,
              ::mesh::MillisecondClock& clock,
              ::mesh::RNG& rng, ::mesh::RTCClock& rtc,
              ::mesh::PacketManager& packets, ::mesh::MeshTables& tables,
              AdvertSink& sink, MessagingState& messaging,
              const TxGate& gate, const Settings& settings)
      : ::mesh::Mesh(radio, clock, rng, rtc, packets, tables), sink_(&sink),
         messaging_(&messaging), gate_(&gate), settings_(&settings),
         driver_(&radio) {}

  // Freeze exactly one route before a packet-scoped permit is hashed. Legacy
  // is the interoperability default; EU is selected only by an explicit,
  // persisted channel scope. Calling Mesh::sendFlood below repeats the same
  // deterministic mutation and never queues a fallback copy.
  bool prepareFloodRoute(::mesh::Packet* packet,
                         ChannelRegionScope regionScope) const {
    if (!packet || !validChannelRegionScope(regionScope)) return false;
    packet->header &= ~PH_ROUTE_MASK;
    if (regionScope == ChannelRegionScope::Legacy) {
      packet->header |= ROUTE_TYPE_FLOOD;
      packet->transport_codes[0] = 0U;
      packet->transport_codes[1] = 0U;
    } else {
      uint16_t code = 0U;
      if (!calculateDefaultTransportCode(
              packet->getPayloadType(), packet->payload,
              packet->payload_len, code)) {
        return false;
      }
      packet->header |= ROUTE_TYPE_TRANSPORT_FLOOD;
      packet->transport_codes[0] = code;
      packet->transport_codes[1] = 0U;
    }
    packet->setPathHashSizeAndCount(1U, 0U);
    return true;
  }

  bool sendFloodRoute(::mesh::Packet* packet,
                      ChannelRegionScope regionScope,
                      uint32_t delayMillis = 0U) {
    if (!prepareFloodRoute(packet, regionScope)) return false;
    if (regionScope == ChannelRegionScope::Legacy) {
      sendFlood(packet, delayMillis);
    } else {
      uint16_t codes[2] = {packet->transport_codes[0],
                           packet->transport_codes[1]};
      sendFlood(packet, codes, delayMillis);
    }
    return true;
  }

  static bool captureFloodRoute(const ::mesh::Packet* packet,
                                FloodRouteBinding& output) {
    output = FloodRouteBinding{};
    if (!packet || !packet->isRouteFlood()) return false;
    output.transportScoped = packet->hasTransportCodes();
    output.pathHashSize = packet->getPathHashSize();
    if (output.transportScoped) {
      output.transportCodes[0] = packet->transport_codes[0];
      output.transportCodes[1] = packet->transport_codes[1];
    }
    return validFloodRouteBinding(output);
  }

  TransportStatus sendDirectText(ContactEntry& recipient, uint32_t timestamp,
                                 const char* text, uint8_t attempt,
                                 uint32_t& expectedAck,
                                 MessageRoute& route,
                                 bool bindOneShotToPacket = false) {
    if (messaging_->pending()) return TransportStatus::SendBusy;
    uint8_t plaintext[kMeshPlaintextCapacity]{};
    size_t plaintextBytes = 0;
    const TextCodecStatus encoded = encodeDirectTextPayload(
        timestamp, attempt, text, plaintext, sizeof(plaintext),
        plaintextBytes);
    if (encoded == TextCodecStatus::TextTooLong) {
      return TransportStatus::TextTooLong;
    }
    if (encoded != TextCodecStatus::Ok) {
      return TransportStatus::InvalidArgument;
    }

    uint8_t secret[PUB_KEY_SIZE]{};
    self_id.calcSharedSecret(secret, recipient.publicKey);
    ::mesh::Identity destination(recipient.publicKey);
    ::mesh::Packet* packet = createDatagram(PAYLOAD_TYPE_TXT_MSG, destination,
                                            secret, plaintext,
                                            plaintextBytes);
    if (!packet) return TransportStatus::PacketPoolFull;

    const size_t textBytes = strnlen(text, kMeshTextCapacity);
    ::mesh::Utils::sha256(reinterpret_cast<uint8_t*>(&expectedAck), 4,
                          plaintext, 5U + textBytes,
                          self_id.pub_key, PUB_KEY_SIZE);
    uint32_t timeout = 0;
    if (recipient.outPathLen == kUnknownPath) {
      route = MessageRoute::Flood;
      if (!prepareFloodRoute(packet, ChannelRegionScope::Legacy)) {
        releasePacket(packet);
        return TransportStatus::InvalidArgument;
      }
      const uint32_t airtime =
          _radio->getEstAirtimeFor(packet->getRawLength());
      timeout = kSendTimeoutBaseMillis + 16UL * airtime;
      if (bindOneShotToPacket && !armOneShotForPacket(packet)) {
        releasePacket(packet);
        return TransportStatus::TxLocked;
      }
      pendingPacket_ = packet;
      (void)sendFloodRoute(packet, ChannelRegionScope::Legacy);
    } else {
      route = MessageRoute::Direct;
      packet->header &= ~PH_ROUTE_MASK;
      packet->header |= ROUTE_TYPE_DIRECT;
      packet->path_len = ::mesh::Packet::copyPath(
          packet->path, recipient.outPath, recipient.outPathLen);
      const uint32_t airtime =
          _radio->getEstAirtimeFor(packet->getRawLength());
      const uint32_t hops = (recipient.outPathLen & 63U) + 1U;
      timeout = kSendTimeoutBaseMillis +
          (airtime * 6UL + 250UL) * hops;
      if (bindOneShotToPacket && !armOneShotForPacket(packet)) {
        releasePacket(packet);
        return TransportStatus::TxLocked;
      }
      pendingPacket_ = packet;
      sendDirect(packet, recipient.outPath, recipient.outPathLen);
    }
    messaging_->beginPending(timestamp, expectedAck, recipient, route,
                             timeout);
    return TransportStatus::Ok;
  }

  bool beginFloodAdvert(::mesh::Packet* packet, uint32_t emittedAt) {
    if (!packet || advertPacket_ ||
        !advertRepeats_.recordQueued(packet->getPayloadType(),
                                     packet->isRouteFlood(), emittedAt)) {
      return false;
    }
    advertPacket_ = packet;
    advertTimestamp_ = emittedAt;
    return true;
  }

  bool beginNearbyAdvert(::mesh::Packet* packet, uint32_t emittedAt) {
    if (!packet || nearbyAdvertPacket_ ||
        !nearbyAdverts_.recordQueued(packet->getPayloadType(),
                                     packet->isRouteFlood(), emittedAt)) {
      return false;
    }
    nearbyAdvertPacket_ = packet;
    nearbyAdvertTimestamp_ = emittedAt;
    return true;
  }

  bool lastFloodAdvertStatus(uint32_t nowMs, FloodAdvertStatus& output) {
    return advertRepeats_.snapshot(nowMs, output);
  }

  bool takeFloodAdvertStatusChanged() {
    return advertRepeats_.takeDirty();
  }

  bool lastNearbyAdvertStatus(NearbyAdvertStatus& output) const {
    return nearbyAdverts_.snapshot(output);
  }

  bool takeNearbyAdvertStatusChanged() {
    return nearbyAdverts_.takeDirty();
  }

  void serviceAdvertRepeat(uint32_t nowMs) {
    (void)advertRepeats_.tick(nowMs);
  }

  void repeatDiagnostics(RepeatDiagnostics& output) const {
    output = repeatDiagnostics_;
    output.syncTurnaroundCompleted = driver_->syncTurnaroundCompleted();
    output.syncTurnaroundStartFailures =
        driver_->syncTurnaroundStartFailures();
    output.syncTurnaroundTimeouts = driver_->syncTurnaroundTimeouts();
    output.rxRearmAttempts = driver_->rxRearmAttempts();
    output.rxRearmRetries = driver_->rxRearmRetries();
    output.rxRearmFailures = driver_->rxRearmFailures();
    output.lastRxStartAttempts =
        driver_->lastCompletedTxRxStartAttempts();
    output.lastRxStartCodeAvailable =
        driver_->lastCompletedTxRxStartCodeAvailable();
    output.lastRxStartCode = driver_->lastCompletedTxRxStartCode();
    output.lastRxStartSoftwareState =
        driver_->lastCompletedTxRxStartSoftwareState();
    output.physicalRxConfirmedAfterTx =
        driver_->physicalRxConfirmedAfterTx();
    output.lastRxChipStatusAvailable =
        driver_->lastCompletedTxRxChipStatusAvailable();
    output.lastRxChipStatus = driver_->lastCompletedTxRxChipStatus();
    output.lastTxDoneToStartReceiveMicrosAvailable =
        driver_->lastTxDoneToStartReceiveMicrosAvailable();
    output.lastTxDoneToStartReceiveMicros =
        driver_->lastTxDoneToStartReceiveMicros();
    output.lastTxDoneToRxConfirmedMicrosAvailable =
        driver_->lastCompletedTxDoneToRxConfirmedMicrosAvailable();
    output.lastTxDoneToRxConfirmedMicros =
        driver_->lastCompletedTxDoneToRxConfirmedMicros();
    driver_->receiveObservability(output);
  }

  void clearRepeatDiagnostics() {
    repeatDiagnostics_ = RepeatDiagnostics{};
    driver_->clearTurnaroundDiagnostics();
  }

  void clearAdvertRepeatTracking() {
    advertRepeats_.clear();
    nearbyAdverts_.clear();
  }

  // A session lock revokes packets that have not completed RF transmission,
  // but it must preserve a direct message's receive-only ACK timer after
  // logTx() has reported Sent.  Accepting that ACK cannot transmit anything.
  void cancelQueuedSends() {
    const ::mesh::Packet* inFlight = currentOutboundPacket();
    if (pendingPacket_ && pendingPacket_ != inFlight) {
      pendingPacket_ = nullptr;
      messaging_->cancelPending();
    }
    if (channelPacket_ && channelPacket_ != inFlight) {
      channelPacket_ = nullptr;
      messaging_->enqueueChannelDelivery(DeliveryState::Cancelled,
                                          channelSlot_, channelTimestamp_);
      channelSlot_ = 0xffU;
      channelTimestamp_ = 0U;
    }
    if (advertPacket_ && advertPacket_ != inFlight) {
      advertPacket_ = nullptr;
      (void)advertRepeats_.markTxFailed(advertTimestamp_);
      advertTimestamp_ = 0U;
    }
    if (nearbyAdvertPacket_ && nearbyAdvertPacket_ != inFlight) {
      nearbyAdvertPacket_ = nullptr;
      (void)nearbyAdverts_.markTxFailed(nearbyAdvertTimestamp_);
      nearbyAdvertTimestamp_ = 0U;
    }
  }

  // Profile replacement, contact removal and messaging reset invalidate even
  // an already-sent direct message's receive context.  Those explicit state
  // changes cancel pre-RF work and mark already-sent direct work unconfirmed.
  void cancelAllSends() {
    pendingPacket_ = nullptr;
    messaging_->cancelPending(true);
    if (channelPacket_) {
      channelPacket_ = nullptr;
      messaging_->enqueueChannelDelivery(DeliveryState::Cancelled,
                                          channelSlot_, channelTimestamp_);
      channelSlot_ = 0xffU;
      channelTimestamp_ = 0U;
    }
    if (advertPacket_) {
      advertPacket_ = nullptr;
      (void)advertRepeats_.markTxFailed(advertTimestamp_);
      advertTimestamp_ = 0U;
    }
    if (nearbyAdvertPacket_) {
      nearbyAdvertPacket_ = nullptr;
      (void)nearbyAdverts_.markTxFailed(nearbyAdvertTimestamp_);
      nearbyAdvertTimestamp_ = 0U;
    }
  }

  bool trackedSendInProgress() const {
    const ::mesh::Packet* inFlight = currentOutboundPacket();
    return inFlight &&
      (inFlight == pendingPacket_ || inFlight == channelPacket_ ||
         inFlight == advertPacket_ || inFlight == nearbyAdvertPacket_);
  }

  TransportStatus sendChannelText(const ChannelEntry& channel,
                                  uint8_t channelSlot,
                                  uint32_t timestamp,
                                  const char* senderName,
                                  const char* text,
                                  bool bindOneShotToPacket = false) {
    if (channelPacket_) return TransportStatus::SendBusy;
    const size_t textBytes = strnlen(text, kChannelOutboundTextBytes + 1U);
    if (textBytes > kChannelOutboundTextBytes) {
      return TransportStatus::TextTooLong;
    }
    uint8_t plaintext[kMeshPlaintextCapacity]{};
    size_t plaintextBytes = 0;
    const TextCodecStatus encoded = encodeChannelTextPayload(
        timestamp, senderName, text, textBytes, plaintext, sizeof(plaintext),
        plaintextBytes);
    if (encoded == TextCodecStatus::TextTooLong) {
      return TransportStatus::TextTooLong;
    }
    if (encoded != TextCodecStatus::Ok) {
      return TransportStatus::InvalidArgument;
    }
    ::mesh::Packet* packet = createGroupDatagram(
        PAYLOAD_TYPE_GRP_TXT, channel.channel, plaintext, plaintextBytes);
    if (!packet) return TransportStatus::PacketPoolFull;
    if (!prepareFloodRoute(packet, channel.regionScope)) {
      releasePacket(packet);
      return TransportStatus::InvalidArgument;
    }
    if (bindOneShotToPacket && !armOneShotForPacket(packet)) {
      releasePacket(packet);
      return TransportStatus::TxLocked;
    }
    channelPacket_ = packet;
    channelSlot_ = channelSlot;
    channelTimestamp_ = timestamp;
    (void)sendFloodRoute(packet, channel.regionScope);
    return TransportStatus::Ok;
  }

 protected:
  ::mesh::DispatcherAction onRecvPacket(::mesh::Packet* packet) override {
    // Keep the accepted protocol surface deliberately small.  MeshCore REQ,
    // CLI, signed text, anon, control, raw, trace and multipart packets are not
    // needed for Kitsu chat and never reach general dispatch.
    if (!packet || packet->getPayloadVer() != PAYLOAD_VER_1) {
      return ACTION_RELEASE;
    }
    switch (packet->getPayloadType()) {
      case PAYLOAD_TYPE_ADVERT:
        if (packet->payload_len < PUB_KEY_SIZE + 4U + SIGNATURE_SIZE ||
            packet->payload_len > PUB_KEY_SIZE + 4U + SIGNATURE_SIZE +
                                      MAX_ADVERT_DATA_SIZE) {
          return ACTION_RELEASE;
        }
        break;
      case PAYLOAD_TYPE_TXT_MSG:
      case PAYLOAD_TYPE_PATH:
        if (packet->payload_len < 2U + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE) {
          return ACTION_RELEASE;
        }
        break;
      case PAYLOAD_TYPE_GRP_TXT:
        if (packet->payload_len < 1U + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE) {
          return ACTION_RELEASE;
        }
        break;
      case PAYLOAD_TYPE_ACK:
        if (packet->payload_len < 4U) return ACTION_RELEASE;
        break;
      default:
        return ACTION_RELEASE;
    }
    return ::mesh::Mesh::onRecvPacket(packet);
  }

  float getAirtimeBudgetFactor() const override {
    return kConservativeAirtimeFactor;
  }

  // A Kitsu is an endpoint, never a flood repeater. Dispatcher otherwise
  // applies its repeater score delay to every inbound flood (up to 32 s),
  // retaining each packet in the ten-entry pool and making burst traffic
  // appear late or disappear when the pool fills. Companion clients consume
  // and release inbound floods immediately.
  int calcRxDelay(float score, uint32_t airTime) const override {
    return endpointFloodReceiveDelayMs(score, airTime);
  }

  bool allowPacketForward(const ::mesh::Packet*) override {
    return false;  // Kitsu is a Client, never a repeater.
  }

  void logRxRaw(float snr, float rssi, const uint8_t raw[], int length) override {
    incrementDiagnostic(repeatDiagnostics_.rawFrames);
    if (!raw || length <= 0) {
      incrementDiagnostic(repeatDiagnostics_.rawRejected);
      return;
    }

    if (static_cast<size_t>(length) == nearby::kWireBytes &&
        raw[0] == nearby::kMagic0 && raw[1] == nearby::kMagic1 &&
        raw[2] == nearby::kProtocolVersion) {
      sink_->captureNearbyRadio(raw, static_cast<size_t>(length), rssi, snr);
      return;
    }

    RepeatWireView wire{};
    if (decodeRepeatWire(raw, static_cast<size_t>(length), wire) !=
        RepeatWireDecodeStatus::Ok) {
      incrementDiagnostic(repeatDiagnostics_.rawRejected);
      return;
    }
    const uint8_t payloadType = wire.payloadType;
    FloodRouteBinding receivedRoute{};
    const bool flood = floodRouteBindingFromWire(wire, receivedRoute);

    // Only the two outbound evidence types need correlation work. The
    // official companion likewise sources its RX log here, before packet-pool
    // allocation and parser/dedup admission.
    if (payloadType != PAYLOAD_TYPE_GRP_TXT &&
        payloadType != PAYLOAD_TYPE_ADVERT) {
      return;
    }
    uint8_t packetHash[kChannelRepeatHashBytes]{};
    uint8_t correlationDigest[kChannelRepeatDigestBytes]{};
    if (!calculateChannelRepeatDigest(payloadType, wire.payload,
                                       wire.payloadBytes,
                                       correlationDigest)) {
      incrementDiagnostic(repeatDiagnostics_.rawRejected);
      return;
    }
    // MeshCore hashes non-TRACE packets over this exact type+payload digest;
    // calculatePacketHash() is its first eight bytes.
    memcpy(packetHash, correlationDigest, sizeof(packetHash));

    if (payloadType == PAYLOAD_TYPE_GRP_TXT && flood &&
        wire.pathCount != 0U) {
      incrementDiagnostic(repeatDiagnostics_.channelForwardCandidates);
      repeatDiagnostics_.lastChannelAvailable = true;
      memcpy(repeatDiagnostics_.lastChannelHash, packetHash,
             sizeof(repeatDiagnostics_.lastChannelHash));
      memset(repeatDiagnostics_.lastPath, 0,
             sizeof(repeatDiagnostics_.lastPath));
      memcpy(repeatDiagnostics_.lastPath, wire.path, wire.pathBytes);
      repeatDiagnostics_.lastPathBytes =
          static_cast<uint8_t>(wire.pathBytes);
      repeatDiagnostics_.lastPathHashSize = wire.pathHashSize;
      repeatDiagnostics_.lastPathCount = wire.pathCount;
      repeatDiagnostics_.lastRssi = rssi;
      repeatDiagnostics_.lastSnr = snr;

      ChannelRepeatObservation observation{};
      const uint8_t* const lastHopToken = wire.lastHopToken();
      const ChannelRepeatObserveResult result =
          messaging_->observeChannelRepeatDetailed(
              payloadType, receivedRoute, wire.pathCount, packetHash,
              correlationDigest, lastHopToken, wire.pathHashSize, millis(),
              observation);
      switch (result) {
        case ChannelRepeatObserveResult::NoHashMatch:
        case ChannelRepeatObserveResult::NotCandidate:
          repeatDiagnostics_.lastResult =
              RepeatDiagnosticResult::NoActiveHash;
          break;
        case ChannelRepeatObserveResult::WireMismatch:
          incrementDiagnostic(repeatDiagnostics_.channelHashMatches);
          incrementDiagnostic(repeatDiagnostics_.channelWireMismatches);
          repeatDiagnostics_.lastResult =
              RepeatDiagnosticResult::WireMismatch;
          break;
        case ChannelRepeatObserveResult::DigestMismatch:
          incrementDiagnostic(repeatDiagnostics_.channelHashMatches);
          incrementDiagnostic(repeatDiagnostics_.channelDigestMismatches);
          repeatDiagnostics_.lastResult =
              RepeatDiagnosticResult::DigestMismatch;
          break;
        case ChannelRepeatObserveResult::Recorded:
          incrementDiagnostic(repeatDiagnostics_.channelHashMatches);
          incrementDiagnostic(repeatDiagnostics_.channelExactMatches);
          incrementDiagnostic(repeatDiagnostics_.channelRecorded);
          repeatDiagnostics_.lastResult = RepeatDiagnosticResult::Recorded;
          break;
        case ChannelRepeatObserveResult::Saturated:
          incrementDiagnostic(repeatDiagnostics_.channelHashMatches);
          incrementDiagnostic(repeatDiagnostics_.channelExactMatches);
          incrementDiagnostic(repeatDiagnostics_.channelSaturated);
          repeatDiagnostics_.lastResult = RepeatDiagnosticResult::Saturated;
          break;
      }
    }
    if (payloadType == PAYLOAD_TYPE_ADVERT && flood &&
        wire.pathCount != 0U) {
      incrementDiagnostic(repeatDiagnostics_.advertForwardCandidates);
      repeatDiagnostics_.lastAdvertAvailable = true;
      memcpy(repeatDiagnostics_.lastAdvertHash, packetHash,
             sizeof(repeatDiagnostics_.lastAdvertHash));
      memset(repeatDiagnostics_.lastAdvertPath, 0,
             sizeof(repeatDiagnostics_.lastAdvertPath));
      memcpy(repeatDiagnostics_.lastAdvertPath, wire.path, wire.pathBytes);
      repeatDiagnostics_.lastAdvertPathBytes =
          static_cast<uint8_t>(wire.pathBytes);
      repeatDiagnostics_.lastAdvertPathHashSize = wire.pathHashSize;
      repeatDiagnostics_.lastAdvertPathCount = wire.pathCount;
      repeatDiagnostics_.lastAdvertRssi = rssi;
      repeatDiagnostics_.lastAdvertSnr = snr;
      const AdvertRepeatObserveResult advertResult =
          advertRepeats_.observeDetailed(
              payloadType, receivedRoute, wire.pathCount, packetHash,
              correlationDigest, wire.lastHopToken(), wire.pathHashSize,
              millis());
      switch (advertResult) {
        case AdvertRepeatObserveResult::NoHashMatch:
        case AdvertRepeatObserveResult::NotCandidate:
          repeatDiagnostics_.lastAdvertResult =
              RepeatDiagnosticResult::NoActiveHash;
          break;
        case AdvertRepeatObserveResult::WireMismatch:
          incrementDiagnostic(repeatDiagnostics_.advertHashMatches);
          incrementDiagnostic(repeatDiagnostics_.advertWireMismatches);
          repeatDiagnostics_.lastAdvertResult =
              RepeatDiagnosticResult::WireMismatch;
          break;
        case AdvertRepeatObserveResult::DigestMismatch:
          incrementDiagnostic(repeatDiagnostics_.advertHashMatches);
          incrementDiagnostic(repeatDiagnostics_.advertDigestMismatches);
          repeatDiagnostics_.lastAdvertResult =
              RepeatDiagnosticResult::DigestMismatch;
          break;
        case AdvertRepeatObserveResult::Recorded:
          incrementDiagnostic(repeatDiagnostics_.advertHashMatches);
          incrementDiagnostic(repeatDiagnostics_.advertExactMatches);
          incrementDiagnostic(repeatDiagnostics_.advertRecorded);
          repeatDiagnostics_.lastAdvertResult =
              RepeatDiagnosticResult::Recorded;
          break;
        case AdvertRepeatObserveResult::Saturated:
          incrementDiagnostic(repeatDiagnostics_.advertHashMatches);
          incrementDiagnostic(repeatDiagnostics_.advertExactMatches);
          incrementDiagnostic(repeatDiagnostics_.advertSaturated);
          repeatDiagnostics_.lastAdvertResult =
              RepeatDiagnosticResult::Saturated;
          break;
      }
    }
  }

  void logRx(::mesh::Packet* packet, int, float) override {
    if (!packet) return;
    incrementDiagnostic(repeatDiagnostics_.parsedFrames);
  }

  void logTx(::mesh::Packet* packet, int) override {
    incrementDiagnostic(repeatDiagnostics_.txDoneFrames);
    // Use only the proof latched by startSendRaw's immediate rearm. A
    // second getStatus()/startReceive here could overwrite that evidence or
    // disturb a fast RX_DONE already waiting for checkRecv().
    repeatDiagnostics_.physicalRxConfirmedAfterTx =
        driver_->physicalRxConfirmedAfterTx();
    repeatDiagnostics_.lastRxChipStatusAvailable =
        driver_->lastCompletedTxRxChipStatusAvailable();
    repeatDiagnostics_.lastRxChipStatus =
        driver_->lastCompletedTxRxChipStatus();
    if (driver_->isInRecvMode()) {
      incrementDiagnostic(repeatDiagnostics_.rxReadyAfterTx);
    }
    if (packet && packet->isRouteFlood()) {
      const bool scoped = packet->hasTransportCodes();
      incrementDiagnostic(scoped
                              ? repeatDiagnostics_.scopedFloodTxDoneFrames
                              : repeatDiagnostics_.unscopedFloodTxDoneFrames);
      repeatDiagnostics_.lastFloodTxAvailable = true;
      repeatDiagnostics_.lastFloodTxScoped = scoped;
      repeatDiagnostics_.lastFloodTxPayloadType = packet->getPayloadType();
      repeatDiagnostics_.lastFloodTxTransportCode =
          scoped ? packet->transport_codes[0] : 0U;
    }
    if (packet == pendingPacket_) {
      pendingPacket_ = nullptr;
      messaging_->markPendingSent();
    }
    if (packet == channelPacket_) {
      channelPacket_ = nullptr;
      uint8_t packetHash[kChannelRepeatHashBytes]{};
      uint8_t correlationDigest[kChannelRepeatDigestBytes]{};
      FloodRouteBinding sentRoute{};
      packet->calculatePacketHash(packetHash);
      const uint8_t payloadType = packet->getPayloadType();
      if (captureFloodRoute(packet, sentRoute) &&
          calculateChannelRepeatDigest(payloadType, packet->payload,
                                       packet->payload_len,
                                       correlationDigest)) {
        messaging_->markChannelSent(
            channelSlot_, channelTimestamp_, payloadType,
            sentRoute, packetHash, correlationDigest, millis());
      } else {
        messaging_->enqueueChannelDelivery(DeliveryState::Sent, channelSlot_,
                                            channelTimestamp_);
      }
      channelSlot_ = 0xffU;
      channelTimestamp_ = 0U;
    }
    if (packet == advertPacket_) {
      advertPacket_ = nullptr;
      uint8_t packetHash[kAdvertRepeatHashBytes]{};
      uint8_t correlationDigest[kAdvertRepeatDigestBytes]{};
      FloodRouteBinding sentRoute{};
      packet->calculatePacketHash(packetHash);
      const uint8_t payloadType = packet->getPayloadType();
      bool markedSent = false;
      if (captureFloodRoute(packet, sentRoute) &&
          calculateChannelRepeatDigest(payloadType, packet->payload,
                                       packet->payload_len,
                                       correlationDigest)) {
        markedSent = advertRepeats_.markSent(
            payloadType, sentRoute, packetHash, correlationDigest,
            advertTimestamp_, millis());
      }
      // A valid tracked advert cannot reach this branch, but never leave an
      // app-visible lifecycle stuck at queued if correlation construction or
      // an upstream packet invariant unexpectedly changes.
      if (!markedSent) {
        (void)advertRepeats_.markTxFailed(advertTimestamp_);
      }
      advertTimestamp_ = 0U;
    }
    if (packet == nearbyAdvertPacket_) {
      nearbyAdvertPacket_ = nullptr;
      (void)nearbyAdverts_.markSent(nearbyAdvertTimestamp_);
      nearbyAdvertTimestamp_ = 0U;
    }
  }

  void logTxFail(::mesh::Packet* packet, int) override {
    incrementDiagnostic(repeatDiagnostics_.txFailedFrames);
    if (packet == pendingPacket_) {
      pendingPacket_ = nullptr;
      messaging_->markPendingTxFailed();
    }
    if (packet == channelPacket_) {
      channelPacket_ = nullptr;
      messaging_->enqueueChannelDelivery(DeliveryState::TxFailed,
                                          channelSlot_, channelTimestamp_);
      channelSlot_ = 0xffU;
      channelTimestamp_ = 0U;
    }
    if (packet == advertPacket_) {
      advertPacket_ = nullptr;
      (void)advertRepeats_.markTxFailed(advertTimestamp_);
      advertTimestamp_ = 0U;
    }
    if (packet == nearbyAdvertPacket_) {
      nearbyAdvertPacket_ = nullptr;
      (void)nearbyAdverts_.markTxFailed(nearbyAdvertTimestamp_);
      nearbyAdvertTimestamp_ = 0U;
    }
  }

  void onAdvertRecv(::mesh::Packet* packet, const ::mesh::Identity& identity,
                    uint32_t timestamp, const uint8_t* appData,
                    size_t appDataBytes) override {
    sink_->captureAdvert(packet, identity, timestamp, appData, appDataBytes);
  }

  int searchPeersByHash(const uint8_t* hash) override {
    matchingCount_ = 0;
    const size_t count = messaging_->contactCount();
    for (size_t ordinal = 0; ordinal < count && matchingCount_ < 4U;
         ++ordinal) {
      ContactEntry* contact = messaging_->contactAt(ordinal);
      if (contact && contact->publicKey[0] == hash[0]) {
        matching_[matchingCount_++] = contact;
      }
    }
    return matchingCount_;
  }

  void getPeerSharedSecret(uint8_t* destination, int peerIndex) override {
    if (!destination || peerIndex < 0 ||
        peerIndex >= static_cast<int>(matchingCount_)) {
      return;
    }
    self_id.calcSharedSecret(destination, matching_[peerIndex]->publicKey);
  }

  void onPeerDataRecv(::mesh::Packet* packet, uint8_t type, int senderIndex,
                      const uint8_t* secret, uint8_t* data,
                      size_t dataBytes) override {
    if (!packet || !secret || !data || type != PAYLOAD_TYPE_TXT_MSG ||
        senderIndex < 0 || senderIndex >= static_cast<int>(matchingCount_)) {
      return;
    }
    DecodedTextPayload decoded{};
    if (decodeDirectTextPayload(data, dataBytes, decoded) !=
        TextCodecStatus::Ok) {
      return;
    }
    ContactEntry& sender = *matching_[senderIndex];
    ReceivedMessage event{};
    event.kind = MessageKind::Direct;
    event.route = packet->isRouteDirect() ? MessageRoute::Direct
                                           : MessageRoute::Flood;
    event.senderAuthenticated = true;
    event.timestamp = decoded.timestamp;
    memcpy(event.publicKey, sender.publicKey, PUB_KEY_SIZE);
    memcpy(event.senderName, sender.name, sizeof(event.senderName));
    memcpy(event.text, decoded.text, decoded.textBytes + 1U);
    event.hopCount = packet->getPathHashCount();
    event.rssi = _radio->getLastRSSI();
    event.snr = packet->getSNR();
    messaging_->enqueueMessage(event);

    uint8_t ack[6]{};
    ::mesh::Utils::sha256(ack, 4, data, 5U + decoded.textBytes,
                          sender.publicKey, PUB_KEY_SIZE);
    const size_t terminator = 5U + decoded.textBytes;
    if (terminator + 1U < dataBytes) ack[4] = data[terminator + 1U];
    getRNG()->random(&ack[5], 1U);

    if (packet->isRouteFlood()) {
      ::mesh::Identity destination(sender.publicKey);
      ::mesh::Packet* path = createPathReturn(
          destination, secret, packet->path, packet->path_len,
          PAYLOAD_TYPE_ACK, ack, sizeof(ack));
      if (authorizeAuthenticatedReply(path)) {
        (void)sendFloodRoute(path, ChannelRegionScope::Legacy,
                             kTextAckDelayMillis);
      }
    } else {
      sendAckTo(sender, ack, sizeof(ack));
    }
  }

  bool onPeerPathRecv(::mesh::Packet*, int senderIndex, const uint8_t*,
                      uint8_t* path, uint8_t pathLen, uint8_t extraType,
                      uint8_t* extra, uint8_t extraBytes) override {
    if (senderIndex < 0 || senderIndex >= static_cast<int>(matchingCount_) ||
        !path || !validPathEncoding(pathLen) || pathLen == kUnknownPath) {
      return false;
    }
    // createPathReturn pads plaintext to AES blocks.  A no-extra return uses
    // masked type 0x0F plus four random bytes (4..19 bytes after the type); a
    // text delivery return uses ACK plus six bytes (6..21 after the type).
    // Reject every other PATH application and impossible padded remainder.
    const bool noExtra = extraType == 0x0fU &&
        extraBytes >= 4U && extraBytes <= 19U;
    const bool deliveryAck = extraType == PAYLOAD_TYPE_ACK && extra &&
        extraBytes >= 6U && extraBytes <= 21U;
    if (!noExtra && !deliveryAck) return false;
    messaging_->updatePath(*matching_[senderIndex], path, pathLen);
    if (deliveryAck) {
      messaging_->acceptAck(extra, extraBytes, true,
                            static_cast<uint8_t>(pathLen & 63U));
    }
    return gate_->allowsTransmit(*settings_);
  }

  void onAckRecv(::mesh::Packet* packet, uint32_t ack) override {
    if (messaging_->acceptAck(reinterpret_cast<const uint8_t*>(&ack), 4U) &&
        packet) {
      packet->markDoNotRetransmit();
    }
  }

  int searchChannelsByHash(const uint8_t* hash,
                           ::mesh::GroupChannel output[],
                           int maxMatches) override {
    if (!hash || !output || maxMatches <= 0) return 0;
    int matches = 0;
    for (uint8_t slot = 0; slot < kMeshChannelCapacity && matches < maxMatches;
         ++slot) {
      const ChannelEntry* channel = messaging_->channel(slot);
      if (channel && channel->channel.hash[0] == hash[0]) {
        output[matches++] = channel->channel;
      }
    }
    return matches;
  }

  void onGroupDataRecv(::mesh::Packet* packet, uint8_t type,
                       const ::mesh::GroupChannel& channel, uint8_t* data,
                       size_t dataBytes) override {
    // Kitsu channel messages are flood-only.  Do not journal a decryptable
    // group payload delivered with direct routing: it is outside the wire
    // contract and has no honest channel path semantics for the companion.
    if (!packet || !data || type != PAYLOAD_TYPE_GRP_TXT ||
        !packet->isRouteFlood()) {
      return;
    }
    const int slot = messaging_->findChannelSlot(channel);
    if (slot < 0) return;
    DecodedTextPayload decoded{};
    if (decodeChannelTextPayload(data, dataBytes, decoded) !=
        TextCodecStatus::Ok) {
      return;
    }

    ReceivedMessage event{};
    event.kind = MessageKind::Channel;
    event.route = MessageRoute::Flood;
    event.senderAuthenticated = false;
    event.timestamp = decoded.timestamp;
    event.channelSlot = static_cast<uint8_t>(slot);
    event.hopCount = packet->getPathHashCount();
    event.rssi = _radio->getLastRSSI();
    event.snr = packet->getSNR();

    const char* separator = strstr(decoded.text, ": ");
    if (separator) {
      const size_t nameBytes = static_cast<size_t>(separator - decoded.text);
      sanitizeUtf8Name(event.senderName, sizeof(event.senderName),
                       reinterpret_cast<const uint8_t*>(decoded.text),
                       nameBytes);
      const char* message = separator + 2;
      const size_t messageBytes = strnlen(message, kMeshTextBytes + 1U);
      memcpy(event.text, message, messageBytes);
      event.text[messageBytes] = '\0';
    } else {
      memcpy(event.text, decoded.text, decoded.textBytes + 1U);
    }
    messaging_->enqueueMessage(event);
  }

 private:
  AdvertSink* sink_;
  MessagingState* messaging_;
  const TxGate* gate_;
  const Settings* settings_;
  GatedCustomSX1262Wrapper* driver_;
  ContactEntry* matching_[4]{};
  uint8_t matchingCount_ = 0;
  ::mesh::Packet* pendingPacket_ = nullptr;
  ::mesh::Packet* channelPacket_ = nullptr;
  uint8_t channelSlot_ = 0xffU;
  uint32_t channelTimestamp_ = 0U;
  ::mesh::Packet* advertPacket_ = nullptr;
  uint32_t advertTimestamp_ = 0U;
  AdvertRepeatTracker advertRepeats_{};
  ::mesh::Packet* nearbyAdvertPacket_ = nullptr;
  uint32_t nearbyAdvertTimestamp_ = 0U;
  NearbyAdvertTracker nearbyAdverts_{};
  RepeatDiagnostics repeatDiagnostics_{};

  static void incrementDiagnostic(uint32_t& value) {
    if (value != UINT32_MAX) ++value;
  }

  bool armOneShotForPacket(::mesh::Packet* packet) {
    if (!packet) return false;
    uint8_t expectedWire[MAX_TRANS_UNIT]{};
    const uint8_t expectedWireBytes = packet->writeTo(expectedWire);
    const bool armed = expectedWireBytes != 0U &&
        driver_->armOneShotForPacket(*settings_, true, expectedWire,
                                     expectedWireBytes);
    memset(expectedWire, 0, sizeof(expectedWire));
    return armed;
  }

  bool authorizeAuthenticatedReply(::mesh::Packet* packet) {
    if (!packet) return false;
    // Under a locked session, the just-created reply must be the only packet
    // eligible to consume the permit. Arduino dispatch is single-threaded, so
    // this check plus immediate queueing binds the next radio start to it.
    if (!gate_->allowsTransmit(*settings_) &&
        (currentOutboundPacket() || _mgr->getOutboundTotal() != 0)) {
      releasePacket(packet);
      return false;
    }
    if (!driver_->armAuthenticatedReply(*settings_)) {
      releasePacket(packet);
      return false;
    }
    return true;
  }

  void sendAckTo(const ContactEntry& destination, const uint8_t* ack,
                 uint8_t ackBytes) {
    ::mesh::Packet* packet = createAck(ack, ackBytes);
    if (!authorizeAuthenticatedReply(packet)) return;
    if (destination.outPathLen == kUnknownPath) {
      (void)sendFloodRoute(packet, ChannelRegionScope::Legacy,
                           kTextAckDelayMillis);
    } else {
      sendDirect(packet, destination.outPath, destination.outPathLen,
                 kTextAckDelayMillis);
    }
  }
};

bool writeIdentityRecord(const ::mesh::LocalIdentity& identity) {
  uint8_t record[kIdentityRecordBytes]{};
  memcpy(record, kIdentityMagic, sizeof(kIdentityMagic));
  writeLe16(record + 4, kIdentitySchema);
  writeLe16(record + 6, static_cast<uint16_t>(kIdentityRecordBytes));
  ::mesh::LocalIdentity copy = identity;
  if (copy.writeTo(record + kIdentityOffsetData, kIdentityBytes) !=
      kIdentityBytes) {
    return false;
  }
  writeLe32(record + kIdentityOffsetCrc,
            crc32WithZeroedIdentityCrc(record, sizeof(record)));

  Preferences preferences;
  if (!preferences.begin(kIdentityNamespace, false)) return false;
  const size_t written =
      preferences.putBytes(kIdentityKey, record, sizeof(record));
  uint8_t verification[kIdentityRecordBytes]{};
  const bool verified = written == sizeof(record) &&
      preferences.getBytesLength(kIdentityKey) == sizeof(verification) &&
      preferences.getBytes(kIdentityKey, verification,
                           sizeof(verification)) == sizeof(verification) &&
      memcmp(record, verification, sizeof(record)) == 0;
  preferences.end();
  return verified;
}

enum class IdentityLoadResult : uint8_t { Loaded, Missing, Invalid, Error };

IdentityLoadResult loadIdentityRecord(::mesh::LocalIdentity& output) {
  Preferences preferences;
  // Open read/write so a first boot can create the namespace.  No bytes are
  // written on the load path itself.
  if (!preferences.begin(kIdentityNamespace, false)) {
    return IdentityLoadResult::Error;
  }
  const size_t storedBytes = preferences.getBytesLength(kIdentityKey);
  if (storedBytes == 0) {
    preferences.end();
    return IdentityLoadResult::Missing;
  }
  if (storedBytes != kIdentityRecordBytes) {
    preferences.end();
    return IdentityLoadResult::Invalid;
  }
  uint8_t record[kIdentityRecordBytes]{};
  const size_t read =
      preferences.getBytes(kIdentityKey, record, sizeof(record));
  preferences.end();
  if (read != sizeof(record) ||
      memcmp(record, kIdentityMagic, sizeof(kIdentityMagic)) != 0 ||
      readLe16(record + 4) != kIdentitySchema ||
      readLe16(record + 6) != kIdentityRecordBytes ||
      readLe32(record + kIdentityOffsetCrc) !=
          crc32WithZeroedIdentityCrc(record, sizeof(record))) {
    return IdentityLoadResult::Invalid;
  }

  const uint8_t* privateKey = record + kIdentityOffsetData;
  const uint8_t* storedPublicKey = privateKey + PRV_KEY_SIZE;
  if (!::mesh::LocalIdentity::validatePrivateKey(privateKey)) {
    return IdentityLoadResult::Invalid;
  }
  ::mesh::LocalIdentity candidate;
  candidate.readFrom(privateKey, PRV_KEY_SIZE);
  if (memcmp(candidate.pub_key, storedPublicKey, PUB_KEY_SIZE) != 0) {
    return IdentityLoadResult::Invalid;
  }
  output = candidate;
  return IdentityLoadResult::Loaded;
}

void bytesToUpperHex(char* destination, const uint8_t* source,
                     size_t sourceBytes) {
  constexpr char digits[] = "0123456789ABCDEF";
  for (size_t index = 0; index < sourceBytes; ++index) {
    destination[index * 2U] = digits[source[index] >> 4U];
    destination[index * 2U + 1U] = digits[source[index] & 0x0fU];
  }
  destination[sourceBytes * 2U] = '\0';
}

bool sameActiveProfile(const Settings& a, const Settings& b) {
  return a.enabled == b.enabled && sameRadioProfile(a.radio, b.radio);
}

}  // namespace

struct KitsuMeshTransport::Impl final : public AdvertSink {
  KitsuBoard board{};
  TxGate txGate{};
  Settings settings = defaultSettings();
  CustomSX1262 physical{new Module(kLoraCs, kLoraDio1, kLoraReset,
                                    kLoraBusy)};
  GatedCustomSX1262Wrapper driver{physical, board, txGate, settings};
  ArduinoMillis millisClock{};
  EspRng rng{};
  SessionRtc rtc{};
  StaticPoolPacketManager packets{kPacketPoolSize};
  SimpleMeshTables tables{};
  MessagingState messaging{};
  KitsuClient client{driver, millisClock, rng, rtc, packets, tables, *this,
                     messaging, txGate, settings};

  ClientIdentity advertisedIdentity{};
  bool identityReady = false;
  bool hardwareInitialized = false;
  bool active = false;
  int16_t radioCode = RADIOLIB_ERR_UNKNOWN;
  ReceivedAdvert advertQueue[kAdvertQueueSize]{};
  uint8_t advertRead = 0;
  uint8_t advertWrite = 0;
  uint8_t advertCount = 0;
  uint32_t receivedAdverts = 0;
  uint32_t droppedAdverts = 0;
  NearbyRadioFrame nearbyRadioQueue[kNearbyRadioQueueSize]{};
  uint8_t nearbyRadioRead = 0U;
  uint8_t nearbyRadioWrite = 0U;
  uint8_t nearbyRadioCount = 0U;
  bool advertCooldownStarted = false;
  uint32_t lastAdvertQueuedAtMillis = 0U;
  bool nearbyPresenceCooldownStarted = false;
  uint32_t lastNearbyPresenceTxAt = 0U;
  bool nearbyActionCooldownStarted = false;
  uint32_t lastNearbyActionTxAt = 0U;
  bool meshLoopSeen = false;
  uint32_t lastMeshLoopAtMs = 0U;
  uint32_t maxMeshLoopGapMs = 0U;

  bool ensureIdentity() {
    const IdentityLoadResult loaded = loadIdentityRecord(client.self_id);
    if (loaded == IdentityLoadResult::Loaded) {
      identityReady = true;
      return true;
    }
    if (loaded != IdentityLoadResult::Missing) return false;

    ::mesh::LocalIdentity candidate(&rng);
    for (uint8_t attempt = 0;
         attempt < 10U &&
         (candidate.pub_key[0] == 0x00 || candidate.pub_key[0] == 0xff);
         ++attempt) {
      candidate = ::mesh::LocalIdentity(&rng);
    }
    if (candidate.pub_key[0] == 0x00 || candidate.pub_key[0] == 0xff ||
        !writeIdentityRecord(candidate)) {
      return false;
    }
    client.self_id = candidate;
    identityReady = true;
    return true;
  }

  void clearOutboundQueue() {
    while (packets.getOutboundTotal() > 0) {
      ::mesh::Packet* packet = packets.removeOutboundByIdx(0);
      if (packet) packets.free(packet);
    }
  }

  void clearPacketQueues() {
    clearOutboundQueue();
    // MeshCore delays weak flood packets by at most 32 seconds.  A time far
    // enough in the future drains those stale packets before a profile swap.
    const uint32_t future = millis() + 60000UL;
    for (;;) {
      ::mesh::Packet* packet = packets.getNextInbound(future);
      if (!packet) break;
      packets.free(packet);
    }
  }

  TransportStatus configureRadio(const Settings& next) {
    txGate.lock();
    driver.revokeOneShot();
    client.cancelQueuedSends();
    clearOutboundQueue();

    // An asynchronous frame that already owns the radio cannot be recalled.
    // Leave its tracker intact and reject the profile mutation until the next
    // command, after logTx/logTxFail has reported the honest outcome.
    if (active && !driver.isInRecvMode()) {
      return TransportStatus::InvalidArgument;
    }
    client.cancelAllSends();
    // Echo correlation is profile-local. A successful radio/profile reset
    // must not let a late frame from the old profile mutate a recent row.
    messaging.closeChannelRepeatTracking();
    client.clearAdvertRepeatTracking();
    clearPacketQueues();
    if (hardwareInitialized) physical.standby();
    active = false;
    settings = next;

    if (!next.enabled) {
      if (hardwareInitialized) physical.sleep(false);
      radioCode = RADIOLIB_ERR_NONE;
      return TransportStatus::Disabled;
    }
    if (!next.radio.selected()) return TransportStatus::NoProfile;

    SPI.begin(kLoraSck, kLoraMiso, kLoraMosi, kLoraCs);
    radioCode = physical.begin(
        static_cast<float>(next.radio.frequencyHz) / 1000000.0f,
        static_cast<float>(next.radio.bandwidthHz) / 1000.0f,
        next.radio.spreadingFactor, next.radio.codingRate,
        next.radio.syncWord, next.radio.txPowerDbm,
        next.radio.preambleSymbols, kTcxoVoltage);
    if (radioCode != RADIOLIB_ERR_NONE) return TransportStatus::RadioInitFailed;
    hardwareInitialized = true;

    const int16_t crc = physical.setCRC(1);
    const int16_t current = physical.setCurrentLimit(140.0f);
    const int16_t rfSwitch = physical.setDio2AsRfSwitch(true);
    const int16_t boosted = physical.setRxBoostedGainMode(true);
    if (crc != RADIOLIB_ERR_NONE || current != RADIOLIB_ERR_NONE ||
        rfSwitch != RADIOLIB_ERR_NONE || boosted != RADIOLIB_ERR_NONE) {
      radioCode = crc != RADIOLIB_ERR_NONE
                      ? crc
                      : current != RADIOLIB_ERR_NONE
                            ? current
                            : rfSwitch != RADIOLIB_ERR_NONE ? rfSwitch
                                                            : boosted;
      physical.sleep(false);
      return TransportStatus::RadioInitFailed;
    }

    client.begin();
    if (!kitsuRadioDio1Claimed()) {
      // Never silently run a receiver without completion signalling.  This
      // also makes a future Arduino/RadioLib signature change fail closed
      // instead of reintroducing the unsafe ipc1 allocation path.
      radioCode = RADIOLIB_ERR_UNKNOWN;
      physical.sleep(false);
      return TransportStatus::RadioInitFailed;
    }
    // v1.17.1's wrapper also derives SX1262 receive deadlines here.  Direct
    // register setters alone leave the generic 66 ms preamble window active;
    // UK/EU Narrow SF8 with its 32-symbol runtime preamble needs ~182 ms.
    driver.setParams(
        static_cast<float>(next.radio.frequencyHz) / 1000000.0f,
        static_cast<float>(next.radio.bandwidthHz) / 1000.0f,
        next.radio.spreadingFactor, next.radio.codingRate);
    driver.setTxPower(next.radio.txPowerDbm);
    client.loop();  // Enter RX immediately; no transmission is queued.
    active = true;
    return TransportStatus::Ok;
  }

  TransportStatus makeAdvert(const Settings& requested,
                             const CurrentLocationOnce& current,
                             ::mesh::Packet*& output) {
    output = nullptr;
    if (!identityReady) return TransportStatus::IdentityStorageFailed;
    if (!rtc.valid()) return TransportStatus::TimeUnset;
    if (validateSettings(requested) != Status::Ok) {
      return TransportStatus::InvalidSettings;
    }

    AdvertLocation location{};
    const bool includeLocation = selectAdvertLocation(requested, current,
                                                       location);
    if (requested.locationMode == LocationMode::CurrentOnce &&
        !includeLocation) {
      return TransportStatus::LocationUnavailable;
    }

    uint8_t appData[MAX_ADVERT_DATA_SIZE]{};
    size_t appBytes = 1;
    appData[0] = ADV_TYPE_CHAT;
    if (includeLocation) {
      appData[0] |= ADV_LATLON_MASK;
      memcpy(appData + appBytes, &location.coordinates.latitudeE6, 4);
      appBytes += 4;
      memcpy(appData + appBytes, &location.coordinates.longitudeE6, 4);
      appBytes += 4;
    }
    appData[0] |= ADV_NAME_MASK;
    const size_t remaining = MAX_ADVERT_DATA_SIZE - appBytes;
    const size_t nameBytes = strnlen(advertisedIdentity.advertisedName,
                                     remaining);
    memcpy(appData + appBytes, advertisedIdentity.advertisedName, nameBytes);
    appBytes += nameBytes;

    output = client.createAdvert(client.self_id, appData, appBytes);
    return output ? TransportStatus::Ok : TransportStatus::PacketPoolFull;
  }

  void captureAdvert(::mesh::Packet* packet,
                     const ::mesh::Identity& identity,
                     uint32_t timestamp, const uint8_t* appData,
                     size_t appDataBytes) override {
    if (!appData || appDataBytes < 1U || appDataBytes > MAX_ADVERT_DATA_SIZE) {
      return;
    }

    ReceivedAdvert event{};
    event.type = appData[0] & 0x0fU;
    event.timestamp = timestamp;
    memcpy(event.publicKey, identity.pub_key, sizeof(event.publicKey));
    memcpy(event.publicKeyPrefix, identity.pub_key,
           sizeof(event.publicKeyPrefix));
    event.rssi = driver.getLastRSSI();
    event.snr = packet ? packet->getSNR() : 0.0f;

    size_t offset = 1;
    if ((appData[0] & ADV_LATLON_MASK) != 0) {
      if (offset + 8U > appDataBytes) return;
      memcpy(&event.location.latitudeE6, appData + offset, 4);
      offset += 4;
      memcpy(&event.location.longitudeE6, appData + offset, 4);
      offset += 4;
      if (validateCoordinates(event.location) != Status::Ok) return;
      event.hasLocation = true;
    }
    if ((appData[0] & ADV_FEAT1_MASK) != 0) {
      if (offset + 2U > appDataBytes) return;
      offset += 2;
    }
    if ((appData[0] & ADV_FEAT2_MASK) != 0) {
      if (offset + 2U > appDataBytes) return;
      offset += 2;
    }
    if ((appData[0] & ADV_NAME_MASK) != 0) {
      const size_t available = appDataBytes - offset;
      sanitizeUtf8Name(event.name, sizeof(event.name), appData + offset,
                       available);
    }
    event.kitsuNamed = strstr(event.name, "Kitsu KT") != nullptr;
    if (event.name[0] != '\0') {
      messaging.learnAdvert(identity.pub_key, event.name, event.type,
                            timestamp);
    }

    if (advertCount == kAdvertQueueSize) {
      // Preserve the newest view of a busy mesh while making loss explicit.
      advertRead = static_cast<uint8_t>((advertRead + 1U) % kAdvertQueueSize);
      --advertCount;
      ++droppedAdverts;
    }
    advertQueue[advertWrite] = event;
    advertWrite = static_cast<uint8_t>((advertWrite + 1U) % kAdvertQueueSize);
    ++advertCount;
    ++receivedAdverts;
  }

  void captureNearbyRadio(const uint8_t* bytes, size_t byteCount,
                          float rssi, float snr) override {
    nearby::Packet packet{};
    if (!bytes || byteCount > kNearbyRadioFrameBytes ||
        nearby::decode(bytes, byteCount, packet) != nearby::Status::Ok) {
      return;
    }
    if (nearbyRadioCount == kNearbyRadioQueueSize) {
      nearbyRadioRead = static_cast<uint8_t>(
          (nearbyRadioRead + 1U) % kNearbyRadioQueueSize);
      --nearbyRadioCount;
    }
    NearbyRadioFrame& frame = nearbyRadioQueue[nearbyRadioWrite];
    frame = NearbyRadioFrame{};
    memcpy(frame.bytes, bytes, byteCount);
    frame.length = static_cast<uint8_t>(byteCount);
    frame.rssi = rssi;
    frame.snr = snr;
    nearbyRadioWrite = static_cast<uint8_t>(
        (nearbyRadioWrite + 1U) % kNearbyRadioQueueSize);
    ++nearbyRadioCount;
  }
};

const char* transportStatusName(TransportStatus status) {
  switch (status) {
    case TransportStatus::Ok: return "ok";
    case TransportStatus::Disabled: return "disabled";
    case TransportStatus::NoProfile: return "no_profile";
    case TransportStatus::InvalidSettings: return "invalid_settings";
    case TransportStatus::InvalidIdentity: return "invalid_identity";
    case TransportStatus::IdentityStorageFailed:
      return "identity_storage_failed";
    case TransportStatus::RadioInitFailed: return "radio_init_failed";
    case TransportStatus::TimeUnset: return "time_unset";
    case TransportStatus::InvalidTime: return "invalid_time";
    case TransportStatus::LocationUnavailable:
      return "location_unavailable";
    case TransportStatus::TxLocked: return "tx_locked";
    case TransportStatus::PacketPoolFull: return "packet_pool_full";
    case TransportStatus::OutputTooSmall: return "output_too_small";
    case TransportStatus::InvalidArgument: return "invalid_argument";
    case TransportStatus::ContactNotFound: return "contact_not_found";
    case TransportStatus::ContactNotClient: return "contact_not_client";
    case TransportStatus::ContactTableFull: return "contact_table_full";
    case TransportStatus::ChannelNotFound: return "channel_not_found";
    case TransportStatus::TextTooLong: return "text_too_long";
    case TransportStatus::SendBusy: return "send_busy";
    case TransportStatus::MessagingStorageFailed:
      return "messaging_storage_failed";
    case TransportStatus::AdvertiseCooldown:
      return "advertise_cooldown";
  }
  return "unknown";
}

const char* messagingStorageWriteResultName(
    MessagingStorageWriteResult result) {
  switch (result) {
    case MessagingStorageWriteResult::NotAttempted: return "not_attempted";
    case MessagingStorageWriteResult::Saved: return "saved";
    case MessagingStorageWriteResult::OpenFailed: return "open_failed";
    case MessagingStorageWriteResult::ClearFailed: return "clear_failed";
    case MessagingStorageWriteResult::WriteFailed: return "write_failed";
    case MessagingStorageWriteResult::VerifyFailed: return "verify_failed";
    case MessagingStorageWriteResult::Ambiguous: return "ambiguous";
  }
  return "unknown";
}

const char* messagingStorageReasonName(MessagingStorageReason reason) {
  switch (reason) {
    case MessagingStorageReason::Ready: return "ready";
    case MessagingStorageReason::LegacyMigrationPending:
      return "legacy_migration_pending";
    case MessagingStorageReason::State2InvalidLegacyUsable:
      return "state2_invalid_legacy_usable";
    case MessagingStorageReason::CompactPeerInvalid:
      return "compact_peer_invalid";
    case MessagingStorageReason::CleanupPending: return "cleanup_pending";
    case MessagingStorageReason::FreshInitializationPending:
      return "fresh_initialization_pending";
    case MessagingStorageReason::NamespaceOpenFailed:
      return "namespace_open_failed";
    case MessagingStorageReason::ReadFailed: return "read_failed";
    case MessagingStorageReason::MissingRecord: return "missing_record";
    case MessagingStorageReason::OrphanedLegacyRecord:
      return "orphaned_legacy_record";
    case MessagingStorageReason::State2Invalid: return "state2_invalid";
    case MessagingStorageReason::InvalidRecord: return "invalid_record";
    case MessagingStorageReason::WriteFailed: return "write_failed";
    case MessagingStorageReason::VerifyFailed: return "verify_failed";
    case MessagingStorageReason::CommitAmbiguous: return "commit_ambiguous";
  }
  return "unknown";
}

KitsuMeshTransport::KitsuMeshTransport() : impl_(new Impl()) {}

KitsuMeshTransport::~KitsuMeshTransport() {
  delete impl_;
  impl_ = nullptr;
}

TransportStatus KitsuMeshTransport::begin(const Settings& settings,
                                          const ClientIdentity& identity) {
  if (!validShortUid(identity.shortUid) ||
      identity.role != Role::Client || identity.advertisedName[0] == '\0') {
    return TransportStatus::InvalidIdentity;
  }
  if (validateSettings(settings) != Status::Ok) {
    return TransportStatus::InvalidSettings;
  }
  impl_->advertisedIdentity = identity;
  if (!impl_->ensureIdentity()) {
    return TransportStatus::IdentityStorageFailed;
  }
  // Messaging storage is an optional capability: corruption must not stop
  // passive adverts/RX.  Operations fail closed until an explicit app reset.
  impl_->messaging.begin();
  return impl_->configureRadio(settings);
}

TransportStatus KitsuMeshTransport::applySettings(const Settings& settings) {
  if (validateSettings(settings) != Status::Ok) {
    return TransportStatus::InvalidSettings;
  }
  return impl_->configureRadio(settings);
}

void KitsuMeshTransport::loop() {
  const uint32_t now = millis();
  if (impl_->meshLoopSeen) {
    const uint32_t gap = static_cast<uint32_t>(now - impl_->lastMeshLoopAtMs);
    if (gap > impl_->maxMeshLoopGapMs) impl_->maxMeshLoopGapMs = gap;
  } else {
    impl_->meshLoopSeen = true;
  }
  impl_->lastMeshLoopAtMs = now;
  if (impl_->active) impl_->client.loop();
  impl_->client.serviceAdvertRepeat(now);
  impl_->messaging.checkTimeout();
}

bool KitsuMeshTransport::identityReady() const {
  return impl_->identityReady;
}

bool KitsuMeshTransport::active() const { return impl_->active; }

int16_t KitsuMeshTransport::radioCode() const { return impl_->radioCode; }

uint32_t KitsuMeshTransport::profileId() const {
  return impl_->settings.radio.profileId;
}

TransportStatus KitsuMeshTransport::setEpoch(uint32_t epochSeconds) {
  if (epochSeconds < kMinimumEpoch || epochSeconds > kMaximumEpoch) {
    return TransportStatus::InvalidTime;
  }
  impl_->rtc.setCurrentTime(epochSeconds);
  return TransportStatus::Ok;
}

bool KitsuMeshTransport::timeValid() const { return impl_->rtc.valid(); }

uint32_t KitsuMeshTransport::currentEpoch() const {
  return impl_->rtc.valid() ? impl_->rtc.getCurrentTime() : 0;
}

bool KitsuMeshTransport::unlockTransmit(const Settings& settings,
                                        bool explicitUserApproval) {
  return impl_->active && sameActiveProfile(settings, impl_->settings) &&
      impl_->txGate.unlockForSession(settings, explicitUserApproval);
}

void KitsuMeshTransport::lockTransmit() {
  impl_->txGate.lock();
  impl_->driver.revokeOneShot();
  impl_->client.cancelQueuedSends();
  // A lock is an immediate revocation for anything not already on air.  This
  // also makes `introduce` followed by `tx lock` in the same serial drain safe.
  impl_->clearOutboundQueue();
}

bool KitsuMeshTransport::transmitAllowed(const Settings& settings) const {
  return impl_->active && sameActiveProfile(settings, impl_->settings) &&
      impl_->txGate.allowsTransmit(settings);
}

TransportStatus KitsuMeshTransport::exportSignedAdvert(
    const Settings& settings, const CurrentLocationOnce& current,
    char* outputHex, size_t outputCapacity, size_t& outputLength) {
  outputLength = 0;
  if (!outputHex || outputCapacity == 0) {
    return TransportStatus::InvalidArgument;
  }
  ::mesh::Packet* packet = nullptr;
  const TransportStatus status = impl_->makeAdvert(settings, current, packet);
  if (status != TransportStatus::Ok) return status;

  if (!impl_->client.prepareFloodRoute(packet,
                                       ChannelRegionScope::Legacy)) {
    impl_->client.releasePacket(packet);
    return TransportStatus::InvalidArgument;
  }
  uint8_t wire[MAX_TRANS_UNIT]{};
  const uint8_t wireBytes = packet->writeTo(wire);
  impl_->client.releasePacket(packet);
  if (outputCapacity < static_cast<size_t>(wireBytes) * 2U + 1U) {
    return TransportStatus::OutputTooSmall;
  }
  bytesToUpperHex(outputHex, wire, wireBytes);
  outputLength = static_cast<size_t>(wireBytes) * 2U;
  return TransportStatus::Ok;
}

TransportStatus KitsuMeshTransport::introduce(
    AdvertScope scope, const Settings& settings,
    const CurrentLocationOnce& current) {
  if (!impl_->active || !settings.enabled ||
      !sameActiveProfile(settings, impl_->settings)) {
    return TransportStatus::Disabled;
  }
  if (!impl_->txGate.allowsTransmit(settings)) {
    return TransportStatus::TxLocked;
  }
  ::mesh::Packet* packet = nullptr;
  const TransportStatus status = impl_->makeAdvert(settings, current, packet);
  if (status != TransportStatus::Ok) return status;

  if (scope == AdvertScope::Nearby) {
    impl_->client.sendZeroHop(packet);
  } else if (scope == AdvertScope::Flood) {
    if (!impl_->client.sendFloodRoute(packet,
                                      ChannelRegionScope::Legacy)) {
      impl_->client.releasePacket(packet);
      return TransportStatus::InvalidArgument;
    }
  } else {
    impl_->client.releasePacket(packet);
    return TransportStatus::InvalidArgument;
  }
  return TransportStatus::Ok;
}

TransportStatus KitsuMeshTransport::advertiseReadiness(
    const Settings& settings, const CurrentLocationOnce& current,
    uint32_t& retryAfterMs) const {
  retryAfterMs = 0U;
  if (!impl_->identityReady) {
    return TransportStatus::IdentityStorageFailed;
  }
  if (validateSettings(settings) != Status::Ok) {
    return TransportStatus::InvalidSettings;
  }
  if (!impl_->active || !settings.enabled ||
      !sameActiveProfile(settings, impl_->settings)) {
    return TransportStatus::Disabled;
  }
  // A companion one-shot is available only under the persisted policy that
  // permits an exact authenticated owner action. It never upgrades Locked to
  // a general transmit session.
  if (settings.txPolicy != TxPolicy::ExplicitSession ||
      impl_->settings.txPolicy != TxPolicy::ExplicitSession) {
    return TransportStatus::TxLocked;
  }
  if (!impl_->rtc.valid()) return TransportStatus::TimeUnset;

  AdvertLocation location{};
  if (settings.locationMode == LocationMode::CurrentOnce &&
      !selectAdvertLocation(settings, current, location)) {
    return TransportStatus::LocationUnavailable;
  }

  if (impl_->advertCooldownStarted) {
    const uint32_t elapsed = millis() - impl_->lastAdvertQueuedAtMillis;
    if (elapsed < kMeshAdvertiseCooldownMs) {
      retryAfterMs = kMeshAdvertiseCooldownMs - elapsed;
      return TransportStatus::AdvertiseCooldown;
    }
  }
  if (!impl_->driver.isInRecvMode() || impl_->messaging.pending() ||
      impl_->client.trackedSendInProgress() ||
      impl_->packets.getOutboundTotal() != 0) {
    return TransportStatus::SendBusy;
  }
  return TransportStatus::Ok;
}

TransportStatus KitsuMeshTransport::introduceOnce(
    AdvertScope scope, const Settings& settings,
    const CurrentLocationOnce& current, bool explicitUserApproval) {
  // Exact authenticated approval is mandatory even when another interface
  // has already opened the broader volatile session gate.
  if (!explicitUserApproval) return TransportStatus::TxLocked;
  if (scope != AdvertScope::Nearby && scope != AdvertScope::Flood) {
    return TransportStatus::InvalidArgument;
  }
  uint32_t retryAfterMs = 0U;
  const TransportStatus readiness =
      advertiseReadiness(settings, current, retryAfterMs);
  if (readiness != TransportStatus::Ok) return readiness;

  ::mesh::Packet* packet = nullptr;
  const TransportStatus created = impl_->makeAdvert(settings, current, packet);
  if (created != TransportStatus::Ok) return created;
  if (!packet || packet->payload_len < PUB_KEY_SIZE + sizeof(uint32_t)) {
    if (packet) impl_->client.releasePacket(packet);
    return TransportStatus::InvalidArgument;
  }
  uint32_t emittedAt = 0U;
  memcpy(&emittedAt, packet->payload + PUB_KEY_SIZE, sizeof(emittedAt));

  // Freeze the exact wire image before authorizing it. sendZeroHop/sendFlood
  // repeat these route mutations and then queue the same packet unchanged.
  if (scope == AdvertScope::Nearby) {
    packet->header &= ~PH_ROUTE_MASK;
    packet->header |= ROUTE_TYPE_DIRECT;
    packet->path_len = 0U;
  } else {
    if (!impl_->client.prepareFloodRoute(packet,
                                         ChannelRegionScope::Legacy)) {
      impl_->client.releasePacket(packet);
      return TransportStatus::InvalidArgument;
    }
  }
  uint8_t expectedWire[MAX_TRANS_UNIT]{};
  const uint8_t expectedWireBytes = packet->writeTo(expectedWire);
  if (expectedWireBytes == 0U ||
      !impl_->driver.armOneShotForPacket(
          settings, explicitUserApproval, expectedWire, expectedWireBytes)) {
    memset(expectedWire, 0, sizeof(expectedWire));
    impl_->client.releasePacket(packet);
    return TransportStatus::TxLocked;
  }
  memset(expectedWire, 0, sizeof(expectedWire));
  if (scope == AdvertScope::Nearby) {
    if (!impl_->client.beginNearbyAdvert(packet, emittedAt)) {
      impl_->driver.revokeOneShot();
      impl_->client.releasePacket(packet);
      return TransportStatus::SendBusy;
    }
    impl_->client.sendZeroHop(packet);
  } else {
    if (!impl_->client.beginFloodAdvert(packet, emittedAt)) {
      impl_->driver.revokeOneShot();
      impl_->client.releasePacket(packet);
      return TransportStatus::SendBusy;
    }
    if (!impl_->client.sendFloodRoute(packet,
                                      ChannelRegionScope::Legacy)) {
      impl_->driver.revokeOneShot();
      impl_->client.releasePacket(packet);
      return TransportStatus::InvalidArgument;
    }
  }
  impl_->advertCooldownStarted = true;
  impl_->lastAdvertQueuedAtMillis = millis();
  return TransportStatus::Ok;
}

bool KitsuMeshTransport::takeAdvert(ReceivedAdvert& output) {
  if (impl_->advertCount == 0) return false;
  output = impl_->advertQueue[impl_->advertRead];
  impl_->advertRead = static_cast<uint8_t>(
      (impl_->advertRead + 1U) % kAdvertQueueSize);
  --impl_->advertCount;
  return true;
}

bool KitsuMeshTransport::lastFloodAdvertStatus(FloodAdvertStatus& output) {
  return impl_->client.lastFloodAdvertStatus(millis(), output);
}

bool KitsuMeshTransport::takeFloodAdvertStatusChanged() {
  return impl_->client.takeFloodAdvertStatusChanged();
}

bool KitsuMeshTransport::lastNearbyAdvertStatus(NearbyAdvertStatus& output) {
  return impl_->client.lastNearbyAdvertStatus(output);
}

bool KitsuMeshTransport::takeNearbyAdvertStatusChanged() {
  return impl_->client.takeNearbyAdvertStatusChanged();
}

bool KitsuMeshTransport::takeNearbyRadioFrame(NearbyRadioFrame& output) {
  if (impl_->nearbyRadioCount == 0U) return false;
  output = impl_->nearbyRadioQueue[impl_->nearbyRadioRead];
  impl_->nearbyRadioQueue[impl_->nearbyRadioRead] = NearbyRadioFrame{};
  impl_->nearbyRadioRead = static_cast<uint8_t>(
      (impl_->nearbyRadioRead + 1U) % kNearbyRadioQueueSize);
  --impl_->nearbyRadioCount;
  return true;
}

TransportStatus KitsuMeshTransport::sendNearbyRadioFrame(
    const Settings& settings, const uint8_t* bytes, size_t byteCount,
    bool explicitUserApproval) {
  nearby::Packet packet{};
  if (!bytes || byteCount != nearby::kWireBytes ||
      nearby::decode(bytes, byteCount, packet) != nearby::Status::Ok) {
    return TransportStatus::InvalidArgument;
  }
  if (!impl_->active || !settings.enabled ||
      !sameActiveProfile(settings, impl_->settings)) {
    return TransportStatus::Disabled;
  }
  if (!explicitUserApproval) return TransportStatus::TxLocked;
  const uint32_t now = millis();
  constexpr uint32_t kPresenceCooldownMs = 5000UL;
  constexpr uint32_t kActionCooldownMs = 1000UL;
  const bool presence = packet.type == nearby::PacketType::Presence;
  if ((presence && impl_->nearbyPresenceCooldownStarted &&
       static_cast<uint32_t>(now - impl_->lastNearbyPresenceTxAt) <
           kPresenceCooldownMs) ||
      (!presence && impl_->nearbyActionCooldownStarted &&
       static_cast<uint32_t>(now - impl_->lastNearbyActionTxAt) <
           kActionCooldownMs)) {
    return TransportStatus::AdvertiseCooldown;
  }
  if (!impl_->driver.isInRecvMode() || impl_->messaging.pending() ||
      impl_->client.trackedSendInProgress() ||
      impl_->packets.getOutboundTotal() != 0U) {
    return TransportStatus::SendBusy;
  }
  if (!impl_->driver.armOneShotForPacket(settings, true, bytes, byteCount)) {
    return TransportStatus::TxLocked;
  }
  const bool sent = impl_->driver.sendDirectOneShotRaw(
      bytes, static_cast<int>(byteCount));
  if (!sent) return TransportStatus::SendBusy;
  if (presence) {
    impl_->nearbyPresenceCooldownStarted = true;
    impl_->lastNearbyPresenceTxAt = now;
  } else {
    impl_->nearbyActionCooldownStarted = true;
    impl_->lastNearbyActionTxAt = now;
  }
  return TransportStatus::Ok;
}

bool KitsuMeshTransport::publicKeyHex(char* output,
                                      size_t outputCapacity) const {
  if (!output || outputCapacity < PUB_KEY_SIZE * 2U + 1U ||
      !impl_->identityReady) {
    return false;
  }
  bytesToUpperHex(output, impl_->client.self_id.pub_key, PUB_KEY_SIZE);
  return true;
}

size_t KitsuMeshTransport::contactCount() const {
  return impl_->messaging.contactCount();
}

bool KitsuMeshTransport::contactAt(size_t index, ContactRecord& output) const {
  const ContactEntry* entry = impl_->messaging.contactAt(index);
  if (!entry) return false;
  ContactRecord record{};
  memcpy(record.publicKey, entry->publicKey, PUB_KEY_SIZE);
  memcpy(record.name, entry->name, sizeof(record.name));
  record.type = entry->type;
  record.pathKnown = entry->outPathLen != kUnknownPath;
  record.lastAdvertTimestamp = entry->lastAdvertTimestamp;
  record.pinned = entry->pinned;
  output = record;
  return true;
}

TransportStatus KitsuMeshTransport::upsertContact(
    const uint8_t publicKey[32], const char* name, uint8_t type) {
  if (!publicKey || !name ||
      (impl_->identityReady &&
       memcmp(publicKey, impl_->client.self_id.pub_key, PUB_KEY_SIZE) == 0)) {
    return TransportStatus::InvalidArgument;
  }
  if (impl_->messaging.pending()) return TransportStatus::SendBusy;
  return impl_->messaging.upsertContact(publicKey, name, type, true);
}

TransportStatus KitsuMeshTransport::stageObservedContact(
    const uint8_t publicKey[32], const char* name, uint8_t type,
    uint32_t advertTimestamp) {
  if (!publicKey || !name ||
      (impl_->identityReady &&
       memcmp(publicKey, impl_->client.self_id.pub_key, PUB_KEY_SIZE) == 0)) {
    return TransportStatus::InvalidArgument;
  }
  return impl_->messaging.stageObservedContact(publicKey, name, type,
                                               advertTimestamp);
}

TransportStatus KitsuMeshTransport::removeContact(
    const uint8_t publicKey[32]) {
  if (!publicKey) return TransportStatus::InvalidArgument;
  lockTransmit();
  if (impl_->client.trackedSendInProgress()) {
    return TransportStatus::SendBusy;
  }
  impl_->client.cancelAllSends();
  return impl_->messaging.removeContact(publicKey);
}

bool KitsuMeshTransport::channelAt(uint8_t slot, ChannelRecord& output) const {
  if (slot >= kMeshChannelCapacity) return false;
  const ChannelEntry* entry = impl_->messaging.channel(slot);
  ChannelRecord record{};
  record.slot = slot;
  if (entry) {
    record.configured = true;
    memcpy(record.name, entry->name, sizeof(record.name));
    record.hash = entry->channel.hash[0];
    record.regionScope = entry->regionScope;
  }
  output = record;
  return true;
}

TransportStatus KitsuMeshTransport::setChannel(
    uint8_t slot, const char* name, const uint8_t secret[32],
    ChannelRegionScope regionScope) {
  if (!name || !secret) return TransportStatus::InvalidArgument;
  lockTransmit();
  return impl_->messaging.setChannel(slot, name, secret, regionScope);
}

TransportStatus KitsuMeshTransport::clearChannel(uint8_t slot) {
  lockTransmit();
  return impl_->messaging.clearChannel(slot);
}

TransportStatus KitsuMeshTransport::sendDirectText(
    const Settings& settings, const uint8_t recipientPublicKey[32],
    const char* text, uint8_t attempt, uint32_t& queuedTimestamp,
    uint32_t& expectedAck, MessageRoute& route) {
  queuedTimestamp = 0;
  expectedAck = 0;
  route = MessageRoute::Flood;
  if (!recipientPublicKey || !text || text[0] == '\0') {
    return TransportStatus::InvalidArgument;
  }
  if (!impl_->messaging.storageReady()) {
    return TransportStatus::MessagingStorageFailed;
  }
  if (!impl_->active || !sameActiveProfile(settings, impl_->settings)) {
    return TransportStatus::Disabled;
  }
  if (!impl_->txGate.allowsTransmit(settings)) {
    return TransportStatus::TxLocked;
  }
  if (!impl_->rtc.valid()) return TransportStatus::TimeUnset;
  if (strnlen(text, kDirectOutboundTextBytes + 1U) >
      kDirectOutboundTextBytes) {
    return TransportStatus::TextTooLong;
  }
  ContactEntry* recipient = impl_->messaging.findContact(recipientPublicKey);
  if (!recipient) return TransportStatus::ContactNotFound;
  if (recipient->type != ADV_TYPE_CHAT) {
    return TransportStatus::ContactNotClient;
  }
  const uint32_t timestamp = impl_->rtc.getCurrentTimeUnique();
  const TransportStatus status = impl_->client.sendDirectText(
      *recipient, timestamp, text, attempt, expectedAck, route);
  if (status == TransportStatus::Ok) queuedTimestamp = timestamp;
  return status;
}

TransportStatus KitsuMeshTransport::sendChannelText(
    const Settings& settings, uint8_t slot, const char* text,
    uint32_t& queuedTimestamp) {
  queuedTimestamp = 0;
  if (!text || text[0] == '\0') return TransportStatus::InvalidArgument;
  if (!impl_->messaging.storageReady()) {
    return TransportStatus::MessagingStorageFailed;
  }
  if (!impl_->active || !sameActiveProfile(settings, impl_->settings)) {
    return TransportStatus::Disabled;
  }
  if (!impl_->txGate.allowsTransmit(settings)) {
    return TransportStatus::TxLocked;
  }
  if (!impl_->rtc.valid()) return TransportStatus::TimeUnset;
  const ChannelEntry* channel = impl_->messaging.channel(slot);
  if (!channel) return TransportStatus::ChannelNotFound;
  const uint32_t timestamp = impl_->rtc.getCurrentTimeUnique();
  const TransportStatus status = impl_->client.sendChannelText(
      *channel, slot, timestamp, impl_->advertisedIdentity.advertisedName,
      text);
  if (status == TransportStatus::Ok) queuedTimestamp = timestamp;
  return status;
}

TransportStatus KitsuMeshTransport::sendDirectTextOnce(
    const Settings& settings, const uint8_t recipientPublicKey[32],
    const char* text, uint8_t attempt, bool explicitUserApproval,
    uint32_t& queuedTimestamp, uint32_t& expectedAck,
    MessageRoute& route) {
  queuedTimestamp = 0U;
  expectedAck = 0U;
  route = MessageRoute::Flood;
  if (!recipientPublicKey || !text || text[0] == '\0') {
    return TransportStatus::InvalidArgument;
  }
  // This entry point is reserved for a single authenticated owner action.
  // An unrelated, already-open serial/session gate is not a substitute for
  // approval of this request.
  if (!explicitUserApproval) return TransportStatus::TxLocked;
  if (!impl_->messaging.storageReady()) {
    return TransportStatus::MessagingStorageFailed;
  }
  if (!impl_->active || !sameActiveProfile(settings, impl_->settings)) {
    return TransportStatus::Disabled;
  }
  if (!impl_->rtc.valid()) return TransportStatus::TimeUnset;
  if (strnlen(text, kDirectOutboundTextBytes + 1U) >
      kDirectOutboundTextBytes) {
    return TransportStatus::TextTooLong;
  }
  ContactEntry* recipient = impl_->messaging.findContact(recipientPublicKey);
  if (!recipient) return TransportStatus::ContactNotFound;
  if (recipient->type != ADV_TYPE_CHAT) {
    return TransportStatus::ContactNotClient;
  }
  if (impl_->messaging.pending() || impl_->client.trackedSendInProgress() ||
      impl_->packets.getOutboundTotal() != 0 ||
      !impl_->driver.isInRecvMode()) {
    return TransportStatus::SendBusy;
  }

  const bool sessionAllowed = impl_->txGate.allowsTransmit(settings);
  const uint32_t timestamp = impl_->rtc.getCurrentTimeUnique();
  const TransportStatus status = impl_->client.sendDirectText(
      *recipient, timestamp, text, attempt, expectedAck, route,
      !sessionAllowed);
  if (status != TransportStatus::Ok) {
    if (!sessionAllowed) impl_->driver.revokeOneShot();
    return status;
  }
  queuedTimestamp = timestamp;
  return TransportStatus::Ok;
}

TransportStatus KitsuMeshTransport::sendChannelTextOnce(
    const Settings& settings, uint8_t slot, const char* text,
    bool explicitUserApproval, uint32_t& queuedTimestamp) {
  queuedTimestamp = 0U;
  if (!text || text[0] == '\0') return TransportStatus::InvalidArgument;
  // Fail before observing the broader session gate: a one-shot API always
  // requires authorization for this exact owner action.
  if (!explicitUserApproval) return TransportStatus::TxLocked;
  if (!impl_->messaging.storageReady()) {
    return TransportStatus::MessagingStorageFailed;
  }
  if (!impl_->active || !sameActiveProfile(settings, impl_->settings)) {
    return TransportStatus::Disabled;
  }
  if (!impl_->rtc.valid()) return TransportStatus::TimeUnset;
  if (strnlen(text, kChannelOutboundTextBytes + 1U) >
      kChannelOutboundTextBytes) {
    return TransportStatus::TextTooLong;
  }
  const ChannelEntry* channel = impl_->messaging.channel(slot);
  if (!channel) return TransportStatus::ChannelNotFound;
  if (impl_->messaging.pending() || impl_->client.trackedSendInProgress() ||
      impl_->packets.getOutboundTotal() != 0 ||
      !impl_->driver.isInRecvMode()) {
    return TransportStatus::SendBusy;
  }

  const bool sessionAllowed = impl_->txGate.allowsTransmit(settings);
  const uint32_t timestamp = impl_->rtc.getCurrentTimeUnique();
  const TransportStatus status = impl_->client.sendChannelText(
      *channel, slot, timestamp, impl_->advertisedIdentity.advertisedName,
      text, !sessionAllowed);
  if (status != TransportStatus::Ok) {
    if (!sessionAllowed) impl_->driver.revokeOneShot();
    return status;
  }
  queuedTimestamp = timestamp;
  return TransportStatus::Ok;
}

bool KitsuMeshTransport::takeMessage(ReceivedMessage& output) {
  return impl_->messaging.takeMessage(output);
}

bool KitsuMeshTransport::takeDelivery(DeliveryEvent& output) {
  return impl_->messaging.takeDelivery(output);
}

bool KitsuMeshTransport::repeatDiagnostics(RepeatDiagnostics& output) const {
  impl_->client.repeatDiagnostics(output);
  output.maxMeshLoopGapMs = impl_->maxMeshLoopGapMs;
  if (impl_->active) {
    impl_->driver.currentReceiveSnapshot(
        output.currentRxSoftwareState,
        output.currentRxChipStatusAvailable,
        output.currentRxChipStatus);
  } else {
    output.currentRxSoftwareState = false;
    output.currentRxChipStatusAvailable = false;
    output.currentRxChipStatus = 0U;
  }
  return true;
}

bool KitsuMeshTransport::directSendPending() const {
  return impl_->messaging.pending();
}

bool KitsuMeshTransport::sendInProgress() const {
  return impl_->client.trackedSendInProgress();
}

uint32_t KitsuMeshTransport::droppedMessageCount() const {
  return impl_->messaging.droppedMessageCount();
}

uint32_t KitsuMeshTransport::droppedDeliveryCount() const {
  return impl_->messaging.droppedDeliveryCount();
}

bool KitsuMeshTransport::messagingStorageReady() const {
  return impl_->messaging.storageReady();
}

bool KitsuMeshTransport::messagingStorageStatus(
    MessagingStorageStatus& output) const {
  output = impl_->messaging.storageStatus();
  return true;
}

TransportStatus KitsuMeshTransport::resetMessagingState() {
  lockTransmit();
  if (impl_->client.trackedSendInProgress()) {
    return TransportStatus::SendBusy;
  }
  impl_->client.cancelAllSends();
  return impl_->messaging.reset() ? TransportStatus::Ok
                                  : TransportStatus::MessagingStorageFailed;
}

uint32_t KitsuMeshTransport::receivedAdvertCount() const {
  return impl_->receivedAdverts;
}

uint32_t KitsuMeshTransport::droppedAdvertCount() const {
  return impl_->droppedAdverts;
}

uint32_t KitsuMeshTransport::queuedAdvertCount() const {
  const int queued = impl_->packets.getOutboundTotal();
  return queued > 0 ? static_cast<uint32_t>(queued) : 0U;
}

}  // namespace mesh
}  // namespace kitsu868
