#pragma once

#include <stddef.h>
#include <stdint.h>

// Transport-independent, radio-first party hotspot and cooperative signal
// hunt. The codec is a sibling of kitsu_nearby_protocol: it performs no radio,
// display, storage, clock, GPS, server, inventory, or creature-unlock work.
// Callers supply time and carry accepted packets over the direct Kitsu path.
namespace kitsu868 {
namespace party {

constexpr uint8_t kMagic0 = 0x50U;  // 'P'
constexpr uint8_t kMagic1 = 0x38U;  // '8'
constexpr uint8_t kProtocolVersion = 1U;
constexpr size_t kWireBytes = 30U;
constexpr uint8_t kMinimumParticipants = 2U;
constexpr uint8_t kMaximumParticipants = 4U;
constexpr uint8_t kHuntRounds = 3U;
constexpr uint16_t kMaximumWindowSeconds = 600U;
constexpr uint8_t kHostStateSchemaVersion = 1U;
constexpr uint8_t kParticipantStateSchemaVersion = 1U;
constexpr uint8_t kRewardStateSchemaVersion = 1U;
constexpr size_t kRewardPeerCapacity = 12U;
constexpr size_t kRecentSessionCapacity = 8U;
constexpr uint32_t kDefaultPeerRewardCooldownSeconds = 900U;

static_assert(kWireBytes <= 32U,
              "party hotspot packet must fit the 32-byte radio budget");

enum class PacketType : uint8_t {
  Beacon = 1U,
  JoinRequest = 2U,
  Welcome = 3U,
  RoundOpen = 4U,
  RoundChoice = 5U,
  Result = 6U,
  Cancel = 7U,
};

enum class SignalChoice : uint8_t {
  None = 0U,
  Sweep = 1U,
  Listen = 2U,
  Pulse = 3U,
};

// Increasing tiers deliberately improve Party Bond rewards. A larger party
// also has more chances to match the hidden signal and earns a full-party
// score bonus, providing a multiplayer incentive without items.
enum class ResultTier : uint8_t {
  Faded = 0U,
  Trace = 1U,
  Found = 2U,
  Resonant = 3U,
};

enum class CancelReason : uint8_t {
  None = 0U,
  HostEnded = 1U,
  TooFewPlayers = 2U,
  SessionExpired = 3U,
};

constexpr uint8_t kFlagJoinOpen = 0x01U;

// The in-memory struct is intentionally not packed. Multi-byte wire values
// are explicitly little-endian. dataA/dataB have exact per-type meanings:
//
// Beacon:     dataA=seed, dataB=0
// JoinRequest:dataA=0, dataB=0
// Welcome:    dataA=accepted UID, dataB=seed
// RoundOpen:  dataA=seed, dataB=0
// RoundChoice:dataA=0, dataB=0
// Result:     dataA=score | maximumScore<<16, dataB=result proof
// Cancel:     dataA=0, dataB=0
struct Packet {
  PacketType type = PacketType::Beacon;
  uint16_t sourceUid = 0U;
  uint16_t hostUid = 0U;
  uint32_t sessionNonce = 0U;
  uint16_t sequence = 0U;
  uint8_t round = 0U;
  uint8_t value = 0U;
  uint8_t participantCount = 0U;
  uint8_t flags = 0U;
  uint32_t dataA = 0U;
  uint32_t dataB = 0U;
  uint16_t windowSeconds = 0U;
};

enum class Status : uint8_t {
  Ok = 0U,
  NullArgument,
  WrongLength,
  BufferTooSmall,
  BadMagic,
  UnsupportedVersion,
  UnsupportedType,
  InvalidSourceUid,
  InvalidHostUid,
  InvalidSessionNonce,
  InvalidSequence,
  InvalidRound,
  InvalidValue,
  InvalidParticipantCount,
  InvalidFlags,
  InvalidSeed,
  InvalidTargetUid,
  InvalidScore,
  InvalidWindow,
  UnexpectedField,
  IntegrityMismatch,
};

// Wire format (all multi-byte integers are little-endian):
//   0..1   magic "P8"
//   2      protocol version (1)
//   3      PacketType
//   4..5   source KT UID suffix
//   6..7   host KT UID suffix
//   8..11  host-generated session nonce
//   12..13 per-source sequence, non-zero and monotonic within the session
//   14     hunt round, 0 outside a hunt and 1..3 during a hunt
//   15     type-specific value (choice, result tier, slot, cancel reason)
//   16     public participant count when supplied by the host
//   17     flags (only Beacon currently permits kFlagJoinOpen)
//   18..21 dataA
//   22..25 dataB
//   26..27 remaining/open window seconds
//   28..29 CRC-16/CCITT-FALSE over bytes 0..27
//
// The CRC catches damage; it is not identity authentication. The transport
// must retain its existing trust, rate, and airtime policy.
Status encode(const Packet& packet, uint8_t* output, size_t capacity,
              size_t* bytesWritten = nullptr);
Status decode(const uint8_t* wire, size_t length, Packet& output);
Status validate(const Packet& packet);
const char* statusName(Status status);
uint16_t crc16CcittFalse(const uint8_t* data, size_t length);
uint32_t packetFingerprint(const Packet& packet);

bool supportedChoice(SignalChoice choice);
bool supportedTier(ResultTier tier);

enum class SessionPhase : uint8_t {
  Idle = 0U,
  Joining,
  Lobby,
  Round1,
  Round2,
  Round3,
  Complete,
  Cancelled,
  Expired,
  Unavailable,
};

struct HuntParticipant {
  uint16_t uid = 0U;
  uint8_t submittedMask = 0U;
  uint8_t choices[kHuntRounds]{};
};

struct HuntResult {
  uint8_t participantCount = 0U;
  uint8_t completedRounds = 0U;
  uint8_t tier = static_cast<uint8_t>(ResultTier::Faded);
  uint8_t targetChoices[kHuntRounds]{};
  uint16_t score = 0U;
  uint16_t maximumScore = 0U;
  uint32_t proof = 0U;
};

// Pure, allocation-free resolver. Participants may be in any order; the
// proof is canonicalized by UID. Missing timed-out choices remain None and do
// not score. The same session transcript always yields the same result.
bool resolveSignalHunt(uint32_t sessionNonce, uint32_t seed,
                       const HuntParticipant* participants,
                       uint8_t participantCount, HuntResult& output);

struct ReplayCursor {
  uint16_t lastSequence = 0U;
  uint16_t reserved = 0U;
  uint32_t lastFingerprint = 0U;
};

struct HostParticipantState {
  HuntParticipant hunt{};
  ReplayCursor replay{};
  uint8_t active = 0U;
  uint8_t slot = 0U;
  uint16_t reserved = 0U;
};

// Fixed-width semantic state for diagnostics/serialization. deadlineMs is in
// the caller's monotonic clock domain; temporary sessions are not intended to
// survive a reboot into a different clock epoch.
struct HostState {
  uint8_t schemaVersion = kHostStateSchemaVersion;
  uint8_t phase = static_cast<uint8_t>(SessionPhase::Idle);
  uint8_t participantCount = 0U;
  uint8_t currentRound = 0U;
  uint16_t hostUid = 0U;
  uint16_t txSequence = 0U;
  uint32_t sessionNonce = 0U;
  uint32_t seed = 0U;
  uint32_t deadlineMs = 0U;
  uint16_t lobbyWindowSeconds = 0U;
  uint16_t roundWindowSeconds = 0U;
  HostParticipantState participants[kMaximumParticipants]{};
  HuntResult result{};
};

static_assert(sizeof(HostState) <= 256U,
              "party host state must remain small and bounded");

bool validateHostState(const HostState& state);

enum class SessionStatus : uint8_t {
  Ok = 0U,
  InvalidArgument,
  InvalidPacket,
  WrongPhase,
  WrongSession,
  WrongHost,
  Duplicate,
  StaleSequence,
  SequenceConflict,
  SequenceExhausted,
  PartyFull,
  UnknownParticipant,
  RoundMismatch,
  ChoiceAlreadySubmitted,
  NotReady,
  TimedOut,
  UnsupportedState,
  InvalidState,
  StateUnavailable,
};

class HostSession {
 public:
  SessionStatus start(uint16_t hostUid, uint32_t sessionNonce, uint32_t seed,
                      uint32_t nowMs, uint16_t lobbyWindowSeconds,
                      uint16_t roundWindowSeconds);
  void reset();

