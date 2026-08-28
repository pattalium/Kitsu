#include "adventure_progression.h"

#include <stddef.h>

namespace kitsu868 {
namespace adventure {
namespace {

constexpr uint32_t kStateMagic = UINT32_C(0x3150414B);
constexpr uint8_t kFlagCommuteSafe = 1U << 0;
constexpr uint8_t kFlagEarlyReturn = 1U << 1;
constexpr uint8_t kFlagCurrentZoneNew = 1U << 2;
constexpr uint8_t kFlagChainEligible = 1U << 3;
constexpr uint8_t kFlagTrustedStartClock = 1U << 4;
constexpr uint8_t kFlagCommunityDay = 1U << 5;
constexpr uint8_t kAllowedFlags = kFlagCommuteSafe | kFlagEarlyReturn |
    kFlagCurrentZoneNew | kFlagChainEligible | kFlagTrustedStartClock |
    kFlagCommunityDay;
constexpr uint8_t kJournalFlagTrustedTime = 1U << 0;
constexpr uint8_t kJournalFlagCommunityDay = 1U << 1;
constexpr uint8_t kJournalFlagHomeComfort = 1U << 2;
constexpr uint8_t kJournalFlagCommuteSafe = 1U << 3;
constexpr uint8_t kPostcardCount = 10U;
constexpr uint32_t kMaximumStepDelta = UINT32_C(1000000);
constexpr uint32_t kMaximumSegmentMeters = UINT32_C(200000);

struct PostcardRow {
  const char* title;
  const char* line;
};

constexpr PostcardRow kPostcards[kPostcardCount] = {
    {"MEADOW NOTE", "THE PATH KEPT GOING"},
    {"FOREST NOTE", "LEAVES HID THE SIGNAL"},
    {"HIGH NOTE", "THE VIEW WAS WORTH IT"},
    {"TOWN NOTE", "I FOUND A NEW CORNER"},
    {"HALF A TRAIL", "I BROUGHT BACK A CLUE"},
    {"SMALL DETOUR", "NEXT TIME I GO FARTHER"},
    {"EARLY POST", "HOME WON THIS ROUND"},
    {"SAFE RETURN", "I TOOK THE QUIET WAY"},
    {"RESCUE ECHO", "A FRIEND FOUND MY SIGNAL"},
    {"PARTY RESCUE", "EVERYONE ANSWERED"},
};

uint32_t saturatingAddU32(uint32_t left, uint32_t right) {
  return right > UINT32_MAX - left ? UINT32_MAX : left + right;
}

uint16_t saturatingAddU16(uint16_t left, uint32_t right) {
  return right > static_cast<uint32_t>(UINT16_MAX - left)
      ? UINT16_MAX
      : static_cast<uint16_t>(left + right);
}

uint32_t mix32(uint32_t value) {
  value ^= value >> 16U;
  value *= UINT32_C(0x7FEB352D);
  value ^= value >> 15U;
  value *= UINT32_C(0x846CA68B);
  value ^= value >> 16U;
  return value;
}

uint32_t stateCrc(const ProgressState& state) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&state);
  uint32_t crc = UINT32_C(0xFFFFFFFF);
  for (size_t index = 0U; index < offsetof(ProgressState, crc32); ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (UINT32_C(0xEDB88320) & mask);
    }
  }
  return ~crc;
}

bool validTerrain(uint8_t value) {
  return value < static_cast<uint8_t>(Terrain::Count);
}

bool validObjective(uint8_t value) {
  return value < static_cast<uint8_t>(Objective::Count);
}

bool validRisk(uint8_t value) {
  return value < static_cast<uint8_t>(Risk::Count);
}

bool validPersonality(uint8_t value) {
  return value <= static_cast<uint8_t>(PersonalityKind::Impish);
}

bool validWeather(uint8_t value) {
  return value < static_cast<uint8_t>(Weather::Count);
}

bool validTimeBand(uint8_t value) {
  return value < static_cast<uint8_t>(TimeBand::Count);
}

bool validMoon(uint8_t value) {
  return value < static_cast<uint8_t>(MoonPhase::Count);
}

bool validPhase(uint8_t value) {
  return value < static_cast<uint8_t>(RoutePhase::Count);
}

bool validOutcome(uint8_t value) {
  return value < static_cast<uint8_t>(Outcome::Count);
}

bool validPrivacy(uint8_t value) {
  return value < static_cast<uint8_t>(PrivacyMode::Count);
}

bool validDecision(MidDecision decision) {
  return static_cast<uint8_t>(decision) <
      static_cast<uint8_t>(MidDecision::Count);
}

bool validClock(const ClockSample& clock) {
  if (clock.trusted > 1U) return false;
  if (clock.trusted == 0U) return true;
  return clock.unixSeconds >= kMinimumTrustedUnixSeconds &&
      clock.dayId != 0U && clock.minuteOfDay < 1440U;
}

TimeBand timeBandFor(const ClockSample& clock) {
  if (clock.trusted == 0U) return TimeBand::Unknown;
  if (clock.minuteOfDay >= 300U && clock.minuteOfDay < 480U) {
    return TimeBand::Dawn;
  }
  if (clock.minuteOfDay < 1020U && clock.minuteOfDay >= 480U) {
    return TimeBand::Day;
  }
  if (clock.minuteOfDay >= 1020U && clock.minuteOfDay < 1200U) {
    return TimeBand::Dusk;
  }
  return TimeBand::Night;
}

MoonPhase moonFor(const ClockSample& clock) {
  if (clock.trusted == 0U) return MoonPhase::Unknown;
  const uint8_t lunarDay = static_cast<uint8_t>(
      (clock.unixSeconds / UINT64_C(86400)) % UINT64_C(29));
  if (lunarDay < 4U) return MoonPhase::NewMoon;
  if (lunarDay < 11U) return MoonPhase::Waxing;
  if (lunarDay < 18U) return MoonPhase::FullMoon;
  return MoonPhase::Waning;
}

bool communityDayFor(const ClockSample& clock) {
  return clock.trusted != 0U && (clock.dayId % 7U) == 0U;
}

bool emptyZone(const ZoneState& zone) {
  return zone.token == 0U && zone.visits == 0U &&
      zone.distanceDecameters == 0U;
}

