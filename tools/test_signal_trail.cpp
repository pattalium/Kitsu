#include "../src/signal_trail.h"

#include <cstdint>
#include <cstring>
#include <iostream>

namespace signal = kitsu868::signal;

namespace {

int failures = 0;

void check(bool condition, const char* description) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL " << description << '\n';
  }
}

signal::LogicalOperationEvent event(
    uint64_t id,
    signal::MeshOperationKind kind = signal::MeshOperationKind::AdvertSent,
    uint8_t successful = 1U) {
  signal::LogicalOperationEvent output{};
  output.operationId = id;
  output.kind = kind;
  output.successful = successful;
  return output;
}

bool sameResult(const signal::SignalTrailResult& left,
                const signal::SignalTrailResult& right) {
  return left.operationId == right.operationId &&
         left.operationKind == right.operationKind &&
         left.encounterOccurred == right.encounterOccurred &&
         left.guaranteed == right.guaranteed &&
         left.guaranteedByTrail == right.guaranteedByTrail &&
         left.guaranteedByRepeater == right.guaranteedByRepeater &&
         left.missesBefore == right.missesBefore &&
         left.missesAfter == right.missesAfter;
}

void testTwentyMissGuaranteeAndDedupe() {
  signal::SignalTrail trail;
  signal::SignalTrailResult result{};

  check(trail.available() && trail.missCount() == 0U &&
            !trail.nextEligibleGuaranteed() &&
            trail.hint() == signal::SignalTrailHint::Quiet,
        "new trail starts visibly empty");

  for (uint64_t id = 1U; id <= signal::kSignalTrailMaximumMisses; ++id) {
    check(trail.process(event(id), 0U, result) ==
              signal::SignalTrailProcessStatus::RecordedMiss,
          "eligible natural miss is recorded");
    check(result.encounterOccurred == 0U && result.guaranteed == 0U &&
              result.missesBefore == id - 1U && result.missesAfter == id,
          "each accepted miss advances exactly once");
  }

  check(trail.missCount() == signal::kSignalTrailMaximumMisses &&
            trail.nextEligibleGuaranteed() &&
            trail.hint() == signal::SignalTrailHint::GuaranteedNext,
        "twenty misses visibly arm the next-operation guarantee");

  const signal::SignalTrailResult beforeDuplicate = result;
  check(trail.process(event(20U), 1U, result) ==
            signal::SignalTrailProcessStatus::DuplicateOperation,
        "duplicate logical operation cannot consume a guarantee");
  check(sameResult(result, beforeDuplicate) && trail.missCount() == 20U,
        "duplicate leaves output and progress untouched");
  check(trail.process(event(19U), 1U, result) ==
            signal::SignalTrailProcessStatus::StaleOperation,
        "stale logical operation cannot consume a guarantee");

  check(trail.process(event(21U), 0U, result) ==
            signal::SignalTrailProcessStatus::RecordedEncounter,
        "next eligible operation after twenty misses is guaranteed");
  check(result.encounterOccurred == 1U && result.guaranteed == 1U &&
            result.guaranteedByTrail == 1U &&
            result.guaranteedByRepeater == 0U &&
            result.missesBefore == 20U && result.missesAfter == 0U &&
            trail.missCount() == 0U && !trail.nextEligibleGuaranteed(),
        "trail guarantee resets progress and identifies its source");
}

