#include "kitsu_party_modes.h"

#include <limits.h>
#include <string.h>

namespace kitsu868 {
namespace party_modes {
namespace {

constexpr uint8_t kKnownFlags = kFlagJoinOpen | kFlagActive | kFlagReady |
                                kFlagRareEncounter;
constexpr uint8_t kAllRoundsMask =
    static_cast<uint8_t>((1U << kMaximumRounds) - 1U);

uint16_t readU16(const uint8_t* input) {
  return static_cast<uint16_t>(input[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8U);
}

uint32_t readU32(const uint8_t* input) {
  return static_cast<uint32_t>(input[0]) |
         (static_cast<uint32_t>(input[1]) << 8U) |
         (static_cast<uint32_t>(input[2]) << 16U) |
         (static_cast<uint32_t>(input[3]) << 24U);
}

void writeU16(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value & 0xffU);
  output[1] = static_cast<uint8_t>((value >> 8U) & 0xffU);
}

void writeU32(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value & 0xffU);
  output[1] = static_cast<uint8_t>((value >> 8U) & 0xffU);
  output[2] = static_cast<uint8_t>((value >> 16U) & 0xffU);
  output[3] = static_cast<uint8_t>((value >> 24U) & 0xffU);
}

uint32_t mix32(uint32_t value) {
  value ^= value >> 16U;
  value *= UINT32_C(0x7FEB352D);
  value ^= value >> 15U;
  value *= UINT32_C(0x846CA68B);
  value ^= value >> 16U;
  return value;
}

uint32_t fnvByte(uint32_t hash, uint8_t value) {
  return (hash ^ value) * UINT32_C(16777619);
}

uint32_t fnv16(uint32_t hash, uint16_t value) {
  hash = fnvByte(hash, static_cast<uint8_t>(value & 0xffU));
  return fnvByte(hash, static_cast<uint8_t>((value >> 8U) & 0xffU));
}

uint32_t fnv32(uint32_t hash, uint32_t value) {
  hash = fnv16(hash, static_cast<uint16_t>(value & 0xffffU));
  return fnv16(hash, static_cast<uint16_t>((value >> 16U) & 0xffffU));
}

uint32_t nonZero(uint32_t value) {
  return value == 0U ? UINT32_C(0x4D385231) : value;
}

bool validModeValue(uint8_t value, bool allowLegacy = false) {
  return value < static_cast<uint8_t>(Mode::Count) &&
         (allowLegacy || value != static_cast<uint8_t>(Mode::SignalHunt));
}

bool validPacketType(uint8_t value) {
  return value >= static_cast<uint8_t>(PacketType::Beacon) &&
         value <= static_cast<uint8_t>(PacketType::Cancel);
}

bool validHostPhase(uint8_t value) {
  return value <= static_cast<uint8_t>(HostPhase::Unavailable);
}

bool validParticipantPhase(uint8_t value) {
  return value <= static_cast<uint8_t>(ParticipantPhase::Unavailable);
}

bool validWindow(uint16_t seconds) {
  return seconds != 0U && seconds <= kMaximumWindowSeconds;
}

uint32_t deadline(uint32_t nowMs, uint16_t seconds) {
  return nowMs + static_cast<uint32_t>(seconds) * 1000U;
}

bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
  return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

uint16_t remainingSeconds(uint32_t nowMs, uint32_t deadlineMs) {
  if (deadlineReached(nowMs, deadlineMs)) return 0U;
  const uint32_t remainingMs = deadlineMs - nowMs;
  const uint32_t rounded = (remainingMs + 999U) / 1000U;
  return static_cast<uint16_t>(rounded > kMaximumWindowSeconds
                                   ? kMaximumWindowSeconds
                                   : rounded);
}

uint32_t packedPrompt(const RoundPrompt& prompt) {
  return static_cast<uint32_t>(prompt.hideSlot) |
         (static_cast<uint32_t>(prompt.echoBeats) << 8U) |
         (static_cast<uint32_t>(prompt.echoPattern) << 16U);
}

uint32_t publicSeedFor(Mode mode, uint32_t privateSeed) {
  if (mode != Mode::HideAndSeek) return privateSeed;
  return nonZero(mix32(privateSeed ^ UINT32_C(0x48494445)));
}

uint32_t wirePromptFor(Mode mode, uint32_t publicSeed, uint8_t round) {
  // Hide-and-seek never broadcasts its answer. Modes without a challenge
  // prompt need no payload; Echo Beat publishes the pattern players repeat.
  return mode == Mode::CoopEcho
             ? packedPrompt(promptFor(mode, publicSeed, round))
             : 0U;
}

uint32_t packedResult(const ModeResult& result) {
  return static_cast<uint32_t>(result.outcomeValue) |
         (static_cast<uint32_t>(result.temperature) << 16U) |
         (static_cast<uint32_t>(result.storyChoice) << 24U);
}

bool replayCursorValid(const ReplayCursor& cursor) {
  return cursor.reserved == 0U &&
         ((cursor.lastSequence == 0U) == (cursor.lastFingerprint == 0U));
}

bool emptyMember(const MemberState& member) {
  if (member.uid != 0U || member.active != 0U || member.ready != 0U ||
      member.joinedRound != 0U || member.submittedMask != 0U ||
      member.replay.lastSequence != 0U || member.replay.reserved != 0U ||
      member.replay.lastFingerprint != 0U) {
    return false;
  }
  for (uint8_t round = 0U; round < kMaximumRounds; ++round) {
    if (member.signals[round] != 0 || member.values[round] != 0U) {
      return false;
    }
  }
  return true;
}

enum class ReplayDecision : uint8_t {
  Fresh,
  Duplicate,
  Stale,
  Conflict,
};

ReplayDecision replayDecision(const ReplayCursor& cursor,
                              const Packet& packet) {
  if (cursor.lastSequence == 0U) return ReplayDecision::Fresh;
  if (packet.sequence == cursor.lastSequence) {
    return packetFingerprint(packet) == cursor.lastFingerprint
               ? ReplayDecision::Duplicate
               : ReplayDecision::Conflict;
  }
  const uint16_t delta = static_cast<uint16_t>(packet.sequence -
                                               cursor.lastSequence);
  return delta < 0x8000U ? ReplayDecision::Fresh : ReplayDecision::Stale;
}

Status replayStatus(ReplayDecision decision) {
  switch (decision) {
    case ReplayDecision::Duplicate: return Status::Duplicate;
    case ReplayDecision::Stale: return Status::StaleSequence;
    case ReplayDecision::Conflict: return Status::SequenceConflict;
    case ReplayDecision::Fresh: return Status::Ok;
  }
  return Status::InvalidState;
}

void rememberPacket(ReplayCursor& cursor, const Packet& packet) {
  cursor.lastSequence = packet.sequence;
  cursor.lastFingerprint = packetFingerprint(packet);
}

bool resultValid(const ModeResult& result, bool populated) {
  if (result.participantCount > kMaximumParticipants ||
      result.completedRounds > kMaximumRounds ||
      result.rareEncounter > 1U ||
      result.storyChoice >= kStoryChoiceCount ||
      result.temperature >
          static_cast<uint8_t>(social::TemperatureHint::MuchHotter) ||
      result.reserved != 0U || result.maximumScore != 1000U ||
      result.score > result.maximumScore) {
    return false;
  }
  if (!populated) {
    return result.participantCount == 0U && result.completedRounds == 0U &&
           result.rareEncounter == 0U && result.storyChoice == 0U &&
           result.temperature ==
               static_cast<uint8_t>(social::TemperatureHint::Unknown) &&
           result.score == 0U && result.outcomeValue == 0U &&
           result.proof == 0U;
  }
  return result.participantCount >= kMinimumParticipants &&
         result.completedRounds != 0U && result.proof != 0U;
}

uint8_t readyCount(const HostState& state) {
  uint8_t count = 0U;
  for (uint8_t index = 0U; index < state.participantCount; ++index) {
    if (state.members[index].active != 0U &&
        state.members[index].ready != 0U) {
      ++count;
    }
  }
  return count;
}

uint16_t clampScore(uint32_t score) {
  return static_cast<uint16_t>(score > 1000U ? 1000U : score);
}

uint16_t meanScore(uint32_t total, uint16_t count) {
  return count == 0U ? 0U : clampScore(total / count);
}

}  // namespace

uint16_t crc16CcittFalse(const uint8_t* data, size_t length) {
  if (!data && length != 0U) return 0U;
  uint16_t crc = 0xffffU;
  for (size_t index = 0U; index < length; ++index) {
    crc ^= static_cast<uint16_t>(data[index]) << 8U;
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      crc = (crc & 0x8000U) != 0U
                ? static_cast<uint16_t>((crc << 1U) ^ 0x1021U)
                : static_cast<uint16_t>(crc << 1U);
    }
  }
  return crc;
}

