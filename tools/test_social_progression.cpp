#include "../src/social_progression.h"

#include <cstring>
#include <iostream>

namespace social = kitsu868::social;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL " << message << '\n';
  }
}

social::FriendObservation observation(uint16_t uid, uint32_t day,
                                      uint32_t epoch) {
  social::FriendObservation value{};
  value.uid = uid;
  value.dayId = day;
  value.epochSeconds = epoch;
  return value;
}

const social::PeerState* peerState(const social::SocialState& state,
                                   uint16_t uid) {
  for (uint8_t index = 0U; index < state.peerCount; ++index) {
    if (state.peers[index].uid == uid) return &state.peers[index];
  }
  return nullptr;
}

void testFriendshipAndFairness() {
  social::SocialProgression progression;
  social::FriendOutcome result{};
  social::FriendObservation first =
      observation(0x1001U, 21000U, 1814400000U);
  check(progression.observeFriend(first, result) == social::SocialStatus::Ok &&
            result.event == social::FriendEvent::FirstMeeting &&
            result.pointsAwarded == 5U && result.friendshipLevel == 0U,
        "53/55 per-peer friendship records a first-meeting bonus");
  check(result.greeting == social::Greeting::NewSignal &&
            std::strcmp(social::greetingLine1(result.greeting),
                        "NEW SIGNAL") == 0,
        "54 known-friend greeting catalogue has a truthful first greeting");

  check(progression.observeFriend(first, result) == social::SocialStatus::Ok &&
            result.event == social::FriendEvent::KnownFriend &&
            result.pointsAwarded == 0U,
        "75 repeating the same peer on the same day cannot farm points");

  social::FriendObservation reunion =
      observation(0x1001U, 21008U, 1815091200U);
  check(progression.observeFriend(reunion, result) == social::SocialStatus::Ok &&
            result.event == social::FriendEvent::Reunion &&
            result.pointsAwarded == 3U &&
            result.greeting == social::Greeting::FriendReturned,
        "56 seven-day reunion gives its bounded bonus and greeting");

  social::FriendObservation second =
      observation(0x1002U, 21008U, 1815091201U);
  social::FriendObservation newcomer =
      observation(0x1003U, 21008U, 1815091202U);
  newcomer.peerIsNewcomer = 1U;
  newcomer.localBondLevel = 3U;
  check(progression.observeFriend(second, result) == social::SocialStatus::Ok,
        "second peer is recorded");
  check(progression.observeFriend(newcomer, result) == social::SocialStatus::Ok &&
            result.dailyUniquePeers == 3U && result.diversityBonus == 2U &&
            result.mentorBonus == 2U &&
            result.greeting == social::Greeting::MentorWelcome,
        "57/58 diversity and newcomer mentoring bonuses compose once");

  social::SessionReward reward{};
  reward.uid = 0x1001U;
  reward.score = 800U;
  reward.sessionNonce = 77U;
  reward.dayId = 21008U;
  reward.epochSeconds = 1815092200U;
  social::RewardOutcome rewardResult{};
  check(progression.recordSessionReward(reward, rewardResult) ==
            social::SocialStatus::Ok &&
            rewardResult.pointsAwarded == 4U &&
            rewardResult.firstSuccess == 1U,
        "53 party success advances only that peer friendship");
  check(progression.recordSessionReward(reward, rewardResult) ==
            social::SocialStatus::Duplicate,
        "75 replayed session cannot award twice");
  reward.sessionNonce = 78U;
  reward.epochSeconds += 2000U;
  check(progression.recordSessionReward(reward, rewardResult) ==
            social::SocialStatus::RateLimited,
        "75 same-peer daily farming is rate limited");
  reward.sessionNonce = 79U;
  reward.dayId = 21007U;
  reward.epochSeconds += 2000U;
  check(progression.recordSessionReward(reward, rewardResult) ==
            social::SocialStatus::ClockRegression,
        "75 an older day cannot bypass the per-peer daily reward limit");

  social::SocialState state = progression.snapshot();
  check(social::validateSocialState(state),
        "friendship persistence record validates with CRC");
  social::SocialProgression restored;
  check(restored.restore(state) == social::SocialStatus::Ok,
        "friendship persistence restores");
  state.crc32 ^= 1U;
  check(restored.restore(state) == social::SocialStatus::StateUnavailable &&
            !restored.available(),
        "corrupt friendship state fails closed");
}

