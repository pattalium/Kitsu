#include "../src/kitsu_nearby_protocol.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace nearby = kitsu868::nearby;

static_assert(static_cast<uint8_t>(nearby::PositiveAction::Pet) == 1U,
              "Pet wire value changed");
static_assert(static_cast<uint8_t>(nearby::PositiveAction::Greet) == 2U,
              "Greet wire value changed");
static_assert(static_cast<uint8_t>(nearby::PositiveAction::Play) == 3U,
              "Play wire value changed");
static_assert(static_cast<uint8_t>(nearby::PositiveAction::Gift) == 4U,
              "Gift wire value must remain 4");

namespace {

int failures = 0;

void check(bool condition, const char* description) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL " << description << '\n';
  }
}

nearby::Packet presencePacket() {
  nearby::Packet packet{};
  packet.type = nearby::PacketType::Presence;
  packet.sourceUid = 0xDEADU;
  packet.targetUid = 0xBEEFU;
  packet.sessionNonce = UINT32_C(0x1234ABCD);
  packet.packId = UINT32_C(0x13579BDF);
  packet.appearance = 17U;
  packet.evolutionStage = 3U;
  packet.bond = 87U;
  packet.mood = 9U;
  packet.emote = 4U;
  return packet;
}

nearby::Packet actionRequest() {
  nearby::Packet packet{};
  packet.type = nearby::PacketType::ActionRequest;
  packet.sourceUid = 0xDEADU;
  packet.targetUid = 0xBEEFU;
  packet.sessionNonce = UINT32_C(0x1234ABCD);
  packet.requestSequence = 0x1234U;
  packet.action = nearby::PositiveAction::Pet;
  return packet;
}

nearby::Packet giftRequest() {
  nearby::Packet packet = actionRequest();
  packet.action = nearby::PositiveAction::Gift;
  return packet;
}

void refreshCrc(std::array<uint8_t, nearby::kWireBytes>& wire) {
  const uint16_t crc = nearby::crc16CcittFalse(wire.data(), 24U);
  wire[24] = static_cast<uint8_t>(crc);
  wire[25] = static_cast<uint8_t>(crc >> 8U);
}

void expectDecodeStatus(std::array<uint8_t, nearby::kWireBytes> wire,
                        nearby::Status expected, const char* description) {
  refreshCrc(wire);
  nearby::Packet parsed{};
  check(nearby::decode(wire.data(), wire.size(), parsed) == expected,
        description);
}

