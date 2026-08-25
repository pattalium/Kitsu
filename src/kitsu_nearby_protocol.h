#pragma once

#include <stddef.h>
#include <stdint.h>

// Transport-independent Kitsu nearby-pet protocol v2. These packets are for
// the dedicated direct Kitsu radio path only. The module has no MeshCore or
// repeater dependency and performs no transmission itself.
//
// encounter_protocol.h remains the unchanged v1 Offer/Reply ABI. This sibling
// codec adds explicit presence and acknowledged positive pet actions.
namespace kitsu868 {
namespace nearby {

constexpr uint8_t kMagic0 = 0x4BU;  // 'K'
constexpr uint8_t kMagic1 = 0x38U;  // '8'
constexpr uint8_t kProtocolVersion = 2U;
constexpr size_t kWireBytes = 26U;
constexpr uint8_t kMaxAppearance = 31U;
constexpr uint8_t kMaxEvolutionStage = 7U;
constexpr uint8_t kMaxBond = 100U;
constexpr uint8_t kMaxMood = 15U;
constexpr uint8_t kMaxEmote = 15U;

static_assert(kWireBytes <= 32U,
              "nearby pet packet must fit the 32-byte budget");

enum class PacketType : uint8_t {
  Presence = 1U,
  ActionRequest = 2U,
  ActionResult = 3U,
};

// This allowlist deliberately contains positive, bounded interactions only.
enum class PositiveAction : uint8_t {
  None = 0U,
  Pet = 1U,
  Greet = 2U,
  Play = 3U,
};

enum class ActionResult : uint8_t {
  None = 0U,
  Accepted = 1U,
  Declined = 2U,
  Busy = 3U,
  Disabled = 4U,
};

// The in-memory struct is not packed. Multi-byte wire values are explicitly
// little-endian. A Presence may be broadcast (targetUid == 0) or directed.
// Action packets always have a non-zero target distinct from sourceUid.
struct Packet {
  PacketType type = PacketType::Presence;
  uint16_t sourceUid = 0U;
  uint16_t targetUid = 0U;
  uint32_t sessionNonce = 0U;
  uint16_t requestSequence = 0U;
  PositiveAction action = PositiveAction::None;
  ActionResult result = ActionResult::None;

  // Presence-only public presentation data. The actual pack is never carried
  // by this packet. ActionRequest/ActionResult require these fields to be zero.
  uint32_t packId = 0U;
  uint8_t appearance = 0U;
  uint8_t evolutionStage = 0U;
  uint8_t bond = 0U;
  uint8_t mood = 0U;
  uint8_t emote = 0U;
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
  InvalidTargetUid,
  SelfTarget,
  InvalidSessionNonce,
  InvalidRequestSequence,
  UnsupportedAction,
  UnsupportedResult,
  InvalidPackId,
  InvalidAppearance,
  InvalidEvolutionStage,
  InvalidBond,
  InvalidMood,
  InvalidEmote,
  UnexpectedField,
  IntegrityMismatch,
};

// Wire format (all multi-byte integers are little-endian):
//   0..1   magic "K8"
//   2      protocol version (2)
//   3      PacketType
//   4..5   source KT UID suffix
//   6..7   target KT UID suffix, zero only for broadcast Presence
//   8..11  session nonce
//   12..13 request sequence, non-zero for action packets
//   14     PositiveAction
//   15     ActionResult
//   16..19 Presence pack ID, zero for action packets
//   20     Presence appearance[4:0] | evolutionStage[2:0] << 5
//   21     Presence bond, 0..100
//   22     Presence mood[3:0] | emote[3:0] << 4
//   23     reserved, must be zero
//   24..25 CRC-16/CCITT-FALSE over bytes 0..23
//
// The CRC detects accidental damage. It is not authentication or identity
// proof; transport policy must apply its own trust/rate/airtime controls.
Status encode(const Packet& packet, uint8_t* output, size_t capacity,
              size_t* bytesWritten = nullptr);
Status decode(const uint8_t* wire, size_t length, Packet& output);
Status validate(const Packet& packet);
const char* statusName(Status status);
uint16_t crc16CcittFalse(const uint8_t* data, size_t length);

bool supportedPositiveAction(PositiveAction action);
bool supportedActionResult(ActionResult result);

// Exact replay token; there are no truncated hashes or collision-based
// decisions. It remains valid when copied into a bounded persisted dedupe
// journal owned by the transport/runtime.
struct DuplicateToken {
  uint8_t valid = 0U;
  uint8_t type = 0U;
  uint16_t sourceUid = 0U;
  uint16_t targetUid = 0U;
  uint32_t sessionNonce = 0U;
  uint16_t requestSequence = 0U;
  uint8_t action = 0U;
};

DuplicateToken makeDuplicateToken(const Packet& packet);
bool isDuplicate(const DuplicateToken& previous, const Packet& candidate);

// Correlates an ActionResult with the request it acknowledges. The result
// source/target direction must be the reverse of the request.
bool actionResultAcknowledges(const Packet& request, const Packet& result);

// Builds the canonical acknowledgement fields without executing an action.
// Returns false for an invalid request or ActionResult::None.
bool makeActionResult(const Packet& request, ActionResult result,
                      Packet& output);

}  // namespace nearby
}  // namespace kitsu868
