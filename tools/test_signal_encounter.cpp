#include "../src/signal_encounter.h"

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

size_t operationIndex(signal::MeshOperationKind kind) {
  return static_cast<size_t>(kind);
}

size_t rarityIndex(signal::Rarity rarity) {
  return static_cast<size_t>(rarity);
}

signal::Configuration validConfiguration() {
  signal::Configuration configuration{};
  configuration.encounterChanceBasisPoints[
      operationIndex(signal::MeshOperationKind::RepeaterDiscovered)] = 0U;
  configuration.encounterChanceBasisPoints[
      operationIndex(signal::MeshOperationKind::PeerDiscovered)] = 0U;
  configuration.encounterChanceBasisPoints[
      operationIndex(signal::MeshOperationKind::MessageSent)] = 10000U;
  configuration.encounterChanceBasisPoints[
      operationIndex(signal::MeshOperationKind::MessageReceived)] = 6500U;
  configuration.encounterChanceBasisPoints[
      operationIndex(signal::MeshOperationKind::AdvertSent)] = 2500U;
  configuration.encounterChanceBasisPoints[
      operationIndex(signal::MeshOperationKind::AdvertReceived)] = 2500U;
  configuration.encounterChanceBasisPoints[
      operationIndex(signal::MeshOperationKind::OtherCompleted)] = 1000U;
  configuration.encounterChanceBasisPoints[
      operationIndex(signal::MeshOperationKind::NearbyKitsuMet)] = 300U;

  configuration.rarityWeightBasisPoints[rarityIndex(signal::Rarity::Common)] =
      4000U;
  configuration.rarityWeightBasisPoints[
      rarityIndex(signal::Rarity::Uncommon)] = 2500U;
  configuration.rarityWeightBasisPoints[rarityIndex(signal::Rarity::Rare)] =
      1500U;
  configuration.rarityWeightBasisPoints[
      rarityIndex(signal::Rarity::VeryRare)] = 900U;
  configuration.rarityWeightBasisPoints[rarityIndex(signal::Rarity::Epic)] =
      600U;
  configuration.rarityWeightBasisPoints[
      rarityIndex(signal::Rarity::Legendary)] = 450U;
  configuration.rarityWeightBasisPoints[
      rarityIndex(signal::Rarity::Mythical)] = 50U;

  const uint16_t codeChances[signal::kRarityCount] = {
      5000U, 4000U, 3000U, 2500U, 2000U, 1000U, 500U,
  };
  for (size_t index = 0U; index < signal::kRarityCount; ++index) {
    configuration.codeChanceBasisPoints[index] = codeChances[index];
  }
  return configuration;
}

signal::LogicalOperationEvent event(uint64_t id,
                                    signal::MeshOperationKind kind,
                                    uint8_t successful = 1U) {
  signal::LogicalOperationEvent output{};
  output.operationId = id;
  output.kind = kind;
  output.successful = successful;
  return output;
}

bool sameRecord(const signal::EncounterRecord& left,
                const signal::EncounterRecord& right) {
  return left.schemaVersion == right.schemaVersion &&
         left.operationKind == right.operationKind &&
         left.encounterOccurred == right.encounterOccurred &&
         left.guaranteed == right.guaranteed &&
         left.rarity == right.rarity &&
         left.codeOutcome == right.codeOutcome &&
         left.encounterRollBasisPoints == right.encounterRollBasisPoints &&
         left.rarityRollBasisPoints == right.rarityRollBasisPoints &&
         left.codeRollBasisPoints == right.codeRollBasisPoints &&
         left.operationId == right.operationId &&
         left.entropy == right.entropy;
}