void testLobbyAndRotatingModes() {
  social::PartyLobby lobby;
  check(lobby.begin(1U, 123U, social::HotspotMode::SignalHunt) ==
            social::SocialStatus::Ok &&
            lobby.join(2U) == social::SocialStatus::Ok && !lobby.canStart(),
        "59 party waits for every member ready");
  check(lobby.setReady(1U, true) == social::SocialStatus::Ok &&
            lobby.setReady(2U, true) == social::SocialStatus::Ok &&
            lobby.canStart() && lobby.start() == social::SocialStatus::Ok,
        "59 ready check opens the round only after unanimous readiness");
  check(lobby.joinInProgress(3U) == social::SocialStatus::Ok &&
            lobby.state().members[2].joinedRound == 1U,
        "60 a late peer can join during the bounded early round");
  check(lobby.advanceRound() == social::SocialStatus::Ok &&
            lobby.advanceRound() == social::SocialStatus::Ok &&
            lobby.joinInProgress(4U) == social::SocialStatus::JoinClosed,
        "60 join-in-progress closes after round two");

  bool observed[social::kModeCount]{};
  for (uint32_t day = 1U; day < 300U; ++day) {
    observed[static_cast<uint8_t>(social::rotatingHotspotMode(day, 0xCAFEU))] =
        true;
  }
  for (uint8_t index = 0U; index < social::kModeCount; ++index) {
    check(observed[index], "61 rotating hotspot schedule reaches every mode");
  }
  check(std::strcmp(social::hotspotModeName(social::HotspotMode::SignalHunt),
                    "signal_hunt") == 0,
        "62 cooperative signal hunt remains an explicit hotspot mode");
}

void testCooperativeActivities() {
  check(social::triangulationScore(-70, -68, -69) >
            social::triangulationScore(-100, -82, -115),
        "63 stable strong samples score above scattered weak triangulation");
  check(social::temperatureHint(-90, -79) ==
            social::TemperatureHint::MuchHotter &&
            social::temperatureHint(-79, -90) ==
            social::TemperatureHint::MuchColder,
        "64 hot-and-cold hints follow signal movement");
  check(social::hideAndSeekScore(2U, 2U, 8U) == 1000U &&
            social::hideAndSeekScore(2U, 5U, 8U) < 1000U,
        "65 radio hide-and-seek rewards the correct circular slot");

  const uint32_t tightTaps[3] = {1000U, 1040U, 1070U};
  const uint32_t looseTaps[3] = {1000U, 1400U, 1750U};
  const uint32_t wrappedTaps[2] = {UINT32_MAX - 20U, 10U};
  check(social::synchronizedTapScore(tightTaps, 3U, 80U, 600U) == 1000U &&
            social::synchronizedTapScore(looseTaps, 3U, 80U, 600U) == 0U &&
            social::synchronizedTapScore(wrappedTaps, 2U, 40U, 100U) == 1000U,
        "66 synchronized tap uses the group timing spread");

  const uint16_t echoPerfect[3] = {0x15U, 0x15U, 0x15U};
  const uint16_t echoMixed[3] = {0x15U, 0x14U, 0x00U};
  const uint16_t echoOutOfRange[2] = {0xFFFFU, 0x00U};
  check(social::cooperativeEchoScore(0x15U, echoPerfect, 3U, 5U) == 1000U &&
            social::cooperativeEchoScore(0x15U, echoMixed, 3U, 5U) < 1000U &&
            social::cooperativeEchoScore(0U, echoOutOfRange, 2U, 5U) == 0U,
        "67 cooperative Echo Beat scores every player's pattern");

  social::PartyLobby relay;
  check(relay.begin(1U, 999U, social::HotspotMode::AsyncRelay) ==
            social::SocialStatus::Ok &&
            relay.join(2U) == social::SocialStatus::Ok &&
            relay.setReady(1U, true) == social::SocialStatus::Ok &&
            relay.setReady(2U, true) == social::SocialStatus::Ok &&
            relay.start() == social::SocialStatus::Ok &&
            relay.addScore(1U, 250U) == social::SocialStatus::Ok &&
            relay.addScore(2U, 300U) == social::SocialStatus::Ok,
        "68 asynchronous relay retains each peer contribution");

  social::SocialProgression progression;
  check(progression.cooperativeRareEncounterEligible(2U, 800U, 1000U) &&
            progression.cooperativeRareEncounterEligible(4U, 800U, 1000U) &&
            !progression.cooperativeRareEncounterEligible(1U, 1000U, 1000U),
        "69 rare cooperative encounter accepts the full 2-4 member party");
  uint8_t merged = 0U;
  check(progression.mergeSharedTrail(333U, 12U, 15U, merged) ==
            social::SocialStatus::Ok && merged == 20U &&
            progression.mergeSharedTrail(333U, 1U, 1U, merged) ==
            social::SocialStatus::Duplicate,
        "70 shared Signal Trail merges once and keeps the global cap");

  const uint8_t clearVote[3] = {1U, 3U, 2U};
  const uint8_t tiedVote[3] = {2U, 2U, 0U};
  check(social::storyVoteWinner(clearVote, 1U) == 1U &&
            social::storyVoteWinner(tiedVote, 7U) < 2U,
        "71 group story vote resolves majority and deterministic ties");

  uint8_t completed = 0U;
  check(progression.contributeDailyChallenge(22000U, 2U, 5U, completed) ==
            social::SocialStatus::Ok && completed == 0U &&
            progression.contributeDailyChallenge(22000U, 3U, 5U, completed) ==
            social::SocialStatus::Ok && completed == 1U &&
            progression.contributeDailyChallenge(22000U, 1U, 5U, completed) ==
            social::SocialStatus::Ok && completed == 0U,
        "72/74 challenge board completes exactly once per shared day");
}