void testGoldenPresenceAndRequest() {
  static constexpr std::array<uint8_t, nearby::kWireBytes> expectedPresence = {
      0x4B, 0x38, 0x02, 0x01, 0xAD, 0xDE, 0xEF, 0xBE, 0xCD,
      0xAB, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0xDF, 0x9B,
      0x57, 0x13, 0x71, 0x57, 0x49, 0x00, 0x74, 0xDB,
  };
  static constexpr std::array<uint8_t, nearby::kWireBytes> expectedRequest = {
      0x4B, 0x38, 0x02, 0x02, 0xAD, 0xDE, 0xEF, 0xBE, 0xCD,
      0xAB, 0x34, 0x12, 0x34, 0x12, 0x01, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xBC, 0x49,
  };
  static constexpr std::array<uint8_t, nearby::kWireBytes> expectedGift = {
      0x4B, 0x38, 0x02, 0x02, 0xAD, 0xDE, 0xEF, 0xBE, 0xCD,
      0xAB, 0x34, 0x12, 0x34, 0x12, 0x04, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCC, 0x8B,
  };

  std::array<uint8_t, nearby::kWireBytes> wire{};
  size_t written = 99U;
  check(nearby::encode(presencePacket(), wire.data(), wire.size(), &written) ==
            nearby::Status::Ok,
        "v2 presence encodes");
  check(written == nearby::kWireBytes && wire == expectedPresence,
        "v2 presence matches golden wire bytes");
  check(nearby::crc16CcittFalse(wire.data(), 24U) == 0xDB74U,
        "v2 presence golden CRC is DB74");

  nearby::Packet parsed{};
  check(nearby::decode(wire.data(), wire.size(), parsed) == nearby::Status::Ok,
        "v2 presence decodes");
  const nearby::Packet expected = presencePacket();
  check(parsed.type == expected.type &&
            parsed.sourceUid == expected.sourceUid &&
            parsed.targetUid == expected.targetUid &&
            parsed.sessionNonce == expected.sessionNonce &&
            parsed.packId == expected.packId &&
            parsed.appearance == expected.appearance &&
            parsed.evolutionStage == expected.evolutionStage &&
            parsed.bond == expected.bond && parsed.mood == expected.mood &&
            parsed.emote == expected.emote,
        "presence round-trips every public presentation field");

  check(nearby::encode(actionRequest(), wire.data(), wire.size(), &written) ==
            nearby::Status::Ok &&
            wire == expectedRequest,
        "Pet request matches golden wire bytes");

  check(nearby::encode(giftRequest(), wire.data(), wire.size(), &written) ==
            nearby::Status::Ok &&
            wire == expectedGift &&
            nearby::crc16CcittFalse(wire.data(), 24U) == 0x8BCCU,
        "Gift request uses action value 4 in the unchanged v2 wire format");
  check(nearby::decode(wire.data(), wire.size(), parsed) ==
            nearby::Status::Ok &&
            parsed.action == nearby::PositiveAction::Gift,
        "Gift request round-trips through the v2 codec");
}

