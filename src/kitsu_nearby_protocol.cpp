#include "kitsu_nearby_protocol.h"

namespace kitsu868 {
namespace nearby {
namespace {

constexpr size_t kReservedOffset = 23U;
constexpr size_t kIntegrityOffset = 24U;

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
  return type == PacketType::Presence || type == PacketType::ActionRequest ||
         type == PacketType::ActionResult;
}

bool profileFieldsAreZero(const Packet& packet) {
  return packet.packId == 0U && packet.appearance == 0U &&
         packet.evolutionStage == 0U && packet.bond == 0U &&
         packet.mood == 0U && packet.emote == 0U;
}

}  // namespace

bool supportedPositiveAction(PositiveAction action) {
  return action == PositiveAction::Pet || action == PositiveAction::Greet ||
         action == PositiveAction::Play || action == PositiveAction::Gift;
}

bool supportedActionResult(ActionResult result) {
  return result == ActionResult::Accepted ||
         result == ActionResult::Declined || result == ActionResult::Busy ||
         result == ActionResult::Disabled;
}

Status validate(const Packet& packet) {
  if (!supportedPacketType(packet.type)) {
    return Status::UnsupportedType;
  }
  if (packet.sourceUid == 0U) {
    return Status::InvalidSourceUid;
  }
  if (packet.sessionNonce == 0U) {
    return Status::InvalidSessionNonce;
  }

  if (packet.type == PacketType::Presence) {
    if (packet.targetUid == packet.sourceUid) {
      return Status::SelfTarget;
    }
    if (packet.requestSequence != 0U) {
      return Status::InvalidRequestSequence;
    }
    if (packet.action != PositiveAction::None) {
      return Status::UnsupportedAction;
    }
    if (packet.result != ActionResult::None) {
      return Status::UnsupportedResult;
    }
    if (packet.packId == 0U) {
      return Status::InvalidPackId;
    }
    if (packet.appearance > kMaxAppearance) {
      return Status::InvalidAppearance;
    }
    if (packet.evolutionStage > kMaxEvolutionStage) {
      return Status::InvalidEvolutionStage;
    }
    if (packet.bond > kMaxBond) {
      return Status::InvalidBond;
    }
    if (packet.mood > kMaxMood) {
      return Status::InvalidMood;
    }
    if (packet.emote > kMaxEmote) {
      return Status::InvalidEmote;
    }
    return Status::Ok;
  }

  if (packet.targetUid == 0U) {
    return Status::InvalidTargetUid;
  }
  if (packet.targetUid == packet.sourceUid) {
    return Status::SelfTarget;
  }
  if (packet.requestSequence == 0U) {
    return Status::InvalidRequestSequence;
  }
  if (!supportedPositiveAction(packet.action)) {
    return Status::UnsupportedAction;
  }
  if (!profileFieldsAreZero(packet)) {
    return Status::UnexpectedField;
  }

  if (packet.type == PacketType::ActionRequest) {
    return packet.result == ActionResult::None ? Status::Ok
                                               : Status::UnsupportedResult;
  }
  return supportedActionResult(packet.result) ? Status::Ok
                                               : Status::UnsupportedResult;
}

uint16_t crc16CcittFalse(const uint8_t* data, size_t length) {
  if (data == nullptr && length != 0U) {
    return 0U;
  }
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
  if (bytesWritten != nullptr) {
    *bytesWritten = 0U;
  }
  if (output == nullptr) {
    return Status::NullArgument;
  }
  if (capacity < kWireBytes) {
    return Status::BufferTooSmall;
  }
  const Status packetStatus = validate(packet);
  if (packetStatus != Status::Ok) {
    return packetStatus;
  }

  output[0] = kMagic0;
  output[1] = kMagic1;
  output[2] = kProtocolVersion;
  output[3] = static_cast<uint8_t>(packet.type);
  put16(output + 4U, packet.sourceUid);
  put16(output + 6U, packet.targetUid);
  put32(output + 8U, packet.sessionNonce);
  put16(output + 12U, packet.requestSequence);
  output[14] = static_cast<uint8_t>(packet.action);
  output[15] = static_cast<uint8_t>(packet.result);
  put32(output + 16U, packet.packId);
  output[20] = static_cast<uint8_t>(packet.appearance |
                                    (packet.evolutionStage << 5U));
  output[21] = packet.bond;
  output[22] = static_cast<uint8_t>(packet.mood | (packet.emote << 4U));
  output[kReservedOffset] = 0U;
  put16(output + kIntegrityOffset,
        crc16CcittFalse(output, kIntegrityOffset));

  if (bytesWritten != nullptr) {
    *bytesWritten = kWireBytes;
  }
  return Status::Ok;
}

Status decode(const uint8_t* wire, size_t length, Packet& output) {
  if (wire == nullptr) {
    return Status::NullArgument;
  }
  if (length != kWireBytes) {
    return Status::WrongLength;
  }
  if (wire[0] != kMagic0 || wire[1] != kMagic1) {
    return Status::BadMagic;
  }
  if (wire[2] != kProtocolVersion) {
    return Status::UnsupportedVersion;
  }
  const PacketType type = static_cast<PacketType>(wire[3]);
  if (!supportedPacketType(type)) {
    return Status::UnsupportedType;
  }
  if (wire[kReservedOffset] != 0U) {
    return Status::UnexpectedField;
  }
  if (get16(wire + kIntegrityOffset) !=
      crc16CcittFalse(wire, kIntegrityOffset)) {
    return Status::IntegrityMismatch;
  }

  Packet candidate{};
  candidate.type = type;
  candidate.sourceUid = get16(wire + 4U);
  candidate.targetUid = get16(wire + 6U);
  candidate.sessionNonce = get32(wire + 8U);
  candidate.requestSequence = get16(wire + 12U);
  candidate.action = static_cast<PositiveAction>(wire[14]);
  candidate.result = static_cast<ActionResult>(wire[15]);
  candidate.packId = get32(wire + 16U);
  candidate.appearance = static_cast<uint8_t>(wire[20] & 0x1FU);
  candidate.evolutionStage = static_cast<uint8_t>(wire[20] >> 5U);
  candidate.bond = wire[21];
  candidate.mood = static_cast<uint8_t>(wire[22] & 0x0FU);
  candidate.emote = static_cast<uint8_t>(wire[22] >> 4U);

  const Status candidateStatus = validate(candidate);
  if (candidateStatus != Status::Ok) {
    return candidateStatus;
  }
  output = candidate;
  return Status::Ok;
}

const char* statusName(Status status) {
  switch (status) {
    case Status::Ok:
      return "ok";
    case Status::NullArgument:
      return "null_argument";
    case Status::WrongLength:
      return "wrong_length";
    case Status::BufferTooSmall:
      return "buffer_too_small";
    case Status::BadMagic:
      return "bad_magic";
    case Status::UnsupportedVersion:
      return "unsupported_version";
    case Status::UnsupportedType:
      return "unsupported_type";
    case Status::InvalidSourceUid:
      return "invalid_source_uid";
    case Status::InvalidTargetUid:
      return "invalid_target_uid";
    case Status::SelfTarget:
      return "self_target";
    case Status::InvalidSessionNonce:
      return "invalid_session_nonce";
    case Status::InvalidRequestSequence:
      return "invalid_request_sequence";
    case Status::UnsupportedAction:
      return "unsupported_action";
    case Status::UnsupportedResult:
      return "unsupported_result";
    case Status::InvalidPackId:
      return "invalid_pack_id";
    case Status::InvalidAppearance:
      return "invalid_appearance";
    case Status::InvalidEvolutionStage:
      return "invalid_evolution_stage";
    case Status::InvalidBond:
      return "invalid_bond";
    case Status::InvalidMood:
      return "invalid_mood";
    case Status::InvalidEmote:
      return "invalid_emote";
    case Status::UnexpectedField:
      return "unexpected_field";
    case Status::IntegrityMismatch:
      return "integrity_mismatch";
  }
  return "unknown";
}

DuplicateToken makeDuplicateToken(const Packet& packet) {
  DuplicateToken output{};
  if (validate(packet) != Status::Ok) {
    return output;
  }
  output.valid = 1U;
  output.type = static_cast<uint8_t>(packet.type);
  output.sourceUid = packet.sourceUid;
  output.targetUid = packet.targetUid;
  output.sessionNonce = packet.sessionNonce;
  output.requestSequence = packet.requestSequence;
  output.action = static_cast<uint8_t>(packet.action);
  return output;
}

bool isDuplicate(const DuplicateToken& previous, const Packet& candidate) {
  return previous.valid == 1U && validate(candidate) == Status::Ok &&
         previous.type == static_cast<uint8_t>(candidate.type) &&
         previous.sourceUid == candidate.sourceUid &&
         previous.targetUid == candidate.targetUid &&
         previous.sessionNonce == candidate.sessionNonce &&
         previous.requestSequence == candidate.requestSequence &&
         previous.action == static_cast<uint8_t>(candidate.action);
}

bool actionResultAcknowledges(const Packet& request, const Packet& result) {
  return validate(request) == Status::Ok &&
         request.type == PacketType::ActionRequest &&
         validate(result) == Status::Ok &&
         result.type == PacketType::ActionResult &&
         result.sourceUid == request.targetUid &&
         result.targetUid == request.sourceUid &&
         result.sessionNonce == request.sessionNonce &&
         result.requestSequence == request.requestSequence &&
         result.action == request.action;
}

bool makeActionResult(const Packet& request, ActionResult result,
                      Packet& output) {
  if (validate(request) != Status::Ok ||
      request.type != PacketType::ActionRequest ||
      !supportedActionResult(result)) {
    return false;
  }
  Packet candidate{};
  candidate.type = PacketType::ActionResult;
  candidate.sourceUid = request.targetUid;
  candidate.targetUid = request.sourceUid;
  candidate.sessionNonce = request.sessionNonce;
  candidate.requestSequence = request.requestSequence;
  candidate.action = request.action;
  candidate.result = result;
  output = candidate;
  return true;
}

}  // namespace nearby
}  // namespace kitsu868