uint32_t packetFingerprint(const Packet& packet) {
  uint32_t hash = UINT32_C(2166136261);
  hash = fnvByte(hash, static_cast<uint8_t>(packet.type));
  hash = fnvByte(hash, static_cast<uint8_t>(packet.mode));
  hash = fnvByte(hash, packet.round);
  hash = fnvByte(hash, packet.flags);
  hash = fnvByte(hash, packet.participantCount);
  hash = fnv16(hash, packet.sourceUid);
  hash = fnv16(hash, packet.hostUid);
  hash = fnv32(hash, packet.sessionNonce);
  hash = fnv16(hash, packet.sequence);
  hash = fnv16(hash, static_cast<uint16_t>(packet.value));
  hash = fnv32(hash, packet.dataA);
  hash = fnv32(hash, packet.dataB);
  hash = fnv16(hash, packet.windowSeconds);
  return nonZero(hash);
}

const char* modeName(Mode mode) {
  switch (mode) {
    case Mode::SignalHunt: return "signal_hunt";
    case Mode::Triangulation: return "triangulation";
    case Mode::HotCold: return "hot_cold";
    case Mode::HideAndSeek: return "hide_and_seek";
    case Mode::SyncRhythm: return "sync_rhythm";
    case Mode::CoopEcho: return "coop_echo";
    case Mode::AsyncRelay: return "async_relay";
    case Mode::RareEncounter: return "rare_encounter";
    case Mode::SharedTrail: return "shared_trail";
    case Mode::StoryVote: return "story_vote";
    case Mode::Count: break;
  }
  return "unknown";
}

const char* statusName(Status status) {
  switch (status) {
    case Status::Ok: return "ok";
    case Status::NullArgument: return "null_argument";
    case Status::WrongLength: return "wrong_length";
    case Status::BufferTooSmall: return "buffer_too_small";
    case Status::BadMagic: return "bad_magic";
    case Status::UnsupportedVersion: return "unsupported_version";
    case Status::UnsupportedType: return "unsupported_type";
    case Status::UnsupportedMode: return "unsupported_mode";
    case Status::LegacySignalHunt: return "legacy_signal_hunt";
    case Status::InvalidPacket: return "invalid_packet";
    case Status::InvalidArgument: return "invalid_argument";
    case Status::InvalidSourceUid: return "invalid_source_uid";
    case Status::InvalidHostUid: return "invalid_host_uid";
    case Status::InvalidSessionNonce: return "invalid_session_nonce";
    case Status::InvalidSequence: return "invalid_sequence";
    case Status::InvalidRound: return "invalid_round";
    case Status::InvalidFlags: return "invalid_flags";
    case Status::InvalidParticipantCount: return "invalid_participant_count";
    case Status::InvalidContribution: return "invalid_contribution";
    case Status::InvalidWindow: return "invalid_window";
    case Status::UnexpectedField: return "unexpected_field";
    case Status::IntegrityMismatch: return "integrity_mismatch";
    case Status::WrongPhase: return "wrong_phase";
    case Status::WrongSession: return "wrong_session";
    case Status::WrongHost: return "wrong_host";
    case Status::Duplicate: return "duplicate";
    case Status::StaleSequence: return "stale_sequence";
    case Status::SequenceConflict: return "sequence_conflict";
    case Status::SequenceExhausted: return "sequence_exhausted";
    case Status::PartyFull: return "party_full";
    case Status::UnknownParticipant: return "unknown_participant";
    case Status::NotReady: return "not_ready";
    case Status::JoinClosed: return "join_closed";
    case Status::RoundMismatch: return "round_mismatch";
    case Status::AlreadySubmitted: return "already_submitted";
    case Status::TimedOut: return "timed_out";
    case Status::InvalidState: return "invalid_state";
    case Status::StateUnavailable: return "state_unavailable";
  }
  return "unknown";
}

uint8_t roundsForMode(Mode mode) {
  switch (mode) {
    case Mode::Triangulation:
    case Mode::HotCold:
    case Mode::AsyncRelay:
      return 3U;
    case Mode::HideAndSeek:
    case Mode::SyncRhythm:
    case Mode::CoopEcho:
    case Mode::RareEncounter:
    case Mode::SharedTrail:
    case Mode::StoryVote:
      return 1U;
    case Mode::SignalHunt:
    case Mode::Count:
      break;
  }
  return 0U;
}

Mode rotatingMode(uint32_t dayId, uint32_t groupSeed) {
  return static_cast<Mode>(mix32(dayId ^ groupSeed ^ UINT32_C(0x4D38524F)) %
                           static_cast<uint8_t>(Mode::Count));
}

RoundPrompt promptFor(Mode mode, uint32_t seed, uint8_t round) {
  RoundPrompt prompt{};
  if (!validModeValue(static_cast<uint8_t>(mode)) || round == 0U ||
      round > roundsForMode(mode)) {
    return prompt;
  }
  prompt.round = round;
  const uint32_t mixed = mix32(seed ^
      (static_cast<uint32_t>(static_cast<uint8_t>(mode)) << 24U) ^
      (static_cast<uint32_t>(round) * UINT32_C(0x9E3779B9)));
  prompt.hideSlot = static_cast<uint8_t>(mixed % kHideSlotCount);
  prompt.echoBeats = static_cast<uint8_t>(5U + ((mixed >> 8U) % 4U));
  const uint16_t mask = static_cast<uint16_t>(
      (UINT16_C(1) << prompt.echoBeats) - 1U);
  prompt.echoPattern = static_cast<uint16_t>((mixed >> 16U) & mask);
  return prompt;
}

bool validContribution(Mode mode, const Contribution& contribution,
                       const RoundPrompt& prompt) {
  if (prompt.round == 0U) return false;
  switch (mode) {
    case Mode::Triangulation:
    case Mode::HotCold:
      return contribution.signalRssi >= -140 &&
             contribution.signalRssi <= 0 && contribution.value == 0U;
    case Mode::HideAndSeek:
      return contribution.signalRssi == 0 &&
             contribution.value < kHideSlotCount;
    case Mode::SyncRhythm:
      return contribution.signalRssi == 0 && contribution.value <= 2000U;
    case Mode::CoopEcho: {
      const uint16_t mask = static_cast<uint16_t>(
          (UINT16_C(1) << prompt.echoBeats) - 1U);
      return contribution.signalRssi == 0 &&
             (contribution.value & static_cast<uint16_t>(~mask)) == 0U;
    }
    case Mode::AsyncRelay:
    case Mode::RareEncounter:
      return contribution.signalRssi == 0 && contribution.value <= 1000U;
    case Mode::SharedTrail:
      return contribution.signalRssi == 0 && contribution.value <= 20U;
    case Mode::StoryVote:
      return contribution.signalRssi == 0 &&
             contribution.value < kStoryChoiceCount;
    case Mode::SignalHunt:
    case Mode::Count:
      return false;
  }
  return false;
}