void testConfigurationAndRarityBoundaries() {
  const signal::Configuration configuration = validConfiguration();
  check(signal::validateConfiguration(configuration) ==
            signal::ConfigurationStatus::Ok,
        "valid configuration accepted");
  check(configuration.encounterChanceBasisPoints[
            operationIndex(signal::MeshOperationKind::NearbyKitsuMet)] > 0U &&
            configuration.encounterChanceBasisPoints[
                operationIndex(signal::MeshOperationKind::NearbyKitsuMet)] <
                configuration.encounterChanceBasisPoints[
                    operationIndex(signal::MeshOperationKind::MessageReceived)],
        "new Kitsu meeting has a lower nonzero wild-encounter chance");
  check(configuration.rarityWeightBasisPoints[
            rarityIndex(signal::Rarity::Mythical)] <
            signal::kOnePercentBasisPoints,
        "Mythical weight is below one percent");
  for (size_t index = 0U; index < signal::kRarityCount; ++index) {
    check(configuration.codeChanceBasisPoints[index] > 0U,
          "every encounter tier has usable code resolution");
  }

  struct Boundary {
    uint16_t roll;
    signal::Rarity expected;
  };
  const Boundary boundaries[] = {
      {0U, signal::Rarity::Common},
      {3999U, signal::Rarity::Common},
      {4000U, signal::Rarity::Uncommon},
      {6499U, signal::Rarity::Uncommon},
      {6500U, signal::Rarity::Rare},
      {7999U, signal::Rarity::Rare},
      {8000U, signal::Rarity::VeryRare},
      {8899U, signal::Rarity::VeryRare},
      {8900U, signal::Rarity::Epic},
      {9499U, signal::Rarity::Epic},
      {9500U, signal::Rarity::Legendary},
      {9949U, signal::Rarity::Legendary},
      {9950U, signal::Rarity::Mythical},
      {9999U, signal::Rarity::Mythical},
  };
  for (const Boundary& boundary : boundaries) {
    signal::Rarity selected = signal::Rarity::Common;
    check(signal::rarityForRoll(configuration, boundary.roll, selected) &&
              selected == boundary.expected,
          "ordered rarity boundary resolves correctly");
  }

  signal::Rarity unchanged = signal::Rarity::Epic;
  check(!signal::rarityForRoll(configuration, 10000U, unchanged) &&
            unchanged == signal::Rarity::Epic,
        "out-of-range rarity roll rejected transactionally");
  check(signal::codeOutcomeForRoll(configuration, signal::Rarity::Common,
                                   4999U) == signal::CodeOutcome::Revealed,
        "code roll below chance reveals a code");
  check(signal::codeOutcomeForRoll(configuration, signal::Rarity::Common,
                                   5000U) ==
            signal::CodeOutcome::NotRevealed,
        "code roll at chance does not reveal a code");

  signal::Configuration invalid = configuration;
  invalid.encounterChanceBasisPoints[0] = 10001U;
  check(signal::validateConfiguration(invalid) ==
            signal::ConfigurationStatus::EncounterChanceOutOfRange,
        "encounter chance above 10000 rejected");
  invalid = configuration;
  ++invalid.rarityWeightBasisPoints[0];
  check(signal::validateConfiguration(invalid) ==
            signal::ConfigurationStatus::RarityWeightTotalInvalid,
        "rarity weights must total 10000");
  invalid = configuration;
  invalid.rarityWeightBasisPoints[rarityIndex(signal::Rarity::Mythical)] =
      100U;
  invalid.rarityWeightBasisPoints[rarityIndex(signal::Rarity::Common)] -=
      50U;
  check(signal::validateConfiguration(invalid) ==
            signal::ConfigurationStatus::MythicalWeightMustBeSubOnePercent,
        "Mythical weight at one percent rejected");
  invalid = configuration;
  invalid.rarityWeightBasisPoints[rarityIndex(signal::Rarity::Mythical)] = 0U;
  invalid.rarityWeightBasisPoints[rarityIndex(signal::Rarity::Common)] += 50U;
  check(signal::validateConfiguration(invalid) ==
            signal::ConfigurationStatus::MythicalWeightMustBeSubOnePercent,
        "configuration must retain a non-zero Mythical tier");
  invalid = configuration;
  invalid.codeChanceBasisPoints[rarityIndex(signal::Rarity::Epic)] = 10001U;
  check(signal::validateConfiguration(invalid) ==
            signal::ConfigurationStatus::CodeChanceOutOfRange,
        "code chance above 10000 rejected");
}

void testGuaranteeDedupeAndResetRecovery() {
  const signal::Configuration configuration = validConfiguration();
  signal::SignalEncounterCoordinator coordinator(configuration);
  signal::EncounterRecord record{};

  // The configuration entry is zero, proving the guarantee is structural and
  // cannot accidentally be weakened by balance data.
  check(coordinator.process(
            event(100U, signal::MeshOperationKind::RepeaterDiscovered),
            UINT32_C(0x12345678), record) ==
            signal::ProcessStatus::RecordedEncounter,
        "repeater discovery guarantees an encounter");
  check(record.encounterOccurred == 1U && record.guaranteed == 1U &&
            signal::validateRecord(record),
        "guaranteed encounter record is valid");
  const signal::EncounterRecord firstRecord = record;

  check(coordinator.process(
            event(100U, signal::MeshOperationKind::RepeaterDiscovered),
            UINT32_C(0xFFFFFFFF), record) ==
            signal::ProcessStatus::DuplicateOperation,
        "same logical operation cannot reroll with different entropy");
  check(sameRecord(record, firstRecord),
        "duplicate leaves caller output untouched");
  check(coordinator.process(
            event(99U, signal::MeshOperationKind::MessageSent), 1U, record) ==
            signal::ProcessStatus::StaleOperation,
        "older operation cannot reroll after newer persisted operation");

  const signal::CoordinatorState saved = coordinator.snapshot();
  signal::SignalEncounterCoordinator afterReset(configuration);
  check(afterReset.restore(saved) == signal::RestoreStatus::Ok,
        "coordinator state restores after reset");
  check(afterReset.process(
            event(100U, signal::MeshOperationKind::RepeaterDiscovered), 0U,
            record) == signal::ProcessStatus::DuplicateOperation,
        "reset-stable state still blocks replay");

  check(afterReset.process(
            event(101U, signal::MeshOperationKind::MessageReceived, 0U), 9U,
            record) == signal::ProcessStatus::UnsuccessfulOperation,
        "failed MeshCore operation does not roll");
  check(afterReset.snapshot().lastOperationId == 100U,
        "failed operation does not advance dedupe state");

  check(afterReset.process(
            event(101U, signal::MeshOperationKind::PeerDiscovered), 9U,
            record) == signal::ProcessStatus::RecordedNoEncounter,
        "zero-chance non-repeater records no encounter");
  check(record.encounterOccurred == 0U && record.guaranteed == 0U &&
            record.codeOutcome ==
                static_cast<uint8_t>(signal::CodeOutcome::NotApplicable) &&
            signal::validateRecord(record),
        "no-encounter record keeps code outcome separate");

  signal::CoordinatorState invalidState = afterReset.snapshot();
  invalidState.lastRecord.operationId = 500U;
  const signal::CoordinatorState beforeBadRestore = afterReset.snapshot();
  check(afterReset.restore(invalidState) == signal::RestoreStatus::InvalidState,
        "inconsistent persisted state rejected");
  check(afterReset.snapshot().lastOperationId ==
            beforeBadRestore.lastOperationId,
        "failed restore is transactional");

  invalidState = signal::CoordinatorState{};
  invalidState.schemaVersion = 2U;
  check(afterReset.restore(invalidState) ==
            signal::RestoreStatus::UnsupportedSchema,
        "unknown persisted schema rejected");
}

