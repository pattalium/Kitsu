#include "../src/kitsu_party_hotspot.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace party = kitsu868::party;

static_assert(party::kWireBytes == 30U, "party wire size changed");
static_assert(party::kWireBytes <= 32U, "party packet exceeds radio budget");
static_assert(party::kMaximumParticipants == 4U,
              "party roster bound changed");
static_assert(party::kHuntRounds == 3U, "signal hunt round count changed");

namespace {

int failures = 0;

void check(bool condition, const char* description) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL " << description << '\n';
  }
}

party::Packet beaconPacket() {
  party::Packet packet{};
  packet.type = party::PacketType::Beacon;
  packet.sourceUid = 0x1001U;
  packet.hostUid = 0x1001U;
  packet.sessionNonce = UINT32_C(0x11223344);
  packet.sequence = 1U;
  packet.participantCount = 1U;
  packet.flags = party::kFlagJoinOpen;
  packet.dataA = UINT32_C(0x55667788);
  packet.windowSeconds = 120U;
  return packet;
}

void refreshCrc(std::array<uint8_t, party::kWireBytes>& wire) {
  const uint16_t crc = party::crc16CcittFalse(wire.data(), 28U);
  wire[28] = static_cast<uint8_t>(crc);
  wire[29] = static_cast<uint8_t>(crc >> 8U);
}

void testWireContractAndValidation() {
  static constexpr std::array<uint8_t, party::kWireBytes> expected = {
      0x50, 0x38, 0x01, 0x01, 0x01, 0x10, 0x01, 0x10, 0x44, 0x33,
      0x22, 0x11, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x88, 0x77,
      0x66, 0x55, 0x00, 0x00, 0x00, 0x00, 0x78, 0x00, 0x6F, 0x87,
  };
  std::array<uint8_t, party::kWireBytes> wire{};
  size_t written = 99U;
  check(party::encode(beaconPacket(), wire.data(), wire.size(), &written) ==
            party::Status::Ok,
        "party beacon encodes");
  check(written == party::kWireBytes && wire == expected,
        "party beacon matches golden wire bytes");
  check(wire[0] == 'P' && wire[1] == '8' &&
            party::crc16CcittFalse(wire.data(), 28U) == 0x876FU,
        "party wire has distinct P8 magic and golden CRC");

  party::Packet decoded{};
  check(party::decode(wire.data(), wire.size(), decoded) ==
            party::Status::Ok &&
            decoded.type == party::PacketType::Beacon &&
            decoded.sourceUid == 0x1001U &&
            decoded.sessionNonce == UINT32_C(0x11223344) &&
            decoded.dataA == UINT32_C(0x55667788) &&
            decoded.windowSeconds == 120U,
        "party beacon round-trips all semantic fields");
  check(party::packetFingerprint(decoded) != 0U,
        "valid party packet has a non-zero replay fingerprint");

  for (size_t index = 0U; index < wire.size(); ++index) {
    std::array<uint8_t, party::kWireBytes> damaged = wire;
    damaged[index] ^= 0x01U;
    party::Packet ignored{};
    if (party::decode(damaged.data(), damaged.size(), ignored) ==
        party::Status::Ok) {
      ++failures;
      std::cerr << "FAIL one-bit corruption accepted at byte " << index
                << '\n';
    }
  }
  check(party::decode(nullptr, wire.size(), decoded) ==
            party::Status::NullArgument &&
            party::decode(wire.data(), wire.size() - 1U, decoded) ==
                party::Status::WrongLength,
        "party decoder rejects null and wrong-length input");

  std::array<uint8_t, party::kWireBytes> changed = wire;
  changed[0] = 'K';
  refreshCrc(changed);
  check(party::decode(changed.data(), changed.size(), decoded) ==
            party::Status::BadMagic,
        "nearby-family magic is rejected by party decoder");
  changed = wire;
  changed[2] = 2U;
  refreshCrc(changed);
  check(party::decode(changed.data(), changed.size(), decoded) ==
            party::Status::UnsupportedVersion,
        "unknown party protocol version is rejected");
  changed = wire;
  changed[17] = 0x80U;
  refreshCrc(changed);
  check(party::decode(changed.data(), changed.size(), decoded) ==
            party::Status::InvalidFlags,
        "unknown beacon flags are rejected");

  party::Packet invalid = beaconPacket();
  invalid.sourceUid = 0U;
  check(party::validate(invalid) == party::Status::InvalidSourceUid,
        "zero source UID is rejected");
  invalid = beaconPacket();
  invalid.sourceUid = 0x2222U;
  check(party::validate(invalid) == party::Status::InvalidHostUid,
        "host-owned packet requires host source");
  invalid = beaconPacket();
  invalid.sessionNonce = 0U;
  check(party::validate(invalid) == party::Status::InvalidSessionNonce,
        "zero session nonce is rejected");
  invalid = beaconPacket();
  invalid.sequence = 0U;
  check(party::validate(invalid) == party::Status::InvalidSequence,
        "zero sequence is rejected");
  invalid = beaconPacket();
  invalid.participantCount = 5U;
  check(party::validate(invalid) ==
            party::Status::InvalidParticipantCount,
        "oversized party count is rejected");
  invalid = beaconPacket();
  invalid.windowSeconds = party::kMaximumWindowSeconds + 1U;
  check(party::validate(invalid) == party::Status::InvalidWindow,
        "oversized radio window is rejected");

  size_t unchanged = 77U;
  check(party::encode(beaconPacket(), nullptr, wire.size(), &unchanged) ==
            party::Status::NullArgument &&
            unchanged == 0U,
        "encoder rejects null output transactionally");
  check(std::strcmp(party::statusName(party::Status::IntegrityMismatch),
                    "integrity_mismatch") == 0,
        "party protocol status names are stable");
}

