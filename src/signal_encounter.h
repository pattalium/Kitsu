#pragma once

#include <stddef.h>
#include <stdint.h>

// Portable decision engine for wild-creature encounters caused by successful,
// deduplicated radio activity. MeshCore operations and a first meeting with a
// nearby Kitsu may both trigger this local decision engine, but their over-air
// transports and packet formats remain completely separate. This module has
// no MeshCore, radio, storage, display, pack-catalogue, or creature-name
// dependency.
namespace kitsu868 {
namespace signal {

constexpr uint16_t kBasisPointScale = 10000U;
constexpr uint16_t kOnePercentBasisPoints = 100U;
constexpr uint8_t kRecordSchemaVersion = 1U;
constexpr uint8_t kCoordinatorStateSchemaVersion = 1U;

// A caller assigns one strictly increasing operationId after a logical
// operation completes successfully. Packet fragments, retries, repeat
// observations, and delivery-state updates reuse the original operationId and
// therefore cannot roll again.
enum class MeshOperationKind : uint8_t {
  RepeaterDiscovered = 0,
  PeerDiscovered,
  MessageSent,
  MessageReceived,
  AdvertSent,
  AdvertReceived,
  OtherCompleted,
  // Appended to preserve every persisted 0..6 operation value from schema 1.
  // This represents a first direct Kitsu-neighbor meeting, never a MeshCore
  // packet or repeater-forwarded operation.
  NearbyKitsuMet,
  Count,
};

// Numeric order is part of the persisted record contract.
enum class Rarity : uint8_t {
  Common = 0,
  Uncommon,
  Rare,
  VeryRare,
  Epic,
  Legendary,
  Mythical,
  Count,
};

enum class CodeOutcome : uint8_t {
  NotApplicable = 0,
  NotRevealed,
  Revealed,
};

constexpr size_t kMeshOperationKindCount =
    static_cast<size_t>(MeshOperationKind::Count);
constexpr size_t kRarityCount = static_cast<size_t>(Rarity::Count);

struct Configuration {
  // RepeaterDiscovered is unconditionally guaranteed by the engine. Its
  // entry is retained for stable indexing but is ignored when processing.
  uint16_t encounterChanceBasisPoints[kMeshOperationKindCount]{};

  // Weights must total 10,000. Mythical must be non-zero and below 100,
  // keeping its conditional and therefore total encounter probability below
  // one percent.
  uint16_t rarityWeightBasisPoints[kRarityCount]{};

  // Code resolution is an independent roll after a creature encounter. The
  // selected encounter rarity indexes this table.
  uint16_t codeChanceBasisPoints[kRarityCount]{};
};

enum class ConfigurationStatus : uint8_t {
  Ok = 0,
  EncounterChanceOutOfRange,
  RarityWeightTotalInvalid,
  MythicalWeightMustBeSubOnePercent,
  CodeChanceOutOfRange,
};

struct LogicalOperationEvent {
  uint64_t operationId = 0U;
  MeshOperationKind kind = MeshOperationKind::OtherCompleted;
  uint8_t successful = 0U;
};

// Fixed-width semantic DTO suitable for storage-backed reset recovery. It is
// intentionally not a raw on-flash/wire format: callers may store fields in
// their existing authenticated record container without relying on compiler
// padding or endianness.
struct EncounterRecord {
  uint8_t schemaVersion = kRecordSchemaVersion;
  uint8_t operationKind =
      static_cast<uint8_t>(MeshOperationKind::OtherCompleted);
  uint8_t encounterOccurred = 0U;
  uint8_t guaranteed = 0U;
  uint8_t rarity = static_cast<uint8_t>(Rarity::Common);
  uint8_t codeOutcome = static_cast<uint8_t>(CodeOutcome::NotApplicable);
  uint16_t encounterRollBasisPoints = 0U;
  uint16_t rarityRollBasisPoints = 0U;
  uint16_t codeRollBasisPoints = 0U;
  uint64_t operationId = 0U;
  uint32_t entropy = 0U;
};

struct CoordinatorState {
  uint8_t schemaVersion = kCoordinatorStateSchemaVersion;
  uint8_t hasLastRecord = 0U;
  uint16_t reserved = 0U;
  uint64_t lastOperationId = 0U;
  EncounterRecord lastRecord{};
};

enum class ProcessStatus : uint8_t {
  RecordedNoEncounter = 0,
  RecordedEncounter,
  InvalidConfiguration,
  InvalidEvent,
  UnsuccessfulOperation,
  DuplicateOperation,
  StaleOperation,
};

enum class RestoreStatus : uint8_t {
  Ok = 0,
  UnsupportedSchema,
  InvalidState,
};

ConfigurationStatus validateConfiguration(const Configuration& configuration);
const char* configurationStatusName(ConfigurationStatus status);

bool validOperationKind(MeshOperationKind kind);
bool validRarity(Rarity rarity);

// Pure helpers used by the coordinator and available to host/emulator tests.
bool rarityForRoll(const Configuration& configuration,
                   uint16_t rollBasisPoints, Rarity& output);
CodeOutcome codeOutcomeForRoll(const Configuration& configuration,
                               Rarity rarity, uint16_t rollBasisPoints);

bool validateRecord(const EncounterRecord& record);

class SignalEncounterCoordinator {
 public:
  explicit SignalEncounterCoordinator(const Configuration& configuration);

  ConfigurationStatus configurationStatus() const;

  // entropy is supplied by the platform (ESP hardware RNG in firmware,
  // deterministic injection in host/browser tests). The same state, event,
  // configuration, and entropy always produce the same record.
  ProcessStatus process(const LogicalOperationEvent& event, uint32_t entropy,
                        EncounterRecord& output);

  CoordinatorState snapshot() const;
  RestoreStatus restore(const CoordinatorState& state);
  void reset();

 private:
  Configuration configuration_{};
  ConfigurationStatus configurationStatus_ =
      ConfigurationStatus::RarityWeightTotalInvalid;
  CoordinatorState state_{};
};

const char* processStatusName(ProcessStatus status);
const char* restoreStatusName(RestoreStatus status);

}  // namespace signal
}  // namespace kitsu868
