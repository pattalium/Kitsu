#include "../src/activity_suite.h"

#include <iostream>

namespace activity = kitsu868::activities;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL " << message << '\n';
  }
}

activity::CompanionModifier normalModifier() {
  return activity::CompanionModifier{};
}

void testMorseAndGhost() {
  activity::ActivitySuite suite;
  check(suite.start(activity::ActivityKind::MorseSignal, 100U, 0x1234U, 2U,
                    normalModifier()),
        "89 Morse Signal starts");
  activity::ActivityState state = suite.snapshot();
  const uint32_t inputAt =
      100U + static_cast<uint32_t>(state.patternCount) * state.target;
  suite.tick(inputAt);
  check(suite.view(inputAt).phase == activity::ActivityPhase::Playing,
        "89 Morse presentation advances to replay");
  for (uint8_t index = 0U; index < state.patternCount; ++index) {
    const uint32_t at = inputAt + 100U + index * 500U;
    if (((state.patternBits >> index) & 1U) != 0U) {
      check(suite.press(at) == activity::InputResult::Accepted,
            "89 dash press accepted");
      suite.release(at + 500U);
    } else {
      suite.tap(at);
    }
  }
  check(suite.view(inputAt + 5000U).phase == activity::ActivityPhase::Result &&
            suite.snapshot().score == 1000U,
        "89 exact dot/dash replay earns full score");
  suite.tick(inputAt + 7000U);
  check(suite.startGhost(activity::ActivityKind::MorseSignal,
                         inputAt + 7100U, normalModifier()) &&
            suite.view(inputAt + 7100U).ghostScore == 1000U,
        "94 Ghost Challenge reuses the stored best seed and score");
}

void testTunerReactionHoldAndBreathing() {
  activity::ActivitySuite tuner;
  check(tuner.start(activity::ActivityKind::StaticTuner, 0U, 88U, 2U,
                    normalModifier()),
        "90 Static Tuner starts");
  uint32_t bestAt = 0U;
  uint8_t bestDistance = 255U;
  for (uint32_t at = 0U; at < 3000U; at += 5U) {
    const activity::ActivityView view = tuner.view(at);
    const uint8_t distance = view.marker > view.target
        ? view.marker - view.target : view.target - view.marker;
    if (distance < bestDistance) {
      bestDistance = distance;
      bestAt = at;
    }
  }
  check(tuner.tap(bestAt) == activity::InputResult::Completed &&
            tuner.snapshot().score >= 900U,
        "90 tuning close to the hidden target scores highly");

  activity::ActivitySuite reaction;
  check(reaction.start(activity::ActivityKind::ReactionFlash, 1000U, 44U, 3U,
                       normalModifier()),
        "91 Reaction Flash starts");
  const uint32_t cueDelay = reaction.snapshot().target;
  check(reaction.tap(1000U + cueDelay - 1U) ==
            activity::InputResult::TooEarly &&
            reaction.snapshot().score == 0U,
        "91 premature reaction is truthfully rejected");

  activity::ActivitySuite reactionPerfect;
  reactionPerfect.start(activity::ActivityKind::ReactionFlash, 1000U, 44U,
                        3U, normalModifier());
  check(reactionPerfect.tap(1000U + reactionPerfect.snapshot().target + 60U) ==
            activity::InputResult::Completed &&
            reactionPerfect.snapshot().score == 1000U,
        "91 prompt reaction earns full score");

  activity::ActivitySuite steady;
  steady.start(activity::ActivityKind::HoldSteady, 500U, 99U, 3U,
               normalModifier());
  const uint32_t target = steady.snapshot().target;
  check(steady.press(800U) == activity::InputResult::Accepted &&
            steady.release(800U + target) == activity::InputResult::Completed &&
            steady.snapshot().score == 1000U,
        "92 Hold Steady measures the actual held duration");

  activity::ActivitySuite breathe;
  breathe.start(activity::ActivityKind::PulseBreathing, 100U, 77U, 2U,
                normalModifier());
  const uint32_t period = breathe.snapshot().target;
  check(breathe.tap(100U + period / 2U) == activity::InputResult::Accepted &&
            breathe.tap(100U + period + period / 2U) ==
                activity::InputResult::Accepted &&
            breathe.tap(100U + 2U * period + period / 2U) ==
                activity::InputResult::Completed &&
            breathe.snapshot().score == 1000U,
        "93 Pulse Breathing rewards three taps at calm cycle peaks");
}

void testDailyModifiersAndQol() {
  const activity::DailyActivity dailyA =
      activity::proceduralDailyActivity(22000U, 0xCAFEU);
  const activity::DailyActivity dailyB =
      activity::proceduralDailyActivity(22000U, 0xCAFEU);
  const activity::DailyActivity nextDay =
      activity::proceduralDailyActivity(22001U, 0xCAFEU);
  check(dailyA.kind == dailyB.kind && dailyA.seed == dailyB.seed &&
            dailyA.difficulty == dailyB.difficulty &&
            dailyA.seed != nextDay.seed,
        "95 procedural daily game is stable for a day and rotates next day");

  const activity::CompanionModifier bold = activity::companionModifier(
      kitsu868::PersonalityKind::Bold, kitsu868::CompanionMood::Excited);
  const activity::CompanionModifier gentle = activity::companionModifier(
      kitsu868::PersonalityKind::Gentle, kitsu868::CompanionMood::Drowsy);
  check(bold.speedPercent > gentle.speedPercent &&
            bold.rewardPercent > gentle.rewardPercent &&
            gentle.tolerancePercent > bold.tolerancePercent,
        "96 companion personality and mood produce bounded game modifiers");

  activity::ActivitySuite suite;
  suite.rememberDialogue(321U);
  uint16_t dialogue = 0U;
  check(suite.replayDialogue(dialogue) && dialogue == 321U,
        "97 last dialogue can be replayed by stable catalogue ID");
  check(suite.setQuickAction(activity::QuickAction::Expedition) &&
            suite.quickAction() == activity::QuickAction::Expedition,
        "98 configurable quick action persists an explicit action");
  check(suite.setQuietHours(true, 1320U, 420U) &&
            suite.quietAt(1380U) && suite.quietAt(300U) &&
            !suite.quietAt(720U),
        "99 overnight quiet hours suppress only the configured interval");
}

