#pragma once

#include <stdint.h>

#include "signal_encounter.h"

// Persistent pity meter for signal encounters. This module deliberately does
// not consume entropy or select rarity/code outcomes: callers keep using the
// existing SignalEncounterCoordinator rolls, then pass only its natural
// encounter result here.
namespace kitsu868 {
namespace signal {

constexpr uint8_t kSignalTrailMaximumMisses = 20U;
constexpr uint8_t kSignalTrailStateSchemaVersion = 1U;

enum class SignalTrailHint : uint8_t {
  Quiet = 0,
  FaintSignal,
  TracksNearby,
  VeryClose,
  GuaranteedNext,
};

// Fixed-width semantic DTO for the caller's existing persistence container.
// It is not a raw flash/wire format and does not depend on compiler padding.
struct SignalTrailState {
  uint8_t schemaVersion = kSignalTrailStateSchemaVersion;
  uint8_t missCount = 0U;
  uint8_t hasLastOperation = 0U;
  uint8_t reserved = 0U;
  uint64_t lastOperationId = 0U;
};

// One accepted logical operation. `naturallyOccurred` is intentionally not
// stored here: encounterOccurred is the final decision after repeater and
// trail guarantees are applied.
struct SignalTrailResult {
  uint64_t operationId = 0U;
  uint8_t operationKind =
      static_cast<uint8_t>(MeshOperationKind::OtherCompleted);
  uint8_t encounterOccurred = 0U;
  uint8_t guaranteed = 0U;
  uint8_t guaranteedByTrail = 0U;
  uint8_t guaranteedByRepeater = 0U;
  uint8_t missesBefore = 0U;
  uint8_t missesAfter = 0U;
};

enum class SignalTrailProcessStatus : uint8_t {
  RecordedMiss = 0,
  RecordedEncounter,
  InvalidEvent,
  UnsuccessfulOperation,
  DuplicateOperation,
  StaleOperation,
  StateUnavailable,
};

enum class SignalTrailRestoreStatus : uint8_t {
  Ok = 0,
  UnsupportedSchema,
  InvalidState,
};

enum class SignalTrailMergeStatus : uint8_t {
  Applied = 0,
  Unchanged,
  InvalidMissCount,
  StateUnavailable,
};

bool validateSignalTrailState(const SignalTrailState& state);
SignalTrailHint signalTrailHintForMissCount(uint8_t missCount);
const char* signalTrailHintName(SignalTrailHint hint);

class SignalTrail {
 public:
  SignalTrail() = default;

  // naturallyOccurred must be the existing coordinator's 0/1 encounter
  // result. At twenty prior misses, this method changes only the final
  // encounter decision; the coordinator's rarity and code rolls remain the
  // ones to resolve. Repeater discovery remains an immediate guarantee.
  SignalTrailProcessStatus process(const LogicalOperationEvent& event,
                                   uint8_t naturallyOccurred,
                                   SignalTrailResult& output);

  // Imports an already-bounded peer/group trail result without advancing the
  // local logical-operation replay cursor. The live meter only moves forward.
  SignalTrailMergeStatus mergeSharedMissCount(uint8_t mergedMissCount);

  SignalTrailState snapshot() const;

  // Invalid persisted input quarantines the meter. Processing then fails
  // closed with StateUnavailable until the caller deliberately reset()s it or
  // restores a valid state; corrupt state can never silently erase progress or
  // permit a replayed logical operation.
  SignalTrailRestoreStatus restore(const SignalTrailState& state);
  void reset();

  bool available() const;
  uint8_t missCount() const;
  bool nextEligibleGuaranteed() const;
  SignalTrailHint hint() const;

 private:
  void quarantine();

  SignalTrailState state_{};
  bool available_ = true;
};

const char* signalTrailProcessStatusName(SignalTrailProcessStatus status);
const char* signalTrailRestoreStatusName(SignalTrailRestoreStatus status);

}  // namespace signal
}  // namespace kitsu868
