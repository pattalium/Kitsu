#include "../src/kitsu_party_modes.h"
#include "../src/kitsu_party_hotspot.h"

#include <cstring>
#include <iostream>

namespace modes = kitsu868::party_modes;
namespace legacy = kitsu868::party;

namespace {

int failures = 0;
bool covered[72]{};

void check(bool condition, const char* message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL " << message << '\n';
  }
}

void cover(uint8_t feature, bool condition, const char* message) {
  check(condition, message);
  if (condition && feature < sizeof(covered) / sizeof(covered[0])) {
    covered[feature] = true;
  }
}

struct PairSession {
  modes::HostSession host;
  modes::ParticipantSession guest;
  modes::Packet open{};
  uint32_t now = 1000U;
};

bool preparePair(modes::Mode mode, PairSession& pair,
                 uint32_t nonce = UINT32_C(0x12345678),
                 uint32_t seed = UINT32_C(0xA1B2C3D4)) {
  if (pair.host.start(0x1001U, nonce, mode, seed, pair.now, 60U, 20U) !=
      modes::Status::Ok) {
    return false;
  }
  modes::Packet beacon{};
  if (pair.host.makeBeacon(pair.now, beacon) != modes::Status::Ok ||
      pair.guest.observeBeacon(0x2002U, beacon, pair.now) !=
          modes::Status::Ok) {
    return false;
  }
  modes::Packet join{};
  modes::Packet welcome{};
  if (pair.guest.makeJoinRequest(join) != modes::Status::Ok ||
      pair.host.acceptJoin(join, pair.now, welcome) != modes::Status::Ok ||
      pair.guest.acceptHostPacket(welcome, pair.now) != modes::Status::Ok) {
    return false;
  }
  modes::Packet ready{};
  if (pair.guest.makeReady(true, ready) != modes::Status::Ok ||
      pair.host.acceptReady(ready) != modes::Status::Ok ||
      pair.host.setHostReady(true) != modes::Status::Ok ||
      !pair.host.canStart() ||
      pair.host.begin(pair.now, pair.open) != modes::Status::Ok ||
      pair.guest.acceptHostPacket(pair.open, pair.now) != modes::Status::Ok) {
    return false;
  }
  return true;
}

bool playRound(PairSession& pair, const modes::Contribution& hostValue,
               const modes::Contribution& guestValue,
               modes::Packet& output) {
  if (pair.host.submitHostContribution(hostValue) != modes::Status::Ok) {
    return false;
  }
  modes::Packet contribution{};
  if (pair.guest.makeContribution(guestValue, contribution) !=
          modes::Status::Ok ||
      pair.host.acceptContribution(contribution, pair.now + 10U) !=
          modes::Status::Ok) {
    return false;
  }
  const modes::HostState beforeDuplicate = pair.host.snapshot();
  if (pair.host.acceptContribution(contribution, pair.now + 11U) !=
          modes::Status::Duplicate ||
      std::memcmp(&beforeDuplicate, &pair.host.state(),
                  sizeof(beforeDuplicate)) != 0) {
    return false;
  }
  pair.now += 100U;
  if (pair.host.advance(pair.now, output) != modes::Status::Ok ||
      pair.guest.acceptHostPacket(output, pair.now) != modes::Status::Ok) {
    return false;
  }
  return true;
}

modes::ModeResult runThreeRoundSignalMode(
    modes::Mode mode, const int16_t hostSignals[3],
    const int16_t guestSignals[3],
    modes::ParticipantSession* guestOut = nullptr) {
  PairSession pair{};
  check(preparePair(mode, pair), "three-round session prepares");
  modes::Packet output{};
  for (uint8_t round = 0U; round < 3U; ++round) {
    modes::Contribution host{};
    modes::Contribution guest{};
    host.signalRssi = hostSignals[round];
    guest.signalRssi = guestSignals[round];
    check(playRound(pair, host, guest, output),
          "three-round signal contribution completes");
  }
  if (guestOut) *guestOut = pair.guest;
  return pair.host.state().result;
}