void acceptOpenForAll(const party::Packet& open,
                      std::array<party::ParticipantSession, 3>& clients) {
  for (party::ParticipantSession& client : clients) {
    check(client.acceptHostPacket(open, 2000U + open.round) ==
              party::SessionStatus::Ok,
          "participant accepts host round-open packet");
  }
}

void testHostJoinThreeRoundLifecycleAndReplay() {
  party::HostSession host;
  check(host.start(0x1001U, UINT32_C(0x11223344),
                   UINT32_C(0x55667788), 1000U, 120U, 30U) ==
            party::SessionStatus::Ok &&
            party::validateHostState(host.snapshot()),
        "host starts a serializable temporary lobby");

  party::Packet beacon{};
  check(host.makeBeacon(1000U, beacon) == party::SessionStatus::Ok &&
            beacon.sequence == 1U &&
            beacon.flags == party::kFlagJoinOpen,
        "host emits an open hotspot beacon");

  std::array<party::ParticipantSession, 3> clients{};
  std::array<uint16_t, 3> clientUids = {0x1002U, 0x1003U, 0x1004U};
  std::array<party::Packet, 3> joins{};
  for (size_t index = 0U; index < clients.size(); ++index) {
    check(clients[index].observeBeacon(clientUids[index], beacon, 1000U) ==
              party::SessionStatus::Ok &&
              clients[index].makeJoinRequest(joins[index]) ==
                  party::SessionStatus::Ok,
          "participant observes hotspot and creates bounded join request");
    party::Packet welcome{};
    check(host.acceptJoin(joins[index], 1100U, welcome) ==
              party::SessionStatus::Ok &&
              welcome.dataA == clientUids[index] &&
              welcome.value == index + 1U &&
              clients[index].acceptHostPacket(welcome, 1100U) ==
                  party::SessionStatus::Ok,
          "host assigns deterministic slot and participant joins");
  }
  check(host.state().participantCount == party::kMaximumParticipants &&
            party::validateHostState(host.snapshot()),
        "host caps the roster at four including itself");
  party::Packet ignored{};
  check(host.acceptJoin(joins[0], 1200U, ignored) ==
            party::SessionStatus::Duplicate &&
            host.state().participantCount == 4U,
        "exact join replay is idempotent");

  party::ParticipantSession extra;
  party::Packet extraJoin{};
  check(extra.observeBeacon(0x1005U, beacon, 1000U) ==
            party::SessionStatus::Ok &&
            extra.makeJoinRequest(extraJoin) == party::SessionStatus::Ok &&
            host.acceptJoin(extraJoin, 1200U, ignored) ==
                party::SessionStatus::PartyFull,
        "fifth participant is rejected without expanding state");

  party::Packet open{};
  check(host.beginHunt(2000U, open) == party::SessionStatus::Ok &&
            open.type == party::PacketType::RoundOpen && open.round == 1U,
        "four-player lobby starts asynchronous round one");
  acceptOpenForAll(open, clients);
  check(host.submitHostChoice(1U, party::SignalChoice::Sweep) ==
            party::SessionStatus::Ok,
        "host records one local choice");

  const std::array<party::SignalChoice, 3> roundOne = {
      party::SignalChoice::Listen, party::SignalChoice::Pulse,
      party::SignalChoice::Sweep};
  std::array<party::Packet, 3> firstChoices{};
  for (size_t index = 0U; index < clients.size(); ++index) {
    check(clients[index].makeChoice(roundOne[index], firstChoices[index]) ==
              party::SessionStatus::Ok &&
              host.acceptChoice(firstChoices[index], 2100U) ==
                  party::SessionStatus::Ok,
          "each joined peer contributes one round-one choice");
  }
  check(host.acceptChoice(firstChoices[0], 2101U) ==
            party::SessionStatus::Duplicate,
        "exact choice retransmission cannot score twice");
  party::Packet stale = firstChoices[0];
  stale.sequence = 1U;
  check(host.acceptChoice(stale, 2101U) ==
            party::SessionStatus::StaleSequence,
        "older sender sequence is rejected without changing a choice");
  party::Packet conflict = firstChoices[0];
  conflict.value = static_cast<uint8_t>(party::SignalChoice::Pulse);
  check(host.acceptChoice(conflict, 2101U) ==
            party::SessionStatus::SequenceConflict,
        "same sequence with changed choice is a conflict");
  conflict.sequence = static_cast<uint16_t>(conflict.sequence + 1U);
  check(host.acceptChoice(conflict, 2101U) ==
            party::SessionStatus::ChoiceAlreadySubmitted,
        "new sequence cannot reroll an already submitted round");

  check(host.advance(2102U, open) == party::SessionStatus::Ok &&
            open.round == 2U,
        "all answers advance round one before its timeout");
  acceptOpenForAll(open, clients);
  check(host.submitHostChoice(2U, party::SignalChoice::Pulse) ==
            party::SessionStatus::Ok,
        "host contributes to round two");
  party::Packet secondChoice{};
  check(clients[0].makeChoice(party::SignalChoice::Sweep, secondChoice) ==
            party::SessionStatus::Ok &&
            host.acceptChoice(secondChoice, 2200U) ==
                party::SessionStatus::Ok,
        "one peer can answer an asynchronous round");
  check(host.advance(30000U, open) == party::SessionStatus::NotReady,
        "host waits while unanswered round window remains");
  check(host.advance(32102U, open) == party::SessionStatus::Ok &&
            open.round == 3U,
        "round timeout advances with missing choices left unscored");
  acceptOpenForAll(open, clients);

  check(host.submitHostChoice(3U, party::SignalChoice::Listen) ==
            party::SessionStatus::Ok,
        "host contributes final choice");
  const std::array<party::SignalChoice, 3> roundThree = {
      party::SignalChoice::Listen, party::SignalChoice::Sweep,
      party::SignalChoice::Pulse};
  for (size_t index = 0U; index < clients.size(); ++index) {
    party::Packet choice{};
    check(clients[index].makeChoice(roundThree[index], choice) ==
              party::SessionStatus::Ok &&
              host.acceptChoice(choice, 32200U) ==
                  party::SessionStatus::Ok,
          "all peers contribute a final-round choice");
  }
  party::Packet resultPacket{};
  check(host.advance(32201U, resultPacket) == party::SessionStatus::Ok &&
            resultPacket.type == party::PacketType::Result &&
            host.state().result.proof != 0U &&
            host.state().result.participantCount == 4U &&
            party::validateHostState(host.snapshot()),
        "round three resolves one deterministic bounded result");
  for (party::ParticipantSession& client : clients) {
    check(client.acceptHostPacket(resultPacket, 32202U) ==
              party::SessionStatus::Ok &&
              client.state().phase ==
                  static_cast<uint8_t>(party::SessionPhase::Complete) &&
              client.state().result.proof == host.state().result.proof,
          "participants accept the same final proof");
  }
  check(clients[0].acceptHostPacket(resultPacket, 32203U) ==
            party::SessionStatus::Duplicate &&
            clients[0].state().result.proof == host.state().result.proof,
        "participant treats repeated host result idempotently");

  std::array<party::HuntParticipant, 4> reversed{};
  for (size_t index = 0U; index < reversed.size(); ++index) {
    reversed[index] = host.state().participants[reversed.size() - 1U - index]
                          .hunt;
  }
  party::HuntResult recomputed{};
  check(party::resolveSignalHunt(host.state().sessionNonce,
                                 host.state().seed, reversed.data(), 4U,
                                 recomputed) &&
            recomputed.proof == host.state().result.proof &&
            recomputed.score == host.state().result.score,
        "result and proof are deterministic across roster ordering");

  party::HostSession restoredHost;
  check(restoredHost.restore(host.snapshot()) == party::SessionStatus::Ok &&
            restoredHost.state().result.proof == host.state().result.proof,
        "valid host semantic state restores exactly");
  party::HostState invalidHost = host.snapshot();
  invalidHost.participantCount = 5U;
  check(restoredHost.restore(invalidHost) ==
            party::SessionStatus::InvalidState &&
            restoredHost.makeBeacon(0U, ignored) ==
                party::SessionStatus::StateUnavailable,
        "invalid host restore quarantines the temporary session");

  party::ParticipantSession restoredClient;
  check(restoredClient.restore(clients[0].snapshot()) ==
            party::SessionStatus::Ok,
        "valid participant semantic state restores exactly");
  party::ParticipantState invalidClient = clients[0].snapshot();
  invalidClient.reserved = 1U;
  check(restoredClient.restore(invalidClient) ==
            party::SessionStatus::InvalidState &&
            restoredClient.makeChoice(party::SignalChoice::Sweep, ignored) ==
                party::SessionStatus::StateUnavailable,
        "invalid participant restore fails closed");
}

