#include "adventure_progression.h"

#include <stddef.h>
#include <stdint.h>

#include <cstring>
#include <iostream>

namespace {

using namespace kitsu868;
using namespace kitsu868::adventure;

int failures = 0;
bool covered[89]{};

void check(bool condition, const char* label) {
  if (condition) return;
  ++failures;
  std::cerr << "FAIL " << label << '\n';
}

void cover(uint8_t feature, bool condition, const char* label) {
  check(condition, label);
  if (condition && feature < sizeof(covered) / sizeof(covered[0])) {
    covered[feature] = true;
  }
}

ClockSample trustedClock(uint32_t dayId, uint16_t minuteOfDay,
                         uint32_t extraDays = 0U) {
  ClockSample clock{};
  clock.unixSeconds = kMinimumTrustedUnixSeconds +
      static_cast<uint64_t>(extraDays) * UINT64_C(86400);
  clock.dayId = dayId;
  clock.minuteOfDay = minuteOfDay;
  clock.trusted = 1U;
  return clock;
}

ClockSample untrustedClock() {
  ClockSample clock{};
  // Deliberately implausible fields prove that trusted=0 makes timed features
  // fail closed rather than consuming caller garbage.
  clock.unixSeconds = UINT64_MAX;
  clock.dayId = UINT32_MAX;
  clock.minuteOfDay = UINT16_MAX;
  clock.trusted = 0U;
  return clock;
}

RouteRequest baseRequest() {
  RouteRequest request{};
  request.terrain = Terrain::Meadow;
  request.objective = Objective::Explore;
  request.risk = Risk::Balanced;
  request.personality = PersonalityKind::Gentle;
  request.weather = Weather::Clear;
  request.baseTargetSteps = 2000U;
  request.commuteSafe = 0U;
  return request;
}

party::HuntResult partyResult(uint8_t participantCount, uint32_t proof) {
  party::HuntResult result{};
  result.participantCount = participantCount;
  result.completedRounds = party::kHuntRounds;
  result.tier = static_cast<uint8_t>(party::ResultTier::Found);
  result.targetChoices[0] =
      static_cast<uint8_t>(party::SignalChoice::Sweep);
  result.targetChoices[1] =
      static_cast<uint8_t>(party::SignalChoice::Listen);
  result.targetChoices[2] =
      static_cast<uint8_t>(party::SignalChoice::Pulse);
  result.score = 6U;
  result.maximumScore = 12U;
  result.proof = proof;
  return result;
}

void completeAndAcknowledge(AdventureProgression& progression,
                            const ClockSample& now) {
  RouteView route = progression.view();
  check(route.phase == RoutePhase::Active, "completion helper has active route");
  check(progression.addSteps(route.targetSteps) == Status::Ok,
        "completion helper adds target steps");
  check(progression.finish(now) == Status::Ok,
        "completion helper finishes route");
  route = progression.view();
  check(route.outcome == Outcome::Complete,
        "completion helper produced complete outcome");
  check(progression.acknowledge(route.routeId) == Status::Ok,
        "completion helper acknowledges route");
}

bool containsRawValue(const ProgressState& state, int32_t value) {
  uint8_t needle[sizeof(value)]{};
  std::memcpy(needle, &value, sizeof(value));
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&state);
  for (size_t offset = 0U;
       offset + sizeof(value) <= sizeof(state); ++offset) {
    if (std::memcmp(bytes + offset, needle, sizeof(value)) == 0) return true;
  }
  return false;
}