modes::ModeResult runSingleValueMode(modes::Mode mode, uint16_t hostValue,
                                     uint16_t guestValue,
                                     modes::ParticipantSession* guestOut =
                                         nullptr) {
  PairSession pair{};
  check(preparePair(mode, pair), "single-round session prepares");
  modes::Contribution host{};
  modes::Contribution guest{};
  host.value = hostValue;
  guest.value = guestValue;
  modes::Packet resultPacket{};
  check(playRound(pair, host, guest, resultPacket),
        "single-round value contribution completes");
  check(resultPacket.type == modes::PacketType::Result,
        "single-round mode emits result packet");
  const modes::ParticipantState acceptedResult = pair.guest.snapshot();
  check(pair.guest.acceptHostPacket(resultPacket, pair.now) ==
            modes::Status::Duplicate &&
            std::memcmp(&acceptedResult, &pair.guest.state(),
                        sizeof(acceptedResult)) == 0,
        "replayed result packet is idempotent at participant");
  if (guestOut) *guestOut = pair.guest;
  return pair.host.state().result;
}

void testCodecAndLegacyCompatibility() {
  modes::HostSession host;
  check(host.start(1U, 99U, modes::Mode::Triangulation, 77U, 100U, 30U,
                   10U) == modes::Status::Ok,
        "M8 codec host starts");
  modes::Packet beacon{};
  check(host.makeBeacon(100U, beacon) == modes::Status::Ok,
        "M8 beacon created");
  uint8_t wire[modes::kWireBytes]{};
  size_t written = 0U;
  check(modes::encode(beacon, wire, sizeof(wire), &written) ==
            modes::Status::Ok && written == modes::kWireBytes &&
            wire[0] == modes::kMagic0 && wire[1] == modes::kMagic1,
        "M8 codec writes one bounded LoRa frame");
  modes::Packet decoded{};
  check(modes::decode(wire, sizeof(wire), decoded) == modes::Status::Ok &&
            modes::packetFingerprint(decoded) ==
                modes::packetFingerprint(beacon),
        "M8 codec round trips semantic fields");
  wire[21] ^= 0x01U;
  check(modes::decode(wire, sizeof(wire), decoded) ==
            modes::Status::IntegrityMismatch,
        "M8 CRC rejects damaged payload");

  legacy::Packet p8{};
  p8.type = legacy::PacketType::Beacon;
  p8.sourceUid = 1U;
  p8.hostUid = 1U;
  p8.sessionNonce = 2U;
  p8.sequence = 1U;
  p8.participantCount = 1U;
  p8.flags = legacy::kFlagJoinOpen;
  p8.dataA = 3U;
  p8.windowSeconds = 10U;
  uint8_t p8Wire[legacy::kWireBytes]{};
  check(legacy::encode(p8, p8Wire, sizeof(p8Wire)) == legacy::Status::Ok &&
            p8Wire[0] != modes::kMagic0 &&
            modes::decode(p8Wire, sizeof(p8Wire), decoded) ==
                modes::Status::WrongLength,
        "62 existing P8 Signal Hunt remains a disjoint wire family");
  cover(62U, host.start(1U, 100U, modes::Mode::SignalHunt, 1U, 0U, 10U,
                        10U) == modes::Status::LegacySignalHunt,
        "62 M8 explicitly delegates Signal Hunt to legacy P8");
}

