#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kitsu_repeat_wire.h"

namespace kitsu868 {
namespace mesh {

// MeshCore Packet::calculatePacketHash() emits exactly eight bytes.  The
// digest covers payload type + encrypted payload and deliberately excludes
// the flood path, so a repeater's copy keeps the original digest while its
// path count grows.
constexpr size_t kChannelRepeatHashBytes = 8U;
constexpr size_t kChannelRepeatDigestBytes = 32U;
constexpr uint8_t kChannelGroupTextPayloadType = 0x05U;
constexpr uint8_t kChannelRepeatSlotCount = 4U;
// Match the complete app journal so every still-visible sent row can retain
// its own lifecycle window. When full, a new send reports observation
// unavailable instead of silently evicting an older open row.
constexpr size_t kChannelRepeatTrackerCapacity = 24U;
constexpr uint32_t kChannelRepeatWindowMs = 120000UL;
constexpr uint8_t kChannelRepeatSourceCapacity = 4U;
constexpr uint8_t kChannelRepeatSourceTokenBytes = 3U;

struct ChannelRepeatSource {
  uint8_t tokenBytes = 0U;
  uint8_t token[kChannelRepeatSourceTokenBytes]{};
};

struct ChannelRepeatObservation {
  uint8_t channelSlot = 0xffU;
  uint32_t messageTimestamp = 0U;
  uint8_t repeatCount = 0U;
  // True while the bounded local RF observation window remains open. A
  // false transition retains the final count and lets the journal distinguish
  // "still listening" from a completed zero-result observation.
  bool observationOpen = false;
  uint8_t sourceCount = 0U;
  ChannelRepeatSource sources[kChannelRepeatSourceCapacity]{};
  bool sourcesTruncated = false;
};

enum class ChannelRepeatObserveResult : uint8_t {
  NotCandidate = 0,
  NoHashMatch,
  WireMismatch,
  DigestMismatch,
  Recorded,
  Saturated,
};

// Full-strength companion to MeshCore's intentionally truncated packet hash.
// Both are derived from exactly the payload-type byte followed by immutable
// payload bytes. Route and path are intentionally absent so a forwarded copy
// remains correlatable after a repeater appends its path hash.
bool calculateChannelRepeatDigest(
    uint8_t payloadType, const uint8_t* payload, size_t payloadBytes,
    uint8_t digest[kChannelRepeatDigestBytes]);

// Counts received rebroadcast copies of recently sent channel packets.  This
// is intentionally not a delivery receipt and not a unique-repeater counter:
// one repeater can produce more than one observable copy, while a repeater
// outside this Kitsu's receive range can forward the packet without its copy
// ever returning here.
class ChannelRepeatTracker {
 public:
  bool recordSent(uint8_t payloadType,
                  const FloodRouteBinding& route,
                  const uint8_t hash[kChannelRepeatHashBytes],
                  const uint8_t digest[kChannelRepeatDigestBytes],
                  uint8_t channelSlot, uint32_t messageTimestamp,
                  uint32_t nowMs);

  // Returns true only when a matching observation advances the saturating
  // count.  Calling this from MeshCore's logRx hook is essential because the
  // normal receive pipeline deliberately removes duplicate packet hashes.
  ChannelRepeatObserveResult observeDetailed(
      uint8_t payloadType, const FloodRouteBinding& route,
      uint8_t pathCount,
      const uint8_t hash[kChannelRepeatHashBytes],
      const uint8_t digest[kChannelRepeatDigestBytes], uint32_t nowMs,
      ChannelRepeatObservation& output);
  ChannelRepeatObserveResult observeDetailed(
      uint8_t payloadType, const FloodRouteBinding& route,
      uint8_t pathCount,
      const uint8_t hash[kChannelRepeatHashBytes],
      const uint8_t digest[kChannelRepeatDigestBytes],
      const uint8_t* lastHopToken, uint8_t lastHopTokenBytes, uint32_t nowMs,
      ChannelRepeatObservation& output);
  bool observe(uint8_t payloadType, const FloodRouteBinding& route,
               uint8_t pathCount,
               const uint8_t hash[kChannelRepeatHashBytes],
               const uint8_t digest[kChannelRepeatDigestBytes],
               uint32_t nowMs, ChannelRepeatObservation& output) {
    return observeDetailed(payloadType, route, pathCount, hash, digest,
                           nowMs, output) ==
        ChannelRepeatObserveResult::Recorded;
  }

  bool lookup(uint8_t channelSlot, uint32_t messageTimestamp,
              uint32_t nowMs, ChannelRepeatObservation& output);
  // Coalesces any burst for a tracked send into one cumulative transition.
  // A caller that drains later receives the newest count, not an arbitrary
  // earlier event from a bounded lifecycle queue.
  bool takeDirty(ChannelRepeatObservation& output);
  size_t activeCount(uint32_t nowMs);
  void expire(uint32_t nowMs);
  // Closes every active window and retains one dirty final transition per
  // entry for the journal consumer (for example on a radio-profile change).
  void closeAll();
  void clear();

 private:
  struct Entry {
    bool active = false;
    uint8_t hash[kChannelRepeatHashBytes]{};
    uint8_t digest[kChannelRepeatDigestBytes]{};
    FloodRouteBinding route{};
    uint8_t channelSlot = 0xffU;
    uint32_t messageTimestamp = 0U;
    uint32_t sentAtMs = 0U;
    uint8_t repeatCount = 0U;
    uint8_t sourceCount = 0U;
    ChannelRepeatSource sources[kChannelRepeatSourceCapacity]{};
    bool sourcesTruncated = false;
    bool dirty = false;
    bool closePending = false;
  };

  Entry entries_[kChannelRepeatTrackerCapacity]{};

  static void fillObservation(const Entry& entry,
                              ChannelRepeatObservation& output);
  static bool recordSource(Entry& entry, const uint8_t* token,
                           uint8_t tokenBytes);
};

}  // namespace mesh
}  // namespace kitsu868
