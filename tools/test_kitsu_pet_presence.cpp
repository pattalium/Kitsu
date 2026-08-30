#include "../src/kitsu_pet_presence.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <string.h>

using kitsu868::presence::EventAppeared;
using kitsu868::presence::EventApproaching;
using kitsu868::presence::EventBandChanged;
using kitsu868::presence::EventFamiliar;
using kitsu868::presence::EventGone;
using kitsu868::presence::EventGroupEnded;
using kitsu868::presence::EventGroupStarted;
using kitsu868::presence::EventLeaving;
using kitsu868::presence::GroupSummary;
using kitsu868::presence::Observation;
using kitsu868::presence::ObserveResult;
using kitsu868::presence::ObserveStatus;
using kitsu868::presence::PeerSnapshot;
using kitsu868::presence::PetPresenceTracker;
using kitsu868::presence::SignalBand;
using kitsu868::presence::SignalTrend;
using kitsu868::presence::hasEvent;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL " << message << '\n';
  }
}

bool closeTo(float left, float right, float tolerance = 0.01f) {
  return std::fabs(left - right) <= tolerance;
}

Observation sample(uint16_t uid, float rssi, uint32_t at,
                   float snr = 7.0f, bool familiar = false) {
  Observation value{};
  value.uid = uid;
  value.rssiDbm = rssi;
  value.snrDb = snr;
  value.observedAtMs = at;
  value.familiarHint = familiar;
  return value;
}

void testValidationAndFirstAppearance() {
  PetPresenceTracker tracker;
  Observation invalid = sample(0U, -70.0f, 0U);
  check(tracker.observe(invalid).status == ObserveStatus::InvalidInput,
        "zero UID is rejected");
  invalid = sample(1U, std::numeric_limits<float>::quiet_NaN(), 0U);
  check(tracker.observe(invalid).status == ObserveStatus::InvalidInput,
        "non-finite RSSI is rejected");
  invalid = sample(1U, -141.0f, 0U);
  check(tracker.observe(invalid).status == ObserveStatus::InvalidInput,
        "impossible RSSI is rejected");
  invalid = sample(1U, -70.0f, 0U, 33.0f);
  check(tracker.observe(invalid).status == ObserveStatus::InvalidInput,
        "impossible SNR is rejected");

  const ObserveResult appeared = tracker.observe(sample(0x1001U, -84.0f, 0U,
                                                        8.5f));
  check(appeared.status == ObserveStatus::Ok &&
            hasEvent(appeared.events, EventAppeared),
        "first valid signal appears");
  check(!hasEvent(appeared.events, EventFamiliar) &&
            !appeared.peer.familiar,
        "first signal is not invented as familiar");
  check(appeared.peer.band == SignalBand::Medium &&
            appeared.peer.trend == SignalTrend::Unknown,
        "first signal has a band but no premature trend");
  check(closeTo(appeared.peer.smoothedRssiDbm, -84.0f) &&
            closeTo(appeared.peer.latestSnrDb, 8.5f),
        "first signal and SNR are retained");
  check(strcmp(kitsu868::presence::signalBandName(appeared.peer.band),
               "medium") == 0,
        "band name is stable");
}

void testSmoothingJitterAndHysteresis() {
  PetPresenceTracker tracker;
  tracker.observe(sample(1U, -100.0f, 0U));
  ObserveResult changed = tracker.observe(sample(1U, -60.0f, 5000U));
  check(closeTo(changed.peer.smoothedRssiDbm, -90.0f),
        "RSSI uses a deterministic one-quarter EWMA");
  check(changed.peer.band == SignalBand::Weak,
        "one strong sample does not jump a weak hysteretic band");

  changed = tracker.observe(sample(1U, -60.0f, 10000U));
  changed = tracker.observe(sample(1U, -60.0f, 15000U));
  check(changed.peer.band == SignalBand::Medium &&
            hasEvent(changed.events, EventBandChanged),
        "sustained strength crosses the weak-to-medium entry margin");
  changed = tracker.observe(sample(1U, -60.0f, 20000U));

  // Raw jitter around the nominal -85 dBm boundary cannot bounce a Medium band
  // through its -90 dBm exit margin after smoothing.
  const float jitter[] = {-88.0f, -82.0f, -89.0f, -83.0f, -87.0f, -84.0f};
  uint32_t now = 25000U;
  for (float value : jitter) {
    changed = tracker.observe(sample(1U, value, now));
    now += 5000U;
    check(changed.peer.band == SignalBand::Medium,
          "medium-band jitter is absorbed by smoothing and hysteresis");
    check(!hasEvent(changed.events, EventApproaching) &&
              !hasEvent(changed.events, EventLeaving),
          "jitter does not emit a directional trend");
  }

  // A fresh Strong signal must fall below -70 dBm smoothed before leaving
  // that band.
  PetPresenceTracker closeTracker;
  ObserveResult close = closeTracker.observe(sample(2U, -60.0f, 0U));
  check(close.peer.band == SignalBand::Strong,
        "strong signal band initializes at the strong boundary");
  close = closeTracker.observe(sample(2U, -75.0f, 5000U));
  check(close.peer.band == SignalBand::Strong,
        "strong band holds above its hysteretic exit");
  close = closeTracker.observe(sample(2U, -100.0f, 10000U));
  close = closeTracker.observe(sample(2U, -100.0f, 15000U));
  check(close.peer.band == SignalBand::Medium,
        "sustained weaker signal exits strong without chatter");
}