Status validate(const Packet& packet) {
  const uint8_t typeValue = static_cast<uint8_t>(packet.type);
  const uint8_t modeValue = static_cast<uint8_t>(packet.mode);
  if (!validPacketType(typeValue)) return Status::UnsupportedType;
  if (!validModeValue(modeValue)) {
    return modeValue == static_cast<uint8_t>(Mode::SignalHunt)
               ? Status::LegacySignalHunt
               : Status::UnsupportedMode;
  }
  if (packet.sourceUid == 0U) return Status::InvalidSourceUid;
  if (packet.hostUid == 0U) return Status::InvalidHostUid;
  if (packet.sessionNonce == 0U) return Status::InvalidSessionNonce;
  if (packet.sequence == 0U) return Status::InvalidSequence;
  if ((packet.flags & static_cast<uint8_t>(~kKnownFlags)) != 0U) {
    return Status::InvalidFlags;
  }
  if (packet.participantCount < 1U ||
      packet.participantCount > kMaximumParticipants) {
    return Status::InvalidParticipantCount;
  }
  const uint8_t rounds = roundsForMode(packet.mode);
  const bool inRound = packet.round != 0U;
  if (packet.round > rounds) return Status::InvalidRound;

  switch (packet.type) {
    case PacketType::Beacon:
      if (packet.sourceUid != packet.hostUid ||
          (inRound != ((packet.flags & kFlagActive) != 0U)) ||
          (packet.flags & (kFlagReady | kFlagRareEncounter)) != 0U ||
          packet.value < 0 || packet.value > packet.participantCount ||
          packet.dataA == 0U || packet.dataB != 0U ||
          !validWindow(packet.windowSeconds)) {
        return Status::UnexpectedField;
      }
      break;
    case PacketType::JoinRequest:
      if (packet.sourceUid == packet.hostUid ||
          (packet.flags & ~kFlagActive) != 0U ||
          packet.value != 0 || packet.dataA != 0U || packet.dataB != 0U ||
          packet.windowSeconds != 0U) {
        return Status::UnexpectedField;
      }
      break;
    case PacketType::Welcome:
      if (packet.sourceUid != packet.hostUid || packet.value < 1 ||
          packet.value >= kMaximumParticipants || packet.dataA == 0U ||
          packet.dataB == 0U || (packet.dataB >> 16U) != 0U ||
          (packet.flags & (kFlagJoinOpen | kFlagRareEncounter)) != 0U ||
          (inRound != ((packet.flags & kFlagActive) != 0U)) ||
          !validWindow(packet.windowSeconds)) {
        return Status::UnexpectedField;
      }
      break;
    case PacketType::Ready:
      if (packet.sourceUid == packet.hostUid || packet.round != 0U ||
          packet.flags != 0U || (packet.value != 0 && packet.value != 1) ||
          packet.dataA != 0U || packet.dataB != 0U ||
          packet.windowSeconds != 0U) {
        return Status::UnexpectedField;
      }
      break;
    case PacketType::RoundOpen:
      if (packet.sourceUid != packet.hostUid || !inRound ||
          packet.participantCount < kMinimumParticipants ||
          packet.flags != kFlagActive || packet.value != 0 ||
          packet.dataA == 0U || packet.dataB !=
              wirePromptFor(packet.mode, packet.dataA, packet.round) ||
          !validWindow(packet.windowSeconds)) {
        return Status::UnexpectedField;
      }
      break;
    case PacketType::Contribution: {
      Contribution contribution{};
      contribution.signalRssi = packet.value;
      contribution.value = static_cast<uint16_t>(packet.dataA & 0xffffU);
      if (packet.sourceUid == packet.hostUid || !inRound ||
          packet.participantCount < kMinimumParticipants ||
          packet.flags != kFlagActive || (packet.dataA >> 16U) != 0U ||
          packet.dataB != 0U || packet.windowSeconds != 0U ||
          !validContribution(packet.mode, contribution,
                             promptFor(packet.mode, UINT32_C(1),
                                       packet.round))) {
        // Echo validity depends on the real seed/prompt and is checked by the
        // session. Generic validation only rejects impossible high bits.
        if (packet.mode != Mode::CoopEcho || packet.sourceUid == packet.hostUid ||
            !inRound || packet.flags != kFlagActive ||
            (packet.dataA >> 16U) != 0U || packet.dataB != 0U ||
            packet.windowSeconds != 0U || packet.value != 0) {
          return Status::InvalidContribution;
        }
      }
      break;
    }
    case PacketType::Result:
      if (packet.sourceUid != packet.hostUid || !inRound ||
          packet.round != rounds ||
          packet.participantCount < kMinimumParticipants ||
          (packet.flags & ~(kFlagActive | kFlagRareEncounter)) != 0U ||
          (packet.flags & kFlagActive) == 0U || packet.value < 0 ||
          packet.value > 1000 || packet.dataB == 0U ||
          packet.windowSeconds != 0U) {
        return Status::UnexpectedField;
      }
      if (((packet.dataA >> 16U) & 0xffU) >
              static_cast<uint8_t>(social::TemperatureHint::MuchHotter) ||
          ((packet.dataA >> 24U) & 0xffU) >= kStoryChoiceCount ||
          (packet.mode == Mode::StoryVote &&
           static_cast<uint16_t>(packet.dataA & 0xffffU) !=
               static_cast<uint8_t>((packet.dataA >> 24U) & 0xffU))) {
        return Status::UnexpectedField;
      }
      break;
    case PacketType::Cancel:
      if (packet.sourceUid != packet.hostUid || packet.round != 0U ||
          packet.flags != 0U || packet.value != 0 || packet.dataA != 0U ||
          packet.dataB != 0U || packet.windowSeconds != 0U) {
        return Status::UnexpectedField;
      }
      break;
  }
  return Status::Ok;
}

Status encode(const Packet& packet, uint8_t* output, size_t capacity,
              size_t* bytesWritten) {
  if (!output) return Status::NullArgument;
  if (capacity < kWireBytes) return Status::BufferTooSmall;
  const Status status = validate(packet);
  if (status != Status::Ok) return status;
  memset(output, 0, kWireBytes);
  output[0] = kMagic0;
  output[1] = kMagic1;
  output[2] = kProtocolVersion;
  output[3] = static_cast<uint8_t>(packet.type);
  output[4] = static_cast<uint8_t>(packet.mode);
  output[5] = packet.round;
  output[6] = packet.flags;
  output[7] = packet.participantCount;
  writeU16(output + 8U, packet.sourceUid);
  writeU16(output + 10U, packet.hostUid);
  writeU32(output + 12U, packet.sessionNonce);
  writeU16(output + 16U, packet.sequence);
  writeU16(output + 18U, static_cast<uint16_t>(packet.value));
  writeU32(output + 20U, packet.dataA);
  writeU32(output + 24U, packet.dataB);
  writeU16(output + 28U, packet.windowSeconds);
  writeU16(output + 30U, crc16CcittFalse(output, 30U));
  if (bytesWritten) *bytesWritten = kWireBytes;
  return Status::Ok;
}

Status decode(const uint8_t* wire, size_t length, Packet& output) {
  if (!wire) return Status::NullArgument;
  if (length != kWireBytes) return Status::WrongLength;
  if (wire[0] != kMagic0 || wire[1] != kMagic1) return Status::BadMagic;
  if (wire[2] != kProtocolVersion) return Status::UnsupportedVersion;
  if (readU16(wire + 30U) != crc16CcittFalse(wire, 30U)) {
    return Status::IntegrityMismatch;
  }
  Packet candidate{};
  candidate.type = static_cast<PacketType>(wire[3]);
  candidate.mode = static_cast<Mode>(wire[4]);
  candidate.round = wire[5];
  candidate.flags = wire[6];
  candidate.participantCount = wire[7];
  candidate.sourceUid = readU16(wire + 8U);
  candidate.hostUid = readU16(wire + 10U);
  candidate.sessionNonce = readU32(wire + 12U);
  candidate.sequence = readU16(wire + 16U);
  candidate.value = static_cast<int16_t>(readU16(wire + 18U));
  candidate.dataA = readU32(wire + 20U);
  candidate.dataB = readU32(wire + 24U);
  candidate.windowSeconds = readU16(wire + 28U);
  const Status status = validate(candidate);
  if (status != Status::Ok) return status;
  output = candidate;
  return Status::Ok;
}