void testRouteInputsDecisionsAndEnvironment() {
  const ClockSample dawn = trustedClock(701U, 360U, 1U);
  RouteRequest request = baseRequest();

  AdventureProgression first;
  AdventureProgression identical;
  check(first.begin(request, dawn, UINT32_C(0x12345678)) == Status::Ok,
        "baseline route starts");
  check(identical.begin(request, dawn, UINT32_C(0x12345678)) == Status::Ok,
        "identical route starts");
  const RouteView baseline = first.view();
  const RouteView same = identical.view();
  cover(86, baseline.routeId == same.routeId &&
                baseline.routeVariant == same.routeVariant,
        "[86] identical route inputs are deterministic");

  AdventureProgression terrainRoute;
  RouteRequest terrainRequest = request;
  terrainRequest.terrain = Terrain::Ridge;
  check(terrainRoute.begin(terrainRequest, dawn, UINT32_C(0x12345678)) ==
            Status::Ok,
        "terrain route starts");
  cover(36, terrainRoute.view().terrain == Terrain::Ridge &&
                terrainRoute.view().targetSteps > baseline.targetSteps &&
                terrainRoute.view().routeId != baseline.routeId,
        "[36] terrain changes route identity and effort");

  AdventureProgression objectiveRoute;
  RouteRequest objectiveRequest = request;
  objectiveRequest.objective = Objective::MeetCreature;
  check(objectiveRoute.begin(objectiveRequest, dawn,
                             UINT32_C(0x12345678)) == Status::Ok,
        "objective route starts");
  cover(37, objectiveRoute.view().objective == Objective::MeetCreature &&
                objectiveRoute.view().routeId != baseline.routeId,
        "[37] objective selects a distinct route");

  AdventureProgression riskRoute;
  RouteRequest riskRequest = request;
  riskRequest.risk = Risk::Bold;
  check(riskRoute.begin(riskRequest, dawn, UINT32_C(0x12345678)) ==
            Status::Ok,
        "risk route starts");
  cover(38, riskRoute.view().risk == Risk::Bold &&
                riskRoute.view().targetSteps > baseline.targetSteps,
        "[38] bold risk increases route effort");

  AdventureProgression personalityRoute;
  RouteRequest personalityRequest = request;
  personalityRequest.personality = PersonalityKind::Curious;
  check(personalityRoute.begin(personalityRequest, dawn,
                               UINT32_C(0x12345678)) == Status::Ok,
        "personality route starts");
  cover(39, personalityRoute.view().personality ==
                    PersonalityKind::Curious &&
                personalityRoute.view().routeId != baseline.routeId &&
                personalityRoute.view().targetSteps < baseline.targetSteps,
        "[39] personality influences route and effort");

  const uint8_t branchBefore = first.view().branchCode;
  check(first.decide(MidDecision::Continue) == Status::Ok,
        "continue decision accepted");
  check(first.decide(MidDecision::Detour) == Status::Ok,
        "detour decision accepted");
  check(first.decide(MidDecision::Help) == Status::Ok,
        "help decision accepted");
  cover(40, first.view().decisionCount == kMaximumMidDecisions,
        "[40] three mid-route decisions are recorded");
  cover(41, first.view().branchCode != branchBefore,
        "[41] decisions deterministically advance branch code");
  check(first.decide(MidDecision::Continue) == Status::DecisionLimit,
        "decision limit is bounded");

  const uint8_t beforeSteps = first.view().progressPercent;
  check(first.addSteps(500U) == Status::Ok, "steps advance route");
  cover(49, first.view().steps == 500U &&
                first.view().progressPercent > beforeSteps,
        "[49] step counter advances route progress");

  cover(83, baseline.timeBand == TimeBand::Dawn,
        "[83] trusted minute selects dawn route band");
  cover(84, baseline.weather == Weather::Clear,
        "[84] route retains supplied weather context");
  cover(85, baseline.moonPhase != MoonPhase::Unknown,
        "[85] trusted wall clock derives moon route phase");

  AdventureProgression clockless;
  check(clockless.begin(request, untrustedClock(), UINT32_C(0x13579BDF)) ==
            Status::Ok,
        "route starts without wall clock");
  check(clockless.view().timeBand == TimeBand::Unknown &&
            clockless.view().moonPhase == MoonPhase::Unknown &&
            clockless.view().communityDayActive == 0U,
        "untrusted wall clock fails timed features closed");
  ClockSample invalidTrusted = trustedClock(0U, 1500U);
  invalidTrusted.unixSeconds = 10U;
  check(AdventureProgression{}.begin(request, invalidTrusted, 1U) ==
            Status::InvalidClock,
        "invalid trusted clock is rejected");

  AdventureProgression commute;
  RouteRequest commuteRequest = request;
  commuteRequest.commuteSafe = 1U;
  commuteRequest.risk = Risk::Bold;
  check(commute.begin(commuteRequest, dawn, 5U) ==
            Status::CommuteRestricted,
        "commute route rejects bold risk");
  commuteRequest.risk = Risk::Careful;
  check(commute.begin(commuteRequest, dawn, 5U) == Status::Ok,
        "careful commute route starts");
  cover(87, commute.view().commuteSafe == 1U &&
                commute.decide(MidDecision::Detour) ==
                    Status::CommuteRestricted,
        "[87] commute-safe route blocks risky detours");
}

