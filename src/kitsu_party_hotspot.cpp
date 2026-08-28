#include "kitsu_party_hotspot.h"

#include <limits.h>

namespace kitsu868 {
namespace party {
namespace {

constexpr size_t kIntegrityOffset = 28U;
constexpr uint8_t kAllRoundsMask =
    static_cast<uint8_t>((1U << kHuntRounds) - 1U);

void put16(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
}

void put32(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
  output[2] = static_cast<uint8_t>(value >> 16U);
  output[3] = static_cast<uint8_t>(value >> 24U);
}

uint16_t get16(const uint8_t* input) {
  return static_cast<uint16_t>(input[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8U);
}

uint32_t get32(const uint8_t* input) {
  return static_cast<uint32_t>(input[0]) |
         (static_cast<uint32_t>(input[1]) << 8U) |
         (static_cast<uint32_t>(input[2]) << 16U) |
         (static_cast<uint32_t>(input[3]) << 24U);
}

bool supportedPacketType(PacketType type) {
  return type == PacketType::Beacon || type == PacketType::JoinRequest ||
         type == PacketType::Welcome || type == PacketType::RoundOpen ||
         type == PacketType::RoundChoice || type == PacketType::Result ||
         type == PacketType::Cancel;
}

bool validParticipantCount(uint8_t count, uint8_t minimum) {
  return count >= minimum && count <= kMaximumParticipants;
}

bool validWindow(uint16_t seconds) {
  return seconds != 0U && seconds <= kMaximumWindowSeconds;
}

bool hostOwned(PacketType type) {
  return type == PacketType::Beacon || type == PacketType::Welcome ||
         type == PacketType::RoundOpen || type == PacketType::Result ||
         type == PacketType::Cancel;
}

bool validCancelReason(uint8_t value) {
  return value >= static_cast<uint8_t>(CancelReason::HostEnded) &&
         value <= static_cast<uint8_t>(CancelReason::SessionExpired);
}

uint16_t maximumSignalScore(uint8_t participantCount) {
  return static_cast<uint16_t>(
      static_cast<uint16_t>(5U * participantCount - 2U) * kHuntRounds);
}

ResultTier tierForScore(uint16_t score) {
  if (score >= 35U) return ResultTier::Resonant;
  if (score >= 20U) return ResultTier::Found;
  if (score >= 8U) return ResultTier::Trace;
  return ResultTier::Faded;
}

bool validPhase(SessionPhase phase) {
  return phase >= SessionPhase::Idle && phase <= SessionPhase::Unavailable;
}

bool runningPhase(SessionPhase phase) {
  return phase == SessionPhase::Round1 || phase == SessionPhase::Round2 ||
         phase == SessionPhase::Round3;
}

SessionPhase phaseForRound(uint8_t round) {
  if (round == 1U) return SessionPhase::Round1;
  if (round == 2U) return SessionPhase::Round2;
  if (round == 3U) return SessionPhase::Round3;
  return SessionPhase::Idle;
}

bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
  return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

uint16_t remainingWindowSeconds(uint32_t nowMs, uint32_t deadlineMs) {
  if (deadlineReached(nowMs, deadlineMs)) return 0U;
  const uint32_t remainingMs = deadlineMs - nowMs;
  const uint32_t rounded = (remainingMs + 999U) / 1000U;
  return static_cast<uint16_t>(
      rounded > kMaximumWindowSeconds ? kMaximumWindowSeconds : rounded);
}

uint32_t windowDeadline(uint32_t nowMs, uint16_t seconds) {
  return nowMs + static_cast<uint32_t>(seconds) * UINT32_C(1000);
}

uint32_t avalanche(uint32_t value) {
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
  hash = fnvByte(hash, static_cast<uint8_t>(value));
  return fnvByte(hash, static_cast<uint8_t>(value >> 8U));
}

uint32_t fnv32(uint32_t hash, uint32_t value) {
  hash = fnv16(hash, static_cast<uint16_t>(value));
  return fnv16(hash, static_cast<uint16_t>(value >> 16U));
}

uint8_t targetChoice(uint32_t sessionNonce, uint32_t seed, uint8_t round) {
  const uint32_t mixed = avalanche(
      seed ^ sessionNonce ^
      (UINT32_C(0x9E3779B9) * static_cast<uint32_t>(round)) ^
      UINT32_C(0x53494748));  // "SIGH"
  return static_cast<uint8_t>((mixed % 3U) + 1U);
}

uint32_t nonZeroProof(uint32_t value) {
  return value == 0U ? UINT32_C(0x50525459) : value;  // "PRTY"
}

enum class ReplayDecision : uint8_t {
  Fresh = 0U,
  Duplicate,
  Stale,
  Conflict,
};

ReplayDecision replayDecision(const ReplayCursor& cursor,
                              const Packet& packet) {
  if (cursor.lastSequence == 0U) return ReplayDecision::Fresh;
  if (packet.sequence < cursor.lastSequence) return ReplayDecision::Stale;
  if (packet.sequence > cursor.lastSequence) return ReplayDecision::Fresh;
  return packetFingerprint(packet) == cursor.lastFingerprint
             ? ReplayDecision::Duplicate
             : ReplayDecision::Conflict;
}

SessionStatus replayStatus(ReplayDecision decision) {
  if (decision == ReplayDecision::Duplicate) return SessionStatus::Duplicate;
  if (decision == ReplayDecision::Stale)
    return SessionStatus::StaleSequence;
  if (decision == ReplayDecision::Conflict)
    return SessionStatus::SequenceConflict;
  return SessionStatus::Ok;
}

void rememberPacket(ReplayCursor& cursor, const Packet& packet) {
  cursor.lastSequence = packet.sequence;
  cursor.reserved = 0U;
  cursor.lastFingerprint = packetFingerprint(packet);
}

bool replayCursorValid(const ReplayCursor& cursor) {
  if (cursor.reserved != 0U) return false;
  return (cursor.lastSequence == 0U) == (cursor.lastFingerprint == 0U);
}

bool huntParticipantValid(const HuntParticipant& participant) {
  if (participant.uid == 0U ||
      (participant.submittedMask & ~kAllRoundsMask) != 0U) {
    return false;
  }
  for (uint8_t index = 0U; index < kHuntRounds; ++index) {
    const bool submitted =
        (participant.submittedMask & static_cast<uint8_t>(1U << index)) != 0U;
    const SignalChoice choice =
        static_cast<SignalChoice>(participant.choices[index]);
    if (submitted != supportedChoice(choice)) return false;
  }
  return true;
}

bool huntResultValid(const HuntResult& result, bool complete) {
  if (!complete) {
    if (result.participantCount != 0U || result.completedRounds != 0U ||
        result.tier != static_cast<uint8_t>(ResultTier::Faded) ||
        result.score != 0U || result.maximumScore != 0U ||
        result.proof != 0U) {
      return false;
    }
    for (uint8_t index = 0U; index < kHuntRounds; ++index) {
      if (result.targetChoices[index] != 0U) return false;
    }
    return true;
  }
  if (!validParticipantCount(result.participantCount,
                             kMinimumParticipants) ||
      result.completedRounds != kHuntRounds ||
      !supportedTier(static_cast<ResultTier>(result.tier)) ||
      result.maximumScore == 0U || result.score > result.maximumScore ||
      result.proof == 0U) {
    return false;
  }
  for (uint8_t index = 0U; index < kHuntRounds; ++index) {
    if (!supportedChoice(
            static_cast<SignalChoice>(result.targetChoices[index]))) {
      return false;
    }
  }
  return true;
}

template <typename T>
T saturatingAdd(T left, T right) {
  const T maximum = static_cast<T>(~static_cast<T>(0U));
  return right > static_cast<T>(maximum - left) ? maximum
                                                : static_cast<T>(left + right);
}

}  // namespace

bool supportedChoice(SignalChoice choice) {
  return choice == SignalChoice::Sweep || choice == SignalChoice::Listen ||
         choice == SignalChoice::Pulse;
}

bool supportedTier(ResultTier tier) {
  return tier == ResultTier::Faded || tier == ResultTier::Trace ||
         tier == ResultTier::Found || tier == ResultTier::Resonant;
}

Status validate(const Packet& packet) {
  if (!supportedPacketType(packet.type)) return Status::UnsupportedType;
  if (packet.sourceUid == 0U) return Status::InvalidSourceUid;
  if (packet.hostUid == 0U) return Status::InvalidHostUid;
  if (packet.sessionNonce == 0U) return Status::InvalidSessionNonce;
  if (packet.sequence == 0U) return Status::InvalidSequence;
  if (hostOwned(packet.type) != (packet.sourceUid == packet.hostUid)) {
    return packet.sourceUid == packet.hostUid ? Status::InvalidSourceUid
                                               : Status::InvalidHostUid;
  }

  switch (packet.type) {
    case PacketType::Beacon:
      if (packet.round != 0U) return Status::InvalidRound;
      if (packet.value != 0U) return Status::InvalidValue;
      if (!validParticipantCount(packet.participantCount, 1U))
        return Status::InvalidParticipantCount;
      if ((packet.flags & ~kFlagJoinOpen) != 0U)
        return Status::InvalidFlags;
      if (packet.participantCount == kMaximumParticipants &&
          (packet.flags & kFlagJoinOpen) != 0U) {
        return Status::InvalidFlags;
      }
      if (packet.dataA == 0U) return Status::InvalidSeed;
      if (packet.dataB != 0U) return Status::UnexpectedField;
      if (!validWindow(packet.windowSeconds)) return Status::InvalidWindow;
      return Status::Ok;

    case PacketType::JoinRequest:
      if (packet.round != 0U) return Status::InvalidRound;
      if (packet.value != 0U) return Status::InvalidValue;
      if (packet.participantCount != 0U)
        return Status::InvalidParticipantCount;
      if (packet.flags != 0U) return Status::InvalidFlags;
      if (packet.dataA != 0U || packet.dataB != 0U ||
          packet.windowSeconds != 0U) {
        return Status::UnexpectedField;
      }
      return Status::Ok;

    case PacketType::Welcome:
      if (packet.round != 0U) return Status::InvalidRound;
      if (!validParticipantCount(packet.participantCount,
                                 kMinimumParticipants)) {
        return Status::InvalidParticipantCount;
      }
      if (packet.value == 0U || packet.value >= packet.participantCount)
        return Status::InvalidValue;
      if (packet.flags != 0U) return Status::InvalidFlags;
      if (packet.dataA == 0U || packet.dataA > UINT16_MAX ||
          static_cast<uint16_t>(packet.dataA) == packet.hostUid) {
        return Status::InvalidTargetUid;
      }
      if (packet.dataB == 0U) return Status::InvalidSeed;
      if (!validWindow(packet.windowSeconds)) return Status::InvalidWindow;
      return Status::Ok;

    case PacketType::RoundOpen:
      if (packet.round == 0U || packet.round > kHuntRounds)
        return Status::InvalidRound;
      if (packet.value != 0U) return Status::InvalidValue;
      if (!validParticipantCount(packet.participantCount,
                                 kMinimumParticipants)) {
        return Status::InvalidParticipantCount;
      }
      if (packet.flags != 0U) return Status::InvalidFlags;
      if (packet.dataA == 0U) return Status::InvalidSeed;
      if (packet.dataB != 0U) return Status::UnexpectedField;
      if (!validWindow(packet.windowSeconds)) return Status::InvalidWindow;
      return Status::Ok;

    case PacketType::RoundChoice:
      if (packet.round == 0U || packet.round > kHuntRounds)
        return Status::InvalidRound;
      if (!supportedChoice(static_cast<SignalChoice>(packet.value)))
        return Status::InvalidValue;
      if (packet.participantCount != 0U)
        return Status::InvalidParticipantCount;
      if (packet.flags != 0U) return Status::InvalidFlags;
      if (packet.dataA != 0U || packet.dataB != 0U ||
          packet.windowSeconds != 0U) {
        return Status::UnexpectedField;
      }
      return Status::Ok;

    case PacketType::Result: {
      if (packet.round != kHuntRounds) return Status::InvalidRound;
      if (!supportedTier(static_cast<ResultTier>(packet.value)))
        return Status::InvalidValue;
      if (!validParticipantCount(packet.participantCount,
                                 kMinimumParticipants)) {
        return Status::InvalidParticipantCount;
      }
      if (packet.flags != 0U) return Status::InvalidFlags;
      const uint16_t score = static_cast<uint16_t>(packet.dataA);
      const uint16_t maximum = static_cast<uint16_t>(packet.dataA >> 16U);
      if (maximum != maximumSignalScore(packet.participantCount) ||
          score > maximum || packet.dataB == 0U ||
          packet.value != static_cast<uint8_t>(tierForScore(score))) {
        return Status::InvalidScore;
      }
      if (packet.windowSeconds != 0U) return Status::UnexpectedField;
      return Status::Ok;
    }

    case PacketType::Cancel:
      if (packet.round != 0U) return Status::InvalidRound;
      if (!validCancelReason(packet.value)) return Status::InvalidValue;
      if (!validParticipantCount(packet.participantCount, 1U))
        return Status::InvalidParticipantCount;
      if (packet.flags != 0U) return Status::InvalidFlags;
      if (packet.dataA != 0U || packet.dataB != 0U ||
          packet.windowSeconds != 0U) {
        return Status::UnexpectedField;
      }
      return Status::Ok;
  }
  return Status::UnsupportedType;
}

uint16_t crc16CcittFalse(const uint8_t* data, size_t length) {
  if (data == nullptr && length != 0U) return 0U;
  uint16_t crc = UINT16_C(0xFFFF);
  for (size_t index = 0U; index < length; ++index) {
    crc ^= static_cast<uint16_t>(static_cast<uint16_t>(data[index]) << 8U);
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      crc = (crc & UINT16_C(0x8000))
                ? static_cast<uint16_t>((crc << 1U) ^ UINT16_C(0x1021))
                : static_cast<uint16_t>(crc << 1U);
    }
  }
  return crc;
}

Status encode(const Packet& packet, uint8_t* output, size_t capacity,
              size_t* bytesWritten) {
  if (bytesWritten != nullptr) *bytesWritten = 0U;
  if (output == nullptr) return Status::NullArgument;
  if (capacity < kWireBytes) return Status::BufferTooSmall;
  const Status packetStatus = validate(packet);
  if (packetStatus != Status::Ok) return packetStatus;

  output[0] = kMagic0;
  output[1] = kMagic1;
  output[2] = kProtocolVersion;
  output[3] = static_cast<uint8_t>(packet.type);
  put16(output + 4U, packet.sourceUid);
  put16(output + 6U, packet.hostUid);
  put32(output + 8U, packet.sessionNonce);
  put16(output + 12U, packet.sequence);
  output[14] = packet.round;
  output[15] = packet.value;
  output[16] = packet.participantCount;
  output[17] = packet.flags;
  put32(output + 18U, packet.dataA);
  put32(output + 22U, packet.dataB);
  put16(output + 26U, packet.windowSeconds);
  put16(output + kIntegrityOffset,
        crc16CcittFalse(output, kIntegrityOffset));
  if (bytesWritten != nullptr) *bytesWritten = kWireBytes;
  return Status::Ok;
}

Status decode(const uint8_t* wire, size_t length, Packet& output) {
  if (wire == nullptr) return Status::NullArgument;
  if (length != kWireBytes) return Status::WrongLength;
  if (wire[0] != kMagic0 || wire[1] != kMagic1) return Status::BadMagic;
  if (wire[2] != kProtocolVersion) return Status::UnsupportedVersion;
  const PacketType type = static_cast<PacketType>(wire[3]);
  if (!supportedPacketType(type)) return Status::UnsupportedType;
  if (get16(wire + kIntegrityOffset) !=
      crc16CcittFalse(wire, kIntegrityOffset)) {
    return Status::IntegrityMismatch;
  }

  Packet candidate{};
  candidate.type = type;
  candidate.sourceUid = get16(wire + 4U);
  candidate.hostUid = get16(wire + 6U);
  candidate.sessionNonce = get32(wire + 8U);
  candidate.sequence = get16(wire + 12U);
  candidate.round = wire[14];
  candidate.value = wire[15];
  candidate.participantCount = wire[16];
  candidate.flags = wire[17];
  candidate.dataA = get32(wire + 18U);
  candidate.dataB = get32(wire + 22U);
  candidate.windowSeconds = get16(wire + 26U);
  const Status candidateStatus = validate(candidate);
  if (candidateStatus != Status::Ok) return candidateStatus;
  output = candidate;
  return Status::Ok;
}

uint32_t packetFingerprint(const Packet& packet) {
  uint8_t wire[kWireBytes]{};
  if (encode(packet, wire, sizeof(wire)) != Status::Ok) return 0U;
  uint32_t hash = UINT32_C(2166136261);
  for (size_t index = 0U; index < kIntegrityOffset; ++index) {
    hash = fnvByte(hash, wire[index]);
  }
  return nonZeroProof(hash);
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
    case Status::InvalidSourceUid: return "invalid_source_uid";
    case Status::InvalidHostUid: return "invalid_host_uid";
    case Status::InvalidSessionNonce: return "invalid_session_nonce";
    case Status::InvalidSequence: return "invalid_sequence";
    case Status::InvalidRound: return "invalid_round";
    case Status::InvalidValue: return "invalid_value";
    case Status::InvalidParticipantCount:
      return "invalid_participant_count";
    case Status::InvalidFlags: return "invalid_flags";
    case Status::InvalidSeed: return "invalid_seed";
    case Status::InvalidTargetUid: return "invalid_target_uid";
    case Status::InvalidScore: return "invalid_score";
    case Status::InvalidWindow: return "invalid_window";
    case Status::UnexpectedField: return "unexpected_field";
    case Status::IntegrityMismatch: return "integrity_mismatch";
  }
  return "unknown";
}

bool resolveSignalHunt(uint32_t sessionNonce, uint32_t seed,
                       const HuntParticipant* participants,
                       uint8_t participantCount, HuntResult& output) {
  if (sessionNonce == 0U || seed == 0U || participants == nullptr ||
      !validParticipantCount(participantCount, kMinimumParticipants)) {
    return false;
  }

  HuntParticipant canonical[kMaximumParticipants]{};
  for (uint8_t index = 0U; index < participantCount; ++index) {
    if (!huntParticipantValid(participants[index])) return false;
    canonical[index] = participants[index];
    for (uint8_t prior = 0U; prior < index; ++prior) {
      if (canonical[prior].uid == canonical[index].uid) return false;
    }
  }
  for (uint8_t index = 1U; index < participantCount; ++index) {
    HuntParticipant value = canonical[index];
    uint8_t position = index;
    while (position != 0U && canonical[position - 1U].uid > value.uid) {
      canonical[position] = canonical[position - 1U];
      --position;
    }
    canonical[position] = value;
  }

  HuntResult candidate{};
  candidate.participantCount = participantCount;
  candidate.completedRounds = kHuntRounds;
  candidate.maximumScore = maximumSignalScore(participantCount);

  for (uint8_t roundIndex = 0U; roundIndex < kHuntRounds; ++roundIndex) {
    const uint8_t target = targetChoice(sessionNonce, seed,
                                        static_cast<uint8_t>(roundIndex + 1U));
    candidate.targetChoices[roundIndex] = target;
    uint8_t choiceMask = 0U;
    uint8_t submitted = 0U;
    for (uint8_t participantIndex = 0U;
         participantIndex < participantCount; ++participantIndex) {
      const HuntParticipant& participant = canonical[participantIndex];
      if ((participant.submittedMask &
           static_cast<uint8_t>(1U << roundIndex)) == 0U) {
        continue;
      }
      const uint8_t choice = participant.choices[roundIndex];
      ++submitted;
      ++candidate.score;
      if (choice == target) candidate.score += 3U;
      choiceMask |= static_cast<uint8_t>(1U << (choice - 1U));
    }
    if (participantCount >= 3U && choiceMask == 0x07U) {
      candidate.score += 2U;
    }
    if (submitted == participantCount) {
      candidate.score +=
          static_cast<uint16_t>(participantCount - kMinimumParticipants);
    }
  }

  const ResultTier tier = tierForScore(candidate.score);
  candidate.tier = static_cast<uint8_t>(tier);

  uint32_t proof = UINT32_C(2166136261);
  proof = fnv32(proof, UINT32_C(0x50534831));  // "PSH1"
  proof = fnv32(proof, sessionNonce);
  proof = fnv32(proof, seed);
  proof = fnvByte(proof, participantCount);
  for (uint8_t index = 0U; index < participantCount; ++index) {
    proof = fnv16(proof, canonical[index].uid);
    proof = fnvByte(proof, canonical[index].submittedMask);
    for (uint8_t round = 0U; round < kHuntRounds; ++round) {
      proof = fnvByte(proof, canonical[index].choices[round]);
    }
  }
  for (uint8_t round = 0U; round < kHuntRounds; ++round) {
    proof = fnvByte(proof, candidate.targetChoices[round]);
  }
  proof = fnv16(proof, candidate.score);
  proof = fnv16(proof, candidate.maximumScore);
  proof = fnvByte(proof, candidate.tier);
  candidate.proof = nonZeroProof(proof);
  output = candidate;
  return true;
}

bool validateHostState(const HostState& state) {
  if (state.schemaVersion != kHostStateSchemaVersion ||
      !validPhase(static_cast<SessionPhase>(state.phase)) ||
      static_cast<SessionPhase>(state.phase) == SessionPhase::Joining ||
      static_cast<SessionPhase>(state.phase) == SessionPhase::Unavailable) {
    return false;
  }
  const SessionPhase phase = static_cast<SessionPhase>(state.phase);
  if (phase == SessionPhase::Idle) {
    return state.participantCount == 0U && state.currentRound == 0U &&
           state.hostUid == 0U && state.txSequence == 0U &&
           state.sessionNonce == 0U && state.seed == 0U &&
           state.deadlineMs == 0U && state.lobbyWindowSeconds == 0U &&
           state.roundWindowSeconds == 0U &&
           huntResultValid(state.result, false);
  }
  if (state.hostUid == 0U || state.sessionNonce == 0U || state.seed == 0U ||
      !validParticipantCount(state.participantCount, 1U) ||
      !validWindow(state.lobbyWindowSeconds) ||
      !validWindow(state.roundWindowSeconds)) {
    return false;
  }
  if (phase == SessionPhase::Lobby && state.currentRound != 0U) return false;
  if (runningPhase(phase) &&
      (state.currentRound == 0U || state.currentRound > kHuntRounds ||
       phase != phaseForRound(state.currentRound))) {
    return false;
  }

  uint8_t activeCount = 0U;
  for (uint8_t index = 0U; index < kMaximumParticipants; ++index) {
    const HostParticipantState& participant = state.participants[index];
    if (participant.active > 1U || participant.reserved != 0U ||
        participant.slot != index || !replayCursorValid(participant.replay)) {
      return false;
    }
    if (participant.active == 0U) continue;
    ++activeCount;
    if (!huntParticipantValid(participant.hunt)) return false;
    if (index == 0U && (participant.hunt.uid != state.hostUid ||
                       participant.replay.lastSequence != 0U)) {
      return false;
    }
    if (index != 0U && participant.hunt.uid == state.hostUid) return false;
    for (uint8_t prior = 0U; prior < index; ++prior) {
      if (state.participants[prior].active != 0U &&
          state.participants[prior].hunt.uid == participant.hunt.uid) {
        return false;
      }
    }
  }
  if (activeCount != state.participantCount ||
      state.participants[0].active == 0U) {
    return false;
  }
  if (phase == SessionPhase::Complete) {
    return state.currentRound == kHuntRounds && state.deadlineMs == 0U &&
           state.result.participantCount == state.participantCount &&
           huntResultValid(state.result, true);
  }
  if ((phase == SessionPhase::Cancelled || phase == SessionPhase::Expired) &&
      state.deadlineMs != 0U) {
    return false;
  }
  return huntResultValid(state.result, false);
}

SessionStatus HostSession::start(uint16_t hostUid, uint32_t sessionNonce,
                                 uint32_t seed, uint32_t nowMs,
                                 uint16_t lobbyWindowSeconds,
                                 uint16_t roundWindowSeconds) {
  if (hostUid == 0U || sessionNonce == 0U || seed == 0U ||
      !validWindow(lobbyWindowSeconds) ||
      !validWindow(roundWindowSeconds)) {
    return SessionStatus::InvalidArgument;
  }
  state_ = HostState{};
  state_.phase = static_cast<uint8_t>(SessionPhase::Lobby);
  state_.participantCount = 1U;
  state_.hostUid = hostUid;
  state_.sessionNonce = sessionNonce;
  state_.seed = seed;
  state_.deadlineMs = windowDeadline(nowMs, lobbyWindowSeconds);
  state_.lobbyWindowSeconds = lobbyWindowSeconds;
  state_.roundWindowSeconds = roundWindowSeconds;
  state_.participants[0].active = 1U;
  state_.participants[0].slot = 0U;
  state_.participants[0].hunt.uid = hostUid;
  for (uint8_t index = 1U; index < kMaximumParticipants; ++index) {
    state_.participants[index].slot = index;
  }
  available_ = true;
  return SessionStatus::Ok;
}

void HostSession::reset() {
  state_ = HostState{};
  available_ = true;
}

SessionStatus HostSession::makeHostPacket(
    PacketType type, uint8_t round, uint8_t value, uint8_t participantCount,
    uint8_t flags, uint32_t dataA, uint32_t dataB, uint16_t windowSeconds,
    Packet& output) {
  if (!available_) return SessionStatus::StateUnavailable;
  if (state_.txSequence == UINT16_MAX)
    return SessionStatus::SequenceExhausted;
  Packet candidate{};
  candidate.type = type;
  candidate.sourceUid = state_.hostUid;
  candidate.hostUid = state_.hostUid;
  candidate.sessionNonce = state_.sessionNonce;
  candidate.sequence = static_cast<uint16_t>(state_.txSequence + 1U);
  candidate.round = round;
  candidate.value = value;
  candidate.participantCount = participantCount;
  candidate.flags = flags;
  candidate.dataA = dataA;
  candidate.dataB = dataB;
  candidate.windowSeconds = windowSeconds;
  if (validate(candidate) != Status::Ok)
    return SessionStatus::InvalidArgument;
  state_.txSequence = candidate.sequence;
  output = candidate;
  return SessionStatus::Ok;
}

SessionStatus HostSession::expireIfNeeded(uint32_t nowMs) {
  if (!available_) return SessionStatus::StateUnavailable;
  const SessionPhase phase = static_cast<SessionPhase>(state_.phase);
  if (phase == SessionPhase::Lobby &&
      deadlineReached(nowMs, state_.deadlineMs)) {
    state_.phase = static_cast<uint8_t>(SessionPhase::Expired);
    state_.deadlineMs = 0U;
    return SessionStatus::TimedOut;
  }
  if (runningPhase(phase) && deadlineReached(nowMs, state_.deadlineMs)) {
    return SessionStatus::TimedOut;
  }
  return SessionStatus::Ok;
}

SessionStatus HostSession::makeBeacon(uint32_t nowMs, Packet& output) {
  if (!available_) return SessionStatus::StateUnavailable;
  if (static_cast<SessionPhase>(state_.phase) != SessionPhase::Lobby)
    return SessionStatus::WrongPhase;
  if (expireIfNeeded(nowMs) == SessionStatus::TimedOut)
    return SessionStatus::TimedOut;
  const uint16_t remaining = remainingWindowSeconds(nowMs, state_.deadlineMs);
  const uint8_t flags = state_.participantCount < kMaximumParticipants
                            ? kFlagJoinOpen
                            : 0U;
  return makeHostPacket(PacketType::Beacon, 0U, 0U,
                        state_.participantCount, flags, state_.seed, 0U,
                        remaining, output);
}

HostParticipantState* HostSession::findParticipant(uint16_t uid) {
  for (uint8_t index = 0U; index < kMaximumParticipants; ++index) {
    if (state_.participants[index].active != 0U &&
        state_.participants[index].hunt.uid == uid) {
      return &state_.participants[index];
    }
  }
  return nullptr;
}

const HostParticipantState* HostSession::findParticipant(uint16_t uid) const {
  for (uint8_t index = 0U; index < kMaximumParticipants; ++index) {
    if (state_.participants[index].active != 0U &&
        state_.participants[index].hunt.uid == uid) {
      return &state_.participants[index];
    }
  }
  return nullptr;
}

SessionStatus HostSession::acceptJoin(const Packet& request, uint32_t nowMs,
                                      Packet& welcome) {
  if (!available_) return SessionStatus::StateUnavailable;
  if (static_cast<SessionPhase>(state_.phase) != SessionPhase::Lobby)
    return SessionStatus::WrongPhase;
  if (expireIfNeeded(nowMs) == SessionStatus::TimedOut)
    return SessionStatus::TimedOut;
  if (validate(request) != Status::Ok ||
      request.type != PacketType::JoinRequest) {
    return SessionStatus::InvalidPacket;
  }
  if (request.hostUid != state_.hostUid) return SessionStatus::WrongHost;
  if (request.sessionNonce != state_.sessionNonce)
    return SessionStatus::WrongSession;

  HostParticipantState* participant = findParticipant(request.sourceUid);
  if (participant != nullptr) {
    const ReplayDecision decision = replayDecision(participant->replay, request);
    if (decision != ReplayDecision::Fresh) return replayStatus(decision);
  } else {
    if (state_.participantCount >= kMaximumParticipants)
      return SessionStatus::PartyFull;
    if (state_.txSequence == UINT16_MAX)
      return SessionStatus::SequenceExhausted;
    participant = &state_.participants[state_.participantCount];
    *participant = HostParticipantState{};
    participant->active = 1U;
    participant->slot = state_.participantCount;
    participant->hunt.uid = request.sourceUid;
    ++state_.participantCount;
  }
  if (state_.txSequence == UINT16_MAX)
    return SessionStatus::SequenceExhausted;
  rememberPacket(participant->replay, request);

  const uint16_t remaining = remainingWindowSeconds(nowMs, state_.deadlineMs);
  const SessionStatus welcomeStatus = makeHostPacket(
      PacketType::Welcome, 0U, participant->slot, state_.participantCount,
      0U, participant->hunt.uid, state_.seed, remaining, welcome);
  if (welcomeStatus != SessionStatus::Ok) return welcomeStatus;
  return SessionStatus::Ok;
}

SessionStatus HostSession::beginHunt(uint32_t nowMs, Packet& roundOpen) {
  if (!available_) return SessionStatus::StateUnavailable;
  if (static_cast<SessionPhase>(state_.phase) != SessionPhase::Lobby)
    return SessionStatus::WrongPhase;
  if (expireIfNeeded(nowMs) == SessionStatus::TimedOut)
    return SessionStatus::TimedOut;
  if (state_.participantCount < kMinimumParticipants)
    return SessionStatus::NotReady;
  if (state_.txSequence == UINT16_MAX)
    return SessionStatus::SequenceExhausted;
  state_.currentRound = 1U;
  state_.phase = static_cast<uint8_t>(SessionPhase::Round1);
  state_.deadlineMs = windowDeadline(nowMs, state_.roundWindowSeconds);
  return makeHostPacket(PacketType::RoundOpen, 1U, 0U,
                        state_.participantCount, 0U, state_.seed, 0U,
                        state_.roundWindowSeconds, roundOpen);
}

SessionStatus HostSession::submitHostChoice(uint8_t round,
                                            SignalChoice choice) {
  if (!available_) return SessionStatus::StateUnavailable;
  if (!runningPhase(static_cast<SessionPhase>(state_.phase)))
    return SessionStatus::WrongPhase;
  if (round != state_.currentRound) return SessionStatus::RoundMismatch;
  if (!supportedChoice(choice)) return SessionStatus::InvalidArgument;
  HostParticipantState& host = state_.participants[0];
  const uint8_t mask = static_cast<uint8_t>(1U << (round - 1U));
  if ((host.hunt.submittedMask & mask) != 0U)
    return SessionStatus::ChoiceAlreadySubmitted;
  host.hunt.choices[round - 1U] = static_cast<uint8_t>(choice);
  host.hunt.submittedMask |= mask;
  return SessionStatus::Ok;
}

SessionStatus HostSession::acceptChoice(const Packet& choice,
                                        uint32_t nowMs) {
  if (!available_) return SessionStatus::StateUnavailable;
  if (!runningPhase(static_cast<SessionPhase>(state_.phase)))
    return SessionStatus::WrongPhase;
  if (deadlineReached(nowMs, state_.deadlineMs))
    return SessionStatus::TimedOut;
  if (validate(choice) != Status::Ok ||
      choice.type != PacketType::RoundChoice) {
    return SessionStatus::InvalidPacket;
  }
  if (choice.hostUid != state_.hostUid) return SessionStatus::WrongHost;
  if (choice.sessionNonce != state_.sessionNonce)
    return SessionStatus::WrongSession;
  if (choice.round != state_.currentRound)
    return SessionStatus::RoundMismatch;
  HostParticipantState* participant = findParticipant(choice.sourceUid);
  if (participant == nullptr || participant->slot == 0U)
    return SessionStatus::UnknownParticipant;
  const ReplayDecision decision = replayDecision(participant->replay, choice);
  if (decision != ReplayDecision::Fresh) return replayStatus(decision);
  const uint8_t mask = static_cast<uint8_t>(1U << (choice.round - 1U));
  if ((participant->hunt.submittedMask & mask) != 0U)
    return SessionStatus::ChoiceAlreadySubmitted;
  participant->hunt.choices[choice.round - 1U] = choice.value;
  participant->hunt.submittedMask |= mask;
  rememberPacket(participant->replay, choice);
  return SessionStatus::Ok;
}

bool HostSession::allCurrentChoicesSubmitted() const {
  if (state_.currentRound == 0U || state_.currentRound > kHuntRounds)
    return false;
  const uint8_t mask =
      static_cast<uint8_t>(1U << (state_.currentRound - 1U));
  for (uint8_t index = 0U; index < state_.participantCount; ++index) {
    if ((state_.participants[index].hunt.submittedMask & mask) == 0U)
      return false;
  }
  return true;
}

SessionStatus HostSession::advance(uint32_t nowMs, Packet& output) {
  if (!available_) return SessionStatus::StateUnavailable;
  if (!runningPhase(static_cast<SessionPhase>(state_.phase)))
    return SessionStatus::WrongPhase;
  if (!allCurrentChoicesSubmitted() &&
      !deadlineReached(nowMs, state_.deadlineMs)) {
    return SessionStatus::NotReady;
  }
  if (state_.txSequence == UINT16_MAX)
    return SessionStatus::SequenceExhausted;

  if (state_.currentRound < kHuntRounds) {
    ++state_.currentRound;
    state_.phase = static_cast<uint8_t>(phaseForRound(state_.currentRound));
    state_.deadlineMs = windowDeadline(nowMs, state_.roundWindowSeconds);
    return makeHostPacket(PacketType::RoundOpen, state_.currentRound, 0U,
                          state_.participantCount, 0U, state_.seed, 0U,
                          state_.roundWindowSeconds, output);
  }

  HuntParticipant transcript[kMaximumParticipants]{};
  for (uint8_t index = 0U; index < state_.participantCount; ++index) {
    transcript[index] = state_.participants[index].hunt;
  }
  HuntResult result{};
  if (!resolveSignalHunt(state_.sessionNonce, state_.seed, transcript,
                         state_.participantCount, result)) {
    return SessionStatus::InvalidState;
  }
  const uint32_t packedScore =
      static_cast<uint32_t>(result.score) |
      (static_cast<uint32_t>(result.maximumScore) << 16U);
  const SessionStatus packetStatus = makeHostPacket(
      PacketType::Result, kHuntRounds, result.tier,
      state_.participantCount, 0U, packedScore, result.proof, 0U, output);
  if (packetStatus != SessionStatus::Ok) return packetStatus;
  state_.result = result;
  state_.phase = static_cast<uint8_t>(SessionPhase::Complete);
  state_.deadlineMs = 0U;
  return SessionStatus::Ok;
}

SessionStatus HostSession::cancel(CancelReason reason, Packet& output) {
  if (!available_) return SessionStatus::StateUnavailable;
  const SessionPhase phase = static_cast<SessionPhase>(state_.phase);
  if (phase == SessionPhase::Idle || phase == SessionPhase::Complete ||
      phase == SessionPhase::Cancelled || phase == SessionPhase::Expired) {
    return SessionStatus::WrongPhase;
  }
  if (!validCancelReason(static_cast<uint8_t>(reason)))
    return SessionStatus::InvalidArgument;
  const SessionStatus packetStatus = makeHostPacket(
      PacketType::Cancel, 0U, static_cast<uint8_t>(reason),
      state_.participantCount, 0U, 0U, 0U, 0U, output);
  if (packetStatus != SessionStatus::Ok) return packetStatus;
  state_.phase = static_cast<uint8_t>(SessionPhase::Cancelled);
  state_.deadlineMs = 0U;
  return SessionStatus::Ok;
}

const HostState& HostSession::state() const { return state_; }

HostState HostSession::snapshot() const { return state_; }

SessionStatus HostSession::restore(const HostState& state) {
  if (state.schemaVersion != kHostStateSchemaVersion) {
    state_ = HostState{};
    state_.schemaVersion = 0U;
    state_.phase = static_cast<uint8_t>(SessionPhase::Unavailable);
    available_ = false;
    return SessionStatus::UnsupportedState;
  }
  if (!validateHostState(state)) {
    state_ = HostState{};
    state_.schemaVersion = 0U;
    state_.phase = static_cast<uint8_t>(SessionPhase::Unavailable);
    available_ = false;
    return SessionStatus::InvalidState;
  }
  state_ = state;
  available_ = true;
  return SessionStatus::Ok;
}

bool validateParticipantState(const ParticipantState& state) {
  if (state.schemaVersion != kParticipantStateSchemaVersion ||
      !validPhase(static_cast<SessionPhase>(state.phase)) ||
      static_cast<SessionPhase>(state.phase) == SessionPhase::Unavailable ||
      (state.submittedMask & ~kAllRoundsMask) != 0U ||
      !replayCursorValid(state.hostReplay)) {
    return false;
  }
  const SessionPhase phase = static_cast<SessionPhase>(state.phase);
  if (phase == SessionPhase::Idle) {
    return state.participantCount == 0U && state.currentRound == 0U &&
           state.localUid == 0U && state.hostUid == 0U &&
           state.txSequence == 0U && state.reserved == 0U &&
           state.sessionNonce == 0U && state.seed == 0U &&
           state.deadlineMs == 0U && state.submittedMask == 0U &&
           huntResultValid(state.result, false);
  }
  if (state.reserved != 0U || state.localUid == 0U || state.hostUid == 0U ||
      state.localUid == state.hostUid || state.sessionNonce == 0U ||
      state.seed == 0U || !validParticipantCount(state.participantCount, 1U)) {
    return false;
  }
  for (uint8_t index = 0U; index < kHuntRounds; ++index) {
    const bool submitted =
        (state.submittedMask & static_cast<uint8_t>(1U << index)) != 0U;
    if (submitted != supportedChoice(
                         static_cast<SignalChoice>(state.choices[index]))) {
      return false;
    }
  }
  if ((phase == SessionPhase::Joining || phase == SessionPhase::Lobby) &&
      state.currentRound != 0U) {
    return false;
  }
  if (runningPhase(phase) &&
      (state.currentRound == 0U || state.currentRound > kHuntRounds ||
       phase != phaseForRound(state.currentRound))) {
    return false;
  }
  if (phase == SessionPhase::Complete) {
    return state.currentRound == kHuntRounds && state.deadlineMs == 0U &&
           state.result.participantCount == state.participantCount &&
           huntResultValid(state.result, true);
  }
  if ((phase == SessionPhase::Cancelled || phase == SessionPhase::Expired) &&
      state.deadlineMs != 0U) {
    return false;
  }
  return huntResultValid(state.result, false);
}

SessionStatus ParticipantSession::observeBeacon(uint16_t localUid,
                                                const Packet& beacon,
                                                uint32_t nowMs) {
  if (localUid == 0U || validate(beacon) != Status::Ok ||
      beacon.type != PacketType::Beacon || localUid == beacon.hostUid) {
    return SessionStatus::InvalidArgument;
  }
  if ((beacon.flags & kFlagJoinOpen) == 0U ||
      beacon.participantCount >= kMaximumParticipants) {
    return SessionStatus::PartyFull;
  }
  state_ = ParticipantState{};
  state_.phase = static_cast<uint8_t>(SessionPhase::Joining);
  state_.participantCount = beacon.participantCount;
  state_.localUid = localUid;
  state_.hostUid = beacon.hostUid;
  state_.sessionNonce = beacon.sessionNonce;
  state_.seed = beacon.dataA;
  state_.deadlineMs = windowDeadline(nowMs, beacon.windowSeconds);
  rememberPacket(state_.hostReplay, beacon);
  available_ = true;
  return SessionStatus::Ok;
}

SessionStatus ParticipantSession::makeJoinRequest(Packet& output) {
  if (!available_) return SessionStatus::StateUnavailable;
  const SessionPhase phase = static_cast<SessionPhase>(state_.phase);
  if (phase != SessionPhase::Joining && phase != SessionPhase::Lobby)
    return SessionStatus::WrongPhase;
  if (state_.txSequence == UINT16_MAX)
    return SessionStatus::SequenceExhausted;
  Packet candidate{};
  candidate.type = PacketType::JoinRequest;
  candidate.sourceUid = state_.localUid;
  candidate.hostUid = state_.hostUid;
  candidate.sessionNonce = state_.sessionNonce;
  candidate.sequence = static_cast<uint16_t>(state_.txSequence + 1U);
  if (validate(candidate) != Status::Ok)
    return SessionStatus::InvalidState;
  state_.txSequence = candidate.sequence;
  output = candidate;
  return SessionStatus::Ok;
}

SessionStatus ParticipantSession::acceptHostPacket(const Packet& packet,
                                                   uint32_t nowMs) {
  if (!available_) return SessionStatus::StateUnavailable;
  if (validate(packet) != Status::Ok || !hostOwned(packet.type))
    return SessionStatus::InvalidPacket;
  if (packet.hostUid != state_.hostUid || packet.sourceUid != state_.hostUid)
    return SessionStatus::WrongHost;
  if (packet.sessionNonce != state_.sessionNonce)
    return SessionStatus::WrongSession;
  const ReplayDecision decision = replayDecision(state_.hostReplay, packet);
  if (decision != ReplayDecision::Fresh) return replayStatus(decision);

  const SessionPhase phase = static_cast<SessionPhase>(state_.phase);
  switch (packet.type) {
    case PacketType::Beacon:
      if (phase != SessionPhase::Joining && phase != SessionPhase::Lobby)
        return SessionStatus::WrongPhase;
      if (packet.dataA != state_.seed) return SessionStatus::WrongSession;
      state_.participantCount = packet.participantCount;
      state_.deadlineMs = windowDeadline(nowMs, packet.windowSeconds);
      break;

    case PacketType::Welcome:
      if (phase != SessionPhase::Joining && phase != SessionPhase::Lobby)
        return SessionStatus::WrongPhase;
      if (packet.dataA != state_.localUid || packet.dataB != state_.seed)
        return SessionStatus::WrongSession;
      state_.phase = static_cast<uint8_t>(SessionPhase::Lobby);
      state_.participantCount = packet.participantCount;
      state_.deadlineMs = windowDeadline(nowMs, packet.windowSeconds);
      break;

    case PacketType::RoundOpen:
      if (phase != SessionPhase::Joining && phase != SessionPhase::Lobby &&
          !runningPhase(phase)) {
        return SessionStatus::WrongPhase;
      }
      if (packet.dataA != state_.seed ||
          packet.round <= state_.currentRound) {
        return SessionStatus::RoundMismatch;
      }
      state_.currentRound = packet.round;
      state_.phase = static_cast<uint8_t>(phaseForRound(packet.round));
      state_.participantCount = packet.participantCount;
      state_.deadlineMs = windowDeadline(nowMs, packet.windowSeconds);
      break;

    case PacketType::Result: {
      if (!runningPhase(phase)) return SessionStatus::WrongPhase;
      HuntResult result{};
      result.participantCount = packet.participantCount;
      result.completedRounds = kHuntRounds;
      result.tier = packet.value;
      result.score = static_cast<uint16_t>(packet.dataA);
      result.maximumScore = static_cast<uint16_t>(packet.dataA >> 16U);
      result.proof = packet.dataB;
      for (uint8_t round = 0U; round < kHuntRounds; ++round) {
        result.targetChoices[round] = targetChoice(
            state_.sessionNonce, state_.seed, static_cast<uint8_t>(round + 1U));
      }
      if (!huntResultValid(result, true))
        return SessionStatus::InvalidPacket;
      state_.result = result;
      state_.participantCount = packet.participantCount;
      state_.currentRound = kHuntRounds;
      state_.phase = static_cast<uint8_t>(SessionPhase::Complete);
      state_.deadlineMs = 0U;
      break;
    }

    case PacketType::Cancel:
      if (phase == SessionPhase::Idle || phase == SessionPhase::Complete ||
          phase == SessionPhase::Cancelled || phase == SessionPhase::Expired) {
        return SessionStatus::WrongPhase;
      }
      state_.participantCount = packet.participantCount;
      state_.phase = static_cast<uint8_t>(SessionPhase::Cancelled);
      state_.deadlineMs = 0U;
      break;

    case PacketType::JoinRequest:
    case PacketType::RoundChoice:
      return SessionStatus::InvalidPacket;
  }
  rememberPacket(state_.hostReplay, packet);
  return SessionStatus::Ok;
}

SessionStatus ParticipantSession::makeChoice(SignalChoice choice,
                                             Packet& output) {
  if (!available_) return SessionStatus::StateUnavailable;
  if (!runningPhase(static_cast<SessionPhase>(state_.phase)))
    return SessionStatus::WrongPhase;
  if (!supportedChoice(choice)) return SessionStatus::InvalidArgument;
  const uint8_t mask =
      static_cast<uint8_t>(1U << (state_.currentRound - 1U));
  if ((state_.submittedMask & mask) != 0U)
    return SessionStatus::ChoiceAlreadySubmitted;
  if (state_.txSequence == UINT16_MAX)
    return SessionStatus::SequenceExhausted;
  Packet candidate{};
  candidate.type = PacketType::RoundChoice;
  candidate.sourceUid = state_.localUid;
  candidate.hostUid = state_.hostUid;
  candidate.sessionNonce = state_.sessionNonce;
  candidate.sequence = static_cast<uint16_t>(state_.txSequence + 1U);
  candidate.round = state_.currentRound;
  candidate.value = static_cast<uint8_t>(choice);
  if (validate(candidate) != Status::Ok)
    return SessionStatus::InvalidState;
  state_.txSequence = candidate.sequence;
  state_.submittedMask |= mask;
  state_.choices[state_.currentRound - 1U] = candidate.value;
  output = candidate;
  return SessionStatus::Ok;
}

SessionStatus ParticipantSession::expireIfNeeded(uint32_t nowMs) {
  if (!available_) return SessionStatus::StateUnavailable;
  const SessionPhase phase = static_cast<SessionPhase>(state_.phase);
  if ((phase == SessionPhase::Joining || phase == SessionPhase::Lobby ||
       runningPhase(phase)) &&
      deadlineReached(nowMs, state_.deadlineMs)) {
    state_.phase = static_cast<uint8_t>(SessionPhase::Expired);
    state_.deadlineMs = 0U;
    return SessionStatus::TimedOut;
  }
  return SessionStatus::Ok;
}

void ParticipantSession::reset() {
  state_ = ParticipantState{};
  available_ = true;
}

const ParticipantState& ParticipantSession::state() const { return state_; }

ParticipantState ParticipantSession::snapshot() const { return state_; }

SessionStatus ParticipantSession::restore(const ParticipantState& state) {
  if (state.schemaVersion != kParticipantStateSchemaVersion) {
    state_ = ParticipantState{};
    state_.schemaVersion = 0U;
    state_.phase = static_cast<uint8_t>(SessionPhase::Unavailable);
    available_ = false;
    return SessionStatus::UnsupportedState;
  }
  if (!validateParticipantState(state)) {
    state_ = ParticipantState{};
    state_.schemaVersion = 0U;
    state_.phase = static_cast<uint8_t>(SessionPhase::Unavailable);
    available_ = false;
    return SessionStatus::InvalidState;
  }
  state_ = state;
  available_ = true;
  return SessionStatus::Ok;
}

bool validateRewardState(const RewardState& state) {
  if (state.schemaVersion != kRewardStateSchemaVersion ||
      state.peerCount > kRewardPeerCapacity ||
      state.recentSessionCount > kRecentSessionCapacity ||
      state.recentSessionNext >= kRecentSessionCapacity ||
      state.rewardedHunts > state.completedHunts) {
    return false;
  }
  if (state.completedHunts == 0U) {
    if (state.lastObservedDayId != 0U ||
        state.lastObservedEpochSeconds != 0U ||
        state.recentSessionCount != 0U) {
      return false;
    }
  } else if (state.lastObservedDayId == 0U ||
             state.lastObservedEpochSeconds == 0U ||
             state.recentSessionCount == 0U) {
    return false;
  }
  if (state.recentSessionCount < kRecentSessionCapacity) {
    if (state.recentSessionNext != state.recentSessionCount) return false;
  }
  for (size_t index = 0U; index < kRecentSessionCapacity; ++index) {
    const bool occupied = index < state.recentSessionCount;
    if (occupied != (state.recentSessionNonces[index] != 0U)) return false;
    if (occupied) {
      for (size_t prior = 0U; prior < index; ++prior) {
        if (state.recentSessionNonces[prior] ==
            state.recentSessionNonces[index]) {
          return false;
        }
      }
    }
  }
  for (size_t index = 0U; index < kRewardPeerCapacity; ++index) {
    const PeerRewardState& peer = state.peers[index];
    const bool occupied = index < state.peerCount;
    if (!occupied) {
      if (peer.uid != 0U || peer.rewardedParties != 0U ||
          peer.lastRewardDayId != 0U || peer.lastRewardEpochSeconds != 0U) {
        return false;
      }
      continue;
    }
    if (peer.uid == 0U || peer.rewardedParties == 0U ||
        peer.lastRewardDayId == 0U || peer.lastRewardEpochSeconds == 0U ||
        peer.lastRewardDayId > state.lastObservedDayId ||
        peer.lastRewardEpochSeconds > state.lastObservedEpochSeconds) {
      return false;
    }
    for (size_t prior = 0U; prior < index; ++prior) {
      if (state.peers[prior].uid == peer.uid) return false;
    }
  }
  if (state.rewardedHunts == 0U) {
    return state.partyBond == 0U && state.rewardedPeerEvents == 0U &&
           state.lastStreakDayId == 0U && state.currentStreakDays == 0U &&
           state.longestStreakDays == 0U && state.peerCount == 0U;
  }
  return state.partyBond != 0U && state.rewardedPeerEvents != 0U &&
         state.lastStreakDayId != 0U && state.currentStreakDays != 0U &&
         state.longestStreakDays >= state.currentStreakDays &&
         state.lastStreakDayId <= state.lastObservedDayId &&
         state.peerCount != 0U;
}

PeerRewardState* PartyRewardLedger::findPeer(uint16_t uid) {
  for (uint8_t index = 0U; index < state_.peerCount; ++index) {
    if (state_.peers[index].uid == uid) return &state_.peers[index];
  }
  return nullptr;
}

PeerRewardState* PartyRewardLedger::allocatePeer(uint16_t uid) {
  PeerRewardState* existing = findPeer(uid);
  if (existing != nullptr) return existing;
  if (state_.peerCount < kRewardPeerCapacity) {
    PeerRewardState* output = &state_.peers[state_.peerCount++];
    *output = PeerRewardState{};
    output->uid = uid;
    return output;
  }
  size_t oldest = 0U;
  for (size_t index = 1U; index < kRewardPeerCapacity; ++index) {
    if (state_.peers[index].lastRewardDayId <
            state_.peers[oldest].lastRewardDayId ||
        (state_.peers[index].lastRewardDayId ==
             state_.peers[oldest].lastRewardDayId &&
         state_.peers[index].lastRewardEpochSeconds <
             state_.peers[oldest].lastRewardEpochSeconds) ||
        (state_.peers[index].lastRewardDayId ==
             state_.peers[oldest].lastRewardDayId &&
         state_.peers[index].lastRewardEpochSeconds ==
             state_.peers[oldest].lastRewardEpochSeconds &&
         state_.peers[index].uid < state_.peers[oldest].uid)) {
      oldest = index;
    }
  }
  state_.peers[oldest] = PeerRewardState{};
  state_.peers[oldest].uid = uid;
  return &state_.peers[oldest];
}

bool PartyRewardLedger::seenSession(uint32_t nonce) const {
  for (uint8_t index = 0U; index < state_.recentSessionCount; ++index) {
    if (state_.recentSessionNonces[index] == nonce) return true;
  }
  return false;
}

void PartyRewardLedger::rememberSession(uint32_t nonce) {
  if (state_.recentSessionCount < kRecentSessionCapacity) {
    state_.recentSessionNonces[state_.recentSessionCount] = nonce;
    ++state_.recentSessionCount;
    state_.recentSessionNext = static_cast<uint8_t>(
        state_.recentSessionCount % kRecentSessionCapacity);
    return;
  }
  state_.recentSessionNonces[state_.recentSessionNext] = nonce;
  state_.recentSessionNext = static_cast<uint8_t>(
      (state_.recentSessionNext + 1U) % kRecentSessionCapacity);
}

RewardStatus PartyRewardLedger::recordCompletedHunt(
    const CompletedHuntReward& completed, uint32_t peerCooldownSeconds,
    RewardOutcome& output) {
  if (!available_) return RewardStatus::StateUnavailable;
  if (completed.selfUid == 0U || completed.sessionNonce == 0U ||
      completed.resultProof == 0U || completed.dayId == 0U ||
      completed.nowEpochSeconds == 0U ||
      !validParticipantCount(completed.participantCount,
                             kMinimumParticipants) ||
      !supportedTier(static_cast<ResultTier>(completed.tier))) {
    return RewardStatus::InvalidInput;
  }
  uint8_t selfCount = 0U;
  for (uint8_t index = 0U; index < completed.participantCount; ++index) {
    if (completed.participantUids[index] == 0U)
      return RewardStatus::InvalidInput;
    if (completed.participantUids[index] == completed.selfUid) ++selfCount;
    for (uint8_t prior = 0U; prior < index; ++prior) {
      if (completed.participantUids[prior] ==
          completed.participantUids[index]) {
        return RewardStatus::InvalidInput;
      }
    }
  }
  for (uint8_t index = completed.participantCount;
       index < kMaximumParticipants; ++index) {
    if (completed.participantUids[index] != 0U)
      return RewardStatus::InvalidInput;
  }
  if (selfCount != 1U) return RewardStatus::InvalidInput;
  if (seenSession(completed.sessionNonce))
    return RewardStatus::DuplicateSession;
  if ((state_.lastObservedDayId != 0U &&
       completed.dayId < state_.lastObservedDayId) ||
      (state_.lastObservedEpochSeconds != 0U &&
       completed.nowEpochSeconds < state_.lastObservedEpochSeconds)) {
    return RewardStatus::ClockRegression;
  }

  uint16_t eligibleUids[kMaximumParticipants - 1U]{};
  uint8_t eligibleCount = 0U;
  for (uint8_t index = 0U; index < completed.participantCount; ++index) {
    const uint16_t uid = completed.participantUids[index];
    if (uid == completed.selfUid) continue;
    const PeerRewardState* peer = findPeer(uid);
    if (peer != nullptr) {
      if (peer->lastRewardDayId == completed.dayId) continue;
      const uint32_t elapsed =
          completed.nowEpochSeconds - peer->lastRewardEpochSeconds;
      if (elapsed < peerCooldownSeconds) continue;
    }
    eligibleUids[eligibleCount++] = uid;
  }

  rememberSession(completed.sessionNonce);
  state_.completedHunts = saturatingAdd(state_.completedHunts, UINT32_C(1));
  state_.lastObservedDayId = completed.dayId;
  state_.lastObservedEpochSeconds = completed.nowEpochSeconds;
  output = RewardOutcome{};
  output.partyBondAfter = state_.partyBond;
  output.currentStreakDays = state_.currentStreakDays;
  output.longestStreakDays = state_.longestStreakDays;
  if (eligibleCount == 0U) return RewardStatus::RecordedNoEligiblePeer;

  for (uint8_t index = 0U; index < eligibleCount; ++index) {
    PeerRewardState* peer = allocatePeer(eligibleUids[index]);
    peer->lastRewardDayId = completed.dayId;
    peer->lastRewardEpochSeconds = completed.nowEpochSeconds;
    peer->rewardedParties =
        saturatingAdd(peer->rewardedParties, static_cast<uint16_t>(1U));
  }

  const uint16_t perPeer =
      static_cast<uint16_t>(1U + static_cast<uint16_t>(completed.tier));
  const uint16_t groupBonus =
      static_cast<uint16_t>(completed.participantCount -
                            kMinimumParticipants);
  const uint16_t bondAwarded = static_cast<uint16_t>(
      static_cast<uint16_t>(eligibleCount) * perPeer + groupBonus);
  state_.partyBond =
      saturatingAdd(state_.partyBond, static_cast<uint32_t>(bondAwarded));
  state_.rewardedHunts =
      saturatingAdd(state_.rewardedHunts, UINT32_C(1));
  state_.rewardedPeerEvents = saturatingAdd(
      state_.rewardedPeerEvents, static_cast<uint32_t>(eligibleCount));

  uint8_t streakAdvanced = 0U;
  if (state_.lastStreakDayId != completed.dayId) {
    if (state_.lastStreakDayId != 0U &&
        completed.dayId == state_.lastStreakDayId + 1U) {
      state_.currentStreakDays = saturatingAdd(
          state_.currentStreakDays, static_cast<uint16_t>(1U));
    } else {
      state_.currentStreakDays = 1U;
    }
    state_.lastStreakDayId = completed.dayId;
    if (state_.currentStreakDays > state_.longestStreakDays) {
      state_.longestStreakDays = state_.currentStreakDays;
    }
    streakAdvanced = 1U;
  }

  output.eligibleUniquePeers = eligibleCount;
  output.streakAdvanced = streakAdvanced;
  output.bondAwarded = bondAwarded;
  output.partyBondAfter = state_.partyBond;
  output.currentStreakDays = state_.currentStreakDays;
  output.longestStreakDays = state_.longestStreakDays;
  return RewardStatus::Awarded;
}

const RewardState& PartyRewardLedger::state() const { return state_; }

RewardState PartyRewardLedger::snapshot() const { return state_; }

RewardStatus PartyRewardLedger::restore(const RewardState& state) {
  if (state.schemaVersion != kRewardStateSchemaVersion) {
    quarantine();
    return RewardStatus::UnsupportedState;
  }
  if (!validateRewardState(state)) {
    quarantine();
    return RewardStatus::InvalidState;
  }
  state_ = state;
  available_ = true;
  return RewardStatus::Awarded;
}

void PartyRewardLedger::reset() {
  state_ = RewardState{};
  available_ = true;
}

void PartyRewardLedger::quarantine() {
  state_ = RewardState{};
  state_.schemaVersion = 0U;
  available_ = false;
}

const char* sessionStatusName(SessionStatus status) {
  switch (status) {
    case SessionStatus::Ok: return "ok";
    case SessionStatus::InvalidArgument: return "invalid_argument";
    case SessionStatus::InvalidPacket: return "invalid_packet";
    case SessionStatus::WrongPhase: return "wrong_phase";
    case SessionStatus::WrongSession: return "wrong_session";
    case SessionStatus::WrongHost: return "wrong_host";
    case SessionStatus::Duplicate: return "duplicate";
    case SessionStatus::StaleSequence: return "stale_sequence";
    case SessionStatus::SequenceConflict: return "sequence_conflict";
    case SessionStatus::SequenceExhausted: return "sequence_exhausted";
    case SessionStatus::PartyFull: return "party_full";
    case SessionStatus::UnknownParticipant: return "unknown_participant";
    case SessionStatus::RoundMismatch: return "round_mismatch";
    case SessionStatus::ChoiceAlreadySubmitted:
      return "choice_already_submitted";
    case SessionStatus::NotReady: return "not_ready";
    case SessionStatus::TimedOut: return "timed_out";
    case SessionStatus::UnsupportedState: return "unsupported_state";
    case SessionStatus::InvalidState: return "invalid_state";
    case SessionStatus::StateUnavailable: return "state_unavailable";
  }
  return "unknown";
}

const char* rewardStatusName(RewardStatus status) {
  switch (status) {
    case RewardStatus::Awarded: return "awarded";
    case RewardStatus::RecordedNoEligiblePeer:
      return "recorded_no_eligible_peer";
    case RewardStatus::DuplicateSession: return "duplicate_session";
    case RewardStatus::InvalidInput: return "invalid_input";
    case RewardStatus::ClockRegression: return "clock_regression";
    case RewardStatus::UnsupportedState: return "unsupported_state";
    case RewardStatus::InvalidState: return "invalid_state";
    case RewardStatus::StateUnavailable: return "state_unavailable";
  }
  return "unknown";
}

}  // namespace party
}  // namespace kitsu868
