#include "../src/kitsu_ble_action.h"

#include <assert.h>
#include <string.h>

#include <string>
#include <vector>

using kitsu868::connectivity::BleActionCommand;
using kitsu868::connectivity::BleActionDecodeResult;
using kitsu868::connectivity::BleActionKind;
using kitsu868::connectivity::BleActionReplayCache;
using kitsu868::connectivity::BleActionReplayDecision;
using kitsu868::connectivity::BleMessageRoute;

namespace {

constexpr char kId[] = "00112233-4455-6677-8899-aabbccddeeff";
constexpr char kPeerKey[] =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
constexpr uint32_t kNow = 1800000000UL;
constexpr uint32_t kDefaultDeadline = kNow + 30UL;

BleActionDecodeResult decode(const std::string& json,
                             BleActionCommand& output) {
  return kitsu868::connectivity::decodeBleActionCommand(
      reinterpret_cast<const uint8_t*>(json.data()), json.size(), output);
}

std::string body(const char* kind, const char* params = "{}",
                 const char* expiry = "1800000030", const char* id = kId) {
  return std::string("{\"action_id\":\"") + id +
      "\",\"kind\":\"" + kind + "\",\"expires_at_epoch\":" + expiry +
      ",\"params\":" + params + "}";
}

std::string messageParams(const char* route, const char* target,
                          const char* textJson) {
  return std::string("{\"route\":\"") + route +
      "\",\"target_id\":\"" + target + "\",\"text\":\"" +
      textJson + "\"}";
}

void expect(const std::string& json, BleActionDecodeResult expected) {
  BleActionCommand command{};
  assert(decode(json, command) == expected);
}

void testSafeActionContract() {
  BleActionCommand command{};
  assert(decode(body("pet"), command) == BleActionDecodeResult::Ok);
  assert(command.kind == BleActionKind::Pet);
  assert(command.expiresAtEpoch == kDefaultDeadline &&
         command.durationMs == 0U);
  assert(command.actionIdValid && strcmp(command.actionIdText, kId) == 0);
  assert(kitsu868::connectivity::bleActionKindAvailable(command.kind));

  assert(decode(body("feed"), command) == BleActionDecodeResult::Ok);
  assert(command.kind == BleActionKind::Feed);
  assert(decode(body("play"), command) == BleActionDecodeResult::Ok);
  assert(command.kind == BleActionKind::Play);
  assert(decode(body("listen_once", "{\"duration_ms\":60000}"),
                command) == BleActionDecodeResult::Ok);
  assert(command.kind == BleActionKind::ListenOnce);
  assert(command.durationMs == 60000U);

  // Field order and harmless JSON whitespace are not part of the schema.
  const std::string reordered =
      " { \"params\" : { \"duration_ms\" : 1000 },"
      "\"expires_at_epoch\":1800000001,\"kind\":\"listen_once\","
      "\"action_id\":\"00112233-4455-6677-8899-AABBCCDDEEFF\" } ";
  assert(decode(reordered, command) == BleActionDecodeResult::Ok);
  assert(command.durationMs == 1000U);
  assert(strcmp(command.actionIdText, kId) == 0);

  // UUID.fromString/UUID(uuidString:) both permit the nil UUID.
  assert(decode(body("pet", "{}", "1800000120",
                     "00000000-0000-0000-0000-000000000000"),
                command) == BleActionDecodeResult::Ok);
}

void testStrictOuterSchemaAndBounds() {
  expect("{}", BleActionDecodeResult::UnknownField);
  expect(std::string("{\"action_id\":\"") + kId +
             "\",\"kind\":\"pet\",\"expires_in_ms\":30000,"
             "\"params\":{}}",
         BleActionDecodeResult::UnknownField);
  expect(body("pet", "{}", "0"), BleActionDecodeResult::InvalidExpiry);
  expect(body("pet", "{}", "1704067199"),
         BleActionDecodeResult::InvalidExpiry);
  expect(body("pet", "{}", "4102444801"),
         BleActionDecodeResult::InvalidExpiry);
  expect(body("pet", "{}", "-1"), BleActionDecodeResult::InvalidExpiry);
  expect(body("pet", "{}", "1.0"),
         BleActionDecodeResult::MalformedJson);
  expect(body("p\\u0065t"), BleActionDecodeResult::InvalidKind);
  expect(body("unknown"), BleActionDecodeResult::InvalidKind);
  expect(body("pet", "{}", "30000", "not-a-uuid"),
         BleActionDecodeResult::InvalidActionId);

  const std::string duplicate =
      std::string("{\"action_id\":\"") + kId +
      "\",\"action_id\":\"" + kId +
      "\",\"kind\":\"pet\",\"expires_at_epoch\":1800000001,"
      "\"params\":{}}";
  expect(duplicate, BleActionDecodeResult::DuplicateField);
  expect(body("pet").insert(body("pet").size() - 1U, ",\"extra\":1"),
         BleActionDecodeResult::MalformedJson);

  std::string invalidUtf8 = body("pet");
  invalidUtf8.insert(invalidUtf8.size() - 1U, 1U, static_cast<char>(0xc0));
  expect(invalidUtf8, BleActionDecodeResult::MalformedJson);
}

void testExactKindParams() {
  expect(body("pet", "{\"duration_ms\":1000}"),
         BleActionDecodeResult::InvalidParams);
  expect(body("listen_once", "{}"), BleActionDecodeResult::InvalidParams);
  expect(body("listen_once", "{\"duration_ms\":999}"),
         BleActionDecodeResult::InvalidParams);
  expect(body("listen_once", "{\"duration_ms\":60001}"),
         BleActionDecodeResult::InvalidParams);
  expect(body("listen_once",
              "{\"duration_ms\":1000,\"duration_ms\":1000}"),
         BleActionDecodeResult::DuplicateField);
  expect(body("listen_once", "{\"duration_ms\":1000,\"x\":1}"),
         BleActionDecodeResult::InvalidParams);
}

void testTxActionsAndUnavailableActionsValidate() {
  BleActionCommand command{};
  assert(decode(body("advertise_once", "{\"scope\":\"nearby\"}"),
                command) == BleActionDecodeResult::Ok);
  assert(command.kind == BleActionKind::AdvertiseOnce);
  assert(!kitsu868::connectivity::bleActionKindAvailable(command.kind));
  expect(body("advertise_once", "{\"scope\":\"world\"}"),
         BleActionDecodeResult::InvalidParams);

  const std::string directHello =
      messageParams("direct", kPeerKey, "hello \\u263a");
  assert(decode(body("send_message", directHello.c_str()),
                command) == BleActionDecodeResult::Ok);
  assert(command.kind == BleActionKind::SendMessage);
  assert(kitsu868::connectivity::bleActionKindAvailable(command.kind));
  assert(command.messageRoute == BleMessageRoute::Direct);
  assert(strcmp(command.messageTarget, kPeerKey) == 0);
  assert(command.messageTargetBytes == 43U);
  assert(strcmp(command.messageText, "hello \xe2\x98\xba") == 0);
  assert(command.messageTextBytes == 9U);
  assert(command.durationMs == 0U);

  assert(decode(body("send_message",
                     "{\"route\":\"channel\",\"target_id\":\"0\"," 
                     "\"text\":\"Public hello\"}"),
                command) == BleActionDecodeResult::Ok);
  assert(command.messageRoute == BleMessageRoute::Channel);
  assert(strcmp(command.messageTarget, "0") == 0);
  assert(strcmp(command.messageText, "Public hello") == 0);
  expect(body("send_message",
              "{\"route\":\"direct\",\"target_id\":\"\"," 
              "\"text\":\"hello\"}"),
         BleActionDecodeResult::InvalidParams);
  expect(body("send_message",
              "{\"route\":\"direct\",\"target_id\":\"peer-1\"," 
              "\"text\":\"hello\"}"),
         BleActionDecodeResult::InvalidParams);
  // Width/alphabet alone are insufficient for unpadded base64url. A 32-byte
  // key has four discarded low bits in its final character; nonzero values
  // must not create alternate spellings of the same identity/replay binding.
  std::string noncanonicalPeerKey = kPeerKey;
  noncanonicalPeerKey.back() = 'B';
  const std::string noncanonicalTarget =
      messageParams("direct", noncanonicalPeerKey.c_str(), "hello");
  expect(body("send_message", noncanonicalTarget.c_str()),
         BleActionDecodeResult::InvalidParams);
  std::string canonicalNonzeroTail = kPeerKey;
  canonicalNonzeroTail.back() = 'Q';
  const std::string canonicalNonzeroTarget =
      messageParams("direct", canonicalNonzeroTail.c_str(), "hello");
  assert(decode(body("send_message", canonicalNonzeroTarget.c_str()),
                command) == BleActionDecodeResult::Ok);
  assert(strcmp(command.messageTarget, canonicalNonzeroTail.c_str()) == 0);
  expect(body("send_message",
              "{\"route\":\"channel\",\"target_id\":\"04\"," 
              "\"text\":\"hello\"}"),
         BleActionDecodeResult::InvalidParams);
  expect(body("send_message",
              "{\"route\":\"channel\",\"target_id\":\"4\"," 
              "\"text\":\"hello\"}"),
         BleActionDecodeResult::InvalidParams);
  const std::string nulMessage =
      messageParams("direct", kPeerKey, "\\u0000");
  expect(body("send_message", nulMessage.c_str()),
         BleActionDecodeResult::InvalidParams);
  const std::string longText(129U, 'x');
  expect(body("send_message",
              (std::string("{\"route\":\"channel\",\"target_id\":\"0\"," 
                           "\"text\":\"") + longText + "\"}").c_str()),
         BleActionDecodeResult::InvalidParams);

  assert(decode(body("share_location_once",
                     "{\"lat_e6\":-90000000,\"lon_e6\":180000000,"
                     "\"exposure\":\"map_card\"}"),
                command) == BleActionDecodeResult::Ok);
  assert(command.kind == BleActionKind::ShareLocationOnce);
  assert(!kitsu868::connectivity::bleActionKindAvailable(command.kind));
  expect(body("share_location_once",
              "{\"lat_e6\":-90000001,\"lon_e6\":0,"
              "\"exposure\":\"map_card\"}"),
         BleActionDecodeResult::InvalidParams);
}

void testDirectReceipt() {
  BleActionCommand command{};
  assert(decode(body("pet"), command) == BleActionDecodeResult::Ok);
  uint8_t output[256]{};
  size_t outputBytes = 0U;
  assert(kitsu868::connectivity::encodeBleActionReceipt(
      command, true, "applied", nullptr, output, sizeof(output),
      outputBytes));
  assert(std::string(reinterpret_cast<char*>(output), outputBytes) ==
         "{\"action_id\":\"00112233-4455-6677-8899-aabbccddeeff\","
         "\"accepted\":true,\"state\":\"applied\"}");

  assert(kitsu868::connectivity::encodeBleActionReceipt(
      command, false, "rejected", "action_unavailable", output,
      sizeof(output), outputBytes));
  assert(std::string(reinterpret_cast<char*>(output), outputBytes) ==
         "{\"action_id\":\"00112233-4455-6677-8899-aabbccddeeff\","
         "\"accepted\":false,\"state\":\"rejected\","
         "\"error_code\":\"action_unavailable\"}");
  assert(!kitsu868::connectivity::encodeBleActionReceipt(
      command, true, "applied", "error", output, sizeof(output),
      outputBytes));
}

BleActionCommand commandFor(unsigned ordinal, BleActionKind kind,
                            uint32_t deadline = kDefaultDeadline,
                            uint32_t duration = 0U) {
  char id[37]{};
  snprintf(id, sizeof(id), "00112233-4455-6677-8899-%012x", ordinal);
  char deadlineText[16]{};
  snprintf(deadlineText, sizeof(deadlineText), "%lu",
           static_cast<unsigned long>(deadline));
  BleActionCommand command{};
  char params[40] = "{}";
  if (kind == BleActionKind::ListenOnce) {
    const uint32_t listenDuration = duration == 0U ? 60000U : duration;
    snprintf(params, sizeof(params), "{\"duration_ms\":%lu}",
             static_cast<unsigned long>(listenDuration));
  } else {
    assert(duration == 0U);
  }
  const char* name = kitsu868::connectivity::bleActionKindName(kind);
  assert(decode(body(name, params, deadlineText, id), command) ==
         BleActionDecodeResult::Ok);
  return command;
}

uint32_t testCrc32(const uint8_t* input, size_t bytes) {
  uint32_t crc = 0xffffffffUL;
  for (size_t index = 0U; index < bytes; ++index) {
    crc ^= input[index];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = static_cast<uint32_t>(
          -static_cast<int32_t>(crc & 1U));
      crc = (crc >> 1U) ^ (0xedb88320UL & mask);
    }
  }
  return ~crc;
}

