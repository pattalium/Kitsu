#include "kitsu_focus_session.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {

using kitsu868::focus::ClockSample;
using kitsu868::focus::CompletionKind;
using kitsu868::focus::DurationKind;
using kitsu868::focus::FocusSession;
using kitsu868::focus::FocusState;
using kitsu868::focus::Phase;
using kitsu868::focus::RestoreStatus;
using kitsu868::focus::StartRequest;
using kitsu868::focus::Status;
using kitsu868::focus::Update;

int failures = 0;

#define EXPECT_TRUE(condition)                                                \
  do {                                                                        \
    if (!(condition)) {                                                       \
      printf("FAIL line %d: %s\n", __LINE__, #condition);                    \
      ++failures;                                                             \
    }                                                                         \
  } while (false)

#define EXPECT_EQ(expected, actual) EXPECT_TRUE((expected) == (actual))

ClockSample clockAt(uint32_t monotonicMs) {
  ClockSample clock{};
  clock.monotonicMs = monotonicMs;
  return clock;
}

ClockSample trustedClock(uint32_t monotonicMs, uint64_t unixSeconds) {
  ClockSample clock{};
  clock.monotonicMs = monotonicMs;
  clock.unixTrusted = true;
  clock.unixSeconds = unixSeconds;
  return clock;
}

StartRequest request(uint64_t id, DurationKind duration,
                     uint16_t customMinutes = 0U) {
  StartRequest value{};
  value.sessionId = id;
  value.duration = duration;
  value.customMinutes = customMinutes;
  return value;
}

void reseal(FocusState& state) {
  state.crc32 = kitsu868::focus::focusStateCrc(state);
}

void expectValid(const FocusSession& session) {
  EXPECT_TRUE(kitsu868::focus::validateFocusState(session.snapshot()));
}

void testStartAndDurations() {
  Update update{};
  FocusSession session;
  expectValid(session);

  EXPECT_EQ(Status::Ok,
            session.start(request(1U, DurationKind::TwentyFiveMinutes),
                          clockAt(100U), update));
  EXPECT_EQ(Phase::Focus, session.phase());
  EXPECT_EQ(25U, session.view().focusMinutes);
  EXPECT_EQ(5U, session.view().breakMinutes);
  EXPECT_EQ(UINT32_C(1800000), session.view().remainingMs);
  EXPECT_TRUE(update.changed);
  EXPECT_EQ(Phase::Idle, update.before);
  EXPECT_EQ(Phase::Focus, update.after);
  expectValid(session);

  EXPECT_EQ(Status::Ok, session.cancel(1U, clockAt(100U), update));
  EXPECT_EQ(Status::Ok,
            session.start(request(2U, DurationKind::FiftyMinutes),
                          clockAt(200U), update));
  EXPECT_EQ(50U, session.view().focusMinutes);
  EXPECT_EQ(10U, session.view().breakMinutes);
  EXPECT_EQ(Status::Ok, session.cancel(2U, clockAt(200U), update));

  EXPECT_EQ(Status::Ok,
            session.start(request(3U, DurationKind::Custom, 5U),
                          clockAt(300U), update));
  EXPECT_EQ(5U, session.view().focusMinutes);
  EXPECT_EQ(5U, session.view().breakMinutes);
  EXPECT_EQ(Status::Ok, session.cancel(3U, clockAt(300U), update));

  EXPECT_EQ(Status::Ok,
            session.start(request(4U, DurationKind::Custom, 120U),
                          clockAt(400U), update));
  EXPECT_EQ(120U, session.view().focusMinutes);
  EXPECT_EQ(15U, session.view().breakMinutes);
  EXPECT_EQ(Status::Ok, session.cancel(4U, clockAt(400U), update));

  EXPECT_EQ(Status::InvalidArgument,
            session.start(request(5U, DurationKind::Custom, 4U),
                          clockAt(500U), update));
  EXPECT_EQ(Status::InvalidArgument,
            session.start(request(6U, DurationKind::Custom, 121U),
                          clockAt(500U), update));
  EXPECT_EQ(Status::InvalidArgument,
            session.start(request(7U, DurationKind::TwentyFiveMinutes, 25U),
                          clockAt(500U), update));
  expectValid(session);
}

void testTickCompletionAndPrompt() {
  Update update{};
  FocusSession session;
  EXPECT_EQ(Status::Ok,
            session.start(request(10U, DurationKind::TwentyFiveMinutes),
                          clockAt(1000U), update));

  EXPECT_EQ(Status::Ok,
            session.tick(clockAt(UINT32_C(1500999)), update));
  EXPECT_EQ(Phase::Focus, session.phase());
  EXPECT_TRUE(update.changed);
  EXPECT_TRUE(!update.focusCompleted);
  EXPECT_TRUE(!session.prompt().recommendPulseBreathing);

  EXPECT_EQ(Status::Ok,
            session.tick(clockAt(UINT32_C(1501000)), update));
  EXPECT_EQ(Phase::Break, session.phase());
  EXPECT_TRUE(update.focusCompleted);
  EXPECT_TRUE(!update.sessionCompleted);
  EXPECT_TRUE(update.recommendPulseBreathing);
  EXPECT_TRUE(session.prompt().recommendPulseBreathing);
  EXPECT_TRUE(strcmp("TRY PULSE BREATHING", session.prompt().detail) == 0);

  EXPECT_EQ(Status::Ok,
            session.tick(clockAt(UINT32_C(1801000)), update));
  EXPECT_EQ(Phase::Completed, session.phase());
  EXPECT_TRUE(update.sessionCompleted);
  EXPECT_TRUE(!update.recommendPulseBreathing);
  EXPECT_EQ(CompletionKind::Natural, session.view().completion);
  EXPECT_EQ(0U, session.view().remainingMs);
  EXPECT_TRUE(!session.prompt().recommendPulseBreathing);
  expectValid(session);

  EXPECT_EQ(Status::Ok, session.acknowledge(10U));
  EXPECT_EQ(Phase::Idle, session.phase());
  expectValid(session);
}

void testMonotonicWrapAndRollback() {
  Update update{};
  FocusSession session;
  const uint32_t startedAt = UINT32_C(0xFFFFFF00);
  EXPECT_EQ(Status::Ok,
            session.start(request(20U, DurationKind::TwentyFiveMinutes),
                          clockAt(startedAt), update));
  EXPECT_EQ(Status::Ok,
            session.tick(clockAt(startedAt + UINT32_C(1500000)), update));
  EXPECT_EQ(Phase::Break, session.phase());
  EXPECT_TRUE(update.focusCompleted);
  EXPECT_EQ(Status::Ok,
            session.tick(clockAt(startedAt + UINT32_C(1800000)), update));
  EXPECT_EQ(Phase::Completed, session.phase());
  expectValid(session);

  FocusSession rollback;
  EXPECT_EQ(Status::Ok,
            rollback.start(request(21U, DurationKind::TwentyFiveMinutes),
                           clockAt(10000U), update));
  const FocusState before = rollback.snapshot();
  EXPECT_EQ(Status::ClockRollback, rollback.tick(clockAt(9000U), update));
  const FocusState after = rollback.snapshot();
  EXPECT_TRUE(memcmp(&before, &after, sizeof(before)) == 0);
  expectValid(rollback);
}

void testStopCancelAndAcknowledge() {
  Update update{};
  FocusSession session;
  EXPECT_EQ(Status::Ok,
            session.start(request(30U, DurationKind::TwentyFiveMinutes),
                          clockAt(100U), update));
  EXPECT_EQ(Status::WrongSession,
            session.stop(31U, clockAt(60100U), update));
  EXPECT_EQ(Status::Ok, session.stop(30U, clockAt(60100U), update));
  EXPECT_EQ(Phase::Completed, session.phase());
  EXPECT_EQ(CompletionKind::Stopped, session.view().completion);
  EXPECT_TRUE(!update.sessionCompleted);
  EXPECT_TRUE(!update.recommendPulseBreathing);
  EXPECT_TRUE(!session.prompt().recommendPulseBreathing);
  expectValid(session);

  EXPECT_EQ(Status::WrongPhase,
            session.stop(30U, clockAt(60100U), update));
  EXPECT_EQ(Status::Ok, session.acknowledge(30U));
  EXPECT_EQ(Phase::Idle, session.phase());
  EXPECT_EQ(Status::WrongPhase, session.acknowledge(30U));

  EXPECT_EQ(Status::Ok,
            session.start(request(31U, DurationKind::TwentyFiveMinutes),
                          clockAt(1000U), update));
  EXPECT_EQ(Status::Ok,
            session.tick(clockAt(UINT32_C(1501000)), update));
  EXPECT_EQ(Phase::Break, session.phase());
  EXPECT_EQ(Status::Ok,
            session.cancel(31U, clockAt(UINT32_C(1501000)), update));
  EXPECT_EQ(Phase::Idle, session.phase());
  EXPECT_EQ(CompletionKind::Cancelled, session.view().completion);
  EXPECT_TRUE(!update.recommendPulseBreathing);
  EXPECT_TRUE(!session.prompt().recommendPulseBreathing);
  expectValid(session);
}

void testRebootRecovery() {
  const uint64_t baseUnix = UINT64_C(1800000000);
  Update update{};
  FocusSession original;
  EXPECT_EQ(Status::Ok,
            original.start(request(40U, DurationKind::TwentyFiveMinutes),
                           trustedClock(500U, baseUnix), update));
  EXPECT_EQ(Status::Ok,
            original.tick(trustedClock(60500U, baseUnix + 60U), update));
  const FocusState saved = original.snapshot();
  EXPECT_EQ(UINT32_C(60000), saved.elapsedMs);
  EXPECT_EQ(UINT32_C(60000), saved.anchorElapsedMs);
  expectValid(original);

  FocusSession recoveredBreak;
  EXPECT_EQ(RestoreStatus::Ok,
            recoveredBreak.restore(saved,
                                   trustedClock(77U, baseUnix + 1500U),
                                   update));
  EXPECT_EQ(Phase::Break, recoveredBreak.phase());
  EXPECT_EQ(UINT32_C(1500000), recoveredBreak.view().elapsedMs);
  EXPECT_TRUE(update.focusCompleted);
  EXPECT_TRUE(update.recommendPulseBreathing);
  expectValid(recoveredBreak);

  FocusSession recoveredCompleted;
  EXPECT_EQ(RestoreStatus::Ok,
            recoveredCompleted.restore(saved,
                                       trustedClock(88U, baseUnix + 1800U),
                                       update));
  EXPECT_EQ(Phase::Completed, recoveredCompleted.phase());
  EXPECT_EQ(CompletionKind::Natural, recoveredCompleted.view().completion);
  EXPECT_TRUE(update.focusCompleted);
  EXPECT_TRUE(update.sessionCompleted);
  EXPECT_TRUE(!update.recommendPulseBreathing);
  expectValid(recoveredCompleted);

  FocusSession untrustedRecovery;
  EXPECT_EQ(RestoreStatus::Ok,
            untrustedRecovery.restore(saved, clockAt(900U), update));
  EXPECT_EQ(Phase::Focus, untrustedRecovery.phase());
  EXPECT_EQ(UINT32_C(60000), untrustedRecovery.view().elapsedMs);
  EXPECT_EQ(0U, untrustedRecovery.snapshot().anchorUnixSeconds);
  EXPECT_EQ(Status::Ok,
            untrustedRecovery.tick(clockAt(10900U), update));
  EXPECT_EQ(UINT32_C(70000), untrustedRecovery.view().elapsedMs);
  expectValid(untrustedRecovery);
}

void testTrustedClockRollback() {
  const uint64_t baseUnix = UINT64_C(1800000000);
  Update update{};
  FocusSession session;
  EXPECT_EQ(Status::Ok,
            session.start(request(50U, DurationKind::TwentyFiveMinutes),
                          trustedClock(100U, baseUnix), update));
  EXPECT_EQ(Status::Ok,
            session.tick(trustedClock(1100U, baseUnix + 1U), update));
  const FocusState beforeLiveRollback = session.snapshot();
  EXPECT_EQ(Status::ClockRollback,
            session.tick(trustedClock(2100U, baseUnix), update));
  const FocusState afterLiveRollback = session.snapshot();
  EXPECT_TRUE(memcmp(&beforeLiveRollback, &afterLiveRollback,
                     sizeof(beforeLiveRollback)) == 0);

  FocusSession target;
  EXPECT_EQ(Status::Ok,
            target.start(request(51U, DurationKind::Custom, 5U),
                         clockAt(500U), update));
  const FocusState targetBefore = target.snapshot();
  EXPECT_EQ(RestoreStatus::ClockRollback,
            target.restore(beforeLiveRollback,
                           trustedClock(999U, baseUnix), update));
  const FocusState targetAfter = target.snapshot();
  EXPECT_TRUE(memcmp(&targetBefore, &targetAfter, sizeof(targetBefore)) == 0);
  EXPECT_EQ(Status::Ok, target.tick(clockAt(1500U), update));
  EXPECT_EQ(UINT32_C(1000), target.view().elapsedMs);
  expectValid(target);
}

void testCorruptionAndAtomicRestore() {
  Update update{};
  FocusSession source;
  EXPECT_EQ(Status::Ok,
            source.start(request(60U, DurationKind::TwentyFiveMinutes),
                         clockAt(100U), update));
  const FocusState good = source.snapshot();

  FocusSession target;
  EXPECT_EQ(Status::Ok,
            target.start(request(61U, DurationKind::Custom, 5U),
                         clockAt(200U), update));
  const FocusState targetBefore = target.snapshot();

  FocusState badCrc = good;
  badCrc.focusMinutes ^= 1U;
  EXPECT_EQ(RestoreStatus::BadCrc,
            target.restore(badCrc, clockAt(300U), update));
  const FocusState targetAfterBadCrc = target.snapshot();
  EXPECT_TRUE(memcmp(&targetBefore, &targetAfterBadCrc,
                     sizeof(targetBefore)) == 0);

  FocusState wrongVersion = good;
  wrongVersion.schemaVersion = 2U;
  reseal(wrongVersion);
  EXPECT_EQ(RestoreStatus::UnsupportedSchema,
            target.restore(wrongVersion, clockAt(300U), update));

  FocusState invalid = good;
  invalid.phase = 99U;
  reseal(invalid);
  EXPECT_EQ(RestoreStatus::InvalidState,
            target.restore(invalid, clockAt(300U), update));

  FocusState wrongMagic = good;
  wrongMagic.magic = 0U;
  reseal(wrongMagic);
  EXPECT_EQ(RestoreStatus::BadMagic,
            target.restore(wrongMagic, clockAt(300U), update));
  expectValid(target);
}

void testIdempotency() {
  Update update{};
  FocusSession session;
  const StartRequest first =
      request(70U, DurationKind::TwentyFiveMinutes);
  EXPECT_EQ(Status::Ok, session.start(first, clockAt(100U), update));
  EXPECT_EQ(Status::Ok, session.tick(clockAt(60100U), update));
  const FocusState beforeRetry = session.snapshot();

  EXPECT_EQ(Status::Duplicate,
            session.start(first, clockAt(999999U), update));
  const FocusState afterRetry = session.snapshot();
  EXPECT_TRUE(memcmp(&beforeRetry, &afterRetry, sizeof(beforeRetry)) == 0);
  EXPECT_EQ(Status::Conflict,
            session.start(request(70U, DurationKind::FiftyMinutes),
                          clockAt(999999U), update));
  EXPECT_EQ(Status::Busy,
            session.start(request(71U, DurationKind::FiftyMinutes),
                          clockAt(999999U), update));

  EXPECT_EQ(Status::Ok, session.cancel(70U, clockAt(60100U), update));
  EXPECT_EQ(Status::Duplicate,
            session.start(first, clockAt(70000U), update));
  EXPECT_EQ(Status::Ok,
            session.start(request(71U, DurationKind::FiftyMinutes),
                          clockAt(70000U), update));
  EXPECT_EQ(Status::Ok, session.stop(71U, clockAt(70000U), update));
  EXPECT_EQ(Status::Busy,
            session.start(request(72U, DurationKind::Custom, 5U),
                          clockAt(70000U), update));
  EXPECT_EQ(Status::Ok, session.acknowledge(71U));
  EXPECT_EQ(Status::Duplicate,
            session.start(request(71U, DurationKind::FiftyMinutes),
                          clockAt(70000U), update));
  EXPECT_EQ(Status::Ok,
            session.start(request(72U, DurationKind::Custom, 5U),
                          clockAt(70000U), update));
  expectValid(session);
}

}  // namespace

int main() {
  testStartAndDurations();
  testTickCompletionAndPrompt();
  testMonotonicWrapAndRollback();
  testStopCancelAndAcknowledge();
  testRebootRecovery();
  testTrustedClockRollback();
  testCorruptionAndAtomicRestore();
  testIdempotency();

  if (failures != 0) {
    printf("kitsu_focus_session: %d failure(s)\n", failures);
    return 1;
  }
  printf("kitsu_focus_session: all tests passed\n");
  return 0;
}
