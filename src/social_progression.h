#pragma once

#include <stddef.h>
#include <stdint.h>

// Persistent friendship and transport-independent cooperative activities.
// Radio/BLE callers carry only value types into this module; it performs no
// I/O, allocation, rendering, location access, or external state mutation.
namespace kitsu868 {
namespace social {

constexpr uint8_t kPeerCapacity = 12U;
constexpr uint8_t kPartyCapacity = 4U;
constexpr uint8_t kRecentSessionCapacity = 8U;
constexpr uint8_t kStoryChoiceCount = 3U;
constexpr uint8_t kModeCount = 11U;
constexpr uint8_t kRewardPeerCapacity = kPartyCapacity - 1U;
constexpr uint8_t kStateSchemaVersion = 1U;
constexpr uint32_t kMinimumTrustedEpoch = UINT32_C(1577836800);
constexpr uint32_t kRewardCooldownSeconds = 900U;

enum class FriendEvent : uint8_t {
  None = 0U,
  FirstMeeting,
  KnownFriend,
  Reunion,
};

enum class Greeting : uint8_t {
  NewSignal = 0U,
  FamiliarSignal,
  FriendReturned,
  CloseFriend,
  MentorWelcome,
};

enum class HotspotMode : uint8_t {
  SignalHunt = 0U,
  Triangulation,
  HotCold,
  HideAndSeek,
  SyncTap,
  CoopEcho,
  AsyncRelay,
  RareEncounter,
  SharedTrail,
  StoryVote,
  DailyChallenge,
};

enum class TemperatureHint : uint8_t {
  Unknown = 0U,
  MuchColder,
  Colder,
  Same,
  Hotter,
  MuchHotter,
};

enum class SocialStatus : uint8_t {
  Ok = 0U,
  InvalidInput,
  StateUnavailable,
  StateFull,
  Duplicate,
  RateLimited,
  ClockRegression,
  WrongSession,
  WrongPhase,
  PartyFull,
  AlreadyJoined,
  NotJoined,
  NotReady,
  JoinClosed,
};

#pragma pack(push, 1)
struct PeerState {
  uint16_t uid = 0U;
  uint16_t friendshipPoints = 0U;
  uint16_t meetings = 0U;
  uint16_t successfulParties = 0U;
  uint16_t bestPartyScore = 0U;
  uint16_t reserved = 0U;
  uint32_t firstSeenDay = 0U;
  uint32_t lastSeenDay = 0U;
  uint32_t lastRewardDay = 0U;
  uint32_t lastRewardEpoch = 0U;
};

struct SocialState {
  uint32_t magic = UINT32_C(0x31434F53);  // "SOC1" little-endian.
  uint16_t bytes = sizeof(SocialState);
  uint8_t schemaVersion = kStateSchemaVersion;
  uint8_t peerCount = 0U;
  uint32_t currentDay = 0U;
  uint16_t dailySeenMask = 0U;
  uint8_t dailyUniquePeers = 0U;
  uint8_t dailyChallengeComplete = 0U;
  uint16_t dailyChallengeProgress = 0U;
  uint16_t dailyChallengeTarget = 0U;
  uint32_t completedParties = 0U;
  uint32_t cooperativeRareEncounters = 0U;
  uint32_t sharedTrailProgress = 0U;
  uint32_t recentSessions[kRecentSessionCapacity]{};
  uint8_t recentSessionCount = 0U;
  uint8_t recentSessionNext = 0U;
  uint16_t reserved = 0U;
  PeerState peers[kPeerCapacity]{};
  uint32_t crc32 = 0U;
};
#pragma pack(pop)

static_assert(sizeof(SocialState) <= 448U,
              "social persistence must stay small and bounded");

struct FriendObservation {
  uint16_t uid = 0U;
  uint32_t dayId = 0U;
  uint32_t epochSeconds = 0U;
  uint8_t peerIsNewcomer = 0U;
  uint8_t localBondLevel = 0U;
};

struct FriendOutcome {
  FriendEvent event = FriendEvent::None;
  Greeting greeting = Greeting::NewSignal;
  uint8_t friendshipLevel = 0U;
  uint8_t pointsAwarded = 0U;
  uint8_t diversityBonus = 0U;
  uint8_t mentorBonus = 0U;
  uint8_t dailyUniquePeers = 0U;
};

struct SessionReward {
  uint16_t uid = 0U;
  uint16_t score = 0U;
  uint32_t sessionNonce = 0U;
  uint32_t dayId = 0U;
  uint32_t epochSeconds = 0U;
};

struct RewardOutcome {
  uint8_t pointsAwarded = 0U;
  uint8_t friendshipLevel = 0U;
  uint8_t firstSuccess = 0U;
  uint8_t challengeCompleted = 0U;
};

struct PartyRewardRequest {
  uint32_t sessionNonce = 0U;
  uint32_t dayId = 0U;
  uint32_t epochSeconds = 0U;
  uint16_t score = 0U;
  uint8_t peerCount = 0U;
  uint16_t peerUids[kRewardPeerCapacity]{};
};

struct PartyRewardBatchOutcome {
  uint8_t peerCount = 0U;
  RewardOutcome peers[kRewardPeerCapacity]{};
};

struct PartyMember {
  uint16_t uid = 0U;
  uint8_t ready = 0U;
  uint8_t joinedRound = 0U;
  uint16_t score = 0U;
};

struct PartyLobbyState {
  uint32_t sessionNonce = 0U;
  uint8_t mode = static_cast<uint8_t>(HotspotMode::SignalHunt);
  uint8_t phase = 0U;  // 0 idle, 1 lobby, 2 active, 3 complete.
  uint8_t round = 0U;
  uint8_t memberCount = 0U;
  PartyMember members[kPartyCapacity]{};
};

struct LeaderboardEntry {
  uint16_t uid = 0U;
  uint16_t friendshipPoints = 0U;
  uint16_t successfulParties = 0U;
  uint16_t bestPartyScore = 0U;
};

bool validateSocialState(const SocialState& state);
uint32_t socialStateCrc(const SocialState& state);
uint8_t friendshipLevel(uint16_t points);
const char* greetingLine1(Greeting greeting);
const char* greetingLine2(Greeting greeting);
const char* hotspotModeName(HotspotMode mode);

class SocialProgression {
 public:
  SocialProgression();
  void reset();
  SocialStatus restore(const SocialState& state);
  SocialState snapshot() const { return state_; }
  bool available() const { return available_; }

