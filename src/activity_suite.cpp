#include "activity_suite.h"

#include <string.h>

namespace kitsu868 {
namespace activities {

namespace {

constexpr uint32_t kMagic = UINT32_C(0x31544341);
constexpr uint8_t kFlagPressed = 0x01U;
constexpr uint8_t kFlagResumed = 0x02U;
constexpr uint8_t kFlagGhost = 0x04U;
constexpr uint32_t kResultDurationMs = 1200U;

uint32_t crc32Bytes(const uint8_t* bytes, size_t length) {
  uint32_t crc = UINT32_C(0xFFFFFFFF);
  for (size_t index = 0U; index < length; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (UINT32_C(0xEDB88320) & mask);
    }
  }
  return ~crc;
}

uint32_t mix32(uint32_t value) {
  value ^= value >> 16U;
  value *= UINT32_C(0x7FEB352D);
  value ^= value >> 15U;
  value *= UINT32_C(0x846CA68B);
  value ^= value >> 16U;
  return value == 0U ? 1U : value;
}

uint16_t absDifference(uint32_t left, uint32_t right) {
  const uint32_t difference = left > right ? left - right : right - left;
  return difference > UINT16_MAX ? UINT16_MAX
                                 : static_cast<uint16_t>(difference);
}

bool validKind(ActivityKind kind) {
  return kind >= ActivityKind::MorseSignal &&
         kind <= ActivityKind::PulseBreathing;
}

bool validPhase(ActivityPhase phase) {
  return phase <= ActivityPhase::Finished;
}

}  // namespace

uint32_t activityStateCrc(const ActivityState& state) {
  return crc32Bytes(reinterpret_cast<const uint8_t*>(&state),
                    offsetof(ActivityState, crc32));
}

bool validateActivityState(const ActivityState& state) {
  if (state.magic != kMagic || state.bytes != sizeof(ActivityState) ||
      state.schemaVersion != kStateSchemaVersion ||
      !validPhase(static_cast<ActivityPhase>(state.phase)) ||
      state.difficulty == 0U || state.difficulty > 5U ||
      (state.flags & ~(kFlagPressed | kFlagResumed | kFlagGhost)) != 0U ||
      state.patternCount > 8U || state.inputCount > state.patternCount ||
      state.successCount > state.patternCount || state.pulseCount > 3U ||
      state.maximumScore == 0U || state.score > state.maximumScore ||
      state.quickAction > static_cast<uint8_t>(QuickAction::Expedition) ||
      state.quietHoursEnabled > 1U || state.quietStartMinute >= 1440U ||
      state.quietEndMinute >= 1440U || state.modifierSpeedPercent < 50U ||
      state.modifierSpeedPercent > 150U ||
      state.modifierTolerancePercent < 50U ||
      state.modifierTolerancePercent > 150U ||
      state.modifierRewardPercent < 50U ||
      state.modifierRewardPercent > 150U || state.reserved != 0U ||
      state.crc32 != activityStateCrc(state)) {
    return false;
  }
  for (uint8_t index = 0U; index < kActivityCount; ++index) {
    const bool hasGhost = state.bestScores[index] != 0U;
    if (state.bestScores[index] > state.maximumScore ||
        hasGhost != (state.ghostSeeds[index] != 0U) ||
        hasGhost != (state.ghostDifficulties[index] != 0U) ||
        state.ghostDifficulties[index] > 5U) {
      return false;
    }
  }
  const ActivityKind kind = static_cast<ActivityKind>(state.kind);
  const ActivityPhase phase = static_cast<ActivityPhase>(state.phase);
  if (kind == ActivityKind::None) {
    if (phase != ActivityPhase::Idle || state.flags != 0U ||
        state.patternCount != 0U || state.inputCount != 0U ||
        state.successCount != 0U || state.pulseCount != 0U ||
        state.patternBits != 0U || state.inputBits != 0U || state.score != 0U ||
        state.target != 0U || state.auxiliary != 0U || state.dayId != 0U) {
      return false;
    }
  } else if (!validKind(kind) || phase == ActivityPhase::Idle) {
    return false;
  }
  if (validKind(kind)) {
    if (state.seed == 0U || state.target == 0U ||
        (phase == ActivityPhase::Presenting &&
         kind != ActivityKind::MorseSignal) ||
        ((state.flags & kFlagPressed) != 0U &&
         (phase != ActivityPhase::Playing ||
          (kind != ActivityKind::MorseSignal &&
           kind != ActivityKind::HoldSteady)))) {
      return false;
    }
    if (kind == ActivityKind::MorseSignal) {
      const uint16_t mask = static_cast<uint16_t>(
          (1U << state.patternCount) - 1U);
      if (state.patternCount < 4U || state.auxiliary != 0U ||
          state.pulseCount != 0U || state.successCount != 0U ||
          (state.patternBits & static_cast<uint16_t>(~mask)) != 0U ||
          (state.inputBits & static_cast<uint16_t>(~mask)) != 0U) {
        return false;
      }
    } else {
      if (state.patternCount != 0U || state.inputCount != 0U ||
          state.successCount != 0U || state.patternBits != 0U ||
          state.inputBits != 0U) {
        return false;
      }
      if (kind == ActivityKind::StaticTuner) {
        if (state.target < 10U || state.target > 90U || state.auxiliary < 2U ||
            state.pulseCount != 0U) {
          return false;
        }
      } else if (kind == ActivityKind::PulseBreathing) {
        if (state.target < 2U || state.auxiliary > 3000U ||
            (phase == ActivityPhase::Playing && state.pulseCount >= 3U)) {
          return false;
        }
      } else if (state.auxiliary != 0U || state.pulseCount != 0U) {
        return false;
      }
    }
  }
  return true;
}

const char* activityName(ActivityKind kind) {
  switch (kind) {
    case ActivityKind::MorseSignal: return "MORSE";
    case ActivityKind::StaticTuner: return "TUNER";
    case ActivityKind::ReactionFlash: return "FLASH";
    case ActivityKind::HoldSteady: return "STEADY";
    case ActivityKind::PulseBreathing: return "BREATHE";
    default: return "NONE";
  }
}

CompanionModifier companionModifier(PersonalityKind personality,
                                    CompanionMood mood) {
  CompanionModifier output{};
  switch (personality) {
    case PersonalityKind::Playful:
      output.speedPercent = 110U;
      output.rewardPercent = 105U;
      break;
    case PersonalityKind::Bold:
      output.speedPercent = 115U;
      output.tolerancePercent = 90U;
      output.rewardPercent = 110U;
      break;
    case PersonalityKind::Curious:
      output.speedPercent = 105U;
      output.tolerancePercent = 105U;
      break;
    case PersonalityKind::Gentle:
    default:
      output.speedPercent = 95U;
      output.tolerancePercent = 115U;
      break;
  }
  if (mood == CompanionMood::Drowsy) {
    output.speedPercent = static_cast<uint8_t>(output.speedPercent * 85U / 100U);
    output.tolerancePercent = static_cast<uint8_t>(
        output.tolerancePercent < 120U ? output.tolerancePercent + 10U : 130U);
  } else if (mood == CompanionMood::Excited) {
    output.speedPercent = static_cast<uint8_t>(
        output.speedPercent > 118U ? 125U : output.speedPercent + 7U);
  }
  return output;
}

DailyActivity proceduralDailyActivity(uint32_t dayId,
                                      uint32_t companionFingerprint) {
  const uint32_t seed = mix32(dayId ^ companionFingerprint ^ UINT32_C(0x4B495453));
  DailyActivity output{};
  output.kind = static_cast<ActivityKind>(1U + seed % kActivityCount);
  output.difficulty = static_cast<uint8_t>(1U + (seed >> 8U) % 5U);
  output.seed = seed;
  return output;
}

ActivitySuite::ActivitySuite() { reset(); }

void ActivitySuite::refreshCrc() { state_.crc32 = activityStateCrc(state_); }

void ActivitySuite::reset() {
  state_ = ActivityState{};
  modifier_ = CompanionModifier{};
  refreshCrc();
  available_ = true;
}

bool ActivitySuite::restore(const ActivityState& state) {
  if (!validateActivityState(state)) {
    state_ = ActivityState{};
    refreshCrc();
    available_ = false;
    return false;
  }
  state_ = state;
  modifier_.speedPercent = state_.modifierSpeedPercent;
  modifier_.tolerancePercent = state_.modifierTolerancePercent;
  modifier_.rewardPercent = state_.modifierRewardPercent;
  available_ = true;
  return true;
}

uint8_t ActivitySuite::activityIndex(ActivityKind kind) const {
  return static_cast<uint8_t>(kind) - 1U;
}

bool ActivitySuite::start(ActivityKind kind, uint32_t nowMs, uint32_t seed,
                          uint8_t difficulty,
                          const CompanionModifier& modifier,
                          uint32_t dayId) {
  if (!available_ || !validKind(kind) || seed == 0U || difficulty == 0U ||
      difficulty > 5U || modifier.speedPercent < 50U ||
      modifier.speedPercent > 150U || modifier.tolerancePercent < 50U ||
      modifier.tolerancePercent > 150U || modifier.rewardPercent < 50U ||
      modifier.rewardPercent > 150U) {
    return false;
  }
  if (static_cast<ActivityPhase>(state_.phase) != ActivityPhase::Idle &&
      static_cast<ActivityPhase>(state_.phase) != ActivityPhase::Finished) {
    return false;
  }
  const uint16_t previousBest[kActivityCount] = {
      state_.bestScores[0], state_.bestScores[1], state_.bestScores[2],
      state_.bestScores[3], state_.bestScores[4]};
  const uint32_t previousSeeds[kActivityCount] = {
      state_.ghostSeeds[0], state_.ghostSeeds[1], state_.ghostSeeds[2],
      state_.ghostSeeds[3], state_.ghostSeeds[4]};
  const uint8_t previousDifficulties[kActivityCount] = {
      state_.ghostDifficulties[0], state_.ghostDifficulties[1],
      state_.ghostDifficulties[2], state_.ghostDifficulties[3],
      state_.ghostDifficulties[4]};
  const uint16_t dialogue = state_.lastDialogueId;
  const uint16_t quietStart = state_.quietStartMinute;
  const uint16_t quietEnd = state_.quietEndMinute;
  const uint8_t quick = state_.quickAction;
  const uint8_t quietEnabled = state_.quietHoursEnabled;
  state_ = ActivityState{};
  for (uint8_t index = 0U; index < kActivityCount; ++index) {
    state_.bestScores[index] = previousBest[index];
    state_.ghostSeeds[index] = previousSeeds[index];
    state_.ghostDifficulties[index] = previousDifficulties[index];
  }
  state_.lastDialogueId = dialogue;
  state_.quietStartMinute = quietStart;
  state_.quietEndMinute = quietEnd;
  state_.quickAction = quick;
  state_.quietHoursEnabled = quietEnabled;
  state_.kind = static_cast<uint8_t>(kind);
  state_.phase = static_cast<uint8_t>(
      kind == ActivityKind::MorseSignal ? ActivityPhase::Presenting
                                        : ActivityPhase::Playing);
  state_.difficulty = difficulty;
  state_.seed = seed;
  state_.dayId = dayId;
  state_.phaseStartedAt = nowMs;
  state_.modifierSpeedPercent = modifier.speedPercent;
  state_.modifierTolerancePercent = modifier.tolerancePercent;
  state_.modifierRewardPercent = modifier.rewardPercent;
  modifier_ = modifier;

  if (kind == ActivityKind::MorseSignal) {
    state_.patternCount = static_cast<uint8_t>(3U + difficulty);
    if (state_.patternCount > 8U) state_.patternCount = 8U;
    state_.patternBits = static_cast<uint16_t>(
        mix32(seed) & ((1U << state_.patternCount) - 1U));
    state_.target = static_cast<uint16_t>(
        450U * 100U / modifier.speedPercent);
  } else if (kind == ActivityKind::StaticTuner) {
    state_.target = static_cast<uint16_t>(10U + mix32(seed) % 81U);
    state_.auxiliary = static_cast<uint16_t>(
        2600U * 100U / modifier.speedPercent);
  } else if (kind == ActivityKind::ReactionFlash) {
    state_.target = static_cast<uint16_t>(
        (900U + mix32(seed) % 2201U) * 100U / modifier.speedPercent);
  } else if (kind == ActivityKind::HoldSteady) {
    state_.target = static_cast<uint16_t>(
        (1000U + difficulty * 350U) * 100U / modifier.speedPercent);
  } else {
    state_.target = static_cast<uint16_t>(
        (1400U - difficulty * 100U) * 100U / modifier.speedPercent);
  }
  refreshCrc();
  return true;
}

bool ActivitySuite::startDaily(uint32_t dayId,
                               uint32_t companionFingerprint,
                               uint32_t nowMs,
                               const CompanionModifier& modifier) {
  if (dayId == 0U) return false;
  const DailyActivity daily =
      proceduralDailyActivity(dayId, companionFingerprint);
  return start(daily.kind, nowMs, daily.seed, daily.difficulty, modifier,
               dayId);
}

bool ActivitySuite::startGhost(ActivityKind kind, uint32_t nowMs,
                               const CompanionModifier& modifier) {
  if (!validKind(kind)) return false;
  const uint8_t index = activityIndex(kind);
  if (state_.bestScores[index] == 0U || state_.ghostSeeds[index] == 0U) {
    return false;
  }
  const uint32_t ghostSeed = state_.ghostSeeds[index];
  const uint8_t ghostDifficulty = state_.ghostDifficulties[index];
  if (ghostDifficulty == 0U ||
      !start(kind, nowMs, ghostSeed, ghostDifficulty, modifier)) {
    return false;
  }
  state_.flags = static_cast<uint8_t>(state_.flags | kFlagGhost);
  refreshCrc();
  return true;
}

uint32_t ActivitySuite::elapsed(uint32_t nowMs) const {
  return static_cast<uint32_t>(nowMs - state_.phaseStartedAt);
}

uint16_t ActivitySuite::timingScore(uint32_t errorMs, uint32_t perfectMs,
                                    uint32_t maximumMs) const {
  perfectMs = perfectMs * modifier_.tolerancePercent / 100U;
  maximumMs = maximumMs * modifier_.tolerancePercent / 100U;
  if (errorMs <= perfectMs) return 1000U;
  if (errorMs >= maximumMs || maximumMs <= perfectMs) return 0U;
  return static_cast<uint16_t>((maximumMs - errorMs) * 1000U /
                               (maximumMs - perfectMs));
}

uint8_t ActivitySuite::tunerMarker(uint32_t nowMs) const {
  const uint32_t period = state_.auxiliary == 0U ? 2000U : state_.auxiliary;
  const uint32_t position = elapsed(nowMs) % period;
  const uint32_t half = period / 2U;
  const uint32_t scaled = position <= half ? position : period - position;
  return static_cast<uint8_t>(scaled * 100U / half);
}

uint16_t ActivitySuite::pulseScore(uint32_t nowMs) const {
  const uint32_t period = state_.target == 0U ? 1000U : state_.target;
  const uint32_t position = elapsed(nowMs) % period;
  const uint32_t peak = period / 2U;
  return timingScore(absDifference(position, peak), 80U, period / 2U);
}

void ActivitySuite::finish(uint16_t score, uint32_t nowMs) {
  uint32_t rewarded = static_cast<uint32_t>(score) * modifier_.rewardPercent /
                      100U;
  if (rewarded > 1000U) rewarded = 1000U;
  state_.score = static_cast<uint16_t>(rewarded);
  state_.phase = static_cast<uint8_t>(ActivityPhase::Result);
  state_.phaseStartedAt = nowMs;
  state_.flags = static_cast<uint8_t>(state_.flags & ~kFlagPressed);
  const ActivityKind kind = static_cast<ActivityKind>(state_.kind);
  const uint8_t index = activityIndex(kind);
  if ((state_.flags & kFlagGhost) == 0U &&
      state_.score > state_.bestScores[index]) {
    state_.bestScores[index] = state_.score;
    state_.ghostSeeds[index] = state_.seed;
    state_.ghostDifficulties[index] = state_.difficulty;
  }
  refreshCrc();
}

void ActivitySuite::tick(uint32_t nowMs) {
  if (!available_) return;
  const ActivityPhase phase = static_cast<ActivityPhase>(state_.phase);
  const ActivityKind kind = static_cast<ActivityKind>(state_.kind);
  if (phase == ActivityPhase::Presenting &&
      kind == ActivityKind::MorseSignal) {
    const uint32_t duration =
        static_cast<uint32_t>(state_.patternCount) * state_.target;
    if (elapsed(nowMs) >= duration) {
      state_.phase = static_cast<uint8_t>(ActivityPhase::Playing);
      state_.phaseStartedAt = nowMs;
      refreshCrc();
    }
  } else if (phase == ActivityPhase::Playing &&
             kind == ActivityKind::ReactionFlash &&
             elapsed(nowMs) > static_cast<uint32_t>(state_.target) + 1600U) {
    finish(0U, nowMs);
  } else if (phase == ActivityPhase::Playing &&
             kind == ActivityKind::PulseBreathing &&
             state_.pulseCount >= 3U) {
    const uint16_t score = static_cast<uint16_t>(
        static_cast<uint32_t>(state_.auxiliary) / 3U);
    finish(score, nowMs);
  } else if (phase == ActivityPhase::Result &&
             elapsed(nowMs) >= kResultDurationMs) {
    state_.phase = static_cast<uint8_t>(ActivityPhase::Finished);
    refreshCrc();
  }
}

InputResult ActivitySuite::tap(uint32_t nowMs) {
  if (!available_) return InputResult::Invalid;
  const ActivityPhase phase = static_cast<ActivityPhase>(state_.phase);
  const ActivityKind kind = static_cast<ActivityKind>(state_.kind);
  if (phase != ActivityPhase::Playing) return InputResult::Ignored;
  if (kind == ActivityKind::MorseSignal) {
    if (state_.inputCount >= state_.patternCount) return InputResult::Ignored;
    ++state_.inputCount;  // A tap is a dot (zero bit).
    if (state_.inputCount == state_.patternCount) {
      const uint16_t mask = static_cast<uint16_t>(
          (1U << state_.patternCount) - 1U);
      uint16_t difference = static_cast<uint16_t>(
          (state_.patternBits ^ state_.inputBits) & mask);
      uint8_t wrong = 0U;
      while (difference != 0U) {
        wrong = static_cast<uint8_t>(wrong + (difference & 1U));
        difference >>= 1U;
      }
      finish(static_cast<uint16_t>(
                 (state_.patternCount - wrong) * 1000U / state_.patternCount),
             nowMs);
      return InputResult::Completed;
    }
    refreshCrc();
    return InputResult::Accepted;
  }
  if (kind == ActivityKind::StaticTuner) {
    const uint8_t marker = tunerMarker(nowMs);
    const uint8_t target = static_cast<uint8_t>(state_.target);
    const uint8_t distance = marker > target ? marker - target : target - marker;
    finish(timingScore(distance, 2U, 25U), nowMs);
    return InputResult::Completed;
  }
  if (kind == ActivityKind::ReactionFlash) {
    const uint32_t sinceStart = elapsed(nowMs);
    if (sinceStart < state_.target) {
      finish(0U, nowMs);
      return InputResult::TooEarly;
    }
    finish(timingScore(sinceStart - state_.target, 120U, 900U), nowMs);
    return InputResult::Completed;
  }
  if (kind == ActivityKind::PulseBreathing) {
    const uint16_t score = pulseScore(nowMs);
    state_.auxiliary = static_cast<uint16_t>(state_.auxiliary + score);
    ++state_.pulseCount;
    if (state_.pulseCount >= 3U) {
      finish(static_cast<uint16_t>(state_.auxiliary / 3U), nowMs);
      return InputResult::Completed;
    }
    refreshCrc();
    return InputResult::Accepted;
  }
  return InputResult::Ignored;
}

InputResult ActivitySuite::press(uint32_t nowMs) {
  if (!available_) return InputResult::Invalid;
  const ActivityKind kind = static_cast<ActivityKind>(state_.kind);
  if (static_cast<ActivityPhase>(state_.phase) != ActivityPhase::Playing ||
      (kind != ActivityKind::HoldSteady &&
       kind != ActivityKind::MorseSignal) ||
      (state_.flags & kFlagPressed) != 0U) {
    return InputResult::Ignored;
  }
  state_.flags = static_cast<uint8_t>(state_.flags | kFlagPressed);
  state_.inputStartedAt = nowMs;
  refreshCrc();
  return InputResult::Accepted;
}

InputResult ActivitySuite::release(uint32_t nowMs) {
  if (!available_) return InputResult::Invalid;
  const ActivityKind kind = static_cast<ActivityKind>(state_.kind);
  if (static_cast<ActivityPhase>(state_.phase) != ActivityPhase::Playing) {
    return InputResult::Ignored;
  }
  if (kind == ActivityKind::MorseSignal) {
    if ((state_.flags & kFlagPressed) == 0U) return InputResult::Ignored;
    if (state_.inputCount >= state_.patternCount) return InputResult::Ignored;
    const uint32_t held = static_cast<uint32_t>(nowMs - state_.inputStartedAt);
    state_.flags = static_cast<uint8_t>(state_.flags & ~kFlagPressed);
    if (held >= 350U) {
      state_.inputBits = static_cast<uint16_t>(
          state_.inputBits | (1U << state_.inputCount));
    }
    return tap(nowMs);
  }
  if (kind != ActivityKind::HoldSteady ||
      (state_.flags & kFlagPressed) == 0U) {
    return InputResult::Ignored;
  }
  const uint32_t held = static_cast<uint32_t>(nowMs - state_.inputStartedAt);
  state_.flags = static_cast<uint8_t>(state_.flags & ~kFlagPressed);
  finish(timingScore(absDifference(held, state_.target), 100U, 900U), nowMs);
  return InputResult::Completed;
}

void ActivitySuite::cancel() {
  if (!available_) return;
  state_.kind = static_cast<uint8_t>(ActivityKind::None);
  state_.phase = static_cast<uint8_t>(ActivityPhase::Idle);
  state_.difficulty = 1U;
  state_.flags = 0U;
  state_.patternCount = 0U;
  state_.inputCount = 0U;
  state_.successCount = 0U;
  state_.pulseCount = 0U;
  state_.patternBits = 0U;
  state_.inputBits = 0U;
  state_.score = 0U;
  state_.maximumScore = 1000U;
  state_.target = 0U;
  state_.auxiliary = 0U;
  state_.seed = 1U;
  state_.dayId = 0U;
  state_.phaseStartedAt = 0U;
  state_.inputStartedAt = 0U;
  state_.modifierSpeedPercent = 100U;
  state_.modifierTolerancePercent = 100U;
  state_.modifierRewardPercent = 100U;
  modifier_ = CompanionModifier{};
  refreshCrc();
}

bool ActivitySuite::resumeAfterCrash(uint32_t nowMs) {
  if (!available_) return false;
  const ActivityPhase phase = static_cast<ActivityPhase>(state_.phase);
  if (phase != ActivityPhase::Presenting && phase != ActivityPhase::Playing &&
      phase != ActivityPhase::Result) {
    return false;
  }
  state_.phaseStartedAt = nowMs;
  state_.inputStartedAt = nowMs;
  state_.flags = static_cast<uint8_t>(
      (state_.flags & ~(kFlagPressed)) | kFlagResumed);
  refreshCrc();
  return true;
}

ActivityView ActivitySuite::view(uint32_t nowMs) const {
  ActivityView output{};
  output.kind = static_cast<ActivityKind>(state_.kind);
  output.phase = static_cast<ActivityPhase>(state_.phase);
  output.difficulty = state_.difficulty;
  output.score = state_.score;
  output.maximumScore = state_.maximumScore;
  output.resumed = (state_.flags & kFlagResumed) != 0U ? 1U : 0U;
  if (validKind(output.kind)) {
    output.ghostScore = state_.bestScores[activityIndex(output.kind)];
  }
  if (output.kind == ActivityKind::MorseSignal) {
    output.total = state_.patternCount;
    output.progress = output.phase == ActivityPhase::Presenting
        ? static_cast<uint8_t>(elapsed(nowMs) / state_.target)
        : state_.inputCount;
    if (output.progress > output.total) output.progress = output.total;
    if (output.phase == ActivityPhase::Presenting &&
        output.progress < state_.patternCount) {
      output.cueOn = static_cast<uint8_t>(
          (state_.patternBits >> output.progress) & 1U);
      const uint32_t duration =
          static_cast<uint32_t>(state_.patternCount) * state_.target;
      output.remainingMs = elapsed(nowMs) >= duration
          ? 0U : duration - elapsed(nowMs);
    }
  } else if (output.kind == ActivityKind::StaticTuner) {
    output.marker = tunerMarker(nowMs);
    output.target = static_cast<uint8_t>(state_.target);
  } else if (output.kind == ActivityKind::ReactionFlash) {
    output.cueOn = elapsed(nowMs) >= state_.target ? 1U : 0U;
    output.remainingMs = output.cueOn != 0U ? 0U : state_.target - elapsed(nowMs);
  } else if (output.kind == ActivityKind::HoldSteady) {
    output.target = static_cast<uint8_t>(
        state_.target > 2550U ? 255U : state_.target / 10U);
    if ((state_.flags & kFlagPressed) != 0U) {
      output.progress = static_cast<uint8_t>(
          static_cast<uint32_t>(nowMs - state_.inputStartedAt) >= state_.target
              ? 100U
              : static_cast<uint32_t>(nowMs - state_.inputStartedAt) * 100U /
                    state_.target);
    }
  } else if (output.kind == ActivityKind::PulseBreathing) {
    output.progress = state_.pulseCount;
    output.total = 3U;
    const uint32_t period = state_.target;
    const uint32_t position = elapsed(nowMs) % period;
    const uint32_t half = period / 2U;
    output.marker = static_cast<uint8_t>(
        (position <= half ? position : period - position) * 100U / half);
  }
  if (output.phase == ActivityPhase::Result) {
    const uint32_t sinceResult = elapsed(nowMs);
    output.remainingMs = sinceResult >= kResultDurationMs
        ? 0U : kResultDurationMs - sinceResult;
  }
  return output;
}

void ActivitySuite::rememberDialogue(uint16_t dialogueId) {
  if (!available_ || dialogueId == 0U) return;
  state_.lastDialogueId = dialogueId;
  refreshCrc();
}

bool ActivitySuite::replayDialogue(uint16_t& dialogueId) const {
  if (!available_ || state_.lastDialogueId == 0U) return false;
  dialogueId = state_.lastDialogueId;
  return true;
}

bool ActivitySuite::setQuickAction(QuickAction action) {
  if (!available_ || action > QuickAction::Expedition) return false;
  state_.quickAction = static_cast<uint8_t>(action);
  refreshCrc();
  return true;
}

QuickAction ActivitySuite::quickAction() const {
  return static_cast<QuickAction>(state_.quickAction);
}

bool ActivitySuite::setQuietHours(bool enabled, uint16_t startMinute,
                                  uint16_t endMinute) {
  if (!available_ || startMinute >= 1440U || endMinute >= 1440U ||
      (enabled && startMinute == endMinute)) {
    return false;
  }
  state_.quietHoursEnabled = enabled ? 1U : 0U;
  state_.quietStartMinute = startMinute;
  state_.quietEndMinute = endMinute;
  refreshCrc();
  return true;
}

bool ActivitySuite::quietAt(uint16_t localMinute) const {
  if (!available_ || state_.quietHoursEnabled == 0U || localMinute >= 1440U) {
    return false;
  }
  if (state_.quietStartMinute < state_.quietEndMinute) {
    return localMinute >= state_.quietStartMinute &&
           localMinute < state_.quietEndMinute;
  }
  return localMinute >= state_.quietStartMinute ||
         localMinute < state_.quietEndMinute;
}

}  // namespace activities
}  // namespace kitsu868
