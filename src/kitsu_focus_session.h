#pragma once

#include <stddef.h>
#include <stdint.h>

// A small, allocation-free focus timer. The caller owns storage, display,
// notifications and clock trust. This module deliberately has no dependency on
// companion care, sleep, quiet-hours or the activity implementation.
namespace kitsu868 {
namespace focus {

constexpr uint8_t kFocusStateSchemaVersion = 1U;
constexpr uint16_t kMinimumFocusMinutes = 5U;
constexpr uint16_t kMaximumFocusMinutes = 120U;
constexpr uint16_t kTwentyFiveMinutePreset = 25U;
constexpr uint16_t kFiftyMinutePreset = 50U;
constexpr uint64_t kMinimumTrustedUnixSeconds = UINT64_C(1577836800);
constexpr uint64_t kMaximumTrustedUnixSeconds = UINT64_C(4102444800);

enum class Phase : uint8_t {
  Idle = 0U,
  Focus,
  Break,
  Completed,
};

enum class DurationKind : uint8_t {
  TwentyFiveMinutes = 0U,
  FiftyMinutes,
  Custom,
};

enum class CompletionKind : uint8_t {
  None = 0U,
  Natural,
  Stopped,
  Cancelled,
};

enum class Status : uint8_t {
  Ok = 0U,
  Duplicate,
  InvalidArgument,
  Busy,
  Conflict,
  WrongSession,
  WrongPhase,
  ClockRollback,
  InvalidClock,
};

enum class RestoreStatus : uint8_t {
  Ok = 0U,
  BadMagic,
  UnsupportedSchema,
  InvalidState,
  BadCrc,
  ClockRollback,
  InvalidClock,
};

struct ClockSample {
  uint32_t monotonicMs = 0U;
  bool unixTrusted = false;
  uint64_t unixSeconds = 0U;
};

struct StartRequest {
  // This is both the operation id and the durable session id. Repeating the
  // same valid request is a no-op, including after cancellation/completion.
  uint64_t sessionId = 0U;
  DurationKind duration = DurationKind::TwentyFiveMinutes;
  uint16_t customMinutes = 0U;
};

struct Update {
  Phase before = Phase::Idle;
  Phase after = Phase::Idle;
  bool changed = false;
  bool focusCompleted = false;
  bool sessionCompleted = false;
  // This recommends the existing PulseBreathing activity to the caller. The
  // engine never starts it and never modifies any companion setting/state.
  bool recommendPulseBreathing = false;
};

struct Prompt {
  constexpr Prompt()
      : title(""), detail(""), recommendPulseBreathing(false) {}
  constexpr Prompt(const char* promptTitle, const char* promptDetail,
                   bool recommendBreathing)
      : title(promptTitle),
        detail(promptDetail),
        recommendPulseBreathing(recommendBreathing) {}

  const char* title;
  const char* detail;
  bool recommendPulseBreathing;
};

#pragma pack(push, 1)
struct FocusState {
  uint32_t magic = UINT32_C(0x3153464B);  // "KFS1" little-endian.
  uint16_t bytes = sizeof(FocusState);
  uint8_t schemaVersion = kFocusStateSchemaVersion;
  uint8_t phase = static_cast<uint8_t>(Phase::Idle);
  uint8_t durationKind =
      static_cast<uint8_t>(DurationKind::TwentyFiveMinutes);
  uint8_t completionKind = static_cast<uint8_t>(CompletionKind::None);
  uint16_t focusMinutes = 0U;
  uint16_t breakMinutes = 0U;
  uint16_t reserved = 0U;
  uint32_t elapsedMs = 0U;
  uint32_t anchorElapsedMs = 0U;
  uint64_t sessionId = 0U;
  uint64_t anchorUnixSeconds = 0U;
  uint32_t sequence = 0U;
  uint32_t crc32 = 0U;
};
#pragma pack(pop)

static_assert(sizeof(FocusState) == 48U,
              "focus persistence layout must remain stable");

struct View {
  Phase phase = Phase::Idle;
  DurationKind duration = DurationKind::TwentyFiveMinutes;
  CompletionKind completion = CompletionKind::None;
  uint64_t sessionId = 0U;
  uint16_t focusMinutes = 0U;
  uint16_t breakMinutes = 0U;
  uint32_t elapsedMs = 0U;
  uint32_t remainingMs = 0U;
  uint32_t sequence = 0U;
  Prompt prompt{};
};

uint32_t focusStateCrc(const FocusState& state);
bool validateFocusState(const FocusState& state);
uint16_t recommendedBreakMinutes(uint16_t focusMinutes);

class FocusSession {
 public:
  FocusSession();

  void reset();
  Status start(const StartRequest& request, const ClockSample& clock,
               Update& update);
  Status tick(const ClockSample& clock, Update& update);
  Status stop(uint64_t sessionId, const ClockSample& clock, Update& update);
  Status cancel(uint64_t sessionId, const ClockSample& clock, Update& update);
  Status acknowledge(uint64_t sessionId);

  RestoreStatus restore(const FocusState& saved, const ClockSample& clock,
                        Update& update);
  FocusState snapshot() const { return state_; }
  View view() const;
  Prompt prompt() const;

  Phase phase() const;
  uint64_t sessionId() const { return state_.sessionId; }

 private:
  void refreshCrc();
  void bumpSequence();
  void arm(uint32_t monotonicMs);
  void disarm();
  void applyElapsed(uint32_t elapsedMs, Update& update);

  FocusState state_{};
  uint32_t bootAnchorMs_ = 0U;
  uint32_t bootAnchorElapsedMs_ = 0U;
  bool timingArmed_ = false;
};

}  // namespace focus
}  // namespace kitsu868