void testChainsAntiRepeatOutcomesPostcardsAndJournal() {
  const ClockSample now = trustedClock(707U, 720U, 8U);
  RouteRequest request = baseRequest();
  AdventureProgression progression;

  uint8_t priorVariants[kRouteHistoryCapacity]{};
  uint8_t priorCount = 0U;
  bool antiRepeat = true;
  for (uint8_t routeIndex = 0U; routeIndex < 5U; ++routeIndex) {
    check(progression.begin(request, now, UINT32_C(0x11112222)) == Status::Ok,
          "chain route starts");
    const RouteView route = progression.view();
    if (routeIndex > 0U) {
      for (uint8_t index = 0U; index < priorCount; ++index) {
        if (priorVariants[index] == route.routeVariant) antiRepeat = false;
      }
    }
    if (priorCount < kRouteHistoryCapacity) {
      priorVariants[priorCount++] = route.routeVariant;
    } else {
      for (uint8_t index = 1U; index < kRouteHistoryCapacity; ++index) {
        priorVariants[index - 1U] = priorVariants[index];
      }
      priorVariants[kRouteHistoryCapacity - 1U] = route.routeVariant;
    }
    if (routeIndex == 1U) {
      cover(42, route.chainDepth == 2U,
            "[42] successful matching routes build a chain");
    }
    completeAndAcknowledge(progression, now);
  }
  cover(43, antiRepeat,
        "[43] recent route variants are not repeated");

  AdventureProgression partial;
  check(partial.begin(request, untrustedClock(), 7U) == Status::Ok,
        "partial route starts");
  check(partial.addSteps(partial.view().targetSteps / 2U) == Status::Ok,
        "partial route receives half steps");
  check(partial.finish(untrustedClock()) == Status::Ok,
        "partial route finishes");
  cover(44, partial.view().outcome == Outcome::Partial,
        "[44] incomplete safe route returns a partial result");
  TextPostcard postcard{};
  cover(51, partial.currentPostcard(postcard) &&
                std::strcmp(postcard.title, "NO POSTCARD") != 0 &&
                std::strlen(postcard.line) != 0U,
        "[51] returned route yields a fixed text postcard");
  JournalEntry newest{};
  cover(52, partial.journalCount() == 1U &&
                partial.journalNewest(0U, newest) && newest.dayId == 0U &&
                newest.outcome == static_cast<uint8_t>(Outcome::Partial),
        "[52] journal records outcome and omits untrusted day");

  AdventureProgression early;
  check(early.begin(request, now, 9U) == Status::Ok,
        "early return route starts");
  check(early.decide(MidDecision::ReturnEarly) == Status::Ok,
        "return decision accepted");
  check(early.finish(now) == Status::Ok, "early route finishes");
  cover(45, early.view().outcome == Outcome::EarlyReturn,
        "[45] return decision produces early-return outcome");

  RouteView returned = early.view();
  check(early.acknowledge(returned.routeId + 1U) == Status::WrongRoute,
        "wrong route acknowledgement rejected");
  check(early.acknowledge(returned.routeId) == Status::Ok,
        "correct route acknowledgement accepted");
}