bool emptyJournal(const JournalEntry& entry) {
  return entry.routeId == 0U && entry.dayId == 0U &&
      entry.zoneToken == 0U &&
      entry.outcome == static_cast<uint8_t>(Outcome::None) &&
      entry.terrain == static_cast<uint8_t>(Terrain::Meadow) &&
      entry.objective == static_cast<uint8_t>(Objective::Explore) &&
      entry.postcardId == kNoPostcard && entry.partySize == 0U &&
      entry.chainDepth == 0U && entry.routeVariant == 0U &&
      entry.flags == 0U;
}

bool validPartySize(uint8_t partySize) {
  return partySize == 0U ||
      (partySize >= party::kMinimumParticipants &&
       partySize <= party::kMaximumParticipants);
}

bool validJournal(const JournalEntry& entry) {
  if (entry.routeId == 0U ||
      entry.outcome <= static_cast<uint8_t>(Outcome::None) ||
      entry.outcome >= static_cast<uint8_t>(Outcome::Count) ||
      !validTerrain(entry.terrain) || !validObjective(entry.objective) ||
      entry.postcardId >= kPostcardCount || !validPartySize(entry.partySize) ||
      entry.chainDepth == 0U || entry.routeVariant >= kRouteVariantCount ||
      (entry.flags & ~(kJournalFlagTrustedTime |
                       kJournalFlagCommunityDay |
                       kJournalFlagHomeComfort |
                       kJournalFlagCommuteSafe)) != 0U) {
    return false;
  }
  if (((entry.flags & kJournalFlagTrustedTime) != 0U) !=
      (entry.dayId != 0U)) {
    return false;
  }
  return (entry.flags & kJournalFlagCommunityDay) == 0U ||
      (entry.flags & kJournalFlagTrustedTime) != 0U;
}

uint8_t progressPercent(const ProgressState& state) {
  if (state.targetSteps == 0U) return 0U;
  uint32_t stepPercent = state.steps >= state.targetSteps
      ? 100U
      : static_cast<uint32_t>(
            (static_cast<uint64_t>(state.steps) * 100U) /
            state.targetSteps);
  const uint32_t decisionAndHookBonus =
      state.progressPoints > 30U ? 30U : state.progressPoints;
  const uint32_t distanceBonus = state.routeDistanceMeters / 250U > 10U
      ? 10U
      : state.routeDistanceMeters / 250U;
  stepPercent += decisionAndHookBonus + distanceBonus;
  return static_cast<uint8_t>(stepPercent > 100U ? 100U : stepPercent);
}

uint16_t routeSignature(Terrain terrain, Objective objective,
                        uint8_t variant) {
  return static_cast<uint16_t>(
      1U + static_cast<uint16_t>(static_cast<uint8_t>(terrain)) * 60U +
      static_cast<uint16_t>(static_cast<uint8_t>(objective)) * 12U +
      variant);
}

bool historyContains(const ProgressState& state, uint16_t signature) {
  for (uint8_t index = 0U; index < state.historyCount; ++index) {
    if (state.routeHistory[index] == signature) return true;
  }
  return false;
}

void appendHistory(ProgressState& state, uint16_t signature) {
  state.routeHistory[state.historyNext] = signature;
  if (state.historyCount < kRouteHistoryCapacity) ++state.historyCount;
  state.historyNext = static_cast<uint8_t>(
      (state.historyNext + 1U) % kRouteHistoryCapacity);
}

uint32_t chainKeyFor(const RouteRequest& request) {
  uint32_t value = static_cast<uint32_t>(
      static_cast<uint8_t>(request.terrain));
  value |= static_cast<uint32_t>(
      static_cast<uint8_t>(request.objective)) << 4U;
  value |= static_cast<uint32_t>(
      static_cast<uint8_t>(request.personality)) << 8U;
  value = mix32(value ^ UINT32_C(0xA5C31D27));
  return value == 0U ? 1U : value;
}

bool validPartyResult(const party::HuntResult& result) {
  return result.participantCount >= party::kMinimumParticipants &&
      result.participantCount <= party::kMaximumParticipants &&
      result.completedRounds == party::kHuntRounds &&
      result.tier <= static_cast<uint8_t>(party::ResultTier::Resonant) &&
      result.maximumScore != 0U && result.score <= result.maximumScore &&
      result.proof != 0U;
}

bool validEncounterRecord(const signal::EncounterRecord& encounter) {
  if (encounter.schemaVersion != signal::kRecordSchemaVersion ||
      encounter.operationId == 0U ||
      encounter.operationKind >=
          static_cast<uint8_t>(signal::MeshOperationKind::Count) ||
      encounter.encounterOccurred > 1U || encounter.guaranteed > 1U ||
      encounter.rarity >= static_cast<uint8_t>(signal::Rarity::Count) ||
      encounter.codeOutcome >
          static_cast<uint8_t>(signal::CodeOutcome::Revealed) ||
      encounter.encounterRollBasisPoints >= signal::kBasisPointScale ||
      encounter.rarityRollBasisPoints >= signal::kBasisPointScale ||
      encounter.codeRollBasisPoints >= signal::kBasisPointScale) {
    return false;
  }
  if (encounter.encounterOccurred == 0U) {
    return encounter.guaranteed == 0U &&
        encounter.codeOutcome ==
            static_cast<uint8_t>(signal::CodeOutcome::NotApplicable);
  }
  return true;
}

int32_t floorDivide(int32_t value, int32_t divisor) {
  int32_t quotient = value / divisor;
  const int32_t remainder = value % divisor;
  if (remainder != 0 && value < 0) --quotient;
  return quotient;
}

uint32_t preciseZoneToken(const PreciseLocationSample& sample,
                          uint32_t salt) {
  // A 0.025-degree grid is deliberately coarse. Only the salted mixed token
  // leaves this stack frame; neither cell coordinate is persisted.
  const int32_t latitudeCell = floorDivide(sample.latitudeE7, 250000);
  const int32_t longitudeCell = floorDivide(sample.longitudeE7, 250000);
  uint32_t value = static_cast<uint32_t>(latitudeCell) *
      UINT32_C(0x9E3779B9);
  value ^= static_cast<uint32_t>(longitudeCell) * UINT32_C(0x85EBCA6B);
  value = mix32(value ^ salt ^ UINT32_C(0xC2B2AE35));
  return value == 0U ? 1U : value;
}

