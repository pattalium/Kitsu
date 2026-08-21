#include "../src/encounter_protocol.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>

namespace encounter = kitsu868::encounter;

namespace {

int failures = 0;

void check(bool condition, const char* description) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL " << description << '\n';
  }
}

encounter::Packet syntheticPacket() {
  encounter::Packet packet{};
  packet.type = encounter::PacketType::Offer;
  packet.uid = 0xDEAD;  // Synthetic fixture KTDEAD.
  packet.packId = 0x13579BDF;
  packet.appearance = 17;
  packet.evolutionStage = 3;
  packet.bond = 87;
  packet.mood = 9;
  packet.emote = 4;
  packet.nonce = 0x1234ABCD;
  return packet;
}

void refreshCrc(std::array<uint8_t, encounter::kWireBytes>& wire) {
  const uint16_t crc = encounter::crc16CcittFalse(wire.data(), 17);
  wire[17] = static_cast<uint8_t>(crc);
  wire[18] = static_cast<uint8_t>(crc >> 8);
}

void expectDecodeStatus(std::array<uint8_t, encounter::kWireBytes> wire,
                        encounter::Status expected, const char* description) {
  refreshCrc(wire);
  encounter::Packet parsed{};
  check(encounter::decode(wire.data(), wire.size(), parsed) == expected,
        description);
}

void testGoldenVector() {
  static constexpr std::array<uint8_t, encounter::kWireBytes> expected = {
      0x4B, 0x38, 0x01, 0x01, 0xAD, 0xDE, 0xDF, 0x9B, 0x57, 0x13,
      0x71, 0x57, 0x49, 0xCD, 0xAB, 0x34, 0x12, 0x15, 0xFA,
  };

  std::array<uint8_t, encounter::kWireBytes> encoded{};
  size_t written = 99;
  const encounter::Packet source = syntheticPacket();
  check(encounter::encode(source, encoded.data(), encoded.size(), &written) ==
            encounter::Status::Ok,
        "golden packet encodes");
  check(written == encounter::kWireBytes, "encoder reports 19 bytes");
  check(encoded == expected, "encoder matches golden wire bytes");
  check(encounter::crc16CcittFalse(encoded.data(), 17) == 0xFA15,
        "golden CRC-16/CCITT-FALSE is FA15");

  encounter::Packet parsed{};
  check(encounter::decode(expected.data(), expected.size(), parsed) ==
            encounter::Status::Ok,
        "golden packet decodes");
  check(parsed.type == source.type && parsed.uid == source.uid &&
            parsed.packId == source.packId &&
            parsed.appearance == source.appearance &&
            parsed.evolutionStage == source.evolutionStage &&
            parsed.bond == source.bond && parsed.mood == source.mood &&
            parsed.emote == source.emote && parsed.nonce == source.nonce,
        "golden packet round-trips every field");
}

