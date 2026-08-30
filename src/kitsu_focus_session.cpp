#include "kitsu_focus_session.h"

namespace kitsu868 {
namespace focus {
namespace {

constexpr uint32_t kFocusStateMagic = UINT32_C(0x3153464B);
constexpr uint32_t kMillisecondsPerMinute = UINT32_C(60000);
constexpr uint32_t kMaximumMonotonicDelta = UINT32_C(0x7FFFFFFF);

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

bool validPhase(uint8_t value) {
  return value <= static_cast<uint8_t>(Phase::Completed);
}

bool validDurationKind(uint8_t value) {
  return value <= static_cast<uint8_t>(DurationKind::Custom);
}

bool validCompletionKind(uint8_t value) {
  return value <= static_cast<uint8_t>(CompletionKind::Cancelled);
}

bool validTrustedUnix(uint64_t unixSeconds) {
  return unixSeconds >= kMinimumTrustedUnixSeconds &&
         unixSeconds <= kMaximumTrustedUnixSeconds;
}

bool validateClock(const ClockSample& clock) {
  return !clock.unixTrusted || validTrustedUnix(clock.unixSeconds);
}

uint32_t durationMs(uint16_t minutes) {
  return static_cast<uint32_t>(minutes) * kMillisecondsPerMinute;
}

uint32_t focusMs(const FocusState& state) {
  return durationMs(state.focusMinutes);
}

uint32_t totalMs(const FocusState& state) {
  return durationMs(static_cast<uint16_t>(state.focusMinutes +
                                          state.breakMinutes));
}

bool normalizeDuration(const StartRequest& request, uint16_t& focusMinutes,
                       uint16_t& breakMinutes) {
  switch (request.duration) {
    case DurationKind::TwentyFiveMinutes:
      if (request.customMinutes != 0U) {
        return false;
      }
      focusMinutes = kTwentyFiveMinutePreset;
      break;
    case DurationKind::FiftyMinutes:
      if (request.customMinutes != 0U) {
        return false;
      }
      focusMinutes = kFiftyMinutePreset;
      break;
    case DurationKind::Custom:
      if (request.customMinutes < kMinimumFocusMinutes ||
          request.customMinutes > kMaximumFocusMinutes) {
        return false;
      }
      focusMinutes = request.customMinutes;
      break;
    default:
      return false;
  }
  breakMinutes = recommendedBreakMinutes(focusMinutes);
  return true;
}

bool validStoredDuration(const FocusState& state) {
  const DurationKind kind = static_cast<DurationKind>(state.durationKind);
  StartRequest request{};
  request.sessionId = state.sessionId;
  request.duration = kind;
  request.customMinutes =
      kind == DurationKind::Custom ? state.focusMinutes : 0U;
  uint16_t normalizedFocus = 0U;
  uint16_t normalizedBreak = 0U;
  return normalizeDuration(request, normalizedFocus, normalizedBreak) &&
         normalizedFocus == state.focusMinutes &&
         normalizedBreak == state.breakMinutes;
}

bool validStateSemantics(const FocusState& state) {
  if (!validPhase(state.phase) || !validDurationKind(state.durationKind) ||
      !validCompletionKind(state.completionKind) || state.reserved != 0U) {
    return false;
  }

  const Phase phase = static_cast<Phase>(state.phase);
  const CompletionKind completion =
      static_cast<CompletionKind>(state.completionKind);

  if (state.sessionId == 0U) {
    return phase == Phase::Idle && completion == CompletionKind::None &&
           state.focusMinutes == 0U && state.breakMinutes == 0U &&
           state.elapsedMs == 0U && state.anchorElapsedMs == 0U &&
           state.anchorUnixSeconds == 0U && state.sequence == 0U;
  }

  if (state.sequence == 0U || !validStoredDuration(state)) {
    return false;
  }

  const uint32_t focusDuration = focusMs(state);
  const uint32_t totalDuration = totalMs(state);
  if (state.elapsedMs > totalDuration ||
      state.anchorElapsedMs > state.elapsedMs) {
    return false;
  }
  if (state.anchorUnixSeconds == 0U) {
    if (state.anchorElapsedMs != 0U) {
      return false;
    }
  } else if (!validTrustedUnix(state.anchorUnixSeconds)) {
    return false;
  }

  switch (phase) {
    case Phase::Focus:
      return completion == CompletionKind::None &&
             state.elapsedMs < focusDuration;
    case Phase::Break:
      return completion == CompletionKind::None &&
             state.elapsedMs >= focusDuration &&
             state.elapsedMs < totalDuration;
    case Phase::Completed:
      if (state.anchorElapsedMs != 0U || state.anchorUnixSeconds != 0U) {
        return false;
      }
      if (completion == CompletionKind::Natural) {
        return state.elapsedMs == totalDuration;
      }
      return completion == CompletionKind::Stopped &&
             state.elapsedMs < totalDuration;
    case Phase::Idle:
      if (state.anchorElapsedMs != 0U || state.anchorUnixSeconds != 0U) {
        return false;
      }
      if (completion == CompletionKind::Natural) {
        return state.elapsedMs == totalDuration;
      }
      if (completion == CompletionKind::Stopped ||
          completion == CompletionKind::Cancelled) {
        return state.elapsedMs < totalDuration;
      }
      return false;
    default:
      return false;
  }
}

uint32_t recoveredElapsed(const FocusState& state,
                          uint64_t currentUnixSeconds) {
  const uint32_t totalDuration = totalMs(state);
  if (state.anchorUnixSeconds == 0U) {
    return state.elapsedMs;
  }

  const uint64_t deltaSeconds = currentUnixSeconds - state.anchorUnixSeconds;
  const uint32_t remaining = totalDuration - state.anchorElapsedMs;
  const uint64_t secondsToEnd =
      (static_cast<uint64_t>(remaining) + 999U) / 1000U;
  uint32_t fromUnix = totalDuration;
  if (deltaSeconds < secondsToEnd) {
    fromUnix = state.anchorElapsedMs +
               static_cast<uint32_t>(deltaSeconds * UINT64_C(1000));
  }
  return fromUnix > state.elapsedMs ? fromUnix : state.elapsedMs;
}

void initializeUpdate(Update& update, Phase phase) {
  update = Update{};
  update.before = phase;
  update.after = phase;
}

}  // namespace

uint32_t focusStateCrc(const FocusState& state) {
  return crc32Bytes(reinterpret_cast<const uint8_t*>(&state),
                    offsetof(FocusState, crc32));
}

bool validateFocusState(const FocusState& state) {
  return state.magic == kFocusStateMagic &&
         state.bytes == sizeof(FocusState) &&
         state.schemaVersion == kFocusStateSchemaVersion &&
         state.crc32 == focusStateCrc(state) && validStateSemantics(state);
}

uint16_t recommendedBreakMinutes(uint16_t focusMinutes) {
  if (focusMinutes <= 25U) {
    return 5U;
  }
  if (focusMinutes <= 50U) {
    return 10U;
  }
  return 15U;
}

FocusSession::FocusSession() { reset(); }

void FocusSession::reset() {
  state_ = FocusState{};
  disarm();
  refreshCrc();
}

Phase FocusSession::phase() const {
  return static_cast<Phase>(state_.phase);
}

void FocusSession::refreshCrc() { state_.crc32 = focusStateCrc(state_); }

void FocusSession::bumpSequence() {
  ++state_.sequence;
  if (state_.sequence == 0U) {
    state_.sequence = 1U;
  }
}

void FocusSession::arm(uint32_t monotonicMs) {
  bootAnchorMs_ = monotonicMs;
  bootAnchorElapsedMs_ = state_.elapsedMs;
  timingArmed_ = true;
}

void FocusSession::disarm() {
  bootAnchorMs_ = 0U;
  bootAnchorElapsedMs_ = 0U;
  timingArmed_ = false;
}

Status FocusSession::start(const StartRequest& request,
                           const ClockSample& clock, Update& update) {
  initializeUpdate(update, phase());

  uint16_t requestedFocus = 0U;
  uint16_t requestedBreak = 0U;
  if (request.sessionId == 0U ||
      !normalizeDuration(request, requestedFocus, requestedBreak)) {
    return Status::InvalidArgument;
  }

  if (state_.sessionId == request.sessionId) {
    const bool sameRequest =
        state_.durationKind == static_cast<uint8_t>(request.duration) &&
        state_.focusMinutes == requestedFocus &&
        state_.breakMinutes == requestedBreak;
    return sameRequest ? Status::Duplicate : Status::Conflict;
  }

  if (phase() != Phase::Idle) {
    return Status::Busy;
  }
  if (!validateClock(clock)) {
    return Status::InvalidClock;
  }

  FocusState started{};
  started.phase = static_cast<uint8_t>(Phase::Focus);
  started.durationKind = static_cast<uint8_t>(request.duration);
  started.completionKind = static_cast<uint8_t>(CompletionKind::None);
  started.focusMinutes = requestedFocus;
  started.breakMinutes = requestedBreak;
  started.sessionId = request.sessionId;
  started.sequence = state_.sequence;
  if (clock.unixTrusted) {
    started.anchorUnixSeconds = clock.unixSeconds;
  }
  state_ = started;
  bumpSequence();
  refreshCrc();
  arm(clock.monotonicMs);

  update.changed = true;
  update.after = Phase::Focus;
  return Status::Ok;
}

void FocusSession::applyElapsed(uint32_t elapsedMs, Update& update) {
  const Phase oldPhase = phase();
  const uint32_t focusDuration = focusMs(state_);
  const uint32_t totalDuration = totalMs(state_);
  state_.elapsedMs = elapsedMs > totalDuration ? totalDuration : elapsedMs;

  Phase newPhase = Phase::Focus;
  if (state_.elapsedMs >= totalDuration) {
    newPhase = Phase::Completed;
    state_.completionKind = static_cast<uint8_t>(CompletionKind::Natural);
    state_.anchorElapsedMs = 0U;
    state_.anchorUnixSeconds = 0U;
    disarm();
  } else if (state_.elapsedMs >= focusDuration) {
    newPhase = Phase::Break;
  }
  state_.phase = static_cast<uint8_t>(newPhase);

  update.after = newPhase;
  update.focusCompleted = oldPhase == Phase::Focus &&
                          newPhase != Phase::Focus;
  update.sessionCompleted = newPhase == Phase::Completed &&
                            oldPhase != Phase::Completed;
  update.recommendPulseBreathing = oldPhase == Phase::Focus &&
                                   newPhase == Phase::Break;
}

Status FocusSession::tick(const ClockSample& clock, Update& update) {
  initializeUpdate(update, phase());
  if (phase() != Phase::Focus && phase() != Phase::Break) {
    return Status::Ok;
  }
  if (!validateClock(clock)) {
    return Status::InvalidClock;
  }
  if (clock.unixTrusted && state_.anchorUnixSeconds != 0U &&
      clock.unixSeconds < state_.anchorUnixSeconds) {
    return Status::ClockRollback;
  }
  if (!timingArmed_) {
    return Status::InvalidClock;
  }

  const uint32_t monotonicDelta = clock.monotonicMs - bootAnchorMs_;
  if (monotonicDelta > kMaximumMonotonicDelta) {
    return Status::ClockRollback;
  }

  const uint32_t totalDuration = totalMs(state_);
  uint32_t elapsed = totalDuration;
  if (monotonicDelta < totalDuration - bootAnchorElapsedMs_) {
    elapsed = bootAnchorElapsedMs_ + monotonicDelta;
  }

  const FocusState beforeState = state_;
  applyElapsed(elapsed, update);
  if (phase() == Phase::Focus || phase() == Phase::Break) {
    if (clock.unixTrusted) {
      state_.anchorUnixSeconds = clock.unixSeconds;
      state_.anchorElapsedMs = state_.elapsedMs;
    }
  }

  const bool stateChanged =
      state_.elapsedMs != beforeState.elapsedMs ||
      state_.phase != beforeState.phase ||
      state_.completionKind != beforeState.completionKind ||
      state_.anchorElapsedMs != beforeState.anchorElapsedMs ||
      state_.anchorUnixSeconds != beforeState.anchorUnixSeconds;
  if (stateChanged) {
    bumpSequence();
    refreshCrc();
    update.changed = true;
  }
  return Status::Ok;
}

Status FocusSession::stop(uint64_t sessionId, const ClockSample& clock,
                          Update& update) {
  initializeUpdate(update, phase());
  if (sessionId == 0U || sessionId != state_.sessionId) {
    return Status::WrongSession;
  }
  if (phase() != Phase::Focus && phase() != Phase::Break) {
    return Status::WrongPhase;
  }

  const Status tickStatus = tick(clock, update);
  if (tickStatus != Status::Ok || phase() == Phase::Completed) {
    return tickStatus;
  }

  state_.phase = static_cast<uint8_t>(Phase::Completed);
  state_.completionKind = static_cast<uint8_t>(CompletionKind::Stopped);
  state_.anchorElapsedMs = 0U;
  state_.anchorUnixSeconds = 0U;
  disarm();
  bumpSequence();
  refreshCrc();
  update.changed = true;
  update.after = Phase::Completed;
  update.sessionCompleted = false;
  update.recommendPulseBreathing = false;
  return Status::Ok;
}

Status FocusSession::cancel(uint64_t sessionId, const ClockSample& clock,
                            Update& update) {
  initializeUpdate(update, phase());
  if (sessionId == 0U || sessionId != state_.sessionId) {
    return Status::WrongSession;
  }
  if (phase() != Phase::Focus && phase() != Phase::Break) {
    return Status::WrongPhase;
  }

  const Status tickStatus = tick(clock, update);
  if (tickStatus != Status::Ok || phase() == Phase::Completed) {
    return tickStatus;
  }

  state_.phase = static_cast<uint8_t>(Phase::Idle);
  state_.completionKind = static_cast<uint8_t>(CompletionKind::Cancelled);
  state_.anchorElapsedMs = 0U;
  state_.anchorUnixSeconds = 0U;
  disarm();
  bumpSequence();
  refreshCrc();
  update.changed = true;
  update.after = Phase::Idle;
  update.sessionCompleted = false;
  update.recommendPulseBreathing = false;
  return Status::Ok;
}

Status FocusSession::acknowledge(uint64_t sessionId) {
  if (sessionId == 0U || sessionId != state_.sessionId) {
    return Status::WrongSession;
  }
  if (phase() != Phase::Completed) {
    return Status::WrongPhase;
  }
  state_.phase = static_cast<uint8_t>(Phase::Idle);
  bumpSequence();
  refreshCrc();
  return Status::Ok;
}

RestoreStatus FocusSession::restore(const FocusState& saved,
                                    const ClockSample& clock,
                                    Update& update) {
  initializeUpdate(update, phase());
  if (saved.magic != kFocusStateMagic) {
    return RestoreStatus::BadMagic;
  }
  if (saved.bytes != sizeof(FocusState) ||
      saved.schemaVersion != kFocusStateSchemaVersion) {
    return RestoreStatus::UnsupportedSchema;
  }
  if (saved.crc32 != focusStateCrc(saved)) {
    return RestoreStatus::BadCrc;
  }
  if (!validStateSemantics(saved)) {
    return RestoreStatus::InvalidState;
  }

  const Phase savedPhase = static_cast<Phase>(saved.phase);
  initializeUpdate(update, savedPhase);
  const bool active = savedPhase == Phase::Focus || savedPhase == Phase::Break;
  if (active && !validateClock(clock)) {
    return RestoreStatus::InvalidClock;
  }
  if (active && clock.unixTrusted && saved.anchorUnixSeconds != 0U &&
      clock.unixSeconds < saved.anchorUnixSeconds) {
    return RestoreStatus::ClockRollback;
  }

  FocusState restored = saved;
  state_ = restored;
  disarm();
  if (active) {
    const uint32_t elapsed =
        clock.unixTrusted ? recoveredElapsed(saved, clock.unixSeconds)
                          : saved.elapsedMs;
    applyElapsed(elapsed, update);
    if (phase() == Phase::Focus || phase() == Phase::Break) {
      if (clock.unixTrusted) {
        state_.anchorUnixSeconds = clock.unixSeconds;
        state_.anchorElapsedMs = state_.elapsedMs;
      } else {
        state_.anchorUnixSeconds = 0U;
        state_.anchorElapsedMs = 0U;
      }
      arm(clock.monotonicMs);
    }
  }

  const bool stateChanged =
      state_.elapsedMs != saved.elapsedMs || state_.phase != saved.phase ||
      state_.completionKind != saved.completionKind ||
      state_.anchorElapsedMs != saved.anchorElapsedMs ||
      state_.anchorUnixSeconds != saved.anchorUnixSeconds;
  if (stateChanged) {
    bumpSequence();
    refreshCrc();
    update.changed = true;
  }
  return RestoreStatus::Ok;
}

Prompt FocusSession::prompt() const {
  switch (phase()) {
    case Phase::Focus:
      return Prompt{"FOCUS TIME", "STAY WITH IT", false};
    case Phase::Break:
      return Prompt{"FOCUS COMPLETE", "TRY PULSE BREATHING", true};
    case Phase::Completed:
      if (static_cast<CompletionKind>(state_.completionKind) ==
          CompletionKind::Stopped) {
        return Prompt{"SESSION STOPPED", "READY WHEN YOU ARE", false};
      }
      return Prompt{"SESSION COMPLETE", "NICE WORK", false};
    case Phase::Idle:
    default:
      return Prompt{};
  }
}

View FocusSession::view() const {
  View result{};
  result.phase = phase();
  result.duration = static_cast<DurationKind>(state_.durationKind);
  result.completion =
      static_cast<CompletionKind>(state_.completionKind);
  result.sessionId = state_.sessionId;
  result.focusMinutes = state_.focusMinutes;
  result.breakMinutes = state_.breakMinutes;
  result.elapsedMs = state_.elapsedMs;
  result.sequence = state_.sequence;
  if (phase() == Phase::Focus || phase() == Phase::Break) {
    result.remainingMs = totalMs(state_) - state_.elapsedMs;
  }
  result.prompt = prompt();
  return result;
}

}  // namespace focus
}  // namespace kitsu868