void testNaturalAndRepeaterResets() {
  signal::SignalTrail trail;
  signal::SignalTrailResult result{};
  for (uint64_t id = 1U; id <= 7U; ++id) {
    (void)trail.process(event(id), 0U, result);
  }
  check(trail.process(event(8U), 1U, result) ==
            signal::SignalTrailProcessStatus::RecordedEncounter &&
            result.guaranteed == 0U && result.missesBefore == 7U &&
            result.missesAfter == 0U,
        "natural encounter resets without being marked guaranteed");

  for (uint64_t id = 9U; id <= 11U; ++id) {
    (void)trail.process(event(id), 0U, result);
  }
  check(trail.process(
            event(12U, signal::MeshOperationKind::RepeaterDiscovered), 0U,
            result) == signal::SignalTrailProcessStatus::RecordedEncounter &&
            result.guaranteed == 1U &&
            result.guaranteedByRepeater == 1U &&
            result.guaranteedByTrail == 0U && result.missesAfter == 0U,
        "repeater remains an immediate guarantee and resets progress");

  check(trail.process(event(13U, signal::MeshOperationKind::MessageSent, 0U),
                      0U, result) ==
            signal::SignalTrailProcessStatus::UnsuccessfulOperation &&
            trail.snapshot().lastOperationId == 12U,
        "unsuccessful operation cannot advance trail or dedupe state");

  signal::LogicalOperationEvent malformed = event(13U);
  malformed.successful = 2U;
  check(trail.process(malformed, 0U, result) ==
            signal::SignalTrailProcessStatus::InvalidEvent,
        "non-boolean success marker is rejected");
  malformed = event(13U);
  check(trail.process(malformed, 2U, result) ==
            signal::SignalTrailProcessStatus::InvalidEvent,
        "non-boolean natural encounter marker is rejected");
}

void testHintsPersistenceAndFailClosedRestore() {
  check(signal::signalTrailHintForMissCount(0U) ==
            signal::SignalTrailHint::Quiet &&
            signal::signalTrailHintForMissCount(4U) ==
                signal::SignalTrailHint::Quiet &&
            signal::signalTrailHintForMissCount(5U) ==
                signal::SignalTrailHint::FaintSignal &&
            signal::signalTrailHintForMissCount(10U) ==
                signal::SignalTrailHint::TracksNearby &&
            signal::signalTrailHintForMissCount(15U) ==
                signal::SignalTrailHint::VeryClose &&
            signal::signalTrailHintForMissCount(20U) ==
                signal::SignalTrailHint::GuaranteedNext,
        "visible hints change at 5/10/15/20 misses");
  check(std::strcmp(signal::signalTrailHintName(
                        signal::SignalTrailHint::TracksNearby),
                    "tracks_nearby") == 0,
        "hint name is stable for UI integration");

  signal::SignalTrail original;
  signal::SignalTrailResult result{};
  for (uint64_t id = 101U; id <= 119U; ++id) {
    (void)original.process(event(id), 0U, result);
  }
  const signal::SignalTrailState saved = original.snapshot();
  check(signal::validateSignalTrailState(saved) && saved.missCount == 19U,
        "nineteen-miss progress is persistable");

  signal::SignalTrail restored;
  check(restored.restore(saved) == signal::SignalTrailRestoreStatus::Ok &&
            restored.missCount() == 19U,
        "valid trail restores after reset");
  check(restored.process(event(120U), 0U, result) ==
            signal::SignalTrailProcessStatus::RecordedMiss &&
            restored.nextEligibleGuaranteed(),
        "restored progress reaches guarantee without losing a miss");
  check(restored.process(event(120U), 0U, result) ==
            signal::SignalTrailProcessStatus::DuplicateOperation &&
            restored.nextEligibleGuaranteed(),
        "restored dedupe remains monotonic");

  signal::SignalTrailState invalid = saved;
  invalid.missCount = 21U;
  check(restored.restore(invalid) ==
            signal::SignalTrailRestoreStatus::InvalidState &&
            !restored.available(),
        "out-of-range persisted progress is quarantined");
  check(!signal::validateSignalTrailState(restored.snapshot()),
        "quarantined snapshot cannot be persisted as valid empty progress");
  check(restored.process(event(121U), 0U, result) ==
            signal::SignalTrailProcessStatus::StateUnavailable,
        "invalid restore fails closed instead of silently restarting");

  restored.reset();
  check(restored.available() && restored.missCount() == 0U,
        "explicit reset recovers a quarantined meter");
  invalid = signal::SignalTrailState{};
  invalid.schemaVersion = 2U;
  check(restored.restore(invalid) ==
            signal::SignalTrailRestoreStatus::UnsupportedSchema &&
            !restored.available(),
        "unknown schema also fails closed");

  invalid = signal::SignalTrailState{};
  invalid.hasLastOperation = 0U;
  invalid.missCount = 1U;
  check(!signal::validateSignalTrailState(invalid),
        "progress without an operation record is rejected");
  invalid = signal::SignalTrailState{};
  invalid.hasLastOperation = 1U;
  invalid.lastOperationId = 0U;
  check(!signal::validateSignalTrailState(invalid),
        "operation marker without an ID is rejected");
}

