#include "../src/kitsu_advert_repeat_tracker.h"

#include <Packet.h>
#include <SHA256.h>

#include <assert.h>
#include <string.h>

namespace {

using kitsu868::mesh::AdvertRepeatTracker;
using kitsu868::mesh::AdvertRepeatObserveResult;
using kitsu868::mesh::AdvertTransmitState;
using kitsu868::mesh::FloodAdvertStatus;
using kitsu868::mesh::FloodRouteBinding;
using kitsu868::mesh::NearbyAdvertStatus;
using kitsu868::mesh::NearbyAdvertTracker;
using kitsu868::mesh::kAdvertMaximumEmittedAt;
using kitsu868::mesh::kAdvertMinimumEmittedAt;
using kitsu868::mesh::kAdvertPayloadType;
using kitsu868::mesh::kAdvertRepeatDigestBytes;
using kitsu868::mesh::kAdvertRepeatHashBytes;
using kitsu868::mesh::kAdvertRepeatWindowMs;

struct Fingerprint {
  uint8_t hash[kAdvertRepeatHashBytes]{};
  uint8_t digest[kAdvertRepeatDigestBytes]{};
};

Fingerprint fingerprint(const mesh::Packet& packet) {
  Fingerprint output{};
  packet.calculatePacketHash(output.hash);
  SHA256 sha;
  const uint8_t payloadType = packet.getPayloadType();
  sha.update(&payloadType, 1U);
  sha.update(packet.payload, packet.payload_len);
  sha.finalize(output.digest, sizeof(output.digest));
  assert(memcmp(output.hash, output.digest, sizeof(output.hash)) == 0);
  return output;
}

mesh::Packet advertPacket(uint8_t marker) {
  mesh::Packet packet;
  packet.header = static_cast<uint8_t>(
      (kAdvertPayloadType << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD);
  packet.setPathHashSizeAndCount(1U, 0U);
  packet.payload_len = 104U;
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

bool markSent(AdvertRepeatTracker& tracker, const mesh::Packet& packet,
              uint32_t emittedAt, uint32_t nowMs) {
  const Fingerprint value = fingerprint(packet);
  return tracker.markSent(packet.getPayloadType(), routeFor(packet),
                          value.hash, value.digest, emittedAt, nowMs);
}

bool observe(AdvertRepeatTracker& tracker, const mesh::Packet& packet,
             uint32_t nowMs) {
  const Fingerprint value = fingerprint(packet);
  return tracker.observe(packet.getPayloadType(), routeFor(packet),
                         packet.getPathHashCount(), value.hash, value.digest,
                         nowMs);
}

}  // namespace

int main() {
  NearbyAdvertTracker nearbyTracker;
  NearbyAdvertStatus nearbyStatus{};
  constexpr uint32_t nearbyEmittedAt = 1787459999UL;
  assert(!nearbyTracker.recordQueued(kAdvertPayloadType, true,
                                     nearbyEmittedAt));
  assert(nearbyTracker.recordQueued(kAdvertPayloadType, false,
                                    nearbyEmittedAt));
  assert(nearbyTracker.snapshot(nearbyStatus));
  assert(nearbyStatus.state == AdvertTransmitState::Queued);
  assert(nearbyTracker.takeDirty());
  assert(nearbyTracker.markSent(nearbyEmittedAt));
  assert(nearbyTracker.snapshot(nearbyStatus));
  assert(nearbyStatus.state == AdvertTransmitState::Sent);
  assert(nearbyTracker.takeDirty());
  assert(nearbyTracker.recordQueued(kAdvertPayloadType, false,
                                    nearbyEmittedAt + 1U));
  assert(nearbyTracker.markTxFailed(nearbyEmittedAt + 1U));
  assert(nearbyTracker.snapshot(nearbyStatus));
  assert(nearbyStatus.state == AdvertTransmitState::TxFailed);

  AdvertRepeatTracker tracker;
  FloodAdvertStatus status{};
  constexpr uint32_t emittedAt = 1787460000UL;
  mesh::Packet sent = advertPacket(0x20U);
  mesh::Packet echo = sent;
  echo.setPathHashSizeAndCount(1U, 1U);
  echo.path[0] = 0x91U;

  const Fingerprint sentFingerprint = fingerprint(sent);
  const FloodRouteBinding legacyRoute = routeFor(sent);
  const Fingerprint echoFingerprint = fingerprint(echo);
  assert(memcmp(sentFingerprint.hash, echoFingerprint.hash,
                sizeof(sentFingerprint.hash)) == 0);
  assert(memcmp(sentFingerprint.digest, echoFingerprint.digest,
                sizeof(sentFingerprint.digest)) == 0);

  assert(!tracker.snapshot(0U, status));
  assert(!tracker.takeDirty());

  // Only a valid MeshCore flood advert with a producer-valid RTC timestamp can
  // replace the retained record.
  assert(!tracker.recordQueued(0x05U, true, emittedAt));
  assert(!tracker.recordQueued(kAdvertPayloadType, false, emittedAt));
  assert(!tracker.recordQueued(kAdvertPayloadType, true,
                               kAdvertMinimumEmittedAt - 1U));
  assert(!tracker.recordQueued(kAdvertPayloadType, true,
                               kAdvertMaximumEmittedAt + 1U));
  assert(tracker.recordQueued(kAdvertPayloadType, true, emittedAt));
  assert(tracker.snapshot(10U, status));
  assert(status.available);
  assert(status.emittedAt == emittedAt);
  assert(status.state == AdvertTransmitState::Queued);
  assert(!status.repeatCountKnown);
  assert(!status.observationOpen);
  assert(tracker.takeDirty());
  assert(!tracker.takeDirty());

  // Nearby is not repeatable and must not overwrite the most recent
  // Mesh-wide lifecycle record.
  assert(!tracker.recordQueued(kAdvertPayloadType, false, emittedAt + 1U));
  assert(tracker.snapshot(11U, status));
  assert(status.emittedAt == emittedAt);
  assert(status.state == AdvertTransmitState::Queued);

  // A returned packet cannot be counted before the exact physical logTx.
  assert(!observe(tracker, echo, 999U));
  assert(!tracker.markSent(kAdvertPayloadType, legacyRoute,
                           sentFingerprint.hash,
                           sentFingerprint.digest, emittedAt + 1U, 1000U));
  assert(markSent(tracker, sent, emittedAt, 1000U));
  assert(tracker.snapshot(1000U, status));
  assert(status.state == AdvertTransmitState::Sent);
  assert(status.repeatCountKnown);
  assert(status.repeatCount == 0U);
  assert(status.observationOpen);
  assert(tracker.takeDirty());

  // Repeat correlation is bound to the exact physical route prefix, not just
  // the type+payload fingerprint. A scoped send diagnoses legacy, wrong
  // code0/code1, and path-width copies separately from a missing hash.
  AdvertRepeatTracker routeTracker;
  constexpr uint32_t routeEmittedAt = emittedAt + 1000U;
  mesh::Packet scopedSent = sent;
  scopedSent.header = static_cast<uint8_t>(
      (kAdvertPayloadType << PH_TYPE_SHIFT) | ROUTE_TYPE_TRANSPORT_FLOOD);
  scopedSent.transport_codes[0] = 0x1234U;
  scopedSent.transport_codes[1] = 0U;
  scopedSent.setPathHashSizeAndCount(1U, 0U);
  mesh::Packet scopedEcho = scopedSent;
  scopedEcho.setPathHashSizeAndCount(1U, 1U);
  scopedEcho.path[0] = 0xa1U;
  const Fingerprint routeFingerprint = fingerprint(scopedSent);
  assert(routeTracker.recordQueued(kAdvertPayloadType, true, routeEmittedAt));
  assert(routeTracker.markSent(kAdvertPayloadType, routeFor(scopedSent),
                               routeFingerprint.hash,
                               routeFingerprint.digest, routeEmittedAt,
                               500U));
  assert(routeTracker.observeDetailed(
             kAdvertPayloadType, routeFor(echo), 1U, routeFingerprint.hash,
             routeFingerprint.digest, 501U) ==
         AdvertRepeatObserveResult::WireMismatch);
  mesh::Packet wrongCode0 = scopedEcho;
  wrongCode0.transport_codes[0] ^= 0x0001U;
  assert(routeTracker.observeDetailed(
             kAdvertPayloadType, routeFor(wrongCode0), 1U,
             routeFingerprint.hash, routeFingerprint.digest, 502U) ==
         AdvertRepeatObserveResult::WireMismatch);
  mesh::Packet wrongCode1 = scopedEcho;
  wrongCode1.transport_codes[1] = 1U;
  assert(routeTracker.observeDetailed(
             kAdvertPayloadType, routeFor(wrongCode1), 1U,
             routeFingerprint.hash, routeFingerprint.digest, 503U) ==
         AdvertRepeatObserveResult::WireMismatch);
  mesh::Packet wrongWidth = scopedEcho;
  wrongWidth.setPathHashSizeAndCount(2U, 1U);
  assert(routeTracker.observeDetailed(
             kAdvertPayloadType, routeFor(wrongWidth), 1U,
             routeFingerprint.hash, routeFingerprint.digest, 504U) ==
         AdvertRepeatObserveResult::WireMismatch);
  assert(routeTracker.observeDetailed(
             kAdvertPayloadType, routeFor(scopedEcho), 1U,
             routeFingerprint.hash, routeFingerprint.digest, 505U) ==
         AdvertRepeatObserveResult::Recorded);

  // The original zero-path form is not repeater evidence.
  assert(!observe(tracker, sent, 1001U));

  mesh::Packet directEcho = echo;
  directEcho.header = static_cast<uint8_t>(
      (kAdvertPayloadType << PH_TYPE_SHIFT) | ROUTE_TYPE_DIRECT);
  assert(!observe(tracker, directEcho, 1002U));

  mesh::Packet wrongType = echo;
  wrongType.header = static_cast<uint8_t>(
      (0x05U << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD);
  assert(!observe(tracker, wrongType, 1003U));

  uint8_t wrongHash[kAdvertRepeatHashBytes]{};
  memcpy(wrongHash, sentFingerprint.hash, sizeof(wrongHash));
  wrongHash[0] ^= 0x80U;
  assert(!tracker.observe(kAdvertPayloadType, legacyRoute, 1U, wrongHash,
                          sentFingerprint.digest, 1004U));

  uint8_t wrongDigest[kAdvertRepeatDigestBytes]{};
  memcpy(wrongDigest, sentFingerprint.digest, sizeof(wrongDigest));
  wrongDigest[31] ^= 0x80U;
  assert(!tracker.observe(kAdvertPayloadType, legacyRoute, 1U,
                          sentFingerprint.hash, wrongDigest, 1005U));

  assert(observe(tracker, echo, 1006U));
  assert(tracker.snapshot(1006U, status));
  assert(status.repeatCount == 1U);
  assert(tracker.takeDirty());

  // Every matching returned packet copy counts; longer paths do not imply a
  // unique-repeater estimate.
  echo.setPathHashSizeAndCount(1U, 2U);
  echo.path[1] = 0x42U;
  assert(observe(tracker, echo, 1007U));
  assert(tracker.snapshot(1007U, status));
  assert(status.repeatCount == 2U);

  // Exactly 120 seconds closes the observation, matching channel lifecycle
  // semantics, while retaining the final count as one coalesced transition.
  assert(!observe(tracker, echo, 1000U + kAdvertRepeatWindowMs));
  assert(!tracker.tick(1000U + kAdvertRepeatWindowMs));
  assert(tracker.takeDirty());
  assert(!tracker.takeDirty());
  assert(!tracker.tick(1000U + kAdvertRepeatWindowMs + 1U));
  assert(tracker.snapshot(1000U + kAdvertRepeatWindowMs + 1U, status));
  assert(status.state == AdvertTransmitState::Sent);
  assert(status.repeatCountKnown);
  assert(status.repeatCount == 2U);
  assert(!status.observationOpen);
  assert(!observe(tracker, echo, 1000U + kAdvertRepeatWindowMs + 2U));
  assert(!tracker.takeDirty());

  // A new Mesh-wide queue replaces the old record. A late copy of the old
  // packet cannot be attributed to the new send.
  constexpr uint32_t secondEmittedAt = emittedAt + 30U;
  mesh::Packet second = advertPacket(0x70U);
  mesh::Packet secondEcho = second;
  secondEcho.setPathHashSizeAndCount(1U, 1U);
  secondEcho.path[0] = 0x55U;
  assert(tracker.recordQueued(kAdvertPayloadType, true, secondEmittedAt));
  assert(markSent(tracker, second, secondEmittedAt, 200000U));
  assert(!observe(tracker, echo, 200001U));
  assert(observe(tracker, secondEcho, 200002U));
  assert(tracker.snapshot(200002U, status));
  assert(status.emittedAt == secondEmittedAt);
  assert(status.repeatCount == 1U);

  // A queued physical failure retains an explicit failure lifecycle with no
  // numeric repeat evidence.
  constexpr uint32_t failedEmittedAt = secondEmittedAt + 30U;
  assert(tracker.recordQueued(kAdvertPayloadType, true, failedEmittedAt));
  assert(!tracker.markTxFailed(secondEmittedAt));
  assert(tracker.markTxFailed(failedEmittedAt));
  assert(tracker.snapshot(200100U, status));
  assert(status.state == AdvertTransmitState::TxFailed);
  assert(!status.repeatCountKnown);
  assert(status.repeatCount == 0U);
  assert(!status.observationOpen);

  // Saturate at 255 without wrapping.
  constexpr uint32_t saturationEmittedAt = failedEmittedAt + 30U;
  assert(tracker.recordQueued(kAdvertPayloadType, true,
                              saturationEmittedAt));
  assert(markSent(tracker, sent, saturationEmittedAt, 300000U));
  for (uint16_t count = 1U; count <= 255U; ++count) {
    assert(observe(tracker, echo, 300000U + count));
  }
  assert(!observe(tracker, echo, 300300U));
  assert(tracker.snapshot(300300U, status));
  assert(status.repeatCount == 255U);

  // Retain at most four distinct exact final path tokens while continuing to
  // count every returned copy. These tokens are path prefixes, not identities.
  AdvertRepeatTracker sourceTracker;
  assert(sourceTracker.recordQueued(kAdvertPayloadType, true,
                                    saturationEmittedAt + 1U));
  assert(markSent(sourceTracker, sent, saturationEmittedAt + 1U, 400000U));
  for (uint8_t index = 0U; index < 6U; ++index) {
    const uint8_t token[] = {
        static_cast<uint8_t>(0xa0U + index),
        static_cast<uint8_t>(0xb0U + index),
        static_cast<uint8_t>(0xc0U + index)};
    assert(sourceTracker.observeDetailed(
               kAdvertPayloadType, legacyRoute, 1U, sentFingerprint.hash,
               sentFingerprint.digest, token, 3U, 400001U + index) ==
           AdvertRepeatObserveResult::Recorded);
  }
  assert(sourceTracker.snapshot(400010U, status));
  assert(status.repeatCount == 6U);
  assert(status.sourceCount == 4U);
  assert(status.sourcesTruncated);
  assert(status.sources[0].tokenBytes == 3U);
  assert(status.sources[0].token[0] == 0xa0U);

  // Unsigned elapsed arithmetic stays correct across millis() wraparound.
  tracker.clear();
  constexpr uint32_t wrapEmittedAt = saturationEmittedAt + 30U;
  constexpr uint32_t nearWrap = 0xfffffff0UL;
  assert(tracker.recordQueued(kAdvertPayloadType, true, wrapEmittedAt));
  assert(markSent(tracker, sent, wrapEmittedAt, nearWrap));
  assert(observe(tracker, echo, 0x00000010UL));
  assert(tracker.snapshot(0x00000010UL, status));
  assert(status.observationOpen);
  assert(tracker.tick(nearWrap + kAdvertRepeatWindowMs));
  assert(tracker.snapshot(nearWrap + kAdvertRepeatWindowMs, status));
  assert(!status.observationOpen);
  assert(status.repeatCount == 1U);

  // Clearing profile-local state retains a dirty edge while removing the
  // public record completely.
  (void)tracker.takeDirty();
  tracker.clear();
  assert(!tracker.snapshot(0U, status));
  assert(!status.available);
  assert(tracker.takeDirty());
  assert(!tracker.takeDirty());
  return 0;
}
