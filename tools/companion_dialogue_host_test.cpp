#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/companion_dialogue.h"

using namespace kitsu868;

namespace {

void assertDisplayText(const char* line) {
  assert(line != nullptr);
  assert(strlen(line) <= 16U);
}

bool recentContains(const dialogue::ActionState& state, uint16_t id) {
  for (uint8_t index = 0U; index < state.recentCount; ++index) {
    if (state.recent[index] == id) return true;
  }
  return false;
}

dialogue::ActionContext richContext(uint8_t personality) {
  dialogue::ActionContext context{};
  context.personality = static_cast<PersonalityKind>(personality);
  context.mood = CompanionMood::Loved;
  context.vitals.energy = 20U;
  context.vitals.curiosity = 90U;
  context.vitals.affection = 90U;
  context.bondLevel = 5U;
  context.outcome = dialogue::ActionOutcome::Success;
  context.nearby = true;
  return context;
}

void testActionDeterminismAndAntiRepeat() {
  for (uint8_t action = 0U; action < dialogue::kActionCount; ++action) {
    for (uint8_t person = 0U; person < 6U; ++person) {
      dialogue::ActionState first{};
      dialogue::ActionState again{};
      const dialogue::ActionContext context = richContext(person);
      for (uint8_t turn = 0U; turn < 32U; ++turn) {
        const dialogue::ActionLine left = dialogue::selectActionLine(
            static_cast<dialogue::Action>(action), context,
            UINT32_C(0x12340000) + action * 31U + person, first);
        const dialogue::ActionLine right = dialogue::selectActionLine(
            static_cast<dialogue::Action>(action), context,
            UINT32_C(0x12340000) + action * 31U + person, again);
        assert(left.id != 0U);
        assert(left.id == right.id);
        assert(strcmp(left.line1, right.line1) == 0);
        assert(strcmp(left.line2, right.line2) == 0);
        assert(left.flavor == right.flavor);
        assertDisplayText(left.line1);
        assertDisplayText(left.line2);
        assert(dialogue::validateActionState(first));
        assert(dialogue::validateActionState(again));
      }

      dialogue::ActionState antiRepeat{};
      for (uint8_t turn = 0U; turn < 24U; ++turn) {
        const dialogue::ActionState before = antiRepeat;
        const dialogue::ActionLine line = dialogue::selectActionLine(
            static_cast<dialogue::Action>(action), context,
            UINT32_C(0xABC00000) + action * 17U + person, antiRepeat);
        assert(!recentContains(before, line.id));
      }
    }
  }
}

void testActionContextAndTruthfulOutcomes() {
  const dialogue::LineFlavor expected[] = {
      dialogue::LineFlavor::LowEnergy,
      dialogue::LineFlavor::Bonded,
      dialogue::LineFlavor::Curious,
      dialogue::LineFlavor::Nearby,
  };
  for (size_t flavor = 0U; flavor < sizeof(expected) / sizeof(expected[0]);
       ++flavor) {
    bool found = false;
    for (uint32_t seed = 0U; seed < 512U && !found; ++seed) {
      dialogue::ActionState state{};
      const dialogue::ActionLine line = dialogue::selectActionLine(
          dialogue::Action::Pet, richContext(3U), seed, state);
      found = line.flavor == expected[flavor];
    }
    assert(found);
  }

  const dialogue::ActionOutcome outcomes[] = {
      dialogue::ActionOutcome::Failed,
      dialogue::ActionOutcome::Busy,
      dialogue::ActionOutcome::NoReply,
  };
  for (size_t outcome = 0U; outcome < sizeof(outcomes) / sizeof(outcomes[0]);
       ++outcome) {
    dialogue::ActionState state{};
    dialogue::ActionContext context = richContext(5U);
    context.outcome = outcomes[outcome];
    for (uint8_t turn = 0U; turn < 16U; ++turn) {
      const dialogue::ActionLine line = dialogue::selectActionLine(
          dialogue::Action::Listen, context, UINT32_C(0x24681357), state);
      assert(line.id != 0U);
      assert(line.flavor == dialogue::LineFlavor::Outcome);
      assertDisplayText(line.line1);
      assertDisplayText(line.line2);
    }
  }

  dialogue::ActionState state{};
  dialogue::ActionContext invalidOutcome = richContext(0U);
  invalidOutcome.outcome = static_cast<dialogue::ActionOutcome>(255U);
  const dialogue::ActionLine safeFailure = dialogue::selectActionLine(
      dialogue::Action::Pet, invalidOutcome, 4U, state);
  assert(safeFailure.flavor == dialogue::LineFlavor::Outcome);

  const dialogue::ActionState before = state;
  const dialogue::ActionLine fallback = dialogue::selectActionLine(
      static_cast<dialogue::Action>(255U), richContext(0U), 7U, state);
  assert(fallback.id == 0U);
  assert(before.selections == state.selections);
  assert(before.recentHead == state.recentHead);
  assert(before.recentCount == state.recentCount);
  assert(memcmp(before.recent, state.recent, sizeof(state.recent)) == 0);
}

void testActionStateRepairAndReset() {
  dialogue::ActionState bad{};
  bad.recentHead = dialogue::kActionRecentCapacity;
  assert(!dialogue::validateActionState(bad));
  const dialogue::ActionLine repaired = dialogue::selectActionLine(
      dialogue::Action::Wake, richContext(255U), 9U, bad);
  assert(repaired.id != 0U);
  assert(dialogue::validateActionState(bad));

  bad.recent[0] = 0U;
  assert(!dialogue::validateActionState(bad));
  dialogue::resetActionState(bad);
  assert(dialogue::validateActionState(bad));
  assert(bad.selections == 0U && bad.recentCount == 0U);
}

void assertBeat(const dialogue::StoryBeat& beat, bool choice) {
  assert(beat.storyId >= 1U && beat.storyId <= dialogue::kStoryCount);
  assertDisplayText(beat.line1);
  assertDisplayText(beat.line2);
  assert(beat.awaitsChoice == choice);
  for (uint8_t index = 0U; index < dialogue::kStoryChoiceCount; ++index) {
    if (choice) {
      assert(beat.choices[index] != nullptr);
      assert(beat.choices[index][0] != '\0');
      assertDisplayText(beat.choices[index]);
    } else {
      assert(beat.choices[index] == nullptr);
    }
  }
}

void testStoryLifecycleAndCatalogue() {
  const dialogue::StoryTrigger triggers[] = {
      dialogue::StoryTrigger::QuietMoment,
      dialogue::StoryTrigger::ExpeditionReturn,
      dialogue::StoryTrigger::NearbySignal,
  };
  uint32_t seenMask = 0U;
  for (size_t trigger = 0U; trigger < sizeof(triggers) / sizeof(triggers[0]);
       ++trigger) {
    dialogue::StoryState state{};
    uint8_t previous = 0U;
    for (uint8_t storyNumber = 0U; storyNumber < 2U; ++storyNumber) {
      dialogue::StoryBeat opening{};
      assert(dialogue::startStory(triggers[trigger], PersonalityKind::Curious,
                                  UINT32_C(0x10203040), state, opening));
      assertBeat(opening, false);
      assert(opening.scene == 0U);
      assert(opening.storyId != previous);
      previous = opening.storyId;
      seenMask |= UINT32_C(1) << static_cast<uint8_t>(opening.storyId - 1U);

      dialogue::StoryBeat duplicate{};
      assert(!dialogue::startStory(triggers[trigger], PersonalityKind::Gentle,
                                   0U, state, duplicate));
      dialogue::StoryResolution tooEarly{};
      assert(!dialogue::resolveStory(dialogue::StoryChoice::First,
                                     PersonalityKind::Gentle, state,
                                     tooEarly));

      dialogue::StoryBeat prompt{};
      assert(dialogue::advanceStory(state, prompt));
      assertBeat(prompt, true);
      assert(prompt.scene == 1U && prompt.storyId == opening.storyId);
      assert(!dialogue::advanceStory(state, duplicate));

      // Every authored result and personality variant stays OLED-sized and
      // returns only bounded stat effects.
      for (uint8_t person = 0U; person < 6U; ++person) {
        for (uint8_t choice = 0U; choice < dialogue::kStoryChoiceCount;
             ++choice) {
          dialogue::StoryState copy = state;
          dialogue::StoryResolution result{};
          assert(dialogue::resolveStory(
              static_cast<dialogue::StoryChoice>(choice),
              static_cast<PersonalityKind>(person), copy, result));
          assert(result.storyId == opening.storyId);
          assertDisplayText(result.line1);
          assertDisplayText(result.line2);
          assert(result.affectionDelta >= 0 && result.affectionDelta <= 2);
          assert(result.energyDelta >= -2 && result.energyDelta <= 1);
          assert(result.curiosityDelta >= 0 && result.curiosityDelta <= 2);
          assert(dialogue::storyCompleted(copy, result.storyId));
          assert(copy.activeStory == dialogue::kNoActiveStory);
          assert(dialogue::validateStoryState(copy));
        }
      }

      dialogue::StoryResolution result{};
      assert(dialogue::resolveStory(dialogue::StoryChoice::Second,
                                    PersonalityKind::Curious, state, result));
      assert(dialogue::storyCompleted(state, result.storyId));
      assert(state.completions == static_cast<uint16_t>(storyNumber + 1U));
      assert(!dialogue::resolveStory(dialogue::StoryChoice::Second,
                                     PersonalityKind::Curious, state, result));
    }
  }
  assert(seenMask == (UINT32_C(1) << dialogue::kStoryCount) - 1U);
}

void testStoryDeterminismPersonalityAndCancellation() {
  dialogue::StoryState first{};
  dialogue::StoryState again{};
  dialogue::StoryBeat firstOpening{};
  dialogue::StoryBeat againOpening{};
  assert(dialogue::startStory(dialogue::StoryTrigger::QuietMoment,
                              PersonalityKind::Gentle, 99U, first,
                              firstOpening));
  assert(dialogue::startStory(dialogue::StoryTrigger::QuietMoment,
                              PersonalityKind::Gentle, 99U, again,
                              againOpening));
  assert(firstOpening.storyId == againOpening.storyId);
  assert(strcmp(firstOpening.line1, againOpening.line1) == 0);
  assert(first.starts == again.starts);
  assert(first.completedMask == again.completedMask);
  assert(first.completions == again.completions);
  assert(first.activeStory == again.activeStory);
  assert(first.scene == again.scene);
  assert(first.recentHead == again.recentHead);
  assert(first.recentCount == again.recentCount);
  assert(memcmp(first.recent, again.recent, sizeof(first.recent)) == 0);
  assert(dialogue::advanceStory(first, firstOpening));
  assert(dialogue::advanceStory(again, againOpening));

  // Gentle and Bold have different preferred choices in both quiet stories.
  bool observedPersonalityDifference = false;
  for (uint8_t choice = 0U; choice < dialogue::kStoryChoiceCount; ++choice) {
    dialogue::StoryState gentleState = first;
    dialogue::StoryState boldState = first;
    dialogue::StoryResolution gentle{};
    dialogue::StoryResolution bold{};
    assert(dialogue::resolveStory(static_cast<dialogue::StoryChoice>(choice),
                                  PersonalityKind::Gentle, gentleState,
                                  gentle));
    assert(dialogue::resolveStory(static_cast<dialogue::StoryChoice>(choice),
                                  PersonalityKind::Bold, boldState, bold));
    if (gentle.personalityMatch != bold.personalityMatch) {
      assert(strcmp(gentle.line1, bold.line1) != 0 ||
             strcmp(gentle.line2, bold.line2) != 0);
      observedPersonalityDifference = true;
    }
  }
  assert(observedPersonalityDifference);

  dialogue::cancelStory(again);
  assert(again.activeStory == dialogue::kNoActiveStory);
  assert(dialogue::validateStoryState(again));
  assert(!dialogue::currentStoryBeat(again, againOpening));
}

void testStoryValidationRepairAndSaturation() {
  dialogue::StoryState bad{};
  bad.activeStory = dialogue::kStoryCount;
  assert(!dialogue::validateStoryState(bad));
  dialogue::StoryBeat beat{};
  assert(dialogue::startStory(dialogue::StoryTrigger::NearbySignal,
                              static_cast<PersonalityKind>(255U), 17U, bad,
                              beat));
  assert(dialogue::validateStoryState(bad));
  assert(dialogue::advanceStory(bad, beat));
  bad.completions = UINT16_MAX;
  dialogue::StoryResolution result{};
  assert(dialogue::resolveStory(dialogue::StoryChoice::Third,
                                PersonalityKind::Gentle, bad, result));
  assert(bad.completions == UINT16_MAX);

  bad.completedMask |= UINT32_C(1) << dialogue::kStoryCount;
  assert(!dialogue::validateStoryState(bad));
  dialogue::cancelStory(bad);
  assert(dialogue::validateStoryState(bad));
  assert(bad.completedMask == 0U && bad.completions == 0U);

  dialogue::StoryState clean{};
  assert(!dialogue::startStory(static_cast<dialogue::StoryTrigger>(255U),
                               PersonalityKind::Gentle, 0U, clean, beat));
  assert(!dialogue::resolveStory(static_cast<dialogue::StoryChoice>(255U),
                                 PersonalityKind::Gentle, clean, result));
  assert(!dialogue::storyCompleted(clean, 0U));
  assert(!dialogue::storyCompleted(clean,
                                   static_cast<uint8_t>(dialogue::kStoryCount + 1U)));
}

}  // namespace

int main() {
  static_assert(sizeof(dialogue::ActionState) <= 16U,
                "Action anti-repeat state unexpectedly grew");
  static_assert(sizeof(dialogue::StoryState) <= 20U,
                "Micro-story state unexpectedly grew");

  testActionDeterminismAndAntiRepeat();
  testActionContextAndTruthfulOutcomes();
  testActionStateRepairAndReset();
  testStoryLifecycleAndCatalogue();
  testStoryDeterminismPersonalityAndCancellation();
  testStoryValidationRepairAndSaturation();
  puts("PASS companion_dialogue_host");
  return 0;
}