void testCrashResumeAndIntegrity() {
  activity::ActivitySuite original;
  original.rememberDialogue(900U);
  original.setQuickAction(activity::QuickAction::DailyGame);
  original.setQuietHours(true, 1200U, 360U);
  check(original.start(activity::ActivityKind::HoldSteady, 100U, 55U, 4U,
                       normalModifier()),
        "100 resumable activity starts");
  original.press(400U);
  const activity::ActivityState checkpoint = original.snapshot();
  check(activity::validateActivityState(checkpoint),
        "100 crash checkpoint has a valid CRC and semantic shape");

  activity::ActivitySuite restored;
  check(restored.restore(checkpoint) && restored.resumeAfterCrash(50U) &&
            restored.view(50U).resumed == 1U &&
            restored.view(50U).phase == activity::ActivityPhase::Playing,
        "100 active logical progress resumes with a rebased boot clock");
  check(restored.press(100U) == activity::InputResult::Accepted &&
            restored.release(100U + restored.snapshot().target) ==
                activity::InputResult::Completed,
        "100 resumed activity remains playable rather than auto-completing");
  uint16_t dialogue = 0U;
  check(restored.replayDialogue(dialogue) && dialogue == 900U &&
            restored.quickAction() == activity::QuickAction::DailyGame &&
            restored.quietAt(1300U),
        "97-99 settings survive the same crash-safe snapshot");

  activity::ActivityState corrupt = checkpoint;
  corrupt.crc32 ^= 0x100U;
  check(!restored.restore(corrupt) && !restored.available(),
        "100 corrupt crash state is quarantined fail-closed");
}

void testPersistenceEdges() {
  activity::CompanionModifier altered{};
  altered.speedPercent = 80U;
  altered.tolerancePercent = 70U;
  altered.rewardPercent = 50U;

  activity::ActivitySuite hold;
  check(hold.start(activity::ActivityKind::HoldSteady, 10U, 77U, 4U,
                   altered),
        "96 modified activity starts");
  const uint16_t holdTarget = hold.snapshot().target;
  hold.press(20U);
  activity::ActivitySuite restoredHold;
  check(restoredHold.restore(hold.snapshot()) &&
            restoredHold.resumeAfterCrash(100U) &&
            restoredHold.press(110U) == activity::InputResult::Accepted &&
            restoredHold.release(110U + holdTarget) ==
                activity::InputResult::Completed &&
            restoredHold.snapshot().score == 500U,
        "96/100 crash restore preserves the active companion modifier");

  restoredHold.tick(110U + holdTarget + 1300U);
  check(restoredHold.startGhost(activity::ActivityKind::HoldSteady,
                                110U + holdTarget + 1400U,
                                normalModifier()) &&
            restoredHold.snapshot().difficulty == 4U,
        "94 ghost challenge recreates the recorded difficulty");

  activity::ActivitySuite pulse;
  pulse.start(activity::ActivityKind::PulseBreathing, 100U, 91U, 2U,
              normalModifier());
  const uint32_t period = pulse.snapshot().target;
  pulse.tap(100U + period / 2U);
  pulse.tap(100U + period + period / 2U);
  const activity::ActivityState pulseCheckpoint = pulse.snapshot();
  activity::ActivitySuite restoredPulse;
  check(activity::validateActivityState(pulseCheckpoint) &&
            restoredPulse.restore(pulseCheckpoint) &&
            restoredPulse.resumeAfterCrash(500U) &&
            restoredPulse.tap(500U + period / 2U) ==
                activity::InputResult::Completed &&
            restoredPulse.snapshot().score == 1000U,
        "93/100 pulse accumulator remains valid and resumable mid-game");

  activity::ActivityState invalidPulse = pulseCheckpoint;
  invalidPulse.target = 1U;
  invalidPulse.crc32 = activity::activityStateCrc(invalidPulse);
  check(!activity::validateActivityState(invalidPulse),
        "100 CRC-valid pulse state with a zero-half period is rejected");

  activity::ActivitySuite cancelled;
  cancelled.start(activity::ActivityKind::MorseSignal, 0U, 12U, 2U,
                  normalModifier());
  cancelled.cancel();
  const activity::ActivityState idle = cancelled.snapshot();
  check(activity::validateActivityState(idle) &&
            idle.kind == static_cast<uint8_t>(activity::ActivityKind::None) &&
            idle.patternCount == 0U && idle.target == 0U,
        "100 cancel emits a canonical idle persistence record");
}

}  // namespace

int main() {
  testMorseAndGhost();
  testTunerReactionHoldAndBreathing();
  testDailyModifiersAndQol();
  testCrashResumeAndIntegrity();
  testPersistenceEdges();
  if (failures != 0) {
    std::cerr << "TEST_FAIL activity_suite failures=" << failures << '\n';
    return 1;
  }
  std::cout << "TEST_PASS activity_suite features=89-100 games=5" << '\n';
  return 0;
}
