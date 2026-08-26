#include <assert.h>
#include <stdint.h>

#include "../src/kitsu_ble_bond_recovery.h"

using kitsu868::connectivity::BleBondRecoveryOutcome;
using kitsu868::connectivity::ControllerAuthoritySnapshot;
using kitsu868::connectivity::clearControllerAuthoritySnapshot;
using kitsu868::connectivity::controllerAuthoritiesUnchanged;
using kitsu868::connectivity::evaluateBleBondRecovery;

namespace {

ControllerAuthoritySnapshot fixture() {
  ControllerAuthoritySnapshot snapshot{};
  snapshot.controllerCount = 4U;
  for (size_t slot = 0U;
       slot < kitsu868::connectivity::kKitsuControllerCapacity; ++slot) {
    snapshot.occupied[slot] = true;
    for (size_t i = 0U;
         i < kitsu868::connectivity::kKitsuControllerIdBytes; ++i) {
      snapshot.ids[slot][i] = static_cast<uint8_t>(slot * 17U + i);
    }
    for (size_t i = 0U; i < kitsu868::connectivity::kKitsuSecretBytes; ++i) {
      snapshot.roots[slot][i] = static_cast<uint8_t>(slot * 31U + i + 1U);
    }
  }
  return snapshot;
}

}  // namespace

int main() {
  ControllerAuthoritySnapshot before = fixture();
  ControllerAuthoritySnapshot after = before;
  assert(controllerAuthoritiesUnchanged(before, after));
  assert(evaluateBleBondRecovery(true, 3, 0, before, after) ==
         BleBondRecoveryOutcome::Cleared);
  assert(evaluateBleBondRecovery(false, 3, 0, before, after) ==
         BleBondRecoveryOutcome::BondStoreError);
  assert(evaluateBleBondRecovery(true, 3, 1, before, after) ==
         BleBondRecoveryOutcome::BondStoreError);

  after.roots[2][11] ^= 0x80U;
  assert(!controllerAuthoritiesUnchanged(before, after));
  assert(evaluateBleBondRecovery(true, 3, 0, before, after) ==
         BleBondRecoveryOutcome::ControllerAuthorityChanged);

  clearControllerAuthoritySnapshot(before);
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&before);
  for (size_t i = 0U; i < sizeof(before); ++i) assert(bytes[i] == 0U);
  clearControllerAuthoritySnapshot(after);
  return 0;
}