void testLeaderboard() {
  social::SocialProgression progression;
  social::FriendOutcome friendResult{};
  for (uint16_t uid = 1U; uid <= 3U; ++uid) {
    social::FriendObservation value =
        observation(uid, 23000U, 1817000000U + uid);
    check(progression.observeFriend(value, friendResult) ==
              social::SocialStatus::Ok,
          "leaderboard peer added");
  }
  for (uint16_t uid = 1U; uid <= 3U; ++uid) {
    social::SessionReward reward{};
    reward.uid = uid;
    reward.score = static_cast<uint16_t>(uid * 200U);
    reward.sessionNonce = 100U + uid;
    reward.dayId = 23001U;
    reward.epochSeconds = 1817087000U + uid;
    social::RewardOutcome output{};
    check(progression.recordSessionReward(reward, output) ==
              social::SocialStatus::Ok,
          "leaderboard reward added");
  }
  social::LeaderboardEntry entries[3]{};
  check(progression.leaderboard(entries, 3U) == 3U &&
            entries[0].uid == 3U && entries[1].uid == 2U &&
            entries[2].uid == 1U,
        "73 local party leaderboard orders bounded peer records");
}

void testIntegrityEdges() {
  social::SocialProgression full;
  social::FriendOutcome friendResult{};
  for (uint16_t uid = 1U; uid <= social::kPeerCapacity; ++uid) {
    check(full.observeFriend(observation(uid, 24000U, 1818000000U + uid),
                             friendResult) == social::SocialStatus::Ok,
          "bounded peer table fills");
  }
  const social::SocialState beforeFull = full.snapshot();
  const social::SocialStatus fullStatus = full.observeFriend(
      observation(99U, 24001U, 1818086400U), friendResult);
  const social::SocialState afterFull = full.snapshot();
  check(fullStatus == social::SocialStatus::StateFull &&
            std::memcmp(&beforeFull, &afterFull,
                        sizeof(social::SocialState)) == 0 &&
            social::validateSocialState(afterFull),
        "53 rejected full-table observation leaves persistence unchanged");

  social::SocialProgression rewards;
  check(rewards.observeFriend(observation(1U, 25000U, 1819000000U),
                              friendResult) == social::SocialStatus::Ok,
        "reward peer added");
  social::SessionReward reward{};
  reward.uid = 1U;
  reward.score = 1000U;
  reward.sessionNonce = 500U;
  reward.dayId = 25001U;
  reward.epochSeconds = 1819086400U;
  social::RewardOutcome rewardResult{};
  check(rewards.recordSessionReward(reward, rewardResult) ==
            social::SocialStatus::Ok &&
            rewards.snapshot().currentDay == 25001U,
        "75 accepted reward advances the monotonic social day");
  reward.sessionNonce = 501U;
  reward.dayId = 25000U;
  reward.epochSeconds += 2000U;
  check(rewards.recordSessionReward(reward, rewardResult) ==
            social::SocialStatus::ClockRegression,
        "75 stale reward day fails closed despite a newer epoch");
  reward.score = 1001U;
  reward.dayId = 25002U;
  check(rewards.recordSessionReward(reward, rewardResult) ==
            social::SocialStatus::InvalidInput,
        "75 normalized party reward rejects an out-of-range score");

  social::SocialProgression ranking;
  for (uint16_t uid = 1U; uid <= 4U; ++uid) {
    ranking.observeFriend(observation(uid, 26000U, 1820000000U + uid),
                          friendResult);
  }
  reward = social::SessionReward{};
  reward.uid = 4U;
  reward.score = 1000U;
  reward.sessionNonce = 600U;
  reward.dayId = 26001U;
  reward.epochSeconds = 1820086400U;
  ranking.recordSessionReward(reward, rewardResult);
  social::LeaderboardEntry winner{};
  check(ranking.leaderboard(&winner, 1U) == 1U && winner.uid == 4U,
        "73 bounded leaderboard selects from every stored peer");

  social::SocialProgression sessions;
  uint8_t merged = 0U;
  sessions.mergeSharedTrail(700U, 1U, 1U, merged);
  social::SocialState malformed = sessions.snapshot();
  malformed.recentSessionNext = 7U;
  malformed.crc32 = social::socialStateCrc(malformed);
  social::SocialProgression rejected;
  check(rejected.restore(malformed) == social::SocialStatus::StateUnavailable,
        "75 CRC-valid malformed session ring is rejected");
}

