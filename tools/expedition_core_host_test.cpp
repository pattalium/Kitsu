#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/expedition_core.h"

using namespace kitsu868;

namespace {

constexpr uint64_t kUnixBase = UINT64_C(1800000000);

expedition::ClockSample clockAt(uint32_t bootId, uint32_t monotonicMillis,
                                uint64_t unixSeconds = 0U,
                                bool unixValid = false) {
  expedition::ClockSample clock{};
  clock.bootId = bootId;
  clock.monotonicMillis = monotonicMillis;
  clock.unixSeconds = unixSeconds;
  clock.unixValid = unixValid ? 1U : 0U;
  return clock;
}

expedition::StartContext contextFor(PersonalityKind personality) {
  expedition::StartContext context{};
  context.personality = personality;
  context.mood = CompanionMood::Listening;
  context.affection = 44U;
  context.companionFingerprint = UINT32_C(0x12345678);
  return context;
}

void finishAtDue(expedition::ExpeditionCore& core, uint32_t nextBootId) {
  const expedition::ExpeditionState state = core.snapshot();
  assert(state.dueUnixSeconds >= kUnixBase);
  assert(core.poll(clockAt(nextBootId, 10U, state.dueUnixSeconds, true)) ==
         expedition::PollStatus::BecameReady);
}

void testDurationsReportsAndFreshState() {
  static_assert(sizeof(expedition::ExpeditionState) == 64U,
                "Expedition state must stay compact");
  assert(expedition::durationSeconds(expedition::Duration::Short) == 900U);
  assert(expedition::durationSeconds(expedition::Duration::Medium) == 7200U);
  assert(expedition::durationSeconds(expedition::Duration::Long) == 28800U);
  assert(strcmp(expedition::durationLabel(expedition::Duration::Medium),
                "MEDIUM") == 0);
  assert(strcmp(expedition::phaseLabel(expedition::Phase::Traveling),
                "SCOUTING") == 0);

  for (uint8_t index = 0U; index < expedition::kReportCount; ++index) {
    expedition::ExpeditionReport report{};
    assert(expedition::reportForIndex(index, report));
    assert(report.headline != NULL && report.headline[0] != '\0');
    assert(report.detail != NULL && report.detail[0] != '\0');
  }
  expedition::ExpeditionReport invalid{};
  assert(!expedition::reportForIndex(expedition::kReportCount, invalid));

  expedition::ExpeditionCore core;
  assert(expedition::validateExpeditionState(core.snapshot()));
  const expedition::ExpeditionView view = core.view();
  assert(view.phase == expedition::Phase::Idle);
  assert(view.totalSeconds == 0U && view.reportIndex == expedition::kNoReport);
  expedition::CompletionHooks hooks{};
  assert(!core.completion(hooks));
}

void testOutcomeFixedAtStartAndAcrossRestore() {
  expedition::ExpeditionCore first;
  expedition::StartContext context = contextFor(PersonalityKind::Impish);
  context.eligibleEncounterMask =
      (UINT32_C(1) << 2U) | (UINT32_C(1) << 7U);
  const expedition::ClockSample start = clockAt(7U, 500U, kUnixBase, true);
  assert(first.start(expedition::Duration::Medium, context, start,
                     UINT32_C(0xCAFEBABE)) ==
         expedition::StartStatus::Started);

  const expedition::ExpeditionState persisted = first.snapshot();
  assert(expedition::validateExpeditionState(persisted));
  assert(persisted.reportIndex >= 20U && persisted.reportIndex < 24U);
  assert(persisted.memoryReportIndex == persisted.reportIndex);
  assert(persisted.affectionDelta == 2);
  assert(persisted.mood == static_cast<uint8_t>(CompanionMood::Impish));
  assert(persisted.personalityAxis ==
         static_cast<uint8_t>(expedition::PersonalityAxis::Playfulness));
  assert(persisted.personalityDelta == 1);

  const expedition::ExpeditionView traveling = first.view();
  assert(traveling.phase == expedition::Phase::Traveling);
  assert(traveling.reportIndex == expedition::kNoReport);
  assert(first.start(expedition::Duration::Short, context, start, 1U) ==
         expedition::StartStatus::Busy);

  expedition::ExpeditionCore restored;
  assert(restored.restore(persisted) == expedition::RestoreStatus::Ok);
  assert(restored.snapshot().expeditionId == persisted.expeditionId);
  assert(restored.snapshot().reportIndex == persisted.reportIndex);
  assert(restored.snapshot().encounterCatalogIndex ==
         persisted.encounterCatalogIndex);

  finishAtDue(restored, 8U);
  const expedition::ExpeditionView returned = restored.view();
  assert(returned.phase == expedition::Phase::Ready);
  assert(returned.progressPercent == 100U);
  assert(returned.reportIndex == persisted.reportIndex);

  expedition::CompletionHooks hooks{};
  assert(restored.completion(hooks));
  assert(hooks.expeditionId == persisted.expeditionId);
  assert(hooks.memoryReportIndex == persisted.reportIndex);
  assert(hooks.affectionDelta == persisted.affectionDelta);
  assert(restored.acknowledge(hooks.expeditionId + 1U) ==
         expedition::AcknowledgeStatus::WrongExpedition);
  assert(restored.completion(hooks));
  assert(restored.acknowledge(hooks.expeditionId) ==
         expedition::AcknowledgeStatus::Acknowledged);
  assert(!restored.completion(hooks));
  assert(restored.view().phase == expedition::Phase::Idle);
  assert(restored.snapshot().sequence == persisted.sequence);
}

void testMonotonicWallAndRebootTiming() {
  expedition::ExpeditionCore core;
  const expedition::StartContext context = contextFor(PersonalityKind::Gentle);
  assert(core.start(expedition::Duration::Short, context,
                    clockAt(10U, 1000U, kUnixBase, true), 77U) ==
         expedition::StartStatus::Started);

  // A backwards wall-clock correction cannot undo monotonic progress.
  assert(core.poll(clockAt(10U, 301000U, kUnixBase - 10U, true)) ==
         expedition::PollStatus::Progressed);
  assert(core.view().remainingSeconds == 600U);
  assert(core.view().progressPercent == 33U);

  // A new boot cannot reuse monotonic time. The fixed wall deadline safely
  // accounts for downtime instead.
  assert(core.poll(clockAt(11U, 25U, kUnixBase + 700U, true)) ==
         expedition::PollStatus::Progressed);
  assert(core.view().remainingSeconds == 200U);
  finishAtDue(core, 12U);
  assert(core.view().remainingSeconds == 0U);
}

void testNoWallPausesAcrossBootThenAnchors() {
  expedition::ExpeditionCore core;
  const expedition::StartContext context = contextFor(PersonalityKind::Curious);
  assert(core.start(expedition::Duration::Short, context,
                    clockAt(20U, 1000U), 123U) ==
         expedition::StartStatus::Started);
  assert(core.snapshot().dueUnixSeconds == 0U);

  assert(core.poll(clockAt(20U, 301000U)) ==
         expedition::PollStatus::Progressed);
  assert(core.view().remainingSeconds == 600U);
  const expedition::ExpeditionState persisted = core.snapshot();

  expedition::ExpeditionCore rebooted;
  assert(rebooted.restore(persisted) == expedition::RestoreStatus::Ok);
  assert(rebooted.poll(clockAt(21U, 50U)) ==
         expedition::PollStatus::Progressed);
  assert(rebooted.view().remainingSeconds == 600U);

  // When a trusted wall clock first appears, it anchors the amount that was
  // safely proven before the reboot; the unknown downtime is not invented.
  assert(rebooted.poll(clockAt(21U, 50U, kUnixBase, true)) ==
         expedition::PollStatus::Progressed);
  assert(rebooted.snapshot().dueUnixSeconds == kUnixBase + 600U);
  assert(rebooted.poll(clockAt(22U, 1U, kUnixBase + 600U, true)) ==
         expedition::PollStatus::BecameReady);
}

void testMonotonicRolloverAndResetDefense() {
  expedition::ExpeditionCore rollover;
  const expedition::StartContext context = contextFor(PersonalityKind::Bold);
  const uint32_t nearWrap = UINT32_MAX - 500U;
  assert(rollover.start(expedition::Duration::Short, context,
                        clockAt(30U, nearWrap), 9U) ==
         expedition::StartStatus::Started);
  assert(rollover.poll(clockAt(30U, 499U)) ==
         expedition::PollStatus::Progressed);
  assert(rollover.view().remainingSeconds == 899U);

  expedition::ExpeditionCore resetLooking;
  assert(resetLooking.start(expedition::Duration::Short, context,
                            clockAt(31U, 100000U), 10U) ==
         expedition::StartStatus::Started);
  assert(resetLooking.poll(clockAt(31U, 50U)) ==
         expedition::PollStatus::Progressed);
  assert(resetLooking.view().remainingSeconds == 900U);
  assert(resetLooking.snapshot().checkpointMonotonicMillis == 50U);
}

void testEncounterCandidatesRespectEligibility() {
  const uint32_t eligible =
      (UINT32_C(1) << 2U) | (UINT32_C(1) << 7U) |
      (UINT32_C(1) << 20U);
  bool found = false;
  for (uint32_t entropy = 0U; entropy < 1000U; ++entropy) {
    expedition::ExpeditionCore core;
    expedition::StartContext context = contextFor(PersonalityKind::Playful);
    context.eligibleEncounterMask = eligible;
    assert(core.start(expedition::Duration::Long, context,
                      clockAt(40U, 0U), entropy) ==
           expedition::StartStatus::Started);
    const uint8_t candidate = core.snapshot().encounterCatalogIndex;
    if (candidate != expedition::kNoEncounter) {
      assert(candidate < expedition::kCatalogCreatureCount);
      assert((eligible & (UINT32_C(1) << candidate)) != 0U);
      found = true;
    }
  }
  assert(found);

  expedition::ExpeditionCore none;
  expedition::StartContext context = contextFor(PersonalityKind::Playful);
  assert(none.start(expedition::Duration::Long, context, clockAt(41U, 0U),
                    1U) == expedition::StartStatus::Started);
  assert(none.snapshot().encounterCatalogIndex == expedition::kNoEncounter);
}

void testValidationAndBadInputs() {
  expedition::ExpeditionCore core;
  expedition::StartContext context = contextFor(PersonalityKind::Shy);
  assert(core.start(expedition::Duration::Count, context, clockAt(50U, 0U),
                    0U) == expedition::StartStatus::InvalidDuration);
  assert(core.start(expedition::Duration::Short, context, clockAt(0U, 0U),
                    0U) == expedition::StartStatus::InvalidClock);
  assert(core.start(expedition::Duration::Short, context,
                    clockAt(50U, 0U, 10U, true), 0U) ==
         expedition::StartStatus::InvalidClock);

  context.affection = 101U;
  assert(core.start(expedition::Duration::Short, context, clockAt(50U, 0U),
                    0U) == expedition::StartStatus::InvalidContext);
  context = contextFor(PersonalityKind::Shy);
  context.eligibleEncounterMask = UINT32_C(1) << 25U;
  assert(core.start(expedition::Duration::Short, context, clockAt(50U, 0U),
                    0U) == expedition::StartStatus::InvalidContext);
  context = contextFor(static_cast<PersonalityKind>(255U));
  assert(core.start(expedition::Duration::Short, context, clockAt(50U, 0U),
                    0U) == expedition::StartStatus::InvalidContext);

  expedition::ExpeditionCore valid;
  context = contextFor(PersonalityKind::Shy);
  assert(valid.start(expedition::Duration::Short, context,
                     clockAt(51U, 0U), 0U) ==
         expedition::StartStatus::Started);
  expedition::ExpeditionState corrupt = valid.snapshot();
  --corrupt.remainingSeconds;
  assert(!expedition::validateExpeditionState(corrupt));
  expedition::ExpeditionCore target;
  assert(target.restore(corrupt) == expedition::RestoreStatus::InvalidState);

  expedition::ExpeditionState badMagic = valid.snapshot();
  badMagic.magic = 0U;
  assert(target.restore(badMagic) == expedition::RestoreStatus::BadMagic);
  expedition::ExpeditionState future = valid.snapshot();
  ++future.schemaVersion;
  assert(target.restore(future) ==
         expedition::RestoreStatus::UnsupportedSchema);
}

}  // namespace

int main() {
  testDurationsReportsAndFreshState();
  testOutcomeFixedAtStartAndAcrossRestore();
  testMonotonicWallAndRebootTiming();
  testNoWallPausesAcrossBootThenAnchors();
  testMonotonicRolloverAndResetDefense();
  testEncounterCandidatesRespectEligibility();
  testValidationAndBadInputs();
  puts("PASS expedition_core_host");
  return 0;
}
