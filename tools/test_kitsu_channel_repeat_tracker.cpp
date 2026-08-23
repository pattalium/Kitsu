#include "../src/kitsu_channel_repeat_tracker.h"

#include <Packet.h>

#include <assert.h>
#include <string.h>

namespace {

using kitsu868::mesh::ChannelRepeatObservation;
using kitsu868::mesh::ChannelRepeatObserveResult;
using kitsu868::mesh::ChannelRepeatTracker;
using kitsu868::mesh::FloodRouteBinding;
using kitsu868::mesh::kChannelGroupTextPayloadType;
using kitsu868::mesh::kChannelRepeatDigestBytes;
using kitsu868::mesh::kChannelRepeatHashBytes;
using kitsu868::mesh::kChannelRepeatTrackerCapacity;
using kitsu868::mesh::kChannelRepeatWindowMs;

struct Fingerprint {
  uint8_t hash[kChannelRepeatHashBytes]{};
  uint8_t digest[kChannelRepeatDigestBytes]{};
};

Fingerprint fingerprint(const mesh::Packet& packet) {
  Fingerprint output{};
  packet.calculatePacketHash(output.hash);
  assert(kitsu868::mesh::calculateChannelRepeatDigest(
      packet.getPayloadType(), packet.payload, packet.payload_len,
      output.digest));
  // For GRP_TXT, MeshCore's exact eight-byte packet hash is the prefix of the
  // full SHA-256 over the same payload-type byte + immutable payload bytes.
  assert(memcmp(output.hash, output.digest, kChannelRepeatHashBytes) == 0);
  return output;
}

mesh::Packet channelPacket(uint8_t marker) {
  mesh::Packet packet;
  packet.header = static_cast<uint8_t>(
      (kChannelGroupTextPayloadType << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD);
  packet.setPathHashSizeAndCount(1U, 0U);
  packet.payload_len = 20U;
  for (uint8_t index = 0U; index < packet.payload_len; ++index) {
    packet.payload[index] = static_cast<uint8_t>(marker + index);
  }
  return packet;
}

FloodRouteBinding routeFor(const mesh::Packet& packet) {
  FloodRouteBinding output{};
  if (!packet.isRouteFlood()) return output;
  output.transportScoped = packet.hasTransportCodes();
  output.pathHashSize = packet.getPathHashSize();
  if (output.transportScoped) {
    output.transportCodes[0] = packet.transport_codes[0];
    output.transportCodes[1] = packet.transport_codes[1];
  }
  return output;
}

bool observe(ChannelRepeatTracker& tracker, const mesh::Packet& packet,
             uint32_t now, ChannelRepeatObservation& output) {
  const Fingerprint fp = fingerprint(packet);
  return tracker.observe(packet.getPayloadType(), routeFor(packet),
                         packet.getPathHashCount(), fp.hash, fp.digest, now,
                         output);
}

bool record(ChannelRepeatTracker& tracker, const mesh::Packet& packet,
            uint8_t slot, uint32_t timestamp, uint32_t now) {
  const Fingerprint fp = fingerprint(packet);
  return tracker.recordSent(packet.getPayloadType(), routeFor(packet),
                            fp.hash, fp.digest, slot, timestamp, now);
}

}  // namespace

int main() {
  ChannelRepeatTracker tracker;
  ChannelRepeatObservation observation{};
  mesh::Packet sent = channelPacket(0x20U);
  const FloodRouteBinding legacyRoute = routeFor(sent);
  const FloodRouteBinding invalidRoute{};
  const Fingerprint sentFingerprint = fingerprint(sent);

  mesh::Packet echo = sent;
  echo.setPathHashSizeAndCount(1U, 1U);
  echo.path[0] = 0x91U;
  const Fingerprint echoFingerprint = fingerprint(echo);
  // Path changes are intentionally excluded from both correlation hashes.
  assert(memcmp(sentFingerprint.hash, echoFingerprint.hash,
                kChannelRepeatHashBytes) == 0);
  assert(memcmp(sentFingerprint.digest, echoFingerprint.digest,
                kChannelRepeatDigestBytes) == 0);

  // An echo cannot be claimed before the corresponding physical logTx.
  assert(!observe(tracker, echo, 1001U, observation));

  // Only an outbound flood GRP_TXT with a real slot/timestamp is trackable.
  assert(!tracker.recordSent(0x06U, legacyRoute, sentFingerprint.hash,
                             sentFingerprint.digest, 0U, 100U, 1000U));
  assert(!tracker.recordSent(kChannelGroupTextPayloadType, invalidRoute,
                             sentFingerprint.hash, sentFingerprint.digest,
                             0U, 100U, 1000U));
  assert(!tracker.recordSent(kChannelGroupTextPayloadType, legacyRoute,
                             sentFingerprint.hash, sentFingerprint.digest,
                             4U, 100U, 1000U));
  assert(!tracker.recordSent(kChannelGroupTextPayloadType, legacyRoute,
                             sentFingerprint.hash, sentFingerprint.digest,
                             0U, 0U, 1000U));
  assert(record(tracker, sent, 0U, 100U, 1000U));

  // Hearing the original zero-hop form is not repeater evidence.
  assert(!observe(tracker, sent, 1001U, observation));
  assert(observe(tracker, echo, 1002U, observation));
  assert(observation.channelSlot == 0U);
  assert(observation.messageTimestamp == 100U);
  assert(observation.repeatCount == 1U);

  // Every returned copy counts, including another copy with a longer path;
  // this is intentionally not a unique-repeater estimate.
  echo.setPathHashSizeAndCount(1U, 2U);
  echo.path[1] = 0x42U;
  assert(observe(tracker, echo, 1003U, observation));
  assert(observation.repeatCount == 2U);

  mesh::Packet payloadMutation = echo;
  payloadMutation.payload[7] ^= 0x01U;
  const Fingerprint payloadFingerprint = fingerprint(payloadMutation);
  assert(memcmp(sentFingerprint.digest, payloadFingerprint.digest,
                kChannelRepeatDigestBytes) != 0);
  assert(!observe(tracker, payloadMutation, 1004U, observation));

  mesh::Packet typeMutation = echo;
  typeMutation.header = static_cast<uint8_t>(
      (0x06U << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD);
  const Fingerprint typeFingerprint = fingerprint(typeMutation);
  assert(memcmp(sentFingerprint.digest, typeFingerprint.digest,
                kChannelRepeatDigestBytes) != 0);
  assert(!observe(tracker, typeMutation, 1005U, observation));

  mesh::Packet directMutation = echo;
  directMutation.header = static_cast<uint8_t>(
      (kChannelGroupTextPayloadType << PH_TYPE_SHIFT) | ROUTE_TYPE_DIRECT);
  assert(!observe(tracker, directMutation, 1006U, observation));

  // A fabricated collision in MeshCore's truncated hash still fails the
  // retained full-digest correlation.
  uint8_t differentDigest[kChannelRepeatDigestBytes]{};
  memcpy(differentDigest, sentFingerprint.digest, sizeof(differentDigest));
  differentDigest[31] ^= 0x80U;
  assert(!tracker.observe(kChannelGroupTextPayloadType, legacyRoute, 1U,
                          sentFingerprint.hash, differentDigest, 1007U,
                          observation));

  // Same-channel sends overlap safely because every event carries the exact
  // unique message timestamp as well as the slot.
  mesh::Packet second = channelPacket(0x60U);
  mesh::Packet secondEcho = second;
  secondEcho.setPathHashSizeAndCount(1U, 1U);
  secondEcho.path[0] = 0x55U;
  assert(record(tracker, second, 0U, 101U, 1100U));
  assert(observe(tracker, secondEcho, 1101U, observation));
  assert(observation.channelSlot == 0U);
  assert(observation.messageTimestamp == 101U);
  assert(observation.repeatCount == 1U);
  assert(observe(tracker, echo, 1102U, observation));
  assert(observation.messageTimestamp == 100U);
  assert(observation.repeatCount == 3U);

  // Expiry is bounded and wrap-safe.
  assert(!observe(tracker, echo, 1000U + kChannelRepeatWindowMs + 1U,
                  observation));
  assert(tracker.activeCount(1000U + kChannelRepeatWindowMs + 1U) == 1U);
  tracker.clear();
  mesh::Packet wrapPacket = channelPacket(0xa0U);
  mesh::Packet wrapEcho = wrapPacket;
  wrapEcho.setPathHashSizeAndCount(1U, 1U);
  const uint32_t nearWrap = 0xfffffff0UL;
  assert(record(tracker, wrapPacket, 1U, 200U, nearWrap));
  assert(observe(tracker, wrapEcho, 0x00000010UL, observation));
  assert(!observe(tracker, wrapEcho,
                  nearWrap + kChannelRepeatWindowMs + 1U, observation));

  // Capacity matches the complete visible journal and never silently evicts
  // an open row. A hypothetical extra send has no observation lifecycle.
  tracker.clear();
  mesh::Packet packets[kChannelRepeatTrackerCapacity + 1U]{};
  for (size_t index = 0U; index < kChannelRepeatTrackerCapacity; ++index) {
    packets[index] = channelPacket(static_cast<uint8_t>(index * 9U));
    assert(record(tracker, packets[index],
                  static_cast<uint8_t>(index % 4U),
                  static_cast<uint32_t>(1000U + index),
                  static_cast<uint32_t>(2000U + index)));
  }
  packets[kChannelRepeatTrackerCapacity] = channelPacket(0xf3U);
  assert(!record(tracker, packets[kChannelRepeatTrackerCapacity], 0U,
                 2000U, 2100U));
  assert(tracker.activeCount(2101U) == kChannelRepeatTrackerCapacity);
  mesh::Packet oldestEcho = packets[0];
  oldestEcho.setPathHashSizeAndCount(1U, 1U);
  assert(observe(tracker, oldestEcho, 2102U, observation));
  assert(observation.messageTimestamp == 1000U);
  mesh::Packet rejectedEcho = packets[kChannelRepeatTrackerCapacity];
  rejectedEcho.setPathHashSizeAndCount(1U, 1U);
  assert(!observe(tracker, rejectedEcho, 2103U, observation));

  // Distinct final path tokens are retained in first-seen order, bounded at
  // four, while repeatCount continues to count every returned copy.
  tracker.clear();
  assert(record(tracker, sent, 0U, 3000U, 5000U));
  for (uint8_t index = 0U; index < 6U; ++index) {
    const uint8_t token[] = {
        static_cast<uint8_t>(0xa0U + index),
        static_cast<uint8_t>(0xb0U + index),
        static_cast<uint8_t>(0xc0U + index)};
    assert(tracker.observeDetailed(
               kChannelGroupTextPayloadType, legacyRoute, 1U,
               sentFingerprint.hash, sentFingerprint.digest, token, 3U,
               5001U + index, observation) ==
           ChannelRepeatObserveResult::Recorded);
  }
  assert(observation.repeatCount == 6U);
  assert(observation.observationOpen);
  assert(observation.sourceCount == 4U);
  assert(observation.sourcesTruncated);
  assert(observation.sources[0].tokenBytes == 3U);
  assert(observation.sources[0].token[0] == 0xa0U);

  // Exactly 120 seconds after physical TX produces one retained final close,
  // including a completed zero result when no echo was observed.
  tracker.clear();
  assert(record(tracker, sent, 0U, 3100U, 6000U));
  tracker.expire(6000U + kChannelRepeatWindowMs);
  assert(tracker.takeDirty(observation));
  assert(observation.messageTimestamp == 3100U);
  assert(observation.repeatCount == 0U);
  assert(!observation.observationOpen);
  assert(!tracker.takeDirty(observation));

  tracker.clear();
  assert(record(tracker, sent, 0U, 3200U, 7000U));
  tracker.closeAll();
  assert(tracker.takeDirty(observation));
  assert(observation.messageTimestamp == 3200U);
  assert(!observation.observationOpen);

  // Counter saturates instead of wrapping and no longer emits transitions
  // once its durable value reaches 255.
  tracker.clear();
  mesh::Packet saturationPacket = channelPacket(0xe0U);
  mesh::Packet saturationEcho = saturationPacket;
  saturationEcho.setPathHashSizeAndCount(1U, 1U);
  assert(record(tracker, saturationPacket, 3U, 4000U, 3000U));
  for (uint16_t count = 1U; count <= 255U; ++count) {
    assert(observe(tracker, saturationEcho,
                   static_cast<uint32_t>(3000U + count), observation));
    assert(observation.repeatCount == static_cast<uint8_t>(count));
  }
  assert(!observe(tracker, saturationEcho, 3300U, observation));
  assert(tracker.lookup(3U, 4000U, 3300U, observation));
  assert(observation.repeatCount == 255U);
  // All 255 callbacks arrived before a consumer drain, but one coalesced
  // transition retains the latest cumulative value and then clears cleanly.
  assert(tracker.takeDirty(observation));
  assert(observation.channelSlot == 3U);
  assert(observation.messageTimestamp == 4000U);
  assert(observation.repeatCount == 255U);
  assert(!tracker.takeDirty(observation));

  // Profile/reset cancellation clears even otherwise-live sent trackers.
  tracker.clear();
  assert(tracker.activeCount(3300U) == 0U);
  assert(!observe(tracker, saturationEcho, 3301U, observation));
  return 0;
}