void testSustainedTrends() {
  PetPresenceTracker tracker;
  tracker.observe(sample(3U, -100.0f, 0U));
  tracker.observe(sample(3U, -80.0f, 5000U));
  ObserveResult result = tracker.observe(sample(3U, -70.0f, 10000U));
  check(!hasEvent(result.events, EventApproaching) &&
            result.peer.trend == SignalTrend::Unknown,
        "one stronger vote cannot declare approaching");
  result = tracker.observe(sample(3U, -65.0f, 13000U));
  check(hasEvent(result.events, EventApproaching) &&
            result.peer.trend == SignalTrend::Approaching,
        "two separated stronger votes declare approaching");

  tracker.observe(sample(3U, -100.0f, 18000U));
  result = tracker.observe(sample(3U, -110.0f, 23000U));
  check(!hasEvent(result.events, EventLeaving),
        "one weaker vote cannot declare leaving");
  result = tracker.observe(sample(3U, -115.0f, 26000U));
  check(hasEvent(result.events, EventLeaving) &&
            result.peer.trend == SignalTrend::Leaving,
        "two separated weaker votes declare leaving");
  check(strcmp(kitsu868::presence::signalTrendName(result.peer.trend),
               "leaving") == 0,
        "trend name is stable and contains no direction claim");
}

void testGoneMissedHeartbeatAndFamiliarReturn() {
  PetPresenceTracker tracker;
  tracker.observe(sample(4U, -70.0f, 1000U));
  auto expired = tracker.expire(1000U + kitsu868::presence::kGoneAfterMs - 1U);
  check(expired.goneCount == 0U && expired.group.presentCount == 1U,
        "one millisecond before timeout remains present");
  expired = tracker.expire(1000U + kitsu868::presence::kGoneAfterMs);
  check(expired.goneCount == 1U && expired.goneUids[0] == 4U &&
            hasEvent(expired.events, EventGone) &&
            expired.group.presentCount == 0U,
        "missed heartbeats become one explicit gone transition");
  check(tracker.expire(99999U).goneCount == 0U,
        "gone is emitted once, not on every tick");

  const ObserveResult returned = tracker.observe(sample(4U, -71.0f, 100000U));
  check(hasEvent(returned.events, EventAppeared) &&
            hasEvent(returned.events, EventFamiliar) &&
            returned.peer.familiar,
        "a tracked UID returning after gone is familiar");

  PetPresenceTracker hinted;
  const ObserveResult known = hinted.observe(sample(5U, -80.0f, 0U, 5.0f,
                                                     true));
  check(hasEvent(known.events, EventAppeared) &&
            hasEvent(known.events, EventFamiliar) && known.peer.familiar,
        "persistent caller hint recognizes a UID on first runtime sighting");
}

void testMillisWrapAndStaleTimestamp() {
  PetPresenceTracker tracker;
  constexpr uint32_t start = UINT32_C(0xFFFFFF00);
  tracker.observe(sample(6U, -75.0f, start));
  auto expired = tracker.expire(start + kitsu868::presence::kGoneAfterMs - 1U);
  check(expired.timeAccepted && expired.goneCount == 0U,
        "presence timeout is safe one millisecond before millis wrap deadline");
  expired = tracker.expire(start + kitsu868::presence::kGoneAfterMs);
  check(expired.goneCount == 1U && expired.goneUids[0] == 6U,
        "presence timeout is wrap-safe at the exact deadline");
  const ObserveResult oldReplay = tracker.observe(sample(
      6U, -40.0f, start + kitsu868::presence::kGoneAfterMs - 1U));
  check(oldReplay.status == ObserveStatus::StaleTimestamp,
        "an older replay cannot resurrect a gone signal");

  PetPresenceTracker stale;
  stale.observe(sample(7U, -75.0f, 10000U));
  const ObserveResult rejected = stale.observe(sample(7U, -50.0f, 9000U));
  check(rejected.status == ObserveStatus::StaleTimestamp &&
            closeTo(rejected.peer.latestRssiDbm, -75.0f),
        "backward non-wrap timestamps cannot mutate signal state");
}