void testCoordinatorIntegrationKeepsOriginalRolls() {
  signal::Configuration configuration{};
  const uint16_t rarityWeights[signal::kRarityCount] = {
      5500U, 2500U, 1200U, 500U, 200U, 50U, 50U};
  for (size_t index = 0U; index < signal::kRarityCount; ++index) {
    configuration.rarityWeightBasisPoints[index] = rarityWeights[index];
    configuration.codeChanceBasisPoints[index] = 2500U;
  }
  check(signal::validateConfiguration(configuration) ==
            signal::ConfigurationStatus::Ok,
        "zero-natural-chance integration configuration is valid");
  signal::SignalEncounterCoordinator coordinator(configuration);
  signal::SignalTrail trail;
  signal::EncounterRecord record{};
  signal::SignalTrailResult trailResult{};
  for (uint64_t id = 1U; id <= 21U; ++id) {
    const signal::LogicalOperationEvent operation = event(id);
    const signal::ProcessStatus coordinatorStatus = coordinator.process(
        operation, static_cast<uint32_t>(UINT32_C(0x10203040) + id), record);
    check(coordinatorStatus == signal::ProcessStatus::RecordedNoEncounter,
          "coordinator remains a natural miss at zero configured chance");
    const uint16_t rarityRollBefore = record.rarityRollBasisPoints;
    const uint16_t codeRollBefore = record.codeRollBasisPoints;
    const signal::SignalTrailProcessStatus trailStatus = trail.process(
        operation, record.encounterOccurred, trailResult);
    if (id <= 20U) {
      check(trailStatus == signal::SignalTrailProcessStatus::RecordedMiss,
            "first twenty coordinator misses fill trail");
    } else {
      check(trailStatus ==
                    signal::SignalTrailProcessStatus::RecordedEncounter &&
                trailResult.guaranteedByTrail == 1U,
            "twenty-first coordinator miss becomes one trail encounter");
      signal::Rarity rarity = signal::Rarity::Common;
      check(signal::rarityForRoll(configuration, rarityRollBefore, rarity),
            "forced encounter resolves rarity from coordinator roll");
      const signal::CodeOutcome code = signal::codeOutcomeForRoll(
          configuration, rarity, codeRollBefore);
      check(code == signal::CodeOutcome::Revealed ||
                code == signal::CodeOutcome::NotRevealed,
            "forced encounter resolves code from coordinator roll");
      check(record.rarityRollBasisPoints == rarityRollBefore &&
                record.codeRollBasisPoints == codeRollBefore,
            "trail does not reroll or mutate coordinator entropy");
    }
  }
}

}  // namespace

int main() {
  testTwentyMissGuaranteeAndDedupe();
  testNaturalAndRepeaterResets();
  testHintsPersistenceAndFailClosedRestore();
  testCoordinatorIntegrationKeepsOriginalRolls();

  if (failures != 0) {
    std::cerr << "TEST_FAIL signal_trail failures=" << failures << '\n';
    return 1;
  }
  std::cout << "TEST_PASS signal_trail cap=20 hints=5,10,15,20 "
               "dedupe=monotonic restore=fail_closed repeater=immediate "
               "coordinator_rolls=preserved\n";
  return 0;
}
