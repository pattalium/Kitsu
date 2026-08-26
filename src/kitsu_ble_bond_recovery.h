#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kitsu_device_security.h"

namespace kitsu868 {
namespace connectivity {

struct ControllerAuthoritySnapshot {
  uint8_t controllerCount = 0U;
  bool occupied[kKitsuControllerCapacity]{};
  uint8_t ids[kKitsuControllerCapacity][kKitsuControllerIdBytes]{};
  uint8_t roots[kKitsuControllerCapacity][kKitsuSecretBytes]{};
};

inline void clearControllerAuthoritySnapshot(
    ControllerAuthoritySnapshot& snapshot) {
  volatile uint8_t* output =
      reinterpret_cast<volatile uint8_t*>(&snapshot);
  for (size_t i = 0U; i < sizeof(snapshot); ++i) output[i] = 0U;
}

inline bool captureControllerAuthorities(
    const KitsuDeviceSecurity& security,
    ControllerAuthoritySnapshot& snapshot) {
  clearControllerAuthoritySnapshot(snapshot);
  const DeviceSecurityStatus status = security.status();
  if (!status.begun || status.controllerCount > kKitsuControllerCapacity) {
    return false;
  }
  snapshot.controllerCount = status.controllerCount;
  uint8_t occupied = 0U;
  for (size_t slot = 0U; slot < kKitsuControllerCapacity; ++slot) {
    if (!security.controllerAtSlot(slot, snapshot.ids[slot])) continue;
    snapshot.occupied[slot] = true;
    ++occupied;
    if (!security.findControllerRoot(snapshot.ids[slot],
                                     snapshot.roots[slot])) {
      clearControllerAuthoritySnapshot(snapshot);
      return false;
    }
  }
  if (occupied != snapshot.controllerCount) {
    clearControllerAuthoritySnapshot(snapshot);
    return false;
  }
  return true;
}

inline bool controllerAuthoritiesUnchanged(
    const ControllerAuthoritySnapshot& before,
    const ControllerAuthoritySnapshot& after) {
  uint8_t difference =
      static_cast<uint8_t>(before.controllerCount ^ after.controllerCount);
  for (size_t slot = 0U; slot < kKitsuControllerCapacity; ++slot) {
    difference |= static_cast<uint8_t>(before.occupied[slot] ^
                                       after.occupied[slot]);
    for (size_t i = 0U; i < kKitsuControllerIdBytes; ++i) {
      difference |= static_cast<uint8_t>(before.ids[slot][i] ^
                                         after.ids[slot][i]);
    }
    for (size_t i = 0U; i < kKitsuSecretBytes; ++i) {
      difference |= static_cast<uint8_t>(before.roots[slot][i] ^
                                         after.roots[slot][i]);
    }
  }
  return difference == 0U;
}

enum class BleBondRecoveryOutcome : uint8_t {
  Cleared = 0,
  BondStoreError,
  ControllerAuthorityChanged,
  InvalidSnapshot,
};

inline BleBondRecoveryOutcome evaluateBleBondRecovery(
    bool deleteSucceeded, int bondsBefore, int bondsAfter,
    const ControllerAuthoritySnapshot& before,
    const ControllerAuthoritySnapshot& after) {
  if (bondsBefore < 0 || bondsAfter < 0) {
    return BleBondRecoveryOutcome::BondStoreError;
  }
  if (!controllerAuthoritiesUnchanged(before, after)) {
    return BleBondRecoveryOutcome::ControllerAuthorityChanged;
  }
  return deleteSucceeded && bondsAfter == 0
      ? BleBondRecoveryOutcome::Cleared
      : BleBondRecoveryOutcome::BondStoreError;
}

}  // namespace connectivity
}  // namespace kitsu868
