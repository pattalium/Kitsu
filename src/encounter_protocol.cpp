#include "encounter_protocol.h"

namespace kitsu868 {
namespace encounter {
namespace {

constexpr size_t kIntegrityOffset = 17;

bool supportedType(PacketType type) {
  return type == PacketType::Offer || type == PacketType::Reply;
}

void put16(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8);
}

void put32(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8);
  output[2] = static_cast<uint8_t>(value >> 16);
  output[3] = static_cast<uint8_t>(value >> 24);
}

uint16_t get16(const uint8_t* input) {
  return static_cast<uint16_t>(input[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8);
}

uint32_t get32(const uint8_t* input) {
  return static_cast<uint32_t>(input[0]) |
         (static_cast<uint32_t>(input[1]) << 8) |
         (static_cast<uint32_t>(input[2]) << 16) |
         (static_cast<uint32_t>(input[3]) << 24);
}

uint32_t fnvByte(uint32_t hash, uint8_t value) {
  return (hash ^ value) * UINT32_C(16777619);
}

uint32_t fnv16(uint32_t hash, uint16_t value) {
  hash = fnvByte(hash, static_cast<uint8_t>(value));
  return fnvByte(hash, static_cast<uint8_t>(value >> 8));
}

uint32_t fnv32(uint32_t hash, uint32_t value) {
  hash = fnvByte(hash, static_cast<uint8_t>(value));
  hash = fnvByte(hash, static_cast<uint8_t>(value >> 8));
  hash = fnvByte(hash, static_cast<uint8_t>(value >> 16));
  return fnvByte(hash, static_cast<uint8_t>(value >> 24));
}

uint32_t avalanche32(uint32_t value) {
  value ^= value >> 16;
  value *= UINT32_C(0x7FEB352D);
  value ^= value >> 15;
  value *= UINT32_C(0x846CA68B);
  value ^= value >> 16;
  return value;
}

bool participantAfter(const Packet& a, const Packet& b) {
  return a.uid > b.uid || (a.uid == b.uid && a.nonce > b.nonce);
}

}  // namespace

Status validate(const Packet& packet) {
  if (!supportedType(packet.type)) {
    return Status::UnsupportedType;
  }
  if (packet.uid == 0) {
    return Status::InvalidUid;
  }
  if (packet.packId == 0) {
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
  if (packet.nonce == 0) {
    return Status::InvalidNonce;
  }
  return Status::Ok;
}

uint16_t crc16CcittFalse(const uint8_t* data, size_t length) {
  if (data == nullptr && length != 0) {
    return 0;
  }
  uint16_t crc = UINT16_C(0xFFFF);
  for (size_t index = 0; index < length; ++index) {
    crc ^= static_cast<uint16_t>(static_cast<uint16_t>(data[index]) << 8);
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & UINT16_C(0x8000))
                ? static_cast<uint16_t>((crc << 1) ^ UINT16_C(0x1021))
                : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

Status encode(const Packet& packet, uint8_t* output, size_t capacity,
              size_t* bytesWritten) {
  if (bytesWritten != nullptr) {
    *bytesWritten = 0;
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
  put16(output + 4, packet.uid);
  put32(output + 6, packet.packId);
  output[10] = static_cast<uint8_t>(packet.appearance |
                                    (packet.evolutionStage << 5));
  output[11] = packet.bond;
  output[12] = static_cast<uint8_t>(packet.mood | (packet.emote << 4));
  put32(output + 13, packet.nonce);
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
  if (!supportedType(type)) {
    return Status::UnsupportedType;
  }
  if (get16(wire + kIntegrityOffset) !=
      crc16CcittFalse(wire, kIntegrityOffset)) {
    return Status::IntegrityMismatch;
  }

  Packet candidate{};
  candidate.type = type;
  candidate.uid = get16(wire + 4);
  candidate.packId = get32(wire + 6);
  candidate.appearance = static_cast<uint8_t>(wire[10] & 0x1F);
  candidate.evolutionStage = static_cast<uint8_t>(wire[10] >> 5);
  candidate.bond = wire[11];
  candidate.mood = static_cast<uint8_t>(wire[12] & 0x0F);
  candidate.emote = static_cast<uint8_t>(wire[12] >> 4);
  candidate.nonce = get32(wire + 13);

  const Status packetStatus = validate(candidate);
  if (packetStatus != Status::Ok) {
    return packetStatus;
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
    case Status::InvalidUid:
      return "invalid_uid";
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
    case Status::InvalidNonce:
      return "invalid_nonce";
    case Status::IntegrityMismatch:
      return "integrity_mismatch";
  }
  return "unknown";
}

uint16_t compactPeerHash(const Packet& packet) {
  uint8_t identity[6];
  put16(identity, packet.uid);
  put32(identity + 2, packet.packId);
  return crc16CcittFalse(identity, sizeof(identity));
}

bool samePeerExact(const Packet& a, const Packet& b) {
  return a.uid == b.uid;
}

DuplicateToken makeDuplicateToken(const Packet& packet) {
  DuplicateToken token{};
  token.peerHash = compactPeerHash(packet);
  token.nonce = packet.nonce;
  return token;
}

bool isDuplicate(const DuplicateToken& previous, const Packet& candidate) {
  return previous.peerHash == compactPeerHash(candidate) &&
         previous.nonce == candidate.nonce;
}

SharedResult deriveSharedResult(const Packet& a, const Packet& b) {
  const Packet* first = &a;
  const Packet* second = &b;
  if (participantAfter(a, b)) {
    first = &b;
    second = &a;
  }

  // FNV-1a over a domain tag and the canonical (UID, nonce) pairs, followed by
  // an avalanche.  This is stable across architectures and argument order.
  uint32_t hash = UINT32_C(2166136261);
  hash = fnvByte(hash, 'K');
  hash = fnvByte(hash, '8');
  hash = fnvByte(hash, 'R');
  hash = fnvByte(hash, kProtocolVersion);
  hash = fnv16(hash, first->uid);
  hash = fnv32(hash, first->nonce);
  hash = fnv16(hash, second->uid);
  hash = fnv32(hash, second->nonce);
  hash = avalanche32(hash);

  SharedResult result{};
  result.seed = hash;
  result.trait = static_cast<uint8_t>(hash % kTraitCount);
  result.gift = static_cast<uint8_t>((hash >> 8) % kGiftCount);
  return result;
}

}  // namespace encounter
}  // namespace kitsu868

