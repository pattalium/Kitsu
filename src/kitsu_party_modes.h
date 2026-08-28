#pragma once

#include <stddef.h>
#include <stdint.h>

#include "social_progression.h"

// Transport-independent multiplayer modes that complement the existing P8
// Signal Hunt. M8 packets use a different magic and can therefore share the
// same LoRa receive path without changing the proven P8 codec/session.
//
// The caller owns radio I/O, UI, clocks and persistence. Session methods only
// consume caller-supplied monotonic milliseconds and fixed-width value types.
namespace kitsu868 {
namespace party_modes {

constexpr uint8_t kMagic0 = 0x4DU;  // 'M'
constexpr uint8_t kMagic1 = 0x38U;  // '8'
constexpr uint8_t kProtocolVersion = 1U;
constexpr size_t kWireBytes = 32U;
constexpr uint8_t kMinimumParticipants = 2U;
constexpr uint8_t kMaximumParticipants = 4U;
constexpr uint8_t kMaximumRounds = 3U;
constexpr uint8_t kLateJoinLastRound = 2U;
constexpr uint8_t kHideSlotCount = 8U;
constexpr uint8_t kHiddenSlotUnavailable = UINT8_MAX;
constexpr uint8_t kStoryChoiceCount = 3U;
constexpr uint16_t kMaximumWindowSeconds = 600U;

static_assert(kWireBytes <= 32U,
              "M8 party packet must fit the direct LoRa payload budget");

// Values deliberately match social::HotspotMode for the shared modes. The
// extension rejects SignalHunt so the caller delegates mode 0 to the existing
// kitsu_party_hotspot P8 implementation.
enum class Mode : uint8_t {
  SignalHunt = 0U,
  Triangulation = 1U,
  HotCold = 2U,
  HideAndSeek = 3U,
  SyncRhythm = 4U,
  CoopEcho = 5U,
  AsyncRelay = 6U,
  RareEncounter = 7U,
  SharedTrail = 8U,
  StoryVote = 9U,
  Count = 10U,
};

enum class PacketType : uint8_t {
  Beacon = 1U,
  JoinRequest = 2U,
  Welcome = 3U,
  Ready = 4U,
  RoundOpen = 5U,
  Contribution = 6U,
  Result = 7U,
  Cancel = 8U,
};

enum class HostPhase : uint8_t {
  Idle = 0U,
  Lobby,
  Active,
  Complete,
  Cancelled,
  Expired,
  Unavailable,
};

enum class ParticipantPhase : uint8_t {
  Idle = 0U,
  Observed,
  Joining,
  Lobby,
  Active,
  Complete,
  Cancelled,
  Expired,
  Unavailable,
};

enum class Status : uint8_t {
  Ok = 0U,
  NullArgument,
  WrongLength,
  BufferTooSmall,
  BadMagic,
  UnsupportedVersion,
  UnsupportedType,
  UnsupportedMode,
  LegacySignalHunt,
  InvalidPacket,
  InvalidArgument,
  InvalidSourceUid,
  InvalidHostUid,
  InvalidSessionNonce,
  InvalidSequence,
  InvalidRound,
  InvalidFlags,
  InvalidParticipantCount,
  InvalidContribution,
  InvalidWindow,
  UnexpectedField,
  IntegrityMismatch,
  WrongPhase,
  WrongSession,
  WrongHost,
  Duplicate,
  StaleSequence,
  SequenceConflict,
  SequenceExhausted,
  PartyFull,
  UnknownParticipant,
  NotReady,
  JoinClosed,
  RoundMismatch,
  AlreadySubmitted,
  TimedOut,
  InvalidState,
  StateUnavailable,
};

constexpr uint8_t kFlagJoinOpen = 0x01U;
constexpr uint8_t kFlagActive = 0x02U;
constexpr uint8_t kFlagReady = 0x04U;
constexpr uint8_t kFlagRareEncounter = 0x08U;

// Fixed 32-byte wire layout (all multi-byte values little-endian):
//   0..1   magic "M8"
//   2      protocol version
//   3      PacketType
//   4      Mode
//   5      round (0 in lobby, otherwise 1..3)
//   6      flags
//   7      participant count
//   8..9   source UID suffix
//   10..11 host UID suffix
//   12..15 host-generated session nonce
//   16..17 per-source sequence
//   18..19 signed value (ready, RSSI, score, or member slot)
//   20..23 dataA (seed or typed contribution value)
//   24..27 dataB (accepted UID, prompt, packed outcome, or result proof)
//   28..29 remaining/open window seconds
//   30..31 CRC-16/CCITT-FALSE over bytes 0..29
// The CRC detects radio damage; it does not authenticate peers. The caller
// must retain the direct-radio trust, rate and airtime policy used by P8.
struct Packet {
  PacketType type = PacketType::Beacon;
  Mode mode = Mode::Triangulation;
  uint8_t round = 0U;
  uint8_t flags = 0U;
  uint8_t participantCount = 0U;
  uint16_t sourceUid = 0U;
  uint16_t hostUid = 0U;
  uint32_t sessionNonce = 0U;
  uint16_t sequence = 0U;
  int16_t value = 0;
  uint32_t dataA = 0U;
  uint32_t dataB = 0U;
  uint16_t windowSeconds = 0U;
};

Status encode(const Packet& packet, uint8_t* output, size_t capacity,
              size_t* bytesWritten = nullptr);
Status decode(const uint8_t* wire, size_t length, Packet& output);
Status validate(const Packet& packet);
const char* statusName(Status status);
const char* modeName(Mode mode);
uint16_t crc16CcittFalse(const uint8_t* data, size_t length);
uint32_t packetFingerprint(const Packet& packet);

struct ReplayCursor {
  uint16_t lastSequence = 0U;
  uint16_t reserved = 0U;
  uint32_t lastFingerprint = 0U;
};

// Only one field is meaningful for a given mode. Keeping both fixed-width
// fields avoids unions and makes radio/UI adapters simple.
struct Contribution {
  int16_t signalRssi = 0;  // triangulation and hot/cold: -140..0 dBm.
  uint16_t value = 0U;     // slot, tap offset, pattern, points, misses, vote.
};

struct RoundPrompt {
  uint8_t round = 0U;
  // Host promptFor() receives private session entropy and contains the slot.
  // ParticipantSession::prompt() returns kHiddenSlotUnavailable until result.
  uint8_t hideSlot = kHiddenSlotUnavailable;
  uint8_t echoBeats = 0U;
  uint8_t reserved = 0U;
  uint16_t echoPattern = 0U;
  uint16_t syncPerfectMs = 80U;
  uint16_t syncMaximumMs = 600U;
};

struct ModeResult {
  uint8_t participantCount = 0U;
  uint8_t completedRounds = 0U;
  uint8_t rareEncounter = 0U;
  uint8_t storyChoice = 0U;
  uint8_t temperature =
      static_cast<uint8_t>(social::TemperatureHint::Unknown);
  uint8_t reserved = 0U;
  uint16_t score = 0U;
  uint16_t maximumScore = 1000U;
  uint16_t outcomeValue = 0U;
  uint32_t proof = 0U;
};

struct MemberState {
  uint16_t uid = 0U;
  uint8_t active = 0U;
  uint8_t ready = 0U;
  uint8_t joinedRound = 0U;
  uint8_t submittedMask = 0U;
  ReplayCursor replay{};
  int16_t signals[kMaximumRounds]{};
  uint16_t values[kMaximumRounds]{};
};

struct HostState {
  uint8_t schemaVersion = 1U;
  uint8_t phase = static_cast<uint8_t>(HostPhase::Idle);
  uint8_t mode = static_cast<uint8_t>(Mode::Triangulation);
  uint8_t participantCount = 0U;
  uint8_t currentRound = 0U;
  uint8_t totalRounds = 0U;
  uint16_t hostUid = 0U;
  uint16_t txSequence = 0U;
  uint16_t lobbyWindowSeconds = 0U;
  uint16_t roundWindowSeconds = 0U;
  uint32_t sessionNonce = 0U;
  uint32_t seed = 0U;
  uint32_t deadlineMs = 0U;
  MemberState members[kMaximumParticipants]{};
  ModeResult result{};
};

struct ParticipantState {
  uint8_t schemaVersion = 1U;
  uint8_t phase = static_cast<uint8_t>(ParticipantPhase::Idle);
  uint8_t mode = static_cast<uint8_t>(Mode::Triangulation);
  uint8_t participantCount = 0U;
  uint8_t currentRound = 0U;
  uint8_t totalRounds = 0U;
  uint8_t ready = 0U;
  uint8_t readyCount = 0U;
  uint8_t submittedMask = 0U;
  uint16_t localUid = 0U;
  uint16_t hostUid = 0U;
  uint16_t txSequence = 0U;
  uint16_t reserved = 0U;
  uint32_t sessionNonce = 0U;
  uint32_t seed = 0U;
  uint32_t deadlineMs = 0U;
  ReplayCursor hostReplay{};
  int16_t priorSignal = 0;
  uint8_t hasPriorSignal = 0U;
  uint8_t temperature =
      static_cast<uint8_t>(social::TemperatureHint::Unknown);
  ModeResult result{};
};

static_assert(sizeof(HostState) <= 256U,
              "M8 host state must remain fixed and small");
static_assert(sizeof(ParticipantState) <= 96U,
              "M8 participant state must remain fixed and small");

bool validateHostState(const HostState& state);
bool validateParticipantState(const ParticipantState& state);
uint8_t roundsForMode(Mode mode);
Mode rotatingMode(uint32_t dayId, uint32_t groupSeed);
RoundPrompt promptFor(Mode mode, uint32_t seed, uint8_t round);
bool validContribution(Mode mode, const Contribution& contribution,
                       const RoundPrompt& prompt);

class HostSession {
 public:
  Status start(uint16_t hostUid, uint32_t sessionNonce, Mode mode,
               uint32_t seed, uint32_t nowMs,
               uint16_t lobbyWindowSeconds,
               uint16_t roundWindowSeconds);
  void reset();