void testValidationAndCorruption() {
  std::array<uint8_t, nearby::kWireBytes> wire{};
  check(nearby::encode(presencePacket(), wire.data(), wire.size()) ==
            nearby::Status::Ok,
        "corruption fixture encodes");
  for (size_t index = 0U; index < wire.size(); ++index) {
    std::array<uint8_t, nearby::kWireBytes> corrupted = wire;
    corrupted[index] ^= 0x01U;
    nearby::Packet parsed{};
    if (nearby::decode(corrupted.data(), corrupted.size(), parsed) ==
        nearby::Status::Ok) {
      ++failures;
      std::cerr << "FAIL one-bit corruption accepted at byte " << index
                << '\n';
    }
  }

  nearby::Packet parsed{};
  check(nearby::decode(nullptr, wire.size(), parsed) ==
            nearby::Status::NullArgument,
        "null input rejected");
  check(nearby::decode(wire.data(), wire.size() - 1U, parsed) ==
            nearby::Status::WrongLength,
        "short input rejected");
  check(nearby::decode(wire.data(), wire.size() + 1U, parsed) ==
            nearby::Status::WrongLength,
        "long input rejected");

  std::array<uint8_t, nearby::kWireBytes> changed = wire;
  changed[0] = 'X';
  expectDecodeStatus(changed, nearby::Status::BadMagic,
                     "unknown magic rejected");
  changed = wire;
  changed[2] = 3U;
  expectDecodeStatus(changed, nearby::Status::UnsupportedVersion,
                     "unknown version rejected");
  changed = wire;
  changed[3] = 4U;
  expectDecodeStatus(changed, nearby::Status::UnsupportedType,
                     "unknown packet type rejected");
  changed = wire;
  changed[23] = 1U;
  expectDecodeStatus(changed, nearby::Status::UnexpectedField,
                     "non-zero reserved byte rejected");

  nearby::Packet invalid = presencePacket();
  invalid.sourceUid = 0U;
  check(nearby::validate(invalid) == nearby::Status::InvalidSourceUid,
        "zero source UID rejected");
  invalid = presencePacket();
  invalid.targetUid = invalid.sourceUid;
  check(nearby::validate(invalid) == nearby::Status::SelfTarget,
        "self-targeted presence rejected");
  invalid = presencePacket();
  invalid.targetUid = 0U;
  check(nearby::validate(invalid) == nearby::Status::Ok,
        "broadcast presence accepts a zero target");
  invalid = presencePacket();
  invalid.sessionNonce = 0U;
  check(nearby::validate(invalid) == nearby::Status::InvalidSessionNonce,
        "zero session nonce rejected");
  invalid = presencePacket();
  invalid.requestSequence = 1U;
  check(nearby::validate(invalid) == nearby::Status::InvalidRequestSequence,
        "presence cannot carry request sequence");
  invalid = presencePacket();
  invalid.action = nearby::PositiveAction::Pet;
  check(nearby::validate(invalid) == nearby::Status::UnsupportedAction,
        "presence cannot execute an action");
  invalid = presencePacket();
  invalid.packId = 0U;
  check(nearby::validate(invalid) == nearby::Status::InvalidPackId,
        "presence requires an installed pack ID");
  invalid = presencePacket();
  invalid.appearance = nearby::kMaxAppearance + 1U;
  check(nearby::validate(invalid) == nearby::Status::InvalidAppearance,
        "presence appearance range validated");
  invalid = presencePacket();
  invalid.evolutionStage = nearby::kMaxEvolutionStage + 1U;
  check(nearby::validate(invalid) == nearby::Status::InvalidEvolutionStage,
        "presence evolution range validated");
  invalid = presencePacket();
  invalid.bond = nearby::kMaxBond + 1U;
  check(nearby::validate(invalid) == nearby::Status::InvalidBond,
        "presence bond range validated");
  invalid = presencePacket();
  invalid.mood = nearby::kMaxMood + 1U;
  check(nearby::validate(invalid) == nearby::Status::InvalidMood,
        "presence mood range validated");
  invalid = presencePacket();
  invalid.emote = nearby::kMaxEmote + 1U;
  check(nearby::validate(invalid) == nearby::Status::InvalidEmote,
        "presence emote range validated");

  invalid = actionRequest();
  invalid.targetUid = 0U;
  check(nearby::validate(invalid) == nearby::Status::InvalidTargetUid,
        "action request requires target UID");
  invalid = actionRequest();
  invalid.requestSequence = 0U;
  check(nearby::validate(invalid) == nearby::Status::InvalidRequestSequence,
        "action request requires sequence");
  invalid = actionRequest();
  invalid.action = static_cast<nearby::PositiveAction>(99U);
  check(nearby::validate(invalid) == nearby::Status::UnsupportedAction,
        "unknown action rejected");
  invalid = actionRequest();
  invalid.result = nearby::ActionResult::Accepted;
  check(nearby::validate(invalid) == nearby::Status::UnsupportedResult,
        "request cannot claim its own result");
  invalid = actionRequest();
  invalid.packId = 1U;
  check(nearby::validate(invalid) == nearby::Status::UnexpectedField,
        "action request cannot smuggle profile data");

  std::array<uint8_t, nearby::kWireBytes> output{};
  size_t written = 77U;
  check(nearby::encode(presencePacket(), nullptr, output.size(), &written) ==
            nearby::Status::NullArgument &&
            written == 0U,
        "encoder rejects null output transactionally");
  check(nearby::encode(presencePacket(), output.data(), output.size() - 1U,
                       &written) == nearby::Status::BufferTooSmall &&
            written == 0U,
        "encoder rejects undersized output");

  nearby::Packet sentinel = presencePacket();
  sentinel.sourceUid = 0xAAAAU;
  changed = wire;
  changed[19] ^= 0x80U;
  check(nearby::decode(changed.data(), changed.size(), sentinel) ==
            nearby::Status::IntegrityMismatch,
        "damaged payload reports integrity mismatch");
  check(sentinel.sourceUid == 0xAAAAU,
        "failed decode leaves output untouched");
}