void testCapacityAndDeterministicReuse() {
  PetPresenceTracker tracker;
  for (uint16_t uid = 1U; uid <= kitsu868::presence::kPeerCapacity; ++uid) {
    const ObserveResult result = tracker.observe(
        sample(static_cast<uint16_t>(0x1000U + uid),
               static_cast<float>(-100 + uid), 0U));
    check(result.status == ObserveStatus::Ok,
          "each bounded presence slot accepts one peer");
  }
  check(tracker.trackedCount() == kitsu868::presence::kPeerCapacity &&
            tracker.summary().presentCount ==
                kitsu868::presence::kPeerCapacity,
        "tracker reaches its exact fixed capacity");
  const ObserveResult full = tracker.observe(sample(0x2000U, -40.0f, 1U));
  check(full.status == ObserveStatus::CapacityFull &&
            full.group.presentCount == kitsu868::presence::kPeerCapacity,
        "a ninth signal cannot evict a currently present peer");

  tracker.expire(kitsu868::presence::kGoneAfterMs);
  const ObserveResult reused = tracker.observe(
      sample(0x2000U, -40.0f, kitsu868::presence::kGoneAfterMs + 1U));
  check(reused.status == ObserveStatus::Ok &&
            tracker.trackedCount() == kitsu868::presence::kPeerCapacity,
        "oldest inactive slot is reused without allocation");
  PeerSnapshot peer{};
  check(!tracker.peer(0x1001U, peer) && tracker.peer(0x2000U, peer),
        "equal-age inactive slots reuse deterministic slot zero");
}

void testGroupSummaryAndTransitions() {
  PetPresenceTracker tracker;
  ObserveResult first = tracker.observe(sample(0x3001U, -80.0f, 0U, 4.0f,
                                               true));
  check(!first.group.groupActive && first.group.presentCount == 1U &&
            first.group.familiarCount == 1U,
        "one familiar signal is not a group");
  ObserveResult second = tracker.observe(sample(0x3002U, -60.0f, 0U));
  check(hasEvent(second.events, EventGroupStarted) &&
            second.group.groupActive && second.group.presentCount == 2U &&
            second.group.strongestSignalUid == 0x3002U &&
            second.group.strongestSignalBand == SignalBand::Strong,
        "second signal starts a group with an honest strongest-signal summary");
  ObserveResult third = tracker.observe(sample(0x3003U, -70.0f, 0U));
  check(!hasEvent(third.events, EventGroupStarted) &&
            third.group.presentCount == 3U,
        "additional members do not repeat group-start transition");

  tracker.observe(sample(0x3002U, -61.0f, 10000U));
  tracker.observe(sample(0x3003U, -71.0f, 10000U));
  const auto expired = tracker.expire(kitsu868::presence::kGoneAfterMs);
  check(expired.goneCount == 1U && expired.goneUids[0] == 0x3001U &&
            !hasEvent(expired.events, EventGroupEnded) &&
            expired.group.presentCount == 2U,
        "group remains active while two refreshed signals remain");
  const auto ended = tracker.expire(10000U +
                                     kitsu868::presence::kGoneAfterMs);
  check(ended.goneCount == 2U && hasEvent(ended.events, EventGroupEnded) &&
            !ended.group.groupActive,
        "last missed group heartbeats emit one group-ended transition");
}

}  // namespace

int main() {
  static_assert(kitsu868::presence::kPeerCapacity == 8U,
                "pet presence capacity changed");
  testValidationAndFirstAppearance();
  testSmoothingJitterAndHysteresis();
  testSustainedTrends();
  testGoneMissedHeartbeatAndFamiliarReturn();
  testMillisWrapAndStaleTimestamp();
  testCapacityAndDeterministicReuse();
  testGroupSummaryAndTransitions();

  if (failures != 0) {
    std::cerr << "TEST_FAIL kitsu_pet_presence failures=" << failures << '\n';
    return 1;
  }
  std::cout << "TEST_PASS kitsu_pet_presence capacity=8 gone_ms=20000 "
               "bands=weak,medium,strong trends=approaching,leaving\n";
  return 0;
}