bool validateHostState(const HostState& state) {
  if (state.schemaVersion != 1U || !validHostPhase(state.phase) ||
      !validModeValue(state.mode) || state.currentRound > state.totalRounds ||
      state.participantCount > kMaximumParticipants) {
    return false;
  }
  const HostPhase phase = static_cast<HostPhase>(state.phase);
  if (phase == HostPhase::Idle) {
    bool membersEmpty = true;
    for (uint8_t index = 0U; index < kMaximumParticipants; ++index) {
      membersEmpty = membersEmpty && emptyMember(state.members[index]);
    }
    return state.mode == static_cast<uint8_t>(Mode::Triangulation) &&
           state.participantCount == 0U && state.currentRound == 0U &&
           state.totalRounds == 0U &&
           state.hostUid == 0U && state.txSequence == 0U &&
           state.lobbyWindowSeconds == 0U &&
           state.roundWindowSeconds == 0U &&
           state.sessionNonce == 0U && state.seed == 0U &&
           state.deadlineMs == 0U && membersEmpty &&
           resultValid(state.result, false);
  }
  if (phase == HostPhase::Unavailable ||
      state.totalRounds != roundsForMode(static_cast<Mode>(state.mode)) ||
      state.hostUid == 0U ||
      state.sessionNonce == 0U || state.seed == 0U ||
      !validWindow(state.lobbyWindowSeconds) ||
      !validWindow(state.roundWindowSeconds) ||
      state.participantCount == 0U) {
    return false;
  }
  if ((phase == HostPhase::Lobby && state.currentRound != 0U) ||
      (phase == HostPhase::Active && state.currentRound == 0U) ||
      (phase == HostPhase::Active &&
       state.participantCount < kMinimumParticipants) ||
      (phase == HostPhase::Complete &&
       state.currentRound != state.totalRounds) ||
      (phase == HostPhase::Expired && state.currentRound != 0U) ||
      ((phase == HostPhase::Complete || phase == HostPhase::Cancelled ||
        phase == HostPhase::Expired) && state.deadlineMs != 0U)) {
    return false;
  }
  uint8_t active = 0U;
  for (uint8_t index = 0U; index < kMaximumParticipants; ++index) {
    const MemberState& member = state.members[index];
    if (member.active > 1U || member.ready > 1U ||
        (member.submittedMask & ~kAllRoundsMask) != 0U ||
        !replayCursorValid(member.replay)) {
      return false;
    }
    if (member.active == 0U) {
      if (!emptyMember(member)) return false;
      continue;
    }
    ++active;
    if (member.uid == 0U || member.joinedRound > state.totalRounds ||
        (index == 0U && member.uid != state.hostUid) ||
        (index == 0U && member.replay.lastSequence != 0U)) {
      return false;
    }
    for (uint8_t prior = 0U; prior < index; ++prior) {
      if (state.members[prior].active != 0U &&
          state.members[prior].uid == member.uid) {
        return false;
      }
    }
  }
  if (active != state.participantCount) return false;
  if (phase == HostPhase::Complete &&
      (state.result.participantCount != state.participantCount ||
       state.result.completedRounds != state.totalRounds)) {
    return false;
  }
  return resultValid(state.result, phase == HostPhase::Complete);
}

bool validateParticipantState(const ParticipantState& state) {
  if (state.schemaVersion != 1U || !validParticipantPhase(state.phase) ||
      !validModeValue(state.mode) || state.ready > 1U ||
      state.readyCount > state.participantCount ||
      state.hasPriorSignal > 1U ||
      state.temperature >
          static_cast<uint8_t>(social::TemperatureHint::MuchHotter) ||
      (state.submittedMask & ~kAllRoundsMask) != 0U ||
      !replayCursorValid(state.hostReplay)) {
    return false;
  }
  const ParticipantPhase phase = static_cast<ParticipantPhase>(state.phase);
  if (phase == ParticipantPhase::Idle) {
    return state.mode == static_cast<uint8_t>(Mode::Triangulation) &&
           state.participantCount == 0U && state.currentRound == 0U &&
           state.totalRounds == 0U && state.ready == 0U &&
           state.readyCount == 0U &&
           state.submittedMask == 0U && state.localUid == 0U &&
           state.hostUid == 0U && state.txSequence == 0U &&
           state.reserved == 0U &&
           state.sessionNonce == 0U && state.seed == 0U &&
           state.deadlineMs == 0U && state.hostReplay.lastSequence == 0U &&
           state.hostReplay.lastFingerprint == 0U &&
           state.priorSignal == 0 && state.hasPriorSignal == 0U &&
           state.temperature == static_cast<uint8_t>(
                                    social::TemperatureHint::Unknown) &&
           resultValid(state.result, false);
  }
  if (phase == ParticipantPhase::Unavailable || state.localUid == 0U ||
      state.hostUid == 0U || state.localUid == state.hostUid ||
      state.sessionNonce == 0U || state.seed == 0U ||
      state.totalRounds != roundsForMode(static_cast<Mode>(state.mode)) ||
      state.currentRound > state.totalRounds ||
      state.participantCount == 0U ||
      state.participantCount > kMaximumParticipants || state.reserved != 0U) {
    return false;
  }
  if ((phase == ParticipantPhase::Lobby && state.currentRound != 0U) ||
      (phase == ParticipantPhase::Active && state.currentRound == 0U) ||
      (phase == ParticipantPhase::Complete &&
       state.currentRound != state.totalRounds) ||
      ((phase == ParticipantPhase::Complete ||
        phase == ParticipantPhase::Cancelled ||
        phase == ParticipantPhase::Expired) && state.deadlineMs != 0U)) {
    return false;
  }
  if (phase == ParticipantPhase::Complete &&
      (state.result.participantCount != state.participantCount ||
       state.result.completedRounds != state.totalRounds)) {
    return false;
  }
  return resultValid(state.result, phase == ParticipantPhase::Complete);
}

Status HostSession::start(uint16_t hostUid, uint32_t sessionNonce, Mode mode,
                          uint32_t seed, uint32_t nowMs,
                          uint16_t lobbyWindowSeconds,
                          uint16_t roundWindowSeconds) {
  if (mode == Mode::SignalHunt) return Status::LegacySignalHunt;
  if (!validModeValue(static_cast<uint8_t>(mode))) {
    return Status::UnsupportedMode;
  }
  if (hostUid == 0U || sessionNonce == 0U || seed == 0U ||
      !validWindow(lobbyWindowSeconds) ||
      !validWindow(roundWindowSeconds)) {
    return Status::InvalidArgument;
  }
  state_ = HostState{};
  state_.phase = static_cast<uint8_t>(HostPhase::Lobby);
  state_.mode = static_cast<uint8_t>(mode);
  state_.participantCount = 1U;
  state_.totalRounds = roundsForMode(mode);
  state_.hostUid = hostUid;
  state_.sessionNonce = sessionNonce;
  state_.seed = seed;
  state_.deadlineMs = deadline(nowMs, lobbyWindowSeconds);
  state_.lobbyWindowSeconds = lobbyWindowSeconds;
  state_.roundWindowSeconds = roundWindowSeconds;
  state_.members[0].uid = hostUid;
  state_.members[0].active = 1U;
  available_ = true;
  return Status::Ok;
}

void HostSession::reset() {
  state_ = HostState{};
  available_ = true;
}

MemberState* HostSession::member(uint16_t uid) {
  for (uint8_t index = 0U; index < state_.participantCount; ++index) {
    if (state_.members[index].active != 0U &&
        state_.members[index].uid == uid) {
      return &state_.members[index];
    }
  }
  return nullptr;
}

const MemberState* HostSession::member(uint16_t uid) const {
  for (uint8_t index = 0U; index < state_.participantCount; ++index) {
    if (state_.members[index].active != 0U &&
        state_.members[index].uid == uid) {
      return &state_.members[index];
    }
  }
  return nullptr;
}

Status HostSession::makeHostPacket(PacketType type, uint8_t round,
                                   uint8_t flags, int16_t value,
                                   uint32_t dataA, uint32_t dataB,
                                   uint16_t windowSeconds, Packet& output) {
  if (!available_) return Status::StateUnavailable;
  if (state_.txSequence == UINT16_MAX) return Status::SequenceExhausted;
  Packet candidate{};
  candidate.type = type;
  candidate.mode = static_cast<Mode>(state_.mode);
  candidate.round = round;
  candidate.flags = flags;
  candidate.participantCount = state_.participantCount;
  candidate.sourceUid = state_.hostUid;
  candidate.hostUid = state_.hostUid;
  candidate.sessionNonce = state_.sessionNonce;
  candidate.sequence = static_cast<uint16_t>(state_.txSequence + 1U);
  candidate.value = value;
  candidate.dataA = dataA;
  candidate.dataB = dataB;
  candidate.windowSeconds = windowSeconds;
  const Status status = validate(candidate);
  if (status != Status::Ok) return status;
  state_.txSequence = candidate.sequence;
  output = candidate;
  return Status::Ok;
}

