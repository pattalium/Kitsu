#pragma once

#include <stddef.h>
#include <stdint.h>

#include "companion_brain.h"

// Five tiny one-button activities plus the durable quality-of-life settings
// they share. The caller owns display, storage, button debouncing and dialogue
// catalogues. This module is allocation-free and independent of Arduino.
namespace kitsu868 {
namespace activities {

constexpr uint8_t kActivityCount = 5U;
constexpr uint8_t kStateSchemaVersion = 1U;

enum class ActivityKind : uint8_t {
  None = 0U,
  MorseSignal,
  StaticTuner,
  ReactionFlash,
  HoldSteady,
  PulseBreathing,
};

enum class ActivityPhase : uint8_t {
  Idle = 0U,
  Presenting,
  Playing,
  Result,
  Finished,
};

enum class InputResult : uint8_t {
  Ignored = 0U,
  Accepted,
  Completed,
  TooEarly,
  Invalid,
};

enum class QuickAction : uint8_t {
  Pet = 0U,
  Feed,
  Play,
  Listen,
  DailyGame,
  Expedition,
};

struct CompanionModifier {
  uint8_t speedPercent = 100U;
  uint8_t tolerancePercent = 100U;
  uint8_t rewardPercent = 100U;
};

struct DailyActivity {
  ActivityKind kind = ActivityKind::MorseSignal;
  uint8_t difficulty = 1U;
  uint32_t seed = 1U;
};

#pragma pack(push, 1)
struct ActivityState {
  uint32_t magic = UINT32_C(0x31544341);  // "ACT1" little-endian.
  uint16_t bytes = sizeof(ActivityState);
  uint8_t schemaVersion = kStateSchemaVersion;
  uint8_t kind = static_cast<uint8_t>(ActivityKind::None);
  uint8_t phase = static_cast<uint8_t>(ActivityPhase::Idle);
  uint8_t difficulty = 1U;
  uint8_t flags = 0U;
  uint8_t patternCount = 0U;
  uint8_t inputCount = 0U;
  uint8_t successCount = 0U;
  uint8_t pulseCount = 0U;
  uint16_t patternBits = 0U;
  uint16_t inputBits = 0U;
  uint16_t score = 0U;
  uint16_t maximumScore = 1000U;
  uint16_t target = 0U;
  uint16_t auxiliary = 0U;
  uint32_t seed = 1U;
  uint32_t dayId = 0U;
  uint32_t phaseStartedAt = 0U;
  uint32_t inputStartedAt = 0U;
  uint16_t bestScores[kActivityCount]{};
  uint32_t ghostSeeds[kActivityCount]{};
  uint8_t ghostDifficulties[kActivityCount]{};
  uint16_t lastDialogueId = 0U;
  uint16_t quietStartMinute = 1320U;
  uint16_t quietEndMinute = 420U;
  uint8_t quickAction = static_cast<uint8_t>(QuickAction::Pet);
  uint8_t quietHoursEnabled = 0U;
  uint8_t modifierSpeedPercent = 100U;
  uint8_t modifierTolerancePercent = 100U;
  uint8_t modifierRewardPercent = 100U;
  uint8_t reserved = 0U;
  uint32_t crc32 = 0U;
};
#pragma pack(pop)

static_assert(sizeof(ActivityState) <= 96U,
              "activity persistence must remain compact");

struct ActivityView {
  ActivityKind kind = ActivityKind::None;
  ActivityPhase phase = ActivityPhase::Idle;
  uint8_t difficulty = 0U;
  uint8_t progress = 0U;
  uint8_t total = 0U;
  uint8_t marker = 0U;
  uint8_t target = 0U;
  uint8_t cueOn = 0U;
  uint8_t resumed = 0U;
  uint16_t score = 0U;
  uint16_t maximumScore = 0U;
  uint16_t ghostScore = 0U;
  uint32_t remainingMs = 0U;
};

uint32_t activityStateCrc(const ActivityState& state);
bool validateActivityState(const ActivityState& state);
const char* activityName(ActivityKind kind);
CompanionModifier companionModifier(PersonalityKind personality,
                                    CompanionMood mood);
DailyActivity proceduralDailyActivity(uint32_t dayId,
                                      uint32_t companionFingerprint);

class ActivitySuite {
 public:
  ActivitySuite();
  void reset();
  bool restore(const ActivityState& state);
  ActivityState snapshot() const { return state_; }
  bool available() const { return available_; }

  bool start(ActivityKind kind, uint32_t nowMs, uint32_t seed,
             uint8_t difficulty, const CompanionModifier& modifier,
             uint32_t dayId = 0U);
  bool startDaily(uint32_t dayId, uint32_t companionFingerprint,
                  uint32_t nowMs, const CompanionModifier& modifier);
  bool startGhost(ActivityKind kind, uint32_t nowMs,
                  const CompanionModifier& modifier);
  void tick(uint32_t nowMs);
  InputResult tap(uint32_t nowMs);
  InputResult press(uint32_t nowMs);
  InputResult release(uint32_t nowMs);
  void cancel();

  // A reboot cannot prove elapsed millis. Resume preserves logical progress
  // but restarts the current timing window from the supplied new boot clock.
  bool resumeAfterCrash(uint32_t nowMs);
  ActivityView view(uint32_t nowMs) const;

  void rememberDialogue(uint16_t dialogueId);
  bool replayDialogue(uint16_t& dialogueId) const;
  bool setQuickAction(QuickAction action);
  QuickAction quickAction() const;
  bool setQuietHours(bool enabled, uint16_t startMinute,
                     uint16_t endMinute);
  bool quietAt(uint16_t localMinute) const;

 private:
  void refreshCrc();
  void finish(uint16_t score, uint32_t nowMs);
  uint8_t activityIndex(ActivityKind kind) const;
  uint16_t timingScore(uint32_t errorMs, uint32_t perfectMs,
                       uint32_t maximumMs) const;
  uint32_t elapsed(uint32_t nowMs) const;
  uint8_t tunerMarker(uint32_t nowMs) const;
  uint16_t pulseScore(uint32_t nowMs) const;

  ActivityState state_{};
  CompanionModifier modifier_{};
  bool available_ = true;
};

}  // namespace activities
}  // namespace kitsu868