void testTimeoutsAndMultiplayerSignalIncentive() {
  party::HostSession lonely;
  party::Packet packet{};
  check(lonely.start(1U, 2U, 3U, 100U, 1U, 1U) ==
            party::SessionStatus::Ok &&
            lonely.beginHunt(200U, packet) == party::SessionStatus::NotReady &&
            lonely.makeBeacon(1100U, packet) ==
                party::SessionStatus::TimedOut &&
            lonely.state().phase ==
                static_cast<uint8_t>(party::SessionPhase::Expired),
        "temporary lobby expires and cannot start solo");

  party::ParticipantSession waiting;
  party::Packet beacon = beaconPacket();
  beacon.windowSeconds = 1U;
  check(waiting.observeBeacon(0x2002U, beacon, 500U) ==
            party::SessionStatus::Ok &&
            waiting.expireIfNeeded(1500U) ==
                party::SessionStatus::TimedOut,
        "joining client applies the advertised timeout");

  std::array<party::HuntParticipant, 4> players{};
  for (uint8_t index = 0U; index < 4U; ++index) {
    players[index].uid = static_cast<uint16_t>(0x3001U + index);
    players[index].submittedMask = 0x07U;
    for (uint8_t round = 0U; round < party::kHuntRounds; ++round) {
      players[index].choices[round] =
          static_cast<uint8_t>(party::SignalChoice::Sweep);
    }
  }
  party::HuntResult probe{};
  check(party::resolveSignalHunt(9U, 10U, players.data(), 4U, probe),
        "target choices can be derived from a valid transcript");
  for (uint8_t index = 0U; index < 4U; ++index) {
    for (uint8_t round = 0U; round < party::kHuntRounds; ++round) {
      players[index].choices[round] = probe.targetChoices[round];
    }
  }
  party::HuntResult duo{};
  party::HuntResult full{};
  check(party::resolveSignalHunt(9U, 10U, players.data(), 2U, duo) &&
            party::resolveSignalHunt(9U, 10U, players.data(), 4U, full) &&
            full.score > duo.score &&
            full.tier == static_cast<uint8_t>(party::ResultTier::Resonant) &&
            duo.tier == static_cast<uint8_t>(party::ResultTier::Found),
        "a full cooperating party has a concrete signal-strength incentive");
}