void testReadyAndReplay() {
  modes::HostSession host;
  modes::ParticipantSession guest;
  check(host.start(0x1001U, 101U, modes::Mode::AsyncRelay, 55U, 0U, 30U,
                   10U) == modes::Status::Ok,
        "ready session starts");
  modes::Packet beacon{};
  modes::Packet join{};
  modes::Packet welcome{};
  modes::Packet ready{};
  check(host.makeBeacon(0U, beacon) == modes::Status::Ok &&
            guest.observeBeacon(0xF002U, beacon, 0U) == modes::Status::Ok &&
            guest.makeJoinRequest(join) == modes::Status::Ok,
        "high-bit UID observes and requests a join");
  modes::Packet laterBeacon{};
  check(host.makeBeacon(0U, laterBeacon) == modes::Status::Ok &&
            guest.observeBeacon(0xF002U, laterBeacon, 0U) ==
                modes::Status::WrongPhase,
        "joining peer does not advance beyond its pending Welcome sequence");
  check(
            host.acceptJoin(join, 0U, welcome) == modes::Status::Ok &&
            guest.acceptHostPacket(welcome, 0U) == modes::Status::Ok,
        "high-bit UID joins without signed truncation");
  check(!host.canStart() && host.setHostReady(true) == modes::Status::Ok &&
            !host.canStart(),
        "party does not start while one member is unready");
  check(guest.makeReady(true, ready) == modes::Status::Ok &&
            host.acceptReady(ready) == modes::Status::Ok && host.canStart(),
        "guest ready completes unanimous ready check");
  const modes::HostState accepted = host.snapshot();
  check(host.acceptReady(ready) == modes::Status::Duplicate &&
            std::memcmp(&accepted, &host.state(), sizeof(accepted)) == 0,
        "replayed ready packet is idempotent");
  modes::Packet conflict = ready;
  conflict.value = 0;
  check(host.acceptReady(conflict) == modes::Status::SequenceConflict &&
            std::memcmp(&accepted, &host.state(), sizeof(accepted)) == 0,
        "same sequence with different ready content is rejected");
  modes::Packet readyBeacon{};
  check(host.makeBeacon(0U, readyBeacon) == modes::Status::Ok &&
            guest.acceptHostPacket(readyBeacon, 0U) == modes::Status::Ok &&
            guest.state().readyCount == 2U,
        "unanimous ready count is visible to admitted participants");
  modes::Packet open{};
  cover(59U, host.begin(1U, open) == modes::Status::Ok,
        "59 only a unanimous ready party enters round one");
}

void testLateJoin() {
  PairSession pair{};
  check(preparePair(modes::Mode::AsyncRelay, pair, 202U, 303U),
        "late-join relay prepares");
  modes::ParticipantSession late;
  modes::Packet activeBeacon{};
  modes::Packet join{};
  modes::Packet welcome{};
  check(pair.host.makeBeacon(pair.now, activeBeacon) == modes::Status::Ok &&
            (activeBeacon.flags & modes::kFlagJoinOpen) != 0U &&
            (activeBeacon.flags & modes::kFlagActive) != 0U &&
            late.observeBeacon(0x3003U, activeBeacon, pair.now) ==
                modes::Status::Ok &&
            late.makeJoinRequest(join) == modes::Status::Ok &&
            pair.host.acceptJoin(join, pair.now, welcome) ==
                modes::Status::Ok &&
            late.acceptHostPacket(welcome, pair.now) == modes::Status::Ok &&
            late.state().phase ==
                static_cast<uint8_t>(modes::ParticipantPhase::Active) &&
            pair.host.state().members[2].joinedRound == 1U &&
            pair.host.state().members[2].ready == 1U,
        "late peer joins active round one with truthful state");

  for (uint8_t round = 1U; round <= 3U; ++round) {
    modes::Contribution hostValue{};
    modes::Contribution guestValue{};
    modes::Contribution lateValue{};
    hostValue.value = 700U;
    guestValue.value = 800U;
    lateValue.value = 900U;
    check(pair.host.submitHostContribution(hostValue) == modes::Status::Ok,
          "late session host contributes");
    modes::Packet guestPacket{};
    modes::Packet latePacket{};
    check(pair.guest.makeContribution(guestValue, guestPacket) ==
              modes::Status::Ok &&
              pair.host.acceptContribution(guestPacket, pair.now + 1U) ==
                  modes::Status::Ok &&
              late.makeContribution(lateValue, latePacket) ==
                  modes::Status::Ok &&
              pair.host.acceptContribution(latePacket, pair.now + 2U) ==
                  modes::Status::Ok,
          "late member contributes to current relay round");
    modes::Packet next{};
    pair.now += 100U;
    check(pair.host.advance(pair.now, next) == modes::Status::Ok &&
              pair.guest.acceptHostPacket(next, pair.now) ==
                  modes::Status::Ok &&
              late.acceptHostPacket(next, pair.now) == modes::Status::Ok,
          "late relay advances for every member");
    if (round == 2U) {
      modes::Packet closedBeacon{};
      modes::ParticipantSession tooLate;
      check(pair.host.makeBeacon(pair.now, closedBeacon) ==
                modes::Status::Ok &&
                (closedBeacon.flags & modes::kFlagJoinOpen) == 0U &&
                tooLate.observeBeacon(0x4004U, closedBeacon, pair.now) ==
                    modes::Status::JoinClosed,
            "round three beacon closes late join");
    }
  }
  cover(60U, pair.host.state().result.participantCount == 3U &&
                 pair.host.state().result.score == 800U,
        "60 bounded late join participates in the real scored transcript");
}