void reseal(std::vector<uint8_t>& blob) {
  assert(blob.size() >= sizeof(uint32_t));
  const uint32_t checksum = testCrc32(
      blob.data(), blob.size() - sizeof(checksum));
  memcpy(blob.data() + blob.size() - sizeof(checksum), &checksum,
         sizeof(checksum));
}

bool containsBytes(const std::vector<uint8_t>& haystack,
                   const char* needle, size_t needleBytes) {
  if (!needle || needleBytes == 0U || needleBytes > haystack.size()) {
    return false;
  }
  for (size_t offset = 0U;
       offset + needleBytes <= haystack.size(); ++offset) {
    if (memcmp(haystack.data() + offset, needle, needleBytes) == 0) {
      return true;
    }
  }
  return false;
}

void testCommandDigestAndNoPlaintextPersistence() {
  BleActionCommand pet = commandFor(1U, BleActionKind::Pet);
  uint8_t digest[kitsu868::connectivity::kBleActionCommandDigestBytes]{};
  assert(kitsu868::connectivity::bleActionCommandDigest(pet, digest));
  static const uint8_t expectedDigest[] = {
      0x7aU, 0x4cU, 0x89U, 0xc0U, 0xdbU, 0xd2U, 0x2cU, 0x81U,
      0x14U, 0xc0U, 0xe2U, 0x02U, 0x0cU, 0x20U, 0xb8U, 0xd4U,
      0xdfU, 0xbbU, 0x0fU, 0xa9U, 0x62U, 0x2fU, 0x3eU, 0x51U,
      0xdcU, 0xb7U, 0x7bU, 0xc0U, 0xdcU, 0x58U, 0xb0U, 0x59U,
  };
  static_assert(sizeof(expectedDigest) == sizeof(digest),
                "SHA-256 width changed");
  assert(memcmp(digest, expectedDigest, sizeof(digest)) == 0);

  const char privateText[] = "private fox rendezvous at the old oak";
  const std::string params =
      messageParams("direct", kPeerKey, privateText);
  BleActionCommand message{};
  assert(decode(body("send_message", params.c_str()), message) ==
         BleActionDecodeResult::Ok);
  uint8_t messageDigest[
      kitsu868::connectivity::kBleActionCommandDigestBytes]{};
  assert(kitsu868::connectivity::bleActionCommandDigest(
      message, messageDigest));
  static const uint8_t expectedMessageDigest[] = {
      0x33U, 0xc2U, 0x85U, 0x87U, 0x6fU, 0x22U, 0x71U, 0x59U,
      0xd0U, 0x86U, 0xb2U, 0x30U, 0x4aU, 0x5bU, 0xfdU, 0x8fU,
      0x66U, 0x7aU, 0x3dU, 0x7dU, 0xd2U, 0x51U, 0xbcU, 0x16U,
      0xfcU, 0xe9U, 0xf1U, 0x40U, 0xa9U, 0x28U, 0xf6U, 0x54U,
  };
  assert(memcmp(messageDigest, expectedMessageDigest,
                sizeof(messageDigest)) == 0);
  BleActionReplayCache cache;
  assert(cache.remember(message, kNow));
  size_t serializedBytes = 0U;
  const uint8_t* serialized = cache.serialized(serializedBytes);
  assert(serializedBytes ==
         kitsu868::connectivity::kBleActionReplaySerializedBytes);
  assert(serializedBytes == 436U);
  assert(serializedBytes <= 512U);
  std::vector<uint8_t> snapshot(serialized, serialized + serializedBytes);
  assert(!containsBytes(snapshot, message.messageTarget,
                        message.messageTargetBytes));
  assert(!containsBytes(snapshot, message.messageText,
                        message.messageTextBytes));
}

