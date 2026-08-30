#include "kitsu_pet_presence.h"

#include <cmath>
#include <string.h>

namespace kitsu868 {
namespace presence {
namespace {

constexpr uint32_t kMaximumComparableIntervalMs = UINT32_C(0x7FFFFFFF);
constexpr float kMinimumRssiDbm = -140.0f;
constexpr float kMaximumRssiDbm = 0.0f;
constexpr float kMinimumSnrDb = -32.0f;
constexpr float kMaximumSnrDb = 32.0f;
constexpr float kRssiFilterAlpha = 0.25f;

// Initial band boundaries match the existing Android presentation. The five
// dB entry/exit margins prevent chatter around either boundary.
constexpr float kStrongBoundaryDbm = -65.0f;
constexpr float kMediumBoundaryDbm = -85.0f;
constexpr float kBandHysteresisDb = 5.0f;

constexpr float kTrendEnterDeltaDb = 5.0f;
constexpr float kTrendSteadyDeltaDb = 2.0f;

bool validInterval(uint32_t later, uint32_t earlier, uint32_t& elapsed) {
  elapsed = later - earlier;
  return elapsed <= kMaximumComparableIntervalMs;
}

bool validObservation(const Observation& observation) {
  return observation.uid != 0U && std::isfinite(observation.rssiDbm) &&
      std::isfinite(observation.snrDb) &&
      observation.rssiDbm >= kMinimumRssiDbm &&
      observation.rssiDbm <= kMaximumRssiDbm &&
      observation.snrDb >= kMinimumSnrDb &&
      observation.snrDb <= kMaximumSnrDb;
}

SignalBand initialBand(float rssiDbm) {
  if (rssiDbm >= kStrongBoundaryDbm) return SignalBand::Strong;
  if (rssiDbm >= kMediumBoundaryDbm) return SignalBand::Medium;
  return SignalBand::Weak;
}

SignalBand nextBand(SignalBand current, float rssiDbm) {
  switch (current) {
    case SignalBand::Weak:
      if (rssiDbm >= kStrongBoundaryDbm + kBandHysteresisDb) {
        return SignalBand::Strong;
      }
      if (rssiDbm >= kMediumBoundaryDbm + kBandHysteresisDb) {
        return SignalBand::Medium;
      }
      return SignalBand::Weak;
    case SignalBand::Medium:
      if (rssiDbm >= kStrongBoundaryDbm + kBandHysteresisDb) {
        return SignalBand::Strong;
      }
      if (rssiDbm < kMediumBoundaryDbm - kBandHysteresisDb) {
        return SignalBand::Weak;
      }
      return SignalBand::Medium;
    case SignalBand::Strong:
      if (rssiDbm < kMediumBoundaryDbm - kBandHysteresisDb) {
        return SignalBand::Weak;
      }
      if (rssiDbm < kStrongBoundaryDbm - kBandHysteresisDb) {
        return SignalBand::Medium;
      }
      return SignalBand::Strong;
    case SignalBand::Unknown:
      return initialBand(rssiDbm);
  }
  return SignalBand::Unknown;
}

SignalTrend trendCandidate(float deltaDb) {
  if (deltaDb >= kTrendEnterDeltaDb) return SignalTrend::Approaching;
  if (deltaDb <= -kTrendEnterDeltaDb) return SignalTrend::Leaving;
  if (deltaDb >= -kTrendSteadyDeltaDb &&
      deltaDb <= kTrendSteadyDeltaDb) {
    return SignalTrend::Steady;
  }
  return SignalTrend::Unknown;
}

void copyGone(const ExpireResult& source, ObserveResult& destination) {
  destination.goneCount = source.goneCount;
  for (uint8_t index = 0U; index < source.goneCount; ++index) {
    destination.goneUids[index] = source.goneUids[index];
  }
}

}  // namespace

PetPresenceTracker::PetPresenceTracker() { reset(); }

void PetPresenceTracker::reset() {
  memset(slots_, 0, sizeof(slots_));
  clockReady_ = false;
  lastNowMs_ = 0U;
}

int PetPresenceTracker::findSlot(uint16_t uid) const {
  for (uint8_t index = 0U; index < kPeerCapacity; ++index) {
    if (slots_[index].used && slots_[index].uid == uid) return index;
  }
  return -1;
}

int PetPresenceTracker::allocateSlot(uint16_t uid, uint32_t nowMs) {
  for (uint8_t index = 0U; index < kPeerCapacity; ++index) {
    if (!slots_[index].used) {
      slots_[index] = Slot{};
      slots_[index].used = true;
      slots_[index].uid = uid;
      return index;
    }
  }

  // Never evict a currently present peer to admit a ninth signal. Once peers
  // have gone, replace the oldest inactive slot; ties retain deterministic
  // slot order.
  int oldest = -1;
  uint32_t oldestAge = 0U;
  for (uint8_t index = 0U; index < kPeerCapacity; ++index) {
    if (slots_[index].present) continue;
    uint32_t age = 0U;
    if (!validInterval(nowMs, slots_[index].lastSeenAtMs, age)) continue;
    if (oldest < 0 || age > oldestAge) {
      oldest = index;
      oldestAge = age;
    }
  }
  if (oldest < 0) return -1;
  slots_[oldest] = Slot{};
  slots_[oldest].used = true;
  slots_[oldest].uid = uid;
  return oldest;
}

void PetPresenceTracker::beginAppearance(
    Slot& slot, const Observation& observation, uint16_t& events) {
  const bool remembered = slot.seenBefore || observation.familiarHint;
  slot.present = true;
  slot.familiar = remembered;
  slot.latestRssiDbm = observation.rssiDbm;
  slot.smoothedRssiDbm = observation.rssiDbm;
  slot.latestSnrDb = observation.snrDb;
  slot.lastSeenAtMs = observation.observedAtMs;
  slot.lastFilterAtMs = observation.observedAtMs;
  slot.trendBaselineAtMs = observation.observedAtMs;
  slot.lastTrendVoteAtMs = observation.observedAtMs;
  slot.trendBaselineRssiDbm = observation.rssiDbm;
  slot.sampleCount = 1U;
  slot.trendVotes = 0U;
  slot.band = initialBand(observation.rssiDbm);
  slot.trend = SignalTrend::Unknown;
  slot.trendCandidate = SignalTrend::Unknown;
  slot.seenBefore = true;
  events |= EventAppeared;
  if (remembered) events |= EventFamiliar;
}

void PetPresenceTracker::updateSignal(
    Slot& slot, const Observation& observation, uint16_t& events) {
  uint32_t filterElapsed = 0U;
  const bool filterTimeValid = validInterval(
      observation.observedAtMs, slot.lastFilterAtMs, filterElapsed);

  slot.latestRssiDbm = observation.rssiDbm;
  slot.latestSnrDb = observation.snrDb;
  slot.lastSeenAtMs = observation.observedAtMs;
  if (observation.familiarHint && !slot.familiar) {
    slot.familiar = true;
    events |= EventFamiliar;
  }
  if (!filterTimeValid || filterElapsed < kMinimumFilterSampleGapMs) return;

  slot.lastFilterAtMs = observation.observedAtMs;
  slot.smoothedRssiDbm +=
      (observation.rssiDbm - slot.smoothedRssiDbm) * kRssiFilterAlpha;
  if (slot.sampleCount != UINT16_MAX) ++slot.sampleCount;

  const SignalBand previousBand = slot.band;
  slot.band = nextBand(slot.band, slot.smoothedRssiDbm);
  if (slot.band != previousBand) events |= EventBandChanged;

  uint32_t trendSpan = 0U;
  if (!validInterval(observation.observedAtMs, slot.trendBaselineAtMs,
                     trendSpan) ||
      trendSpan < kTrendMinimumSpanMs) {
    return;
  }
  const SignalTrend candidate = trendCandidate(
      slot.smoothedRssiDbm - slot.trendBaselineRssiDbm);
  if (candidate == SignalTrend::Unknown) {
    slot.trendCandidate = SignalTrend::Unknown;
    slot.trendVotes = 0U;
    return;
  }

  uint32_t voteGap = 0U;
  const bool separatedVote = validInterval(
      observation.observedAtMs, slot.lastTrendVoteAtMs, voteGap) &&
      voteGap >= kTrendVoteGapMs;
  if (slot.trendCandidate != candidate) {
    slot.trendCandidate = candidate;
    slot.trendVotes = 1U;
    slot.lastTrendVoteAtMs = observation.observedAtMs;
  } else if (separatedVote && slot.trendVotes < UINT8_MAX) {
    ++slot.trendVotes;
    slot.lastTrendVoteAtMs = observation.observedAtMs;
  }
  if (slot.trendVotes < kTrendRequiredVotes) return;

  if (slot.trend != candidate) {
    slot.trend = candidate;
    if (candidate == SignalTrend::Approaching) {
      events |= EventApproaching;
    } else if (candidate == SignalTrend::Leaving) {
      events |= EventLeaving;
    } else if (candidate == SignalTrend::Steady) {
      events |= EventTrendSettled;
    }
  }
  slot.trendBaselineAtMs = observation.observedAtMs;
  slot.trendBaselineRssiDbm = slot.smoothedRssiDbm;
  slot.trendCandidate = SignalTrend::Unknown;
  slot.trendVotes = 0U;
}

ObserveResult PetPresenceTracker::observe(const Observation& observation) {
  ObserveResult result{};
  if (!validObservation(observation)) {
    result.status = ObserveStatus::InvalidInput;
    result.group = summary();
    return result;
  }
  if (clockReady_) {
    uint32_t clockElapsed = 0U;
    if (!validInterval(observation.observedAtMs, lastNowMs_, clockElapsed)) {
      result.status = ObserveStatus::StaleTimestamp;
      const int existing = findSlot(observation.uid);
      if (existing >= 0) result.peer = snapshot(slots_[existing]);
      result.group = summary();
      return result;
    }
  }

  int slotIndex = findSlot(observation.uid);
  if (slotIndex >= 0) {
    uint32_t elapsed = 0U;
    if (!validInterval(observation.observedAtMs,
                       slots_[slotIndex].lastSeenAtMs, elapsed)) {
      result.status = ObserveStatus::StaleTimestamp;
      result.peer = snapshot(slots_[slotIndex]);
      result.group = summary();
      return result;
    }
  }

  const ExpireResult expired = expire(observation.observedAtMs);
  result.events = expired.events;
  copyGone(expired, result);
  const uint8_t countBefore = expired.group.presentCount;

  slotIndex = findSlot(observation.uid);
  if (slotIndex < 0) {
    slotIndex = allocateSlot(observation.uid, observation.observedAtMs);
  }
  if (slotIndex < 0) {
    result.status = ObserveStatus::CapacityFull;
    result.group = summary();
    return result;
  }

  Slot& slot = slots_[slotIndex];
  if (!slot.present) {
    beginAppearance(slot, observation, result.events);
  } else {
    updateSignal(slot, observation, result.events);
  }

  result.status = ObserveStatus::Ok;
  result.peer = snapshot(slot);
  result.group = summary();
  if (countBefore < 2U && result.group.presentCount >= 2U) {
    result.events |= EventGroupStarted;
  }
  return result;
}

ExpireResult PetPresenceTracker::expire(uint32_t nowMs) {
  ExpireResult result{};
  if (clockReady_) {
    uint32_t clockElapsed = 0U;
    if (!validInterval(nowMs, lastNowMs_, clockElapsed)) {
      result.group = summary();
      return result;
    }
  }
  clockReady_ = true;
  lastNowMs_ = nowMs;
  result.timeAccepted = true;
  const uint8_t countBefore = summary().presentCount;
  for (uint8_t index = 0U; index < kPeerCapacity; ++index) {
    Slot& slot = slots_[index];
    if (!slot.used || !slot.present) continue;
    uint32_t age = 0U;
    if (!validInterval(nowMs, slot.lastSeenAtMs, age) ||
        age < kGoneAfterMs) {
      continue;
    }
    slot.present = false;
    slot.trend = SignalTrend::Unknown;
    slot.trendCandidate = SignalTrend::Unknown;
    slot.trendVotes = 0U;
    result.goneUids[result.goneCount++] = slot.uid;
  }
  if (result.goneCount != 0U) result.events |= EventGone;
  result.group = summary();
  if (countBefore >= 2U && result.group.presentCount < 2U) {
    result.events |= EventGroupEnded;
  }
  return result;
}

GroupSummary PetPresenceTracker::summary() const {
  GroupSummary output{};
  for (uint8_t index = 0U; index < kPeerCapacity; ++index) {
    const Slot& slot = slots_[index];
    if (!slot.used || !slot.present) continue;
    ++output.presentCount;
    if (slot.familiar) ++output.familiarCount;
    if (!output.strongestSignalAvailable ||
        slot.smoothedRssiDbm > output.strongestSmoothedRssiDbm ||
        (slot.smoothedRssiDbm == output.strongestSmoothedRssiDbm &&
         slot.uid < output.strongestSignalUid)) {
      output.strongestSignalAvailable = true;
      output.strongestSignalUid = slot.uid;
      output.strongestSmoothedRssiDbm = slot.smoothedRssiDbm;
      output.strongestSignalBand = slot.band;
    }
  }
  output.groupActive = output.presentCount >= 2U;
  return output;
}

PeerSnapshot PetPresenceTracker::snapshot(const Slot& slot) {
  PeerSnapshot output{};
  output.tracked = slot.used;
  output.present = slot.present;
  output.familiar = slot.familiar;
  output.uid = slot.uid;
  output.latestRssiDbm = slot.latestRssiDbm;
  output.smoothedRssiDbm = slot.smoothedRssiDbm;
  output.latestSnrDb = slot.latestSnrDb;
  output.lastSeenAtMs = slot.lastSeenAtMs;
  output.sampleCount = slot.sampleCount;
  output.band = slot.band;
  output.trend = slot.trend;
  return output;
}

bool PetPresenceTracker::peer(uint16_t uid, PeerSnapshot& output) const {
  const int index = findSlot(uid);
  if (index < 0) return false;
  output = snapshot(slots_[index]);
  return true;
}

bool PetPresenceTracker::peerAt(uint8_t slot, PeerSnapshot& output) const {
  if (slot >= kPeerCapacity || !slots_[slot].used) return false;
  output = snapshot(slots_[slot]);
  return true;
}

uint8_t PetPresenceTracker::trackedCount() const {
  uint8_t count = 0U;
  for (uint8_t index = 0U; index < kPeerCapacity; ++index) {
    if (slots_[index].used) ++count;
  }
  return count;
}

bool hasEvent(uint16_t events, Event event) {
  return (events & static_cast<uint16_t>(event)) != 0U;
}

const char* signalBandName(SignalBand band) {
  switch (band) {
    case SignalBand::Unknown: return "unknown";
    case SignalBand::Weak: return "weak";
    case SignalBand::Medium: return "medium";
    case SignalBand::Strong: return "strong";
  }
  return "unknown";
}

const char* signalTrendName(SignalTrend trend) {
  switch (trend) {
    case SignalTrend::Unknown: return "unknown";
    case SignalTrend::Steady: return "steady";
    case SignalTrend::Approaching: return "approaching";
    case SignalTrend::Leaving: return "leaving";
  }
  return "unknown";
}

const char* observeStatusName(ObserveStatus status) {
  switch (status) {
    case ObserveStatus::Ok: return "ok";
    case ObserveStatus::InvalidInput: return "invalid_input";
    case ObserveStatus::StaleTimestamp: return "stale_timestamp";
    case ObserveStatus::CapacityFull: return "capacity_full";
  }
  return "unknown";
}

}  // namespace presence
}  // namespace kitsu868
