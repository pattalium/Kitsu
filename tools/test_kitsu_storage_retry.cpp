#include <assert.h>
#include <stdio.h>

#include "../src/kitsu_storage_retry.h"

using namespace kitsu868::connectivity;

int main() {
  StorageRetrySchedule schedule;
  assert(!schedule.attemptDue(0U));

  schedule.markDirty(100U, 5000U);
  assert(!schedule.attemptDue(5099U));
  schedule.markDirty(200U, 0U);
  assert(schedule.attemptDue(200U));
  schedule.recordAttempt(200U);
  schedule.recordFailure(200U, StorageRetryFailure::Transient);
  assert(!schedule.attemptDue(5199U));
  assert(schedule.attemptDue(5200U));

  // New data coalesces but cannot turn a failed transaction into a hot loop.
  schedule.markDirty(300U, 0U);
  assert(!schedule.attemptDue(300U));
  schedule.recordFailure(5200U, StorageRetryFailure::Transient);
  assert(!schedule.attemptDue(35199U));
  assert(schedule.attemptDue(35200U));

  schedule.recordFailure(35200U, StorageRetryFailure::NoSpace);
  assert(schedule.status().blockedNoSpace);
  schedule.markDirty(40000U, 0U);
  assert(!schedule.attemptDue(0xffffffffUL));
  schedule.rearmAfterHeadroom(50000U, 5000U);
  assert(!schedule.attemptDue(54999U));
  assert(schedule.attemptDue(55000U));
  schedule.recordSuccess();
  assert(!schedule.status().dirty);
  assert(schedule.status().failureCount == 0U);

  // Deadlines remain correct across millis() rollover.
  schedule.markDirty(0xfffffff0UL, 32U);
  assert(!schedule.attemptDue(0xffffffffUL));
  assert(schedule.attemptDue(0x10U));

  puts("Kitsu storage retry tests passed.");
  return 0;
}