void testExpiryAndTrustedClockContract() {
  BleActionReplayCache cache;
  BleActionCommand command = commandFor(
      1U, BleActionKind::Pet, kNow +
      kitsu868::connectivity::kBleActionMaximumExpirySeconds);
  assert(cache.inspect(command, 0U) ==
         BleActionReplayDecision::TimeUnavailable);
  assert(cache.inspect(command,
                       kitsu868::connectivity::kBleActionMinimumTrustedEpoch -
                           1U) ==
         BleActionReplayDecision::TimeUnavailable);
  assert(cache.inspect(command, kNow) == BleActionReplayDecision::Fresh);
  assert(cache.remember(command, kNow));
  assert(cache.inspect(command, kNow + 119U) ==
         BleActionReplayDecision::DuplicateIndeterminate);
  assert(cache.inspect(command, command.expiresAtEpoch) ==
         BleActionReplayDecision::Expired);

  BleActionCommand expired = commandFor(
      2U, BleActionKind::Pet, kNow);
  assert(cache.inspect(expired, kNow) ==
         BleActionReplayDecision::Expired);
  assert(!cache.remember(expired, kNow));

  BleActionCommand tooFar = commandFor(
      3U, BleActionKind::Pet,
      kNow + kitsu868::connectivity::kBleActionMaximumExpirySeconds + 1U);
  assert(cache.inspect(tooFar, kNow) ==
         BleActionReplayDecision::InvalidExpiry);
  assert(!cache.remember(tooFar, kNow));
  assert(!cache.remember(command, 0U));
}

