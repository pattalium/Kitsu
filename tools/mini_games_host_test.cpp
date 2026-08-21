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

void testLabels() {
  assert(strcmp(miniGamePhaseLabel(MiniGamePhase::Playing), "TAP!") == 0);
  assert(strcmp(miniGameResultLabel(MiniGameResult::Perfect), "PERFECT") == 0);
  assert(strcmp(miniGameResultLabel(MiniGameResult::TooEarly), "EARLY") == 0);
  assert(strcmp(pounceFetchTitle(PounceFetchVerb::Pounce), "POUNCE") == 0);
  assert(strcmp(pounceFetchTitle(PounceFetchVerb::Fetch), "FETCH") == 0);
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

}  // namespace

int main() {
  static_assert(sizeof(SignalCatchGame) <= 48,
                "Signal Catch state unexpectedly grew");
  static_assert(sizeof(PounceFetchGame) <= 48,
                "Pounce/Fetch state unexpectedly grew");

  testLabels();
  testSignalDeterminismAndScoring();
  testSignalTimeoutAndSparseTick();
  testSignalMillisWrap();
  testPounceDeterminismAndScoring();
  testPouncePerfectOwnsTopBaseScore();
  testPounceEarlyTimeoutAndWrap();

  return 0;
}