uint8_t postcardIdFor(const ProgressState& state) {
  const Outcome outcome = static_cast<Outcome>(state.outcome);
  switch (outcome) {
    case Outcome::Complete:
      return static_cast<uint8_t>(
          (state.terrain + state.personality) % 4U);
    case Outcome::Partial:
      return static_cast<uint8_t>(4U + (state.objective & 1U));
    case Outcome::EarlyReturn:
      return static_cast<uint8_t>(6U + (state.terrain & 1U));
    case Outcome::Rescued:
      return state.partySize == party::kMaximumParticipants ? 9U : 8U;
    case Outcome::None:
    case Outcome::Count:
      break;
  }
  return kNoPostcard;
}

Familiarity familiarityFor(const ZoneState& zone, bool home) {
  if (home) return Familiarity::Home;
  if (zone.visits <= 1U) return Familiarity::NewZone;
  if (zone.visits < 4U) return Familiarity::Familiar;
  return Familiarity::WellKnown;
}

}  // namespace

bool postcardForId(uint8_t postcardId, TextPostcard& output) {
  if (postcardId >= kPostcardCount) return false;
  output.title = kPostcards[postcardId].title;
  output.line = kPostcards[postcardId].line;
  return true;
}

const char* statusName(Status status) {
  switch (status) {
    case Status::Ok: return "ok";
    case Status::NoChange: return "no_change";
    case Status::Duplicate: return "duplicate";
    case Status::InvalidArgument: return "invalid_argument";
    case Status::InvalidClock: return "invalid_clock";
    case Status::InvalidState: return "invalid_state";
    case Status::Busy: return "busy";
    case Status::WrongPhase: return "wrong_phase";
    case Status::PrivacyDisabled: return "privacy_disabled";
    case Status::PrivacyModeMismatch: return "privacy_mode_mismatch";
    case Status::DecisionLimit: return "decision_limit";
    case Status::CommuteRestricted: return "commute_restricted";
    case Status::RescueRequired: return "rescue_required";
    case Status::WrongRoute: return "wrong_route";
    case Status::NotFound: return "not_found";
  }
  return "unknown";
}

bool validateProgressState(const ProgressState& state) {
  if (state.magic != kStateMagic ||
      state.bytes != static_cast<uint16_t>(sizeof(ProgressState)) ||
      state.schemaVersion != kProgressSchemaVersion ||
      state.crc32 != stateCrc(state) || !validPhase(state.phase) ||
      !validPrivacy(state.privacyMode) || !validOutcome(state.outcome) ||
      (state.flags & ~kAllowedFlags) != 0U ||
      state.historyCount > kRouteHistoryCapacity ||
      state.historyNext >= kRouteHistoryCapacity ||
      state.journalCount > kJournalCapacity ||
      state.journalNext >= kJournalCapacity ||
      state.zoneCount > kMaximumZones) {
    return false;
  }
  for (size_t index = 0U; index < sizeof(state.reserved); ++index) {
    if (state.reserved[index] != 0U) return false;
  }

  if (state.historyCount < kRouteHistoryCapacity) {
    if (state.historyNext != state.historyCount) return false;
    for (uint8_t index = state.historyCount;
         index < kRouteHistoryCapacity; ++index) {
      if (state.routeHistory[index] != 0U) return false;
    }
  }
  for (uint8_t index = 0U; index < state.historyCount; ++index) {
    if (state.routeHistory[index] == 0U) return false;
  }

  if (state.journalCount < kJournalCapacity) {
    if (state.journalNext != state.journalCount) return false;
    for (uint8_t index = state.journalCount;
         index < kJournalCapacity; ++index) {
      if (!emptyJournal(state.journal[index])) return false;
    }
  }
  for (uint8_t index = 0U; index < state.journalCount; ++index) {
    if (!validJournal(state.journal[index])) return false;
  }

  for (uint8_t index = 0U; index < state.zoneCount; ++index) {
    if (state.zones[index].token == 0U ||
        state.zones[index].visits == 0U) {
      return false;
    }
    for (uint8_t other = static_cast<uint8_t>(index + 1U);
         other < state.zoneCount; ++other) {
      if (state.zones[index].token == state.zones[other].token) return false;
    }
  }
  for (uint8_t index = state.zoneCount; index < kMaximumZones; ++index) {
    if (!emptyZone(state.zones[index])) return false;
  }

  const PrivacyMode privacy = static_cast<PrivacyMode>(state.privacyMode);
  if (privacy == PrivacyMode::Off) {
    if (state.currentZoneToken != 0U || state.homeZoneToken != 0U ||
        state.locationSalt != 0U || state.totalDistanceMeters != 0U ||
        state.routeDistanceMeters != 0U || state.zoneCount != 0U ||
        (state.flags & kFlagCurrentZoneNew) != 0U) {
      return false;
    }
  } else if (privacy == PrivacyMode::Coarse) {
    if (state.locationSalt != 0U) return false;
  } else if (state.locationSalt == 0U) {
    return false;
  }

  if (state.currentZoneToken != 0U) {
    bool found = false;
    for (uint8_t index = 0U; index < state.zoneCount; ++index) {
      if (state.zones[index].token == state.currentZoneToken) found = true;
    }
    if (!found) return false;
  }

  if (state.communityDayBonuses == 0U &&
      state.lastCommunityDayId != 0U) {
    return false;
  }
  if (state.lastHotspotToken == 0U && state.lastHotspotDayId != 0U) {
    return false;
  }

  const RoutePhase phase = static_cast<RoutePhase>(state.phase);
  if (phase == RoutePhase::Idle) {
    if (state.outcome != static_cast<uint8_t>(Outcome::None) ||
        state.routeId != 0U || state.routeSeed != 0U ||
        state.baseTargetSteps != 0U || state.targetSteps != 0U ||
        state.steps != 0U || state.routeDistanceMeters != 0U ||
        state.progressPoints != 0U || state.riskPoints != 0U ||
        state.decisionCount != 0U || state.branchCode != 0U ||
        state.partySize != 0U || state.routeVariant != 0U ||
        state.timeBand != static_cast<uint8_t>(TimeBand::Unknown) ||
        state.moonPhase != static_cast<uint8_t>(MoonPhase::Unknown) ||
        (state.flags & (kFlagCommuteSafe | kFlagEarlyReturn |
                        kFlagTrustedStartClock | kFlagCommunityDay)) != 0U) {
      return false;
    }
    return true;
  }

  if (!validTerrain(state.terrain) || !validObjective(state.objective) ||
      !validRisk(state.risk) || !validPersonality(state.personality) ||
      !validWeather(state.weather) || !validTimeBand(state.timeBand) ||
      !validMoon(state.moonPhase) || state.sequence == 0U ||
      state.routeId == 0U || state.routeSeed == 0U ||
      state.baseTargetSteps < 100U || state.baseTargetSteps > 100000U ||
      state.targetSteps < 100U || state.targetSteps > 100000U ||
      state.decisionCount > kMaximumMidDecisions ||
      !validPartySize(state.partySize) || state.chainDepth == 0U ||
      state.routeVariant >= kRouteVariantCount) {
    return false;
  }
  if (state.partySize != 0U && state.lastPartyProof == 0U) return false;
  if ((state.flags & kFlagCommuteSafe) != 0U &&
      state.risk != static_cast<uint8_t>(Risk::Careful)) {
    return false;
  }
  if ((state.flags & kFlagTrustedStartClock) == 0U) {
    if (state.timeBand != static_cast<uint8_t>(TimeBand::Unknown) ||
        state.moonPhase != static_cast<uint8_t>(MoonPhase::Unknown) ||
        (state.flags & kFlagCommunityDay) != 0U) {
      return false;
    }
  }

  if (phase == RoutePhase::Returned) {
    return state.outcome > static_cast<uint8_t>(Outcome::None) &&
        state.outcome < static_cast<uint8_t>(Outcome::Count);
  }
  if (state.outcome != static_cast<uint8_t>(Outcome::None)) return false;
  if (phase == RoutePhase::AwaitingRescue) {
    return state.risk == static_cast<uint8_t>(Risk::Bold) &&
        (state.flags & kFlagEarlyReturn) == 0U;
  }
  return phase == RoutePhase::Active;
}

