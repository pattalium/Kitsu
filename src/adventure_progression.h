#pragma once

#include <stddef.h>
#include <stdint.h>

#include "expedition_core.h"
#include "kitsu_party_hotspot.h"
#include "signal_encounter.h"

// Allocation-free adventure and exploration progression. The caller owns
// sensors, UI, storage, and policy. Precise coordinates are accepted only as
// transient method arguments; the persistence record contains coarse opaque
// zone tokens and aggregates, never latitude or longitude.
namespace kitsu868 {
namespace adventure {

constexpr uint8_t kProgressSchemaVersion = 1U;
constexpr uint8_t kMaximumZones = 5U;
constexpr uint8_t kRouteHistoryCapacity = 4U;
constexpr uint8_t kJournalCapacity = 5U;
constexpr uint8_t kMaximumMidDecisions = 3U;
constexpr uint8_t kRouteVariantCount = 12U;
constexpr uint8_t kNoPostcard = UINT8_MAX;
constexpr uint64_t kMinimumTrustedUnixSeconds = UINT64_C(1577836800);

enum class Terrain : uint8_t {
  Meadow = 0U,
  Forest,
  Ridge,
  Waterfront,
  Town,
  Count,
};

enum class Objective : uint8_t {
  Explore = 0U,
  FollowSignal,
  MeetCreature,
  Community,
  ReturnHome,
  Count,
};

enum class Risk : uint8_t {
  Careful = 0U,
  Balanced,
  Bold,
  Count,
};

enum class MidDecision : uint8_t {
  Continue = 0U,
  Detour,
  Help,
  ReturnEarly,
  Count,
};

enum class RoutePhase : uint8_t {
  Idle = 0U,
  Active,
  AwaitingRescue,
  Returned,
  Count,
};

enum class Outcome : uint8_t {
  None = 0U,
  Partial,
  Complete,
  EarlyReturn,
  Rescued,
  Count,
};

enum class PrivacyMode : uint8_t {
  Off = 0U,
  Coarse,
  PreciseTransient,
  Count,
};

enum class Familiarity : uint8_t {
  NewZone = 0U,
  Familiar,
  WellKnown,
  Home,
};

enum class Weather : uint8_t {
  Unknown = 0U,
  Clear,
  Rain,
  Wind,
  Snow,
  Count,
};

enum class TimeBand : uint8_t {
  Unknown = 0U,
  Dawn,
  Day,
  Dusk,
  Night,
  Count,
};

enum class MoonPhase : uint8_t {
  Unknown = 0U,
  NewMoon,
  Waxing,
  FullMoon,
  Waning,
  Count,
};

enum class Status : uint8_t {
  Ok = 0U,
  NoChange,
  Duplicate,
  InvalidArgument,
  InvalidClock,
  InvalidState,
  Busy,
  WrongPhase,
  PrivacyDisabled,
  PrivacyModeMismatch,
  DecisionLimit,
  CommuteRestricted,
  RescueRequired,
  WrongRoute,
  NotFound,
};

enum class RestoreStatus : uint8_t {
  Ok = 0U,
  BadMagic,
  UnsupportedSchema,
  InvalidState,
};

struct ClockSample {
  uint64_t unixSeconds = 0U;
  uint32_t dayId = 0U;
  uint16_t minuteOfDay = 0U;
  uint8_t trusted = 0U;
};

struct RouteRequest {
  Terrain terrain = Terrain::Meadow;
  Objective objective = Objective::Explore;
  Risk risk = Risk::Balanced;
  PersonalityKind personality = PersonalityKind::Gentle;
  Weather weather = Weather::Unknown;
  uint32_t baseTargetSteps = 1000U;
  uint8_t commuteSafe = 0U;
};

struct PreciseLocationSample {
  int32_t latitudeE7 = 0;
  int32_t longitudeE7 = 0;
  uint16_t accuracyMeters = 0U;
};

struct LocationUpdate {
  uint32_t zoneToken = 0U;
  uint32_t totalDistanceMeters = 0U;
  Familiarity familiarity = Familiarity::NewZone;
  uint8_t newZone = 0U;
  uint8_t homeComfort = 0U;
  uint8_t storedZoneCount = 0U;
};

struct HotspotUpdate {
  uint16_t hotspotVisits = 0U;
  uint16_t communityDayBonuses = 0U;
  uint8_t participantCount = 0U;
  uint8_t trustedTime = 0U;
  uint8_t communityDayActive = 0U;
  uint8_t bonusAwarded = 0U;
};

struct RouteView {
  RoutePhase phase = RoutePhase::Idle;
  Outcome outcome = Outcome::None;
  Terrain terrain = Terrain::Meadow;
  Objective objective = Objective::Explore;
  Risk risk = Risk::Balanced;
  PersonalityKind personality = PersonalityKind::Gentle;
  Weather weather = Weather::Unknown;
  TimeBand timeBand = TimeBand::Unknown;
  MoonPhase moonPhase = MoonPhase::Unknown;
  uint32_t routeId = 0U;
  uint32_t targetSteps = 0U;
  uint32_t steps = 0U;
  uint32_t routeDistanceMeters = 0U;
  uint8_t progressPercent = 0U;
  uint8_t decisionCount = 0U;
  uint8_t branchCode = 0U;
  uint8_t partySize = 0U;
  uint8_t chainDepth = 0U;
  uint8_t routeVariant = 0U;
  uint8_t commuteSafe = 0U;
  uint8_t communityDayActive = 0U;
  uint8_t homeComfort = 0U;
};

struct TextPostcard {
  const char* title = "NO POSTCARD";
  const char* line = "NOTHING TO REPORT";
};

#pragma pack(push, 1)
struct ZoneState {
  uint32_t token = 0U;
  uint16_t visits = 0U;
  uint16_t distanceDecameters = 0U;
};

struct JournalEntry {
  uint32_t routeId = 0U;
  uint32_t dayId = 0U;
  uint32_t zoneToken = 0U;
  uint8_t outcome = static_cast<uint8_t>(Outcome::None);
  uint8_t terrain = static_cast<uint8_t>(Terrain::Meadow);
  uint8_t objective = static_cast<uint8_t>(Objective::Explore);
  uint8_t postcardId = kNoPostcard;
  uint8_t partySize = 0U;
  uint8_t chainDepth = 0U;
  uint8_t routeVariant = 0U;
  uint8_t flags = 0U;
};

// Exact snapshot record. Numeric enum values and field order are schema 1.
// Zone tokens are caller-supplied coarse identifiers or salted hashes of a
// coarse cell. They are not raw coordinates.
struct ProgressState {
  uint32_t magic = UINT32_C(0x3150414B);  // "KAP1" little-endian.
  uint16_t bytes = 0U;
  uint8_t schemaVersion = kProgressSchemaVersion;
  uint8_t phase = static_cast<uint8_t>(RoutePhase::Idle);
  uint8_t privacyMode = static_cast<uint8_t>(PrivacyMode::Off);
  uint8_t outcome = static_cast<uint8_t>(Outcome::None);
  uint8_t terrain = static_cast<uint8_t>(Terrain::Meadow);
  uint8_t objective = static_cast<uint8_t>(Objective::Explore);
  uint8_t risk = static_cast<uint8_t>(Risk::Balanced);
  uint8_t personality = static_cast<uint8_t>(PersonalityKind::Gentle);
  uint8_t weather = static_cast<uint8_t>(Weather::Unknown);
  uint8_t timeBand = static_cast<uint8_t>(TimeBand::Unknown);
  uint8_t moonPhase = static_cast<uint8_t>(MoonPhase::Unknown);
  uint8_t decisionCount = 0U;
  uint8_t branchCode = 0U;
  uint8_t partySize = 0U;
  uint8_t chainDepth = 0U;
  uint8_t routeVariant = 0U;
  uint8_t historyCount = 0U;
  uint8_t historyNext = 0U;
  uint8_t journalCount = 0U;
  uint8_t journalNext = 0U;
  uint8_t zoneCount = 0U;
  uint8_t flags = 0U;
  uint16_t communityHotspotVisits = 0U;
  uint16_t communityDayBonuses = 0U;
  uint16_t progressPoints = 0U;
  uint16_t riskPoints = 0U;
  uint32_t sequence = 0U;
  uint32_t routeId = 0U;
  uint32_t routeSeed = 0U;
  uint32_t baseTargetSteps = 0U;
  uint32_t targetSteps = 0U;
  uint32_t steps = 0U;
  uint32_t routeDistanceMeters = 0U;
  uint32_t totalDistanceMeters = 0U;
  uint32_t currentZoneToken = 0U;
  uint32_t homeZoneToken = 0U;
  uint32_t locationSalt = 0U;
  uint32_t lastChainKey = 0U;
  uint32_t lastExpeditionId = 0U;
  uint32_t lastPartyProof = 0U;
  uint64_t lastEncounterOperationId = 0U;
  uint32_t lastHotspotToken = 0U;
  uint32_t lastHotspotDayId = 0U;
  uint32_t lastCommunityDayId = 0U;
  ZoneState zones[kMaximumZones]{};
  uint16_t routeHistory[kRouteHistoryCapacity]{};
  JournalEntry journal[kJournalCapacity]{};
  uint8_t reserved[8]{};
  uint32_t crc32 = 0U;
};
#pragma pack(pop)

static_assert(sizeof(ZoneState) == 8U, "zone snapshot layout changed");
static_assert(sizeof(JournalEntry) == 20U,
              "journal snapshot layout changed");
static_assert(sizeof(ProgressState) <= 288U,
              "adventure progression snapshot must remain compact");

bool validateProgressState(const ProgressState& state);
bool postcardForId(uint8_t postcardId, TextPostcard& output);
const char* statusName(Status status);

class AdventureProgression {
 public:
  AdventureProgression();