void testPartyRescueAndExistingHooks() {
  const ClockSample now = trustedClock(708U, 900U, 9U);
  RouteRequest bold = baseRequest();
  bold.risk = Risk::Bold;

  AdventureProgression twoPerson;
  check(twoPerson.begin(bold, now, 10U) == Status::Ok,
        "bold rescue route starts");
  check(twoPerson.finish(now) == Status::RescueRequired &&
            twoPerson.view().phase == RoutePhase::AwaitingRescue,
        "low-progress bold route requests rescue");
  const party::HuntResult duo = partyResult(2U, UINT32_C(0xA1A2A3A4));
  check(twoPerson.rescue(duo, now) == Status::Ok,
        "two-person rescue accepted");
  cover(46, twoPerson.view().outcome == Outcome::Rescued,
        "[46] party assistance resolves rescue state");
  cover(47, twoPerson.view().partySize == 2U,
        "[47] two-participant rescue is supported");

  AdventureProgression twoAssist;
  AdventureProgression fourAssist;
  RouteRequest community = baseRequest();
  community.objective = Objective::Community;
  check(twoAssist.begin(community, now, 20U) == Status::Ok,
        "two-person assist route starts");
  check(fourAssist.begin(community, now, 20U) == Status::Ok,
        "four-person assist route starts");
  check(twoAssist.applyParty(partyResult(2U, UINT32_C(0xB1B2B3B4))) ==
            Status::Ok,
        "two-person hunt hook applied");
  check(fourAssist.applyParty(partyResult(4U, UINT32_C(0xC1C2C3C4))) ==
            Status::Ok,
        "four-person hunt hook applied");
  cover(48, fourAssist.view().partySize == 4U &&
                fourAssist.view().targetSteps < twoAssist.view().targetSteps,
        "[48] four-participant party provides larger route assist");
  check(fourAssist.applyParty(partyResult(4U, UINT32_C(0xC1C2C3C4))) ==
            Status::Duplicate,
        "party proof is idempotent");

  expedition::CompletionHooks expeditionHook{};
  expeditionHook.expeditionId = UINT32_C(0x01020304);
  expeditionHook.affectionDelta = 2;
  expeditionHook.encounterCatalogIndex = 3U;
  const uint8_t progressBeforeExpedition =
      fourAssist.view().progressPercent;
  check(fourAssist.applyExpedition(expeditionHook) == Status::Ok &&
            fourAssist.view().progressPercent > progressBeforeExpedition,
        "existing expedition value hook advances route");
  check(fourAssist.applyExpedition(expeditionHook) == Status::Duplicate,
        "expedition ID is idempotent");

  signal::EncounterRecord encounter{};
  encounter.operationId = UINT64_C(42);
  encounter.operationKind =
      static_cast<uint8_t>(signal::MeshOperationKind::NearbyKitsuMet);
  encounter.encounterOccurred = 1U;
  encounter.rarity = static_cast<uint8_t>(signal::Rarity::Rare);
  encounter.codeOutcome =
      static_cast<uint8_t>(signal::CodeOutcome::NotApplicable);
  check(fourAssist.applyEncounter(encounter) == Status::Ok,
        "existing encounter value hook advances route");
  check(fourAssist.applyEncounter(encounter) == Status::Duplicate,
        "encounter operation is idempotent");
}