AdventureProgression::AdventureProgression() {
  reset();
}

void AdventureProgression::refreshCrc() {
  state_.crc32 = stateCrc(state_);
}

void AdventureProgression::reset() {
  state_ = ProgressState{};
  state_.bytes = static_cast<uint16_t>(sizeof(ProgressState));
  refreshCrc();
}

RestoreStatus AdventureProgression::restore(const ProgressState& state) {
  if (state.magic != kStateMagic) return RestoreStatus::BadMagic;
  if (state.schemaVersion != kProgressSchemaVersion ||
      state.bytes != static_cast<uint16_t>(sizeof(ProgressState))) {
    return RestoreStatus::UnsupportedSchema;
  }
  if (!validateProgressState(state)) return RestoreStatus::InvalidState;
  state_ = state;
  return RestoreStatus::Ok;
}

Status AdventureProgression::setPrivacyMode(PrivacyMode mode,
                                            uint32_t locationSalt) {
  if (!validateProgressState(state_)) return Status::InvalidState;
  if (!validPrivacy(static_cast<uint8_t>(mode))) {
    return Status::InvalidArgument;
  }
  if (mode == PrivacyMode::PreciseTransient && locationSalt == 0U) {
    return Status::InvalidArgument;
  }
  if (mode != PrivacyMode::PreciseTransient) locationSalt = 0U;
  if (state_.privacyMode == static_cast<uint8_t>(mode) &&
      state_.locationSalt == locationSalt) {
    return Status::NoChange;
  }

  state_.privacyMode = static_cast<uint8_t>(mode);
  state_.locationSalt = locationSalt;
  state_.currentZoneToken = 0U;
  state_.homeZoneToken = 0U;
  state_.totalDistanceMeters = 0U;
  state_.routeDistanceMeters = 0U;
  state_.zoneCount = 0U;
  state_.flags &= static_cast<uint8_t>(~kFlagCurrentZoneNew);
  for (uint8_t index = 0U; index < kMaximumZones; ++index) {
    state_.zones[index] = ZoneState{};
  }
  recalculateTarget();
  refreshCrc();
  return Status::Ok;
}

Status AdventureProgression::setHomeZone(uint32_t coarseZoneToken) {
  if (!validateProgressState(state_)) return Status::InvalidState;
  if (state_.privacyMode == static_cast<uint8_t>(PrivacyMode::Off)) {
    return Status::PrivacyDisabled;
  }
  if (coarseZoneToken == 0U) return Status::InvalidArgument;
  if (state_.homeZoneToken == coarseZoneToken) return Status::NoChange;
  state_.homeZoneToken = coarseZoneToken;
  recalculateTarget();
  refreshCrc();
  return Status::Ok;
}

Status AdventureProgression::observeCoarseZone(
    uint32_t coarseZoneToken, uint32_t stepDelta, uint32_t segmentMeters,
    LocationUpdate& output) {
  if (!validateProgressState(state_)) return Status::InvalidState;
  if (state_.privacyMode == static_cast<uint8_t>(PrivacyMode::Off)) {
    return Status::PrivacyDisabled;
  }
  if (state_.privacyMode != static_cast<uint8_t>(PrivacyMode::Coarse)) {
    return Status::PrivacyModeMismatch;
  }
  if (coarseZoneToken == 0U) return Status::InvalidArgument;
  return observeZoneToken(coarseZoneToken, stepDelta, segmentMeters, output);
}

