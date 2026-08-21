#pragma once

#include <stddef.h>
#include <stdint.h>

// Transport-independent Kitsu868 encounter packets.
//
// This module only validates, encodes, and interprets bytes.  It deliberately
// contains no RadioLib/LoRa calls and cannot transmit.  A future caller must
// put transmission behind an explicit user action, airtime/duty-cycle budget,
// power/frequency configuration, and any required channel-access gate.
namespace kitsu868 {
namespace encounter {

constexpr uint8_t kMagic0 = 0x4B;  // 'K'
constexpr uint8_t kMagic1 = 0x38;  // '8'
constexpr uint8_t kProtocolVersion = 1;
constexpr size_t kWireBytes = 19;
constexpr uint8_t kMaxAppearance = 31;
constexpr uint8_t kMaxEvolutionStage = 7;
constexpr uint8_t kMaxBond = 100;
constexpr uint8_t kMaxMood = 15;
constexpr uint8_t kMaxEmote = 15;
constexpr uint8_t kTraitCount = 16;
constexpr uint8_t kGiftCount = 12;

static_assert(kWireBytes <= 32, "encounter packet must fit the 32-byte budget");

enum class PacketType : uint8_t {
  Offer = 1,
  Reply = 2,
};

// uid is the four hexadecimal digits following "KT" (KTDEAD -> 0xDEAD).
// The wire representation is explicit; this in-memory struct is not packed.
struct Packet {
  PacketType type;
  uint16_t uid;
  uint32_t packId;
  uint8_t appearance;
  uint8_t evolutionStage;
  uint8_t bond;
  uint8_t mood;
  uint8_t emote;
  uint32_t nonce;
};

enum class Status : uint8_t {
  Ok = 0,
  NullArgument,
  WrongLength,
  BufferTooSmall,
  BadMagic,
  UnsupportedVersion,
  UnsupportedType,
  InvalidUid,
  InvalidPackId,
  InvalidAppearance,
  InvalidEvolutionStage,
  InvalidBond,
  InvalidMood,
  InvalidEmote,
  InvalidNonce,
  IntegrityMismatch,
};

// Wire format (all multi-byte integers are little-endian):
//   0..1   magic "K8"
//   2      protocol version (1)
//   3      PacketType
//   4..5   compact KT UID suffix
//   6..9   companion pack ID
//   10     appearance[4:0] | evolutionStage[2:0] << 5
//   11     bond, 0..100
//   12     mood[3:0] | emote[3:0] << 4
//   13..16 encounter nonce
//   17..18 CRC-16/CCITT-FALSE over bytes 0..16
//
// The CRC detects accidental damage; it is not authentication and must not be
// treated as proof that a peer or packet is trustworthy.
Status encode(const Packet& packet, uint8_t* output, size_t capacity,
              size_t* bytesWritten = nullptr);
Status decode(const uint8_t* wire, size_t length, Packet& output);
Status validate(const Packet& packet);
const char* statusName(Status status);

uint16_t crc16CcittFalse(const uint8_t* data, size_t length);

// A short cache hint for peer deduplication.  Hash collisions are possible;
// use samePeerExact() whenever both full packets are still available.
uint16_t compactPeerHash(const Packet& packet);
bool samePeerExact(const Packet& a, const Packet& b);

struct DuplicateToken {
  uint16_t peerHash;
  uint32_t nonce;
};

DuplicateToken makeDuplicateToken(const Packet& packet);
bool isDuplicate(const DuplicateToken& previous, const Packet& candidate);

// Both devices get the same cosmetic outcome regardless of argument order.
// The result is deterministic, non-secret, and derived only from the two
// compact identities and two encounter nonces.
struct SharedResult {
  uint32_t seed;
  uint8_t trait;
  uint8_t gift;
};

SharedResult deriveSharedResult(const Packet& a, const Packet& b);

}  // namespace encounter
}  // namespace kitsu868