  Status makeBeacon(uint32_t nowMs, Packet& output);
  Status acceptJoin(const Packet& request, uint32_t nowMs, Packet& welcome);
  Status setHostReady(bool ready);
  Status acceptReady(const Packet& readyPacket);
  bool canStart() const;
  Status begin(uint32_t nowMs, Packet& roundOpen);

  Status submitHostContribution(const Contribution& contribution);
  Status acceptContribution(const Packet& contribution, uint32_t nowMs);
  Status advance(uint32_t nowMs, Packet& output);
  Status cancel(Packet& output);
  Status expireIfNeeded(uint32_t nowMs);

  const HostState& state() const { return state_; }
  HostState snapshot() const { return state_; }
  Status restore(const HostState& state);

 private:
  MemberState* member(uint16_t uid);
  const MemberState* member(uint16_t uid) const;
  bool allCurrentContributions() const;
  Status makeHostPacket(PacketType type, uint8_t round, uint8_t flags,
                        int16_t value, uint32_t dataA, uint32_t dataB,
                        uint16_t windowSeconds, Packet& output);
  Status resolveResult(ModeResult& output) const;

  HostState state_{};
  bool available_ = true;
};

class ParticipantSession {
 public:
  Status observeBeacon(uint16_t localUid, const Packet& beacon,
                       uint32_t nowMs);
  Status makeJoinRequest(Packet& output);
  Status acceptHostPacket(const Packet& packet, uint32_t nowMs);
  Status makeReady(bool ready, Packet& output);
  Status makeContribution(const Contribution& contribution, Packet& output);
  Status expireIfNeeded(uint32_t nowMs);
  void reset();

  social::TemperatureHint temperatureHint() const;
  RoundPrompt prompt() const;
  const ParticipantState& state() const { return state_; }
  ParticipantState snapshot() const { return state_; }
  Status restore(const ParticipantState& state);

 private:
  Status makeParticipantPacket(PacketType type, uint8_t round, int16_t value,
                               uint32_t dataA, Packet& output);

  ParticipantState state_{};
  bool available_ = true;
};

}  // namespace party_modes
}  // namespace kitsu868