void testPersistentPendingAppliedAndConflicts() {
  BleActionReplayCache cache;
  BleActionCommand first = commandFor(1U, BleActionKind::Pet);
  assert(cache.inspect(first, kNow) == BleActionReplayDecision::Fresh);
  assert(cache.remember(first, kNow));
  assert(cache.inspect(first, kNow) ==
         BleActionReplayDecision::DuplicateIndeterminate);

  size_t pendingBytes = 0U;
  const uint8_t* pending = cache.serialized(pendingBytes);
  std::vector<uint8_t> pendingSnapshot(pending, pending + pendingBytes);
  BleActionReplayCache restoredPending;
  assert(restoredPending.load(pendingSnapshot.data(), pendingSnapshot.size()));
  assert(restoredPending.inspect(first, 0U) ==
         BleActionReplayDecision::TimeUnavailable);
  assert(restoredPending.inspect(first, kNow + 1U) ==
         BleActionReplayDecision::DuplicateIndeterminate);
  assert(cache.markApplied(first));
  assert(cache.inspect(first, kNow) ==
         BleActionReplayDecision::DuplicateApplied);

  BleActionCommand conflict = first;
  conflict.kind = BleActionKind::Feed;
  assert(cache.inspect(conflict, kNow) ==
         BleActionReplayDecision::Conflict);
  conflict = first;
  ++conflict.expiresAtEpoch;
  assert(cache.inspect(conflict, kNow) ==
         BleActionReplayDecision::Conflict);

  BleActionCommand message{};
  const std::string firstParams = messageParams("direct", kPeerKey, "first");
  assert(decode(body("send_message", firstParams.c_str()),
                message) == BleActionDecodeResult::Ok);
  assert(cache.remember(message, kNow));
  BleActionCommand changedMessage{};
  const std::string secondParams =
      messageParams("direct", kPeerKey, "second");
  assert(decode(body("send_message", secondParams.c_str()),
                changedMessage) == BleActionDecodeResult::Ok);
  assert(cache.inspect(changedMessage, kNow) ==
         BleActionReplayDecision::Conflict);

  size_t serializedBytes = 0U;
  const uint8_t* serialized = cache.serialized(serializedBytes);
  std::vector<uint8_t> snapshot(serialized, serialized + serializedBytes);
  BleActionReplayCache restored;
  assert(restored.load(snapshot.data(), snapshot.size()));
  assert(restored.inspect(first, kNow) ==
         BleActionReplayDecision::DuplicateApplied);

  // Crossing the persisted deadline after a power loss makes the original
  // frame non-executable rather than refreshing its old relative TTL.
  BleActionReplayCache restoredAfterExpiry;
  assert(restoredAfterExpiry.load(pendingSnapshot.data(),
                                   pendingSnapshot.size()));
  assert(restoredAfterExpiry.inspect(first, first.expiresAtEpoch) ==
         BleActionReplayDecision::Expired);
}