  void reset();
  RestoreStatus restore(const ProgressState& state);
  ProgressState snapshot() const { return state_; }

  Status setPrivacyMode(PrivacyMode mode, uint32_t locationSalt = 0U);
  Status setHomeZone(uint32_t coarseZoneToken);
  Status observeCoarseZone(uint32_t coarseZoneToken, uint32_t stepDelta,
                           uint32_t segmentMeters, LocationUpdate& output);
  Status observePreciseTransient(const PreciseLocationSample& sample,
                                 uint32_t stepDelta,
                                 uint32_t segmentMeters,
                                 LocationUpdate& output);
  Status observeHotspot(uint32_t hotspotToken, uint8_t participantCount,
                        const ClockSample& now, HotspotUpdate& output);

  Status begin(const RouteRequest& request, const ClockSample& now,
               uint32_t entropy);
  Status addSteps(uint32_t stepDelta);
  Status decide(MidDecision decision);

  // Value-type adapters for the existing deterministic engines. These only
  // consume their semantic DTOs; they do not own or mutate those subsystems.
  Status applyExpedition(
      const expedition::CompletionHooks& completion);
  Status applyParty(const party::HuntResult& result);
  Status applyEncounter(const signal::EncounterRecord& encounter);

  Status finish(const ClockSample& now);
  Status rescue(const party::HuntResult& result, const ClockSample& now);
  Status acknowledge(uint32_t routeId);

  RouteView view() const;
  bool currentPostcard(TextPostcard& output) const;
  uint8_t journalCount() const { return state_.journalCount; }
  bool journalNewest(uint8_t newestIndex, JournalEntry& output) const;

 private:
  ProgressState state_{};

  void refreshCrc();
  void recalculateTarget();
  void appendJournal(const ClockSample& now);
  void finalize(Outcome outcome, const ClockSample& now);
  Status observeZoneToken(uint32_t zoneToken, uint32_t stepDelta,
                          uint32_t segmentMeters, LocationUpdate& output);
};

}  // namespace adventure
}  // namespace kitsu868
