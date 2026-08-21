#include "kitsu_lan_protocol.h"

#include <stdio.h>
#include <string.h>

namespace kitsu868 {
namespace connectivity {
namespace {

constexpr size_t kMaximumFields = 10U;
constexpr char kActionDomain[] = "KITSU-ACTION-1\0";
constexpr char kDeviceDomain[] = "KITSU-DEVICE-1\0";
static_assert(sizeof(kActionDomain) - 1U == 15U,
              "action domain includes its trailing NUL");
static_assert(sizeof(kDeviceDomain) - 1U == 15U,
              "device domain includes its trailing NUL");

void secureZero(void* memory, size_t bytes) {
  volatile uint8_t* output = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *output++ = 0U;
}

bool constantTimeEqual(const uint8_t* left, const uint8_t* right,
                       size_t bytes) {
  uint8_t difference = 0U;
  for (size_t i = 0U; i < bytes; ++i) difference |= left[i] ^ right[i];
  return difference == 0U;
}

bool allZero(const uint8_t* input, size_t bytes) {
  uint8_t combined = 0U;
  for (size_t i = 0U; i < bytes; ++i) combined |= input[i];
  return combined == 0U;
}

void putU16Be(uint8_t output[2], uint16_t value) {
  output[0] = static_cast<uint8_t>(value >> 8U);
  output[1] = static_cast<uint8_t>(value);
}

void putU32Be(uint8_t output[4], uint32_t value) {
  output[0] = static_cast<uint8_t>(value >> 24U);
  output[1] = static_cast<uint8_t>(value >> 16U);
  output[2] = static_cast<uint8_t>(value >> 8U);
  output[3] = static_cast<uint8_t>(value);
}

void putU64Be(uint8_t output[8], uint64_t value) {
  for (size_t i = 0U; i < 8U; ++i) {
    output[i] = static_cast<uint8_t>(value >> ((7U - i) * 8U));
  }
}

struct Span {
  constexpr Span(const uint8_t* input = nullptr, size_t inputBytes = 0U)
      : data(input), bytes(inputBytes) {}

