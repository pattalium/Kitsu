#include "expedition_core.h"

#include <stddef.h>

namespace kitsu868 {
namespace expedition {
namespace {

constexpr uint32_t kStateMagic = UINT32_C(0x3158454B);
constexpr uint8_t kFlagDueUnixValid = 1U << 0;
constexpr uint32_t kEligibleEncounterMask =
    (UINT32_C(1) << kCatalogCreatureCount) - 1U;

struct ReportRow {
  const char* headline;
  const char* detail;
};

// Four compact authored reports for each persisted PersonalityKind, in enum
// order. They fit the current two-line display and remain useful on the phone.
constexpr ReportRow kReports[kReportCount] = {
    {"QUIET PATH", "I KEPT IT SAFE"},
    {"WARM SIGNAL", "IT FELT FRIENDLY"},
    {"CLOUD WATCH", "I THOUGHT OF YOU"},
    {"SLOW RETURN", "HOME FELT CLOSE"},

    {"LONG WAY", "I TOOK IT"},
    {"STRONG SIGNAL", "I ANSWERED FIRST"},
    {"ROUGH STATIC", "DID NOT STOP ME"},
    {"HIGH GROUND", "WORTH THE CLIMB"},

    {"THREE NOTES", "STILL A MYSTERY"},
    {"ODD SIGNAL", "I FOLLOWED IT"},
    {"NEW PATTERN", "I REMEMBERED IT"},
    {"HIDDEN PATH", "I FOUND THE TURN"},

    {"ECHO CHASE", "IT NEARLY WON"},
    {"FAST DETOUR", "BEST KIND"},
    {"SIGNAL GAME", "I WON MAYBE"},
    {"WIND RACE", "REMATCH LATER"},

    {"SOFT HELLO", "SOMEONE ANSWERED"},
    {"QUIET CORNER", "I STAYED A WHILE"},
    {"DISTANT STEPS", "I LET THEM PASS"},
    {"SMALL SIGNAL", "I ANSWERED BACK"},

    {"FALSE TRAIL", "I MADE ONE TOO"},
    {"STOLEN ECHO", "IT IS MINE NOW"},
    {"SHORTCUT", "DO NOT ASK"},
    {"STATIC TRICK", "ALMOST GOT ME"},
};

constexpr CompanionMood kResultMoods[6] = {
    CompanionMood::Loved,
    CompanionMood::Proud,
    CompanionMood::Curious,
    CompanionMood::Playful,
    CompanionMood::Content,
    CompanionMood::Impish,
};

constexpr PersonalityAxis kResultAxes[6] = {
    PersonalityAxis::Warmth,
    PersonalityAxis::Boldness,
    PersonalityAxis::Curiosity,
    PersonalityAxis::Playfulness,
    PersonalityAxis::Warmth,
    PersonalityAxis::Playfulness,
};

uint32_t mix32(uint32_t value) {
  value ^= value >> 16U;
  value *= UINT32_C(0x7FEB352D);
  value ^= value >> 15U;
  value *= UINT32_C(0x846CA68B);
  value ^= value >> 16U;
  return value;
}

uint32_t stateCrc(const ExpeditionState& state) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&state);
  const size_t length = offsetof(ExpeditionState, crc32);
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

bool validDuration(Duration duration) {
  return static_cast<uint8_t>(duration) <
      static_cast<uint8_t>(Duration::Count);
}

bool validPhase(uint8_t value) {
  return value <= static_cast<uint8_t>(Phase::Ready);
}

bool validPersonality(PersonalityKind personality) {
  return static_cast<uint8_t>(personality) <=
      static_cast<uint8_t>(PersonalityKind::Impish);
}

bool validMood(uint8_t mood) {
  return mood <= static_cast<uint8_t>(CompanionMood::Awake);
}

bool validClock(const ClockSample& clock) {
  if (clock.bootId == 0U) return false;
  if (clock.unixValid != 0U &&
      clock.unixSeconds < kMinimumTrustedUnixSeconds) {
    return false;
  }
  return true;
}

bool safeDueUnix(uint64_t unixSeconds, uint32_t seconds,
                 uint64_t& output) {
  if (unixSeconds > UINT64_MAX - static_cast<uint64_t>(seconds)) {
    return false;
  }
  output = unixSeconds + static_cast<uint64_t>(seconds);
  return true;
}

uint8_t populationCount(uint32_t bits) {
  uint8_t count = 0U;
  while (bits != 0U) {
    count = static_cast<uint8_t>(count + (bits & 1U));
    bits >>= 1U;
  }
  return count;
}

uint8_t selectedSetBit(uint32_t bits, uint8_t ordinal) {
  for (uint8_t index = 0U; index < kCatalogCreatureCount; ++index) {
    if ((bits & (UINT32_C(1) << index)) != 0U) {
      if (ordinal == 0U) return index;
      --ordinal;
    }
  }
  return kNoEncounter;
}

uint8_t encounterChancePercent(Duration duration) {
  switch (duration) {
    case Duration::Short: return 10U;
    case Duration::Medium: return 25U;
    case Duration::Long: return 40U;
    case Duration::Count: break;
  }
  return 0U;
}

bool monotonicDelta(uint32_t previous, uint32_t current,
                    uint32_t maximumTrustedDelta, uint32_t& output) {
  uint32_t delta = 0U;
  if (current >= previous) {
    delta = current - previous;
  } else if (previous >= UINT32_C(0xF0000000) &&
             current <= UINT32_C(0x0FFFFFFF)) {
    delta = current + (UINT32_MAX - previous) + 1U;
  } else {
    return false;  // Looks like a reboot with a reused boot identifier.
  }
  if (delta > maximumTrustedDelta) return false;
  output = delta;
  return true;
}

void canonicalIdle(ExpeditionState& state, uint32_t sequence) {
  state = ExpeditionState{};
  state.sequence = sequence;
}

}  // namespace

uint32_t durationSeconds(Duration duration) {
  switch (duration) {
    case Duration::Short: return 15U * 60U;
    case Duration::Medium: return 2U * 60U * 60U;
    case Duration::Long: return 8U * 60U * 60U;
    case Duration::Count: break;
  }
  return 0U;
}

const char* durationLabel(Duration duration) {
  switch (duration) {
    case Duration::Short: return "SHORT";
    case Duration::Medium: return "MEDIUM";
    case Duration::Long: return "LONG";
    case Duration::Count: break;
  }
  return "UNKNOWN";
}

const char* phaseLabel(Phase phase) {
  switch (phase) {
    case Phase::Idle: return "IDLE";
    case Phase::Traveling: return "SCOUTING";
    case Phase::Ready: return "RETURNED";
  }
  return "UNKNOWN";
}

bool reportForIndex(uint8_t reportIndex, ExpeditionReport& report) {
  if (reportIndex >= kReportCount) return false;
  report.headline = kReports[reportIndex].headline;
  report.detail = kReports[reportIndex].detail;
  return true;
}

bool validateExpeditionState(const ExpeditionState& state) {
  if (state.magic != kStateMagic ||
      state.schemaVersion != kExpeditionStateSchemaVersion ||
      state.bytes != kExpeditionStateBytes || !validPhase(state.phase) ||
      state.crc32 != stateCrc(state) || (state.flags & ~kFlagDueUnixValid) != 0U) {
    return false;
  }
  for (size_t index = 0U; index < sizeof(state.reserved); ++index) {
    if (state.reserved[index] != 0U) return false;
  }

  const Phase phase = static_cast<Phase>(state.phase);
  if (phase == Phase::Idle) {
    return state.expeditionId == 0U && state.durationSeconds == 0U &&
        state.remainingSeconds == 0U && state.dueUnixSeconds == 0U &&
        state.checkpointBootId == 0U &&
        state.checkpointMonotonicMillis == 0U &&
        state.subsecondMillis == 0U && state.reportIndex == kNoReport &&
        state.affectionDelta == 0 && state.moodStrength == 0U &&
        state.personalityAxis == static_cast<uint8_t>(PersonalityAxis::None) &&
        state.personalityDelta == 0 &&
        state.encounterCatalogIndex == kNoEncounter &&
        state.memoryReportIndex == kNoReport && state.flags == 0U;
  }

  const Duration duration = static_cast<Duration>(state.duration);
  if (!validDuration(duration) || state.sequence == 0U ||
      state.expeditionId == 0U ||
      state.durationSeconds != expedition::durationSeconds(duration) ||
      state.remainingSeconds > state.durationSeconds ||
      state.checkpointBootId == 0U || state.subsecondMillis >= 1000U ||
      state.reportIndex >= kReportCount || state.affectionDelta < 1 ||
      state.affectionDelta > 3 || !validMood(state.mood) ||
      state.moodStrength < 1U || state.moodStrength > 3U ||
      state.personalityAxis <= static_cast<uint8_t>(PersonalityAxis::None) ||
      state.personalityAxis >= static_cast<uint8_t>(PersonalityAxis::Count) ||
      state.personalityDelta < 1 || state.personalityDelta > 2 ||
      (state.encounterCatalogIndex != kNoEncounter &&
       state.encounterCatalogIndex >= kCatalogCreatureCount) ||
      state.memoryReportIndex != state.reportIndex) {
    return false;
  }
  if (phase == Phase::Traveling && state.remainingSeconds == 0U) return false;
  if (phase == Phase::Ready &&
      (state.remainingSeconds != 0U || state.subsecondMillis != 0U)) {
    return false;
  }
  const bool dueValid = (state.flags & kFlagDueUnixValid) != 0U;
  if (dueValid != (state.dueUnixSeconds >= kMinimumTrustedUnixSeconds)) {
    return false;
  }
  return true;
}

ExpeditionCore::ExpeditionCore() {
  reset();
}

void ExpeditionCore::refreshCrc() {
  state_.crc32 = stateCrc(state_);
}

void ExpeditionCore::reset() {
  canonicalIdle(state_, 0U);
  refreshCrc();
}

RestoreStatus ExpeditionCore::restore(const ExpeditionState& state) {
  if (state.magic != kStateMagic) return RestoreStatus::BadMagic;
  if (state.schemaVersion != kExpeditionStateSchemaVersion ||
      state.bytes != kExpeditionStateBytes) {
    return RestoreStatus::UnsupportedSchema;
  }
  if (!validateExpeditionState(state)) return RestoreStatus::InvalidState;
  state_ = state;
  return RestoreStatus::Ok;
}

StartStatus ExpeditionCore::start(Duration duration,
                                  const StartContext& context,
                                  const ClockSample& now,
                                  uint32_t entropy) {
  if (!validateExpeditionState(state_)) return StartStatus::InvalidState;
  if (static_cast<Phase>(state_.phase) != Phase::Idle) {
    return StartStatus::Busy;
  }
  if (!validDuration(duration)) return StartStatus::InvalidDuration;
  if (!validPersonality(context.personality) ||
      !validMood(static_cast<uint8_t>(context.mood)) ||
      context.affection > 100U ||
      (context.eligibleEncounterMask & ~kEligibleEncounterMask) != 0U) {
    return StartStatus::InvalidContext;
  }
  if (!validClock(now)) return StartStatus::InvalidClock;
  if (state_.sequence == UINT32_MAX) return StartStatus::SequenceExhausted;

  const uint32_t nextSequence = state_.sequence + 1U;
  const uint32_t seconds = expedition::durationSeconds(duration);
  uint64_t due = 0U;
  if (now.unixValid != 0U &&
      !safeDueUnix(now.unixSeconds, seconds, due)) {
    return StartStatus::InvalidClock;
  }

  ExpeditionState next{};
  next.phase = static_cast<uint8_t>(Phase::Traveling);
  next.duration = static_cast<uint8_t>(duration);
  next.sequence = nextSequence;
  next.durationSeconds = seconds;
  next.remainingSeconds = seconds;
  next.dueUnixSeconds = due;
  next.checkpointBootId = now.bootId;
  next.checkpointMonotonicMillis = now.monotonicMillis;
  if (now.unixValid != 0U) next.flags |= kFlagDueUnixValid;

  uint32_t seed = entropy ^ context.companionFingerprint ^
      (nextSequence * UINT32_C(0x9E3779B9));
  seed ^= static_cast<uint32_t>(static_cast<uint8_t>(duration)) << 28U;
  seed ^= static_cast<uint32_t>(static_cast<uint8_t>(context.personality))
      << 20U;
  seed ^= static_cast<uint32_t>(static_cast<uint8_t>(context.mood)) << 12U;
  seed ^= static_cast<uint32_t>(context.affection) << 4U;
  seed = mix32(seed);

  next.expeditionId = mix32(seed ^ UINT32_C(0xD1B54A35));
  if (next.expeditionId == 0U) next.expeditionId = 1U;
  const uint8_t personality = static_cast<uint8_t>(context.personality);
  next.reportIndex = static_cast<uint8_t>(
      personality * 4U + (mix32(seed ^ UINT32_C(0x94D049BB)) & 3U));
  next.affectionDelta = static_cast<int8_t>(
      static_cast<uint8_t>(duration) + 1U);
  next.mood = static_cast<uint8_t>(kResultMoods[personality]);
  next.moodStrength = static_cast<uint8_t>(
      static_cast<uint8_t>(duration) + 1U);
  next.personalityAxis = static_cast<uint8_t>(kResultAxes[personality]);
  next.personalityDelta = duration == Duration::Long ? 2 : 1;
  next.memoryReportIndex = next.reportIndex;

  const uint32_t encounterSeed = mix32(seed ^ UINT32_C(0xC2B2AE35));
  const uint32_t eligible =
      context.eligibleEncounterMask & kEligibleEncounterMask;
  if (eligible != 0U && encounterSeed % 100U <
      encounterChancePercent(duration)) {
    const uint8_t count = populationCount(eligible);
    const uint8_t ordinal = static_cast<uint8_t>(
        mix32(encounterSeed ^ UINT32_C(0x27D4EB2F)) % count);
    next.encounterCatalogIndex = selectedSetBit(eligible, ordinal);
  }

  next.crc32 = stateCrc(next);
  state_ = next;
  return StartStatus::Started;
}

PollStatus ExpeditionCore::poll(const ClockSample& now) {
  if (!validateExpeditionState(state_)) return PollStatus::InvalidState;
  if (!validClock(now)) return PollStatus::InvalidClock;
  if (static_cast<Phase>(state_.phase) != Phase::Traveling) {
    return PollStatus::NoChange;
  }

  bool changed = false;
  uint32_t candidateRemaining = state_.remainingSeconds;
  uint16_t candidateSubsecond = state_.subsecondMillis;

  if (now.bootId == state_.checkpointBootId) {
    uint32_t elapsedMillis = 0U;
    const uint32_t maximumTrustedDelta =
        state_.durationSeconds * 1000U + 60000U;
    if (monotonicDelta(state_.checkpointMonotonicMillis,
                       now.monotonicMillis, maximumTrustedDelta,
                       elapsedMillis)) {
      if (elapsedMillis != 0U) {
        const uint64_t totalMillis =
            static_cast<uint64_t>(elapsedMillis) + state_.subsecondMillis;
        const uint32_t elapsedSeconds = static_cast<uint32_t>(
            totalMillis / 1000U);
        candidateSubsecond = static_cast<uint16_t>(totalMillis % 1000U);
        candidateRemaining = elapsedSeconds >= state_.remainingSeconds
            ? 0U
            : state_.remainingSeconds - elapsedSeconds;
        state_.checkpointMonotonicMillis = now.monotonicMillis;
        changed = true;
      }
    } else {
      // A reset-looking value with an unchanged boot identifier is never used
      // as elapsed time. Re-anchor conservatively instead of fast-forwarding.
      state_.checkpointMonotonicMillis = now.monotonicMillis;
      candidateSubsecond = 0U;
      changed = true;
    }
  } else {
    // Monotonic clocks cannot prove downtime across boots. A trusted due time
    // below may account for it; otherwise the journey safely pauses here.
    state_.checkpointBootId = now.bootId;
    state_.checkpointMonotonicMillis = now.monotonicMillis;
    candidateSubsecond = 0U;
    changed = true;
  }

  if ((state_.flags & kFlagDueUnixValid) != 0U && now.unixValid != 0U) {
    uint32_t wallRemaining = 0U;
    if (now.unixSeconds < state_.dueUnixSeconds) {
      const uint64_t difference = state_.dueUnixSeconds - now.unixSeconds;
      wallRemaining = difference > UINT32_MAX
          ? UINT32_MAX
          : static_cast<uint32_t>(difference);
    }
    // Wall and monotonic values are independent estimates from the same
    // starting point. Take the more advanced one; never add them together.
    if (wallRemaining < candidateRemaining) {
      candidateRemaining = wallRemaining;
      candidateSubsecond = 0U;
      changed = true;
    }
  } else if ((state_.flags & kFlagDueUnixValid) == 0U &&
             now.unixValid != 0U) {
    uint64_t due = 0U;
    if (!safeDueUnix(now.unixSeconds, candidateRemaining, due)) {
      return PollStatus::InvalidClock;
    }
    state_.dueUnixSeconds = due;
    state_.flags |= kFlagDueUnixValid;
    changed = true;
  }

  if (candidateRemaining != state_.remainingSeconds ||
      candidateSubsecond != state_.subsecondMillis) {
    state_.remainingSeconds = candidateRemaining;
    state_.subsecondMillis = candidateSubsecond;
    changed = true;
  }

  if (state_.remainingSeconds == 0U) {
    state_.phase = static_cast<uint8_t>(Phase::Ready);
    state_.subsecondMillis = 0U;
    refreshCrc();
    return PollStatus::BecameReady;
  }
  if (!changed) return PollStatus::NoChange;
  refreshCrc();
  return PollStatus::Progressed;
}

ExpeditionView ExpeditionCore::view() const {
  ExpeditionView output{};
  if (!validateExpeditionState(state_)) return output;
  output.phase = static_cast<Phase>(state_.phase);
  if (output.phase == Phase::Idle) return output;
  output.duration = static_cast<Duration>(state_.duration);
  output.expeditionId = state_.expeditionId;
  output.totalSeconds = state_.durationSeconds;
  output.remainingSeconds = state_.remainingSeconds;
  const uint64_t elapsed =
      static_cast<uint64_t>(state_.durationSeconds - state_.remainingSeconds);
  output.progressPercent = static_cast<uint8_t>(
      (elapsed * 100U) / state_.durationSeconds);
  if (output.phase == Phase::Ready) output.reportIndex = state_.reportIndex;
  return output;
}

bool ExpeditionCore::completion(CompletionHooks& hooks) const {
  if (!validateExpeditionState(state_) ||
      static_cast<Phase>(state_.phase) != Phase::Ready) {
    return false;
  }
  hooks.expeditionId = state_.expeditionId;
  hooks.affectionDelta = state_.affectionDelta;
  hooks.mood = static_cast<CompanionMood>(state_.mood);
  hooks.moodStrength = state_.moodStrength;
  hooks.personalityAxis =
      static_cast<PersonalityAxis>(state_.personalityAxis);
  hooks.personalityDelta = state_.personalityDelta;
  hooks.encounterCatalogIndex = state_.encounterCatalogIndex;
  hooks.memoryReportIndex = state_.memoryReportIndex;
  return true;
}

AcknowledgeStatus ExpeditionCore::acknowledge(uint32_t expeditionId) {
  if (!validateExpeditionState(state_)) {
    return AcknowledgeStatus::InvalidState;
  }
  if (static_cast<Phase>(state_.phase) != Phase::Ready) {
    return AcknowledgeStatus::NotReady;
  }
  if (state_.expeditionId != expeditionId) {
    return AcknowledgeStatus::WrongExpedition;
  }
  const uint32_t sequence = state_.sequence;
  canonicalIdle(state_, sequence);
  refreshCrc();
  return AcknowledgeStatus::Acknowledged;
}

}  // namespace expedition
}  // namespace kitsu868