Status HostSession::expireIfNeeded(uint32_t nowMs) {
  if (!available_) return Status::StateUnavailable;
  const HostPhase phase = static_cast<HostPhase>(state_.phase);
  if ((phase == HostPhase::Lobby || phase == HostPhase::Active) &&
      deadlineReached(nowMs, state_.deadlineMs)) {
    if (phase == HostPhase::Lobby) {
      state_.phase = static_cast<uint8_t>(HostPhase::Expired);
      state_.deadlineMs = 0U;
    }
    return Status::TimedOut;
  }
  return Status::Ok;
}

Status HostSession::makeBeacon(uint32_t nowMs, Packet& output) {
  if (!available_) return Status::StateUnavailable;
  const HostPhase phase = static_cast<HostPhase>(state_.phase);
  if (phase != HostPhase::Lobby && phase != HostPhase::Active) {
    return Status::WrongPhase;
  }
  if (expireIfNeeded(nowMs) == Status::TimedOut) {
    return Status::TimedOut;
  }
  const bool joinOpen = state_.participantCount < kMaximumParticipants &&
      (phase == HostPhase::Lobby || state_.currentRound <=
                                          kLateJoinLastRound);
  uint8_t flags = joinOpen ? kFlagJoinOpen : 0U;
  if (phase == HostPhase::Active) flags |= kFlagActive;
  return makeHostPacket(PacketType::Beacon, state_.currentRound, flags,
                        static_cast<int16_t>(readyCount(state_)),
                        publicSeedFor(static_cast<Mode>(state_.mode),
                                      state_.seed),
                        0U, remainingSeconds(nowMs, state_.deadlineMs), output);
}

Status HostSession::acceptJoin(const Packet& request, uint32_t nowMs,
                               Packet& welcome) {
  if (!available_) return Status::StateUnavailable;
  const HostPhase phase = static_cast<HostPhase>(state_.phase);
  if (phase != HostPhase::Lobby && phase != HostPhase::Active) {
    return Status::WrongPhase;
  }
  if (phase == HostPhase::Lobby && expireIfNeeded(nowMs) == Status::TimedOut) {
    return Status::TimedOut;
  }
  if (phase == HostPhase::Active &&
      deadlineReached(nowMs, state_.deadlineMs)) {
    return Status::TimedOut;
  }
  if (phase == HostPhase::Active && state_.currentRound >
                                        kLateJoinLastRound) {
    return Status::JoinClosed;
  }
  if (validate(request) != Status::Ok ||
      request.type != PacketType::JoinRequest) {
    return Status::InvalidPacket;
  }
  if (request.hostUid != state_.hostUid) return Status::WrongHost;
  if (request.sessionNonce != state_.sessionNonce ||
      request.mode != static_cast<Mode>(state_.mode)) {
    return Status::WrongSession;
  }
  const bool activeRequest = (request.flags & kFlagActive) != 0U;
  if (request.round != state_.currentRound ||
      activeRequest != (phase == HostPhase::Active)) {
    return Status::RoundMismatch;
  }
  MemberState* joined = member(request.sourceUid);
  if (joined) {
    const ReplayDecision decision = replayDecision(joined->replay, request);
    return decision == ReplayDecision::Fresh ? Status::Duplicate
                                              : replayStatus(decision);
  }
  if (state_.participantCount >= kMaximumParticipants) {
    return Status::PartyFull;
  }
  joined = &state_.members[state_.participantCount];
  *joined = MemberState{};
  joined->uid = request.sourceUid;
  joined->active = 1U;
  joined->ready = phase == HostPhase::Active ? 1U : 0U;
  joined->joinedRound = phase == HostPhase::Active ? state_.currentRound : 0U;
  rememberPacket(joined->replay, request);
  ++state_.participantCount;

  uint8_t flags = phase == HostPhase::Active ? kFlagActive | kFlagReady : 0U;
  const int16_t slot = static_cast<int16_t>(state_.participantCount - 1U);
  const Status status = makeHostPacket(
      PacketType::Welcome, state_.currentRound, flags,
      slot, publicSeedFor(static_cast<Mode>(state_.mode), state_.seed),
      request.sourceUid,
      remainingSeconds(nowMs, state_.deadlineMs), welcome);
  if (status != Status::Ok) {
    --state_.participantCount;
    state_.members[state_.participantCount] = MemberState{};
  }
  return status;
}

Status HostSession::setHostReady(bool ready) {
  if (!available_) return Status::StateUnavailable;
  if (static_cast<HostPhase>(state_.phase) != HostPhase::Lobby) {
    return Status::WrongPhase;
  }
  state_.members[0].ready = ready ? 1U : 0U;
  return Status::Ok;
}

Status HostSession::acceptReady(const Packet& readyPacket) {
  if (!available_) return Status::StateUnavailable;
  if (static_cast<HostPhase>(state_.phase) != HostPhase::Lobby) {
    return Status::WrongPhase;
  }
  if (validate(readyPacket) != Status::Ok ||
      readyPacket.type != PacketType::Ready) {
    return Status::InvalidPacket;
  }
  if (readyPacket.hostUid != state_.hostUid) return Status::WrongHost;
  if (readyPacket.sessionNonce != state_.sessionNonce ||
      readyPacket.mode != static_cast<Mode>(state_.mode)) {
    return Status::WrongSession;
  }
  MemberState* sender = member(readyPacket.sourceUid);
  if (!sender) return Status::UnknownParticipant;
  const ReplayDecision decision = replayDecision(sender->replay, readyPacket);
  if (decision != ReplayDecision::Fresh) return replayStatus(decision);
  sender->ready = readyPacket.value != 0 ? 1U : 0U;
  rememberPacket(sender->replay, readyPacket);
  return Status::Ok;
}

bool HostSession::canStart() const {
  if (!available_ || static_cast<HostPhase>(state_.phase) != HostPhase::Lobby ||
      state_.participantCount < kMinimumParticipants) {
    return false;
  }
  return readyCount(state_) == state_.participantCount;
}

Status HostSession::begin(uint32_t nowMs, Packet& roundOpen) {
  if (!available_) return Status::StateUnavailable;
  if (static_cast<HostPhase>(state_.phase) != HostPhase::Lobby) {
    return Status::WrongPhase;
  }
  if (!canStart()) return Status::NotReady;
  state_.phase = static_cast<uint8_t>(HostPhase::Active);
  state_.currentRound = 1U;
  state_.deadlineMs = deadline(nowMs, state_.roundWindowSeconds);
  const RoundPrompt prompt = promptFor(static_cast<Mode>(state_.mode),
                                       state_.seed, state_.currentRound);
  const Mode mode = static_cast<Mode>(state_.mode);
  const uint32_t publicSeed = publicSeedFor(mode, state_.seed);
  const Status status = makeHostPacket(
      PacketType::RoundOpen, state_.currentRound, kFlagActive, 0, publicSeed,
      wirePromptFor(mode, publicSeed, prompt.round),
      state_.roundWindowSeconds, roundOpen);
  if (status != Status::Ok) {
    state_.phase = static_cast<uint8_t>(HostPhase::Lobby);
    state_.currentRound = 0U;
  }
  return status;
}

Status HostSession::submitHostContribution(
    const Contribution& contribution) {
  if (!available_) return Status::StateUnavailable;
  if (static_cast<HostPhase>(state_.phase) != HostPhase::Active) {
    return Status::WrongPhase;
  }
  MemberState& host = state_.members[0];
  const uint8_t mask = static_cast<uint8_t>(1U << (state_.currentRound - 1U));
  if ((host.submittedMask & mask) != 0U) return Status::AlreadySubmitted;
  const RoundPrompt prompt = promptFor(static_cast<Mode>(state_.mode),
                                       state_.seed, state_.currentRound);
  if (!validContribution(static_cast<Mode>(state_.mode), contribution,
                         prompt)) {
    return Status::InvalidContribution;
  }
  const uint8_t index = static_cast<uint8_t>(state_.currentRound - 1U);
  host.signals[index] = contribution.signalRssi;
  host.values[index] = contribution.value;
  host.submittedMask |= mask;
  return Status::Ok;
}

