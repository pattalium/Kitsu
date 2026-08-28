#include "signal_trail.h"

namespace kitsu868 {
namespace signal {

bool validateSignalTrailState(const SignalTrailState& state) {
  if (state.schemaVersion != kSignalTrailStateSchemaVersion ||
      state.missCount > kSignalTrailMaximumMisses ||
      state.hasLastOperation > 1U || state.reserved != 0U) {
    return false;
  }
  if (state.hasLastOperation == 0U) {
    return state.lastOperationId == 0U && state.missCount == 0U;
  }
  return state.lastOperationId != 0U;
}

SignalTrailHint signalTrailHintForMissCount(uint8_t missCount) {
  if (missCount >= kSignalTrailMaximumMisses) {
    return SignalTrailHint::GuaranteedNext;
  }
  if (missCount >= 15U) return SignalTrailHint::VeryClose;
  if (missCount >= 10U) return SignalTrailHint::TracksNearby;
  if (missCount >= 5U) return SignalTrailHint::FaintSignal;
  return SignalTrailHint::Quiet;
}

const char* signalTrailHintName(SignalTrailHint hint) {
  switch (hint) {
    case SignalTrailHint::Quiet:
      return "quiet";
    case SignalTrailHint::FaintSignal:
      return "faint_signal";
    case SignalTrailHint::TracksNearby:
      return "tracks_nearby";
    case SignalTrailHint::VeryClose:
      return "very_close";
    case SignalTrailHint::GuaranteedNext:
      return "guaranteed_next";
  }
  return "unknown";
}

SignalTrailProcessStatus SignalTrail::process(
    const LogicalOperationEvent& event, uint8_t naturallyOccurred,
    SignalTrailResult& output) {
  if (!available_) return SignalTrailProcessStatus::StateUnavailable;
  if (event.operationId == 0U || !validOperationKind(event.kind) ||
      event.successful > 1U || naturallyOccurred > 1U) {
    return SignalTrailProcessStatus::InvalidEvent;
  }
  if (event.successful == 0U) {
    return SignalTrailProcessStatus::UnsuccessfulOperation;
  }
  if (state_.hasLastOperation != 0U) {
    if (event.operationId == state_.lastOperationId) {
      return SignalTrailProcessStatus::DuplicateOperation;
    }
    if (event.operationId < state_.lastOperationId) {
      return SignalTrailProcessStatus::StaleOperation;
    }
  }

  SignalTrailResult candidate{};
  candidate.operationId = event.operationId;
  candidate.operationKind = static_cast<uint8_t>(event.kind);
  candidate.missesBefore = state_.missCount;

  const bool repeaterGuaranteed =
      event.kind == MeshOperationKind::RepeaterDiscovered;
  const bool trailGuaranteed =
      !repeaterGuaranteed &&
      state_.missCount >= kSignalTrailMaximumMisses;
  const bool occurred = naturallyOccurred != 0U || repeaterGuaranteed ||
                        trailGuaranteed;

  candidate.guaranteedByRepeater = repeaterGuaranteed ? 1U : 0U;
  candidate.guaranteedByTrail = trailGuaranteed ? 1U : 0U;
  candidate.guaranteed =
      (repeaterGuaranteed || trailGuaranteed) ? 1U : 0U;
  candidate.encounterOccurred = occurred ? 1U : 0U;
  candidate.missesAfter = occurred
                              ? 0U
                              : static_cast<uint8_t>(
                                    state_.missCount < kSignalTrailMaximumMisses
                                        ? state_.missCount + 1U
                                        : kSignalTrailMaximumMisses);

  state_.schemaVersion = kSignalTrailStateSchemaVersion;
  state_.missCount = candidate.missesAfter;
  state_.hasLastOperation = 1U;
  state_.reserved = 0U;
  state_.lastOperationId = event.operationId;
  output = candidate;
  return occurred ? SignalTrailProcessStatus::RecordedEncounter
                  : SignalTrailProcessStatus::RecordedMiss;
}

SignalTrailState SignalTrail::snapshot() const {
  return state_;
}

SignalTrailRestoreStatus SignalTrail::restore(
    const SignalTrailState& state) {
  if (state.schemaVersion != kSignalTrailStateSchemaVersion) {
    quarantine();
    return SignalTrailRestoreStatus::UnsupportedSchema;
  }
  if (!validateSignalTrailState(state)) {
    quarantine();
    return SignalTrailRestoreStatus::InvalidState;
  }
  state_ = state;
  available_ = true;
  return SignalTrailRestoreStatus::Ok;
}

void SignalTrail::reset() {
  state_ = SignalTrailState{};
  available_ = true;
}

bool SignalTrail::available() const {
  return available_;
}

uint8_t SignalTrail::missCount() const {
  return state_.missCount;
}

bool SignalTrail::nextEligibleGuaranteed() const {
  return available_ && state_.missCount >= kSignalTrailMaximumMisses;
}

SignalTrailHint SignalTrail::hint() const {
  return signalTrailHintForMissCount(state_.missCount);
}

void SignalTrail::quarantine() {
  state_ = SignalTrailState{};
  // Keep snapshot() invalid while quarantined so an incautious persistence
  // write cannot turn a failed restore into a valid empty trail on reboot.
  state_.schemaVersion = 0U;
  available_ = false;
}

const char* signalTrailProcessStatusName(SignalTrailProcessStatus status) {
  switch (status) {
    case SignalTrailProcessStatus::RecordedMiss:
      return "recorded_miss";
    case SignalTrailProcessStatus::RecordedEncounter:
      return "recorded_encounter";
    case SignalTrailProcessStatus::InvalidEvent:
      return "invalid_event";
    case SignalTrailProcessStatus::UnsuccessfulOperation:
      return "unsuccessful_operation";
    case SignalTrailProcessStatus::DuplicateOperation:
      return "duplicate_operation";
    case SignalTrailProcessStatus::StaleOperation:
      return "stale_operation";
    case SignalTrailProcessStatus::StateUnavailable:
      return "state_unavailable";
  }
  return "unknown";
}

const char* signalTrailRestoreStatusName(SignalTrailRestoreStatus status) {
  switch (status) {
    case SignalTrailRestoreStatus::Ok:
      return "ok";
    case SignalTrailRestoreStatus::UnsupportedSchema:
      return "unsupported_schema";
    case SignalTrailRestoreStatus::InvalidState:
      return "invalid_state";
  }
  return "unknown";
}

}  // namespace signal
}  // namespace kitsu868
