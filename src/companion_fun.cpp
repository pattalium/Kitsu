#include "companion_fun.h"

namespace kitsu868 {
namespace fun {
namespace {

constexpr uint8_t kPersonalityCount = 6U;
constexpr uint8_t kTriggerCount = 7U;
constexpr uint32_t kCatalogMask = (UINT32_C(1) << kCatalogCreatureCount) - 1U;

struct MomentRow {
  const char* line1;
  const char* line2;
  Reaction reaction;
};

// Rows are trigger-major, then Gentle/Bold/Curious/Playful/Shy/Impish.
constexpr MomentRow kMoments[kTriggerCount][kPersonalityCount] = {
    {
        {"SOFT", "AND SAFE", Reaction::Pet},
        {"THAT TICKLED", "AGAIN", Reaction::Play},
        {"WHAT WAS", "THAT", Reaction::Surprise},
        {"MORE PETS", "PLEASE", Reaction::Play},
        {"...THANK", "YOU", Reaction::Blink},
        {"GOT YOUR", "HAND", Reaction::Surprise},
    },
    {
        {"A WARM", "LITTLE MEAL", Reaction::Feed},
        {"FULL POWER", "NOW", Reaction::Evolve},
        {"NEW FLAVOR", "LOGGED", Reaction::Feed},
        {"SNACK THEN", "ZOOM", Reaction::Play},
        {"SMALL BITES", "ARE NICE", Reaction::Feed},
        {"I SAVED", "ONE CRUMB", Reaction::Blink},
    },
    {
        {"GOOD GAME", "TOGETHER", Reaction::Play},
        {"ONE MORE", "ROUND", Reaction::Evolve},
        {"NEW RULES", "NEXT TIME", Reaction::Blink},
        {"BEST DAY", "EVER", Reaction::Play},
        {"I HAD", "FUN", Reaction::Pet},
        {"I LET YOU", "WIN", Reaction::Surprise},
    },
    {
        {"GOOD", "MORNING", Reaction::Wake},
        {"UP AND", "READY", Reaction::Evolve},
        {"DREAM DATA", "SORTED", Reaction::Blink},
        {"AWAKE", "LET US PLAY", Reaction::Play},
        {"FIVE MORE", "SECONDS", Reaction::Tired},
        {"I WAS NOT", "ASLEEP", Reaction::Surprise},
    },
    {
        {"PERFECT", "TOGETHER", Reaction::Pet},
        {"FLAWLESS", "VICTORY", Reaction::Evolve},
        {"PATTERN", "MASTERED", Reaction::Blink},
        {"PERFECT", "AGAIN AGAIN", Reaction::Play},
        {"I DID IT", "QUIETLY", Reaction::Pet},
        {"PURE SKILL", "PROBABLY", Reaction::Surprise},
    },
    {
        {"A NEW", "FRIEND", Reaction::Meet},
        {"SIGNAL", "CONQUERED", Reaction::Evolve},
        {"WHO IS", "THAT", Reaction::Surprise},
        {"NEW FRIEND", "NEW GAME", Reaction::Play},
        {"HELLO", "FROM HERE", Reaction::Blink},
        {"I FOUND", "THEM FIRST", Reaction::Meet},
    },
    {
        {"JUST", "CHECKING IN", Reaction::Blink},
        {"STILL", "STANDING TALL", Reaction::Evolve},
        {"A TINY", "MYSTERY", Reaction::Surprise},
        {"SUDDEN", "ZOOMIES", Reaction::Play},
        {"I AM HERE", "WITH YOU", Reaction::Pet},
        {"NOTHING", "SUSPICIOUS", Reaction::Surprise},
    },
};

constexpr const char* kDreamLines[kDreamCount][2] = {
    {"CHASED A", "MOON SIGNAL"},
    {"FOUND A", "WARM STAR"},
    {"RAN THROUGH", "SOFT STATIC"},
    {"MET A", "TINY COMET"},
    {"HEARD THE", "NIGHT HUM"},
    {"SHARED A", "CLOUD SNACK"},
    {"FOLLOWED", "SILVER TRACKS"},
    {"SLEPT IN", "A RADIO NEST"},
    {"PLAYED WITH", "ECHOES"},
    {"FOUND YOUR", "FOOTSTEPS"},
    {"COUNTED", "FAR LIGHTS"},
    {"WOKE INSIDE", "A GOOD DREAM"},
};

uint8_t personalityIndex(PersonalityKind personality) {
  const uint8_t value = static_cast<uint8_t>(personality);
  return value < kPersonalityCount ? value : 0U;
}

uint8_t saturatingIncrement(uint8_t value, uint8_t ceiling) {
  return value < ceiling ? static_cast<uint8_t>(value + 1U) : ceiling;
}

}  // namespace

Moment personalityMoment(PersonalityKind personality, MomentTrigger trigger,
                         uint32_t entropy) {
  uint8_t triggerIndex = static_cast<uint8_t>(trigger);
  if (triggerIndex >= kTriggerCount) triggerIndex = 0U;
  uint8_t person = personalityIndex(personality);
  // Rare ambient moments occasionally borrow a neighboring personality's
  // wording, keeping them surprising while ordinary care remains consistent.
  if (trigger == MomentTrigger::RareAmbient && (entropy & 3U) == 3U) {
    person = static_cast<uint8_t>((person + 1U) % kPersonalityCount);
  }
  const MomentRow& row = kMoments[triggerIndex][person];
  Moment moment;
  moment.line1 = row.line1;
  moment.line2 = row.line2;
  moment.reaction = row.reaction;
  return moment;
}

Dream selectDream(PersonalityKind personality, uint32_t companionFingerprint,
                  uint16_t completedDreams) {
  const uint32_t mixed = companionFingerprint ^
      (static_cast<uint32_t>(completedDreams) * UINT32_C(0x9E3779B9)) ^
      (static_cast<uint32_t>(personalityIndex(personality)) *
       UINT32_C(0x85EBCA6B));
  const uint8_t index = static_cast<uint8_t>(mixed % kDreamCount);
  Dream dream;
  dream.line1 = kDreamLines[index][0];
  dream.line2 = kDreamLines[index][1];
  dream.index = index;
  return dream;
}

void resetSessionChallenges(SessionChallenges& challenges) {
  challenges = SessionChallenges{};
}

uint8_t challengeTarget(SessionActivity activity) {
  switch (activity) {
    case SessionActivity::Care: return 2U;
    case SessionActivity::Game: return 1U;
    case SessionActivity::Signal: return 1U;
  }
  return 1U;
}

const char* challengeName(SessionActivity activity) {
  switch (activity) {
    case SessionActivity::Care: return "CARE";
    case SessionActivity::Game: return "GAME";
    case SessionActivity::Signal: return "SIGNAL";
  }
  return "GOAL";
}

uint8_t challengeProgress(const SessionChallenges& challenges,
                          SessionActivity activity) {
  switch (activity) {
    case SessionActivity::Care: return challenges.care;
    case SessionActivity::Game: return challenges.games;
    case SessionActivity::Signal: return challenges.signals;
  }
  return 0U;
}

ChallengeUpdate recordSessionActivity(SessionChallenges& challenges,
                                      SessionActivity activity) {
  ChallengeUpdate update{};
  const uint8_t index = static_cast<uint8_t>(activity);
  if (index >= kSessionChallengeCount) return update;
  const uint8_t bit = static_cast<uint8_t>(1U << index);
  if (activity == SessionActivity::Care) {
    challenges.care = saturatingIncrement(
        challenges.care, challengeTarget(activity));
  } else if (activity == SessionActivity::Game) {
    challenges.games = saturatingIncrement(
        challenges.games, challengeTarget(activity));
  } else {
    challenges.signals = saturatingIncrement(
        challenges.signals, challengeTarget(activity));
  }
  if ((challenges.completedMask & bit) == 0U &&
      challengeProgress(challenges, activity) >= challengeTarget(activity)) {
    challenges.completedMask = static_cast<uint8_t>(
        challenges.completedMask | bit);
    update.newlyCompletedMask = bit;
  }
  constexpr uint8_t all = (1U << kSessionChallengeCount) - 1U;
  if (challenges.completedMask == all && challenges.rewardClaimed == 0U) {
    challenges.rewardClaimed = 1U;
    update.allCompletedNow = 1U;
  }
  return update;
}

bool validateDiscoveryState(const DiscoveryState& state) {
  if (state.schemaVersion != kDiscoveryStateSchemaVersion ||
      state.reserved[0] != 0U || state.reserved[1] != 0U ||
      state.reserved[2] != 0U || (state.seenMask & ~kCatalogMask) != 0U ||
      state.dreamHead >= kDreamHistoryCapacity ||
      state.dreamHistoryCount > kDreamHistoryCapacity) {
    return false;
  }
  const uint8_t expectedDreamHistoryCount =
      state.completedDreams < kDreamHistoryCapacity
      ? static_cast<uint8_t>(state.completedDreams)
      : kDreamHistoryCapacity;
  if (state.dreamHistoryCount != expectedDreamHistoryCount ||
      (state.dreamHistoryCount < kDreamHistoryCapacity &&
       state.dreamHead != state.dreamHistoryCount)) {
    return false;
  }
  for (uint8_t index = 0U; index < kCatalogCreatureCount; ++index) {
    const bool seen = (state.seenMask & (UINT32_C(1) << index)) != 0U;
    if (seen != (state.encounterCounts[index] != 0U) ||
        (!seen && state.lastSources[index] != 0U)) {
      return false;
    }
  }
  for (uint8_t index = 0U; index < state.dreamHistoryCount; ++index) {
    if (state.dreamHistory[index] >= kDreamCount) return false;
  }
  return true;
}

void resetDiscoveryState(DiscoveryState& state) {
  state = DiscoveryState{};
}

bool recordCreatureEncounter(DiscoveryState& state, uint8_t catalogIndex,
                             uint8_t source) {
  if (!validateDiscoveryState(state) ||
      catalogIndex >= kCatalogCreatureCount) {
    return false;
  }
  state.seenMask |= UINT32_C(1) << catalogIndex;
  if (state.encounterCounts[catalogIndex] != UINT16_MAX) {
    ++state.encounterCounts[catalogIndex];
  }
  state.lastSources[catalogIndex] = source;
  return true;
}

bool creatureSeen(const DiscoveryState& state, uint8_t catalogIndex) {
  return validateDiscoveryState(state) &&
      catalogIndex < kCatalogCreatureCount &&
      (state.seenMask & (UINT32_C(1) << catalogIndex)) != 0U;
}

uint8_t seenCreatureCount(const DiscoveryState& state) {
  if (!validateDiscoveryState(state)) return 0U;
  uint32_t bits = state.seenMask;
  uint8_t count = 0U;
  while (bits != 0U) {
    count = static_cast<uint8_t>(count + (bits & 1U));
    bits >>= 1U;
  }
  return count;
}

uint16_t creatureEncounterCount(const DiscoveryState& state,
                                uint8_t catalogIndex) {
  return validateDiscoveryState(state) &&
      catalogIndex < kCatalogCreatureCount
      ? state.encounterCounts[catalogIndex]
      : 0U;
}

bool recordDream(DiscoveryState& state, uint8_t dreamIndex) {
  if (!validateDiscoveryState(state) || dreamIndex >= kDreamCount) {
    return false;
  }
  if (state.completedDreams != UINT16_MAX) ++state.completedDreams;
  state.dreamHistory[state.dreamHead] = dreamIndex;
  state.dreamHead = static_cast<uint8_t>(
      (state.dreamHead + 1U) % kDreamHistoryCapacity);
  if (state.dreamHistoryCount < kDreamHistoryCapacity) {
    ++state.dreamHistoryCount;
  }
  return true;
}

bool recentDream(const DiscoveryState& state, uint8_t newestIndex,
                 uint8_t& dreamIndex) {
  if (!validateDiscoveryState(state) ||
      newestIndex >= state.dreamHistoryCount) {
    return false;
  }
  const uint8_t index = static_cast<uint8_t>(
      (state.dreamHead + kDreamHistoryCapacity - 1U - newestIndex) %
      kDreamHistoryCapacity);
  dreamIndex = state.dreamHistory[index];
  return true;
}

void recordRareReaction(DiscoveryState& state) {
  if (validateDiscoveryState(state) && state.rareReactions != UINT16_MAX) {
    ++state.rareReactions;
  }
}

}  // namespace fun
}  // namespace kitsu868
