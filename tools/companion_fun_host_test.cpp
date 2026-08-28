#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/companion_fun.h"

using namespace kitsu868;

namespace {

void testPersonalityMomentsAndDreams() {
  const fun::Moment gentle = fun::personalityMoment(
      PersonalityKind::Gentle, fun::MomentTrigger::Pet);
  const fun::Moment impish = fun::personalityMoment(
      PersonalityKind::Impish, fun::MomentTrigger::Pet);
  assert(strcmp(gentle.line1, impish.line1) != 0);
  assert(gentle.reaction == fun::Reaction::Pet);
  assert(impish.reaction == fun::Reaction::Surprise);

  const fun::Moment calmAmbient = fun::personalityMoment(
      PersonalityKind::Gentle, fun::MomentTrigger::RareAmbient, 0U);
  const fun::Moment surpriseAmbient = fun::personalityMoment(
      PersonalityKind::Gentle, fun::MomentTrigger::RareAmbient, 3U);
  assert(strcmp(calmAmbient.line1, surpriseAmbient.line1) != 0);
  const fun::Moment invalidPersonality = fun::personalityMoment(
      static_cast<PersonalityKind>(255U), fun::MomentTrigger::Pet);
  assert(strcmp(invalidPersonality.line1, gentle.line1) == 0);

  const fun::Dream first = fun::selectDream(
      PersonalityKind::Curious, UINT32_C(0x12345678), 2U);
  const fun::Dream repeat = fun::selectDream(
      PersonalityKind::Curious, UINT32_C(0x12345678), 2U);
  const fun::Dream next = fun::selectDream(
      PersonalityKind::Curious, UINT32_C(0x12345678), 3U);
  assert(first.index == repeat.index);
  assert(strcmp(first.line1, repeat.line1) == 0);
  assert(first.index < fun::kDreamCount);
  assert(next.index < fun::kDreamCount);
  assert(next.index != first.index);
}

void testSessionChallenges() {
  fun::SessionChallenges state{};
  fun::ChallengeUpdate update = fun::recordSessionActivity(
      state, fun::SessionActivity::Care);
  assert(update.newlyCompletedMask == 0U);
  update = fun::recordSessionActivity(state, fun::SessionActivity::Care);
  assert(update.newlyCompletedMask == 1U);
  assert(update.allCompletedNow == 0U);
  update = fun::recordSessionActivity(state, fun::SessionActivity::Game);
  assert(update.newlyCompletedMask == 2U);
  update = fun::recordSessionActivity(state, fun::SessionActivity::Signal);
  assert(update.newlyCompletedMask == 4U);
  assert(update.allCompletedNow == 1U);
  update = fun::recordSessionActivity(state, fun::SessionActivity::Signal);
  assert(update.newlyCompletedMask == 0U);
  assert(update.allCompletedNow == 0U);
  assert(state.care == 2U && state.games == 1U && state.signals == 1U);
  assert(fun::challengeTarget(fun::SessionActivity::Care) == 2U);
  assert(strcmp(fun::challengeName(fun::SessionActivity::Signal), "SIGNAL") == 0);

  fun::resetSessionChallenges(state);
  assert(state.care == 0U && state.games == 0U && state.signals == 0U);
  assert(state.completedMask == 0U && state.rewardClaimed == 0U);
  update = fun::recordSessionActivity(
      state, static_cast<fun::SessionActivity>(255U));
  assert(update.newlyCompletedMask == 0U && update.allCompletedNow == 0U);
  assert(state.care == 0U && state.games == 0U && state.signals == 0U);
}

void testDiscoveryState() {
  fun::DiscoveryState state{};
  assert(fun::validateDiscoveryState(state));
  assert(fun::seenCreatureCount(state) == 0U);
  assert(fun::recordCreatureEncounter(state, 4U, 2U));
  assert(fun::recordCreatureEncounter(state, 4U, 3U));
  assert(fun::recordCreatureEncounter(state, 20U, 7U));
  assert(fun::creatureSeen(state, 4U));
  assert(fun::creatureEncounterCount(state, 4U) == 2U);
  assert(state.lastSources[4] == 3U);
  assert(fun::seenCreatureCount(state) == 2U);
  assert(!fun::recordCreatureEncounter(state, 21U, 0U));
  assert(fun::recordDream(state, 3U));
  assert(fun::recordDream(state, 5U));
  fun::recordRareReaction(state);
  assert(state.completedDreams == 2U);
  assert(state.rareReactions == 1U);
  uint8_t dream = 0U;
  assert(fun::recentDream(state, 0U, dream) && dream == 5U);
  assert(fun::recentDream(state, 1U, dream) && dream == 3U);
  assert(!fun::recentDream(state, 2U, dream));
  assert(!fun::recordDream(state, fun::kDreamCount));

  state.seenMask |= UINT32_C(1) << 7U;
  assert(!fun::validateDiscoveryState(state));
  assert(!fun::recordCreatureEncounter(state, 1U, 0U));
}

void testDreamJournalRingAndValidation() {
  fun::DiscoveryState journal{};
  for (uint8_t index = 0U; index < 10U; ++index) {
    assert(fun::recordDream(journal, index));
  }
  assert(fun::validateDiscoveryState(journal));
  assert(journal.completedDreams == 10U);
  assert(journal.dreamHistoryCount == fun::kDreamHistoryCapacity);
  assert(journal.dreamHead == 2U);
  for (uint8_t newest = 0U; newest < fun::kDreamHistoryCapacity; ++newest) {
    uint8_t dream = 0U;
    assert(fun::recentDream(journal, newest, dream));
    assert(dream == static_cast<uint8_t>(9U - newest));
  }
  uint8_t dream = 0U;
  assert(!fun::recentDream(journal, fun::kDreamHistoryCapacity, dream));

  fun::DiscoveryState badCount = journal;
  badCount.dreamHistoryCount = 7U;
  assert(!fun::validateDiscoveryState(badCount));

  fun::DiscoveryState badHead{};
  assert(fun::recordDream(badHead, 1U));
  badHead.dreamHead = 0U;
  assert(!fun::validateDiscoveryState(badHead));

  fun::DiscoveryState badDream = journal;
  badDream.dreamHistory[3] = fun::kDreamCount;
  assert(!fun::validateDiscoveryState(badDream));

  fun::DiscoveryState badUnseenSource{};
  badUnseenSource.lastSources[3] = 1U;
  assert(!fun::validateDiscoveryState(badUnseenSource));
}

void testDiscoveryCounterSaturationAndReset() {
  fun::DiscoveryState state{};
  state.seenMask = UINT32_C(1) << 3U;
  state.encounterCounts[3] = UINT16_MAX;
  assert(fun::validateDiscoveryState(state));
  assert(fun::recordCreatureEncounter(state, 3U, 9U));
  assert(state.encounterCounts[3] == UINT16_MAX);
  assert(state.lastSources[3] == 9U);

  state.rareReactions = UINT16_MAX;
  fun::recordRareReaction(state);
  assert(state.rareReactions == UINT16_MAX);

  fun::resetDiscoveryState(state);
  assert(fun::validateDiscoveryState(state));
  assert(fun::seenCreatureCount(state) == 0U);
  assert(state.completedDreams == 0U && state.rareReactions == 0U);
}

}  // namespace

int main() {
  static_assert(sizeof(fun::SessionChallenges) == 5U,
                "Session challenge state must stay compact");
  static_assert(sizeof(fun::DiscoveryState) <= 96U,
                "Discovery state unexpectedly grew");

  testPersonalityMomentsAndDreams();
  testSessionChallenges();
  testDiscoveryState();
  testDreamJournalRingAndValidation();
  testDiscoveryCounterSaturationAndReset();
  puts("PASS companion_fun_host");
  return 0;
}