void testLocationExplorationAndPrivacy() {
  AdventureProgression progression;
  LocationUpdate update{};
  check(progression.observeCoarseZone(UINT32_C(0x11223344), 0U, 10U,
                                      update) == Status::PrivacyDisabled,
        "location ignored while privacy is off");
  ProgressState offState = progression.snapshot();
  check(offState.zoneCount == 0U && offState.currentZoneToken == 0U &&
            offState.totalDistanceMeters == 0U,
        "privacy off persists no location aggregate");

  check(progression.setPrivacyMode(PrivacyMode::Coarse) == Status::Ok,
        "coarse privacy mode enabled");
  const uint32_t homeToken = UINT32_C(0x11223344);
  check(progression.setHomeZone(homeToken) == Status::Ok,
        "coarse home zone configured");
  check(progression.observeCoarseZone(homeToken, 0U, 120U, update) ==
            Status::Ok,
        "coarse home zone observed");
  cover(50, update.zoneToken == homeToken,
        "[50] caller-provided coarse location reaches route state");
  cover(76, update.storedZoneCount == 1U,
        "[76] exploration zone enters bounded zone table");
  cover(77, update.totalDistanceMeters == 120U,
        "[77] distance aggregate advances without coordinates");
  cover(78, update.newZone == 1U,
        "[78] first zone observation reports new-zone discovery");
  cover(80, update.homeComfort == 1U &&
                update.familiarity == Familiarity::Home,
        "[80] configured home zone provides home comfort");

  const uint32_t familiarToken = UINT32_C(0x55667788);
  for (uint8_t visit = 0U; visit < 4U; ++visit) {
    check(progression.observeCoarseZone(familiarToken, 0U, 10U, update) ==
              Status::Ok,
          "familiar zone revisit accepted");
  }
  cover(79, update.familiarity == Familiarity::WellKnown &&
                update.newZone == 0U,
        "[79] repeated visits increase zone familiarity");

  for (uint32_t token = 1U; token <= 6U; ++token) {
    check(progression.observeCoarseZone(UINT32_C(0x80000000) + token,
                                        0U, 0U, update) == Status::Ok,
          "bounded zone replacement accepts new token");
  }
  check(progression.snapshot().zoneCount == kMaximumZones,
        "zone table remains fixed at capacity");

  check(progression.setPrivacyMode(PrivacyMode::PreciseTransient,
                                   UINT32_C(0xDEADBEEF)) == Status::Ok,
        "precise-transient privacy mode enabled");
  PreciseLocationSample precise{};
  precise.latitudeE7 = 488765432;
  precise.longitudeE7 = 268765432;
  precise.accuracyMeters = 12U;
  check(progression.observePreciseTransient(precise, 30U, 345U, update) ==
            Status::Ok,
        "precise transient sample accepted");
  const ProgressState preciseState = progression.snapshot();
  check(preciseState.zoneCount == 1U &&
            preciseState.currentZoneToken != 0U &&
            !containsRawValue(preciseState, precise.latitudeE7) &&
            !containsRawValue(preciseState, precise.longitudeE7),
        "snapshot contains token but no raw precise coordinate bytes");

  check(progression.setPrivacyMode(PrivacyMode::Off) == Status::Ok,
        "privacy can be switched off");
  const ProgressState cleared = progression.snapshot();
  cover(88, cleared.privacyMode == static_cast<uint8_t>(PrivacyMode::Off) &&
                cleared.zoneCount == 0U &&
                cleared.currentZoneToken == 0U &&
                cleared.homeZoneToken == 0U &&
                cleared.locationSalt == 0U &&
                cleared.totalDistanceMeters == 0U,
        "[88] privacy off clears all persisted location derivatives");
}

void testCommunityHotspotsAndCommunityDay() {
  AdventureProgression progression;
  HotspotUpdate update{};
  check(progression.observeHotspot(UINT32_C(0x11110001), 3U,
                                   untrustedClock(), update) == Status::Ok,
        "clockless community hotspot recorded");
  cover(81, update.hotspotVisits == 1U && update.participantCount == 3U,
        "[81] radio community hotspot increments visit count");
  check(update.trustedTime == 0U && update.communityDayActive == 0U &&
            update.bonusAwarded == 0U,
        "clockless hotspot awards no timed bonus");

  const ClockSample communityDay = trustedClock(700U, 840U, 10U);
  check(progression.observeHotspot(UINT32_C(0x11110002), 4U,
                                   communityDay, update) == Status::Ok,
        "trusted community-day hotspot recorded");
  cover(82, update.communityDayActive == 1U &&
                update.bonusAwarded == 1U &&
                update.communityDayBonuses == 1U,
        "[82] weekly community day awards one trusted-day bonus");
  check(progression.observeHotspot(UINT32_C(0x11110002), 4U,
                                   communityDay, update) == Status::Duplicate &&
            update.bonusAwarded == 0U,
        "same trusted hotspot/day is idempotent");
}

