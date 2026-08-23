#include "kitsu_channel_repeat_tracker.h"

#include <SHA256.h>

#include <string.h>

namespace kitsu868 {
namespace mesh {

namespace {

bool sameHash(const uint8_t* left, const uint8_t* right) {
  return left && right &&
      memcmp(left, right, kChannelRepeatHashBytes) == 0;
}

bool sameDigest(const uint8_t* left, const uint8_t* right) {
  return left && right &&
      memcmp(left, right, kChannelRepeatDigestBytes) == 0;
}

}  // namespace

bool calculateChannelRepeatDigest(
    uint8_t payloadType, const uint8_t* payload, size_t payloadBytes,
    uint8_t digest[kChannelRepeatDigestBytes]) {
  if (!digest || (!payload && payloadBytes != 0U)) return false;
  SHA256 sha;
  sha.update(&payloadType, 1U);
  if (payloadBytes != 0U) sha.update(payload, payloadBytes);
  sha.finalize(digest, kChannelRepeatDigestBytes);
  return true;
}

bool ChannelRepeatTracker::recordSent(
    uint8_t payloadType, const FloodRouteBinding& route,
    const uint8_t hash[kChannelRepeatHashBytes],
    const uint8_t digest[kChannelRepeatDigestBytes], uint8_t channelSlot,
    uint32_t messageTimestamp, uint32_t nowMs) {
  if (payloadType != kChannelGroupTextPayloadType ||
      !validFloodRouteBinding(route) || !hash || !digest ||
      channelSlot >= kChannelRepeatSlotCount || messageTimestamp == 0U) {
    return false;
  }

  expire(nowMs);

  Entry* selected = nullptr;
  for (Entry& entry : entries_) {
    if (entry.active && entry.channelSlot == channelSlot &&
        entry.messageTimestamp == messageTimestamp) {
      // logTx is expected once, but make an identical callback idempotent so
      // it can never erase copies that were already observed.
      if (sameHash(entry.hash, hash) && sameDigest(entry.digest, digest) &&
          sameFloodRouteBinding(entry.route, route)) {
        return true;
      }
      selected = &entry;
      break;
    }
    if (!entry.active && !entry.dirty && !selected) selected = &entry;
  }
  // Never evict an open lifecycle: doing so would leave its app journal row
  // permanently "listening" with no possible close transition.
  if (!selected) return false;

  *selected = Entry{};
  selected->active = true;
  memcpy(selected->hash, hash, kChannelRepeatHashBytes);
  memcpy(selected->digest, digest, kChannelRepeatDigestBytes);
  selected->route = route;
  selected->channelSlot = channelSlot;
  selected->messageTimestamp = messageTimestamp;
  selected->sentAtMs = nowMs;
  return true;
}

ChannelRepeatObserveResult ChannelRepeatTracker::observeDetailed(
    uint8_t payloadType, const FloodRouteBinding& route, uint8_t pathCount,
    const uint8_t hash[kChannelRepeatHashBytes],
    const uint8_t digest[kChannelRepeatDigestBytes], uint32_t nowMs,
    ChannelRepeatObservation& output) {
  return observeDetailed(payloadType, route, pathCount, hash, digest, nullptr,
                         0U, nowMs, output);
}

ChannelRepeatObserveResult ChannelRepeatTracker::observeDetailed(
    uint8_t payloadType, const FloodRouteBinding& route, uint8_t pathCount,
    const uint8_t hash[kChannelRepeatHashBytes],
    const uint8_t digest[kChannelRepeatDigestBytes],
    const uint8_t* lastHopToken, uint8_t lastHopTokenBytes, uint32_t nowMs,
    ChannelRepeatObservation& output) {
  output = ChannelRepeatObservation{};
  expire(nowMs);
  if (payloadType != kChannelGroupTextPayloadType ||
      !validFloodRouteBinding(route) || pathCount == 0U || !hash ||
      !digest) {
    return ChannelRepeatObserveResult::NotCandidate;
  }

  Entry* selected = nullptr;
  uint32_t selectedAge = 0xffffffffUL;
  bool hashMatched = false;
  bool wireMismatch = false;
  for (Entry& entry : entries_) {
    if (!entry.active || !sameHash(entry.hash, hash)) continue;
    hashMatched = true;
    if (!sameFloodRouteBinding(entry.route, route)) {
      // Only a full-digest match is strong enough to diagnose a route/code
      // mismatch instead of an unrelated 64-bit packet-hash collision.
      wireMismatch = wireMismatch || sameDigest(entry.digest, digest);
      continue;
    }
    if (!sameDigest(entry.digest, digest)) continue;
    // Correlation requires both MeshCore's exact 64-bit packet hash and the
    // retained full 256-bit digest. If duplicate correlation material still
    // exists inside the bounded window, mutate only the most recent send.
    const uint32_t age = nowMs - entry.sentAtMs;
    if (!selected || age < selectedAge) {
      selected = &entry;
      selectedAge = age;
    }
  }
  if (!selected) {
    if (wireMismatch) return ChannelRepeatObserveResult::WireMismatch;
    return hashMatched ? ChannelRepeatObserveResult::DigestMismatch
                       : ChannelRepeatObserveResult::NoHashMatch;
  }
  const bool sourceChanged =
      recordSource(*selected, lastHopToken, lastHopTokenBytes);
  fillObservation(*selected, output);
  if (selected->repeatCount == 0xffU) {
    if (sourceChanged) selected->dirty = true;
    fillObservation(*selected, output);
    return ChannelRepeatObserveResult::Saturated;
  }

  ++selected->repeatCount;
  selected->dirty = true;
  fillObservation(*selected, output);
  return ChannelRepeatObserveResult::Recorded;
}

bool ChannelRepeatTracker::lookup(uint8_t channelSlot,
                                  uint32_t messageTimestamp,
                                  uint32_t nowMs,
                                  ChannelRepeatObservation& output) {
  output = ChannelRepeatObservation{};
  expire(nowMs);
  for (const Entry& entry : entries_) {
    if (entry.active && entry.channelSlot == channelSlot &&
        entry.messageTimestamp == messageTimestamp) {
      fillObservation(entry, output);
      return true;
    }
  }
  return false;
}

bool ChannelRepeatTracker::takeDirty(ChannelRepeatObservation& output) {
  output = ChannelRepeatObservation{};
  Entry* selected = nullptr;
  for (Entry& entry : entries_) {
    if (!entry.dirty) continue;
    if (!selected ||
        static_cast<int32_t>(entry.messageTimestamp -
                             selected->messageTimestamp) < 0) {
      selected = &entry;
    }
  }
  if (!selected) return false;
  selected->dirty = false;
  fillObservation(*selected, output);
  if (selected->closePending) {
    *selected = Entry{};
  }
  return true;
}

size_t ChannelRepeatTracker::activeCount(uint32_t nowMs) {
  expire(nowMs);
  size_t count = 0U;
  for (const Entry& entry : entries_) count += entry.active ? 1U : 0U;
  return count;
}

void ChannelRepeatTracker::expire(uint32_t nowMs) {
  for (Entry& entry : entries_) {
    if (entry.active && nowMs - entry.sentAtMs >= kChannelRepeatWindowMs) {
      // Preserve the identity/final count until the lifecycle consumer drains
      // one explicit close transition. This makes a zero result distinguishable
      // from an observation that is merely still in progress.
      entry.active = false;
      entry.dirty = true;
      entry.closePending = true;
    }
  }
}

void ChannelRepeatTracker::closeAll() {
  for (Entry& entry : entries_) {
    if (!entry.active) continue;
    entry.active = false;
    entry.dirty = true;
    entry.closePending = true;
  }
}

void ChannelRepeatTracker::clear() {
  for (Entry& entry : entries_) entry = Entry{};
}

void ChannelRepeatTracker::fillObservation(
    const Entry& entry, ChannelRepeatObservation& output) {
  output = ChannelRepeatObservation{};
  output.channelSlot = entry.channelSlot;
  output.messageTimestamp = entry.messageTimestamp;
  output.repeatCount = entry.repeatCount;
  output.observationOpen = entry.active;
  output.sourceCount = entry.sourceCount;
  memcpy(output.sources, entry.sources, sizeof(output.sources));
  output.sourcesTruncated = entry.sourcesTruncated;
}

bool ChannelRepeatTracker::recordSource(Entry& entry, const uint8_t* token,
                                        uint8_t tokenBytes) {
  if (!token || tokenBytes == 0U ||
      tokenBytes > kChannelRepeatSourceTokenBytes) {
    return false;
  }
  for (uint8_t index = 0U; index < entry.sourceCount; ++index) {
    const ChannelRepeatSource& source = entry.sources[index];
    if (source.tokenBytes == tokenBytes &&
        memcmp(source.token, token, tokenBytes) == 0) {
      return false;
    }
  }
  if (entry.sourceCount >= kChannelRepeatSourceCapacity) {
    if (entry.sourcesTruncated) return false;
    entry.sourcesTruncated = true;
    return true;
  }
  ChannelRepeatSource& source = entry.sources[entry.sourceCount++];
  source.tokenBytes = tokenBytes;
  memcpy(source.token, token, tokenBytes);
  return true;
}

}  // namespace mesh
}  // namespace kitsu868