  SessionStatus makeBeacon(uint32_t nowMs, Packet& output);
  SessionStatus acceptJoin(const Packet& request, uint32_t nowMs,
                           Packet& welcome);
  SessionStatus beginHunt(uint32_t nowMs, Packet& roundOpen);
  SessionStatus submitHostChoice(uint8_t round, SignalChoice choice);
  SessionStatus acceptChoice(const Packet& choice, uint32_t nowMs);

  // Advances early once everybody answered or at the current round timeout.
  // Produces the next RoundOpen, or the final Result after round three.
  SessionStatus advance(uint32_t nowMs, Packet& output);
  SessionStatus cancel(CancelReason reason, Packet& output);
  SessionStatus expireIfNeeded(uint32_t nowMs);

  const HostState& state() const;
  HostState snapshot() const;
  SessionStatus restore(const HostState& state);

 private:
  SessionStatus makeHostPacket(PacketType type, uint8_t round,
                               uint8_t value, uint8_t participantCount,
                               uint8_t flags, uint32_t dataA,
                               uint32_t dataB, uint16_t windowSeconds,
                               Packet& output);
  HostParticipantState* findParticipant(uint16_t uid);
  const HostParticipantState* findParticipant(uint16_t uid) const;
  bool allCurrentChoicesSubmitted() const;
  HostState state_{};
  bool available_ = true;
};

struct ParticipantState {
  uint8_t schemaVersion = kParticipantStateSchemaVersion;
  uint8_t phase = static_cast<uint8_t>(SessionPhase::Idle);
  uint8_t participantCount = 0U;
  uint8_t currentRound = 0U;
  uint16_t localUid = 0U;
  uint16_t hostUid = 0U;
  uint16_t txSequence = 0U;
  uint16_t reserved = 0U;
  uint32_t sessionNonce = 0U;
  uint32_t seed = 0U;
  uint32_t deadlineMs = 0U;
  uint8_t submittedMask = 0U;
  uint8_t choices[kHuntRounds]{};
  ReplayCursor hostReplay{};
  HuntResult result{};
};

static_assert(sizeof(ParticipantState) <= 128U,
              "party participant state must remain small and bounded");

bool validateParticipantState(const ParticipantState& state);

class ParticipantSession {
 public:
  SessionStatus observeBeacon(uint16_t localUid, const Packet& beacon,
                              uint32_t nowMs);
  SessionStatus makeJoinRequest(Packet& output);
  SessionStatus acceptHostPacket(const Packet& packet, uint32_t nowMs);
  SessionStatus makeChoice(SignalChoice choice, Packet& output);
  SessionStatus expireIfNeeded(uint32_t nowMs);
  void reset();