void testDeterminismAndIndependentCodeResolution() {
  signal::Configuration configuration = validConfiguration();
  for (size_t index = 0U; index < signal::kRarityCount; ++index) {
    configuration.codeChanceBasisPoints[index] = 10000U;
  }
  signal::SignalEncounterCoordinator left(configuration);
  signal::SignalEncounterCoordinator right(configuration);
  const signal::LogicalOperationEvent message =
      event(UINT64_C(0x0102030405060708),
            signal::MeshOperationKind::MessageSent);
  signal::EncounterRecord leftRecord{};
  signal::EncounterRecord rightRecord{};
  check(left.process(message, UINT32_C(0xA5A5C3C3), leftRecord) ==
            signal::ProcessStatus::RecordedEncounter &&
            right.process(message, UINT32_C(0xA5A5C3C3), rightRecord) ==
                signal::ProcessStatus::RecordedEncounter,
        "deterministic peers both record the encounter");
  check(sameRecord(leftRecord, rightRecord),
        "same input yields byte-field-identical semantic record");
  check(leftRecord.codeOutcome ==
            static_cast<uint8_t>(signal::CodeOutcome::Revealed),
        "100 percent code table reveals independently after encounter");

  for (size_t index = 0U; index < signal::kRarityCount; ++index) {
    configuration.codeChanceBasisPoints[index] = 0U;
  }
  signal::SignalEncounterCoordinator noCode(configuration);
  signal::EncounterRecord noCodeRecord{};
  check(noCode.process(message, UINT32_C(0xA5A5C3C3), noCodeRecord) ==
            signal::ProcessStatus::RecordedEncounter,
        "same encounter occurs with a zero code table");
  check(noCodeRecord.rarity == leftRecord.rarity &&
            noCodeRecord.codeOutcome ==
                static_cast<uint8_t>(signal::CodeOutcome::NotRevealed),
        "code resolution changes without changing creature rarity");

  signal::LogicalOperationEvent malformed = message;
  malformed.operationId = 0U;
  check(noCode.process(malformed, 0U, noCodeRecord) ==
            signal::ProcessStatus::InvalidEvent,
        "zero operation ID rejected");
  malformed = message;
  malformed.successful = 2U;
  check(noCode.process(malformed, 0U, noCodeRecord) ==
            signal::ProcessStatus::InvalidEvent,
        "non-boolean success marker rejected");

  signal::Configuration invalid = configuration;
  invalid.rarityWeightBasisPoints[0] = 0U;
  signal::SignalEncounterCoordinator unusable(invalid);
  check(unusable.process(message, 0U, noCodeRecord) ==
            signal::ProcessStatus::InvalidConfiguration,
        "invalid coordinator configuration cannot process events");
  check(std::strcmp(
            signal::processStatusName(signal::ProcessStatus::DuplicateOperation),
            "duplicate_operation") == 0,
        "process status names are stable");
}

}  // namespace

int main() {
  testConfigurationAndRarityBoundaries();
  testGuaranteeDedupeAndResetRecovery();
  testDeterminismAndIndependentCodeResolution();

  if (failures != 0) {
    std::cerr << "TEST_FAIL signal_encounter failures=" << failures << '\n';
    return 1;
  }
  std::cout << "TEST_PASS signal_encounter rarity_tiers=7 mythical_bp=50 "
               "repeater_guaranteed=1 nearby_kitsu_bp=300 "
               "dedupe=monotonic reset_stable=1\n";
  return 0;
}