void testCapacityFailsClosedAndReclaimsOnlyExpired() {
  BleActionReplayCache cache;
  for (unsigned ordinal = 1U;
       ordinal <= kitsu868::connectivity::kBleActionReplayCapacity;
       ++ordinal) {
    const uint32_t deadline = ordinal == 1U ? kNow + 5U : kNow + 100U;
    assert(cache.remember(
        commandFor(ordinal, BleActionKind::Pet, deadline), kNow));
  }
  BleActionCommand overflow = commandFor(
      99U, BleActionKind::Pet, kNow + 90U);
  assert(!cache.remember(overflow, kNow));
  assert(cache.inspect(overflow, kNow) == BleActionReplayDecision::Fresh);
  for (unsigned ordinal = 1U;
       ordinal <= kitsu868::connectivity::kBleActionReplayCapacity;
       ++ordinal) {
    const uint32_t deadline = ordinal == 1U ? kNow + 5U : kNow + 100U;
    assert(cache.inspect(
        commandFor(ordinal, BleActionKind::Pet, deadline), kNow) ==
        BleActionReplayDecision::DuplicateIndeterminate);
  }

  // Exactly at expiry the first reservation may be reclaimed, while every
  // still-protected reservation remains present.
  assert(cache.remember(overflow, kNow + 5U));
  assert(cache.inspect(overflow, kNow + 5U) ==
         BleActionReplayDecision::DuplicateIndeterminate);
  assert(cache.inspect(commandFor(1U, BleActionKind::Pet, kNow + 5U),
                       kNow + 5U) == BleActionReplayDecision::Expired);
  for (unsigned ordinal = 2U;
       ordinal <= kitsu868::connectivity::kBleActionReplayCapacity;
       ++ordinal) {
    assert(cache.inspect(
        commandFor(ordinal, BleActionKind::Pet, kNow + 100U), kNow + 5U) ==
        BleActionReplayDecision::DuplicateIndeterminate);
  }

  // If more than one slot is expired and a UUID is deliberately reused, its
  // own old slot is replaced. This preserves the load-time no-duplicate-ID
  // invariant instead of leaving two records for one UUID.
  BleActionReplayCache reuse;
  assert(reuse.remember(
      commandFor(1U, BleActionKind::Pet, kNow + 1U), kNow));
  assert(reuse.remember(
      commandFor(2U, BleActionKind::Pet, kNow + 2U), kNow));
  BleActionCommand reusedId = commandFor(
      2U, BleActionKind::Feed, kNow + 30U);
  assert(reuse.remember(reusedId, kNow + 2U));
  size_t reuseBytes = 0U;
  const uint8_t* reuseSerialized = reuse.serialized(reuseBytes);
  std::vector<uint8_t> reuseSnapshot(
      reuseSerialized, reuseSerialized + reuseBytes);
  BleActionReplayCache reuseRestored;
  assert(reuseRestored.load(reuseSnapshot.data(), reuseSnapshot.size()));
  assert(reuseRestored.inspect(reusedId, kNow + 2U) ==
         BleActionReplayDecision::DuplicateIndeterminate);
}

