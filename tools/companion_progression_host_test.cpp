#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/companion_progression.h"

using namespace kitsu868;

namespace {

bool gCovered[36]{};

void cover(uint8_t feature) {
  assert(feature >= 1U && feature <= 35U);
  gCovered[feature] = true;
}

CompanionVitals comfortableVitals() {
  CompanionVitals vitals{};
  vitals.energy = 70U;
  vitals.curiosity = 40U;
  vitals.affection = 50U;
  vitals.sleeping = false;
  vitals.listening = false;
  return vitals;
}

void assertDisplayLine(const progression::DisplayLine& line) {
  assert(line.line1 != nullptr && line.line2 != nullptr);
  assert(strlen(line.line1) <= 16U);
  assert(strlen(line.line2) <= 16U);
}

void performGoal(progression::CompanionProgression& state, uint32_t day,
                 uint16_t firstMinute, uint8_t bond = 0U) {
  const progression::DailyGoal goal = state.dailyGoal();
  for (uint8_t count = 0U; count < goal.target; ++count) {
    dialogue::Action action = dialogue::Action::Pet;
    if (goal.kind == progression::GoalKind::Favorite) {
      action = goal.action;
    } else if (goal.kind == progression::GoalKind::Variety) {
      action = static_cast<dialogue::Action>(count % progression::kActionCount);
    }
    const progression::ActionResult result = state.recordAction(
        action, day, static_cast<uint16_t>(firstMinute + count * 60U), bond);
    assert(result.valid);
  }
  assert(state.dailyGoal().complete());
}

void testMemoryLearningAndActionBehavior() {
  progression::CompanionProgression state;
  state.initialize(UINT32_C(0x12345678), 100U);

  // [1] A delayed event callback is unavailable early, then retains detail.
  assert(state.rememberEvent(42U, 100U, 2U));
  assert(!state.callbackReady(101U));
  progression::Callback callback{};
  assert(state.takeCallback(102U, callback));
  assert(callback.kind == progression::CallbackKind::Event);
  assert(callback.detail == 42U && callback.dueDay == 102U);
  cover(1U);

  // [2] Every action proposes a concrete next link; taking it is recognized.
  progression::ActionResult result =
      state.recordAction(dialogue::Action::Pet, 100U, 360U, 0U);
  assert(result.valid && result.followUp == dialogue::Action::Play);
  result = state.recordAction(result.followUp, 100U, 420U, 0U);
  assert(result.followedSuggestion);
  cover(2U);

  // [3] Three consecutive copies are explicitly recognized as repetition.
  state.recordAction(dialogue::Action::Feed, 100U, 480U, 0U);
  state.recordAction(dialogue::Action::Feed, 100U, 500U, 0U);
  result = state.recordAction(dialogue::Action::Feed, 100U, 520U, 0U);
  assert(result.repeatCount == 3U && result.repeatNoticed);
  cover(3U);

  // [4] A clear learned favorite requires evidence and a two-use lead.
  for (uint8_t use = 0U; use < 6U; ++use) {
    state.recordAction(dialogue::Action::Gift, 100U,
                       static_cast<uint16_t>(600U + use), 0U);
  }
  assert(state.hasFavorite());
  assert(state.favoriteAction() == dialogue::Action::Gift);
  cover(4U);

  // [5] Per-action time learning rejects ties and returns the dominant bucket.
  progression::TimeBucket bucket = progression::TimeBucket::Night;
  assert(state.preferredTime(dialogue::Action::Gift, bucket));
  assert(bucket == progression::TimeBucket::Morning);
  cover(5U);

  // [7] Nicknames are fixed-width printable state and invalid writes are atomic.
  assert(state.setNickname("Mochi"));
  assert(strcmp(state.nickname(), "Mochi") == 0);
  assert(!state.setNickname("1234567890123"));
  assert(strcmp(state.nickname(), "Mochi") == 0);
  cover(7U);

  // [8] Bond progression selects four distinct authored dialogue banks.
  assert(state.bondDialogueBank(0U) == 0U);
  assert(state.bondDialogueBank(1U) == 1U);
  assert(state.bondDialogueBank(3U) == 2U);
  assert(state.bondDialogueBank(5U) == 3U);
  for (uint8_t bank = 0U; bank < 4U; ++bank) {
    assertDisplayLine(progression::CompanionProgression::bondLine(bank));
  }
  cover(8U);

  // [9] Mood has bounded persistent momentum, including anti-spam damping.
  assert(state.moodMomentum() > 0);
  uint8_t snapshot[progression::kSnapshotCapacity]{};
  assert(state.snapshotSize() <= sizeof(snapshot));
  assert(state.writeSnapshot(snapshot, sizeof(snapshot)));
  progression::CompanionProgression restored;
  assert(restored.restoreSnapshot(snapshot, state.snapshotSize(),
                                  UINT32_C(0x12345678)));
  assert(restored.moodMomentum() == state.moodMomentum());
  assert(strcmp(restored.nickname(), "Mochi") == 0);
  snapshot[state.snapshotSize() / 2U] ^= 0x40U;
  progression::CompanionProgression rejected;
  assert(!rejected.restoreSnapshot(snapshot, state.snapshotSize(),
                                   UINT32_C(0x12345678)));
  cover(9U);

  // [15] Speech evolves from both lived actions and bond, capped at four stages.
  result = state.recordAction(dialogue::Action::Pet, 100U, 700U, 5U);
  assert(result.speechStage == 3U && state.speechStage() == 3U);
  assertDisplayLine(progression::CompanionProgression::speechLine(
      state.speechStage()));
  cover(15U);
}

void testSessionsRequestsQuestionsAndComfort() {
  progression::CompanionProgression state;
  state.initialize(77U, 10U);
  CompanionVitals vitals = comfortableVitals();

  progression::SessionResult session = state.startSession(
      10U, 600U, 0U, vitals, progression::Season::Spring);
  assert(session.valid);

  // [10] The companion initiates one needs-aware request on a fresh day.
  assert(session.requestOffered);
  assert(state.requestState() == progression::RequestState::Pending);
  cover(10U);

  // [11] Requests have explicit accept/decline state and persistent counters.
  progression::RequestResult request = state.answerRequest(true);
  assert(request.valid && request.accepted);
  assert(state.acceptedRequests() == 1U);
  progression::ActionResult action =
      state.recordAction(request.requestedAction, 10U, 660U, 0U);
  assert(action.requestCompleted);
  session = state.startSession(11U, 600U, 0U, vitals,
                               progression::Season::Spring);
  assert(session.requestOffered);
  request = state.answerRequest(false);
  assert(request.valid && !request.accepted && state.declinedRequests() == 1U);
  assert(!state.answerRequest(false).valid);
  cover(11U);

  // [12] Deterministic short questions remember the selected preference.
  progression::QuestionKind question{};
  bool foundQuestion = state.pendingQuestion(question);
  for (uint32_t day = 12U; day < 30U && !foundQuestion; ++day) {
    session = state.startSession(day, 600U, 0U, vitals,
                                 progression::Season::Spring);
    foundQuestion = state.pendingQuestion(question);
  }
  assert(foundQuestion);
  assertDisplayLine(progression::CompanionProgression::questionLine(question));
  assert(strlen(progression::CompanionProgression::questionOption(question, 0U)) <=
         16U);
  const progression::QuestionResult answer = state.answerQuestion(1U);
  assert(answer.valid && answer.choice == 1U);
  uint8_t savedChoice = 0U;
  assert(state.preferredQuestionChoice(answer.question, savedChoice));
  assert(savedChoice == 1U);
  cover(12U);

  // [6,27] Absence wording and comeback state use actual missed calendar days.
  progression::CompanionProgression absence;
  absence.initialize(88U, 100U);
  absence.startSession(100U, 600U, 0U, vitals,
                       progression::Season::Spring);
  session = absence.startSession(105U, 600U, 0U, vitals,
                                 progression::Season::Spring);
  assert(session.absentDays == 4U);
  assert(session.greeting == progression::GreetingKind::Comeback);
  assert(session.comebackDay && absence.lastDayWasComeback());
  assertDisplayLine(
      progression::CompanionProgression::greetingLine(session.greeting));
  cover(6U);
  cover(27U);

  // [19] A low-vital comfort need is cleared only by a fitting care action.
  progression::CompanionProgression comfort;
  comfort.initialize(89U, 1U);
  vitals.energy = 5U;
  session = comfort.startSession(1U, 1200U, 0U, vitals,
                                 progression::Season::Winter);
  assert(session.comfort == progression::ComfortKind::Tired);
  assertDisplayLine(
      progression::CompanionProgression::comfortLine(session.comfort));
  action = comfort.recordAction(dialogue::Action::Sleep, 1U, 1210U, 0U);
  assert(action.comforted);
  assert(comfort.comfortNeed() == progression::ComfortKind::None);
  cover(19U);
}

void testRitualHabitCallbacksAndLore() {
  // Fingerprint zero deterministically owns the DawnPets secret habit.
  progression::CompanionProgression state;
  state.initialize(0U, 1U);

  progression::ActionResult third{};
  for (uint32_t day = 1U; day <= 3U; ++day) {
    third = state.recordAction(dialogue::Action::Pet, day, 360U, 1U);
    assert(third.valid);
  }

  // [13] Three consecutive same-time, same-action days establish a ritual.
  assert(third.ritualRecognized && state.hasRitual());
  assert(state.ritualAction() == dialogue::Action::Pet);
  assert(state.ritualTime() == progression::TimeBucket::Morning);
  assert(state.ritualStreak() == 3U);
  cover(13U);

  // [14] The fingerprint-specific hidden habit unlocks through real behavior.
  assert(third.secretHabitUnlocked && state.hasSecretHabit());
  assert(state.secretHabit() == progression::HabitKind::DawnPets);
  assertDisplayLine(progression::CompanionProgression::habitLine(
      state.secretHabit()));
  cover(14U);

  // [16-18] Context callbacks preserve their semantic source and unlock lore.
  assert(state.rememberDream(2U, 3U));
  assert(state.rememberExpedition(4U, 3U));
  assert(state.rememberFriend(6U, 3U));
  bool dream = false;
  bool expedition = false;
  bool friendSeen = false;
  progression::Callback callback{};
  while (state.takeCallback(4U, callback)) {
    assertDisplayLine(
        progression::CompanionProgression::callbackLine(callback));
    dream = dream || callback.kind == progression::CallbackKind::Dream;
    expedition =
        expedition || callback.kind == progression::CallbackKind::Expedition;
    friendSeen = friendSeen || callback.kind == progression::CallbackKind::Friend;
  }
  assert(dream && expedition && friendSeen);
  cover(16U);
  cover(17U);
  cover(18U);

  // [20] Lore is unlocked by actual memories, routines, and lived contexts.
  const uint16_t requiredLore = progression::LoreFirstMemory |
                                progression::LoreRitual |
                                progression::LoreSecretHabit |
                                progression::LoreDream |
                                progression::LoreExpedition |
                                progression::LoreFriend |
                                progression::LoreBond;
  assert((state.loreMask() & requiredLore) == requiredLore);
  const progression::LoreUnlock loreEntries[] = {
      progression::LoreFirstMemory, progression::LoreFavorite,
      progression::LoreRitual,      progression::LoreSecretHabit,
      progression::LoreDream,       progression::LoreExpedition,
      progression::LoreFriend,      progression::LorePerfectDay,
      progression::LoreBond,        progression::LoreAnniversary,
  };
  for (size_t index = 0U;
       index < sizeof(loreEntries) / sizeof(loreEntries[0]); ++index) {
    assertDisplayLine(
        progression::CompanionProgression::loreLine(loreEntries[index]));
  }
  cover(20U);

  // [29] Bond tiers emit one milestone callback per newly crossed bank.
  progression::CompanionProgression bond;
  bond.initialize(91U, 8U);
  assert(bond.observeBond(3U, 8U));
  assert(!bond.observeBond(4U, 8U));
  assert(bond.takeCallback(8U, callback));
  assert(callback.kind == progression::CallbackKind::BondMilestone);
  assert(callback.detail == 2U);
  cover(29U);

  // [30] Achievements are concrete persistent bit flags, not display claims.
  const uint32_t requiredAchievements =
      progression::AchievementRitual |
      progression::AchievementSecretHabit |
      progression::AchievementDreamer |
      progression::AchievementExplorer |
      progression::AchievementFriendly |
      progression::AchievementFirstCallback;
  assert((state.achievementMask() & requiredAchievements) ==
         requiredAchievements);
  cover(30U);
}

void testDailyProgressionAndRecords() {
  CompanionVitals vitals = comfortableVitals();

  // [21] Low-history days start gently; high-history days adapt to variety.
  progression::CompanionProgression adaptive;
  adaptive.initialize(123U, 1U);
  progression::SessionResult session = adaptive.startSession(
      1U, 300U, 0U, vitals, progression::Season::Spring);
  assert(session.goal.kind == progression::GoalKind::AnyCare);
  for (uint8_t action = 0U; action < progression::kActionCount; ++action) {
    adaptive.recordAction(static_cast<dialogue::Action>(action), 1U,
                          static_cast<uint16_t>(300U + action * 50U), 0U);
  }
  session = adaptive.startSession(2U, 300U, 0U, vitals,
                                  progression::Season::Spring);
  assert(session.goal.kind == progression::GoalKind::Variety);
  assert(session.goal.target == 4U);
  cover(21U);

  // [22] Daily seeds are stable per companion/day and change across days.
  assert(adaptive.dailySeed(99U) == adaptive.dailySeed(99U));
  assert(adaptive.dailySeed(99U) != adaptive.dailySeed(100U));
  progression::CompanionProgression other;
  other.initialize(124U, 1U);
  assert(adaptive.dailySeed(99U) != other.dailySeed(99U));
  cover(22U);

  // [23,24,31] Alternation and healthy spacing update distinct personal bests.
  progression::CompanionProgression records;
  records.initialize(321U, 1U);
  records.recordAction(dialogue::Action::Pet, 1U, 300U, 0U);
  records.recordAction(dialogue::Action::Feed, 1U, 360U, 0U);
  records.recordAction(dialogue::Action::Play, 1U, 420U, 0U);
  const progression::ActionResult fourth =
      records.recordAction(dialogue::Action::Listen, 1U, 480U, 0U);
  assert(fourth.varietyChain == 4U && fourth.dailyVariety == 4U);
  assert(fourth.rhythmBonus == 3U);
  const progression::PersonalBests bests = records.personalBests();
  assert(bests.varietyChain == 4U && bests.dailyVariety == 4U);
  assert(bests.careRhythm == 3U && bests.dailyActions == 4U);
  cover(23U);
  cover(24U);
  cover(31U);

  // [25] Completing the goal with variety and rhythm records a perfect day.
  progression::CompanionProgression perfect;
  perfect.initialize(222U, 10U);
  perfect.startSession(10U, 300U, 0U, vitals,
                       progression::Season::Summer);
  perfect.recordAction(dialogue::Action::Pet, 10U, 300U, 0U);
  perfect.recordAction(dialogue::Action::Feed, 10U, 360U, 0U);
  perfect.recordAction(dialogue::Action::Play, 10U, 420U, 0U);
  session = perfect.startSession(11U, 300U, 0U, vitals,
                                 progression::Season::Summer);
  assert(session.previousDayPerfect && perfect.perfectDays() == 1U);
  assert((perfect.loreMask() & progression::LorePerfectDay) != 0U);
  cover(25U);

  // [26] Exactly one failed day is absorbed; the next failure breaks the streak.
  progression::CompanionProgression grace;
  grace.initialize(333U, 1U);
  grace.startSession(1U, 300U, 0U, vitals, progression::Season::Spring);
  performGoal(grace, 1U, 300U);
  grace.startSession(2U, 300U, 0U, vitals, progression::Season::Spring);
  assert(grace.currentStreak() == 1U);
  grace.startSession(3U, 300U, 0U, vitals, progression::Season::Spring);
  assert(grace.currentStreak() == 1U);
  grace.startSession(4U, 300U, 0U, vitals, progression::Season::Spring);
  assert(grace.currentStreak() == 0U);
  cover(26U);

  // [28] A deterministic seven-part micro-story arc advances with calendar day.
  progression::CompanionProgression weekly;
  weekly.initialize(444U, 50U);
  session = weekly.startSession(50U, 300U, 0U, vitals,
                                progression::Season::Autumn);
  assert(session.weeklyChapter == 0U);
  session = weekly.startSession(56U, 300U, 0U, vitals,
                                progression::Season::Autumn);
  assert(session.weeklyChapter == 6U);
  session = weekly.startSession(57U, 300U, 0U, vitals,
                                progression::Season::Autumn);
  assert(session.weeklyChapter == 0U);
  assertDisplayLine(progression::CompanionProgression::weeklyChapterLine(
      session.weeklyChapter));
  cover(28U);

  // [32] A routine needs four observations and a clear two-use lead.
  progression::CompanionProgression routine;
  routine.initialize(555U, 1U);
  for (uint32_t day = 1U; day <= 4U; ++day) {
    routine.recordAction(dialogue::Action::Feed, day, 800U, 0U);
  }
  dialogue::Action routineAction = dialogue::Action::Pet;
  assert(routine.recognizedRoutine(progression::TimeBucket::Day,
                                   routineAction));
  assert(routineAction == dialogue::Action::Feed);
  cover(32U);
}

void testCalendarMomentsAndDisplayCatalogue() {
  CompanionVitals vitals = comfortableVitals();

  // [33] The first and later yearly anniversaries are calendar-derived once/day.
  progression::CompanionProgression anniversary;
  anniversary.initialize(999U, 1000U);
  progression::SessionResult session = anniversary.startSession(
      1365U, 720U, 0U, vitals, progression::Season::Summer);
  assert(session.anniversary);
  assert(!anniversary
              .startSession(1365U, 721U, 0U, vitals,
                            progression::Season::Summer)
              .anniversary);
  assert((anniversary.achievementMask() &
          progression::AchievementAnniversary) != 0U);
  cover(33U);

  // [34] The session retains season and every season has OLED-safe behavior text.
  progression::CompanionProgression seasons;
  seasons.initialize(1001U, 1U);
  for (uint8_t value = 0U; value < 4U; ++value) {
    const progression::Season season =
        static_cast<progression::Season>(value);
    session = seasons.startSession(static_cast<uint32_t>(value + 1U), 700U,
                                   0U, vitals, season);
    assert(session.valid && session.season == season);
    assertDisplayLine(
        progression::CompanionProgression::seasonalLine(season));
  }
  cover(34U);

  // [35] Rare moments are deterministic, fire once, and set an achievement.
  progression::CompanionProgression rare;
  rare.initialize(2024U, 1U);
  uint32_t rareDay = 0U;
  for (uint32_t day = 1U; day <= 100U; ++day) {
    if ((rare.dailySeed(day) % 17U) == 0U) {
      rareDay = day;
      break;
    }
  }
  assert(rareDay != 0U);
  session = rare.startSession(rareDay, 900U, 0U, vitals,
                              progression::Season::Winter);
  assert(session.rareMoment);
  assert(!rare
              .startSession(rareDay, 901U, 0U, vitals,
                            progression::Season::Winter)
              .rareMoment);
  assert((rare.achievementMask() & progression::AchievementRareMoment) != 0U);
  assertDisplayLine(progression::CompanionProgression::rareMomentLine(
      rare.dailySeed(rareDay)));
  cover(35U);

  // Every authored helper remains within the current compact OLED line budget.
  for (uint8_t action = 0U; action < progression::kActionCount; ++action) {
    assertDisplayLine(progression::CompanionProgression::requestLine(
        static_cast<dialogue::Action>(action)));
  }
}

void testBoundedAndInvalidContracts() {
  progression::CompanionProgression state;
  state.initialize(606U, 0U);

  // Day serial zero is valid and the first action cannot earn phantom rhythm.
  progression::ActionResult result =
      state.recordAction(dialogue::Action::Pet, 0U, 600U, 0U);
  assert(result.valid && result.rhythmBonus == 0U);
  assert(state.ritualStreak() == 1U);

  const uint32_t actionsBefore = state.totalActions();
  assert(!state
              .recordAction(static_cast<dialogue::Action>(255U), 0U, 601U, 0U)
              .valid);
  assert(!state.recordAction(dialogue::Action::Pet, 0U, 1440U, 0U).valid);
  assert(state.totalActions() == actionsBefore);
  assert(!state.setNickname("BAD\nNAME"));

  // The callback FIFO has a hard cap and due-day addition saturates safely.
  for (uint8_t index = 0U; index < progression::kCallbackCapacity; ++index) {
    assert(state.rememberEvent(index, UINT32_MAX - 1U, 9U));
  }
  assert(!state.rememberEvent(99U, UINT32_MAX - 1U, 9U));
  progression::Callback callback{};
  for (uint8_t index = 0U; index < progression::kCallbackCapacity; ++index) {
    assert(state.takeCallback(UINT32_MAX, callback));
    assert(callback.detail == index && callback.dueDay == UINT32_MAX);
  }
  assert(!state.takeCallback(UINT32_MAX, callback));

  uint8_t snapshot[progression::kSnapshotCapacity]{};
  assert(state.writeSnapshot(snapshot, sizeof(snapshot)));
  progression::CompanionProgression copy;
  assert(!copy.restoreSnapshot(snapshot, state.snapshotSize() - 1U, 606U));
  assert(!copy.restoreSnapshot(snapshot, state.snapshotSize(), 607U));
  assert(copy.restoreSnapshot(snapshot, state.snapshotSize(), 606U));
  assert(copy.recordAction(dialogue::Action::Pet, 1U, 500U, 0U).valid);
  assert(!copy.recordAction(dialogue::Action::Pet, 0U, 500U, 0U).valid);
}

}  // namespace

int main() {
  static_assert(progression::kSnapshotCapacity <= 512U,
                "Progression storage budget unexpectedly grew");
  testMemoryLearningAndActionBehavior();
  testSessionsRequestsQuestionsAndComfort();
  testRitualHabitCallbacksAndLore();
  testDailyProgressionAndRecords();
  testCalendarMomentsAndDisplayCatalogue();
  testBoundedAndInvalidContracts();
  for (uint8_t feature = 1U; feature <= 35U; ++feature) {
    if (!gCovered[feature]) {
      fprintf(stderr, "UNCOVERED feature %u\n", feature);
      return 1;
    }
  }
  printf("PASS companion_progression_host features=35 snapshot=%u\n",
         static_cast<unsigned>(
             progression::CompanionProgression::snapshotSize()));
  return 0;
}
