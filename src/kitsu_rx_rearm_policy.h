#pragma once

#include <stdint.h>

namespace kitsu868 {
namespace mesh {

constexpr uint8_t kSx126xChipModeMask = 0x70U;
constexpr uint8_t kSx126xChipModeRx = 0x50U;

struct RxRearmEvidence {
  bool startAttempted = false;
  int16_t startCode = 0;
  bool softwareRx = false;
  bool chipStatusAvailable = false;
  uint8_t chipStatus = 0U;
};

constexpr uint8_t sx126xChipMode(uint8_t status) {
  return static_cast<uint8_t>((status & kSx126xChipModeMask) >> 4U);
}

constexpr bool sx126xStatusIsRx(uint8_t status) {
  return (status & kSx126xChipModeMask) == kSx126xChipModeRx;
}

// A second startReceive is destructive if an RX_DONE is already asserted.
// Permit at most one retry only while DIO1 is low and there is positive
// failure evidence. Status unavailability alone is deliberately not failure
// evidence: retrying because a probe failed could erase a frame that arrived
// between the DIO sample and the SPI transaction.
constexpr bool shouldRetryRxRearm(const RxRearmEvidence& evidence,
                                  bool dio1Low) {
  return dio1Low &&
      ((evidence.startAttempted && evidence.startCode != 0) ||
       !evidence.softwareRx ||
       (evidence.chipStatusAvailable &&
        !sx126xStatusIsRx(evidence.chipStatus)));
}

// "RX ready" is intentionally conjunctive. Neither RadioLib's software flag
// nor an SX126x status byte is accepted alone, and an inherited software RX
// flag without this rearm's successful start is never physical proof.
constexpr bool rxRearmPhysicallyConfirmed(const RxRearmEvidence& evidence) {
  return evidence.startAttempted && evidence.startCode == 0 &&
      evidence.softwareRx && evidence.chipStatusAvailable &&
      sx126xStatusIsRx(evidence.chipStatus);
}

}  // namespace mesh
}  // namespace kitsu868
