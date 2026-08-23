#include "../src/kitsu_rx_rearm_policy.h"

#include <assert.h>

namespace {

using kitsu868::mesh::RxRearmEvidence;
using kitsu868::mesh::rxRearmPhysicallyConfirmed;
using kitsu868::mesh::shouldRetryRxRearm;
using kitsu868::mesh::sx126xChipMode;
using kitsu868::mesh::sx126xStatusIsRx;

RxRearmEvidence evidence(bool attempted, int16_t code, bool software,
                         bool statusAvailable, uint8_t status) {
  RxRearmEvidence result{};
  result.startAttempted = attempted;
  result.startCode = code;
  result.softwareRx = software;
  result.chipStatusAvailable = statusAvailable;
  result.chipStatus = status;
  return result;
}

}  // namespace

int main() {
  static_assert(sx126xChipMode(0x50U) == 5U,
                "SX126x RX mode extraction changed");
  static_assert(sx126xStatusIsRx(0x52U),
                "command-status bits must not hide RX mode");
  static_assert(!sx126xStatusIsRx(0x20U),
                "standby must not be accepted as RX");

  const RxRearmEvidence confirmed = evidence(true, 0, true, true, 0x52U);
  assert(rxRearmPhysicallyConfirmed(confirmed));
  assert(!shouldRetryRxRearm(confirmed, true));

  // Every conjunct is proof-bearing; removing any one fails confirmation.
  assert(!rxRearmPhysicallyConfirmed(evidence(false, 0, true, true, 0x52U)));
  assert(!rxRearmPhysicallyConfirmed(evidence(true, -2, true, true, 0x52U)));
  assert(!rxRearmPhysicallyConfirmed(evidence(true, 0, false, true, 0x52U)));
  assert(!rxRearmPhysicallyConfirmed(evidence(true, 0, true, false, 0x00U)));
  assert(!rxRearmPhysicallyConfirmed(evidence(true, 0, true, true, 0x22U)));

  // DIO1 high forbids the destructive retry for every failure shape.
  assert(!shouldRetryRxRearm(evidence(true, -2, false, true, 0x22U), false));

  // An unavailable probe alone is not a retry trigger after a successful
  // start and matching software state.
  assert(!shouldRetryRxRearm(evidence(true, 0, true, false, 0x00U), true));
  assert(!shouldRetryRxRearm(evidence(true, 0, true, false, 0xFFU), true));

  // Actual start/software failures and valid non-RX status are positive
  // evidence and allow exactly the caller's one guarded retry.
  assert(shouldRetryRxRearm(evidence(true, -2, true, false, 0x00U), true));
  assert(shouldRetryRxRearm(evidence(true, 0, false, false, 0x00U), true));
  assert(shouldRetryRxRearm(evidence(true, 0, true, true, 0x22U), true));
  assert(shouldRetryRxRearm(evidence(false, 0, true, true, 0x22U), true));
  return 0;
}
