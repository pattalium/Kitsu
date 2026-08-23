#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kitsu_repeat_wire.h"

namespace kitsu868 {
namespace mesh {

constexpr size_t kAdvertRepeatHashBytes = 8U;
constexpr size_t kAdvertRepeatDigestBytes = 32U;
constexpr uint8_t kAdvertPayloadType = 0x04U;
constexpr uint32_t kAdvertRepeatWindowMs = 120000UL;
constexpr uint32_t kAdvertMinimumEmittedAt = 1704067200UL;
constexpr uint32_t kAdvertMaximumEmittedAt = 4102444800UL;
constexpr uint8_t kAdvertRepeatSourceCapacity = 4U;
constexpr uint8_t kAdvertRepeatSourceTokenBytes = 3U;

struct AdvertRepeatSource {
  uint8_t tokenBytes = 0U;
  uint8_t token[kAdvertRepeatSourceTokenBytes]{};
};

enum class AdvertTransmitState : uint8_t {
  Queued = 0,
  Sent = 1,
  TxFailed = 2,
};

enum class AdvertRepeatObserveResult : uint8_t {
  NotCandidate = 0,
  NoHashMatch,
  WireMismatch,
  DigestMismatch,
  Recorded,
  Saturated,
};

// Volatile evidence for the most recent owner-requested Mesh-wide advert.
// repeatCount is the number of matching rebroadcast packet copies heard by
// this Kitsu, never a unique-repeater count or a delivery receipt.
struct FloodAdvertStatus {
  bool available = false;
  uint32_t emittedAt = 0U;
  AdvertTransmitState state = AdvertTransmitState::Queued;
  bool repeatCountKnown = false;
  uint8_t repeatCount = 0U;
  bool observationOpen = false;
  uint8_t sourceCount = 0U;
  AdvertRepeatSource sources[kAdvertRepeatSourceCapacity]{};
  bool sourcesTruncated = false;
};

// Volatile lifecycle for the most recent owner-requested zero-hop advert.
// Nearby has no returned-copy observation window: this records only whether
// the exact packet was queued, physically transmitted, or failed before TX.
struct NearbyAdvertStatus {
  bool available = false;
  uint32_t emittedAt = 0U;
  AdvertTransmitState state = AdvertTransmitState::Queued;
};

class NearbyAdvertTracker {
 public:
  bool recordQueued(uint8_t payloadType, bool flood, uint32_t emittedAt);
  bool markSent(uint32_t emittedAt);
  bool markTxFailed(uint32_t emittedAt);
  bool snapshot(NearbyAdvertStatus& output) const;
  bool takeDirty();
  void clear();

 private:
  NearbyAdvertStatus status_{};
  bool dirty_ = false;
};

// Retains one Mesh-wide lifecycle record while bounding RF echo correlation
// to a two-minute window after actual physical transmission. Nearby/direct
// adverts are rejected without replacing the retained Mesh-wide record.
class AdvertRepeatTracker {
 public:
  bool recordQueued(uint8_t payloadType, bool flood, uint32_t emittedAt);
  bool markSent(uint8_t payloadType, const FloodRouteBinding& route,
                const uint8_t hash[kAdvertRepeatHashBytes],
                const uint8_t digest[kAdvertRepeatDigestBytes],
                uint32_t emittedAt, uint32_t nowMs);
  bool markTxFailed(uint32_t emittedAt);

  // Returns true only when a matching pre-dedup RF observation increments the
  // saturating count. A path count of zero is the original packet form and is
  // not repeat evidence.
  bool observe(uint8_t payloadType, const FloodRouteBinding& route,
               uint8_t pathCount,
               const uint8_t hash[kAdvertRepeatHashBytes],
               const uint8_t digest[kAdvertRepeatDigestBytes],
               uint32_t nowMs) {
    return observeDetailed(payloadType, route, pathCount, hash, digest,
                           nowMs) == AdvertRepeatObserveResult::Recorded;
  }
  AdvertRepeatObserveResult observeDetailed(
      uint8_t payloadType, const FloodRouteBinding& route,
      uint8_t pathCount,
      const uint8_t hash[kAdvertRepeatHashBytes],
      const uint8_t digest[kAdvertRepeatDigestBytes], uint32_t nowMs);
  AdvertRepeatObserveResult observeDetailed(
      uint8_t payloadType, const FloodRouteBinding& route,
      uint8_t pathCount,
      const uint8_t hash[kAdvertRepeatHashBytes],
      const uint8_t digest[kAdvertRepeatDigestBytes],
      const uint8_t* lastHopToken, uint8_t lastHopTokenBytes,
      uint32_t nowMs);

  // Closes an elapsed observation window but retains the final count.
  bool tick(uint32_t nowMs);
  bool snapshot(uint32_t nowMs, FloodAdvertStatus& output);

  // Coalesces lifecycle/count/window changes for the companion refresh path.
  bool takeDirty();

  // Clears profile/identity-local evidence. If a record existed, the removal
  // itself remains dirty so the companion can remove stale UI state.
  void clear();

 private:
  FloodAdvertStatus status_{};
  uint8_t hash_[kAdvertRepeatHashBytes]{};
  uint8_t digest_[kAdvertRepeatDigestBytes]{};
  FloodRouteBinding route_{};
  uint32_t sentAtMs_ = 0U;
  bool fingerprintValid_ = false;
  bool dirty_ = false;

  void clearFingerprint();
  bool recordSource(const uint8_t* token, uint8_t tokenBytes);
};

}  // namespace mesh
}  // namespace kitsu868