  const uint8_t* data = nullptr;
  size_t bytes = 0U;
};

enum class ValueKind : uint8_t { String = 0, Unsigned };

struct Field {
  Span key{};
  Span value{};
  ValueKind kind = ValueKind::String;
};

enum class ParseResult : uint8_t { Ok = 0, Malformed, Duplicate };

void skipWhitespace(const uint8_t* json, size_t bytes, size_t& cursor) {
  while (cursor < bytes &&
         (json[cursor] == ' ' || json[cursor] == '\t' ||
          json[cursor] == '\r' || json[cursor] == '\n')) {
    ++cursor;
  }
}

bool parseCanonicalString(const uint8_t* json, size_t bytes, size_t& cursor,
                          Span& output) {
  if (cursor >= bytes || json[cursor++] != '"') return false;
  const size_t start = cursor;
  while (cursor < bytes) {
    const uint8_t value = json[cursor++];
    if (value == '"') {
      output.data = json + start;
      output.bytes = cursor - start - 1U;
      return true;
    }
    // Frozen LAN wrappers use canonical ASCII tokens. Escaped spellings are
    // rejected so duplicate-field and MAC-bound values have one spelling.
    if (value < 0x20U || value == '\\' || value >= 0x80U) return false;
  }
  return false;
}

bool spanEqual(const Span& left, const Span& right) {
  return left.bytes == right.bytes &&
         memcmp(left.data, right.data, left.bytes) == 0;
}

bool spanEquals(const Span& span, const char* expected) {
  const size_t expectedBytes = expected ? strlen(expected) : 0U;
  return span.bytes == expectedBytes &&
         memcmp(span.data, expected, expectedBytes) == 0;
}

ParseResult parseObject(const uint8_t* json, size_t jsonBytes,
                        Field fields[kMaximumFields], size_t& fieldCount) {
  fieldCount = 0U;
  if (!json || jsonBytes == 0U || jsonBytes > kLanMaximumFrameBytes ||
      !companion::validUtf8(json, jsonBytes)) {
    return ParseResult::Malformed;
  }
  size_t cursor = 0U;
  skipWhitespace(json, jsonBytes, cursor);
  if (cursor >= jsonBytes || json[cursor++] != '{') {
    return ParseResult::Malformed;
  }
  skipWhitespace(json, jsonBytes, cursor);
  if (cursor < jsonBytes && json[cursor] == '}') {
    return ParseResult::Malformed;
  }
  while (cursor < jsonBytes) {
    if (fieldCount >= kMaximumFields) return ParseResult::Malformed;
    Field& field = fields[fieldCount];
    if (!parseCanonicalString(json, jsonBytes, cursor, field.key) ||
        field.key.bytes == 0U) {
      return ParseResult::Malformed;
    }
    for (size_t i = 0U; i < fieldCount; ++i) {
      if (spanEqual(fields[i].key, field.key)) return ParseResult::Duplicate;
    }
    skipWhitespace(json, jsonBytes, cursor);
    if (cursor >= jsonBytes || json[cursor++] != ':') {
      return ParseResult::Malformed;
    }
    skipWhitespace(json, jsonBytes, cursor);
    if (cursor < jsonBytes && json[cursor] == '"') {
      field.kind = ValueKind::String;
      if (!parseCanonicalString(json, jsonBytes, cursor, field.value)) {
        return ParseResult::Malformed;
      }
    } else {
      field.kind = ValueKind::Unsigned;
      const size_t start = cursor;
      while (cursor < jsonBytes && json[cursor] >= '0' &&
             json[cursor] <= '9') {
        ++cursor;
      }
      if (cursor == start ||
          (cursor - start > 1U && json[start] == '0')) {
        return ParseResult::Malformed;
      }
      field.value.data = json + start;
      field.value.bytes = cursor - start;
    }
    ++fieldCount;
    skipWhitespace(json, jsonBytes, cursor);
    if (cursor >= jsonBytes) return ParseResult::Malformed;
    if (json[cursor] == '}') {
      ++cursor;
      break;
    }
    if (json[cursor++] != ',') return ParseResult::Malformed;
    skipWhitespace(json, jsonBytes, cursor);
  }
  skipWhitespace(json, jsonBytes, cursor);
  return cursor == jsonBytes ? ParseResult::Ok : ParseResult::Malformed;
}

const Field* findField(const Field* fields, size_t count, const char* name) {
  for (size_t i = 0U; i < count; ++i) {
    if (spanEquals(fields[i].key, name)) return fields + i;
  }
  return nullptr;
}

bool exactSchema(const Field* fields, size_t count,
                 const char* const* names, size_t nameCount) {
  if (count != nameCount) return false;
  for (size_t i = 0U; i < nameCount; ++i) {
    if (!findField(fields, count, names[i])) return false;
  }
  return true;
}

bool parseU64(const Span& value, uint64_t& output) {
  if (!value.data || value.bytes == 0U || value.bytes > 19U ||
      (value.bytes > 1U && value.data[0] == '0')) {
    return false;
  }
  uint64_t parsed = 0U;
  for (size_t i = 0U; i < value.bytes; ++i) {
    const uint8_t digit = value.data[i];
    if (digit < '0' || digit > '9') return false;
    const uint8_t number = static_cast<uint8_t>(digit - '0');
    if (parsed > (0x7fffffffffffffffULL - number) / 10U) return false;
    parsed = parsed * 10U + number;
  }
  output = parsed;
  return true;
}

bool parseU32(const Field* field, uint32_t& output) {
  if (!field || field->kind != ValueKind::Unsigned ||
      field->value.bytes == 0U || field->value.bytes > 10U) {
    return false;
  }
  uint64_t value = 0U;
  if (!parseU64(field->value, value) || value > 0xffffffffULL) return false;
  output = static_cast<uint32_t>(value);
  return true;
}

int hexNibble(uint8_t value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return -1;
}

bool parseUuid(const Span& value, uint8_t output[kLanUuidBytes]) {
  if (!value.data || value.bytes != 36U) return false;
  size_t source = 0U;
  size_t target = 0U;
  while (source < value.bytes) {
    if (source == 8U || source == 13U || source == 18U || source == 23U) {
      if (value.data[source++] != '-') return false;
      continue;
    }
    if (source + 1U >= value.bytes || target >= kLanUuidBytes) return false;
    const int high = hexNibble(value.data[source++]);
    const int low = hexNibble(value.data[source++]);
    if (high < 0 || low < 0) return false;
    output[target++] = static_cast<uint8_t>((high << 4U) | low);
  }
  return target == kLanUuidBytes && !allZero(output, kLanUuidBytes);
}

bool decodeExact(const Field* field, uint8_t* output,
                 size_t expectedBytes) {
  if (!field || field->kind != ValueKind::String || !output) return false;
  size_t decoded = 0U;
  return companion::decodeBase64Url(
             reinterpret_cast<const char*>(field->value.data),
             field->value.bytes, output, expectedBytes, decoded) &&
         decoded == expectedBytes;
}

bool validProtocolName(const Span& value, size_t maximumBytes) {
  if (!value.data || value.bytes == 0U || value.bytes > maximumBytes ||
      value.data[0] < 'a' || value.data[0] > 'z') {
    return false;
  }
  for (size_t i = 1U; i < value.bytes; ++i) {
    const uint8_t byte = value.data[i];
    if (!((byte >= 'a' && byte <= 'z') ||
          (byte >= '0' && byte <= '9') || byte == '_' || byte == '.' ||
          byte == '-')) {
      return false;
    }
  }
  return true;
}

bool supportedAction(const Span& value) {
  static const char* const actions[] = {
      "companion.pet",         "companion.feed", "companion.play",
      "companion.listen_once", "sync.pull",      "clock.set",
      "mesh.introduce",        "message.send",
  };
  for (size_t i = 0U; i < sizeof(actions) / sizeof(actions[0]); ++i) {
    if (spanEquals(value, actions[i])) return true;
  }
  return false;
}

class JsonWriter {
 public:
  JsonWriter(uint8_t* output, size_t capacity)
      : output_(output), capacity_(capacity) {}