  SocialStatus observeFriend(const FriendObservation& observation,
                             FriendOutcome& output);
  SocialStatus recordSessionReward(const SessionReward& reward,
                                   RewardOutcome& output);
  SocialStatus recordPartyRewards(const PartyRewardRequest& reward,
                                  PartyRewardBatchOutcome& output);
  SocialStatus contributeDailyChallenge(uint32_t dayId, uint16_t amount,
                                        uint16_t target,
                                        uint8_t& completedNow);
  SocialStatus recordCooperativeRareEncounter(uint32_t sessionNonce);
  SocialStatus recordSharedTrailResult(uint32_t sessionNonce,
                                       uint8_t mergedMisses);
  SocialStatus mergeSharedTrail(uint32_t sessionNonce, uint8_t localMisses,
                                uint8_t peerMisses, uint8_t& mergedMisses);
  bool cooperativeRareEncounterEligible(uint8_t uniquePeers,
                                        uint16_t score,
                                        uint16_t maximumScore) const;
  uint8_t leaderboard(LeaderboardEntry* output, uint8_t capacity) const;

 private:
  PeerState* findPeer(uint16_t uid);
  const PeerState* findPeer(uint16_t uid) const;
  PeerState* allocatePeer(uint16_t uid);
  void beginDay(uint32_t dayId);
  bool seenSession(uint32_t nonce) const;
  void rememberSession(uint32_t nonce);
  void refreshCrc();

  SocialState state_{};
  bool available_ = true;
};

HotspotMode rotatingHotspotMode(uint32_t dayId, uint32_t groupSeed);
uint16_t triangulationScore(int16_t firstRssi, int16_t secondRssi,
                            int16_t thirdRssi);
TemperatureHint temperatureHint(int16_t previousRssi, int16_t currentRssi);
uint16_t hideAndSeekScore(uint8_t hiddenSlot, uint8_t guessedSlot,
                          uint8_t slotCount);
uint16_t synchronizedTapScore(const uint32_t* taps, uint8_t count,
                              uint16_t perfectSpreadMs,
                              uint16_t maximumSpreadMs);
uint16_t cooperativeEchoScore(uint16_t expectedPattern,
                              const uint16_t* playerPatterns,
                              uint8_t count, uint8_t beats);
uint8_t sharedTrailMisses(uint8_t localMisses, uint8_t peerMisses);
uint8_t storyVoteWinner(const uint8_t votes[kStoryChoiceCount],
                        uint32_t tieSeed);

class PartyLobby {
 public:
  SocialStatus begin(uint16_t hostUid, uint32_t sessionNonce,
                     HotspotMode mode);
  SocialStatus join(uint16_t uid);
  SocialStatus setReady(uint16_t uid, bool ready);
  bool canStart() const;
  SocialStatus start();
  SocialStatus joinInProgress(uint16_t uid);
  SocialStatus addScore(uint16_t uid, uint16_t points);
  SocialStatus advanceRound();
  void complete();
  void reset();
  const PartyLobbyState& state() const { return state_; }

 private:
  PartyMember* member(uint16_t uid);
  PartyLobbyState state_{};
};

}  // namespace social
}  // namespace kitsu868