Status AdventureProgression::observePreciseTransient(
    const PreciseLocationSample& sample, uint32_t stepDelta,
    uint32_t segmentMeters, LocationUpdate& output) {
  if (!validateProgressState(state_)) return Status::InvalidState;
  if (state_.privacyMode == static_cast<uint8_t>(PrivacyMode::Off)) {
    return Status::PrivacyDisabled;
  }
  if (state_.privacyMode !=
      static_cast<uint8_t>(PrivacyMode::PreciseTransient)) {
    return Status::PrivacyModeMismatch;
  }
  if (sample.latitudeE7 < -900000000 || sample.latitudeE7 > 900000000 ||
      sample.longitudeE7 < -1800000000 ||
      sample.longitudeE7 > 1800000000 || sample.accuracyMeters == 0U ||
      sample.accuracyMeters > 10000U) {
    return Status::InvalidArgument;
  }
  return observeZoneToken(
      preciseZoneToken(sample, state_.locationSalt), stepDelta,
      segmentMeters, output);
}

Status AdventureProgression::observeZoneToken(
    uint32_t zoneToken, uint32_t stepDelta, uint32_t segmentMeters,
    LocationUpdate& output) {
  if (stepDelta > kMaximumStepDelta ||
      segmentMeters > kMaximumSegmentMeters) {
    return Status::InvalidArgument;
  }

  uint8_t slot = kMaximumZones;
  for (uint8_t index = 0U; index < state_.zoneCount; ++index) {
    if (state_.zones[index].token == zoneToken) {
      slot = index;
      break;
    }
  }
  bool isNew = slot == kMaximumZones;
  if (isNew) {
    if (state_.zoneCount < kMaximumZones) {
      slot = state_.zoneCount++;
    } else {
      slot = 0U;
      for (uint8_t index = 1U; index < kMaximumZones; ++index) {
        if (state_.zones[index].visits < state_.zones[slot].visits) {
          slot = index;
        }
      }
    }
    state_.zones[slot] = ZoneState{};
    state_.zones[slot].token = zoneToken;
  }

  ZoneState& zone = state_.zones[slot];
  zone.visits = saturatingAddU16(zone.visits, 1U);
  uint32_t decameters = segmentMeters / 10U;
  if (segmentMeters != 0U && decameters == 0U) decameters = 1U;
  zone.distanceDecameters = saturatingAddU16(
      zone.distanceDecameters, decameters);
  state_.currentZoneToken = zoneToken;
  if (isNew) {
    state_.flags |= kFlagCurrentZoneNew;
  } else {
    state_.flags &= static_cast<uint8_t>(~kFlagCurrentZoneNew);
  }
  state_.totalDistanceMeters = saturatingAddU32(
      state_.totalDistanceMeters, segmentMeters);

  if (state_.phase == static_cast<uint8_t>(RoutePhase::Active)) {
    state_.steps = saturatingAddU32(state_.steps, stepDelta);
    state_.routeDistanceMeters = saturatingAddU32(
        state_.routeDistanceMeters, segmentMeters);
    recalculateTarget();
  }

  const bool home = state_.homeZoneToken != 0U &&
      state_.homeZoneToken == zoneToken;
  output.zoneToken = zoneToken;
  output.totalDistanceMeters = state_.totalDistanceMeters;
  output.familiarity = familiarityFor(zone, home);
  output.newZone = isNew ? 1U : 0U;
  output.homeComfort = home ? 1U : 0U;
  output.storedZoneCount = state_.zoneCount;
  refreshCrc();
  return Status::Ok;
}

Status AdventureProgression::observeHotspot(
    uint32_t hotspotToken, uint8_t participantCount,
    const ClockSample& now, HotspotUpdate& output) {
  if (!validateProgressState(state_)) return Status::InvalidState;
  if (!validClock(now)) return Status::InvalidClock;
  if (hotspotToken == 0U ||
      participantCount < party::kMinimumParticipants ||
      participantCount > party::kMaximumParticipants) {
    return Status::InvalidArgument;
  }
  if (now.trusted != 0U && state_.lastHotspotToken == hotspotToken &&
      state_.lastHotspotDayId == now.dayId) {
    output.hotspotVisits = state_.communityHotspotVisits;
    output.communityDayBonuses = state_.communityDayBonuses;
    output.participantCount = participantCount;
    output.trustedTime = 1U;
    output.communityDayActive = communityDayFor(now) ? 1U : 0U;
    output.bonusAwarded = 0U;
    return Status::Duplicate;
  }

  state_.communityHotspotVisits = saturatingAddU16(
      state_.communityHotspotVisits, 1U);
  state_.lastHotspotToken = hotspotToken;
  state_.lastHotspotDayId = now.trusted != 0U ? now.dayId : 0U;
  const bool communityDay = communityDayFor(now);
  bool bonus = false;
  if (communityDay && state_.lastCommunityDayId != now.dayId) {
    state_.lastCommunityDayId = now.dayId;
    state_.communityDayBonuses = saturatingAddU16(
        state_.communityDayBonuses, 1U);
    if (state_.phase == static_cast<uint8_t>(RoutePhase::Active)) {
      state_.progressPoints = saturatingAddU16(
          state_.progressPoints, participantCount);
    }
    bonus = true;
  }

  output.hotspotVisits = state_.communityHotspotVisits;
  output.communityDayBonuses = state_.communityDayBonuses;
  output.participantCount = participantCount;
  output.trustedTime = now.trusted;
  output.communityDayActive = communityDay ? 1U : 0U;
  output.bonusAwarded = bonus ? 1U : 0U;
  refreshCrc();
  return Status::Ok;
}