void testSnapshotRestoreAndJournalRing() {
  const ClockSample now = trustedClock(714U, 600U, 14U);
  AdventureProgression progression;
  RouteRequest request = baseRequest();
  for (uint8_t index = 0U; index < kJournalCapacity + 2U; ++index) {
    request.terrain = static_cast<Terrain>(index %
        static_cast<uint8_t>(Terrain::Count));
    check(progression.begin(request, now, 100U + index) == Status::Ok,
          "journal-ring route starts");
    completeAndAcknowledge(progression, now);
  }
  check(progression.journalCount() == kJournalCapacity,
        "journal ring remains at fixed capacity");
  JournalEntry newest{};
  check(progression.journalNewest(0U, newest) && newest.routeId != 0U,
        "newest journal entry is readable");
  check(!progression.journalNewest(kJournalCapacity, newest),
        "journal read rejects out-of-range index");

  const ProgressState snapshot = progression.snapshot();
  check(sizeof(snapshot) <= 288U && validateProgressState(snapshot),
        "compact CRC snapshot validates");
  AdventureProgression restored;
  check(restored.restore(snapshot) == RestoreStatus::Ok &&
            restored.snapshot().crc32 == snapshot.crc32 &&
            restored.journalCount() == kJournalCapacity,
        "CRC snapshot restores exactly");

  ProgressState corrupt = snapshot;
  ++corrupt.totalDistanceMeters;
  check(restored.restore(corrupt) == RestoreStatus::InvalidState,
        "CRC corruption is rejected");
  corrupt = snapshot;
  corrupt.magic = 0U;
  check(restored.restore(corrupt) == RestoreStatus::BadMagic,
        "bad snapshot magic is rejected");
  corrupt = snapshot;
  ++corrupt.schemaVersion;
  check(restored.restore(corrupt) == RestoreStatus::UnsupportedSchema,
        "future snapshot schema is rejected");
  check(std::strcmp(statusName(Status::CommuteRestricted),
                    "commute_restricted") == 0,
        "status names are stable");
}

void verifyFeatureCoverage() {
  for (uint8_t feature = 36U; feature <= 52U; ++feature) {
    if (!covered[feature]) {
      ++failures;
      std::cerr << "FAIL feature coverage missing "
                << static_cast<unsigned>(feature) << '\n';
    }
  }
  for (uint8_t feature = 76U; feature <= 88U; ++feature) {
    if (!covered[feature]) {
      ++failures;
      std::cerr << "FAIL feature coverage missing "
                << static_cast<unsigned>(feature) << '\n';
    }
  }
}

}  // namespace

int main() {
  testRouteInputsDecisionsAndEnvironment();
  testChainsAntiRepeatOutcomesPostcardsAndJournal();
  testPartyRescueAndExistingHooks();
  testLocationExplorationAndPrivacy();
  testCommunityHotspotsAndCommunityDay();
  testSnapshotRestoreAndJournalRing();
  verifyFeatureCoverage();

  if (failures != 0) {
    std::cerr << "TEST_FAIL adventure_progression failures=" << failures
              << '\n';
    return 1;
  }
  std::cout << "TEST_PASS adventure_progression features=36-52,76-88 "
               "snapshot_bytes=" << sizeof(ProgressState)
            << " zones=" << static_cast<unsigned>(kMaximumZones)
            << " journal=" << static_cast<unsigned>(kJournalCapacity)
            << " privacy=off,coarse,precise-transient clock_fail_closed=1\n";
  return 0;
}