void testAtomicPartyRewards() {
  social::SocialProgression progression;
  social::FriendOutcome friendResult{};
  for (uint16_t uid = 1U; uid <= social::kPeerCapacity; ++uid) {
    check(progression.observeFriend(
              observation(uid, 27000U, 1821000000U + uid), friendResult) ==
              social::SocialStatus::Ok,
          "batch reward peer table fills");
  }

  social::PartyRewardRequest request{};
  request.sessionNonce = 800U;
  request.dayId = 27001U;
  request.epochSeconds = 1821086400U;
  request.score = 800U;
  request.peerCount = 1U;  // One remote peer is a two-person party.
  request.peerUids[0] = 1U;
  social::PartyRewardBatchOutcome outcome{};
  check(progression.recordPartyRewards(request, outcome) ==
            social::SocialStatus::Ok &&
            outcome.peerCount == 1U && outcome.peers[0].pointsAwarded == 4U &&
            progression.snapshot().completedParties == 1U &&
            peerState(progression.snapshot(), 1U)->successfulParties == 1U,
        "53 two-person party rewards one peer and counts one party");

  request = social::PartyRewardRequest{};
  request.sessionNonce = 801U;
  request.dayId = 27002U;
  request.epochSeconds = 1821172800U;
  request.score = 600U;
  request.peerCount = 2U;  // Two remote peers are a three-person party.
  request.peerUids[0] = 2U;
  request.peerUids[1] = 3U;
  check(progression.recordPartyRewards(request, outcome) ==
            social::SocialStatus::Ok &&
            outcome.peerCount == 2U &&
            progression.snapshot().completedParties == 2U &&
            peerState(progression.snapshot(), 2U)->successfulParties == 1U &&
            peerState(progression.snapshot(), 3U)->successfulParties == 1U,
        "53 three-person party rewards two peers and counts one party");

  request = social::PartyRewardRequest{};
  request.sessionNonce = 802U;
  request.dayId = 27003U;
  request.epochSeconds = 1821259200U;
  request.score = 1000U;
  request.peerCount = 3U;  // Three remote peers are a four-person party.
  request.peerUids[0] = 4U;
  request.peerUids[1] = 5U;
  request.peerUids[2] = 6U;
  check(progression.recordPartyRewards(request, outcome) ==
            social::SocialStatus::Ok &&
            outcome.peerCount == 3U &&
            progression.snapshot().completedParties == 3U &&
            progression.snapshot().recentSessionCount == 3U &&
            peerState(progression.snapshot(), 4U)->successfulParties == 1U &&
            peerState(progression.snapshot(), 5U)->successfulParties == 1U &&
            peerState(progression.snapshot(), 6U)->successfulParties == 1U,
        "53 four-person party rewards three peers but counts one party");

  const social::SocialState afterFourPlayer = progression.snapshot();
  const social::SocialStatus duplicateStatus =
      progression.recordPartyRewards(request, outcome);
  const social::SocialState afterDuplicate = progression.snapshot();
  check(duplicateStatus == social::SocialStatus::Duplicate &&
            std::memcmp(&afterFourPlayer, &afterDuplicate,
                        sizeof(social::SocialState)) == 0,
        "75 one batch nonce is consumed once for the whole party");

  request.sessionNonce = 803U;
  request.peerCount = 2U;
  request.peerUids[0] = 4U;
  request.peerUids[1] = 7U;
  request.peerUids[2] = 0U;
  social::PartyRewardBatchOutcome untouched{};
  untouched.peerCount = 99U;
  const social::SocialState beforeRateLimit = progression.snapshot();
  const social::SocialStatus rateStatus =
      progression.recordPartyRewards(request, untouched);
  const social::SocialState afterRateLimit = progression.snapshot();
  check(rateStatus == social::SocialStatus::RateLimited &&
            untouched.peerCount == 99U &&
            std::memcmp(&beforeRateLimit, &afterRateLimit,
                        sizeof(social::SocialState)) == 0 &&
            social::validateSocialState(afterRateLimit),
        "75 one ineligible peer rolls back the entire batch");

  request = social::PartyRewardRequest{};
  request.sessionNonce = 804U;
  request.dayId = 27002U;
  request.epochSeconds = 1821345600U;
  request.score = 500U;
  request.peerCount = 1U;
  request.peerUids[0] = 7U;
  const social::SocialState beforeRollback = progression.snapshot();
  const social::SocialStatus rollbackStatus =
      progression.recordPartyRewards(request, outcome);
  const social::SocialState afterRollback = progression.snapshot();
  check(rollbackStatus == social::SocialStatus::ClockRegression &&
            std::memcmp(&beforeRollback, &afterRollback,
                        sizeof(social::SocialState)) == 0,
        "75 batch day rollback is atomic");

  request.dayId = 27004U;
  request.sessionNonce = 805U;
  request.peerCount = 2U;
  request.peerUids[0] = 7U;
  request.peerUids[1] = 7U;
  const social::SocialStatus repeatedUidStatus =
      progression.recordPartyRewards(request, outcome);
  const social::SocialState afterRepeatedUid = progression.snapshot();
  check(repeatedUidStatus == social::SocialStatus::InvalidInput &&
            std::memcmp(&beforeRollback, &afterRepeatedUid,
                        sizeof(social::SocialState)) == 0,
        "75 duplicate UIDs cannot receive the same party reward twice");

  request = social::PartyRewardRequest{};
  request.sessionNonce = 806U;
  request.dayId = 27004U;
  request.epochSeconds = 1821345600U;
  request.score = 700U;
  request.peerCount = 3U;
  request.peerUids[0] = 10U;
  request.peerUids[1] = 11U;
  request.peerUids[2] = 12U;
  check(progression.recordPartyRewards(request, outcome) ==
            social::SocialStatus::Ok &&
            progression.snapshot().peerCount == social::kPeerCapacity &&
            progression.snapshot().completedParties == 4U,
        "53 full peer table still rewards three existing peers atomically");

  request.sessionNonce = 807U;
  request.dayId = 27005U;
  request.epochSeconds += 86400U;
  request.peerCount = 2U;
  request.peerUids[0] = 8U;
  request.peerUids[1] = 99U;
  request.peerUids[2] = 0U;
  const social::SocialState beforeUnknown = progression.snapshot();
  const social::SocialStatus unknownStatus =
      progression.recordPartyRewards(request, outcome);
  const social::SocialState afterUnknown = progression.snapshot();
  check(unknownStatus == social::SocialStatus::NotJoined &&
            std::memcmp(&beforeUnknown, &afterUnknown,
                        sizeof(social::SocialState)) == 0 &&
            social::validateSocialState(afterUnknown),
        "53 unknown peer at capacity fails without a partial reward or CRC drift");
}

}  // namespace

int main() {
  testFriendshipAndFairness();
  testLobbyAndRotatingModes();
  testCooperativeActivities();
  testLeaderboard();
  testIntegrityEdges();
  testAtomicPartyRewards();
  if (failures != 0) {
    std::cerr << "TEST_FAIL social_progression failures=" << failures << '\n';
    return 1;
  }
  std::cout << "TEST_PASS social_progression features=53-75 peers=12 modes=11"
            << '\n';
  return 0;
}
