#include "mini_games.h"

namespace kitsu868 {
namespace {

constexpr uint8_t kDisplayMaxX = 63;
constexpr uint8_t kDisplayMaxY = 127;
constexpr uint8_t kMaximumRounds = 12;
constexpr uint32_t kFallbackSeed = 0x4b383638UL;  // "K868"

uint8_t clampU8(uint8_t value, uint8_t low, uint8_t high) {
  if (value < low) return low;
  if (value > high) return high;
  return value;
}

uint16_t saturatingAdd(uint16_t value, uint8_t increment) {
  const uint16_t room = static_cast<uint16_t>(0xffffU - value);
  return increment > room ? 0xffffU : static_cast<uint16_t>(value + increment);
}

uint8_t absoluteDistance2(uint8_t position, uint8_t left, uint8_t right) {
  const int16_t doubledPosition = static_cast<int16_t>(position) * 2;
  const int16_t doubledCenter = static_cast<int16_t>(left) + right;
  const int16_t delta = doubledPosition - doubledCenter;
  return static_cast<uint8_t>(delta < 0 ? -delta : delta);
}

uint32_t timeRemaining(uint32_t nowMs, uint32_t startedAt, uint32_t durationMs) {
  // Unsigned subtraction is intentionally used throughout this file.  It is
  // well-defined across the 32-bit millis() wrap as long as a single phase is
  // shorter than one complete wrap (all defaults are only a few seconds).
  const uint32_t elapsed = nowMs - startedAt;
  return elapsed >= durationMs ? 0 : durationMs - elapsed;
}

}  // namespace

const char* miniGamePhaseLabel(MiniGamePhase phase) {
  switch (phase) {
    case MiniGamePhase::Inactive: return "READY";
    case MiniGamePhase::Playing: return "TAP!";
    case MiniGamePhase::Result: return "RESULT";
    case MiniGamePhase::Finished: return "DONE";
  }
  return "READY";
}

const char* miniGameResultLabel(MiniGameResult result) {
  switch (result) {
    case MiniGameResult::None: return "";
    case MiniGameResult::Perfect: return "PERFECT";
    case MiniGameResult::Good: return "GOOD";
    case MiniGameResult::Hit: return "HIT";
    case MiniGameResult::Caught: return "CAUGHT";
    case MiniGameResult::TooEarly: return "EARLY";
    case MiniGameResult::TooLate: return "LATE";
    case MiniGameResult::Miss: return "MISS";
  }
  return "";
}

const char* pounceFetchTitle(PounceFetchVerb verb) {
  return verb == PounceFetchVerb::Fetch ? "FETCH" : "POUNCE";
}

SignalCatchGame::SignalCatchGame(const SignalCatchConfig& config) : config_(config) {
  sanitizeConfig();
}

void SignalCatchGame::sanitizeConfig() {
  config_.rounds = clampU8(config_.rounds, 1, kMaximumRounds);
  // Three pixels are needed for the smallest useful target.
  config_.trackLeft = clampU8(config_.trackLeft, 0, kDisplayMaxX - 3);
  config_.trackRight = clampU8(config_.trackRight,
                               static_cast<uint8_t>(config_.trackLeft + 2),
                               kDisplayMaxX);
  const uint8_t trackPixels =
      static_cast<uint8_t>(config_.trackRight - config_.trackLeft + 1);
  config_.targetWidth = clampU8(config_.targetWidth, 3, trackPixels);
  config_.laneY = clampU8(config_.laneY, 0, kDisplayMaxY);
  if (config_.oneWayMs < 250) config_.oneWayMs = 250;
  if (config_.roundMs < config_.oneWayMs) config_.roundMs = config_.oneWayMs;
  if (config_.resultMs < 100) config_.resultMs = 100;
}

void SignalCatchGame::start(uint32_t nowMs, uint32_t seed) {
  phase_ = MiniGamePhase::Playing;
  result_ = MiniGameResult::None;
  rng_ = seed == 0 ? kFallbackSeed : seed;
  score_ = 0;
  roundIndex_ = 0;
  streak_ = 0;
  pointsAwarded_ = 0;
  beginRound(nowMs);
}

void SignalCatchGame::cancel() {
  phase_ = MiniGamePhase::Inactive;
  result_ = MiniGameResult::None;
  pointsAwarded_ = 0;
}

uint32_t SignalCatchGame::nextRandom() {
  uint32_t value = rng_;
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  rng_ = value == 0 ? kFallbackSeed : value;
  return rng_;
}

void SignalCatchGame::beginRound(uint32_t atMs) {
  const uint8_t positions = static_cast<uint8_t>(
      config_.trackRight - config_.trackLeft - config_.targetWidth + 2);
  targetLeft_ = static_cast<uint8_t>(config_.trackLeft + nextRandom() % positions);
  markerX_ = config_.trackLeft;
  phase_ = MiniGamePhase::Playing;
  result_ = MiniGameResult::None;
  pointsAwarded_ = 0;
  phaseStartedAt_ = atMs;
}

void SignalCatchGame::resolve(MiniGameResult result, uint8_t basePoints,
                              uint32_t atMs) {
  result_ = result;
  if (basePoints == 0) {
    streak_ = 0;
    pointsAwarded_ = 0;
  } else {
    if (streak_ != 0xff) ++streak_;
    const uint8_t streakBonus = streak_ > 3 ? 3 : static_cast<uint8_t>(streak_ - 1);
    pointsAwarded_ = static_cast<uint8_t>(basePoints + streakBonus);
    score_ = saturatingAdd(score_, pointsAwarded_);
  }
  phase_ = MiniGamePhase::Result;
  phaseStartedAt_ = atMs;
}

uint8_t SignalCatchGame::markerAt(uint32_t nowMs) const {
  const uint32_t elapsed = nowMs - phaseStartedAt_;
  const uint32_t period = static_cast<uint32_t>(config_.oneWayMs) * 2U;
  const uint32_t phase = elapsed % period;
  const uint32_t distanceTime =
      phase <= config_.oneWayMs ? phase : period - phase;
  const uint32_t span = config_.trackRight - config_.trackLeft;
  const uint32_t offset =
      (distanceTime * span + config_.oneWayMs / 2U) / config_.oneWayMs;
  return static_cast<uint8_t>(config_.trackLeft + offset);
}

uint32_t SignalCatchGame::remainingAt(uint32_t nowMs) const {
  if (phase_ == MiniGamePhase::Playing) {
    return timeRemaining(nowMs, phaseStartedAt_, config_.roundMs);
  }
  if (phase_ == MiniGamePhase::Result) {
    return timeRemaining(nowMs, phaseStartedAt_, config_.resultMs);
  }
  return 0;
}

void SignalCatchGame::tick(uint32_t nowMs) {
  // A loop makes sparse ticks deterministic: if the caller wakes up several
  // phases late, transitions occur at their true boundaries rather than at the
  // delayed tick time.  At most two transitions occur per configured round.
  for (uint8_t transitions = 0; transitions < kMaximumRounds * 2U; ++transitions) {
    if (phase_ == MiniGamePhase::Playing) {
      const uint32_t elapsed = nowMs - phaseStartedAt_;
      if (elapsed < config_.roundMs) return;
      markerX_ = markerAt(phaseStartedAt_ + config_.roundMs);
      resolve(MiniGameResult::Miss, 0, phaseStartedAt_ + config_.roundMs);
      continue;
    }
    if (phase_ == MiniGamePhase::Result) {
      const uint32_t elapsed = nowMs - phaseStartedAt_;
      if (elapsed < config_.resultMs) return;
      const uint32_t nextAt = phaseStartedAt_ + config_.resultMs;
      if (static_cast<uint8_t>(roundIndex_ + 1) >= config_.rounds) {
        phase_ = MiniGamePhase::Finished;
        phaseStartedAt_ = nextAt;
        return;
      }
      ++roundIndex_;
      beginRound(nextAt);
      continue;
    }
    return;
  }
}

MiniGameInput SignalCatchGame::tap(uint32_t nowMs) {
  tick(nowMs);
  if (phase_ != MiniGamePhase::Playing) return MiniGameInput::Ignored;

  const uint8_t marker = markerAt(nowMs);
  markerX_ = marker;
  const uint8_t targetRight =
      static_cast<uint8_t>(targetLeft_ + config_.targetWidth - 1);
  if (marker < targetLeft_ || marker > targetRight) {
    resolve(MiniGameResult::Miss, 0, nowMs);
    return MiniGameInput::Accepted;
  }

  const uint8_t distance2 = absoluteDistance2(marker, targetLeft_, targetRight);
  const uint8_t halfWidth2 = static_cast<uint8_t>(targetRight - targetLeft_);
  if (distance2 <= 1) {
    resolve(MiniGameResult::Perfect, 3, nowMs);
  } else if (distance2 * 2U <= halfWidth2) {
    resolve(MiniGameResult::Good, 2, nowMs);
  } else {
    resolve(MiniGameResult::Hit, 1, nowMs);
  }
  return MiniGameInput::Accepted;
}

SignalCatchView SignalCatchGame::view(uint32_t nowMs) const {
  SignalCatchView value;
  value.phase = phase_;
  value.result = result_;
  value.markerX = phase_ == MiniGamePhase::Playing ? markerAt(nowMs) : markerX_;
  value.markerY = config_.laneY;
  value.targetLeft = targetLeft_;
  value.targetRight = static_cast<uint8_t>(targetLeft_ + config_.targetWidth - 1);
  value.targetY = config_.laneY;
  value.round = phase_ == MiniGamePhase::Inactive ? 0 : static_cast<uint8_t>(roundIndex_ + 1);
  value.totalRounds = config_.rounds;
  value.streak = streak_;
  value.pointsAwarded = pointsAwarded_;
  value.score = score_;
  value.remainingMs = remainingAt(nowMs);
  return value;
}

PounceFetchGame::PounceFetchGame(const PounceFetchConfig& config) : config_(config) {
  sanitizeConfig();
}

void PounceFetchGame::sanitizeConfig() {
  config_.rounds = clampU8(config_.rounds, 1, kMaximumRounds);
  config_.trackLeft = clampU8(config_.trackLeft, 0, kDisplayMaxX - 1);
  config_.trackRight = clampU8(config_.trackRight,
                               static_cast<uint8_t>(config_.trackLeft + 1),
                               kDisplayMaxX);
  config_.catchLeft = clampU8(config_.catchLeft, config_.trackLeft, config_.trackRight);
  config_.catchRight = clampU8(config_.catchRight, config_.catchLeft, config_.trackRight);
  config_.laneY = clampU8(config_.laneY, 0, kDisplayMaxY);
  if (config_.firstTravelMs < 500) config_.firstTravelMs = 500;
  if (config_.minimumTravelMs < 500) config_.minimumTravelMs = 500;
  if (config_.minimumTravelMs > config_.firstTravelMs) {
    config_.minimumTravelMs = config_.firstTravelMs;
  }
  if (config_.resultMs < 100) config_.resultMs = 100;
}

void PounceFetchGame::start(uint32_t nowMs, uint32_t seed) {
  phase_ = MiniGamePhase::Playing;
  result_ = MiniGameResult::None;
  rng_ = seed == 0 ? kFallbackSeed : seed;
  score_ = 0;
  roundIndex_ = 0;
  streak_ = 0;
  pointsAwarded_ = 0;
  beginRound(nowMs);
}

void PounceFetchGame::cancel() {
  phase_ = MiniGamePhase::Inactive;
  result_ = MiniGameResult::None;
  pointsAwarded_ = 0;
}

uint32_t PounceFetchGame::nextRandom() {
  uint32_t value = rng_;
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  rng_ = value == 0 ? kFallbackSeed : value;
  return rng_;
}

void PounceFetchGame::beginRound(uint32_t atMs) {
  direction_ = (nextRandom() & 1U) == 0
      ? TravelDirection::LeftToRight
      : TravelDirection::RightToLeft;
  objectX_ = direction_ == TravelDirection::LeftToRight
      ? config_.trackLeft
      : config_.trackRight;
  phase_ = MiniGamePhase::Playing;
  result_ = MiniGameResult::None;
  pointsAwarded_ = 0;
  phaseStartedAt_ = atMs;
}

void PounceFetchGame::resolve(MiniGameResult result, uint8_t basePoints,
                              uint32_t atMs) {
  result_ = result;
  if (basePoints == 0) {
    streak_ = 0;
    pointsAwarded_ = 0;
  } else {
    if (streak_ != 0xff) ++streak_;
    const uint8_t streakBonus = streak_ > 3 ? 3 : static_cast<uint8_t>(streak_ - 1);
    pointsAwarded_ = static_cast<uint8_t>(basePoints + streakBonus);
    score_ = saturatingAdd(score_, pointsAwarded_);
  }
  phase_ = MiniGamePhase::Result;
  phaseStartedAt_ = atMs;
}

uint16_t PounceFetchGame::travelDuration() const {
  const uint32_t reduction = static_cast<uint32_t>(roundIndex_) * config_.speedupMs;
  if (reduction >= static_cast<uint32_t>(config_.firstTravelMs - config_.minimumTravelMs)) {
    return config_.minimumTravelMs;
  }
  return static_cast<uint16_t>(config_.firstTravelMs - reduction);
}

uint8_t PounceFetchGame::objectAt(uint32_t nowMs) const {
  uint32_t elapsed = nowMs - phaseStartedAt_;
  const uint16_t duration = travelDuration();
  if (elapsed > duration) elapsed = duration;
  const uint32_t span = config_.trackRight - config_.trackLeft;
  const uint32_t offset = (elapsed * span + duration / 2U) / duration;
  if (direction_ == TravelDirection::LeftToRight) {
    return static_cast<uint8_t>(config_.trackLeft + offset);
  }
  return static_cast<uint8_t>(config_.trackRight - offset);
}

uint32_t PounceFetchGame::remainingAt(uint32_t nowMs) const {
  if (phase_ == MiniGamePhase::Playing) {
    return timeRemaining(nowMs, phaseStartedAt_, travelDuration());
  }
  if (phase_ == MiniGamePhase::Result) {
    return timeRemaining(nowMs, phaseStartedAt_, config_.resultMs);
  }
  return 0;
}

void PounceFetchGame::tick(uint32_t nowMs) {
  for (uint8_t transitions = 0; transitions < kMaximumRounds * 2U; ++transitions) {
    if (phase_ == MiniGamePhase::Playing) {
      const uint16_t duration = travelDuration();
      const uint32_t elapsed = nowMs - phaseStartedAt_;
      if (elapsed < duration) return;
      objectX_ = objectAt(phaseStartedAt_ + duration);
      resolve(MiniGameResult::TooLate, 0, phaseStartedAt_ + duration);
      continue;
    }
    if (phase_ == MiniGamePhase::Result) {
      const uint32_t elapsed = nowMs - phaseStartedAt_;
      if (elapsed < config_.resultMs) return;
      const uint32_t nextAt = phaseStartedAt_ + config_.resultMs;
      if (static_cast<uint8_t>(roundIndex_ + 1) >= config_.rounds) {
        phase_ = MiniGamePhase::Finished;
        phaseStartedAt_ = nextAt;
        return;
      }
      ++roundIndex_;
      beginRound(nextAt);
      continue;
    }
    return;
  }
}

MiniGameInput PounceFetchGame::tap(uint32_t nowMs) {
  tick(nowMs);
  if (phase_ != MiniGamePhase::Playing) return MiniGameInput::Ignored;

  const uint8_t object = objectAt(nowMs);
  objectX_ = object;
  if (object >= config_.catchLeft && object <= config_.catchRight) {
    const uint8_t distance2 =
        absoluteDistance2(object, config_.catchLeft, config_.catchRight);
    const bool perfect = distance2 <= 1;
    resolve(perfect ? MiniGameResult::Perfect : MiniGameResult::Caught,
            perfect ? 3 : 2, nowMs);
    return MiniGameInput::Accepted;
  }

  const bool beforeWindow = direction_ == TravelDirection::LeftToRight
      ? object < config_.catchLeft
      : object > config_.catchRight;
  resolve(beforeWindow ? MiniGameResult::TooEarly : MiniGameResult::TooLate, 0, nowMs);
  return MiniGameInput::Accepted;
}

PounceFetchView PounceFetchGame::view(uint32_t nowMs) const {
  PounceFetchView value;
  value.phase = phase_;
  value.result = result_;
  value.direction = direction_;
  value.objectX = phase_ == MiniGamePhase::Playing ? objectAt(nowMs) : objectX_;
  value.objectY = config_.laneY;
  value.catchLeft = config_.catchLeft;
  value.catchRight = config_.catchRight;
  value.catchY = config_.laneY;
  value.round = phase_ == MiniGamePhase::Inactive ? 0 : static_cast<uint8_t>(roundIndex_ + 1);
  value.totalRounds = config_.rounds;
  value.streak = streak_;
  value.pointsAwarded = pointsAwarded_;
  value.score = score_;
  value.travelMs = travelDuration();
  value.remainingMs = remainingAt(nowMs);
  return value;
}

}  // namespace kitsu868