party::CompletedHuntReward completedReward(
    uint32_t nonce, uint32_t day, uint32_t epoch,
    std::array<uint16_t, 4> uids = {0x1001U, 0x1002U, 0x1003U, 0x1004U},
    uint8_t count = 4U,
    party::ResultTier tier = party::ResultTier::Resonant) {
  party::CompletedHuntReward completed{};
  completed.selfUid = 0x1001U;
  completed.participantCount = count;
  completed.tier = static_cast<uint8_t>(tier);
  completed.sessionNonce = nonce;
  completed.resultProof = nonce ^ UINT32_C(0xA5A5A5A5);
  if (completed.resultProof == 0U) completed.resultProof = 1U;
  completed.dayId = day;
  completed.nowEpochSeconds = epoch;
  for (size_t index = 0U; index < uids.size(); ++index) {
    completed.participantUids[index] = uids[index];
  }
  return completed;
}

void testDailyUniqueRewardsBondAndStreak() {
  party::PartyRewardLedger ledger;
  party::RewardOutcome outcome{};
  const party::CompletedHuntReward first =
      completedReward(101U, 100U, 100000U);
  check(ledger.recordCompletedHunt(
            first, party::kDefaultPeerRewardCooldownSeconds, outcome) ==
            party::RewardStatus::Awarded &&
            outcome.eligibleUniquePeers == 3U && outcome.bondAwarded == 14U &&
            outcome.partyBondAfter == 14U && outcome.streakAdvanced == 1U &&
            outcome.currentStreakDays == 1U &&
            party::validateRewardState(ledger.snapshot()),
        "four-player resonant hunt rewards three unique peers and Party Bond");
  const party::RewardState afterFirst = ledger.snapshot();
  check(ledger.recordCompletedHunt(
            first, party::kDefaultPeerRewardCooldownSeconds, outcome) ==
            party::RewardStatus::DuplicateSession &&
            ledger.state().completedHunts == afterFirst.completedHunts &&
            ledger.state().partyBond == afterFirst.partyBond,
        "completed-session retransmission cannot duplicate rewards or stats");

  check(ledger.recordCompletedHunt(
            completedReward(102U, 100U, 100100U),
            party::kDefaultPeerRewardCooldownSeconds, outcome) ==
            party::RewardStatus::RecordedNoEligiblePeer &&
            ledger.state().completedHunts == 2U &&
            ledger.state().partyBond == 14U,
        "same peers on the same day do not farm Party Bond");

  const std::array<uint16_t, 4> oneNewPeer = {
      0x1001U, 0x1002U, 0x2000U, 0U};
  check(ledger.recordCompletedHunt(
            completedReward(103U, 100U, 100200U, oneNewPeer, 3U,
                            party::ResultTier::Found),
            party::kDefaultPeerRewardCooldownSeconds, outcome) ==
            party::RewardStatus::Awarded &&
            outcome.eligibleUniquePeers == 1U && outcome.bondAwarded == 4U &&
            outcome.streakAdvanced == 0U && outcome.partyBondAfter == 18U,
        "new same-day peer remains rewarding while an old peer does not");

  check(ledger.recordCompletedHunt(
            completedReward(104U, 101U, 100300U),
            party::kDefaultPeerRewardCooldownSeconds, outcome) ==
            party::RewardStatus::RecordedNoEligiblePeer,
        "day-boundary replay farming is blocked by per-peer cooldown");
  check(ledger.recordCompletedHunt(
            completedReward(105U, 101U, 101000U),
            party::kDefaultPeerRewardCooldownSeconds, outcome) ==
            party::RewardStatus::Awarded &&
            outcome.eligibleUniquePeers == 3U && outcome.bondAwarded == 14U &&
            outcome.streakAdvanced == 1U && outcome.currentStreakDays == 2U &&
            outcome.longestStreakDays == 2U && outcome.partyBondAfter == 32U,
        "next-day eligible party advances bond and daily streak once");

  check(ledger.recordCompletedHunt(
            completedReward(106U, 100U, 102000U),
            party::kDefaultPeerRewardCooldownSeconds, outcome) ==
            party::RewardStatus::ClockRegression,
        "day regression fails closed");
  party::CompletedHuntReward duplicateUid =
      completedReward(107U, 102U, 102000U);
  duplicateUid.participantUids[2] = duplicateUid.participantUids[1];
  check(ledger.recordCompletedHunt(
            duplicateUid, party::kDefaultPeerRewardCooldownSeconds,
            outcome) == party::RewardStatus::InvalidInput,
        "duplicate roster identity is rejected before reward accounting");

  party::PartyRewardLedger restored;
  check(restored.restore(ledger.snapshot()) == party::RewardStatus::Awarded &&
            restored.state().partyBond == ledger.state().partyBond &&
            restored.state().currentStreakDays == 2U,
        "valid bounded reward ledger restores exactly");
  party::RewardState invalid = ledger.snapshot();
  invalid.peerCount = static_cast<uint8_t>(party::kRewardPeerCapacity + 1U);
  check(restored.restore(invalid) == party::RewardStatus::InvalidState &&
            restored.recordCompletedHunt(
                completedReward(108U, 102U, 102000U),
                party::kDefaultPeerRewardCooldownSeconds, outcome) ==
                party::RewardStatus::StateUnavailable,
        "invalid reward restore quarantines incentives instead of resetting");
  check(std::strcmp(
            party::rewardStatusName(party::RewardStatus::DuplicateSession),
            "duplicate_session") == 0 &&
            std::strcmp(
                party::sessionStatusName(party::SessionStatus::PartyFull),
                "party_full") == 0,
        "party session and reward status names are stable");
}

}  // namespace

int main() {
  testWireContractAndValidation();
  testHostJoinThreeRoundLifecycleAndReplay();
  testTimeoutsAndMultiplayerSignalIncentive();
  testDailyUniqueRewardsBondAndStreak();

  if (failures != 0) {
    std::cerr << "TEST_FAIL kitsu_party_hotspot failures=" << failures
              << '\n';
    return 1;
  }
  std::cout << "TEST_PASS kitsu_party_hotspot version=1 wire_bytes=30 "
               "party=2..4 rounds=3 replay=strict timeout=bounded "
               "result=deterministic rewards=unique_daily_cooldown "
               "bond=enabled streak=enabled\n";
  return 0;
}