void testCorruptionIsRejected() {
  BleActionReplayCache cache;
  BleActionCommand first = commandFor(1U, BleActionKind::Pet);
  assert(cache.remember(first, kNow));
  size_t serializedBytes = 0U;
  const uint8_t* serialized = cache.serialized(serializedBytes);
  std::vector<uint8_t> valid(serialized, serialized + serializedBytes);

  std::vector<uint8_t> badCrc = valid;
  badCrc[12] ^= 0x40U;
  BleActionReplayCache restored;
  assert(!restored.load(badCrc.data(), badCrc.size()));

  // Header reserved byte is structural, not merely CRC protected.
  std::vector<uint8_t> badReserved = valid;
  badReserved[7] = 1U;
  reseal(badReserved);
  assert(!restored.load(badReserved.data(), badReserved.size()));

  // An occupied record with an unknown outcome is rejected even when an
  // attacker or torn-write simulator recomputes the CRC.
  constexpr size_t kHeaderBytes = 8U;
  constexpr size_t kRecordBytes = 16U + 32U + 4U + 1U;
  constexpr size_t kOutcomeOffset = kHeaderBytes + 16U + 32U + 4U;
  static_assert(kRecordBytes == 53U, "packed replay record changed");
  std::vector<uint8_t> badOutcome = valid;
  badOutcome[kOutcomeOffset] = 3U;
  reseal(badOutcome);
  assert(!restored.load(badOutcome.data(), badOutcome.size()));

  // Duplicate occupied action IDs are impossible through the API and are
  // rejected on load even with a valid CRC.
  BleActionCommand second = commandFor(2U, BleActionKind::Pet);
  assert(cache.remember(second, kNow));
  serialized = cache.serialized(serializedBytes);
  std::vector<uint8_t> duplicate(serialized, serialized + serializedBytes);
  memcpy(duplicate.data() + kHeaderBytes + kRecordBytes,
         duplicate.data() + kHeaderBytes, kRecordBytes);
  reseal(duplicate);
  assert(!restored.load(duplicate.data(), duplicate.size()));

  assert(!restored.load(valid.data(), valid.size() - 1U));
}

}  // namespace

int main() {
  testSafeActionContract();
  testStrictOuterSchemaAndBounds();
  testExactKindParams();
  testTxActionsAndUnavailableActionsValidate();
  testDirectReceipt();
  testCommandDigestAndNoPlaintextPersistence();
  testExpiryAndTrustedClockContract();
  testPersistentPendingAppliedAndConflicts();
  testCapacityFailsClosedAndReclaimsOnlyExpired();
  testCorruptionIsRejected();
  return 0;
}