Status HostSession::acceptContribution(const Packet& contributionPacket,
                                       uint32_t nowMs) {
  if (!available_) return Status::StateUnavailable;
  if (static_cast<HostPhase>(state_.phase) != HostPhase::Active) {
    return Status::WrongPhase;
  }
  if (deadlineReached(nowMs, state_.deadlineMs)) return Status::TimedOut;
  if (validate(contributionPacket) != Status::Ok ||
      contributionPacket.type != PacketType::Contribution) {
    return Status::InvalidPacket;
  }
  if (contributionPacket.hostUid != state_.hostUid) return Status::WrongHost;
  if (contributionPacket.sessionNonce != state_.sessionNonce ||
      contributionPacket.mode != static_cast<Mode>(state_.mode)) {
    return Status::WrongSession;
  }
  if (contributionPacket.round != state_.currentRound) {
    return Status::RoundMismatch;
  }
  MemberState* sender = member(contributionPacket.sourceUid);
  if (!sender) return Status::UnknownParticipant;
  const ReplayDecision decision =
      replayDecision(sender->replay, contributionPacket);
  if (decision != ReplayDecision::Fresh) return replayStatus(decision);
  const uint8_t mask = static_cast<uint8_t>(1U << (state_.currentRound - 1U));
  if ((sender->submittedMask & mask) != 0U) return Status::AlreadySubmitted;
  Contribution contribution{};
  contribution.signalRssi = contributionPacket.value;
  contribution.value =
      static_cast<uint16_t>(contributionPacket.dataA & 0xffffU);
  const RoundPrompt prompt = promptFor(static_cast<Mode>(state_.mode),
                                       state_.seed, state_.currentRound);
  if (!validContribution(static_cast<Mode>(state_.mode), contribution,
                         prompt)) {
    return Status::InvalidContribution;
  }
  const uint8_t index = static_cast<uint8_t>(state_.currentRound - 1U);
  sender->signals[index] = contribution.signalRssi;
  sender->values[index] = contribution.value;
  sender->submittedMask |= mask;
  rememberPacket(sender->replay, contributionPacket);
  return Status::Ok;
}

bool HostSession::allCurrentContributions() const {
  if (state_.currentRound == 0U) return false;
  const uint8_t mask = static_cast<uint8_t>(1U << (state_.currentRound - 1U));
  for (uint8_t index = 0U; index < state_.participantCount; ++index) {
    const MemberState& current = state_.members[index];
    if (current.active != 0U && current.joinedRound <= state_.currentRound &&
        (current.submittedMask & mask) == 0U) {
      return false;
    }
  }
  return true;
}

Status HostSession::resolveResult(ModeResult& output) const {
  if (state_.participantCount < kMinimumParticipants ||
      state_.currentRound != state_.totalRounds) {
    return Status::InvalidState;
  }
  ModeResult result{};
  result.participantCount = state_.participantCount;
  result.completedRounds = state_.totalRounds;
  const Mode mode = static_cast<Mode>(state_.mode);

  if (mode == Mode::Triangulation) {
    uint32_t total = 0U;
    uint16_t count = 0U;
    for (uint8_t memberIndex = 0U; memberIndex < state_.participantCount;
         ++memberIndex) {
      const MemberState& current = state_.members[memberIndex];
      if ((current.submittedMask & 0x07U) != 0x07U) continue;
      total += social::triangulationScore(current.signals[0],
                                          current.signals[1],
                                          current.signals[2]);
      ++count;
    }
    result.score = meanScore(total, count);
  } else if (mode == Mode::HotCold) {
    uint32_t total = 0U;
    uint16_t count = 0U;
    int32_t firstTotal = 0;
    int32_t lastTotal = 0;
    for (uint8_t memberIndex = 0U; memberIndex < state_.participantCount;
         ++memberIndex) {
      const MemberState& current = state_.members[memberIndex];
      if ((current.submittedMask & 0x07U) != 0x07U) continue;
      const social::TemperatureHint hint = social::temperatureHint(
          current.signals[0], current.signals[2]);
      static const uint16_t kHintScores[6] = {0U, 0U, 250U, 500U, 800U,
                                               1000U};
      total += kHintScores[static_cast<uint8_t>(hint)];
      firstTotal += current.signals[0];
      lastTotal += current.signals[2];
      ++count;
    }
    result.score = meanScore(total, count);
    if (count != 0U) {
      result.temperature = static_cast<uint8_t>(social::temperatureHint(
          static_cast<int16_t>(firstTotal / count),
          static_cast<int16_t>(lastTotal / count)));
    }
  } else if (mode == Mode::HideAndSeek) {
    const uint8_t hidden = promptFor(mode, state_.seed, 1U).hideSlot;
    uint32_t total = 0U;
    for (uint8_t index = 0U; index < state_.participantCount; ++index) {
      total += social::hideAndSeekScore(
          hidden, static_cast<uint8_t>(state_.members[index].values[0]),
          kHideSlotCount);
    }
    result.score = meanScore(total, state_.participantCount);
    result.outcomeValue = hidden;
  } else if (mode == Mode::SyncRhythm) {
    uint32_t taps[kMaximumParticipants]{};
    for (uint8_t index = 0U; index < state_.participantCount; ++index) {
      taps[index] = state_.members[index].values[0];
    }
    result.score = social::synchronizedTapScore(
        taps, state_.participantCount, 80U, 600U);
  } else if (mode == Mode::CoopEcho) {
    const RoundPrompt prompt = promptFor(mode, state_.seed, 1U);
    uint16_t patterns[kMaximumParticipants]{};
    for (uint8_t index = 0U; index < state_.participantCount; ++index) {
      patterns[index] = state_.members[index].values[0];
    }
    result.score = social::cooperativeEchoScore(
        prompt.echoPattern, patterns, state_.participantCount,
        prompt.echoBeats);
    result.outcomeValue = prompt.echoPattern;
  } else if (mode == Mode::AsyncRelay) {
    uint32_t total = 0U;
    uint16_t contributions = 0U;
    for (uint8_t memberIndex = 0U; memberIndex < state_.participantCount;
         ++memberIndex) {
      const MemberState& current = state_.members[memberIndex];
      for (uint8_t round = current.joinedRound == 0U
                                ? 0U
                                : static_cast<uint8_t>(current.joinedRound - 1U);
           round < state_.totalRounds; ++round) {
        const uint8_t mask = static_cast<uint8_t>(1U << round);
        if ((current.submittedMask & mask) == 0U) continue;
        total += current.values[round];
        ++contributions;
      }
    }
    result.score = meanScore(total, contributions);
    result.outcomeValue = static_cast<uint16_t>(
        total > UINT16_MAX ? UINT16_MAX : total);
  } else if (mode == Mode::RareEncounter) {
    uint32_t total = 0U;
    for (uint8_t index = 0U; index < state_.participantCount; ++index) {
      total += state_.members[index].values[0];
    }
    result.score = meanScore(total, state_.participantCount);
  } else if (mode == Mode::SharedTrail) {
    uint8_t merged = 0U;
    for (uint8_t index = 0U; index < state_.participantCount; ++index) {
      merged = social::sharedTrailMisses(
          merged, static_cast<uint8_t>(state_.members[index].values[0]));
    }
    result.outcomeValue = merged;
    result.score = static_cast<uint16_t>(merged * 50U);
  } else if (mode == Mode::StoryVote) {
    uint8_t votes[kStoryChoiceCount]{};
    for (uint8_t index = 0U; index < state_.participantCount; ++index) {
      ++votes[state_.members[index].values[0]];
    }
    result.storyChoice = social::storyVoteWinner(votes,
                                                  state_.seed ^
                                                      state_.sessionNonce);
    result.outcomeValue = result.storyChoice;
    result.score = static_cast<uint16_t>(
        static_cast<uint32_t>(votes[result.storyChoice]) * 1000U /
        state_.participantCount);
  } else {
    return Status::UnsupportedMode;
  }

  uint16_t expectedContributions = 0U;
  uint16_t actualContributions = 0U;
  for (uint8_t memberIndex = 0U; memberIndex < state_.participantCount;
       ++memberIndex) {
    const MemberState& current = state_.members[memberIndex];
    const uint8_t firstRound = current.joinedRound == 0U
        ? 1U : current.joinedRound;
    for (uint8_t round = firstRound; round <= state_.totalRounds; ++round) {
      ++expectedContributions;
      if ((current.submittedMask &
           static_cast<uint8_t>(1U << (round - 1U))) != 0U) {
        ++actualContributions;
      }
    }
  }
  if (expectedContributions != 0U &&
      actualContributions < expectedContributions) {
    result.score = static_cast<uint16_t>(
        static_cast<uint32_t>(result.score) * actualContributions /
        expectedContributions);
  }

  result.rareEncounter =
      state_.participantCount >= kMinimumParticipants && result.score >= 800U
          ? 1U
          : 0U;
  uint32_t proof = UINT32_C(2166136261);
  proof = fnv32(proof, UINT32_C(0x4D385231));  // "M8R1"
  proof = fnvByte(proof, state_.mode);
  proof = fnv32(proof, state_.sessionNonce);
  proof = fnv32(proof, state_.seed);
  proof = fnvByte(proof, state_.participantCount);
  // Canonicalize the bounded member transcript by UID.
  uint8_t order[kMaximumParticipants]{};
  for (uint8_t index = 0U; index < state_.participantCount; ++index) {
    order[index] = index;
  }
  for (uint8_t index = 1U; index < state_.participantCount; ++index) {
    const uint8_t selected = order[index];
    uint8_t position = index;
    while (position > 0U &&
           state_.members[order[position - 1U]].uid >
               state_.members[selected].uid) {
      order[position] = order[position - 1U];
      --position;
    }
    order[position] = selected;
  }
  for (uint8_t sorted = 0U; sorted < state_.participantCount; ++sorted) {
    const MemberState& current = state_.members[order[sorted]];
    proof = fnv16(proof, current.uid);
    proof = fnvByte(proof, current.joinedRound);
    proof = fnvByte(proof, current.submittedMask);
    for (uint8_t round = 0U; round < state_.totalRounds; ++round) {
      proof = fnv16(proof, static_cast<uint16_t>(current.signals[round]));
      proof = fnv16(proof, current.values[round]);
    }
  }
  proof = fnv16(proof, result.score);
  proof = fnv16(proof, result.outcomeValue);
  proof = fnvByte(proof, result.storyChoice);
  proof = fnvByte(proof, result.temperature);
  proof = fnvByte(proof, result.rareEncounter);
  result.proof = nonZero(proof);
  output = result;
  return Status::Ok;
}