Status AdventureProgression::begin(const RouteRequest& request,
                                   const ClockSample& now,
                                   uint32_t entropy) {
  if (!validateProgressState(state_)) return Status::InvalidState;
  if (state_.phase != static_cast<uint8_t>(RoutePhase::Idle)) {
    return Status::Busy;
  }
  if (!validTerrain(static_cast<uint8_t>(request.terrain)) ||
      !validObjective(static_cast<uint8_t>(request.objective)) ||
      !validRisk(static_cast<uint8_t>(request.risk)) ||
      !validPersonality(static_cast<uint8_t>(request.personality)) ||
      !validWeather(static_cast<uint8_t>(request.weather)) ||
      request.baseTargetSteps < 100U ||
      request.baseTargetSteps > 100000U || request.commuteSafe > 1U) {
    return Status::InvalidArgument;
  }
  if (!validClock(now)) return Status::InvalidClock;
  if (request.commuteSafe != 0U && request.risk != Risk::Careful) {
    return Status::CommuteRestricted;
  }
  if (state_.sequence == UINT32_MAX) return Status::InvalidState;

  const uint32_t nextSequence = state_.sequence + 1U;
  const uint32_t chainKey = chainKeyFor(request);
  const bool continuingChain =
      (state_.flags & kFlagChainEligible) != 0U &&
      state_.lastChainKey == chainKey;
  uint8_t chainDepth = 1U;
  if (continuingChain) {
    chainDepth = state_.chainDepth == UINT8_MAX
        ? UINT8_MAX
        : static_cast<uint8_t>(state_.chainDepth + 1U);
  }

  const TimeBand timeBand = timeBandFor(now);
  const MoonPhase moonPhase = moonFor(now);
  uint32_t seed = entropy ^ (nextSequence * UINT32_C(0x9E3779B9));
  seed ^= static_cast<uint32_t>(static_cast<uint8_t>(request.terrain));
  seed ^= static_cast<uint32_t>(
      static_cast<uint8_t>(request.objective)) << 4U;
  seed ^= static_cast<uint32_t>(static_cast<uint8_t>(request.risk)) << 8U;
  seed ^= static_cast<uint32_t>(
      static_cast<uint8_t>(request.personality)) << 12U;
  seed ^= static_cast<uint32_t>(static_cast<uint8_t>(request.weather)) << 16U;
  seed ^= static_cast<uint32_t>(static_cast<uint8_t>(timeBand)) << 20U;
  seed ^= static_cast<uint32_t>(static_cast<uint8_t>(moonPhase)) << 24U;
  seed ^= state_.currentZoneToken ^ chainKey;
  seed = mix32(seed);
  if (seed == 0U) seed = UINT32_C(0x6D2B79F5);

  uint8_t variant = static_cast<uint8_t>(seed % kRouteVariantCount);
  for (uint8_t attempt = 0U; attempt < kRouteVariantCount; ++attempt) {
    if (!historyContains(
            state_, routeSignature(request.terrain, request.objective,
                                   variant))) {
      break;
    }
    variant = static_cast<uint8_t>((variant + 1U) % kRouteVariantCount);
  }
  const uint16_t signature = routeSignature(
      request.terrain, request.objective, variant);
  uint32_t routeId = mix32(seed ^ nextSequence ^ signature ^
                           UINT32_C(0xD1B54A35));
  if (routeId == 0U) routeId = 1U;

  state_.phase = static_cast<uint8_t>(RoutePhase::Active);
  state_.outcome = static_cast<uint8_t>(Outcome::None);
  state_.terrain = static_cast<uint8_t>(request.terrain);
  state_.objective = static_cast<uint8_t>(request.objective);
  state_.risk = static_cast<uint8_t>(request.risk);
  state_.personality = static_cast<uint8_t>(request.personality);
  state_.weather = static_cast<uint8_t>(request.weather);
  state_.timeBand = static_cast<uint8_t>(timeBand);
  state_.moonPhase = static_cast<uint8_t>(moonPhase);
  state_.decisionCount = 0U;
  state_.branchCode = 0U;
  state_.partySize = 0U;
  state_.chainDepth = chainDepth;
  state_.routeVariant = variant;
  state_.progressPoints = 0U;
  state_.riskPoints = 0U;
  state_.sequence = nextSequence;
  state_.routeId = routeId;
  state_.routeSeed = seed;
  state_.baseTargetSteps = request.baseTargetSteps;
  state_.steps = 0U;
  state_.routeDistanceMeters = 0U;
  state_.lastChainKey = chainKey;
  state_.flags &= kFlagCurrentZoneNew;
  if (request.commuteSafe != 0U) state_.flags |= kFlagCommuteSafe;
  if (now.trusted != 0U) state_.flags |= kFlagTrustedStartClock;
  if (communityDayFor(now)) state_.flags |= kFlagCommunityDay;
  appendHistory(state_, signature);
  recalculateTarget();
  refreshCrc();
  return Status::Ok;
}

void AdventureProgression::recalculateTarget() {
  if (state_.phase == static_cast<uint8_t>(RoutePhase::Idle) ||
      state_.baseTargetSteps == 0U) {
    state_.targetSteps = 0U;
    return;
  }

  int32_t percent = 100;
  const Terrain terrain = static_cast<Terrain>(state_.terrain);
  if (terrain == Terrain::Forest || terrain == Terrain::Waterfront) {
    percent += 10;
  } else if (terrain == Terrain::Ridge) {
    percent += 20;
  } else if (terrain == Terrain::Town) {
    percent -= 10;
  }
  const Objective objective = static_cast<Objective>(state_.objective);
  if (objective == Objective::FollowSignal) percent += 5;
  if (objective == Objective::MeetCreature) percent += 10;
  if (objective == Objective::Community) percent -= 10;
  if (objective == Objective::ReturnHome) percent -= 20;

  const Risk risk = static_cast<Risk>(state_.risk);
  if (risk == Risk::Careful) percent -= 10;
  if (risk == Risk::Bold) percent += 15;

  const PersonalityKind personality =
      static_cast<PersonalityKind>(state_.personality);
  if ((personality == PersonalityKind::Gentle && risk == Risk::Careful) ||
      (personality == PersonalityKind::Bold && risk == Risk::Bold) ||
      (personality == PersonalityKind::Curious &&
       (objective == Objective::Explore ||
        objective == Objective::FollowSignal)) ||
      (personality == PersonalityKind::Playful &&
       objective == Objective::Community)) {
    percent -= 5;
  }

  if (state_.homeZoneToken != 0U &&
      state_.homeZoneToken == state_.currentZoneToken) {
    percent -= 15;
  }
  if (state_.partySize == 2U) percent -= 10;
  if (state_.partySize == 3U) percent -= 15;
  if (state_.partySize == 4U) percent -= 25;
  if ((state_.flags & kFlagCommuteSafe) != 0U) percent -= 15;
  if ((state_.flags & kFlagCommunityDay) != 0U) percent -= 5;
  if (percent < 50) percent = 50;
  if (percent > 180) percent = 180;

  uint64_t target = static_cast<uint64_t>(state_.baseTargetSteps) *
      static_cast<uint32_t>(percent) / 100U;
  if (target < 100U) target = 100U;
  if (target > 100000U) target = 100000U;
  state_.targetSteps = static_cast<uint32_t>(target);
}