void testStrictParserAndValidation() {
  std::array<uint8_t, encounter::kWireBytes> wire{};
  check(encounter::encode(syntheticPacket(), wire.data(), wire.size()) ==
            encounter::Status::Ok,
        "corruption-test packet encodes");

  for (size_t index = 0; index < wire.size(); ++index) {
    auto corrupted = wire;
    corrupted[index] ^= 0x01;
    encounter::Packet parsed{};
    const encounter::Status status =
        encounter::decode(corrupted.data(), corrupted.size(), parsed);
    if (status == encounter::Status::Ok) {
      ++failures;
      std::cerr << "FAIL one-bit corruption accepted at byte " << index << '\n';
    }
  }

  encounter::Packet parsed{};
  check(encounter::decode(nullptr, wire.size(), parsed) ==
            encounter::Status::NullArgument,
        "null input rejected");
  check(encounter::decode(wire.data(), wire.size() - 1, parsed) ==
            encounter::Status::WrongLength,
        "short packet rejected");
  check(encounter::decode(wire.data(), wire.size() + 1, parsed) ==
            encounter::Status::WrongLength,
        "long packet rejected");

  auto badMagic = wire;
  badMagic[0] = 'X';
  expectDecodeStatus(badMagic, encounter::Status::BadMagic,
                     "unknown magic rejected");
  auto badVersion = wire;
  badVersion[2] = 2;
  expectDecodeStatus(badVersion, encounter::Status::UnsupportedVersion,
                     "unknown version rejected");
  auto badType = wire;
  badType[3] = 3;
  expectDecodeStatus(badType, encounter::Status::UnsupportedType,
                     "unknown packet type rejected");
  auto noUid = wire;
  noUid[4] = noUid[5] = 0;
  expectDecodeStatus(noUid, encounter::Status::InvalidUid,
                     "zero UID rejected with valid CRC");
  auto noPack = wire;
  noPack[6] = noPack[7] = noPack[8] = noPack[9] = 0;
  expectDecodeStatus(noPack, encounter::Status::InvalidPackId,
                     "zero pack ID rejected with valid CRC");
  auto badBond = wire;
  badBond[11] = 101;
  expectDecodeStatus(badBond, encounter::Status::InvalidBond,
                     "bond above 100 rejected with valid CRC");
  auto noNonce = wire;
  noNonce[13] = noNonce[14] = noNonce[15] = noNonce[16] = 0;
  expectDecodeStatus(noNonce, encounter::Status::InvalidNonce,
                     "zero nonce rejected with valid CRC");

  // A failed decode is transactional and leaves the caller's packet untouched.
  encounter::Packet sentinel = syntheticPacket();
  sentinel.uid = 0xAAAA;
  auto damaged = wire;
  damaged[8] ^= 0x80;
  check(encounter::decode(damaged.data(), damaged.size(), sentinel) ==
            encounter::Status::IntegrityMismatch,
        "payload damage reports integrity mismatch");
  check(sentinel.uid == 0xAAAA, "failed decode does not mutate output");

  std::array<uint8_t, encounter::kWireBytes> output{};
  size_t written = 77;
  check(encounter::encode(syntheticPacket(), nullptr, output.size(), &written) ==
            encounter::Status::NullArgument &&
            written == 0,
        "encoder rejects null output and clears count");
  check(encounter::encode(syntheticPacket(), output.data(), output.size() - 1,
                          &written) == encounter::Status::BufferTooSmall &&
            written == 0,
        "encoder rejects undersized output");

  encounter::Packet invalid = syntheticPacket();
  invalid.appearance = encounter::kMaxAppearance + 1;
  check(encounter::validate(invalid) == encounter::Status::InvalidAppearance,
        "appearance range validated");
  invalid = syntheticPacket();
  invalid.evolutionStage = encounter::kMaxEvolutionStage + 1;
  check(encounter::validate(invalid) ==
            encounter::Status::InvalidEvolutionStage,
        "evolution range validated");
  invalid = syntheticPacket();
  invalid.mood = encounter::kMaxMood + 1;
  check(encounter::validate(invalid) == encounter::Status::InvalidMood,
        "mood range validated");
  invalid = syntheticPacket();
  invalid.emote = encounter::kMaxEmote + 1;
  check(encounter::validate(invalid) == encounter::Status::InvalidEmote,
        "emote range validated");
  check(std::strcmp(encounter::statusName(encounter::Status::IntegrityMismatch),
                    "integrity_mismatch") == 0,
        "status names are stable");
}

void testSharedResultAndDedupe() {
  const encounter::Packet a = syntheticPacket();
  encounter::Packet b{};
  b.type = encounter::PacketType::Reply;
  b.uid = 0xBEEF;
  b.packId = 0x10203040;
  b.appearance = 3;
  b.evolutionStage = 1;
  b.bond = 42;
  b.mood = 2;
  b.emote = 7;
  b.nonce = 0x89ABCDEF;

  const encounter::SharedResult ab = encounter::deriveSharedResult(a, b);
  const encounter::SharedResult ba = encounter::deriveSharedResult(b, a);
  check(ab.seed == 0x06893892 && ab.trait == 2 && ab.gift == 4,
        "shared result matches golden seed/trait/gift");
  check(ab.seed == ba.seed && ab.trait == ba.trait && ab.gift == ba.gift,
        "shared result is argument-order independent");

  encounter::Packet changed = b;
  ++changed.nonce;
  check(encounter::deriveSharedResult(a, changed).seed != ab.seed,
        "changing a nonce changes the shared result");

  check(encounter::compactPeerHash(a) == 0x186E,
        "compact peer hash matches golden value");
  const encounter::DuplicateToken token = encounter::makeDuplicateToken(a);
  check(encounter::isDuplicate(token, a), "duplicate token recognizes replay");
  encounter::Packet nextEncounter = a;
  ++nextEncounter.nonce;
  check(!encounter::isDuplicate(token, nextEncounter),
        "new nonce is not classified as a duplicate");
  encounter::Packet sameDevice = a;
  sameDevice.packId ^= 0x01010101;
  check(encounter::samePeerExact(a, sameDevice),
        "exact peer identity is independent of companion pack");
  check(!encounter::samePeerExact(a, b), "different UID is a different peer");
}

}  // namespace

int main() {
  testGoldenVector();
  testStrictParserAndValidation();
  testSharedResultAndDedupe();

  if (failures != 0) {
    std::cerr << "TEST_FAIL encounter_protocol failures=" << failures << '\n';
    return 1;
  }
  std::cout << "TEST_PASS encounter_protocol wire_bytes="
            << encounter::kWireBytes
            << " golden_crc=FA15 corruption_bytes=" << encounter::kWireBytes
            << " shared_seed=06893892 trait=2 gift=4 peer_hash=186E\n";
  return 0;
}