void testRotationAndScoredModes() {
  bool seen[static_cast<uint8_t>(modes::Mode::Count)]{};
  for (uint32_t day = 1U; day <= 500U; ++day) {
    seen[static_cast<uint8_t>(modes::rotatingMode(day, 0xCAFEU))] = true;
  }
  bool allSeen = true;
  for (uint8_t index = 0U;
       index < static_cast<uint8_t>(modes::Mode::Count); ++index) {
    allSeen = allSeen && seen[index];
  }
  cover(61U, allSeen, "61 rotating schedule reaches P8 and every M8 mode");

  const int16_t triHost[3] = {-70, -68, -69};
  const int16_t triGuest[3] = {-72, -70, -71};
  const modes::ModeResult triangulation = runThreeRoundSignalMode(
      modes::Mode::Triangulation, triHost, triGuest);
  cover(63U, triangulation.score > 300U && triangulation.proof != 0U,
        "63 real three-round radio samples produce triangulation result");

  const int16_t hotHost[3] = {-105, -91, -78};
  const int16_t hotGuest[3] = {-103, -92, -82};
  modes::ParticipantSession hotParticipant;
  const modes::ModeResult hot = runThreeRoundSignalMode(
      modes::Mode::HotCold, hotHost, hotGuest, &hotParticipant);
  cover(64U, hot.temperature == static_cast<uint8_t>(
                     kitsu868::social::TemperatureHint::MuchHotter) &&
                 hot.score == 1000U &&
                 hotParticipant.temperatureHint() ==
                     kitsu868::social::TemperatureHint::MuchHotter,
        "64 radio movement yields a much-hotter group result");

  PairSession hidePair{};
  check(preparePair(modes::Mode::HideAndSeek, hidePair, 304U, 405U),
        "hide-and-seek prepares");
  const uint8_t hidden = modes::promptFor(
      modes::Mode::HideAndSeek, hidePair.host.state().seed, 1U).hideSlot;
  check(hidePair.guest.prompt().hideSlot == modes::kHiddenSlotUnavailable &&
            hidePair.open.dataB == 0U,
        "hide-and-seek answer is not broadcast to guessing peers");
  modes::Contribution exact{};
  exact.value = hidden;
  modes::Packet hideResultPacket{};
  check(playRound(hidePair, exact, exact, hideResultPacket),
        "hide-and-seek exact guesses complete");
  cover(65U, hidePair.host.state().result.score == 1000U &&
                 hidePair.host.state().result.outcomeValue == hidden,
        "65 shared hidden slot scores real participant guesses");

  const modes::ModeResult sync = runSingleValueMode(
      modes::Mode::SyncRhythm, 100U, 140U);
  cover(66U, sync.score == 1000U,
        "66 synchronized radio-relative taps earn full group timing score");

  PairSession echoPair{};
  check(preparePair(modes::Mode::CoopEcho, echoPair, 406U, 507U),
        "cooperative echo prepares");
  const uint16_t echoPattern = echoPair.guest.prompt().echoPattern;
  modes::Contribution echo{};
  echo.value = echoPattern;
  modes::Packet echoResultPacket{};
  check(playRound(echoPair, echo, echo, echoResultPacket),
        "cooperative echo patterns complete");
  cover(67U, echoPair.host.state().result.score == 1000U &&
                 echoPair.host.state().result.outcomeValue == echoPattern,
        "67 every player's transmitted echo pattern is scored");

  PairSession relayPair{};
  check(preparePair(modes::Mode::AsyncRelay, relayPair, 508U, 609U),
        "relay prepares");
  modes::Packet relayPacket{};
  for (uint8_t round = 0U; round < 3U; ++round) {
    modes::Contribution hostRelay{};
    modes::Contribution guestRelay{};
    hostRelay.value = 700U;
    guestRelay.value = 900U;
    check(playRound(relayPair, hostRelay, guestRelay, relayPacket),
          "relay contribution round completes");
  }
  cover(68U, relayPair.host.state().result.score == 800U &&
                 relayPair.host.state().result.outcomeValue == 4800U,
        "68 asynchronous relay preserves and aggregates each contribution");

  modes::ParticipantSession rareGuest;
  const modes::ModeResult rare = runSingleValueMode(
      modes::Mode::RareEncounter, 900U, 850U, &rareGuest);
  cover(69U, rare.rareEncounter == 1U && rare.score == 875U &&
                 rareGuest.state().result.rareEncounter == 1U,
        "69 qualifying two-player transcript carries cooperative rare outcome");

  const modes::ModeResult trail = runSingleValueMode(
      modes::Mode::SharedTrail, 12U, 15U);
  cover(70U, trail.outcomeValue == 20U && trail.score == 1000U,
        "70 peer trail misses merge once with the global bounded cap");

  const modes::ModeResult story = runSingleValueMode(
      modes::Mode::StoryVote, 1U, 1U);
  cover(71U, story.storyChoice == 1U && story.outcomeValue == 1U &&
                 story.score == 1000U,
        "71 transmitted story votes resolve to the group majority");
}