Status AdventureProgression::addSteps(uint32_t stepDelta) {
  if (!validateProgressState(state_)) return Status::InvalidState;
  if (state_.phase != static_cast<uint8_t>(RoutePhase::Active)) {
    return Status::WrongPhase;
  }
  if (stepDelta > kMaximumStepDelta) return Status::InvalidArgument;
  if (stepDelta == 0U) return Status::NoChange;
  state_.steps = saturatingAddU32(state_.steps, stepDelta);
  refreshCrc();
  return Status::Ok;
}

Status AdventureProgression::decide(MidDecision decision) {
  if (!validateProgressState(state_)) return Status::InvalidState;
  if (state_.phase != static_cast<uint8_t>(RoutePhase::Active) ||
      (state_.flags & kFlagEarlyReturn) != 0U) {
    return Status::WrongPhase;
  }
  if (!validDecision(decision)) return Status::InvalidArgument;
  if (state_.decisionCount >= kMaximumMidDecisions) {
    return Status::DecisionLimit;
  }
  if ((state_.flags & kFlagCommuteSafe) != 0U &&
      decision == MidDecision::Detour) {
    return Status::CommuteRestricted;
  }

  ++state_.decisionCount;
  state_.branchCode = static_cast<uint8_t>(
      (static_cast<uint16_t>(state_.branchCode) * 5U +
       static_cast<uint8_t>(decision) + 1U) % 251U);
  switch (decision) {
    case MidDecision::Continue:
      state_.progressPoints = saturatingAddU16(state_.progressPoints, 5U);
      break;
    case MidDecision::Detour:
      state_.progressPoints = saturatingAddU16(state_.progressPoints, 10U);
      state_.riskPoints = saturatingAddU16(
          state_.riskPoints,
          5U * (static_cast<uint8_t>(state_.risk) + 1U));
      break;
    case MidDecision::Help:
      state_.progressPoints = saturatingAddU16(state_.progressPoints, 8U);
      break;
    case MidDecision::ReturnEarly:
      state_.flags |= kFlagEarlyReturn;
      break;
    case MidDecision::Count:
      return Status::InvalidArgument;
  }
  refreshCrc();
  return Status::Ok;
}

Status AdventureProgression::applyExpedition(
    const expedition::CompletionHooks& completion) {
  if (!validateProgressState(state_)) return Status::InvalidState;
  if (state_.phase != static_cast<uint8_t>(RoutePhase::Active)) {
    return Status::WrongPhase;
  }
  if (completion.expeditionId == 0U) return Status::InvalidArgument;
  if (completion.expeditionId == state_.lastExpeditionId) {
    return Status::Duplicate;
  }
  state_.lastExpeditionId = completion.expeditionId;
  uint32_t points = 4U;
  if (completion.affectionDelta > 0) {
    points += static_cast<uint8_t>(completion.affectionDelta);
  }
  if (completion.hasEncounter()) points += 5U;
  state_.progressPoints = saturatingAddU16(state_.progressPoints, points);
  refreshCrc();
  return Status::Ok;
}

Status AdventureProgression::applyParty(const party::HuntResult& result) {
  if (!validateProgressState(state_)) return Status::InvalidState;
  if (state_.phase != static_cast<uint8_t>(RoutePhase::Active) &&
      state_.phase != static_cast<uint8_t>(RoutePhase::AwaitingRescue)) {
    return Status::WrongPhase;
  }
  if (!validPartyResult(result)) return Status::InvalidArgument;
  if (state_.lastPartyProof == result.proof) return Status::Duplicate;
  state_.lastPartyProof = result.proof;
  state_.partySize = result.participantCount;
  state_.progressPoints = saturatingAddU16(
      state_.progressPoints, static_cast<uint32_t>(result.participantCount) * 2U);
  recalculateTarget();
  refreshCrc();
  return Status::Ok;
}

Status AdventureProgression::applyEncounter(
    const signal::EncounterRecord& encounter) {
  if (!validateProgressState(state_)) return Status::InvalidState;
  if (state_.phase != static_cast<uint8_t>(RoutePhase::Active)) {
    return Status::WrongPhase;
  }
  if (!validEncounterRecord(encounter)) return Status::InvalidArgument;
  if (encounter.operationId == state_.lastEncounterOperationId) {
    return Status::Duplicate;
  }
  if (encounter.operationId < state_.lastEncounterOperationId) {
    return Status::InvalidArgument;
  }
  state_.lastEncounterOperationId = encounter.operationId;
  if (encounter.encounterOccurred != 0U) {
    uint32_t points = state_.objective ==
            static_cast<uint8_t>(Objective::MeetCreature)
        ? 12U
        : 7U;
    state_.progressPoints = saturatingAddU16(state_.progressPoints, points);
  }
  refreshCrc();
  return Status::Ok;
}

void AdventureProgression::appendJournal(const ClockSample& now) {
  JournalEntry entry{};
  entry.routeId = state_.routeId;
  entry.dayId = now.trusted != 0U ? now.dayId : 0U;
  entry.zoneToken = state_.currentZoneToken;
  entry.outcome = state_.outcome;
  entry.terrain = state_.terrain;
  entry.objective = state_.objective;
  entry.postcardId = postcardIdFor(state_);
  entry.partySize = state_.partySize;
  entry.chainDepth = state_.chainDepth;
  entry.routeVariant = state_.routeVariant;
  if (now.trusted != 0U) entry.flags |= kJournalFlagTrustedTime;
  if ((state_.flags & kFlagCommunityDay) != 0U) {
    entry.flags |= kJournalFlagCommunityDay;
  }
  if (state_.homeZoneToken != 0U &&
      state_.homeZoneToken == state_.currentZoneToken) {
    entry.flags |= kJournalFlagHomeComfort;
  }
  if ((state_.flags & kFlagCommuteSafe) != 0U) {
    entry.flags |= kJournalFlagCommuteSafe;
  }
  state_.journal[state_.journalNext] = entry;
  if (state_.journalCount < kJournalCapacity) ++state_.journalCount;
  state_.journalNext = static_cast<uint8_t>(
      (state_.journalNext + 1U) % kJournalCapacity);
}