Status HostSession::advance(uint32_t nowMs, Packet& output) {
  if (!available_) return Status::StateUnavailable;
  if (static_cast<HostPhase>(state_.phase) != HostPhase::Active) {
    return Status::WrongPhase;
  }
  if (!allCurrentContributions() &&
      !deadlineReached(nowMs, state_.deadlineMs)) {
    return Status::NotReady;
  }
  if (state_.currentRound < state_.totalRounds) {
    ++state_.currentRound;
    state_.deadlineMs = deadline(nowMs, state_.roundWindowSeconds);
    const RoundPrompt prompt = promptFor(static_cast<Mode>(state_.mode),
                                         state_.seed, state_.currentRound);
    const Mode mode = static_cast<Mode>(state_.mode);
    const uint32_t publicSeed = publicSeedFor(mode, state_.seed);
    return makeHostPacket(PacketType::RoundOpen, state_.currentRound,
                          kFlagActive, 0, publicSeed,
                          wirePromptFor(mode, publicSeed, prompt.round),
                          state_.roundWindowSeconds, output);
  }
  ModeResult result{};
  const Status resultStatus = resolveResult(result);
  if (resultStatus != Status::Ok) return resultStatus;
  uint8_t flags = kFlagActive;
  if (result.rareEncounter != 0U) flags |= kFlagRareEncounter;
  const Status packetStatus = makeHostPacket(
      PacketType::Result, state_.currentRound, flags,
      static_cast<int16_t>(result.score), packedResult(result), result.proof,
      0U, output);
  if (packetStatus != Status::Ok) return packetStatus;
  state_.result = result;
  state_.phase = static_cast<uint8_t>(HostPhase::Complete);
  state_.deadlineMs = 0U;
  return Status::Ok;
}

Status HostSession::cancel(Packet& output) {
  if (!available_) return Status::StateUnavailable;
  const HostPhase phase = static_cast<HostPhase>(state_.phase);
  if (phase != HostPhase::Lobby && phase != HostPhase::Active) {
    return Status::WrongPhase;
  }
  const Status status = makeHostPacket(PacketType::Cancel, 0U, 0U, 0,
                                       0U, 0U, 0U, output);
  if (status == Status::Ok) {
    state_.phase = static_cast<uint8_t>(HostPhase::Cancelled);
    state_.deadlineMs = 0U;
  }
  return status;
}

Status HostSession::restore(const HostState& state) {
  if (!validateHostState(state)) {
    state_ = HostState{};
    state_.phase = static_cast<uint8_t>(HostPhase::Unavailable);
    available_ = false;
    return Status::InvalidState;
  }
  state_ = state;
  available_ = true;
  return Status::Ok;
}

Status ParticipantSession::observeBeacon(uint16_t localUid,
                                         const Packet& beacon,
                                         uint32_t nowMs) {
  if (!available_) return Status::StateUnavailable;
  if (localUid == 0U || localUid == beacon.hostUid) {
    return Status::InvalidArgument;
  }
  if (validate(beacon) != Status::Ok || beacon.type != PacketType::Beacon) {
    return Status::InvalidPacket;
  }
  if ((beacon.flags & kFlagJoinOpen) == 0U) return Status::JoinClosed;
  const ParticipantPhase phase = static_cast<ParticipantPhase>(state_.phase);
  if (phase != ParticipantPhase::Idle && phase != ParticipantPhase::Observed) {
    return Status::WrongPhase;
  }
  if (phase == ParticipantPhase::Observed &&
      state_.hostUid == beacon.hostUid &&
      state_.sessionNonce == beacon.sessionNonce) {
    const ReplayDecision decision = replayDecision(state_.hostReplay, beacon);
    if (decision != ReplayDecision::Fresh) return replayStatus(decision);
  } else {
    state_ = ParticipantState{};
    state_.phase = static_cast<uint8_t>(ParticipantPhase::Observed);
    state_.localUid = localUid;
    state_.hostUid = beacon.hostUid;
    state_.sessionNonce = beacon.sessionNonce;
    state_.mode = static_cast<uint8_t>(beacon.mode);
    state_.totalRounds = roundsForMode(beacon.mode);
    state_.seed = beacon.dataA;
  }
  state_.participantCount = beacon.participantCount;
  state_.readyCount = static_cast<uint8_t>(beacon.value);
  state_.currentRound = beacon.round;
  state_.deadlineMs = deadline(nowMs, beacon.windowSeconds);
  rememberPacket(state_.hostReplay, beacon);
  return Status::Ok;
}

Status ParticipantSession::makeParticipantPacket(PacketType type,
                                                 uint8_t round,
                                                 int16_t value,
                                                 uint32_t dataA,
                                                 Packet& output) {
  if (!available_) return Status::StateUnavailable;
  if (state_.txSequence == UINT16_MAX) return Status::SequenceExhausted;
  Packet candidate{};
  candidate.type = type;
  candidate.mode = static_cast<Mode>(state_.mode);
  candidate.round = round;
  candidate.flags = round == 0U ? 0U : kFlagActive;
  candidate.participantCount = state_.participantCount;
  candidate.sourceUid = state_.localUid;
  candidate.hostUid = state_.hostUid;
  candidate.sessionNonce = state_.sessionNonce;
  candidate.sequence = static_cast<uint16_t>(state_.txSequence + 1U);
  candidate.value = value;
  candidate.dataA = dataA;
  const Status status = validate(candidate);
  if (status != Status::Ok) return status;
  state_.txSequence = candidate.sequence;
  output = candidate;
  return Status::Ok;
}