  bool literal(const char* input) {
    return input && bytes(reinterpret_cast<const uint8_t*>(input),
                          strlen(input));
  }

  bool bytes(const uint8_t* input, size_t inputBytes) {
    if (!ok_ || (!input && inputBytes != 0U) ||
        inputBytes > capacity_ - used_) {
      ok_ = false;
      return false;
    }
    if (inputBytes != 0U) memcpy(output_ + used_, input, inputBytes);
    used_ += inputBytes;
    return true;
  }

  bool base64(const uint8_t* input, size_t inputBytes) {
    if (!literal("\"")) return false;
    size_t encodedBytes = 0U;
    if (!companion::encodeBase64Url(
            input, inputBytes, reinterpret_cast<char*>(output_ + used_),
            capacity_ - used_, encodedBytes)) {
      ok_ = false;
      return false;
    }
    used_ += encodedBytes;
    return literal("\"");
  }

  bool ok() const { return ok_; }
  size_t used() const { return used_; }

 private:
  uint8_t* output_ = nullptr;
  size_t capacity_ = 0U;
  size_t used_ = 0U;
  bool ok_ = true;
};

void formatUuid(const uint8_t input[kLanUuidBytes], char output[37]) {
  static const char hex[] = "0123456789abcdef";
  size_t target = 0U;
  for (size_t source = 0U; source < kLanUuidBytes; ++source) {
    if (source == 4U || source == 6U || source == 8U || source == 10U) {
      output[target++] = '-';
    }
    output[target++] = hex[input[source] >> 4U];
    output[target++] = hex[input[source] & 0x0fU];
  }
  output[target] = '\0';
}

LanResult decodeAck(const Field* fields, size_t count,
                    LanGatewayFrame& output) {
  static const char* const schema[] = {
      "v", "type", "spool_record_id", "device_sequence"};
  if (!exactSchema(fields, count, schema,
                   sizeof(schema) / sizeof(schema[0]))) {
    return LanResult::UnknownField;
  }
  const Field* version = findField(fields, count, "v");
  const Field* type = findField(fields, count, "type");
  const Field* record = findField(fields, count, "spool_record_id");
  const Field* sequence = findField(fields, count, "device_sequence");
  uint64_t recordValue = 0U;
  uint64_t sequenceValue = 0U;
  if (!version || version->kind != ValueKind::Unsigned ||
      version->value.bytes != 1U || version->value.data[0] != '1' ||
      !type || type->kind != ValueKind::String ||
      !spanEquals(type->value, "gateway_ack") || !record ||
      record->kind != ValueKind::String || !sequence ||
      sequence->kind != ValueKind::String ||
      !parseU64(record->value, recordValue) || recordValue == 0U ||
      !parseU64(sequence->value, sequenceValue) || sequenceValue == 0U) {
    return LanResult::InvalidSequence;
  }
  output = LanGatewayFrame{};
  output.kind = LanFrameKind::GatewayAck;
  output.spoolRecordId = recordValue;
  output.deviceSequence = sequenceValue;
  return LanResult::GatewayAck;
}

}  // namespace

const char* lanResultName(LanResult result) {
  switch (result) {
    case LanResult::Ok: return "ok";
    case LanResult::GatewayAck: return "gateway_ack";
    case LanResult::ActionFresh: return "action_fresh";
    case LanResult::ActionDuplicate: return "action_duplicate";
    case LanResult::InvalidArgument: return "invalid_argument";
    case LanResult::MalformedJson: return "malformed_json";
    case LanResult::DuplicateField: return "duplicate_field";
    case LanResult::UnknownField: return "unknown_field";
    case LanResult::UnsupportedSchema: return "unsupported_schema";
    case LanResult::InvalidIdentity: return "invalid_identity";
    case LanResult::InvalidEncoding: return "invalid_encoding";
    case LanResult::InvalidSequence: return "invalid_sequence";
    case LanResult::WrongCompanion: return "wrong_companion";
    case LanResult::WrongKeyVersion: return "wrong_key_version";
    case LanResult::ClockRequired: return "clock_required";
    case LanResult::Expired: return "expired";
    case LanResult::AuthenticationFailed: return "authentication_failed";
    case LanResult::UnsupportedAction: return "unsupported_action";
    case LanResult::ReplayStoreFailed: return "replay_store_failed";
    case LanResult::OutputTooSmall: return "output_too_small";
    case LanResult::CryptoFailed: return "crypto_failed";
    default: return "unknown";
  }
}

LanResult decodeGatewayFrame(
    const uint8_t* json, size_t jsonBytes,
    const uint8_t expectedCompanionId[kLanUuidBytes],
    uint32_t expectedKeyVersion, int64_t nowEpoch, bool clockValid,
    const uint8_t backendHmacSecret[32], companion::CompanionCrypto& crypto,
    LanActionReplayStore& replayStore, LanGatewayFrame& output,
    uint8_t* paramsOutput, size_t paramsCapacity) {
  output = LanGatewayFrame{};
  if (!json || jsonBytes == 0U || !expectedCompanionId ||
      !backendHmacSecret || !paramsOutput ||
      allZero(expectedCompanionId, kLanUuidBytes) ||
      expectedKeyVersion == 0U) {
    return LanResult::InvalidArgument;
  }
  Field fields[kMaximumFields]{};
  size_t count = 0U;
  const ParseResult parsed = parseObject(json, jsonBytes, fields, count);
  if (parsed == ParseResult::Duplicate) return LanResult::DuplicateField;
  if (parsed != ParseResult::Ok) return LanResult::MalformedJson;
  const Field* type = findField(fields, count, "type");
  if (type) return decodeAck(fields, count, output);

  static const char* const schema[] = {
      "schema",       "action_id",     "companion_id", "key_version",
      "nonce_b64",    "action_type",   "created_epoch", "expires_epoch",
      "params_b64",   "signature_b64",
  };
  if (!exactSchema(fields, count, schema,
                   sizeof(schema) / sizeof(schema[0]))) {
    return LanResult::UnknownField;
  }
  const Field* schemaField = findField(fields, count, "schema");
  if (!schemaField || schemaField->kind != ValueKind::String ||
      !spanEquals(schemaField->value, "kitsu.remote-action.v1")) {
    return LanResult::UnsupportedSchema;
  }

  LanGatewayFrame candidate{};
  candidate.kind = LanFrameKind::RemoteAction;
  const Field* actionId = findField(fields, count, "action_id");
  const Field* companionId = findField(fields, count, "companion_id");
  if (!actionId || actionId->kind != ValueKind::String || !companionId ||
      companionId->kind != ValueKind::String ||
      !parseUuid(actionId->value, candidate.actionId) ||
      !parseUuid(companionId->value, candidate.companionId)) {
    return LanResult::InvalidIdentity;
  }
  if (memcmp(candidate.companionId, expectedCompanionId,
             kLanUuidBytes) != 0) {
    return LanResult::WrongCompanion;
  }
  if (!parseU32(findField(fields, count, "key_version"),
                candidate.keyVersion) ||
      candidate.keyVersion != expectedKeyVersion) {
    return LanResult::WrongKeyVersion;
  }

  const Field* actionType = findField(fields, count, "action_type");
  if (!actionType || actionType->kind != ValueKind::String ||
      !validProtocolName(actionType->value, kLanMaximumActionTypeBytes) ||
      !supportedAction(actionType->value)) {
    return LanResult::UnsupportedAction;
  }
  memcpy(candidate.actionType, actionType->value.data,
         actionType->value.bytes);
  candidate.actionType[actionType->value.bytes] = '\0';

  const Field* created = findField(fields, count, "created_epoch");
  const Field* expires = findField(fields, count, "expires_epoch");
  uint64_t createdValue = 0U;
  uint64_t expiresValue = 0U;
  if (!created || created->kind != ValueKind::String || !expires ||
      expires->kind != ValueKind::String ||
      !parseU64(created->value, createdValue) ||
      !parseU64(expires->value, expiresValue) ||
      createdValue < static_cast<uint64_t>(kLanMinimumKnownEpoch) ||
      expiresValue > static_cast<uint64_t>(kLanMaximumKnownEpoch) ||
      expiresValue <= createdValue || expiresValue - createdValue > 86400U) {
    return LanResult::Expired;
  }
  candidate.createdEpoch = static_cast<int64_t>(createdValue);
  candidate.expiresEpoch = static_cast<int64_t>(expiresValue);
  if (!clockValid || nowEpoch < kLanMinimumKnownEpoch ||
      nowEpoch > kLanMaximumKnownEpoch) {
    return LanResult::ClockRequired;
  }
  if (nowEpoch < candidate.createdEpoch || nowEpoch > candidate.expiresEpoch) {
    return LanResult::Expired;
  }

  uint8_t suppliedMac[kLanSignatureBytes]{};
  if (!decodeExact(findField(fields, count, "nonce_b64"), candidate.nonce,
                   sizeof(candidate.nonce)) ||
      !decodeExact(findField(fields, count, "signature_b64"), suppliedMac,
                   sizeof(suppliedMac))) {
    secureZero(suppliedMac, sizeof(suppliedMac));
    return LanResult::InvalidEncoding;
  }
  const Field* params = findField(fields, count, "params_b64");
  if (!params || params->kind != ValueKind::String) {
    secureZero(suppliedMac, sizeof(suppliedMac));
    return LanResult::InvalidEncoding;
  }
  size_t decodedParams = 0U;
  if (!companion::decodeBase64Url(
          reinterpret_cast<const char*>(params->value.data),
          params->value.bytes, paramsOutput, paramsCapacity, decodedParams) ||
      decodedParams == 0U || decodedParams > kLanMaximumActionParamsBytes ||
      !companion::validUtf8(paramsOutput, decodedParams)) {
    secureZero(suppliedMac, sizeof(suppliedMac));
    if (paramsOutput && paramsCapacity != 0U) {
      secureZero(paramsOutput, paramsCapacity);
    }
    return decodedParams > paramsCapacity ? LanResult::OutputTooSmall
                                          : LanResult::InvalidEncoding;
  }

  uint8_t keyVersion[4]{};
  uint8_t createdEpoch[8]{};
  uint8_t expiresEpoch[8]{};
  uint8_t typeLength[2]{};
  uint8_t paramsLength[4]{};
  putU32Be(keyVersion, candidate.keyVersion);
  putU64Be(createdEpoch, createdValue);
  putU64Be(expiresEpoch, expiresValue);
  putU16Be(typeLength,
           static_cast<uint16_t>(actionType->value.bytes));
  putU32Be(paramsLength, static_cast<uint32_t>(decodedParams));
  const companion::CryptoPart transcript[] = {
      companion::CryptoPart(
          reinterpret_cast<const uint8_t*>(kActionDomain),
          sizeof(kActionDomain) - 1U),
      companion::CryptoPart(candidate.actionId, sizeof(candidate.actionId)),
      companion::CryptoPart(candidate.companionId,
                            sizeof(candidate.companionId)),
      companion::CryptoPart(keyVersion, sizeof(keyVersion)),
      companion::CryptoPart(candidate.nonce, sizeof(candidate.nonce)),
      companion::CryptoPart(createdEpoch, sizeof(createdEpoch)),
      companion::CryptoPart(expiresEpoch, sizeof(expiresEpoch)),
      companion::CryptoPart(typeLength, sizeof(typeLength)),
      companion::CryptoPart(actionType->value.data, actionType->value.bytes),
      companion::CryptoPart(paramsLength, sizeof(paramsLength)),
      companion::CryptoPart(paramsOutput, decodedParams),
  };
  uint8_t expectedMac[kLanSignatureBytes]{};
  const bool authenticated = crypto.hmacSha256(
      backendHmacSecret, transcript,
      sizeof(transcript) / sizeof(transcript[0]), expectedMac);
  const bool macMatches = authenticated &&
      constantTimeEqual(expectedMac, suppliedMac, sizeof(expectedMac));
  secureZero(expectedMac, sizeof(expectedMac));
  secureZero(suppliedMac, sizeof(suppliedMac));
  if (!macMatches) {
    secureZero(paramsOutput, decodedParams);
    return authenticated ? LanResult::AuthenticationFailed
                         : LanResult::CryptoFailed;
  }

  const LanReplayDecision replay = replayStore.acceptAction(
      candidate.actionId, candidate.expiresEpoch, nowEpoch);
  if (replay == LanReplayDecision::Failed) {
    secureZero(paramsOutput, decodedParams);
    return LanResult::ReplayStoreFailed;
  }
  candidate.parameterBytes = decodedParams;
  output = candidate;
  return replay == LanReplayDecision::Duplicate
      ? LanResult::ActionDuplicate
      : LanResult::ActionFresh;
}

LanResult encodeDeviceEnvelope(
    const uint8_t companionId[kLanUuidBytes],
    const uint8_t gatewayId[kLanUuidBytes], uint64_t sequence,
    int64_t issuedEpoch, const uint8_t nonce[kLanNonceBytes],
    const uint8_t requestId[kLanUuidBytes], uint32_t keyVersion,
    const char* payloadType, const uint8_t* payload, size_t payloadBytes,
    const uint8_t backendHmacSecret[32], companion::CompanionCrypto& crypto,
    uint8_t* output, size_t outputCapacity, size_t& outputBytes) {
  outputBytes = 0U;
  const size_t typeBytes = payloadType ? strlen(payloadType) : 0U;
  const Span typeSpan{reinterpret_cast<const uint8_t*>(payloadType),
                      typeBytes};
  if (!companionId || !gatewayId || !nonce || !requestId ||
      !payloadType || !payload || payloadBytes == 0U ||
      payloadBytes > kLanMaximumDevicePayloadBytes ||
      !backendHmacSecret || !output || outputCapacity == 0U ||
      outputCapacity > kLanMaximumFrameBytes || sequence == 0U ||
      sequence > 0x7fffffffffffffffULL || keyVersion == 0U ||
      allZero(companionId, kLanUuidBytes) ||
      allZero(gatewayId, kLanUuidBytes) ||
      allZero(requestId, kLanUuidBytes) ||
      !validProtocolName(typeSpan, kLanMaximumPayloadTypeBytes) ||
      !companion::validUtf8(payload, payloadBytes) ||
      (issuedEpoch != 0 &&
       (issuedEpoch < kLanMinimumKnownEpoch ||
        issuedEpoch > kLanMaximumKnownEpoch))) {
    return LanResult::InvalidArgument;
  }

  uint8_t sequenceBe[8]{};
  uint8_t issuedBe[8]{};
  uint8_t keyVersionBe[4]{};
  uint8_t typeLength[2]{};
  uint8_t payloadLength[4]{};
  putU64Be(sequenceBe, sequence);
  putU64Be(issuedBe, static_cast<uint64_t>(issuedEpoch));
  putU32Be(keyVersionBe, keyVersion);
  putU16Be(typeLength, static_cast<uint16_t>(typeBytes));
  putU32Be(payloadLength, static_cast<uint32_t>(payloadBytes));
  const companion::CryptoPart transcript[] = {
      companion::CryptoPart(
          reinterpret_cast<const uint8_t*>(kDeviceDomain),
          sizeof(kDeviceDomain) - 1U),
      companion::CryptoPart(companionId, kLanUuidBytes),
      companion::CryptoPart(gatewayId, kLanUuidBytes),
      companion::CryptoPart(sequenceBe, sizeof(sequenceBe)),
      companion::CryptoPart(issuedBe, sizeof(issuedBe)),
      companion::CryptoPart(nonce, kLanNonceBytes),
      companion::CryptoPart(requestId, kLanUuidBytes),
      companion::CryptoPart(keyVersionBe, sizeof(keyVersionBe)),
      companion::CryptoPart(typeLength, sizeof(typeLength)),
      companion::CryptoPart(typeSpan.data, typeSpan.bytes),
      companion::CryptoPart(payloadLength, sizeof(payloadLength)),
      companion::CryptoPart(payload, payloadBytes),
  };
  uint8_t signature[kLanSignatureBytes]{};
  if (!crypto.hmacSha256(backendHmacSecret, transcript,
                         sizeof(transcript) / sizeof(transcript[0]),
                         signature)) {
    secureZero(signature, sizeof(signature));
    return LanResult::CryptoFailed;
  }

  char companionText[37]{};
  char gatewayText[37]{};
  char requestText[37]{};
  char sequenceText[21]{};
  char epochText[22]{};
  char versionText[11]{};
  formatUuid(companionId, companionText);
  formatUuid(gatewayId, gatewayText);
  formatUuid(requestId, requestText);
  const int sequenceChars = snprintf(
      sequenceText, sizeof(sequenceText), "%llu",
      static_cast<unsigned long long>(sequence));
  const int epochChars = snprintf(epochText, sizeof(epochText), "%lld",
                                  static_cast<long long>(issuedEpoch));
  const int versionChars = snprintf(versionText, sizeof(versionText), "%lu",
                                    static_cast<unsigned long>(keyVersion));
  if (sequenceChars <= 0 || epochChars <= 0 || versionChars <= 0 ||
      static_cast<size_t>(sequenceChars) >= sizeof(sequenceText) ||
      static_cast<size_t>(epochChars) >= sizeof(epochText) ||
      static_cast<size_t>(versionChars) >= sizeof(versionText)) {
    secureZero(signature, sizeof(signature));
    return LanResult::InvalidArgument;
  }

  JsonWriter writer(output, outputCapacity);
  writer.literal("{\"schema\":\"kitsu.device-envelope.v1\",");
  writer.literal("\"companion_id\":\"");
  writer.literal(companionText);
  writer.literal("\",\"gateway_id\":\"");
  writer.literal(gatewayText);
  writer.literal("\",\"sequence\":\"");
  writer.bytes(reinterpret_cast<const uint8_t*>(sequenceText),
               static_cast<size_t>(sequenceChars));
  writer.literal("\",\"issued_epoch\":\"");
  writer.bytes(reinterpret_cast<const uint8_t*>(epochText),
               static_cast<size_t>(epochChars));
  writer.literal("\",\"nonce_b64\":");
  writer.base64(nonce, kLanNonceBytes);
  writer.literal(",\"request_id\":\"");
  writer.literal(requestText);
  writer.literal("\",\"key_version\":");
  writer.bytes(reinterpret_cast<const uint8_t*>(versionText),
               static_cast<size_t>(versionChars));
  writer.literal(",\"payload_type\":\"");
  writer.literal(payloadType);
  writer.literal("\",\"payload_b64\":");
  writer.base64(payload, payloadBytes);
  writer.literal(",\"signature_b64\":");
  writer.base64(signature, sizeof(signature));
  writer.literal("}");
  secureZero(signature, sizeof(signature));
  if (!writer.ok()) return LanResult::OutputTooSmall;
  outputBytes = writer.used();
  return LanResult::Ok;
}

}  // namespace connectivity
}  // namespace kitsu868