void testActionsAcknowledgementsAndDedupe() {
  const nearby::Packet request = actionRequest();
  check(nearby::supportedPositiveAction(nearby::PositiveAction::Pet) &&
            nearby::supportedPositiveAction(nearby::PositiveAction::Greet) &&
            nearby::supportedPositiveAction(nearby::PositiveAction::Play) &&
            nearby::supportedPositiveAction(nearby::PositiveAction::Gift),
        "positive action allowlist is explicit");
  check(!nearby::supportedPositiveAction(nearby::PositiveAction::None),
        "None is never executable");

  nearby::Packet result{};
  check(nearby::makeActionResult(request, nearby::ActionResult::Accepted,
                                 result),
        "canonical accepted result builds");
  check(result.type == nearby::PacketType::ActionResult &&
            result.sourceUid == request.targetUid &&
            result.targetUid == request.sourceUid &&
            result.result == nearby::ActionResult::Accepted &&
            nearby::validate(result) == nearby::Status::Ok,
        "result reverses endpoints and validates");
  check(nearby::actionResultAcknowledges(request, result),
        "result acknowledges exact request");

  nearby::Packet mismatch = result;
  ++mismatch.requestSequence;
  check(!nearby::actionResultAcknowledges(request, mismatch),
        "different request sequence is not an acknowledgement");
  mismatch = result;
  mismatch.action = nearby::PositiveAction::Greet;
  check(!nearby::actionResultAcknowledges(request, mismatch),
        "different action is not an acknowledgement");
  check(!nearby::makeActionResult(request, nearby::ActionResult::None,
                                  mismatch),
        "empty result cannot become an acknowledgement");

  const nearby::DuplicateToken requestToken =
      nearby::makeDuplicateToken(request);
  check(nearby::isDuplicate(requestToken, request),
        "exact request replay deduplicates");
  nearby::Packet next = request;
  ++next.requestSequence;
  check(!nearby::isDuplicate(requestToken, next),
        "next request sequence is distinct");
  next = request;
  ++next.sessionNonce;
  check(!nearby::isDuplicate(requestToken, next),
        "new nearby session is distinct");
  check(!nearby::isDuplicate(requestToken, result),
        "request and result have separate replay domains");
  nearby::Packet invalidRequest = request;
  invalidRequest.requestSequence = 0U;
  const nearby::DuplicateToken invalidToken =
      nearby::makeDuplicateToken(invalidRequest);
  check(invalidToken.valid == 0U &&
            !nearby::isDuplicate(invalidToken, invalidRequest),
        "invalid packets cannot create or match replay tokens");

  std::array<uint8_t, nearby::kWireBytes> resultWire{};
  check(nearby::encode(result, resultWire.data(), resultWire.size()) ==
            nearby::Status::Ok,
        "accepted action result encodes");
  nearby::Packet parsedResult{};
  check(nearby::decode(resultWire.data(), resultWire.size(), parsedResult) ==
            nearby::Status::Ok &&
            nearby::actionResultAcknowledges(request, parsedResult),
        "accepted action result round-trips as exact acknowledgement");

  const nearby::Packet gift = giftRequest();
  nearby::Packet giftResult{};
  check(nearby::makeActionResult(gift, nearby::ActionResult::Accepted,
                                 giftResult) &&
            giftResult.action == nearby::PositiveAction::Gift &&
            nearby::actionResultAcknowledges(gift, giftResult),
        "Gift receives the same canonical acknowledgement behavior");
  check(!nearby::isDuplicate(requestToken, gift),
        "Gift and Pet remain distinct replay-token actions");
  check(std::strcmp(nearby::statusName(nearby::Status::IntegrityMismatch),
                    "integrity_mismatch") == 0,
        "nearby status names are stable");
}

}  // namespace

int main() {
  testGoldenPresenceAndRequest();
  testValidationAndCorruption();
  testActionsAcknowledgementsAndDedupe();

  if (failures != 0) {
    std::cerr << "TEST_FAIL kitsu_nearby_protocol failures=" << failures
              << '\n';
    return 1;
  }
  std::cout << "TEST_PASS kitsu_nearby_protocol version=2 wire_bytes=26 "
               "presence_crc=DB74 request_crc=49BC gift_crc=8BCC actions=4\n";
  return 0;
}
