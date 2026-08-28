#pragma once

#include <stddef.h>
#include <stdint.h>

#include "companion_brain.h"

// A compact, storage-free expedition decision engine. The caller owns the
// Preferences transaction, clock source, display, and all changes to the
// companion/Field Guide. In particular, start() fixes the result immediately;
// callers must persist snapshot() before presenting the expedition as started.
namespace kitsu868 {
namespace expedition {

constexpr uint8_t kExpeditionStateSchemaVersion = 1U;
constexpr uint8_t kExpeditionStateBytes = 64U;
constexpr uint8_t kReportCount = 24U;
constexpr uint8_t kCatalogCreatureCount = 21U;
constexpr uint8_t kNoReport = UINT8_MAX;
constexpr uint8_t kNoEncounter = UINT8_MAX;
constexpr uint64_t kMinimumTrustedUnixSeconds = UINT64_C(1577836800);

enum class Duration : uint8_t {
  Short = 0,
  Medium,
  Long,
  Count,
};

enum class Phase : uint8_t {
  Idle = 0,
  Traveling,
  Ready,
};

enum class PersonalityAxis : uint8_t {
  None = 0,
  Warmth,
  Playfulness,
  Boldness,
  Curiosity,
  Count,
};

// bootId must be non-zero and must change after every reboot. A persisted boot
// counter or a collision-resistant boot nonce both work. unixValid must only
// be set after the platform considers its wall clock trustworthy.
struct ClockSample {
  uint32_t bootId = 0U;
  uint32_t monotonicMillis = 0U;
  uint64_t unixSeconds = 0U;
  uint8_t unixValid = 0U;
};

struct StartContext {
  PersonalityKind personality = PersonalityKind::Gentle;
  CompanionMood mood = CompanionMood::Content;
  uint8_t affection = 0U;
  uint32_t companionFingerprint = 0U;

  // Only candidates already permitted by the caller's encounter/unlock rules
  // belong in this 21-bit mask. The core may suggest one; it never records it.
  uint32_t eligibleEncounterMask = 0U;
};

// This is an exact fixed-size persistence record with an internal CRC. Store
// the complete record in the firmware's normal authenticated Preferences
// container. Numeric enum values and field order form schema version 1.
#pragma pack(push, 1)
struct ExpeditionState {
  uint32_t magic = UINT32_C(0x3158454B);  // "KEX1" little-endian.
  uint8_t schemaVersion = kExpeditionStateSchemaVersion;
  uint8_t bytes = kExpeditionStateBytes;
  uint8_t phase = static_cast<uint8_t>(Phase::Idle);
  uint8_t duration = static_cast<uint8_t>(Duration::Short);
  uint32_t sequence = 0U;
  uint32_t expeditionId = 0U;
  uint32_t durationSeconds = 0U;
  uint32_t remainingSeconds = 0U;
  uint64_t dueUnixSeconds = 0U;
  uint32_t checkpointBootId = 0U;
  uint32_t checkpointMonotonicMillis = 0U;
  uint16_t subsecondMillis = 0U;
  uint8_t reportIndex = kNoReport;
  int8_t affectionDelta = 0;
  uint8_t mood = static_cast<uint8_t>(CompanionMood::Content);
  uint8_t moodStrength = 0U;
  uint8_t personalityAxis = static_cast<uint8_t>(PersonalityAxis::None);
  int8_t personalityDelta = 0;
  uint8_t encounterCatalogIndex = kNoEncounter;
  uint8_t memoryReportIndex = kNoReport;
  uint8_t flags = 0U;
  uint8_t reserved[9]{};
  uint32_t crc32 = 0U;
};
#pragma pack(pop)

static_assert(sizeof(ExpeditionState) == kExpeditionStateBytes,
              "Expedition persistence schema changed unexpectedly");

struct ExpeditionView {
  Phase phase = Phase::Idle;
  Duration duration = Duration::Short;
  uint32_t expeditionId = 0U;
  uint32_t totalSeconds = 0U;
  uint32_t remainingSeconds = 0U;
  uint8_t progressPercent = 0U;

  // Hidden while traveling so UI code cannot accidentally reveal the fixed
  // result early. It becomes available when phase is Ready.
  uint8_t reportIndex = kNoReport;
};

struct ExpeditionReport {
  const char* headline = "NO JOURNEY";
  const char* detail = "NOTHING TO REPORT";
};

// A completion is a list of requests, not mutations. Apply them through the
// owning systems, keyed by expeditionId for idempotence, then acknowledge().
struct CompletionHooks {
  uint32_t expeditionId = 0U;
  int8_t affectionDelta = 0;
  CompanionMood mood = CompanionMood::Content;
  uint8_t moodStrength = 0U;
  PersonalityAxis personalityAxis = PersonalityAxis::None;
  int8_t personalityDelta = 0;
  uint8_t encounterCatalogIndex = kNoEncounter;
  uint8_t memoryReportIndex = kNoReport;

  bool hasEncounter() const {
    return encounterCatalogIndex != kNoEncounter;
  }
};

enum class RestoreStatus : uint8_t {
  Ok = 0,
  BadMagic,
  UnsupportedSchema,
  InvalidState,
};

enum class StartStatus : uint8_t {
  Started = 0,
  Busy,
  InvalidDuration,
  InvalidContext,
  InvalidClock,
  InvalidState,
  SequenceExhausted,
};

enum class PollStatus : uint8_t {
  NoChange = 0,
  Progressed,
  BecameReady,
  InvalidClock,
  InvalidState,
};

enum class AcknowledgeStatus : uint8_t {
  Acknowledged = 0,
  NotReady,
  WrongExpedition,
  InvalidState,
};

uint32_t durationSeconds(Duration duration);
const char* durationLabel(Duration duration);
const char* phaseLabel(Phase phase);
bool reportForIndex(uint8_t reportIndex, ExpeditionReport& report);
bool validateExpeditionState(const ExpeditionState& state);

class ExpeditionCore {
 public:
  ExpeditionCore();

  void reset();
  RestoreStatus restore(const ExpeditionState& state);
  ExpeditionState snapshot() const { return state_; }

  StartStatus start(Duration duration, const StartContext& context,
                    const ClockSample& now, uint32_t entropy);
  PollStatus poll(const ClockSample& now);

  ExpeditionView view() const;
  bool completion(CompletionHooks& hooks) const;

  // Call only after every requested external hook was committed
  // idempotently. The sequence is retained so a later expedition has a new
  // stable identity even after the completed record is cleared.
  AcknowledgeStatus acknowledge(uint32_t expeditionId);

 private:
  ExpeditionState state_{};

  void refreshCrc();
};

}  // namespace expedition
}  // namespace kitsu868
