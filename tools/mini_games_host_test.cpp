#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../src/mini_games.h"

using namespace kitsu868;

namespace {

uint32_t findSignalHit(SignalCatchGame& game, uint32_t fromMs,
                       uint32_t searchMs) {
  for (uint32_t offset = 0; offset < searchMs; offset += 10) {
    const SignalCatchView view = game.view(fromMs + offset);
    if (view.markerX >= view.targetLeft && view.markerX <= view.targetRight) {
      return fromMs + offset;
    }
  }
  assert(false && "signal marker never crossed target");
  return fromMs;
}

uint32_t findPounceCatch(PounceFetchGame& game, uint32_t fromMs,
                         uint32_t searchMs) {
  for (uint32_t offset = 0; offset < searchMs; offset += 10) {
    const PounceFetchView view = game.view(fromMs + offset);
    if (view.objectX >= view.catchLeft && view.objectX <= view.catchRight) {
      return fromMs + offset;
    }
  }
  assert(false && "moving object never crossed catch window");
  return fromMs;
}

uint32_t advanceToEchoReplay(EchoBeatGame& game, uint32_t fromMs,
                             uint32_t searchMs) {
  for (uint32_t offset = 0; offset <= searchMs; ++offset) {
    const uint32_t now = fromMs + offset;
    game.tick(now);
    if (game.stage() == EchoBeatStage::Replay) return now;
  }
  assert(false && "echo pattern never entered replay");
  return fromMs;
}

uint32_t tapEchoWithError(EchoBeatGame& game, uint32_t nowMs,
                          int16_t timingErrorMs) {
  const EchoBeatView before = game.view(nowMs);
  assert(before.stage == EchoBeatStage::Replay);
  assert(before.nextBeatInMs >
         static_cast<uint32_t>(timingErrorMs < 0 ? -timingErrorMs : 0));
  const uint32_t targetAt = nowMs + before.nextBeatInMs;
  const uint32_t tapAt = timingErrorMs < 0
      ? targetAt - static_cast<uint32_t>(-timingErrorMs)
      : targetAt + static_cast<uint32_t>(timingErrorMs);
  assert(game.tap(tapAt) == MiniGameInput::Accepted);
  return tapAt;
}

void testLabels() {
  assert(strcmp(miniGamePhaseLabel(MiniGamePhase::Playing), "TAP!") == 0);
  assert(strcmp(miniGameResultLabel(MiniGameResult::Perfect), "PERFECT") == 0);
  assert(strcmp(miniGameResultLabel(MiniGameResult::TooEarly), "EARLY") == 0);
  assert(strcmp(pounceFetchTitle(PounceFetchVerb::Pounce), "POUNCE") == 0);
  assert(strcmp(pounceFetchTitle(PounceFetchVerb::Fetch), "FETCH") == 0);
  assert(strcmp(echoBeatStageLabel(EchoBeatStage::Presenting), "LISTEN") == 0);
  assert(strcmp(echoBeatStageLabel(EchoBeatStage::Replay), "REPEAT") == 0);
}

void testSignalDeterminismAndScoring() {
  SignalCatchConfig config;
  config.rounds = 2;
  config.oneWayMs = 1000;
  config.roundMs = 3000;
  config.resultMs = 200;

  SignalCatchGame first(config);
  SignalCatchGame second(config);
  first.start(100, 0x12345678);
  second.start(100, 0x12345678);

  for (uint32_t now = 100; now < 1100; now += 37) {
    const SignalCatchView a = first.view(now);
    const SignalCatchView b = second.view(now);
    assert(a.markerX == b.markerX);
    assert(a.targetLeft == b.targetLeft);
    assert(a.targetRight == b.targetRight);
  }

  const uint32_t hit1 = findSignalHit(first, 100, config.oneWayMs + 10);
  assert(first.tap(hit1) == MiniGameInput::Accepted);
  const SignalCatchView result1 = first.view(hit1 + 50);
  assert(result1.phase == MiniGamePhase::Result);
  assert(result1.result == MiniGameResult::Perfect ||
         result1.result == MiniGameResult::Good ||
         result1.result == MiniGameResult::Hit);
  assert(result1.streak == 1);
  assert(result1.pointsAwarded >= 1);
  assert(result1.markerX >= result1.targetLeft &&
         result1.markerX <= result1.targetRight);

  const uint32_t round2 = hit1 + config.resultMs;
  first.tick(round2);
  assert(first.view(round2).round == 2);
  const uint32_t hit2 = findSignalHit(first, round2, config.oneWayMs + 10);
  assert(first.tap(hit2) == MiniGameInput::Accepted);
  const SignalCatchView result2 = first.view(hit2);
  assert(result2.streak == 2);
  assert(result2.score > result1.score);
  assert(result2.pointsAwarded >= 2);  // Hit plus the streak bonus.
  first.tick(hit2 + config.resultMs);
  assert(first.finished());
  assert(first.tap(hit2 + config.resultMs) == MiniGameInput::Ignored);
}

void testSignalTimeoutAndSparseTick() {
  SignalCatchConfig config;
  config.rounds = 3;
  config.oneWayMs = 250;
  config.roundMs = 500;
  config.resultMs = 100;
  SignalCatchGame game(config);
  game.start(50, 7);

  // One sparse tick crosses every play/result boundary deterministically:
  // 3 * (500 + 100) ms after the start is the exact finish boundary.
  game.tick(50 + 1800);
  const SignalCatchView view = game.view(50 + 1800);
  assert(view.phase == MiniGamePhase::Finished);
  assert(view.round == 3);
  assert(view.score == 0);
  assert(view.result == MiniGameResult::Miss);
}

void testSignalMillisWrap() {
  SignalCatchConfig config;
  config.oneWayMs = 800;
  config.roundMs = 2000;
  SignalCatchGame wrapped(config);
  SignalCatchGame normal(config);
  const uint32_t wrappedStart = 0xffffff00UL;
  wrapped.start(wrappedStart, 99);
  normal.start(100, 99);

  for (uint32_t offset = 0; offset <= 1200; offset += 100) {
    const SignalCatchView a = wrapped.view(wrappedStart + offset);
    const SignalCatchView b = normal.view(100 + offset);
    assert(a.markerX == b.markerX);
    assert(a.remainingMs == b.remainingMs);
    assert(a.targetLeft == b.targetLeft);
  }
}

void testPounceDeterminismAndScoring() {
  PounceFetchConfig config;
  config.rounds = 2;
  config.firstTravelMs = 2000;
  config.speedupMs = 300;
  config.minimumTravelMs = 1200;
  config.resultMs = 200;

  PounceFetchGame first(config);
  PounceFetchGame second(config);
  first.start(1000, 0xabcdef01);
  second.start(1000, 0xabcdef01);
  for (uint32_t now = 1000; now < 2000; now += 43) {
    const PounceFetchView a = first.view(now);
    const PounceFetchView b = second.view(now);
    assert(a.objectX == b.objectX);
    assert(a.direction == b.direction);
  }

  const uint32_t catch1 = findPounceCatch(first, 1000, config.firstTravelMs + 10);
  assert(first.tap(catch1) == MiniGameInput::Accepted);
  const PounceFetchView result1 = first.view(catch1);
  assert(result1.result == MiniGameResult::Perfect ||
         result1.result == MiniGameResult::Caught);
  assert(result1.streak == 1);
  assert(result1.objectX >= result1.catchLeft &&
         result1.objectX <= result1.catchRight);

  const uint32_t round2 = catch1 + config.resultMs;
  first.tick(round2);
  const PounceFetchView next = first.view(round2);
  assert(next.round == 2);
  assert(next.travelMs == 1700);
  const uint32_t catch2 = findPounceCatch(first, round2, next.travelMs + 10);
  first.tap(catch2);
  assert(first.view(catch2).streak == 2);
  first.tick(catch2 + config.resultMs);
  assert(first.finished());
}

void testPouncePerfectOwnsTopBaseScore() {
  PounceFetchConfig config;
  config.rounds = 1;
  config.firstTravelMs = 2000;
  config.minimumTravelMs = 2000;

  PounceFetchGame caught(config);
  caught.start(100, 0x1234);
  const uint32_t edge = findPounceCatch(caught, 100, 2010);
  caught.tap(edge);
  assert(caught.view(edge).result == MiniGameResult::Caught);
  assert(caught.view(edge).pointsAwarded == 2);

  PounceFetchGame perfect(config);
  perfect.start(100, 0x1234);
  uint32_t centered = 0;
  for (uint32_t offset = 0; offset <= 2000; ++offset) {
    const PounceFetchView view = perfect.view(100 + offset);
    const int doubled = static_cast<int>(view.objectX) * 2;
    const int center = static_cast<int>(view.catchLeft) + view.catchRight;
    if (doubled == center - 1 || doubled == center + 1) {
      centered = 100 + offset;
      break;
    }
  }
  assert(centered != 0);
  perfect.tap(centered);
  assert(perfect.view(centered).result == MiniGameResult::Perfect);
  assert(perfect.view(centered).pointsAwarded == 3);
}

void testPounceEarlyTimeoutAndWrap() {
  PounceFetchConfig config;
  config.rounds = 1;
  config.firstTravelMs = 1000;
  config.minimumTravelMs = 1000;
  config.resultMs = 100;

  PounceFetchGame early(config);
  early.start(0, 5);
  assert(early.tap(0) == MiniGameInput::Accepted);
  assert(early.view(0).result == MiniGameResult::TooEarly);

  PounceFetchGame timeout(config);
  timeout.start(200, 5);
  timeout.tick(1200);
  assert(timeout.view(1200).phase == MiniGamePhase::Result);
  assert(timeout.view(1200).result == MiniGameResult::TooLate);
  timeout.tick(1300);
  assert(timeout.finished());

  PounceFetchGame wrapped(config);
  PounceFetchGame normal(config);
  const uint32_t wrappedStart = 0xffffff80UL;
  wrapped.start(wrappedStart, 42);
  normal.start(500, 42);
  for (uint32_t offset = 0; offset < 1000; offset += 73) {
    assert(wrapped.view(wrappedStart + offset).objectX ==
           normal.view(500 + offset).objectX);
    assert(wrapped.view(wrappedStart + offset).remainingMs ==
           normal.view(500 + offset).remainingMs);
  }
}

EchoBeatConfig quickEchoConfig() {
  EchoBeatConfig config;
  config.minimumBeats = 4;
  config.maximumBeats = 4;
  config.leadInMs = 100;
  config.minimumGapMs = 200;
  config.maximumGapMs = 400;
  config.flashMs = 50;
  config.intermissionMs = 100;
  config.perfectWindowMs = 20;
  config.goodWindowMs = 40;
  config.hitWindowMs = 60;
  config.resultMs = 100;
  return config;
}

void testEchoDefaultBeatBounds() {
  EchoBeatGame game;
  for (uint32_t seed = 1; seed <= 64; ++seed) {
    game.start(seed * 10U, seed);
    assert(game.beatCount() >= 3);
    assert(game.beatCount() <= 6);
  }
}

void testEchoPresentationDeterminismAndPerfectReplay() {
  const EchoBeatConfig config = quickEchoConfig();
  EchoBeatGame first(config);
  EchoBeatGame second(config);
  first.start(1000, 0x13572468);
  second.start(1000, 0x13572468);

  assert(first.beatCount() == 4);
  assert(second.beatCount() == first.beatCount());
  assert(first.tap(1000) == MiniGameInput::Ignored);
  uint32_t replayAt = 0;
  for (uint32_t now = 1000; now < 3000; now += 13) {
    first.tick(now);
    second.tick(now);
    const EchoBeatView a = first.view(now);
    const EchoBeatView b = second.view(now);
    assert(a.stage == b.stage);
    assert(a.cueOn == b.cueOn);
    assert(a.presentedBeats == b.presentedBeats);
    assert(a.remainingMs == b.remainingMs);
    assert(a.nextBeatInMs == b.nextBeatInMs);
    if (a.stage == EchoBeatStage::Replay) {
      replayAt = now;
      break;
    }
  }

  assert(replayAt != 0);
  uint32_t now = replayAt;
  assert(first.view(now).presentedBeats == 4);
  for (uint8_t beat = 0; beat < 4; ++beat) {
    now = tapEchoWithError(first, now, 0);
    const EchoBeatView after = first.view(now);
    assert(after.replayedBeats == static_cast<uint8_t>(beat + 1));
    assert(after.lastBeatResult == MiniGameResult::Perfect);
    assert(after.lastTimingErrorMs == 0);
  }

  const EchoBeatView result = first.view(now);
  assert(result.phase == MiniGamePhase::Result);
  assert(result.stage == EchoBeatStage::Result);
  assert(result.result == MiniGameResult::Perfect);
  assert(result.perfectBeats == 4);
  assert(result.missedBeats == 0);
  assert(result.score == 12);
  assert(result.maximumScore == 12);
  first.tick(now + config.resultMs);
  assert(first.finished());
  assert(first.stage() == EchoBeatStage::Finished);
  assert(first.tap(now + config.resultMs) == MiniGameInput::Ignored);
}

void testEchoAccuracyBandsAndAggregateResult() {
  const EchoBeatConfig config = quickEchoConfig();
  EchoBeatGame game(config);
  game.start(500, 0x24681357);
  uint32_t now = advanceToEchoReplay(game, 500, 5000);

  now = tapEchoWithError(game, now, 0);
  assert(game.view(now).lastBeatResult == MiniGameResult::Perfect);
  now = tapEchoWithError(game, now, 21);
  assert(game.view(now).lastBeatResult == MiniGameResult::Good);
  now = tapEchoWithError(game, now, 41);
  assert(game.view(now).lastBeatResult == MiniGameResult::Hit);
  now = tapEchoWithError(game, now, -61);

  const EchoBeatView result = game.view(now);
  assert(result.phase == MiniGamePhase::Result);
  assert(result.result == MiniGameResult::Hit);
  assert(result.perfectBeats == 1);
  assert(result.goodBeats == 1);
  assert(result.hitBeats == 1);
  assert(result.missedBeats == 1);
  assert(result.score == 6);
  assert(result.lastTimingErrorMs == -61);
}

void testEchoSparseTimeoutCancelAndWrap() {
  EchoBeatConfig config = quickEchoConfig();
  config.minimumBeats = 3;
  config.maximumBeats = 3;

  EchoBeatGame sparse(config);
  sparse.start(50, 7);
  sparse.tick(30050);
  const EchoBeatView timedOut = sparse.view(30050);
  assert(timedOut.phase == MiniGamePhase::Finished);
  assert(timedOut.stage == EchoBeatStage::Finished);
  assert(timedOut.result == MiniGameResult::Miss);
  assert(timedOut.missedBeats == 3);
  assert(timedOut.score == 0);

  sparse.start(100, 7);
  sparse.cancel();
  assert(sparse.phase() == MiniGamePhase::Inactive);
  assert(sparse.stage() == EchoBeatStage::Inactive);
  assert(sparse.view(100).totalBeats == 0);
  assert(sparse.tap(100) == MiniGameInput::Ignored);

  EchoBeatGame wrapped(config);
  EchoBeatGame normal(config);
  const uint32_t wrappedStart = 0xffffff00UL;
  wrapped.start(wrappedStart, 0x10203040);
  normal.start(500, 0x10203040);
  for (uint32_t offset = 0; offset <= 2400; offset += 37) {
    wrapped.tick(wrappedStart + offset);
    normal.tick(500 + offset);
    const EchoBeatView a = wrapped.view(wrappedStart + offset);
    const EchoBeatView b = normal.view(500 + offset);
    assert(a.stage == b.stage);
    assert(a.cueOn == b.cueOn);
    assert(a.presentedBeats == b.presentedBeats);
    assert(a.replayedBeats == b.replayedBeats);
    assert(a.nextBeatInMs == b.nextBeatInMs);
    assert(a.remainingMs == b.remainingMs);
  }
}

}  // namespace

int main() {
  static_assert(sizeof(SignalCatchGame) <= 48,
                "Signal Catch state unexpectedly grew");
  static_assert(sizeof(PounceFetchGame) <= 48,
                "Pounce/Fetch state unexpectedly grew");
  static_assert(sizeof(EchoBeatGame) <= 64,
                "Echo Beat state unexpectedly grew");

  testLabels();
  testSignalDeterminismAndScoring();
  testSignalTimeoutAndSparseTick();
  testSignalMillisWrap();
  testPounceDeterminismAndScoring();
  testPouncePerfectOwnsTopBaseScore();
  testPounceEarlyTimeoutAndWrap();
  testEchoDefaultBeatBounds();
  testEchoPresentationDeterminismAndPerfectReplay();
  testEchoAccuracyBandsAndAggregateResult();
  testEchoSparseTimeoutCancelAndWrap();

  return 0;
}
