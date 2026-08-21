#pragma once

#include <stdint.h>

// Tiny, deterministic one-button games.  This module deliberately has no
// Arduino or display dependency: callers supply millis() values and draw the
// returned view data however they like.
namespace kitsu868 {

enum class MiniGamePhase : uint8_t {
  Inactive = 0,
  Playing,
  Result,
  Finished,
};

enum class MiniGameResult : uint8_t {
  None = 0,
  Perfect,
  Good,
  Hit,
  Caught,
  TooEarly,
  TooLate,
  Miss,
};

enum class MiniGameInput : uint8_t {
  Ignored = 0,
  Accepted,
};

enum class TravelDirection : uint8_t {
  LeftToRight = 0,
  RightToLeft,
};

enum class PounceFetchVerb : uint8_t {
  Pounce = 0,
  Fetch,
};

const char* miniGamePhaseLabel(MiniGamePhase phase);
const char* miniGameResultLabel(MiniGameResult result);
const char* pounceFetchTitle(PounceFetchVerb verb);

struct SignalCatchConfig {
  uint8_t rounds = 5;
  uint8_t trackLeft = 5;
  uint8_t trackRight = 58;
  uint8_t targetWidth = 11;
  uint8_t laneY = 66;
  uint16_t oneWayMs = 1800;
  uint16_t roundMs = 6500;
  uint16_t resultMs = 1000;
};

struct SignalCatchView {
  MiniGamePhase phase = MiniGamePhase::Inactive;
  MiniGameResult result = MiniGameResult::None;
  uint8_t markerX = 0;
  uint8_t markerY = 0;
  uint8_t targetLeft = 0;
  uint8_t targetRight = 0;
  uint8_t targetY = 0;
  uint8_t round = 0;
  uint8_t totalRounds = 0;
  uint8_t streak = 0;
  uint8_t pointsAwarded = 0;
  uint16_t score = 0;
  uint32_t remainingMs = 0;
};

class SignalCatchGame {
 public:
  explicit SignalCatchGame(const SignalCatchConfig& config = SignalCatchConfig());

  void start(uint32_t nowMs, uint32_t seed);
  void cancel();
  void tick(uint32_t nowMs);
  MiniGameInput tap(uint32_t nowMs);
  SignalCatchView view(uint32_t nowMs) const;

  MiniGamePhase phase() const { return phase_; }
  bool finished() const { return phase_ == MiniGamePhase::Finished; }
  uint16_t score() const { return score_; }
  uint8_t streak() const { return streak_; }

 private:
  void sanitizeConfig();
  uint32_t nextRandom();
  void beginRound(uint32_t atMs);
  void resolve(MiniGameResult result, uint8_t basePoints, uint32_t atMs);
  uint8_t markerAt(uint32_t nowMs) const;
  uint32_t remainingAt(uint32_t nowMs) const;

  SignalCatchConfig config_;
  MiniGamePhase phase_ = MiniGamePhase::Inactive;
  MiniGameResult result_ = MiniGameResult::None;
  uint32_t rng_ = 1;
  uint32_t phaseStartedAt_ = 0;
  uint16_t score_ = 0;
  uint8_t roundIndex_ = 0;
  uint8_t streak_ = 0;
  uint8_t pointsAwarded_ = 0;
  uint8_t targetLeft_ = 0;
  uint8_t markerX_ = 0;
};

struct PounceFetchConfig {
  uint8_t rounds = 5;
  uint8_t trackLeft = 5;
  uint8_t trackRight = 58;
  uint8_t catchLeft = 25;
  uint8_t catchRight = 38;
  uint8_t laneY = 68;
  uint16_t firstTravelMs = 2800;
  uint16_t speedupMs = 220;
  uint16_t minimumTravelMs = 1700;
  uint16_t resultMs = 1000;
};

struct PounceFetchView {
  MiniGamePhase phase = MiniGamePhase::Inactive;
  MiniGameResult result = MiniGameResult::None;
  TravelDirection direction = TravelDirection::LeftToRight;
  uint8_t objectX = 0;
  uint8_t objectY = 0;
  uint8_t catchLeft = 0;
  uint8_t catchRight = 0;
  uint8_t catchY = 0;
  uint8_t round = 0;
  uint8_t totalRounds = 0;
  uint8_t streak = 0;
  uint8_t pointsAwarded = 0;
  uint16_t score = 0;
  uint16_t travelMs = 0;
  uint32_t remainingMs = 0;
};

class PounceFetchGame {
 public:
  explicit PounceFetchGame(const PounceFetchConfig& config = PounceFetchConfig());

  void start(uint32_t nowMs, uint32_t seed);
  void cancel();
  void tick(uint32_t nowMs);
  MiniGameInput tap(uint32_t nowMs);
  PounceFetchView view(uint32_t nowMs) const;

  MiniGamePhase phase() const { return phase_; }
  bool finished() const { return phase_ == MiniGamePhase::Finished; }
  uint16_t score() const { return score_; }
  uint8_t streak() const { return streak_; }

 private:
  void sanitizeConfig();
  uint32_t nextRandom();
  void beginRound(uint32_t atMs);
  void resolve(MiniGameResult result, uint8_t basePoints, uint32_t atMs);
  uint16_t travelDuration() const;
  uint8_t objectAt(uint32_t nowMs) const;
  uint32_t remainingAt(uint32_t nowMs) const;

  PounceFetchConfig config_;
  MiniGamePhase phase_ = MiniGamePhase::Inactive;
  MiniGameResult result_ = MiniGameResult::None;
  TravelDirection direction_ = TravelDirection::LeftToRight;
  uint32_t rng_ = 1;
  uint32_t phaseStartedAt_ = 0;
  uint16_t score_ = 0;
  uint8_t roundIndex_ = 0;
  uint8_t streak_ = 0;
  uint8_t pointsAwarded_ = 0;
  uint8_t objectX_ = 0;
};

}  // namespace kitsu868
