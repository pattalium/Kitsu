#include "social_progression.h"

#include <string.h>

namespace kitsu868 {
namespace social {

namespace {

constexpr uint32_t kMagic = UINT32_C(0x31434F53);

uint32_t crc32Bytes(const uint8_t* bytes, size_t length) {
  uint32_t crc = UINT32_C(0xFFFFFFFF);
  for (size_t index = 0U; index < length; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (UINT32_C(0xEDB88320) & mask);
    }
  }
  return ~crc;
}

uint16_t saturatingAdd16(uint16_t value, uint16_t amount) {
  return amount > static_cast<uint16_t>(UINT16_MAX - value)
             ? UINT16_MAX
             : static_cast<uint16_t>(value + amount);
}

uint32_t mix32(uint32_t value) {
  value ^= value >> 16U;
  value *= UINT32_C(0x7FEB352D);
  value ^= value >> 15U;
  value *= UINT32_C(0x846CA68B);
  value ^= value >> 16U;
  return value;
}

uint8_t bitCount16(uint16_t value) {
  uint8_t count = 0U;
  while (value != 0U) {
    count = static_cast<uint8_t>(count + (value & 1U));
    value >>= 1U;
  }
  return count;
}

}  // namespace

uint32_t socialStateCrc(const SocialState& state) {
  return crc32Bytes(reinterpret_cast<const uint8_t*>(&state),
                    offsetof(SocialState, crc32));
}

bool validateSocialState(const SocialState& state) {
  const PeerState emptyPeer{};
  if (state.magic != kMagic || state.bytes != sizeof(SocialState) ||
      state.schemaVersion != kStateSchemaVersion ||
      state.peerCount > kPeerCapacity ||
      state.recentSessionCount > kRecentSessionCapacity ||
      state.recentSessionNext >= kRecentSessionCapacity ||
      state.dailyUniquePeers > kPeerCapacity ||
      state.dailyChallengeComplete > 1U || state.reserved != 0U ||
      state.crc32 != socialStateCrc(state)) {
    return false;
  }
  if (state.dailyUniquePeers != bitCount16(state.dailySeenMask)) return false;
  const uint16_t validPeerMask = state.peerCount == 0U
      ? 0U
      : static_cast<uint16_t>((1U << state.peerCount) - 1U);
  if ((state.dailySeenMask & static_cast<uint16_t>(~validPeerMask)) != 0U ||
      (state.dailyChallengeTarget == 0U &&
       (state.dailyChallengeProgress != 0U ||
        state.dailyChallengeComplete != 0U)) ||
      (state.dailyChallengeTarget != 0U &&
       state.dailyChallengeComplete !=
           (state.dailyChallengeProgress >= state.dailyChallengeTarget ? 1U
                                                                        : 0U))) {
    return false;
  }
  for (uint8_t index = 0U; index < kPeerCapacity; ++index) {
    const PeerState& peer = state.peers[index];
    if (index < state.peerCount) {
      if (peer.uid == 0U || peer.meetings == 0U || peer.firstSeenDay == 0U ||
          peer.lastSeenDay < peer.firstSeenDay ||
          peer.lastSeenDay > state.currentDay || peer.reserved != 0U ||
          ((peer.lastRewardDay == 0U) != (peer.lastRewardEpoch == 0U)) ||
          (peer.lastRewardDay != 0U &&
           (peer.lastRewardDay > state.currentDay ||
            peer.lastRewardEpoch < kMinimumTrustedEpoch))) {
        return false;
      }
      for (uint8_t other = 0U; other < index; ++other) {
        if (state.peers[other].uid == peer.uid) return false;
      }
    } else if (memcmp(&peer, &emptyPeer, sizeof(PeerState)) != 0) {
      return false;
    }
  }
  if (state.recentSessionCount < kRecentSessionCapacity &&
      state.recentSessionNext != state.recentSessionCount) {
    return false;
  }
  for (uint8_t index = 0U; index < kRecentSessionCapacity; ++index) {
    if (index < state.recentSessionCount) {
      if (state.recentSessions[index] == 0U) return false;
      for (uint8_t other = 0U; other < index; ++other) {
        if (state.recentSessions[other] == state.recentSessions[index]) {
          return false;
        }
      }
    } else if (state.recentSessions[index] != 0U) {
      return false;
    }
  }
  return true;
}

uint8_t friendshipLevel(uint16_t points) {
  if (points >= 90U) return 4U;
  if (points >= 50U) return 3U;
  if (points >= 25U) return 2U;
  if (points >= 10U) return 1U;
  return 0U;
}

const char* greetingLine1(Greeting greeting) {
  switch (greeting) {
    case Greeting::NewSignal: return "NEW SIGNAL";
    case Greeting::FamiliarSignal: return "YOU AGAIN";
    case Greeting::FriendReturned: return "WELCOME BACK";
    case Greeting::CloseFriend: return "OLD FRIEND";
    case Greeting::MentorWelcome: return "STAY CLOSE";
  }
  return "HELLO";
}

const char* greetingLine2(Greeting greeting) {
  switch (greeting) {
    case Greeting::NewSignal: return "HELLO THERE";
    case Greeting::FamiliarSignal: return "I REMEMBER";
    case Greeting::FriendReturned: return "MISSED YOU";
    case Greeting::CloseFriend: return "SAME WAVELENGTH";
    case Greeting::MentorWelcome: return "I CAN HELP";
  }
  return "FRIEND";
}

const char* hotspotModeName(HotspotMode mode) {
  switch (mode) {
    case HotspotMode::SignalHunt: return "signal_hunt";
    case HotspotMode::Triangulation: return "triangulation";
    case HotspotMode::HotCold: return "hot_cold";
    case HotspotMode::HideAndSeek: return "hide_and_seek";
    case HotspotMode::SyncTap: return "sync_tap";
    case HotspotMode::CoopEcho: return "coop_echo";
    case HotspotMode::AsyncRelay: return "async_relay";
    case HotspotMode::RareEncounter: return "rare_encounter";
    case HotspotMode::SharedTrail: return "shared_trail";
    case HotspotMode::StoryVote: return "story_vote";
    case HotspotMode::DailyChallenge: return "daily_challenge";
  }
  return "signal_hunt";
}

SocialProgression::SocialProgression() { reset(); }

void SocialProgression::refreshCrc() { state_.crc32 = socialStateCrc(state_); }

void SocialProgression::reset() {
  state_ = SocialState{};
  refreshCrc();
  available_ = true;
}

SocialStatus SocialProgression::restore(const SocialState& state) {
  if (!validateSocialState(state)) {
    state_ = SocialState{};
    refreshCrc();
    available_ = false;
    return SocialStatus::StateUnavailable;
  }
  state_ = state;
  available_ = true;
  return SocialStatus::Ok;
}

PeerState* SocialProgression::findPeer(uint16_t uid) {
  for (uint8_t index = 0U; index < state_.peerCount; ++index) {
    if (state_.peers[index].uid == uid) return &state_.peers[index];
  }
  return nullptr;
}

const PeerState* SocialProgression::findPeer(uint16_t uid) const {
  for (uint8_t index = 0U; index < state_.peerCount; ++index) {
    if (state_.peers[index].uid == uid) return &state_.peers[index];
  }
  return nullptr;
}

PeerState* SocialProgression::allocatePeer(uint16_t uid) {
  if (state_.peerCount >= kPeerCapacity) return nullptr;
  PeerState& peer = state_.peers[state_.peerCount++];
  peer = PeerState{};
  peer.uid = uid;
  return &peer;
}

void SocialProgression::beginDay(uint32_t dayId) {
  if (state_.currentDay == dayId) return;
  state_.currentDay = dayId;
  state_.dailySeenMask = 0U;
  state_.dailyUniquePeers = 0U;
  state_.dailyChallengeComplete = 0U;
  state_.dailyChallengeProgress = 0U;
  state_.dailyChallengeTarget = 0U;
}

SocialStatus SocialProgression::observeFriend(
    const FriendObservation& observation, FriendOutcome& output) {
  if (!available_) return SocialStatus::StateUnavailable;
  if (observation.uid == 0U || observation.dayId == 0U ||
      observation.epochSeconds < kMinimumTrustedEpoch ||
      observation.peerIsNewcomer > 1U) {
    return SocialStatus::InvalidInput;
  }
  PeerState* peer = findPeer(observation.uid);
  const bool firstMeeting = peer == nullptr;
  if (state_.currentDay > observation.dayId) return SocialStatus::ClockRegression;
  if (firstMeeting && state_.peerCount >= kPeerCapacity) {
    return SocialStatus::StateFull;
  }
  beginDay(observation.dayId);
  if (firstMeeting) {
    peer = allocatePeer(observation.uid);
    peer->firstSeenDay = observation.dayId;
    peer->lastSeenDay = observation.dayId;
    peer->meetings = 1U;
  }

  const uint8_t peerIndex = static_cast<uint8_t>(peer - &state_.peers[0]);
  const uint16_t peerBit = static_cast<uint16_t>(1U << peerIndex);
  const bool firstToday = (state_.dailySeenMask & peerBit) == 0U;
  const bool reunion = !firstMeeting &&
      observation.dayId - peer->lastSeenDay >= 7U;

  FriendOutcome candidate{};
  candidate.event = firstMeeting ? FriendEvent::FirstMeeting
                                 : reunion ? FriendEvent::Reunion
                                           : FriendEvent::KnownFriend;
  uint8_t points = firstMeeting ? 5U : reunion ? 3U : firstToday ? 1U : 0U;
  if (firstToday) {
    state_.dailySeenMask = static_cast<uint16_t>(state_.dailySeenMask | peerBit);
    state_.dailyUniquePeers = bitCount16(state_.dailySeenMask);
    if (state_.dailyUniquePeers == 3U || state_.dailyUniquePeers == 6U ||
        state_.dailyUniquePeers == 9U) {
      candidate.diversityBonus = 2U;
      points = static_cast<uint8_t>(points + candidate.diversityBonus);
    }
  }
  if (firstMeeting && observation.peerIsNewcomer != 0U &&
      observation.localBondLevel >= 3U) {
    candidate.mentorBonus = 2U;
    points = static_cast<uint8_t>(points + candidate.mentorBonus);
  }
  if (!firstMeeting) peer->meetings = saturatingAdd16(peer->meetings, 1U);
  peer->lastSeenDay = observation.dayId;
  peer->friendshipPoints = saturatingAdd16(peer->friendshipPoints, points);
  candidate.pointsAwarded = points;
  candidate.friendshipLevel = friendshipLevel(peer->friendshipPoints);
  candidate.dailyUniquePeers = state_.dailyUniquePeers;
  if (candidate.mentorBonus != 0U) {
    candidate.greeting = Greeting::MentorWelcome;
  } else if (candidate.friendshipLevel >= 3U) {
    candidate.greeting = Greeting::CloseFriend;
  } else if (reunion) {
    candidate.greeting = Greeting::FriendReturned;
  } else if (firstMeeting) {
    candidate.greeting = Greeting::NewSignal;
  } else {
    candidate.greeting = Greeting::FamiliarSignal;
  }
  output = candidate;
  refreshCrc();
  return SocialStatus::Ok;
}

bool SocialProgression::seenSession(uint32_t nonce) const {
  for (uint8_t index = 0U; index < state_.recentSessionCount; ++index) {
    if (state_.recentSessions[index] == nonce) return true;
  }
  return false;
}

void SocialProgression::rememberSession(uint32_t nonce) {
  state_.recentSessions[state_.recentSessionNext] = nonce;
  state_.recentSessionNext = static_cast<uint8_t>(
      (state_.recentSessionNext + 1U) % kRecentSessionCapacity);
  if (state_.recentSessionCount < kRecentSessionCapacity) {
    ++state_.recentSessionCount;
  }
}

SocialStatus SocialProgression::recordSessionReward(
    const SessionReward& reward, RewardOutcome& output) {
  PartyRewardRequest batch{};
  batch.sessionNonce = reward.sessionNonce;
  batch.dayId = reward.dayId;
  batch.epochSeconds = reward.epochSeconds;
  batch.score = reward.score;
  batch.peerCount = 1U;
  batch.peerUids[0] = reward.uid;
  PartyRewardBatchOutcome batchOutput{};
  const SocialStatus status = recordPartyRewards(batch, batchOutput);
  if (status == SocialStatus::Ok) output = batchOutput.peers[0];
  return status;
}

SocialStatus SocialProgression::recordPartyRewards(
    const PartyRewardRequest& reward, PartyRewardBatchOutcome& output) {
  if (!available_) return SocialStatus::StateUnavailable;
  if (reward.peerCount == 0U || reward.peerCount > kRewardPeerCapacity ||
      reward.score > 1000U || reward.sessionNonce == 0U || reward.dayId == 0U ||
      reward.epochSeconds < kMinimumTrustedEpoch) {
    return SocialStatus::InvalidInput;
  }
  for (uint8_t index = 0U; index < kRewardPeerCapacity; ++index) {
    if ((index < reward.peerCount && reward.peerUids[index] == 0U) ||
        (index >= reward.peerCount && reward.peerUids[index] != 0U)) {
      return SocialStatus::InvalidInput;
    }
    for (uint8_t other = 0U; other < index && index < reward.peerCount;
         ++other) {
      if (reward.peerUids[other] == reward.peerUids[index]) {
        return SocialStatus::InvalidInput;
      }
    }
  }
  if (seenSession(reward.sessionNonce)) return SocialStatus::Duplicate;
  PeerState* peers[kRewardPeerCapacity]{};
  for (uint8_t index = 0U; index < reward.peerCount; ++index) {
    peers[index] = findPeer(reward.peerUids[index]);
    if (!peers[index]) return SocialStatus::NotJoined;
  }
  if (state_.currentDay > reward.dayId) return SocialStatus::ClockRegression;
  for (uint8_t index = 0U; index < reward.peerCount; ++index) {
    const PeerState& peer = *peers[index];
    if (peer.lastRewardDay > reward.dayId ||
        (peer.lastRewardEpoch != 0U &&
         reward.epochSeconds < peer.lastRewardEpoch)) {
      return SocialStatus::ClockRegression;
    }
    if (peer.lastRewardEpoch != 0U &&
        (reward.dayId == peer.lastRewardDay ||
         reward.epochSeconds - peer.lastRewardEpoch <
             kRewardCooldownSeconds)) {
      return SocialStatus::RateLimited;
    }
  }
  beginDay(reward.dayId);
  const uint8_t points = static_cast<uint8_t>(
      2U + (reward.score >= 750U ? 2U : reward.score >= 400U ? 1U : 0U));
  PartyRewardBatchOutcome candidate{};
  candidate.peerCount = reward.peerCount;
  for (uint8_t index = 0U; index < reward.peerCount; ++index) {
    PeerState& peer = *peers[index];
    const bool firstSuccess = peer.successfulParties == 0U;
    peer.friendshipPoints = saturatingAdd16(peer.friendshipPoints, points);
    peer.successfulParties = saturatingAdd16(peer.successfulParties, 1U);
    if (reward.score > peer.bestPartyScore) peer.bestPartyScore = reward.score;
    peer.lastRewardDay = reward.dayId;
    peer.lastRewardEpoch = reward.epochSeconds;
    candidate.peers[index].pointsAwarded = points;
    candidate.peers[index].friendshipLevel =
        friendshipLevel(peer.friendshipPoints);
    candidate.peers[index].firstSuccess = firstSuccess ? 1U : 0U;
  }
  if (state_.completedParties != UINT32_MAX) ++state_.completedParties;
  rememberSession(reward.sessionNonce);
  output = candidate;
  refreshCrc();
  return SocialStatus::Ok;
}

SocialStatus SocialProgression::contributeDailyChallenge(
    uint32_t dayId, uint16_t amount, uint16_t target, uint8_t& completedNow) {
  completedNow = 0U;
  if (!available_) return SocialStatus::StateUnavailable;
  if (dayId == 0U || amount == 0U || target == 0U) {
    return SocialStatus::InvalidInput;
  }
  if (state_.currentDay > dayId) return SocialStatus::ClockRegression;
  beginDay(dayId);
  if (state_.dailyChallengeTarget != 0U &&
      state_.dailyChallengeTarget != target) {
    return SocialStatus::InvalidInput;
  }
  state_.dailyChallengeTarget = target;
  state_.dailyChallengeProgress =
      saturatingAdd16(state_.dailyChallengeProgress, amount);
  if (state_.dailyChallengeComplete == 0U &&
      state_.dailyChallengeProgress >= target) {
    state_.dailyChallengeComplete = 1U;
    completedNow = 1U;
  }
  refreshCrc();
  return SocialStatus::Ok;
}

SocialStatus SocialProgression::mergeSharedTrail(
    uint32_t sessionNonce, uint8_t localMisses, uint8_t peerMisses,
    uint8_t& mergedMisses) {
  if (!available_) return SocialStatus::StateUnavailable;
  if (sessionNonce == 0U || localMisses > 20U || peerMisses > 20U) {
    return SocialStatus::InvalidInput;
  }
  if (seenSession(sessionNonce)) return SocialStatus::Duplicate;
  mergedMisses = sharedTrailMisses(localMisses, peerMisses);
  state_.sharedTrailProgress = state_.sharedTrailProgress >
          UINT32_MAX - mergedMisses
      ? UINT32_MAX
      : state_.sharedTrailProgress + mergedMisses;
  rememberSession(sessionNonce);
  refreshCrc();
  return SocialStatus::Ok;
}

bool SocialProgression::cooperativeRareEncounterEligible(
    uint8_t uniquePeers, uint16_t score, uint16_t maximumScore) const {
  return available_ && uniquePeers >= 2U && uniquePeers <= kPartyCapacity &&
         maximumScore != 0U && score <= maximumScore &&
         static_cast<uint32_t>(score) * 100U >=
             static_cast<uint32_t>(maximumScore) * 80U;
}

uint8_t SocialProgression::leaderboard(LeaderboardEntry* output,
                                       uint8_t capacity) const {
  if (!output || capacity == 0U || !available_) return 0U;
  LeaderboardEntry ranked[kPeerCapacity]{};
  for (uint8_t index = 0U; index < state_.peerCount; ++index) {
    const PeerState& peer = state_.peers[index];
    ranked[index].uid = peer.uid;
    ranked[index].friendshipPoints = peer.friendshipPoints;
    ranked[index].successfulParties = peer.successfulParties;
    ranked[index].bestPartyScore = peer.bestPartyScore;
  }
  for (uint8_t index = 1U; index < state_.peerCount; ++index) {
    LeaderboardEntry value = ranked[index];
    uint8_t position = index;
    while (position > 0U) {
      const LeaderboardEntry& before = ranked[position - 1U];
      const bool greater = value.successfulParties > before.successfulParties ||
          (value.successfulParties == before.successfulParties &&
           (value.bestPartyScore > before.bestPartyScore ||
            (value.bestPartyScore == before.bestPartyScore &&
             value.friendshipPoints > before.friendshipPoints)));
      if (!greater) break;
      ranked[position] = before;
      --position;
    }
    ranked[position] = value;
  }
  const uint8_t count = state_.peerCount < capacity ? state_.peerCount : capacity;
  for (uint8_t index = 0U; index < count; ++index) {
    output[index] = ranked[index];
  }
  return count;
}

HotspotMode rotatingHotspotMode(uint32_t dayId, uint32_t groupSeed) {
  return static_cast<HotspotMode>(mix32(dayId ^ groupSeed) % kModeCount);
}

uint16_t triangulationScore(int16_t firstRssi, int16_t secondRssi,
                            int16_t thirdRssi) {
  if (firstRssi > 0 || secondRssi > 0 || thirdRssi > 0 ||
      firstRssi < -140 || secondRssi < -140 || thirdRssi < -140) {
    return 0U;
  }
  int16_t strongest = firstRssi;
  if (secondRssi > strongest) strongest = secondRssi;
  if (thirdRssi > strongest) strongest = thirdRssi;
  const uint16_t spread = static_cast<uint16_t>(
      (firstRssi > strongest ? firstRssi - strongest : strongest - firstRssi) +
      (secondRssi > strongest ? secondRssi - strongest : strongest - secondRssi) +
      (thirdRssi > strongest ? thirdRssi - strongest : strongest - thirdRssi));
  const uint16_t strength = strongest <= -120 ? 0U
      : static_cast<uint16_t>((strongest + 120) * 8);
  return strength > spread * 3U ? static_cast<uint16_t>(strength - spread * 3U)
                               : 1U;
}

TemperatureHint temperatureHint(int16_t previousRssi, int16_t currentRssi) {
  if (previousRssi > 0 || currentRssi > 0 || previousRssi < -140 ||
      currentRssi < -140) {
    return TemperatureHint::Unknown;
  }
  const int16_t delta = static_cast<int16_t>(currentRssi - previousRssi);
  if (delta >= 8) return TemperatureHint::MuchHotter;
  if (delta >= 3) return TemperatureHint::Hotter;
  if (delta <= -8) return TemperatureHint::MuchColder;
  if (delta <= -3) return TemperatureHint::Colder;
  return TemperatureHint::Same;
}

uint16_t hideAndSeekScore(uint8_t hiddenSlot, uint8_t guessedSlot,
                          uint8_t slotCount) {
  if (slotCount < 2U || hiddenSlot >= slotCount || guessedSlot >= slotCount) {
    return 0U;
  }
  uint8_t distance = hiddenSlot > guessedSlot
      ? static_cast<uint8_t>(hiddenSlot - guessedSlot)
      : static_cast<uint8_t>(guessedSlot - hiddenSlot);
  const uint8_t wrap = static_cast<uint8_t>(slotCount - distance);
  if (wrap < distance) distance = wrap;
  return distance == 0U ? 1000U
      : static_cast<uint16_t>(1000U / static_cast<uint16_t>(distance + 1U));
}

uint16_t synchronizedTapScore(const uint32_t* taps, uint8_t count,
                              uint16_t perfectSpreadMs,
                              uint16_t maximumSpreadMs) {
  if (!taps || count < 2U || count > kPartyCapacity ||
      perfectSpreadMs > maximumSpreadMs || maximumSpreadMs == 0U) {
    return 0U;
  }
  int64_t earliest = 0;
  int64_t latest = 0;
  for (uint8_t index = 1U; index < count; ++index) {
    const uint32_t forward = taps[index] - taps[0];
    const int64_t delta = forward <= static_cast<uint32_t>(INT32_MAX)
        ? static_cast<int64_t>(forward)
        : -static_cast<int64_t>(UINT32_MAX - forward) - 1;
    if (delta < earliest) earliest = delta;
    if (delta > latest) latest = delta;
  }
  const uint64_t spread = static_cast<uint64_t>(latest - earliest);
  if (spread <= perfectSpreadMs) return 1000U;
  if (spread >= maximumSpreadMs) return 0U;
  return static_cast<uint16_t>(
      (static_cast<uint32_t>(maximumSpreadMs) - spread) * 1000U /
      (maximumSpreadMs - perfectSpreadMs));
}

uint16_t cooperativeEchoScore(uint16_t expectedPattern,
                              const uint16_t* playerPatterns,
                              uint8_t count, uint8_t beats) {
  if (!playerPatterns || count < 2U || count > kPartyCapacity || beats == 0U ||
      beats > 16U || (beats < 16U && expectedPattern >= (1U << beats))) {
    return 0U;
  }
  const uint16_t validMask = beats == 16U
      ? UINT16_MAX
      : static_cast<uint16_t>((1U << beats) - 1U);
  uint16_t matches = 0U;
  for (uint8_t player = 0U; player < count; ++player) {
    if ((playerPatterns[player] & static_cast<uint16_t>(~validMask)) != 0U) {
      return 0U;
    }
    const uint16_t difference =
        static_cast<uint16_t>(expectedPattern ^ playerPatterns[player]);
    matches = static_cast<uint16_t>(matches + beats - bitCount16(difference));
  }
  return static_cast<uint16_t>(
      static_cast<uint32_t>(matches) * 1000U /
      (static_cast<uint16_t>(beats) * count));
}

uint8_t sharedTrailMisses(uint8_t localMisses, uint8_t peerMisses) {
  const uint16_t combined = static_cast<uint16_t>(localMisses) + peerMisses;
  return combined > 20U ? 20U : static_cast<uint8_t>(combined);
}

uint8_t storyVoteWinner(const uint8_t votes[kStoryChoiceCount],
                        uint32_t tieSeed) {
  if (!votes) return 0U;
  uint8_t best = 0U;
  uint8_t candidates[kStoryChoiceCount]{};
  uint8_t candidateCount = 0U;
  for (uint8_t index = 0U; index < kStoryChoiceCount; ++index) {
    if (votes[index] > best) best = votes[index];
  }
  for (uint8_t index = 0U; index < kStoryChoiceCount; ++index) {
    if (votes[index] == best) candidates[candidateCount++] = index;
  }
  return candidates[mix32(tieSeed) % candidateCount];
}

PartyMember* PartyLobby::member(uint16_t uid) {
  for (uint8_t index = 0U; index < state_.memberCount; ++index) {
    if (state_.members[index].uid == uid) return &state_.members[index];
  }
  return nullptr;
}

SocialStatus PartyLobby::begin(uint16_t hostUid, uint32_t sessionNonce,
                               HotspotMode mode) {
  if (hostUid == 0U || sessionNonce == 0U ||
      static_cast<uint8_t>(mode) >= kModeCount) {
    return SocialStatus::InvalidInput;
  }
  reset();
  state_.sessionNonce = sessionNonce;
  state_.mode = static_cast<uint8_t>(mode);
  state_.phase = 1U;
  state_.memberCount = 1U;
  state_.members[0].uid = hostUid;
  return SocialStatus::Ok;
}

SocialStatus PartyLobby::join(uint16_t uid) {
  if (uid == 0U) return SocialStatus::InvalidInput;
  if (state_.phase != 1U) return SocialStatus::JoinClosed;
  if (member(uid)) return SocialStatus::AlreadyJoined;
  if (state_.memberCount >= kPartyCapacity) return SocialStatus::PartyFull;
  PartyMember& joined = state_.members[state_.memberCount++];
  joined = PartyMember{};
  joined.uid = uid;
  return SocialStatus::Ok;
}

SocialStatus PartyLobby::setReady(uint16_t uid, bool ready) {
  PartyMember* found = member(uid);
  if (!found) return SocialStatus::NotJoined;
  if (state_.phase != 1U) return SocialStatus::WrongPhase;
  found->ready = ready ? 1U : 0U;
  return SocialStatus::Ok;
}

bool PartyLobby::canStart() const {
  if (state_.phase != 1U || state_.memberCount < 2U) return false;
  for (uint8_t index = 0U; index < state_.memberCount; ++index) {
    if (state_.members[index].ready == 0U) return false;
  }
  return true;
}

SocialStatus PartyLobby::start() {
  if (!canStart()) return SocialStatus::NotReady;
  state_.phase = 2U;
  state_.round = 1U;
  return SocialStatus::Ok;
}

SocialStatus PartyLobby::joinInProgress(uint16_t uid) {
  if (uid == 0U) return SocialStatus::InvalidInput;
  if (state_.phase != 2U || state_.round > 2U) return SocialStatus::JoinClosed;
  if (member(uid)) return SocialStatus::AlreadyJoined;
  if (state_.memberCount >= kPartyCapacity) return SocialStatus::PartyFull;
  PartyMember& joined = state_.members[state_.memberCount++];
  joined = PartyMember{};
  joined.uid = uid;
  joined.ready = 1U;
  joined.joinedRound = state_.round;
  return SocialStatus::Ok;
}

SocialStatus PartyLobby::addScore(uint16_t uid, uint16_t points) {
  if (state_.phase != 2U) return SocialStatus::WrongPhase;
  PartyMember* found = member(uid);
  if (!found) return SocialStatus::NotJoined;
  found->score = saturatingAdd16(found->score, points);
  return SocialStatus::Ok;
}

SocialStatus PartyLobby::advanceRound() {
  if (state_.phase != 2U) return SocialStatus::WrongPhase;
  if (state_.round >= 3U) {
    state_.phase = 3U;
  } else {
    ++state_.round;
  }
  return SocialStatus::Ok;
}

void PartyLobby::complete() { if (state_.phase == 2U) state_.phase = 3U; }

void PartyLobby::reset() { state_ = PartyLobbyState{}; }

}  // namespace social
}  // namespace kitsu868
