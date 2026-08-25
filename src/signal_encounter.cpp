#include "signal_encounter.h"

namespace kitsu868 {
namespace signal {
namespace {

size_t operationIndex(MeshOperationKind kind) {
  return static_cast<size_t>(kind);
}

size_t rarityIndex(Rarity rarity) {
  return static_cast<size_t>(rarity);
}

uint32_t rotateLeft(uint32_t value, uint8_t count) {
  return static_cast<uint32_t>((value << count) |
                               (value >> (32U - count)));
}

uint32_t avalanche(uint32_t value) {
  value ^= value >> 16U;
  value *= UINT32_C(0x7FEB352D);
  value ^= value >> 15U;
  value *= UINT32_C(0x846CA68B);
  value ^= value >> 16U;
  return value;
}

uint16_t deterministicRoll(const LogicalOperationEvent& event,
                           uint32_t entropy, uint32_t domain) {
  const uint32_t operationLow = static_cast<uint32_t>(event.operationId);
  const uint32_t operationHigh =
      static_cast<uint32_t>(event.operationId >> 32U);
  uint32_t value = entropy ^ operationLow ^ rotateLeft(operationHigh, 13U);
  value ^= static_cast<uint32_t>(static_cast<uint8_t>(event.kind)) *
           UINT32_C(0x9E3779B9);
  value ^= domain;
  return static_cast<uint16_t>(avalanche(value) % kBasisPointScale);
}

bool validCodeOutcome(CodeOutcome outcome) {
  return outcome == CodeOutcome::NotApplicable ||
         outcome == CodeOutcome::NotRevealed ||
         outcome == CodeOutcome::Revealed;
}

}  // namespace

bool validOperationKind(MeshOperationKind kind) {
  return static_cast<uint8_t>(kind) <
         static_cast<uint8_t>(MeshOperationKind::Count);
}

bool validRarity(Rarity rarity) {
  return static_cast<uint8_t>(rarity) < static_cast<uint8_t>(Rarity::Count);
}

ConfigurationStatus validateConfiguration(
    const Configuration& configuration) {
  for (size_t index = 0U; index < kMeshOperationKindCount; ++index) {
    if (configuration.encounterChanceBasisPoints[index] > kBasisPointScale) {
      return ConfigurationStatus::EncounterChanceOutOfRange;
    }
  }

  uint32_t rarityTotal = 0U;
  for (size_t index = 0U; index < kRarityCount; ++index) {
    rarityTotal += configuration.rarityWeightBasisPoints[index];
  }
  if (rarityTotal != kBasisPointScale) {
    return ConfigurationStatus::RarityWeightTotalInvalid;
  }
  const uint16_t mythicalWeight = configuration.rarityWeightBasisPoints[
      rarityIndex(Rarity::Mythical)];
  if (mythicalWeight == 0U || mythicalWeight >= kOnePercentBasisPoints) {
    return ConfigurationStatus::MythicalWeightMustBeSubOnePercent;
  }

  for (size_t index = 0U; index < kRarityCount; ++index) {
    if (configuration.codeChanceBasisPoints[index] > kBasisPointScale) {
      return ConfigurationStatus::CodeChanceOutOfRange;
    }
  }
  return ConfigurationStatus::Ok;
}

const char* configurationStatusName(ConfigurationStatus status) {
  switch (status) {
    case ConfigurationStatus::Ok:
      return "ok";
    case ConfigurationStatus::EncounterChanceOutOfRange:
      return "encounter_chance_out_of_range";
    case ConfigurationStatus::RarityWeightTotalInvalid:
      return "rarity_weight_total_invalid";
    case ConfigurationStatus::MythicalWeightMustBeSubOnePercent:
      return "mythical_weight_must_be_sub_one_percent";
    case ConfigurationStatus::CodeChanceOutOfRange:
      return "code_chance_out_of_range";
  }
  return "unknown";
}

bool rarityForRoll(const Configuration& configuration,
                   uint16_t rollBasisPoints, Rarity& output) {
  if (validateConfiguration(configuration) != ConfigurationStatus::Ok ||
      rollBasisPoints >= kBasisPointScale) {
    return false;
  }

  uint32_t upperBound = 0U;
  for (size_t index = 0U; index < kRarityCount; ++index) {
    upperBound += configuration.rarityWeightBasisPoints[index];
    if (rollBasisPoints < upperBound) {
      output = static_cast<Rarity>(index);
      return true;
    }
  }
  return false;
}

CodeOutcome codeOutcomeForRoll(const Configuration& configuration,
                               Rarity rarity, uint16_t rollBasisPoints) {
  if (validateConfiguration(configuration) != ConfigurationStatus::Ok ||
      !validRarity(rarity) || rollBasisPoints >= kBasisPointScale) {
    return CodeOutcome::NotApplicable;
  }
  return rollBasisPoints <
                 configuration.codeChanceBasisPoints[rarityIndex(rarity)]
             ? CodeOutcome::Revealed
             : CodeOutcome::NotRevealed;
}

bool validateRecord(const EncounterRecord& record) {
  if (record.schemaVersion != kRecordSchemaVersion ||
      record.operationId == 0U ||
      !validOperationKind(
          static_cast<MeshOperationKind>(record.operationKind)) ||
      record.encounterOccurred > 1U || record.guaranteed > 1U ||
      record.encounterRollBasisPoints >= kBasisPointScale ||
      record.rarityRollBasisPoints >= kBasisPointScale ||
      record.codeRollBasisPoints >= kBasisPointScale ||
      !validRarity(static_cast<Rarity>(record.rarity)) ||
      !validCodeOutcome(static_cast<CodeOutcome>(record.codeOutcome))) {
    return false;
  }

  const MeshOperationKind kind =
      static_cast<MeshOperationKind>(record.operationKind);
  if (record.guaranteed !=
      (kind == MeshOperationKind::RepeaterDiscovered ? 1U : 0U)) {
    return false;
  }
  if (record.guaranteed != 0U && record.encounterOccurred == 0U) {
    return false;
  }
  const CodeOutcome code = static_cast<CodeOutcome>(record.codeOutcome);
  if (record.encounterOccurred == 0U) {
    return code == CodeOutcome::NotApplicable;
  }
  return code == CodeOutcome::NotRevealed || code == CodeOutcome::Revealed;
}

SignalEncounterCoordinator::SignalEncounterCoordinator(
    const Configuration& configuration)
    : configuration_(configuration),
      configurationStatus_(validateConfiguration(configuration)) {}

ConfigurationStatus SignalEncounterCoordinator::configurationStatus() const {
  return configurationStatus_;
}

ProcessStatus SignalEncounterCoordinator::process(
    const LogicalOperationEvent& event, uint32_t entropy,
    EncounterRecord& output) {
  if (configurationStatus_ != ConfigurationStatus::Ok) {
    return ProcessStatus::InvalidConfiguration;
  }
  if (event.operationId == 0U || !validOperationKind(event.kind) ||
      event.successful > 1U) {
    return ProcessStatus::InvalidEvent;
  }
  if (event.successful == 0U) {
    return ProcessStatus::UnsuccessfulOperation;
  }
  if (state_.hasLastRecord != 0U) {
    if (event.operationId == state_.lastOperationId) {
      return ProcessStatus::DuplicateOperation;
    }
    if (event.operationId < state_.lastOperationId) {
      return ProcessStatus::StaleOperation;
    }
  }

  EncounterRecord candidate{};
  candidate.operationId = event.operationId;
  candidate.operationKind = static_cast<uint8_t>(event.kind);
  candidate.entropy = entropy;
  candidate.encounterRollBasisPoints = deterministicRoll(
      event, entropy, UINT32_C(0x454E4301));  // "ENC" domain
  candidate.rarityRollBasisPoints = deterministicRoll(
      event, entropy, UINT32_C(0x52415202));  // "RAR" domain
  candidate.codeRollBasisPoints = deterministicRoll(
      event, entropy, UINT32_C(0x434F4403));  // "COD" domain

  const bool guaranteed = event.kind == MeshOperationKind::RepeaterDiscovered;
  candidate.guaranteed = guaranteed ? 1U : 0U;
  const uint16_t chance = guaranteed
                              ? kBasisPointScale
                              : configuration_.encounterChanceBasisPoints[
                                    operationIndex(event.kind)];
  const bool occurred = candidate.encounterRollBasisPoints < chance;
  candidate.encounterOccurred = occurred ? 1U : 0U;

  if (occurred) {
    Rarity rarity = Rarity::Common;
    if (!rarityForRoll(configuration_, candidate.rarityRollBasisPoints,
                       rarity)) {
      return ProcessStatus::InvalidConfiguration;
    }
    candidate.rarity = static_cast<uint8_t>(rarity);
    candidate.codeOutcome = static_cast<uint8_t>(codeOutcomeForRoll(
        configuration_, rarity, candidate.codeRollBasisPoints));
  }

  state_.schemaVersion = kCoordinatorStateSchemaVersion;
  state_.hasLastRecord = 1U;
  state_.reserved = 0U;
  state_.lastOperationId = event.operationId;
  state_.lastRecord = candidate;
  output = candidate;
  return occurred ? ProcessStatus::RecordedEncounter
                  : ProcessStatus::RecordedNoEncounter;
}

CoordinatorState SignalEncounterCoordinator::snapshot() const {
  return state_;
}

RestoreStatus SignalEncounterCoordinator::restore(
    const CoordinatorState& state) {
  if (state.schemaVersion != kCoordinatorStateSchemaVersion) {
    return RestoreStatus::UnsupportedSchema;
  }
  if (state.hasLastRecord > 1U || state.reserved != 0U) {
    return RestoreStatus::InvalidState;
  }
  if (state.hasLastRecord == 0U) {
    if (state.lastOperationId != 0U) {
      return RestoreStatus::InvalidState;
    }
  } else if (state.lastOperationId == 0U ||
             state.lastRecord.operationId != state.lastOperationId ||
             !validateRecord(state.lastRecord)) {
    return RestoreStatus::InvalidState;
  }
  state_ = state;
  return RestoreStatus::Ok;
}

void SignalEncounterCoordinator::reset() {
  state_ = CoordinatorState{};
}

const char* processStatusName(ProcessStatus status) {
  switch (status) {
    case ProcessStatus::RecordedNoEncounter:
      return "recorded_no_encounter";
    case ProcessStatus::RecordedEncounter:
      return "recorded_encounter";
    case ProcessStatus::InvalidConfiguration:
      return "invalid_configuration";
    case ProcessStatus::InvalidEvent:
      return "invalid_event";
    case ProcessStatus::UnsuccessfulOperation:
      return "unsuccessful_operation";
    case ProcessStatus::DuplicateOperation:
      return "duplicate_operation";
    case ProcessStatus::StaleOperation:
      return "stale_operation";
  }
  return "unknown";
}

const char* restoreStatusName(RestoreStatus status) {
  switch (status) {
    case RestoreStatus::Ok:
      return "ok";
    case RestoreStatus::UnsupportedSchema:
      return "unsupported_schema";
    case RestoreStatus::InvalidState:
      return "invalid_state";
  }
  return "unknown";
}

}  // namespace signal
}  // namespace kitsu868
