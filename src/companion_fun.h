#pragma once

#include <stddef.h>
#include <stdint.h>

#include "companion_brain.h"

// Small, deterministic fun systems that do not depend on Arduino, storage,
// the display, radio, or a particular companion pack. Firmware owns rendering
// and persistence; host tests exercise the same decisions.
namespace kitsu868 {
namespace fun {

constexpr uint8_t kCatalogCreatureCount = 21U;
constexpr uint8_t kSessionChallengeCount = 3U;
constexpr uint8_t kDreamCount = 12U;
constexpr uint8_t kDreamHistoryCapacity = 8U;
constexpr uint8_t kDiscoveryStateSchemaVersion = 1U;

enum class MomentTrigger : uint8_t {
  Pet = 0,
  Feed,
  Play,
  Wake,
  PerfectGame,
  Encounter,
  RareAmbient,
};

enum class Reaction : uint8_t {
  Blink = 0,
  Pet,
  Surprise,
  Play,
  Tired,
  Feed,
  Wake,
  Meet,
  Evolve,
};

struct Moment {
  const char* line1 = "HELLO";
  const char* line2 = "THERE";
  Reaction reaction = Reaction::Blink;
};

Moment personalityMoment(PersonalityKind personality, MomentTrigger trigger,
                         uint32_t entropy = 0U);

struct Dream {
  const char* line1 = "QUIET";
  const char* line2 = "SIGNALS";
  uint8_t index = 0U;
};

Dream selectDream(PersonalityKind personality, uint32_t companionFingerprint,
                  uint16_t completedDreams);

enum class SessionActivity : uint8_t {
  Care = 0,
  Game,
  Signal,
};

struct SessionChallenges {
  uint8_t care = 0U;
  uint8_t games = 0U;
  uint8_t signals = 0U;
  uint8_t completedMask = 0U;
  uint8_t rewardClaimed = 0U;
};

struct ChallengeUpdate {
  uint8_t newlyCompletedMask = 0U;
  uint8_t allCompletedNow = 0U;
};

void resetSessionChallenges(SessionChallenges& challenges);
ChallengeUpdate recordSessionActivity(SessionChallenges& challenges,
                                      SessionActivity activity);
uint8_t challengeProgress(const SessionChallenges& challenges,
                          SessionActivity activity);
uint8_t challengeTarget(SessionActivity activity);
const char* challengeName(SessionActivity activity);

// Semantic DTO. Firmware wraps it in its normal CRC-protected Preferences
// record instead of relying on this compiler layout as an on-flash format.
struct DiscoveryState {
  uint8_t schemaVersion = kDiscoveryStateSchemaVersion;
  uint8_t reserved[3]{};
  uint32_t seenMask = 0U;
  uint16_t encounterCounts[kCatalogCreatureCount]{};
  uint8_t lastSources[kCatalogCreatureCount]{};
  uint16_t completedDreams = 0U;
  uint16_t rareReactions = 0U;
  uint8_t dreamHistory[kDreamHistoryCapacity]{};
  uint8_t dreamHead = 0U;
  uint8_t dreamHistoryCount = 0U;
};

bool validateDiscoveryState(const DiscoveryState& state);
void resetDiscoveryState(DiscoveryState& state);
bool recordCreatureEncounter(DiscoveryState& state, uint8_t catalogIndex,
                             uint8_t source);
bool creatureSeen(const DiscoveryState& state, uint8_t catalogIndex);
uint8_t seenCreatureCount(const DiscoveryState& state);
uint16_t creatureEncounterCount(const DiscoveryState& state,
                                uint8_t catalogIndex);
bool recordDream(DiscoveryState& state, uint8_t dreamIndex);
bool recentDream(const DiscoveryState& state, uint8_t newestIndex,
                 uint8_t& dreamIndex);
void recordRareReaction(DiscoveryState& state);

}  // namespace fun
}  // namespace kitsu868