void testPersistenceAndInvalidInputs() {
  PairSession pair{};
  check(preparePair(modes::Mode::HotCold, pair, 700U, 701U),
        "snapshot session prepares");
  check(modes::validateHostState(pair.host.snapshot()) &&
            modes::validateParticipantState(pair.guest.snapshot()),
        "active fixed-width session states validate");
  modes::HostSession restoredHost;
  modes::ParticipantSession restoredGuest;
  check(restoredHost.restore(pair.host.snapshot()) == modes::Status::Ok &&
            restoredGuest.restore(pair.guest.snapshot()) == modes::Status::Ok,
        "active fixed-width session states restore");
  modes::HostState badHost = pair.host.snapshot();
  badHost.members[1].submittedMask = 0x80U;
  check(restoredHost.restore(badHost) == modes::Status::InvalidState,
        "malformed host state fails closed");
  modes::ParticipantState badGuest = pair.guest.snapshot();
  badGuest.temperature = 0xffU;
  check(restoredGuest.restore(badGuest) == modes::Status::InvalidState,
        "malformed participant state fails closed");

  modes::Contribution invalid{};
  invalid.signalRssi = 1;
  check(!modes::validContribution(
            modes::Mode::Triangulation, invalid,
            modes::promptFor(modes::Mode::Triangulation, 1U, 1U)),
        "impossible positive RSSI contribution is rejected");
}

}  // namespace

int main() {
  static_assert(sizeof(modes::HostState) <= 256U,
                "host state memory budget regressed");
  static_assert(sizeof(modes::ParticipantState) <= 96U,
                "participant state memory budget regressed");
  static_assert(modes::kWireBytes == 32U,
                "M8 wire no longer fits direct LoRa budget");

  testCodecAndLegacyCompatibility();
  testReadyAndReplay();
  testLateJoin();
  testRotationAndScoredModes();
  testPersistenceAndInvalidInputs();

  for (uint8_t feature = 59U; feature <= 71U; ++feature) {
    if (!covered[feature]) {
      ++failures;
      std::cerr << "FAIL uncovered feature " << static_cast<unsigned>(feature)
                << '\n';
    }
  }
  if (failures != 0) {
    std::cerr << "TEST_FAIL kitsu_party_modes failures=" << failures << '\n';
    return 1;
  }
  std::cout << "TEST_PASS kitsu_party_modes features=59-71 wire=32 host="
            << sizeof(modes::HostState) << " guest="
            << sizeof(modes::ParticipantState) << '\n';
  return 0;
}
