#pragma once

#include <stddef.h>
#include <stdint.h>

// Allocation-free signal-presence tracking for the dedicated Kitsu nearby
// protocol. RSSI bands and trends describe received signal only. They are not
// bearings, distance estimates, or proof of a transmitter's identity.
namespace kitsu868 {
namespace presence {

constexpr uint8_t kPeerCapacity = 8U;
constexpr uint32_t kGoneAfterMs = 20000UL;
constexpr uint32_t kMinimumFilterSampleGapMs = 1000UL;
constexpr uint32_t kTrendMinimumSpanMs = 10000UL;
constexpr uint32_t kTrendVoteGapMs = 3000UL;
constexpr uint8_t kTrendRequiredVotes = 2U;

enum class SignalBand : uint8_t {
  Unknown = 0U,
  Weak,
  Medium,
  Strong,
};

enum class SignalTrend : uint8_t {
  Unknown = 0U,
  Steady,
  Approaching,
  Leaving,
};

enum class ObserveStatus : uint8_t {
  Ok = 0U,
  InvalidInput,
  StaleTimestamp,
  CapacityFull,
};

enum Event : uint16_t {
  EventNone = 0U,
  EventAppeared = UINT16_C(1) << 0U,
  // Historical recognition only. The nearby-v2 wire packet is not signed.
  EventFamiliar = UINT16_C(1) << 1U,
  EventBandChanged = UINT16_C(1) << 2U,
  EventApproaching = UINT16_C(1) << 3U,
  EventLeaving = UINT16_C(1) << 4U,
  EventGone = UINT16_C(1) << 5U,
  EventGroupStarted = UINT16_C(1) << 6U,
  EventGroupEnded = UINT16_C(1) << 7U,
  EventTrendSettled = UINT16_C(1) << 8U,
};

struct Observation {
  uint16_t uid = 0U;
  float rssiDbm = 0.0f;
  float snrDb = 0.0f;
  uint32_t observedAtMs = 0U;
  // Supplied by an existing persistent friendship source when available.
  // It is a remembered-UID hint, never radio authentication.
  bool familiarHint = false;
};

struct PeerSnapshot {
  bool tracked = false;
  bool present = false;
  bool familiar = false;
  uint16_t uid = 0U;
  float latestRssiDbm = 0.0f;
  float smoothedRssiDbm = 0.0f;
  float latestSnrDb = 0.0f;
  uint32_t lastSeenAtMs = 0U;
  uint16_t sampleCount = 0U;
  SignalBand band = SignalBand::Unknown;
  SignalTrend trend = SignalTrend::Unknown;
};

struct GroupSummary {
  uint8_t presentCount = 0U;
  uint8_t familiarCount = 0U;
  bool groupActive = false;
  bool strongestSignalAvailable = false;
  // This is the UID with the strongest smoothed RSSI, not a direction or a
  // claim that it is physically the nearest transmitter.
  uint16_t strongestSignalUid = 0U;
  float strongestSmoothedRssiDbm = 0.0f;
  SignalBand strongestSignalBand = SignalBand::Unknown;
};

struct ExpireResult {
  bool timeAccepted = false;
  uint16_t events = EventNone;
  uint8_t goneCount = 0U;
  uint16_t goneUids[kPeerCapacity]{};
  GroupSummary group{};
};

struct ObserveResult {
  ObserveStatus status = ObserveStatus::InvalidInput;
  uint16_t events = EventNone;
  uint8_t goneCount = 0U;
  uint16_t goneUids[kPeerCapacity]{};
  PeerSnapshot peer{};
  GroupSummary group{};
};

class PetPresenceTracker {
 public:
  PetPresenceTracker();

  void reset();
  ObserveResult observe(const Observation& observation);
  ExpireResult expire(uint32_t nowMs);

  GroupSummary summary() const;
  bool peer(uint16_t uid, PeerSnapshot& output) const;
  bool peerAt(uint8_t slot, PeerSnapshot& output) const;
  uint8_t trackedCount() const;

 private:
  struct Slot {
    bool used = false;
    bool present = false;
    bool seenBefore = false;
    bool familiar = false;
    uint16_t uid = 0U;
    float latestRssiDbm = 0.0f;
    float smoothedRssiDbm = 0.0f;
    float latestSnrDb = 0.0f;
    uint32_t lastSeenAtMs = 0U;
    uint32_t lastFilterAtMs = 0U;
    uint32_t trendBaselineAtMs = 0U;
    uint32_t lastTrendVoteAtMs = 0U;
    float trendBaselineRssiDbm = 0.0f;
    uint16_t sampleCount = 0U;
    uint8_t trendVotes = 0U;
    SignalBand band = SignalBand::Unknown;
    SignalTrend trend = SignalTrend::Unknown;
    SignalTrend trendCandidate = SignalTrend::Unknown;
  };

  Slot slots_[kPeerCapacity]{};
  bool clockReady_ = false;
  uint32_t lastNowMs_ = 0U;

  int findSlot(uint16_t uid) const;
  int allocateSlot(uint16_t uid, uint32_t nowMs);
  void beginAppearance(Slot& slot, const Observation& observation,
                       uint16_t& events);
  void updateSignal(Slot& slot, const Observation& observation,
                    uint16_t& events);
  static PeerSnapshot snapshot(const Slot& slot);
};

bool hasEvent(uint16_t events, Event event);
const char* signalBandName(SignalBand band);
const char* signalTrendName(SignalTrend trend);
const char* observeStatusName(ObserveStatus status);

}  // namespace presence
}  // namespace kitsu868