Status ParticipantSession::makeJoinRequest(Packet& output) {
  if (!available_) return Status::StateUnavailable;
  if (static_cast<ParticipantPhase>(state_.phase) !=
      ParticipantPhase::Observed) {
    return Status::WrongPhase;
  }
  const Status status = makeParticipantPacket(
      PacketType::JoinRequest, state_.currentRound, 0, 0U, output);
  if (status == Status::Ok) {
    state_.phase = static_cast<uint8_t>(ParticipantPhase::Joining);
  }
  return status;
}

Status ParticipantSession::acceptHostPacket(const Packet& packet,
                                            uint32_t nowMs) {
  if (!available_) return Status::StateUnavailable;
  const Status valid = validate(packet);
  if (valid != Status::Ok) return Status::InvalidPacket;
  if (packet.sourceUid != packet.hostUid || packet.hostUid != state_.hostUid) {
    return Status::WrongHost;
  }
  if (packet.sessionNonce != state_.sessionNonce ||
      packet.mode != static_cast<Mode>(state_.mode)) {
    return Status::WrongSession;
  }
  const ReplayDecision decision = replayDecision(state_.hostReplay, packet);
  if (decision != ReplayDecision::Fresh) return replayStatus(decision);

  const ParticipantPhase phase =
      static_cast<ParticipantPhase>(state_.phase);
  if (packet.type == PacketType::Beacon) {
    if ((phase != ParticipantPhase::Lobby &&
         phase != ParticipantPhase::Active) ||
        packet.round != state_.currentRound || packet.dataA != state_.seed) {
      return Status::WrongPhase;
    }
    state_.participantCount = packet.participantCount;
    state_.readyCount = static_cast<uint8_t>(packet.value);
    state_.deadlineMs = deadline(nowMs, packet.windowSeconds);
  } else if (packet.type == PacketType::Welcome) {
    if (phase != ParticipantPhase::Joining ||
        static_cast<uint16_t>(packet.dataB) != state_.localUid) {
      return Status::WrongPhase;
    }
    state_.participantCount = packet.participantCount;
    state_.seed = packet.dataA;
    state_.currentRound = packet.round;
    state_.ready = (packet.flags & kFlagReady) != 0U ? 1U : 0U;
    if (state_.ready != 0U && state_.readyCount < packet.participantCount) {
      ++state_.readyCount;
    }
    state_.phase = static_cast<uint8_t>(
        (packet.flags & kFlagActive) != 0U ? ParticipantPhase::Active
                                           : ParticipantPhase::Lobby);
    state_.deadlineMs = deadline(nowMs, packet.windowSeconds);
  } else if (packet.type == PacketType::RoundOpen) {
    if (phase != ParticipantPhase::Lobby &&
        phase != ParticipantPhase::Active) {
      return Status::WrongPhase;
    }
    if (packet.round == 0U || packet.round > state_.totalRounds ||
        (phase == ParticipantPhase::Active &&
         packet.round != static_cast<uint8_t>(state_.currentRound + 1U))) {
      return Status::RoundMismatch;
    }
    state_.participantCount = packet.participantCount;
    state_.readyCount = packet.participantCount;
    state_.currentRound = packet.round;
    state_.phase = static_cast<uint8_t>(ParticipantPhase::Active);
    state_.deadlineMs = deadline(nowMs, packet.windowSeconds);
  } else if (packet.type == PacketType::Result) {
    if (phase != ParticipantPhase::Active ||
        packet.round != state_.totalRounds) {
      return Status::WrongPhase;
    }
    ModeResult result{};
    result.participantCount = packet.participantCount;
    result.completedRounds = packet.round;
    result.rareEncounter =
        (packet.flags & kFlagRareEncounter) != 0U ? 1U : 0U;
    result.score = static_cast<uint16_t>(packet.value);
    result.outcomeValue = static_cast<uint16_t>(packet.dataA & 0xffffU);
    result.temperature = static_cast<uint8_t>((packet.dataA >> 16U) & 0xffU);
    result.storyChoice = static_cast<uint8_t>((packet.dataA >> 24U) & 0xffU);
    result.proof = packet.dataB;
    if (!resultValid(result, true)) return Status::InvalidPacket;
    state_.result = result;
    state_.phase = static_cast<uint8_t>(ParticipantPhase::Complete);
    state_.deadlineMs = 0U;
  } else if (packet.type == PacketType::Cancel) {
    if (phase != ParticipantPhase::Observed &&
        phase != ParticipantPhase::Joining &&
        phase != ParticipantPhase::Lobby &&
        phase != ParticipantPhase::Active) {
      return Status::WrongPhase;
    }
    state_.phase = static_cast<uint8_t>(ParticipantPhase::Cancelled);
    state_.deadlineMs = 0U;
  } else {
    return Status::InvalidPacket;
  }
  rememberPacket(state_.hostReplay, packet);
  return Status::Ok;
}

Status ParticipantSession::makeReady(bool ready, Packet& output) {
  if (!available_) return Status::StateUnavailable;
  if (static_cast<ParticipantPhase>(state_.phase) != ParticipantPhase::Lobby) {
    return Status::WrongPhase;
  }
  const Status status = makeParticipantPacket(PacketType::Ready, 0U,
                                              ready ? 1 : 0, 0U, output);
  if (status == Status::Ok) state_.ready = ready ? 1U : 0U;
  return status;
}

Status ParticipantSession::makeContribution(
    const Contribution& contribution, Packet& output) {
  if (!available_) return Status::StateUnavailable;
  if (static_cast<ParticipantPhase>(state_.phase) !=
      ParticipantPhase::Active) {
    return Status::WrongPhase;
  }
  const uint8_t mask = static_cast<uint8_t>(1U << (state_.currentRound - 1U));
  if ((state_.submittedMask & mask) != 0U) return Status::AlreadySubmitted;
  const RoundPrompt currentPrompt = prompt();
  if (!validContribution(static_cast<Mode>(state_.mode), contribution,
                         currentPrompt)) {
    return Status::InvalidContribution;
  }
  const Status status = makeParticipantPacket(
      PacketType::Contribution, state_.currentRound,
      contribution.signalRssi, contribution.value, output);
  if (status != Status::Ok) return status;
  state_.submittedMask |= mask;
  const Mode mode = static_cast<Mode>(state_.mode);
  if (mode == Mode::HotCold) {
    if (state_.hasPriorSignal != 0U) {
      state_.temperature = static_cast<uint8_t>(social::temperatureHint(
          state_.priorSignal, contribution.signalRssi));
    }
    state_.priorSignal = contribution.signalRssi;
    state_.hasPriorSignal = 1U;
  }
  return Status::Ok;
}

Status ParticipantSession::expireIfNeeded(uint32_t nowMs) {
  if (!available_) return Status::StateUnavailable;
  const ParticipantPhase phase =
      static_cast<ParticipantPhase>(state_.phase);
  if ((phase == ParticipantPhase::Observed ||
       phase == ParticipantPhase::Joining ||
       phase == ParticipantPhase::Lobby || phase == ParticipantPhase::Active) &&
      deadlineReached(nowMs, state_.deadlineMs)) {
    state_.phase = static_cast<uint8_t>(ParticipantPhase::Expired);
    state_.deadlineMs = 0U;
    return Status::TimedOut;
  }
  return Status::Ok;
}

void ParticipantSession::reset() {
  state_ = ParticipantState{};
  available_ = true;
}

social::TemperatureHint ParticipantSession::temperatureHint() const {
  return static_cast<social::TemperatureHint>(state_.temperature);
}

RoundPrompt ParticipantSession::prompt() const {
  const Mode mode = static_cast<Mode>(state_.mode);
  if (mode == Mode::HideAndSeek) {
    RoundPrompt hidden{};
    hidden.round = state_.currentRound;
    return hidden;
  }
  return promptFor(mode, state_.seed, state_.currentRound);
}

Status ParticipantSession::restore(const ParticipantState& state) {
  if (!validateParticipantState(state)) {
    state_ = ParticipantState{};
    state_.phase = static_cast<uint8_t>(ParticipantPhase::Unavailable);
    available_ = false;
    return Status::InvalidState;
  }
  state_ = state;
  available_ = true;
  return Status::Ok;
}

}  // namespace party_modes
}  // namespace kitsu868
