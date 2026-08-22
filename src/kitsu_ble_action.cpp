#include "kitsu_ble_action.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "kitsu_companion_protocol.h"

#if defined(ARDUINO)
#include <mbedtls/sha256.h>
#endif

namespace kitsu868 {
namespace connectivity {
namespace {

constexpr size_t kMaximumActionBodyBytes = 12000U;
constexpr size_t kMaximumOuterFields = 4U;
constexpr size_t kMaximumParamFields = 3U;
constexpr uint32_t kReplayMagic = 0x3341524bUL;  // "KRA3" on the wire.
constexpr uint8_t kReplayPending = 1U;
constexpr uint8_t kReplayApplied = 2U;

struct Span {
  const uint8_t* data = nullptr;
  size_t bytes = 0U;
  bool escaped = false;
};

enum class ValueKind : uint8_t { String = 0, Integer, Object };

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

bool isHex(uint8_t value) {
  return (value >= '0' && value <= '9') ||
         (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

uint16_t hex4(const uint8_t* input) {
  uint16_t value = 0U;
  for (uint8_t i = 0U; i < 4U; ++i) {
    const uint8_t c = input[i];
    const uint8_t digit = c <= '9'
        ? static_cast<uint8_t>(c - '0')
        : c <= 'F' ? static_cast<uint8_t>(c - 'A' + 10U)
                   : static_cast<uint8_t>(c - 'a' + 10U);
    value = static_cast<uint16_t>((value << 4U) | digit);
  }
  return value;
}

bool parseString(const uint8_t* json, size_t bytes, size_t& cursor,
                 Span& output) {
  if (cursor >= bytes || json[cursor++] != '"') return false;
  const size_t start = cursor;
  bool escaped = false;
  while (cursor < bytes) {
    const uint8_t c = json[cursor++];
    if (c == '"') {
      output.data = json + start;
      output.bytes = cursor - start - 1U;
      output.escaped = escaped;
      return true;
    }
    if (c < 0x20U) return false;
    if (c != '\\') continue;
    escaped = true;
    if (cursor >= bytes) return false;
    const uint8_t escape = json[cursor++];
    if (escape == '"' || escape == '\\' || escape == '/' ||
        escape == 'b' || escape == 'f' || escape == 'n' ||
        escape == 'r' || escape == 't') {
      continue;
    }
    if (escape != 'u' || cursor + 4U > bytes) return false;
    for (uint8_t i = 0U; i < 4U; ++i) {
      if (!isHex(json[cursor + i])) return false;
    }
    const uint16_t first = hex4(json + cursor);
    cursor += 4U;
    if (first >= 0xd800U && first <= 0xdbffU) {
      if (cursor + 6U > bytes || json[cursor] != '\\' ||
          json[cursor + 1U] != 'u') {
        return false;
      }
      for (uint8_t i = 0U; i < 4U; ++i) {
        if (!isHex(json[cursor + 2U + i])) return false;
      }
      const uint16_t second = hex4(json + cursor + 2U);
      if (second < 0xdc00U || second > 0xdfffU) return false;
      cursor += 6U;
    } else if (first >= 0xdc00U && first <= 0xdfffU) {
      return false;
    }
  }
  return false;
}

bool scanObject(const uint8_t* json, size_t bytes, size_t& cursor,
                Span& output) {
  if (cursor >= bytes || json[cursor] != '{') return false;
  const size_t start = cursor++;
  size_t depth = 1U;
  while (cursor < bytes && depth != 0U) {
    if (json[cursor] == '"') {
      Span ignored{};
      if (!parseString(json, bytes, cursor, ignored)) return false;
      continue;
    }
    const uint8_t c = json[cursor++];
    if (c == '{') {
      if (++depth > 8U) return false;
    } else if (c == '}') {
      --depth;
    } else if (c < 0x20U && c != '\t' && c != '\r' && c != '\n') {
      return false;
    }
  }
  if (depth != 0U) return false;
  output.data = json + start;
  output.bytes = cursor - start;
  output.escaped = false;
  return true;
}

bool spanEqual(const Span& left, const Span& right) {
  return left.bytes == right.bytes &&
         memcmp(left.data, right.data, left.bytes) == 0;
}

bool spanEquals(const Span& span, const char* expected) {
  const size_t expectedBytes = expected ? strlen(expected) : 0U;
  return !span.escaped && span.bytes == expectedBytes &&
         memcmp(span.data, expected, expectedBytes) == 0;
}

ParseResult parseObject(const uint8_t* json, size_t jsonBytes,
                        Field* fields, size_t fieldCapacity,
                        size_t& fieldCount) {
  fieldCount = 0U;
  if (!json || jsonBytes < 2U || !fields || fieldCapacity == 0U) {
    return ParseResult::Malformed;
  }
  size_t cursor = 0U;
  skipWhitespace(json, jsonBytes, cursor);
  if (cursor >= jsonBytes || json[cursor++] != '{') {
    return ParseResult::Malformed;
  }
  skipWhitespace(json, jsonBytes, cursor);
  if (cursor < jsonBytes && json[cursor] == '}') {
    ++cursor;
    skipWhitespace(json, jsonBytes, cursor);
    return cursor == jsonBytes ? ParseResult::Ok : ParseResult::Malformed;
  }

  while (cursor < jsonBytes) {
    if (fieldCount >= fieldCapacity) return ParseResult::Malformed;
    Field& field = fields[fieldCount];
    if (!parseString(json, jsonBytes, cursor, field.key) ||
        field.key.escaped || field.key.bytes == 0U) {
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
    if (cursor >= jsonBytes) return ParseResult::Malformed;
    if (json[cursor] == '"') {
      field.kind = ValueKind::String;
      if (!parseString(json, jsonBytes, cursor, field.value)) {
        return ParseResult::Malformed;
      }
    } else if (json[cursor] == '{') {
      field.kind = ValueKind::Object;
      if (!scanObject(json, jsonBytes, cursor, field.value)) {
        return ParseResult::Malformed;
      }
    } else {
      field.kind = ValueKind::Integer;
      const size_t start = cursor;
      if (json[cursor] == '-') ++cursor;
      const size_t digits = cursor;
      while (cursor < jsonBytes && json[cursor] >= '0' &&
             json[cursor] <= '9') {
        ++cursor;
      }
      if (cursor == digits ||
          (cursor - digits > 1U && json[digits] == '0')) {
        return ParseResult::Malformed;
      }
      field.value.data = json + start;
      field.value.bytes = cursor - start;
      field.value.escaped = false;
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

bool parseInt64(const Field* field, int64_t& output) {
  if (!field || field->kind != ValueKind::Integer ||
      !field->value.data || field->value.bytes == 0U ||
      field->value.bytes > 20U) {
    return false;
  }
  size_t cursor = 0U;
  const bool negative = field->value.data[cursor] == '-';
  if (negative && ++cursor == field->value.bytes) return false;
  uint64_t magnitude = 0U;
  const uint64_t maximum = negative ? 0x8000000000000000ULL
                                    : 0x7fffffffffffffffULL;
  for (; cursor < field->value.bytes; ++cursor) {
    const uint8_t c = field->value.data[cursor];
    if (c < '0' || c > '9') return false;
    const uint8_t digit = static_cast<uint8_t>(c - '0');
    if (magnitude > (maximum - digit) / 10U) return false;
    magnitude = magnitude * 10U + digit;
  }
  if (negative) {
    output = magnitude == 0x8000000000000000ULL
        ? static_cast<int64_t>(-0x7fffffffffffffffLL - 1LL)
        : -static_cast<int64_t>(magnitude);
  } else {
    output = static_cast<int64_t>(magnitude);
  }
  return true;
}

bool parseUint32(const Field* field, uint32_t& output) {
  int64_t value = 0;
  if (!parseInt64(field, value) || value < 0 ||
      value > static_cast<int64_t>(UINT32_MAX)) {
    return false;
  }
  output = static_cast<uint32_t>(value);
  return true;
}

int hexNibble(uint8_t value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool parseUuid(const Field* field, BleActionCommand& output) {
  if (!field || field->kind != ValueKind::String || field->value.escaped ||
      field->value.bytes != kBleActionUuidTextBytes) {
    return false;
  }
  static const char hex[] = "0123456789abcdef";
  size_t source = 0U;
  size_t target = 0U;
  while (source < field->value.bytes) {
    if (source == 8U || source == 13U || source == 18U || source == 23U) {
      if (field->value.data[source] != '-') return false;
      output.actionIdText[source] = '-';
      ++source;
      continue;
    }
    if (source + 1U >= field->value.bytes ||
        target >= kBleActionUuidBytes) {
      return false;
    }
    const int high = hexNibble(field->value.data[source]);
    const int low = hexNibble(field->value.data[source + 1U]);
    if (high < 0 || low < 0) return false;
    output.actionId[target++] = static_cast<uint8_t>((high << 4U) | low);
    output.actionIdText[source] = hex[high];
    output.actionIdText[source + 1U] = hex[low];
    source += 2U;
  }
  output.actionIdText[kBleActionUuidTextBytes] = '\0';
  output.actionIdValid = target == kBleActionUuidBytes;
  return output.actionIdValid;
}

bool parseKind(const Field* field, BleActionKind& output) {
  if (!field || field->kind != ValueKind::String || field->value.escaped) {
    return false;
  }
  static const struct {
    const char* name;
    BleActionKind kind;
  } kinds[] = {
      {"pet", BleActionKind::Pet},
      {"feed", BleActionKind::Feed},
      {"play", BleActionKind::Play},
      {"listen_once", BleActionKind::ListenOnce},
      {"send_message", BleActionKind::SendMessage},
  };
  for (size_t i = 0U; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
    if (spanEquals(field->value, kinds[i].name)) {
      output = kinds[i].kind;
      return true;
    }
  }
  return false;
}

bool decodedStringBytes(const Span& value, size_t& decodedBytes) {
  decodedBytes = 0U;
  for (size_t cursor = 0U; cursor < value.bytes;) {
    if (value.data[cursor] != '\\') {
      ++decodedBytes;
      ++cursor;
      continue;
    }
    if (++cursor >= value.bytes) return false;
    const uint8_t escape = value.data[cursor++];
    if (escape != 'u') {
      ++decodedBytes;
      continue;
    }
    if (cursor + 4U > value.bytes) return false;
    const uint16_t first = hex4(value.data + cursor);
    cursor += 4U;
    uint32_t scalar = first;
    if (first >= 0xd800U && first <= 0xdbffU) {
      if (cursor + 6U > value.bytes || value.data[cursor] != '\\' ||
          value.data[cursor + 1U] != 'u') {
        return false;
      }
      const uint16_t second = hex4(value.data + cursor + 2U);
      cursor += 6U;
      scalar = 0x10000UL +
          ((static_cast<uint32_t>(first) - 0xd800UL) << 10U) +
          (static_cast<uint32_t>(second) - 0xdc00UL);
    }
    decodedBytes += scalar <= 0x7fU ? 1U
                    : scalar <= 0x7ffU ? 2U
                    : scalar <= 0xffffU ? 3U : 4U;
  }
  return true;
}

bool appendUtf8(uint32_t scalar, char* output, size_t capacity,
                size_t& written) {
  if (!output || scalar == 0U || scalar > 0x10ffffUL ||
      (scalar >= 0xd800UL && scalar <= 0xdfffUL)) {
    return false;
  }
  const size_t needed = scalar <= 0x7fU ? 1U
                        : scalar <= 0x7ffU ? 2U
                        : scalar <= 0xffffU ? 3U : 4U;
  if (written + needed >= capacity) return false;
  if (needed == 1U) {
    output[written++] = static_cast<char>(scalar);
  } else if (needed == 2U) {
    output[written++] = static_cast<char>(0xc0U | (scalar >> 6U));
    output[written++] = static_cast<char>(0x80U | (scalar & 0x3fU));
  } else if (needed == 3U) {
    output[written++] = static_cast<char>(0xe0U | (scalar >> 12U));
    output[written++] = static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU));
    output[written++] = static_cast<char>(0x80U | (scalar & 0x3fU));
  } else {
    output[written++] = static_cast<char>(0xf0U | (scalar >> 18U));
    output[written++] = static_cast<char>(0x80U | ((scalar >> 12U) & 0x3fU));
    output[written++] = static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU));
    output[written++] = static_cast<char>(0x80U | (scalar & 0x3fU));
  }
  return true;
}

bool decodeString(const Span& value, char* output, size_t capacity,
                  uint8_t& outputBytes) {
  outputBytes = 0U;
  if (!output || capacity == 0U) return false;
  size_t written = 0U;
  for (size_t cursor = 0U; cursor < value.bytes;) {
    const uint8_t current = value.data[cursor++];
    if (current != '\\') {
      if (written + 1U >= capacity) return false;
      output[written++] = static_cast<char>(current);
      continue;
    }
    if (cursor >= value.bytes) return false;
    const uint8_t escape = value.data[cursor++];
    uint32_t scalar = 0U;
    switch (escape) {
      case '"': scalar = '"'; break;
      case '\\': scalar = '\\'; break;
      case '/': scalar = '/'; break;
      case 'b': scalar = '\b'; break;
      case 'f': scalar = '\f'; break;
      case 'n': scalar = '\n'; break;
      case 'r': scalar = '\r'; break;
      case 't': scalar = '\t'; break;
      case 'u': {
        if (cursor + 4U > value.bytes) return false;
        const uint16_t first = hex4(value.data + cursor);
        cursor += 4U;
        scalar = first;
        if (first >= 0xd800U && first <= 0xdbffU) {
          if (cursor + 6U > value.bytes || value.data[cursor] != '\\' ||
              value.data[cursor + 1U] != 'u') {
            return false;
          }
          const uint16_t second = hex4(value.data + cursor + 2U);
          if (second < 0xdc00U || second > 0xdfffU) return false;
          cursor += 6U;
          scalar = 0x10000UL +
              ((static_cast<uint32_t>(first) - 0xd800UL) << 10U) +
              (static_cast<uint32_t>(second) - 0xdc00UL);
        }
        break;
      }
      default: return false;
    }
    if (!appendUtf8(scalar, output, capacity, written)) return false;
  }
  if (written > UINT8_MAX) return false;
  output[written] = '\0';
  outputBytes = static_cast<uint8_t>(written);
  return companion::validUtf8(reinterpret_cast<const uint8_t*>(output),
                              written);
}

bool validDirectTarget(const Span& value) {
  if (value.escaped || value.bytes != kBleActionMessageTargetBytes) {
    return false;
  }
  for (size_t i = 0U; i < value.bytes; ++i) {
    const uint8_t c = value.data[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) {
      return false;
    }
  }

  // A 32-byte key occupies 43 unpadded base64url characters. Merely checking
  // that width and alphabet is not canonical: the low four bits of the final
  // character are padding and some decoders silently discard them. Decode and
  // re-encode so one MeshCore identity has exactly one action/replay binding.
  uint8_t decoded[32]{};
  size_t decodedBytes = 0U;
  if (!companion::decodeBase64Url(
          reinterpret_cast<const char*>(value.data), value.bytes, decoded,
          sizeof(decoded), decodedBytes) ||
      decodedBytes != sizeof(decoded)) {
    return false;
  }
  char canonical[kBleActionMessageTargetBytes + 1U]{};
  size_t canonicalBytes = 0U;
  return companion::encodeBase64Url(
             decoded, sizeof(decoded), canonical, sizeof(canonical),
             canonicalBytes) &&
         canonicalBytes == value.bytes &&
         memcmp(canonical, value.data, value.bytes) == 0;
}

bool validChannelTarget(const Span& value) {
  return !value.escaped && value.bytes == 1U &&
         value.data[0] >= '0' && value.data[0] <= '3';
}

BleActionDecodeResult decodeParams(const Field* params,
                                   BleActionCommand& output) {
  if (!params || params->kind != ValueKind::Object) {
    return BleActionDecodeResult::InvalidParams;
  }
  Field fields[kMaximumParamFields]{};
  size_t count = 0U;
  const ParseResult parsed = parseObject(params->value.data,
                                         params->value.bytes, fields,
                                         kMaximumParamFields, count);
  if (parsed == ParseResult::Duplicate) {
    return BleActionDecodeResult::DuplicateField;
  }
  if (parsed != ParseResult::Ok) {
    return BleActionDecodeResult::MalformedJson;
  }

  switch (output.kind) {
    case BleActionKind::Pet:
    case BleActionKind::Feed:
    case BleActionKind::Play:
      return count == 0U ? BleActionDecodeResult::Ok
                         : BleActionDecodeResult::InvalidParams;

    case BleActionKind::ListenOnce: {
      static const char* const schema[] = {"duration_ms"};
      if (!exactSchema(fields, count, schema, 1U) ||
          !parseUint32(findField(fields, count, "duration_ms"),
                       output.durationMs) ||
          output.durationMs < kBleActionMinimumListenMs ||
          output.durationMs > kBleActionMaximumListenMs) {
        return BleActionDecodeResult::InvalidParams;
      }
      return BleActionDecodeResult::Ok;
    }

    case BleActionKind::SendMessage: {
      static const char* const schema[] = {"route", "target_id", "text"};
      const Field* route = findField(fields, count, "route");
      const Field* target = findField(fields, count, "target_id");
      const Field* text = findField(fields, count, "text");
      size_t textBytes = 0U;
      if (!exactSchema(fields, count, schema, 3U) || !route || !target ||
          !text || route->kind != ValueKind::String ||
          target->kind != ValueKind::String ||
          text->kind != ValueKind::String ||
          !(spanEquals(route->value, "direct") ||
            spanEquals(route->value, "channel")) ||
          !decodedStringBytes(text->value, textBytes) || textBytes == 0U ||
          textBytes > kBleActionMessageTextBytes) {
        return BleActionDecodeResult::InvalidParams;
      }
      output.messageRoute = spanEquals(route->value, "direct")
                                ? BleMessageRoute::Direct
                                : BleMessageRoute::Channel;
      const bool validTarget = output.messageRoute == BleMessageRoute::Direct
                                   ? validDirectTarget(target->value)
                                   : validChannelTarget(target->value);
      if (!validTarget ||
          !decodeString(target->value, output.messageTarget,
                        sizeof(output.messageTarget),
                        output.messageTargetBytes) ||
          !decodeString(text->value, output.messageText,
                        sizeof(output.messageText), output.messageTextBytes) ||
          output.messageTextBytes == 0U) {
        return BleActionDecodeResult::InvalidParams;
      }
      return BleActionDecodeResult::Ok;
    }

  }
  return BleActionDecodeResult::InvalidKind;
}

bool validReceiptToken(const char* token) {
  if (!token || token[0] < 'a' || token[0] > 'z') return false;
  const size_t bytes = strlen(token);
  if (bytes == 0U || bytes > 48U) return false;
  for (size_t i = 1U; i < bytes; ++i) {
    const char c = token[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '_')) {
      return false;
    }
  }
  return true;
}

uint32_t crc32(const uint8_t* input, size_t bytes) {
  uint32_t crc = 0xffffffffUL;
  for (size_t i = 0U; i < bytes; ++i) {
    crc ^= input[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = static_cast<uint32_t>(
          -static_cast<int32_t>(crc & 1U));
      crc = (crc >> 1U) ^ (0xedb88320UL & mask);
    }
  }
  return ~crc;
}

bool allZero(const uint8_t* input, size_t bytes) {
  uint8_t combined = 0U;
  for (size_t i = 0U; i < bytes; ++i) combined |= input[i];
  return combined == 0U;
}

#if defined(ARDUINO)

struct Sha256Context {
  mbedtls_sha256_context value{};
  bool initialized = false;
  bool ok = false;
};

void sha256Start(Sha256Context& context) {
  context = Sha256Context{};
  mbedtls_sha256_init(&context.value);
  context.initialized = true;
  context.ok = mbedtls_sha256_starts_ret(&context.value, 0) == 0;
}

void sha256Update(Sha256Context& context, const uint8_t* input,
                  size_t inputBytes) {
  if (!context.ok || inputBytes == 0U) return;
  if (!input ||
      mbedtls_sha256_update_ret(&context.value, input, inputBytes) != 0) {
    context.ok = false;
  }
}

bool sha256Finish(Sha256Context& context,
                  uint8_t output[kBleActionCommandDigestBytes]) {
  const bool finished = context.ok && output &&
      mbedtls_sha256_finish_ret(&context.value, output) == 0;
  if (context.initialized) mbedtls_sha256_free(&context.value);
  context = Sha256Context{};
  if (!finished && output) memset(output, 0, kBleActionCommandDigestBytes);
  return finished;
}

#else

uint32_t rotateRight(uint32_t value, uint8_t bits) {
  return (value >> bits) | (value << (32U - bits));
}

struct Sha256Context {
  uint32_t state[8]{};
  uint64_t totalBytes = 0U;
  uint8_t block[64]{};
  size_t blockBytes = 0U;
};

void sha256Transform(Sha256Context& context, const uint8_t block[64]) {
  static const uint32_t constants[64] = {
      0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
      0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
      0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
      0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
      0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
      0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
      0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
      0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
      0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
      0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
      0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
      0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
      0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
      0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
      0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
      0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL,
  };
  uint32_t schedule[64]{};
  for (size_t index = 0U; index < 16U; ++index) {
    const size_t offset = index * 4U;
    schedule[index] =
        (static_cast<uint32_t>(block[offset]) << 24U) |
        (static_cast<uint32_t>(block[offset + 1U]) << 16U) |
        (static_cast<uint32_t>(block[offset + 2U]) << 8U) |
        static_cast<uint32_t>(block[offset + 3U]);
  }
  for (size_t index = 16U; index < 64U; ++index) {
    const uint32_t first = schedule[index - 15U];
    const uint32_t second = schedule[index - 2U];
    const uint32_t sigma0 = rotateRight(first, 7U) ^
        rotateRight(first, 18U) ^ (first >> 3U);
    const uint32_t sigma1 = rotateRight(second, 17U) ^
        rotateRight(second, 19U) ^ (second >> 10U);
    schedule[index] = schedule[index - 16U] + sigma0 +
        schedule[index - 7U] + sigma1;
  }

  uint32_t a = context.state[0];
  uint32_t b = context.state[1];
  uint32_t c = context.state[2];
  uint32_t d = context.state[3];
  uint32_t e = context.state[4];
  uint32_t f = context.state[5];
  uint32_t g = context.state[6];
  uint32_t h = context.state[7];
  for (size_t index = 0U; index < 64U; ++index) {
    const uint32_t sum1 = rotateRight(e, 6U) ^
        rotateRight(e, 11U) ^ rotateRight(e, 25U);
    const uint32_t choice = (e & f) ^ ((~e) & g);
    const uint32_t temporary1 = h + sum1 + choice + constants[index] +
        schedule[index];
    const uint32_t sum0 = rotateRight(a, 2U) ^
        rotateRight(a, 13U) ^ rotateRight(a, 22U);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temporary2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }
  context.state[0] += a;
  context.state[1] += b;
  context.state[2] += c;
  context.state[3] += d;
  context.state[4] += e;
  context.state[5] += f;
  context.state[6] += g;
  context.state[7] += h;
}

void sha256Start(Sha256Context& context) {
  context = Sha256Context{};
  context.state[0] = 0x6a09e667UL;
  context.state[1] = 0xbb67ae85UL;
  context.state[2] = 0x3c6ef372UL;
  context.state[3] = 0xa54ff53aUL;
  context.state[4] = 0x510e527fUL;
  context.state[5] = 0x9b05688cUL;
  context.state[6] = 0x1f83d9abUL;
  context.state[7] = 0x5be0cd19UL;
}

void sha256Update(Sha256Context& context, const uint8_t* input,
                  size_t inputBytes) {
  if (!input || inputBytes == 0U) return;
  context.totalBytes += static_cast<uint64_t>(inputBytes);
  while (inputBytes != 0U) {
    const size_t available = sizeof(context.block) - context.blockBytes;
    const size_t copied = inputBytes < available ? inputBytes : available;
    memcpy(context.block + context.blockBytes, input, copied);
    context.blockBytes += copied;
    input += copied;
    inputBytes -= copied;
    if (context.blockBytes == sizeof(context.block)) {
      sha256Transform(context, context.block);
      context.blockBytes = 0U;
    }
  }
}

bool sha256Finish(Sha256Context& context,
                  uint8_t output[kBleActionCommandDigestBytes]) {
  const uint64_t totalBits = context.totalBytes * 8ULL;
  context.block[context.blockBytes++] = 0x80U;
  if (context.blockBytes > 56U) {
    memset(context.block + context.blockBytes, 0,
           sizeof(context.block) - context.blockBytes);
    sha256Transform(context, context.block);
    context.blockBytes = 0U;
  }
  memset(context.block + context.blockBytes, 0, 56U - context.blockBytes);
  for (size_t index = 0U; index < 8U; ++index) {
    context.block[56U + index] = static_cast<uint8_t>(
        totalBits >> ((7U - index) * 8U));
  }
  sha256Transform(context, context.block);
  for (size_t index = 0U; index < 8U; ++index) {
    output[index * 4U] =
        static_cast<uint8_t>(context.state[index] >> 24U);
    output[index * 4U + 1U] =
        static_cast<uint8_t>(context.state[index] >> 16U);
    output[index * 4U + 2U] =
        static_cast<uint8_t>(context.state[index] >> 8U);
    output[index * 4U + 3U] =
        static_cast<uint8_t>(context.state[index]);
  }
  memset(&context, 0, sizeof(context));
  return true;
}

#endif

bool validReplayCommand(const BleActionCommand& command) {
  if (!command.actionIdValid || !bleActionKindAvailable(command.kind) ||
      command.expiresAtEpoch < kBleActionMinimumTrustedEpoch ||
      command.expiresAtEpoch > kBleActionMaximumTrustedEpoch) {
    return false;
  }
  if (command.kind == BleActionKind::ListenOnce) {
    if (command.durationMs < kBleActionMinimumListenMs ||
        command.durationMs > kBleActionMaximumListenMs) {
      return false;
    }
  } else if (command.durationMs != 0U) {
    return false;
  }

  if (command.kind != BleActionKind::SendMessage) {
    return command.messageRoute == BleMessageRoute::None &&
        command.messageTargetBytes == 0U &&
        command.messageTextBytes == 0U &&
        command.messageTarget[0] == '\0' && command.messageText[0] == '\0';
  }
  if (command.messageTextBytes == 0U ||
      command.messageTextBytes > kBleActionMessageTextBytes ||
      command.messageTargetBytes == 0U ||
      command.messageTargetBytes > kBleActionMessageTargetBytes ||
      command.messageTarget[command.messageTargetBytes] != '\0' ||
      command.messageText[command.messageTextBytes] != '\0' ||
      memchr(command.messageTarget, '\0', command.messageTargetBytes) ||
      memchr(command.messageText, '\0', command.messageTextBytes) ||
      !companion::validUtf8(
          reinterpret_cast<const uint8_t*>(command.messageText),
          command.messageTextBytes)) {
    return false;
  }
  Span target{};
  target.data = reinterpret_cast<const uint8_t*>(command.messageTarget);
  target.bytes = command.messageTargetBytes;
  target.escaped = false;
  return command.messageRoute == BleMessageRoute::Direct
      ? validDirectTarget(target)
      : command.messageRoute == BleMessageRoute::Channel &&
            validChannelTarget(target);
}

void sha256Uint32(Sha256Context& context, uint32_t value) {
  const uint8_t encoded[4] = {
      static_cast<uint8_t>(value >> 24U),
      static_cast<uint8_t>(value >> 16U),
      static_cast<uint8_t>(value >> 8U),
      static_cast<uint8_t>(value),
  };
  sha256Update(context, encoded, sizeof(encoded));
}

bool commandDigest(const BleActionCommand& command,
                   uint8_t output[kBleActionCommandDigestBytes]) {
  if (!output || !validReplayCommand(command)) return false;
  static const uint8_t domain[] = "kitsu.ble-action.command.v1";
  Sha256Context context{};
  sha256Start(context);
  sha256Update(context, domain, sizeof(domain) - 1U);
  const uint8_t separator = 0U;
  sha256Update(context, &separator, sizeof(separator));
  sha256Update(context, command.actionId, sizeof(command.actionId));
  const uint8_t kind = static_cast<uint8_t>(command.kind);
  sha256Update(context, &kind, sizeof(kind));
  sha256Uint32(context, command.expiresAtEpoch);
  sha256Uint32(context, command.durationMs);
  const uint8_t route = static_cast<uint8_t>(command.messageRoute);
  sha256Update(context, &route, sizeof(route));
  sha256Update(context, &command.messageTargetBytes,
               sizeof(command.messageTargetBytes));
  sha256Update(context,
      reinterpret_cast<const uint8_t*>(command.messageTarget),
      command.messageTargetBytes);
  const uint8_t textLength[2] = {
      0U, command.messageTextBytes,
  };
  sha256Update(context, textLength, sizeof(textLength));
  sha256Update(context,
      reinterpret_cast<const uint8_t*>(command.messageText),
      command.messageTextBytes);
  return sha256Finish(context, output);
}

template <typename RecordT>
bool occupiedRecordValid(const RecordT& record) {
  return (record.outcome == kReplayPending ||
          record.outcome == kReplayApplied) &&
      record.expiresAtEpoch >= kBleActionMinimumTrustedEpoch &&
      record.expiresAtEpoch <= kBleActionMaximumTrustedEpoch;
}

}  // namespace

const char* bleActionDecodeResultName(BleActionDecodeResult result) {
  switch (result) {
    case BleActionDecodeResult::Ok: return "ok";
    case BleActionDecodeResult::InvalidArgument: return "invalid_argument";
    case BleActionDecodeResult::MalformedJson: return "malformed_json";
    case BleActionDecodeResult::DuplicateField: return "duplicate_field";
    case BleActionDecodeResult::UnknownField: return "unknown_field";
    case BleActionDecodeResult::InvalidActionId: return "invalid_action_id";
    case BleActionDecodeResult::InvalidKind: return "invalid_kind";
    case BleActionDecodeResult::InvalidExpiry: return "invalid_expiry";
    case BleActionDecodeResult::InvalidParams: return "invalid_params";
  }
  return "invalid_action";
}

const char* bleActionKindName(BleActionKind kind) {
  switch (kind) {
    case BleActionKind::Pet: return "pet";
    case BleActionKind::Feed: return "feed";
    case BleActionKind::Play: return "play";
    case BleActionKind::ListenOnce: return "listen_once";
    case BleActionKind::SendMessage: return "send_message";
  }
  return "unknown";
}

bool bleActionKindAvailable(BleActionKind kind) {
  return kind == BleActionKind::Pet || kind == BleActionKind::Feed ||
         kind == BleActionKind::Play || kind == BleActionKind::ListenOnce ||
         kind == BleActionKind::SendMessage;
}

BleActionDecodeResult decodeBleActionCommand(
    const uint8_t* json, size_t jsonBytes, BleActionCommand& output) {
  output = BleActionCommand{};
  if (!json || jsonBytes == 0U || jsonBytes > kMaximumActionBodyBytes) {
    return BleActionDecodeResult::InvalidArgument;
  }
  if (!companion::validUtf8(json, jsonBytes)) {
    return BleActionDecodeResult::MalformedJson;
  }
  Field fields[kMaximumOuterFields]{};
  size_t count = 0U;
  const ParseResult parsed = parseObject(json, jsonBytes, fields,
                                         kMaximumOuterFields, count);
  if (parsed == ParseResult::Duplicate) {
    return BleActionDecodeResult::DuplicateField;
  }
  if (parsed != ParseResult::Ok) {
    return BleActionDecodeResult::MalformedJson;
  }
  static const char* const schema[] = {
      "action_id", "kind", "expires_at_epoch", "params"};
  if (!exactSchema(fields, count, schema, 4U)) {
    return BleActionDecodeResult::UnknownField;
  }
  if (!parseUuid(findField(fields, count, "action_id"), output)) {
    return BleActionDecodeResult::InvalidActionId;
  }
  if (!parseKind(findField(fields, count, "kind"), output.kind)) {
    return BleActionDecodeResult::InvalidKind;
  }
  if (!parseUint32(findField(fields, count, "expires_at_epoch"),
                   output.expiresAtEpoch) ||
      output.expiresAtEpoch < kBleActionMinimumTrustedEpoch ||
      output.expiresAtEpoch > kBleActionMaximumTrustedEpoch) {
    return BleActionDecodeResult::InvalidExpiry;
  }
  return decodeParams(findField(fields, count, "params"), output);
}

bool bleActionCommandDigest(
    const BleActionCommand& command,
    uint8_t output[kBleActionCommandDigestBytes]) {
  return commandDigest(command, output);
}

bool encodeBleActionReceipt(
    const BleActionCommand& command, bool accepted, const char* state,
    const char* errorCode, uint8_t* output, size_t outputCapacity,
    size_t& outputBytes) {
  outputBytes = 0U;
  if (!command.actionIdValid || !validReceiptToken(state) || !output ||
      outputCapacity == 0U || (accepted && errorCode) ||
      (!accepted && !validReceiptToken(errorCode))) {
    return false;
  }
  const int written = errorCode
      ? snprintf(reinterpret_cast<char*>(output), outputCapacity,
                 "{\"action_id\":\"%s\",\"accepted\":false,"
                 "\"state\":\"%s\",\"error_code\":\"%s\"}",
                 command.actionIdText, state, errorCode)
      : snprintf(reinterpret_cast<char*>(output), outputCapacity,
                 "{\"action_id\":\"%s\",\"accepted\":true,"
                 "\"state\":\"%s\"}",
                 command.actionIdText, state);
  if (written <= 0 || static_cast<size_t>(written) >= outputCapacity) {
    if (outputCapacity != 0U) output[0] = 0U;
    return false;
  }
  outputBytes = static_cast<size_t>(written);
  return true;
}

BleActionReplayCache::BleActionReplayCache() { reset(); }

void BleActionReplayCache::reset() {
  blob_ = Blob{};
  blob_.magic = kReplayMagic;
  blob_.bytes = static_cast<uint16_t>(sizeof(blob_));
  seal();
}

bool BleActionReplayCache::load(const uint8_t* serialized,
                                size_t serializedBytes) {
  if (!serialized || serializedBytes != sizeof(blob_)) {
    reset();
    return false;
  }
  Blob candidate{};
  memcpy(&candidate, serialized, sizeof(candidate));
  blob_ = candidate;
  if (!valid()) {
    reset();
    return false;
  }
  return true;
}

const uint8_t* BleActionReplayCache::serialized(
    size_t& serializedBytes) const {
  serializedBytes = sizeof(blob_);
  return reinterpret_cast<const uint8_t*>(&blob_);
}

BleActionReplayDecision BleActionReplayCache::inspect(
    const BleActionCommand& command, uint32_t trustedNowEpoch) const {
  uint8_t digest[kBleActionCommandDigestBytes]{};
  if (!commandDigest(command, digest)) {
    return BleActionReplayDecision::Conflict;
  }
  if (trustedNowEpoch < kBleActionMinimumTrustedEpoch ||
      trustedNowEpoch > kBleActionMaximumTrustedEpoch) {
    return BleActionReplayDecision::TimeUnavailable;
  }
  if (command.expiresAtEpoch <= trustedNowEpoch) {
    return BleActionReplayDecision::Expired;
  }
  if (command.expiresAtEpoch - trustedNowEpoch >
      kBleActionMaximumExpirySeconds) {
    return BleActionReplayDecision::InvalidExpiry;
  }
  for (size_t i = 0U; i < blob_.count; ++i) {
    const Record& record = blob_.records[i];
    if (memcmp(record.actionId, command.actionId,
               sizeof(record.actionId)) != 0) {
      continue;
    }
    if (memcmp(record.commandDigest, digest, sizeof(digest)) == 0) {
      return record.outcome == kReplayApplied
          ? BleActionReplayDecision::DuplicateApplied
          : BleActionReplayDecision::DuplicateIndeterminate;
    }
    // Reusing an action UUID is only safe after its previous absolute
    // protection window has ended. Before then, a different digest is an
    // unambiguous command conflict.
    return record.expiresAtEpoch <= trustedNowEpoch
        ? BleActionReplayDecision::Fresh
        : BleActionReplayDecision::Conflict;
  }
  return BleActionReplayDecision::Fresh;
}

bool BleActionReplayCache::remember(const BleActionCommand& command,
                                    uint32_t trustedNowEpoch) {
  const BleActionReplayDecision decision = inspect(command,
                                                   trustedNowEpoch);
  if (decision == BleActionReplayDecision::DuplicateApplied ||
      decision == BleActionReplayDecision::DuplicateIndeterminate) {
    return true;
  }
  if (decision != BleActionReplayDecision::Fresh) return false;

  size_t slot = kBleActionReplayCapacity;
  for (size_t i = 0U; i < blob_.count; ++i) {
    if (blob_.records[i].expiresAtEpoch <= trustedNowEpoch) {
      if (slot == kBleActionReplayCapacity) slot = i;
      if (memcmp(blob_.records[i].actionId, command.actionId,
                 sizeof(command.actionId)) == 0) {
        slot = i;
        break;
      }
    }
  }
  if (slot == kBleActionReplayCapacity) {
    if (blob_.count >= kBleActionReplayCapacity) return false;
    slot = blob_.count;
    ++blob_.count;
  }

  uint8_t digest[kBleActionCommandDigestBytes]{};
  if (!commandDigest(command, digest)) return false;
  Record& record = blob_.records[slot];
  record = Record{};
  memcpy(record.actionId, command.actionId, sizeof(record.actionId));
  memcpy(record.commandDigest, digest, sizeof(record.commandDigest));
  record.expiresAtEpoch = command.expiresAtEpoch;
  record.outcome = kReplayPending;
  memset(digest, 0, sizeof(digest));
  seal();
  return true;
}

bool BleActionReplayCache::markApplied(const BleActionCommand& command) {
  uint8_t digest[kBleActionCommandDigestBytes]{};
  if (!commandDigest(command, digest)) return false;
  for (size_t i = 0U; i < blob_.count; ++i) {
    Record& record = blob_.records[i];
    if (memcmp(record.actionId, command.actionId,
               sizeof(record.actionId)) != 0) {
      continue;
    }
    if (memcmp(record.commandDigest, digest, sizeof(digest)) != 0) {
      return false;
    }
    record.outcome = kReplayApplied;
    memset(digest, 0, sizeof(digest));
    seal();
    return true;
  }
  memset(digest, 0, sizeof(digest));
  return false;
}

bool BleActionReplayCache::valid() const {
  if (blob_.magic != kReplayMagic || blob_.bytes != sizeof(blob_) ||
      blob_.count > kBleActionReplayCapacity ||
      blob_.reserved != 0U ||
      blob_.crc32 != crc32(reinterpret_cast<const uint8_t*>(&blob_),
                           offsetof(Blob, crc32))) {
    return false;
  }
  for (size_t i = 0U; i < kBleActionReplayCapacity; ++i) {
    if (i < blob_.count) {
      if (!occupiedRecordValid(blob_.records[i])) return false;
      for (size_t earlier = 0U; earlier < i; ++earlier) {
        if (memcmp(blob_.records[earlier].actionId,
                   blob_.records[i].actionId,
                   sizeof(blob_.records[i].actionId)) == 0) {
          return false;
        }
      }
    } else if (!allZero(reinterpret_cast<const uint8_t*>(&blob_.records[i]),
                        sizeof(blob_.records[i]))) {
      return false;
    }
  }
  return true;
}

void BleActionReplayCache::seal() {
  blob_.crc32 = crc32(reinterpret_cast<const uint8_t*>(&blob_),
                      offsetof(Blob, crc32));
}

}  // namespace connectivity
}  // namespace kitsu868