  const ParticipantState& state() const;
  ParticipantState snapshot() const;
  SessionStatus restore(const ParticipantState& state);

 private:
  ParticipantState state_{};
  bool available_ = true;
};

// Persistable, bounded multiplayer incentive state. Each peer can contribute
// Party Bond at most once per supplied day and once per cooldown interval.
// The recent-session ring makes retransmitted completed sessions idempotent.
struct PeerRewardState {
  uint16_t uid = 0U;
  uint16_t rewardedParties = 0U;
  uint32_t lastRewardDayId = 0U;
  uint32_t lastRewardEpochSeconds = 0U;
};

struct RewardState {
  uint8_t schemaVersion = kRewardStateSchemaVersion;
  uint8_t peerCount = 0U;
  uint8_t recentSessionCount = 0U;
  uint8_t recentSessionNext = 0U;
  uint32_t partyBond = 0U;
  uint32_t completedHunts = 0U;
  uint32_t rewardedHunts = 0U;
  uint32_t rewardedPeerEvents = 0U;
  uint32_t lastObservedDayId = 0U;
  uint32_t lastObservedEpochSeconds = 0U;
  uint32_t lastStreakDayId = 0U;
  uint16_t currentStreakDays = 0U;
  uint16_t longestStreakDays = 0U;
  PeerRewardState peers[kRewardPeerCapacity]{};
  uint32_t recentSessionNonces[kRecentSessionCapacity]{};
};

static_assert(sizeof(RewardState) <= 256U,
              "party reward state must remain small and bounded");

struct CompletedHuntReward {
  uint16_t selfUid = 0U;
  uint16_t participantUids[kMaximumParticipants]{};
  uint8_t participantCount = 0U;
  uint8_t tier = static_cast<uint8_t>(ResultTier::Faded);
  uint16_t reserved = 0U;
  uint32_t sessionNonce = 0U;
  uint32_t resultProof = 0U;
  uint32_t dayId = 0U;
  uint32_t nowEpochSeconds = 0U;
};

struct RewardOutcome {
  uint8_t eligibleUniquePeers = 0U;
  uint8_t streakAdvanced = 0U;
  uint16_t bondAwarded = 0U;
  uint32_t partyBondAfter = 0U;
  uint16_t currentStreakDays = 0U;
  uint16_t longestStreakDays = 0U;
};

enum class RewardStatus : uint8_t {
  Awarded = 0U,
  RecordedNoEligiblePeer,
  DuplicateSession,
  InvalidInput,
  ClockRegression,
  UnsupportedState,
  InvalidState,
  StateUnavailable,
};

bool validateRewardState(const RewardState& state);

class PartyRewardLedger {
 public:
  RewardStatus recordCompletedHunt(
      const CompletedHuntReward& completed,
      uint32_t peerCooldownSeconds,
      RewardOutcome& output);

  const RewardState& state() const;
  RewardState snapshot() const;
  RewardStatus restore(const RewardState& state);
  void reset();

 private:
  PeerRewardState* findPeer(uint16_t uid);
  PeerRewardState* allocatePeer(uint16_t uid);
  bool seenSession(uint32_t nonce) const;
  void rememberSession(uint32_t nonce);
  void quarantine();

  RewardState state_{};
  bool available_ = true;
};

const char* sessionStatusName(SessionStatus status);
const char* rewardStatusName(RewardStatus status);

}  // namespace party
}  // namespace kitsu868
