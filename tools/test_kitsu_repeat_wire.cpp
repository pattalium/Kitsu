#include "../src/kitsu_repeat_wire.h"
#include "../src/kitsu_channel_repeat_tracker.h"
#include "../src/kitsu_advert_repeat_tracker.h"
#include "../src/kitsu_transport_scope.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

namespace {

using kitsu868::mesh::RepeatWireDecodeStatus;
using kitsu868::mesh::RepeatWireView;
using kitsu868::mesh::ChannelRepeatObservation;
using kitsu868::mesh::ChannelRepeatObserveResult;
using kitsu868::mesh::ChannelRepeatTracker;
using kitsu868::mesh::AdvertRepeatObserveResult;
using kitsu868::mesh::AdvertRepeatTracker;
using kitsu868::mesh::FloodRouteBinding;
using kitsu868::mesh::decodeRepeatWire;
using kitsu868::mesh::floodRouteBindingFromWire;
using kitsu868::mesh::isExactDefaultScopedRepeat;
using kitsu868::mesh::sameFloodRouteBinding;

constexpr uint8_t header(uint8_t type, uint8_t route,
                         uint8_t version = 0U) {
  return static_cast<uint8_t>((version << 6U) | (type << 2U) | route);
}

}  // namespace

int main() {
  RepeatWireView view{};
  assert(decodeRepeatWire(nullptr, 0U, view) ==
         RepeatWireDecodeStatus::InvalidArgument);
  const uint8_t shortFrame[] = {header(5U, 1U)};
  assert(decodeRepeatWire(shortFrame, sizeof(shortFrame), view) ==
         RepeatWireDecodeStatus::InvalidArgument);

  const uint8_t futureVersion[] = {header(5U, 1U, 1U), 0U, 0x44U};
  assert(decodeRepeatWire(futureVersion, sizeof(futureVersion), view) ==
         RepeatWireDecodeStatus::UnsupportedVersion);

  const uint8_t shortTransport[] = {header(5U, 0U), 0U, 0U, 0U, 0U};
  assert(decodeRepeatWire(shortTransport, sizeof(shortTransport), view) ==
         RepeatWireDecodeStatus::TruncatedTransport);
  const uint8_t validTransport[] = {
      header(5U, 0U), 0x11U, 0x22U, 0x33U, 0x44U, 0U, 0x55U};
  assert(decodeRepeatWire(validTransport, sizeof(validTransport), view) ==
         RepeatWireDecodeStatus::Ok);
  assert(view.flood);
  assert(view.hasTransportCodes);
  assert(view.transportCodes[0] == 0x2211U);
  assert(view.transportCodes[1] == 0x4433U);

  // Golden scoped echo: transport code 8F7A, one one-byte repeater token,
  // immutable payload "hello". It must pass raw decoding as a flood and reach
  // the exact hash/digest tracker, not stop at route classification.
  const uint8_t scopedEcho[] = {
      header(5U, 0U), 0x8fU, 0x7aU, 0x00U, 0x00U,
      0x01U, 0xa1U, 'h', 'e', 'l', 'l', 'o'};
  assert(decodeRepeatWire(scopedEcho, sizeof(scopedEcho), view) ==
         RepeatWireDecodeStatus::Ok);
  assert(view.flood);
  assert(view.pathCount == 1U && view.pathHashSize == 1U);
  assert(view.lastHopToken() && view.lastHopToken()[0] == 0xa1U);
  assert(view.payloadBytes == 5U && memcmp(view.payload, "hello", 5U) == 0);
  assert(isExactDefaultScopedRepeat(view));
  FloodRouteBinding scopedRoute{};
  assert(floodRouteBindingFromWire(view, scopedRoute));
  uint8_t digest[kitsu868::mesh::kChannelRepeatDigestBytes]{};
  uint8_t hash[kitsu868::mesh::kChannelRepeatHashBytes]{};
  assert(kitsu868::mesh::calculateChannelRepeatDigest(
      view.payloadType, view.payload, view.payloadBytes, digest));
  memcpy(hash, digest, sizeof(hash));
  ChannelRepeatTracker tracker;
  assert(tracker.recordSent(view.payloadType, scopedRoute, hash, digest, 0U,
                            100U, 1000U));
  ChannelRepeatObservation observation{};
  assert(tracker.observeDetailed(
             view.payloadType, scopedRoute,
             view.pathCount, hash, digest,
             view.lastHopToken(), view.pathHashSize, 1001U, observation) ==
         ChannelRepeatObserveResult::Recorded);
  assert(observation.repeatCount == 1U);

  const uint8_t wrongCode[] = {
      header(5U, 0U), 0x8eU, 0x7aU, 0x00U, 0x00U,
      0x01U, 0xa1U, 'h', 'e', 'l', 'l', 'o'};
  assert(decodeRepeatWire(wrongCode, sizeof(wrongCode), view) ==
         RepeatWireDecodeStatus::Ok);
  assert(!isExactDefaultScopedRepeat(view));
  FloodRouteBinding wrongCodeRoute{};
  assert(floodRouteBindingFromWire(view, wrongCodeRoute));
  assert(!sameFloodRouteBinding(scopedRoute, wrongCodeRoute));
  assert(tracker.observeDetailed(
             view.payloadType, wrongCodeRoute, view.pathCount, hash, digest,
             view.lastHopToken(), view.pathHashSize, 1002U, observation) ==
         ChannelRepeatObserveResult::WireMismatch);
  const uint8_t nonzeroHomeCode[] = {
      header(5U, 0U), 0x8fU, 0x7aU, 0x01U, 0x00U,
      0x01U, 0xa1U, 'h', 'e', 'l', 'l', 'o'};
  assert(decodeRepeatWire(nonzeroHomeCode, sizeof(nonzeroHomeCode), view) ==
         RepeatWireDecodeStatus::Ok);
  assert(!isExactDefaultScopedRepeat(view));
  FloodRouteBinding wrongCode1Route{};
  assert(floodRouteBindingFromWire(view, wrongCode1Route));
  assert(tracker.observeDetailed(
             view.payloadType, wrongCode1Route, view.pathCount, hash, digest,
             view.lastHopToken(), view.pathHashSize, 1003U, observation) ==
         ChannelRepeatObserveResult::WireMismatch);
  const uint8_t legacyRewrap[] = {
      header(5U, 1U), 0x01U, 0xa1U, 'h', 'e', 'l', 'l', 'o'};
  assert(decodeRepeatWire(legacyRewrap, sizeof(legacyRewrap), view) ==
         RepeatWireDecodeStatus::Ok);
  assert(view.flood && !isExactDefaultScopedRepeat(view));
  FloodRouteBinding legacyRoute{};
  assert(floodRouteBindingFromWire(view, legacyRoute));
  assert(!sameFloodRouteBinding(scopedRoute, legacyRoute));
  assert(tracker.observeDetailed(
             view.payloadType, legacyRoute, view.pathCount, hash, digest,
             view.lastHopToken(), view.pathHashSize, 1003U, observation) ==
         ChannelRepeatObserveResult::WireMismatch);
  ChannelRepeatTracker legacyTracker;
  assert(legacyTracker.recordSent(view.payloadType, legacyRoute, hash, digest,
                                  0U, 101U, 1100U));
  assert(legacyTracker.observeDetailed(
             view.payloadType, legacyRoute, view.pathCount, hash, digest,
             view.lastHopToken(), view.pathHashSize, 1101U, observation) ==
         ChannelRepeatObserveResult::Recorded);
  assert(legacyTracker.observeDetailed(
             view.payloadType, scopedRoute, view.pathCount, hash, digest,
             view.lastHopToken(), view.pathHashSize, 1102U, observation) ==
         ChannelRepeatObserveResult::WireMismatch);
  const uint8_t wrongPathMode[] = {
      header(5U, 0U), 0x8fU, 0x7aU, 0x00U, 0x00U,
      0x41U, 0xa1U, 0xa2U, 'h', 'e', 'l', 'l', 'o'};
  assert(decodeRepeatWire(wrongPathMode, sizeof(wrongPathMode), view) ==
         RepeatWireDecodeStatus::Ok);
  assert(view.pathHashSize == 2U && !isExactDefaultScopedRepeat(view));
  FloodRouteBinding wrongPathRoute{};
  assert(floodRouteBindingFromWire(view, wrongPathRoute));
  assert(tracker.observeDetailed(
             view.payloadType, wrongPathRoute, view.pathCount, hash, digest,
             view.lastHopToken(), view.pathHashSize, 1004U, observation) ==
         ChannelRepeatObserveResult::WireMismatch);

  const uint8_t scopedAdvertEcho[] = {
      header(4U, 0U), 0xb9U, 0x61U, 0x00U, 0x00U,
      0x01U, 0xb2U, 'h', 'e', 'l', 'l', 'o'};
  assert(decodeRepeatWire(scopedAdvertEcho, sizeof(scopedAdvertEcho), view) ==
         RepeatWireDecodeStatus::Ok);
  assert(isExactDefaultScopedRepeat(view));
  assert(floodRouteBindingFromWire(view, scopedRoute));
  assert(kitsu868::mesh::calculateChannelRepeatDigest(
      view.payloadType, view.payload, view.payloadBytes, digest));
  memcpy(hash, digest, sizeof(hash));
  AdvertRepeatTracker advertTracker;
  constexpr uint32_t kAdvertTime = 1704067200UL;
  assert(advertTracker.recordQueued(view.payloadType, true, kAdvertTime));
  assert(advertTracker.markSent(view.payloadType, scopedRoute, hash, digest,
                                kAdvertTime, 2000U));
  assert(advertTracker.observeDetailed(
             view.payloadType, scopedRoute,
             view.pathCount, hash, digest, view.lastHopToken(),
             view.pathHashSize, 2001U) == AdvertRepeatObserveResult::Recorded);

  const uint8_t reservedPath[] = {header(5U, 1U), 0xc1U, 0xaaU, 0x55U};
  assert(decodeRepeatWire(reservedPath, sizeof(reservedPath), view) ==
         RepeatWireDecodeStatus::InvalidPath);
  const uint8_t oversizedPath[] = {header(5U, 1U), 0x7fU, 0xaaU};
  assert(decodeRepeatWire(oversizedPath, sizeof(oversizedPath), view) ==
         RepeatWireDecodeStatus::InvalidPath);
  const uint8_t missingPath[] = {header(5U, 1U), 0x02U, 0xaaU, 0x55U};
  assert(decodeRepeatWire(missingPath, sizeof(missingPath), view) ==
         RepeatWireDecodeStatus::MissingPayload);
  const uint8_t noPayload[] = {header(5U, 1U), 0U};
  assert(decodeRepeatWire(noPayload, sizeof(noPayload), view) ==
         RepeatWireDecodeStatus::MissingPayload);

  const uint8_t path0[] = {header(5U, 1U), 0U, 0x90U};
  assert(decodeRepeatWire(path0, sizeof(path0), view) ==
         RepeatWireDecodeStatus::Ok);
  assert(view.flood);
  assert(view.pathCount == 0U);
  assert(view.lastHopToken() == nullptr);

  const uint8_t path1[] = {header(5U, 1U), 0x02U,
                           0x11U, 0xA1U, 0x71U};
  assert(decodeRepeatWire(path1, sizeof(path1), view) ==
         RepeatWireDecodeStatus::Ok);
  assert(view.pathHashSize == 1U && view.pathCount == 2U);
  assert(view.lastHopToken()[0] == 0xA1U);

  const uint8_t path2[] = {header(5U, 1U), 0x42U,
                           0x11U, 0x12U, 0xB1U, 0xB2U, 0x72U};
  assert(decodeRepeatWire(path2, sizeof(path2), view) ==
         RepeatWireDecodeStatus::Ok);
  assert(view.pathHashSize == 2U && view.pathCount == 2U);
  assert(view.lastHopToken()[0] == 0xB1U);
  assert(view.lastHopToken()[1] == 0xB2U);

  const uint8_t path3[] = {header(5U, 1U), 0x82U,
                           0x11U, 0x12U, 0x13U,
                           0xC1U, 0xC2U, 0xC3U, 0x73U};
  assert(decodeRepeatWire(path3, sizeof(path3), view) ==
         RepeatWireDecodeStatus::Ok);
  assert(view.pathHashSize == 3U && view.pathCount == 2U);
  assert(view.lastHopToken()[0] == 0xC1U);
  assert(view.lastHopToken()[1] == 0xC2U);
  assert(view.lastHopToken()[2] == 0xC3U);
  return 0;
}
