#include "../src/kitsu_nvs_headroom.h"

#include <assert.h>
#include <string.h>

using kitsu868::connectivity::KitsuNvsHeadroom;
using kitsu868::connectivity::NvsHeadroomPlatform;
using kitsu868::connectivity::NvsHeadroomResult;
using kitsu868::connectivity::NvsHeadroomStats;
using kitsu868::connectivity::NvsHeadroomStatus;
using kitsu868::connectivity::kNimbleBondEntries;
using kitsu868::connectivity::kNvsEntriesPerPage;
using kitsu868::connectivity::kPairingRequiredFreeEntries;
using kitsu868::connectivity::kPairingRequiredUsableEntries;
using kitsu868::connectivity::kPairingSafetyEntries;
using kitsu868::connectivity::kSecurityTransactionEntries;
using kitsu868::connectivity::nvsBlobEntries;
using kitsu868::connectivity::nvsHeadroomResultName;
using kitsu868::connectivity::pairingHeadroomReady;
using kitsu868::connectivity::pairingUsableEntries;

namespace {

class MemoryPlatform final : public NvsHeadroomPlatform {
 public:
  bool eraseRetiredActionReplay(bool& changed) override {
    ++actionCalls;
    changed = false;
    if (failAction) return false;
    if (actionPresent) {
      actionPresent = false;
      stats.freeEntries += 10U;
      changed = true;
    }
    return true;
  }

  bool eraseRetiredBluedroidConfig(bool& changed) override {
    ++bluedroidCalls;
    changed = false;
    if (failBluedroid) return false;
    if (bluedroidPresent) {
      bluedroidPresent = false;
      stats.freeEntries += 40U;
      changed = true;
    }
    return true;
  }

  bool readStats(NvsHeadroomStats& output) override {
    ++statsCalls;
    if (failStats) return false;
    output = stats;
    return true;
  }

