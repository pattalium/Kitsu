#include "kitsu_advert_repeat_tracker.h"

#include <string.h>

namespace kitsu868 {
namespace mesh {

namespace {

bool validAdvert(uint8_t payloadType, bool flood, uint32_t emittedAt) {
  return payloadType == kAdvertPayloadType && flood &&
      emittedAt >= kAdvertMinimumEmittedAt &&
      emittedAt <= kAdvertMaximumEmittedAt;
}

bool sameBytes(const uint8_t* left, const uint8_t* right, size_t bytes) {
  return left && right && memcmp(left, right, bytes) == 0;
}

bool validNearbyAdvert(uint8_t payloadType, bool flood, uint32_t emittedAt) {
  return payloadType == kAdvertPayloadType && !flood &&
      emittedAt >= kAdvertMinimumEmittedAt &&
      emittedAt <= kAdvertMaximumEmittedAt;
}

}  // namespace

bool NearbyAdvertTracker::recordQueued(uint8_t payloadType, bool flood,
                                       uint32_t emittedAt) {
  if (!validNearbyAdvert(payloadType, flood, emittedAt)) return false;
  status_ = NearbyAdvertStatus{};
  status_.available = true;
  status_.emittedAt = emittedAt;
  status_.state = AdvertTransmitState::Queued;
  dirty_ = true;
  return true;
}

bool NearbyAdvertTracker::markSent(uint32_t emittedAt) {
  if (!status_.available || status_.state != AdvertTransmitState::Queued ||
      status_.emittedAt != emittedAt) {
    return false;
  }
  status_.state = AdvertTransmitState::Sent;
  dirty_ = true;
  return true;
}

bool NearbyAdvertTracker::markTxFailed(uint32_t emittedAt) {
  if (!status_.available || status_.state != AdvertTransmitState::Queued ||
      status_.emittedAt != emittedAt) {
    return false;
  }
  status_.state = AdvertTransmitState::TxFailed;
  dirty_ = true;
  return true;
}

bool NearbyAdvertTracker::snapshot(NearbyAdvertStatus& output) const {
  output = status_;
  return status_.available;
}

bool NearbyAdvertTracker::takeDirty() {
  const bool output = dirty_;
  dirty_ = false;
  return output;
}

void NearbyAdvertTracker::clear() {
  const bool changed = status_.available;
  status_ = NearbyAdvertStatus{};
  if (changed) dirty_ = true;
}

bool AdvertRepeatTracker::recordQueued(uint8_t payloadType, bool flood,
                                       uint32_t emittedAt) {
  if (!validAdvert(payloadType, flood, emittedAt)) return false;

  status_ = FloodAdvertStatus{};
  status_.available = true;
  status_.emittedAt = emittedAt;
  status_.state = AdvertTransmitState::Queued;
  clearFingerprint();
  dirty_ = true;
  return true;
}

bool AdvertRepeatTracker::markSent(
    uint8_t payloadType, const FloodRouteBinding& route,
    const uint8_t hash[kAdvertRepeatHashBytes],
    const uint8_t digest[kAdvertRepeatDigestBytes], uint32_t emittedAt,
    uint32_t nowMs) {
  if (!validAdvert(payloadType, true, emittedAt) ||
      !validFloodRouteBinding(route) || !hash || !digest ||
      !status_.available || status_.state != AdvertTransmitState::Queued ||
      status_.emittedAt != emittedAt) {
    return false;
  }

  memcpy(hash_, hash, sizeof(hash_));
  memcpy(digest_, digest, sizeof(digest_));
  route_ = route;
  sentAtMs_ = nowMs;
  fingerprintValid_ = true;
  status_.state = AdvertTransmitState::Sent;
  status_.repeatCountKnown = true;
  status_.repeatCount = 0U;
  status_.observationOpen = true;
  dirty_ = true;
  return true;
}

bool AdvertRepeatTracker::markTxFailed(uint32_t emittedAt) {
  if (!status_.available || status_.state != AdvertTransmitState::Queued ||
      status_.emittedAt != emittedAt) {
    return false;
  }

  status_.state = AdvertTransmitState::TxFailed;
  status_.repeatCountKnown = false;
  status_.repeatCount = 0U;
  status_.observationOpen = false;
  clearFingerprint();
  dirty_ = true;
  return true;
}

AdvertRepeatObserveResult AdvertRepeatTracker::observeDetailed(
    uint8_t payloadType, const FloodRouteBinding& route, uint8_t pathCount,
    const uint8_t hash[kAdvertRepeatHashBytes],
    const uint8_t digest[kAdvertRepeatDigestBytes], uint32_t nowMs) {
  return observeDetailed(payloadType, route, pathCount, hash, digest, nullptr,
                         0U, nowMs);
}

AdvertRepeatObserveResult AdvertRepeatTracker::observeDetailed(
    uint8_t payloadType, const FloodRouteBinding& route, uint8_t pathCount,
    const uint8_t hash[kAdvertRepeatHashBytes],
    const uint8_t digest[kAdvertRepeatDigestBytes],
    const uint8_t* lastHopToken, uint8_t lastHopTokenBytes,
    uint32_t nowMs) {
  (void)tick(nowMs);
  if (payloadType != kAdvertPayloadType ||
      !validFloodRouteBinding(route) || pathCount == 0U || !hash ||
      !digest) {
    return AdvertRepeatObserveResult::NotCandidate;
  }
  if (!status_.available || status_.state != AdvertTransmitState::Sent ||
      !status_.observationOpen || !status_.repeatCountKnown ||
      !fingerprintValid_ || !sameBytes(hash_, hash, sizeof(hash_))) {
    return AdvertRepeatObserveResult::NoHashMatch;
  }
  if (!sameFloodRouteBinding(route_, route)) {
    return sameBytes(digest_, digest, sizeof(digest_))
        ? AdvertRepeatObserveResult::WireMismatch
        : AdvertRepeatObserveResult::DigestMismatch;
  }
  if (!sameBytes(digest_, digest, sizeof(digest_))) {
    return AdvertRepeatObserveResult::DigestMismatch;
  }
  const bool sourceChanged =
      recordSource(lastHopToken, lastHopTokenBytes);
  if (status_.repeatCount == 0xffU) {
    if (sourceChanged) dirty_ = true;
    return AdvertRepeatObserveResult::Saturated;
  }

  ++status_.repeatCount;
  dirty_ = true;
  return AdvertRepeatObserveResult::Recorded;
}

bool AdvertRepeatTracker::tick(uint32_t nowMs) {
  if (!status_.available || status_.state != AdvertTransmitState::Sent ||
      !status_.observationOpen ||
      nowMs - sentAtMs_ < kAdvertRepeatWindowMs) {
    return false;
  }

  status_.observationOpen = false;
  clearFingerprint();
  dirty_ = true;
  return true;
}

bool AdvertRepeatTracker::snapshot(uint32_t nowMs,
                                   FloodAdvertStatus& output) {
  (void)tick(nowMs);
  output = status_;
  return status_.available;
}

bool AdvertRepeatTracker::takeDirty() {
  const bool output = dirty_;
  dirty_ = false;
  return output;
}

void AdvertRepeatTracker::clear() {
  const bool changed = status_.available;
  status_ = FloodAdvertStatus{};
  clearFingerprint();
  if (changed) dirty_ = true;
}

void AdvertRepeatTracker::clearFingerprint() {
  memset(hash_, 0, sizeof(hash_));
  memset(digest_, 0, sizeof(digest_));
  route_ = FloodRouteBinding{};
  sentAtMs_ = 0U;
  fingerprintValid_ = false;
}

bool AdvertRepeatTracker::recordSource(const uint8_t* token,
                                       uint8_t tokenBytes) {
  if (!token || tokenBytes == 0U ||
      tokenBytes > kAdvertRepeatSourceTokenBytes) {
    return false;
  }
  for (uint8_t index = 0U; index < status_.sourceCount; ++index) {
    const AdvertRepeatSource& source = status_.sources[index];
    if (source.tokenBytes == tokenBytes &&
        memcmp(source.token, token, tokenBytes) == 0) {
      return false;
    }
  }
  if (status_.sourceCount >= kAdvertRepeatSourceCapacity) {
    if (status_.sourcesTruncated) return false;
    status_.sourcesTruncated = true;
    return true;
  }
  AdvertRepeatSource& source = status_.sources[status_.sourceCount++];
  source.tokenBytes = tokenBytes;
  memcpy(source.token, token, tokenBytes);
  return true;
}

}  // namespace mesh
}  // namespace kitsu868