void AdventureProgression::finalize(Outcome outcome,
                                    const ClockSample& now) {
  state_.outcome = static_cast<uint8_t>(outcome);
  state_.phase = static_cast<uint8_t>(RoutePhase::Returned);
  if (outcome == Outcome::Complete || outcome == Outcome::Rescued) {
    state_.flags |= kFlagChainEligible;
  } else {
    state_.flags &= static_cast<uint8_t>(~kFlagChainEligible);
  }
  appendJournal(now);
  refreshCrc();
}

Status AdventureProgression::finish(const ClockSample& now) {
  if (!validateProgressState(state_)) return Status::InvalidState;
  if (!validClock(now)) return Status::InvalidClock;
  if (state_.phase != static_cast<uint8_t>(RoutePhase::Active)) {
    return Status::WrongPhase;
  }
  if ((state_.flags & kFlagEarlyReturn) != 0U) {
    finalize(Outcome::EarlyReturn, now);
    return Status::Ok;
  }
  const uint8_t progress = progressPercent(state_);
  if (progress >= 100U) {
    finalize(Outcome::Complete, now);
    return Status::Ok;
  }
  if (state_.risk == static_cast<uint8_t>(Risk::Bold) && progress < 40U) {
    state_.phase = static_cast<uint8_t>(RoutePhase::AwaitingRescue);
    refreshCrc();
    return Status::RescueRequired;
  }
  finalize(Outcome::Partial, now);
  return Status::Ok;
}

Status AdventureProgression::rescue(const party::HuntResult& result,
                                    const ClockSample& now) {
  if (!validateProgressState(state_)) return Status::InvalidState;
  if (!validClock(now)) return Status::InvalidClock;
  if (state_.phase != static_cast<uint8_t>(RoutePhase::AwaitingRescue)) {
    return Status::WrongPhase;
  }
  if (!validPartyResult(result)) return Status::InvalidArgument;
  state_.lastPartyProof = result.proof;
  state_.partySize = result.participantCount;
  state_.progressPoints = saturatingAddU16(state_.progressPoints, 20U);
  recalculateTarget();
  finalize(Outcome::Rescued, now);
  return Status::Ok;
}

Status AdventureProgression::acknowledge(uint32_t routeId) {
  if (!validateProgressState(state_)) return Status::InvalidState;
  if (state_.phase != static_cast<uint8_t>(RoutePhase::Returned)) {
    return Status::WrongPhase;
  }
  if (routeId == 0U || routeId != state_.routeId) return Status::WrongRoute;

  const uint8_t retainedFlags = static_cast<uint8_t>(
      state_.flags & (kFlagCurrentZoneNew | kFlagChainEligible));
  state_.phase = static_cast<uint8_t>(RoutePhase::Idle);
  state_.outcome = static_cast<uint8_t>(Outcome::None);
  state_.terrain = static_cast<uint8_t>(Terrain::Meadow);
  state_.objective = static_cast<uint8_t>(Objective::Explore);
  state_.risk = static_cast<uint8_t>(Risk::Balanced);
  state_.personality = static_cast<uint8_t>(PersonalityKind::Gentle);
  state_.weather = static_cast<uint8_t>(Weather::Unknown);
  state_.timeBand = static_cast<uint8_t>(TimeBand::Unknown);
  state_.moonPhase = static_cast<uint8_t>(MoonPhase::Unknown);
  state_.decisionCount = 0U;
  state_.branchCode = 0U;
  state_.partySize = 0U;
  state_.routeVariant = 0U;
  state_.progressPoints = 0U;
  state_.riskPoints = 0U;
  state_.routeId = 0U;
  state_.routeSeed = 0U;
  state_.baseTargetSteps = 0U;
  state_.targetSteps = 0U;
  state_.steps = 0U;
  state_.routeDistanceMeters = 0U;
  state_.flags = retainedFlags;
  refreshCrc();
  return Status::Ok;
}

RouteView AdventureProgression::view() const {
  RouteView output{};
  output.phase = static_cast<RoutePhase>(state_.phase);
  output.outcome = static_cast<Outcome>(state_.outcome);
  output.terrain = static_cast<Terrain>(state_.terrain);
  output.objective = static_cast<Objective>(state_.objective);
  output.risk = static_cast<Risk>(state_.risk);
  output.personality = static_cast<PersonalityKind>(state_.personality);
  output.weather = static_cast<Weather>(state_.weather);
  output.timeBand = static_cast<TimeBand>(state_.timeBand);
  output.moonPhase = static_cast<MoonPhase>(state_.moonPhase);
  output.routeId = state_.routeId;
  output.targetSteps = state_.targetSteps;
  output.steps = state_.steps;
  output.routeDistanceMeters = state_.routeDistanceMeters;
  output.progressPercent = progressPercent(state_);
  output.decisionCount = state_.decisionCount;
  output.branchCode = state_.branchCode;
  output.partySize = state_.partySize;
  output.chainDepth = state_.chainDepth;
  output.routeVariant = state_.routeVariant;
  output.commuteSafe =
      (state_.flags & kFlagCommuteSafe) != 0U ? 1U : 0U;
  output.communityDayActive =
      (state_.flags & kFlagCommunityDay) != 0U ? 1U : 0U;
  output.homeComfort = state_.homeZoneToken != 0U &&
      state_.homeZoneToken == state_.currentZoneToken ? 1U : 0U;
  return output;
}

bool AdventureProgression::currentPostcard(TextPostcard& output) const {
  if (state_.phase != static_cast<uint8_t>(RoutePhase::Returned)) {
    return false;
  }
  return postcardForId(postcardIdFor(state_), output);
}

bool AdventureProgression::journalNewest(uint8_t newestIndex,
                                         JournalEntry& output) const {
  if (newestIndex >= state_.journalCount) return false;
  const uint8_t slot = static_cast<uint8_t>(
      (state_.journalNext + kJournalCapacity - 1U - newestIndex) %
      kJournalCapacity);
  output = state_.journal[slot];
  return true;
}

}  // namespace adventure
}  // namespace kitsu868