  NvsHeadroomStats stats{300U, 132U, 630U};
  bool actionPresent = false;
  bool bluedroidPresent = false;
  bool failAction = false;
  bool failBluedroid = false;
  bool failStats = false;
  size_t actionCalls = 0U;
  size_t bluedroidCalls = 0U;
  size_t statsCalls = 0U;
};

void testPinnedEntryBudgets() {
  assert(nvsBlobEntries(0U) == 2U);
  assert(nvsBlobEntries(1U) == 3U);
  assert(nvsBlobEntries(32U) == 3U);
  assert(nvsBlobEntries(33U) == 4U);
  assert(nvsBlobEntries(364U) == 14U);
  assert(kSecurityTransactionEntries == 14U);
  assert(kNimbleBondEntries == 22U);
  assert(kPairingSafetyEntries == 12U);
  assert(kPairingRequiredUsableEntries == 48U);
  assert(kNvsEntriesPerPage == 126U);
  assert(kPairingRequiredFreeEntries == 174U);
}

void testBackupShapeReclaimsOnlyRetiredRecords() {
  MemoryPlatform platform;
  platform.actionPresent = true;
  platform.bluedroidPresent = true;
  const NvsHeadroomStatus status =
      KitsuNvsHeadroom::preparePairing(platform);
  assert(status.result == NvsHeadroomResult::ReadyReclaimed);
  assert(pairingHeadroomReady(status));
  assert(status.retiredActionReplay);
  assert(status.retiredBluedroid);
  assert(status.stats.freeEntries == 182U);
  assert(pairingUsableEntries(status) == 56U);
  assert(platform.actionCalls == 1U);
  assert(platform.bluedroidCalls == 1U);
  assert(platform.statsCalls == 1U);
}

void testThresholdIsFailClosed() {
  MemoryPlatform exact;
  exact.stats.freeEntries = kPairingRequiredFreeEntries;
  const NvsHeadroomStatus ready = KitsuNvsHeadroom::preparePairing(exact);
  assert(ready.result == NvsHeadroomResult::ReadyAlreadyClean);
  assert(pairingHeadroomReady(ready));
  assert(pairingUsableEntries(ready) == kPairingRequiredUsableEntries);

  MemoryPlatform oneShort;
  oneShort.stats.freeEntries = kPairingRequiredFreeEntries - 1U;
  const NvsHeadroomStatus blocked =
      KitsuNvsHeadroom::preparePairing(oneShort);
  assert(blocked.result == NvsHeadroomResult::Insufficient);
  assert(!pairingHeadroomReady(blocked));
  assert(pairingUsableEntries(blocked) ==
         kPairingRequiredUsableEntries - 1U);

  MemoryPlatform noUsableEntries;
  noUsableEntries.stats.freeEntries = kNvsEntriesPerPage;
  const NvsHeadroomStatus empty =
      KitsuNvsHeadroom::preparePairing(noUsableEntries);
  assert(empty.result == NvsHeadroomResult::Insufficient);
  assert(pairingUsableEntries(empty) == 0U);
}

void testCleanupAndStatsFailuresStopClosed() {
  MemoryPlatform actionFailure;
  actionFailure.failAction = true;
  const NvsHeadroomStatus action =
      KitsuNvsHeadroom::preparePairing(actionFailure);
  assert(action.result ==
         NvsHeadroomResult::RetiredActionReplayCleanupFailed);
  assert(!pairingHeadroomReady(action));
  assert(actionFailure.bluedroidCalls == 0U);
  assert(actionFailure.statsCalls == 0U);

  MemoryPlatform bluedroidFailure;
  bluedroidFailure.failBluedroid = true;
  const NvsHeadroomStatus bluedroid =
      KitsuNvsHeadroom::preparePairing(bluedroidFailure);
  assert(bluedroid.result ==
         NvsHeadroomResult::RetiredBluedroidCleanupFailed);
  assert(!pairingHeadroomReady(bluedroid));
  assert(bluedroidFailure.statsCalls == 0U);

  MemoryPlatform statsFailure;
  statsFailure.failStats = true;
  const NvsHeadroomStatus stats =
      KitsuNvsHeadroom::preparePairing(statsFailure);
  assert(stats.result == NvsHeadroomResult::StatsFailed);
  assert(!pairingHeadroomReady(stats));
}

void testCleanupIsIdempotent() {
  MemoryPlatform platform;
  platform.actionPresent = true;
  platform.bluedroidPresent = true;
  const NvsHeadroomStatus first = KitsuNvsHeadroom::preparePairing(platform);
  const NvsHeadroomStatus second = KitsuNvsHeadroom::preparePairing(platform);
  assert(first.result == NvsHeadroomResult::ReadyReclaimed);
  assert(second.result == NvsHeadroomResult::ReadyAlreadyClean);
  assert(!second.retiredActionReplay);
  assert(!second.retiredBluedroid);
  assert(second.stats.freeEntries == first.stats.freeEntries);
  assert(platform.actionCalls == 2U);
  assert(platform.bluedroidCalls == 2U);
  assert(platform.statsCalls == 2U);
}

void testStableResultNames() {
  assert(strcmp(nvsHeadroomResultName(
                    NvsHeadroomResult::ReadyAlreadyClean),
                "ready-already-clean") == 0);
  assert(strcmp(nvsHeadroomResultName(NvsHeadroomResult::ReadyReclaimed),
                "ready-reclaimed") == 0);
  assert(strcmp(nvsHeadroomResultName(NvsHeadroomResult::Insufficient),
                "insufficient") == 0);
}

}  // namespace

int main() {
  testPinnedEntryBudgets();
  testBackupShapeReclaimsOnlyRetiredRecords();
  testThresholdIsFailClosed();
  testCleanupAndStatsFailuresStopClosed();
  testCleanupIsIdempotent();
  testStableResultNames();
  return 0;
}
