#pragma once

#include <stddef.h>
#include <stdint.h>

#include "companion_brain.h"

// Fixed-storage dialogue and micro-stories for the companion. This module is
// deliberately independent of Arduino, rendering, persistence, and transport.
// Callers own when a line is displayed and whether the compact state is saved.
namespace kitsu868 {
namespace dialogue {

constexpr uint8_t kActionCount = 8U;
constexpr uint8_t kActionRecentCapacity = 4U;
constexpr uint8_t kStoryCount = 6U;
constexpr uint8_t kStoryRecentCapacity = 3U;
constexpr uint8_t kStoryChoiceCount = 3U;
constexpr uint8_t kNoActiveStory = UINT8_MAX;

enum class Action : uint8_t {
  Pet = 0,
  Feed,
  Play,
  Listen,
  Sleep,
  Wake,
  Meet,
  Gift,
};

// Outcomes are explicit so a line never invents a success or radio reply.
enum class ActionOutcome : uint8_t {
  Success = 0,
  Failed,
  Busy,
  NoReply,
};

enum class LineFlavor : uint8_t {
  General = 0,
  Personality,
  LowEnergy,
  Bonded,
  Curious,
  Nearby,
  Outcome,
};

struct ActionContext {
  PersonalityKind personality = PersonalityKind::Gentle;
  CompanionMood mood = CompanionMood::Content;
  CompanionVitals vitals{};
  uint8_t bondLevel = 0U;
  ActionOutcome outcome = ActionOutcome::Success;
  bool nearby = false;
};

struct ActionLine {
  const char* line1 = "STILL HERE";
  const char* line2 = "WITH YOU";
  // Stable for the lifetime of this dialogue catalogue. Zero means fallback.
  uint16_t id = 0U;
  LineFlavor flavor = LineFlavor::General;
};

// A four-line ring is enough to prevent the repetitive one-line feel while
// keeping the state cheap to retain. The selector repairs invalid state before
// using it, so a corrupt optional persisted copy cannot escape array bounds.
struct ActionState {
  uint32_t selections = 0U;
  uint16_t recent[kActionRecentCapacity]{};
  uint8_t recentHead = 0U;
  uint8_t recentCount = 0U;
};

static_assert(sizeof(ActionState) <= 16U,
              "Action anti-repeat state unexpectedly grew");

void resetActionState(ActionState& state);
bool validateActionState(const ActionState& state);

// Reconstructs an authored line from the stable ID returned by
// selectActionLine(). This lets firmware persist/replay the last dialogue
// without storing pointers or text. Invalid and reserved IDs return false and
// leave out unchanged.
bool actionLineById(uint16_t id, ActionLine& out);

ActionLine selectActionLine(Action action, const ActionContext& context,
                            uint32_t companionFingerprint,
                            ActionState& state);

enum class StoryTrigger : uint8_t {
  QuietMoment = 0,
  ExpeditionReturn,
  NearbySignal,
};

enum class StoryChoice : uint8_t {
  First = 0,
  Second,
  Third,
};

// A tone is a small semantic outcome, not an item or inventory reward.
enum class StoryTone : uint8_t {
  Warm = 0,
  Curious,
  Brave,
  Playful,
  Calm,
};

struct StoryBeat {
  const char* line1 = "";
  const char* line2 = "";
  const char* choices[kStoryChoiceCount]{};
  uint8_t storyId = 0U;
  uint8_t scene = 0U;
  bool awaitsChoice = false;
};

struct StoryResolution {
  const char* line1 = "";
  const char* line2 = "";
  uint8_t storyId = 0U;
  StoryTone tone = StoryTone::Warm;
  int8_t affectionDelta = 0;
  int8_t energyDelta = 0;
  int8_t curiosityDelta = 0;
  bool personalityMatch = false;
};

// Story state is intentionally a semantic DTO. It contains no pointers and
// can be wrapped in the firmware's normal versioned/CRC-protected record.
struct StoryState {
  uint32_t starts = 0U;
  uint32_t completedMask = 0U;
  uint16_t completions = 0U;
  uint8_t activeStory = kNoActiveStory;
  uint8_t scene = 0U;
  uint8_t recent[kStoryRecentCapacity]{};
  uint8_t recentHead = 0U;
  uint8_t recentCount = 0U;
};

static_assert(sizeof(StoryState) <= 20U,
              "Micro-story state unexpectedly grew");

void resetStoryState(StoryState& state);
bool validateStoryState(const StoryState& state);

// Starts one of two authored stories for each trigger. A recently presented
// story is avoided whenever its sibling is available.
bool startStory(StoryTrigger trigger, PersonalityKind personality,
                uint32_t companionFingerprint, StoryState& state,
                StoryBeat& beat);

// Scene zero is the opening. advanceStory moves to the decision prompt.
bool currentStoryBeat(const StoryState& state, StoryBeat& beat);
bool advanceStory(StoryState& state, StoryBeat& beat);

// Resolving clears the active story, records completion, and returns bounded
// stat deltas for the caller to clamp/apply. The selected wording changes when
// the choice matches that companion personality's authored inclination.
bool resolveStory(StoryChoice choice, PersonalityKind personality,
                  StoryState& state, StoryResolution& resolution);

void cancelStory(StoryState& state);
bool storyCompleted(const StoryState& state, uint8_t storyId);

}  // namespace dialogue
}  // namespace kitsu868
