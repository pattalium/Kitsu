#include "kitsu_companion_protocol.h"

#include <string.h>

namespace kitsu868 {
namespace companion {
namespace {

void secureZero(void* memory, size_t bytes) {
  volatile uint8_t* output = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *output++ = 0U;
}

bool deadlineExpired(uint32_t now, uint32_t started, uint32_t timeout) {
  return static_cast<uint32_t>(now - started) > timeout;
}

bool constantTimeEqual(const uint8_t* left, const uint8_t* right,
                       size_t bytes) {
  uint8_t difference = 0U;
  for (size_t i = 0U; i < bytes; ++i) difference |= left[i] ^ right[i];
  return difference == 0U;
}

void putU16Be(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value >> 8U);
  output[1] = static_cast<uint8_t>(value);
}

void putU32Be(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value >> 24U);
  output[1] = static_cast<uint8_t>(value >> 16U);
  output[2] = static_cast<uint8_t>(value >> 8U);
  output[3] = static_cast<uint8_t>(value);
}

void putU64Be(uint8_t* output, uint64_t value) {
  for (uint8_t i = 0U; i < 8U; ++i) {
    output[i] = static_cast<uint8_t>(value >> ((7U - i) * 8U));
  }
}

uint32_t getU32Be(const uint8_t* input) {
  return (static_cast<uint32_t>(input[0]) << 24U) |
         (static_cast<uint32_t>(input[1]) << 16U) |
         (static_cast<uint32_t>(input[2]) << 8U) |
         static_cast<uint32_t>(input[3]);
}

bool operationValid(const char* operation, size_t bytes) {
  if (!operation || bytes == 0U || bytes > kMaximumOperationBytes) {
    return false;
  }
  for (size_t i = 0U; i < bytes; ++i) {
    const uint8_t c = static_cast<uint8_t>(operation[i]);
    const bool allowed = (c >= 'a' && c <= 'z') ||
                         (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                         c == '-' || c == '/';
    if (!allowed) return false;
  }
  return true;
}

size_t stringLengthBounded(const char* input, size_t maximum) {
  if (!input) return 0U;
  size_t bytes = 0U;
  while (bytes <= maximum && input[bytes] != '\0') ++bytes;
  return bytes;
}

bool parseDecimal(const uint8_t* input, size_t bytes, uint64_t& value) {
  value = 0U;
  if (!input || bytes == 0U || bytes > 20U ||
      (bytes > 1U && input[0] == '0')) {
    return false;
  }
  for (size_t i = 0U; i < bytes; ++i) {
    if (input[i] < '0' || input[i] > '9') return false;
    const uint8_t digit = static_cast<uint8_t>(input[i] - '0');
    if (value > (UINT64_MAX - digit) / 10U) return false;
    value = value * 10U + digit;
  }
  return value != 0U;
}

size_t formatDecimal(uint64_t value, char output[21]) {
  char reverse[20]{};
  size_t bytes = 0U;
  do {
    reverse[bytes++] = static_cast<char>('0' + (value % 10U));
    value /= 10U;
  } while (value != 0U && bytes < sizeof(reverse));
  for (size_t i = 0U; i < bytes; ++i) output[i] = reverse[bytes - i - 1U];
  output[bytes] = '\0';
  return bytes;
}

int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

struct Span {
  const uint8_t* data = nullptr;
  size_t bytes = 0U;
};

enum Field : uint8_t {
  FieldVersion = 0,
  FieldChannel,
  FieldSequence,
  FieldNonce,
  FieldRequestId,
  FieldOperation,
  FieldPayload,
  FieldMac,
  FieldCount,
};

int fieldIndex(const Span& key) {
  struct Name {
    const char* text;
    size_t bytes;
  };
  static const Name names[FieldCount] = {
      {"v", 1U},
      {"channel", 7U},
      {"seq", 3U},
      {"nonce_b64", 9U},
      {"request_id_b64", 14U},
      {"op", 2U},
      {"payload_b64", 11U},
      {"mac_b64", 7U},
  };
  for (uint8_t i = 0U; i < FieldCount; ++i) {
    if (key.bytes == names[i].bytes &&
        memcmp(key.data, names[i].text, key.bytes) == 0) {
      return i;
    }
  }
  return -1;
}

void skipWhitespace(const uint8_t* json, size_t bytes, size_t& cursor) {
  while (cursor < bytes &&
         (json[cursor] == ' ' || json[cursor] == '\t' ||
          json[cursor] == '\r' || json[cursor] == '\n')) {
    ++cursor;
  }
}

bool parseSimpleString(const uint8_t* json, size_t bytes, size_t& cursor,
                       Span& output) {
  if (cursor >= bytes || json[cursor++] != '"') return false;
  const size_t start = cursor;
  while (cursor < bytes && json[cursor] != '"') {
    if (json[cursor] < 0x20U || json[cursor] == '\\') return false;
    ++cursor;
  }
  if (cursor >= bytes) return false;
  output.data = json + start;
  output.bytes = cursor - start;
  ++cursor;
  return true;
}

bool parseUnsignedToken(const uint8_t* json, size_t bytes, size_t& cursor,
                        Span& output) {
  const size_t start = cursor;
  while (cursor < bytes && json[cursor] >= '0' && json[cursor] <= '9') {
    ++cursor;
  }
  if (cursor == start || (cursor - start > 1U && json[start] == '0')) {
    return false;
  }
  output.data = json + start;
  output.bytes = cursor - start;
  return true;
}

ProtocolResult parseOuterObject(const uint8_t* json, size_t bytes,
                                Span fields[FieldCount]) {
  if (!json || bytes == 0U) return ProtocolResult::MalformedJson;
  size_t cursor = 0U;
  uint16_t seen = 0U;
  skipWhitespace(json, bytes, cursor);
  if (cursor >= bytes || json[cursor++] != '{') {
    return ProtocolResult::MalformedJson;
  }
  skipWhitespace(json, bytes, cursor);
  if (cursor < bytes && json[cursor] == '}') {
    return ProtocolResult::MissingField;
  }
  while (cursor < bytes) {
    Span key{};
    if (!parseSimpleString(json, bytes, cursor, key)) {
      return ProtocolResult::MalformedJson;
    }
    const int index = fieldIndex(key);
    if (index < 0) return ProtocolResult::UnknownField;
    const uint16_t mask = static_cast<uint16_t>(1U << index);
    if ((seen & mask) != 0U) return ProtocolResult::DuplicateField;
    seen |= mask;
    skipWhitespace(json, bytes, cursor);
    if (cursor >= bytes || json[cursor++] != ':') {
      return ProtocolResult::MalformedJson;
    }
    skipWhitespace(json, bytes, cursor);
    const bool numeric = index == FieldVersion || index == FieldChannel;
    if (!(numeric ? parseUnsignedToken(json, bytes, cursor, fields[index])
                  : parseSimpleString(json, bytes, cursor, fields[index]))) {
      return ProtocolResult::MalformedJson;
    }
    skipWhitespace(json, bytes, cursor);
    if (cursor >= bytes) return ProtocolResult::MalformedJson;
    if (json[cursor] == '}') {
      ++cursor;
      break;
    }
    if (json[cursor++] != ',') return ProtocolResult::MalformedJson;
    skipWhitespace(json, bytes, cursor);
  }
  skipWhitespace(json, bytes, cursor);
  if (cursor != bytes) return ProtocolResult::MalformedJson;
  if (seen != static_cast<uint16_t>((1U << FieldCount) - 1U)) {
    return ProtocolResult::MissingField;
  }
  return ProtocolResult::Ok;
}

class JsonValidator {
 public:
  JsonValidator(const uint8_t* input, size_t bytes)
      : input_(input), bytes_(bytes) {}

  bool valid() {
    skip();
    if (!value(0U)) return false;
    skip();
    return cursor_ == bytes_;
  }

 private:
  void skip() {
    while (cursor_ < bytes_ &&
           (input_[cursor_] == ' ' || input_[cursor_] == '\t' ||
            input_[cursor_] == '\r' || input_[cursor_] == '\n')) {
      ++cursor_;
    }
  }

  bool value(uint8_t depth) {
    if (depth > 16U || cursor_ >= bytes_) return false;
    switch (input_[cursor_]) {
      case '{': return object(static_cast<uint8_t>(depth + 1U));
      case '[': return array(static_cast<uint8_t>(depth + 1U));
      case '"': return string();
      case 't': return literal("true", 4U);
      case 'f': return literal("false", 5U);
      case 'n': return literal("null", 4U);
      default: return number();
    }
  }

  bool literal(const char* expected, size_t bytes) {
    if (cursor_ + bytes > bytes_ ||
        memcmp(input_ + cursor_, expected, bytes) != 0) {
      return false;
    }
    cursor_ += bytes;
    return true;
  }

  bool hex4(uint16_t& value) {
    value = 0U;
    if (cursor_ + 4U > bytes_) return false;
    for (uint8_t i = 0U; i < 4U; ++i) {
      const uint8_t c = input_[cursor_++];
      uint8_t digit = 0U;
      if (c >= '0' && c <= '9') digit = static_cast<uint8_t>(c - '0');
      else if (c >= 'a' && c <= 'f') digit = static_cast<uint8_t>(c - 'a' + 10U);
      else if (c >= 'A' && c <= 'F') digit = static_cast<uint8_t>(c - 'A' + 10U);
      else return false;
      value = static_cast<uint16_t>((value << 4U) | digit);
    }
    return true;
  }

  bool string() {
    if (input_[cursor_++] != '"') return false;
    while (cursor_ < bytes_) {
      const uint8_t c = input_[cursor_++];
      if (c == '"') return true;
      if (c < 0x20U) return false;
      if (c != '\\') continue;
      if (cursor_ >= bytes_) return false;
      const uint8_t escaped = input_[cursor_++];
      if (escaped == '"' || escaped == '\\' || escaped == '/' ||
          escaped == 'b' || escaped == 'f' || escaped == 'n' ||
          escaped == 'r' || escaped == 't') {
        continue;
      }
      if (escaped != 'u') return false;
      uint16_t first = 0U;
      if (!hex4(first)) return false;
      if (first >= 0xd800U && first <= 0xdbffU) {
        if (cursor_ + 2U > bytes_ || input_[cursor_++] != '\\' ||
            input_[cursor_++] != 'u') {
          return false;
        }
        uint16_t second = 0U;
        if (!hex4(second) || second < 0xdc00U || second > 0xdfffU) {
          return false;
        }
      } else if (first >= 0xdc00U && first <= 0xdfffU) {
        return false;
      }
    }
    return false;
  }

  bool number() {
    const size_t start = cursor_;
    if (cursor_ < bytes_ && input_[cursor_] == '-') ++cursor_;
    if (cursor_ >= bytes_) return false;
    if (input_[cursor_] == '0') {
      ++cursor_;
      if (cursor_ < bytes_ && input_[cursor_] >= '0' &&
          input_[cursor_] <= '9') return false;
    } else {
      if (input_[cursor_] < '1' || input_[cursor_] > '9') return false;
      while (cursor_ < bytes_ && input_[cursor_] >= '0' &&
             input_[cursor_] <= '9') ++cursor_;
    }
    if (cursor_ < bytes_ && input_[cursor_] == '.') {
      ++cursor_;
      const size_t fraction = cursor_;
      while (cursor_ < bytes_ && input_[cursor_] >= '0' &&
             input_[cursor_] <= '9') ++cursor_;
      if (cursor_ == fraction) return false;
    }
    if (cursor_ < bytes_ &&
        (input_[cursor_] == 'e' || input_[cursor_] == 'E')) {
      ++cursor_;
      if (cursor_ < bytes_ &&
          (input_[cursor_] == '+' || input_[cursor_] == '-')) ++cursor_;
      const size_t exponent = cursor_;
      while (cursor_ < bytes_ && input_[cursor_] >= '0' &&
             input_[cursor_] <= '9') ++cursor_;
      if (cursor_ == exponent) return false;
    }
    return cursor_ > start;
  }

  bool object(uint8_t depth) {
    ++cursor_;
    skip();
    if (cursor_ < bytes_ && input_[cursor_] == '}') {
      ++cursor_;
      return true;
    }
    while (cursor_ < bytes_) {
      if (!string()) return false;
      skip();
      if (cursor_ >= bytes_ || input_[cursor_++] != ':') return false;
      skip();
      if (!value(depth)) return false;
      skip();
      if (cursor_ >= bytes_) return false;
      if (input_[cursor_] == '}') {
        ++cursor_;
        return true;
      }
      if (input_[cursor_++] != ',') return false;
      skip();
    }
    return false;
  }

  bool array(uint8_t depth) {
    ++cursor_;
    skip();
    if (cursor_ < bytes_ && input_[cursor_] == ']') {
      ++cursor_;
      return true;
    }
    while (cursor_ < bytes_) {
      if (!value(depth)) return false;
      skip();
      if (cursor_ >= bytes_) return false;
      if (input_[cursor_] == ']') {
        ++cursor_;
        return true;
      }
      if (input_[cursor_++] != ',') return false;
      skip();
    }
    return false;
  }

  const uint8_t* input_;
  size_t bytes_;
  size_t cursor_ = 0U;
};

class OutputWriter {
 public:
  OutputWriter(uint8_t* output, size_t capacity)
      : output_(output), capacity_(capacity) {}

  bool append(const char* text, size_t bytes) {
    if (!text || cursor_ + bytes > capacity_) return false;
    memcpy(output_ + cursor_, text, bytes);
    cursor_ += bytes;
    return true;
  }

  bool appendBase64(const uint8_t* input, size_t bytes) {
    size_t encoded = 0U;
    if (!encodeBase64Url(input, bytes,
                         reinterpret_cast<char*>(output_ + cursor_),
                         capacity_ - cursor_, encoded)) {
      return false;
    }
    cursor_ += encoded;
    return true;
  }

  size_t bytes() const { return cursor_; }

 private:
  uint8_t* output_;
  size_t capacity_;
  size_t cursor_ = 0U;
};

ProtocolResult calculateEnvelopeMac(
    EnvelopeChannel channel, uint64_t sequence,
    const uint8_t nonce[kEnvelopeNonceBytes],
    const uint8_t requestId[kRequestIdBytes], const char* operation,
    size_t operationBytes, const uint8_t* payload, size_t payloadBytes,
    const uint8_t key[kEnvelopeKeyBytes], CompanionCrypto& crypto,
    uint8_t output[kEnvelopeMacBytes]) {
  static const uint8_t domain[] = {'K', 'I', 'T', 'S', 'U', '-', 'E', 'N',
                                   'V', '-', '1', 0};
  uint8_t channelByte = static_cast<uint8_t>(channel);
  uint8_t sequenceBytes[8]{};
  uint8_t operationLength[2]{};
  uint8_t payloadLength[4]{};
  putU64Be(sequenceBytes, sequence);
  putU16Be(operationLength, static_cast<uint16_t>(operationBytes));
  putU32Be(payloadLength, static_cast<uint32_t>(payloadBytes));
  const CryptoPart parts[] = {
      {domain, sizeof(domain)},
      {&channelByte, 1U},
      {sequenceBytes, sizeof(sequenceBytes)},
      {nonce, kEnvelopeNonceBytes},
      {requestId, kRequestIdBytes},
      {operationLength, sizeof(operationLength)},
      {reinterpret_cast<const uint8_t*>(operation), operationBytes},
      {payloadLength, sizeof(payloadLength)},
      {payload, payloadBytes},
  };
  const bool ok = crypto.hmacSha256(key, parts,
                                    sizeof(parts) / sizeof(parts[0]), output);
  secureZero(sequenceBytes, sizeof(sequenceBytes));
  return ok ? ProtocolResult::Ok : ProtocolResult::CryptoFailed;
}

bool roleAllowed(const char* role) {
  return role && (strcmp(role, "device") == 0 ||
                  strcmp(role, "client") == 0 || strcmp(role, "ok") == 0);
}

}  // namespace

const char* frameResultName(FrameResult result) {
  switch (result) {
    case FrameResult::NeedMore: return "need_more";
    case FrameResult::Ready: return "ready";
    case FrameResult::InvalidArgument: return "invalid_argument";
    case FrameResult::EmptyFrame: return "empty_frame";
    case FrameResult::Oversize: return "oversize";
    case FrameResult::TimedOut: return "timed_out";
    case FrameResult::PipelinedFrame: return "pipelined_frame";
    case FrameResult::OutputTooSmall: return "output_too_small";
    default: return "unknown";
  }
}

LengthFrameParser::LengthFrameParser() = default;

bool LengthFrameParser::begin(uint8_t* storage, size_t storageBytes,
                              size_t maximumFrameBytes, uint32_t timeoutMs) {
  reset();
  if (!storage || storageBytes == 0U || maximumFrameBytes == 0U ||
      maximumFrameBytes > storageBytes ||
      maximumFrameBytes > kMaximumFrameBytes || timeoutMs == 0U) {
    storage_ = nullptr;
    return false;
  }
  storage_ = storage;
  storageBytes_ = storageBytes;
  maximumFrameBytes_ = maximumFrameBytes;
  timeoutMs_ = timeoutMs;
  return true;
}

void LengthFrameParser::reset() {
  memset(header_, 0, sizeof(header_));
  headerBytes_ = 0U;
  expectedBytes_ = 0U;
  receivedBytes_ = 0U;
  startedAt_ = 0U;
  started_ = false;
  ready_ = false;
}

FrameResult LengthFrameParser::feed(const uint8_t* input, size_t inputBytes,
                                    uint32_t nowMillis) {
  if (!storage_ || (!input && inputBytes != 0U)) {
    return FrameResult::InvalidArgument;
  }
  if (inputBytes == 0U) return poll(nowMillis);
  if (ready_) {
    reset();
    return FrameResult::PipelinedFrame;
  }
  if (!started_) {
    started_ = true;
    startedAt_ = nowMillis;
  } else if (deadlineExpired(nowMillis, startedAt_, timeoutMs_)) {
    reset();
    return FrameResult::TimedOut;
  }

  size_t cursor = 0U;
  while (headerBytes_ < kFrameHeaderBytes && cursor < inputBytes) {
    header_[headerBytes_++] = input[cursor++];
  }
  if (headerBytes_ == kFrameHeaderBytes && expectedBytes_ == 0U) {
    expectedBytes_ = getU32Be(header_);
    if (expectedBytes_ == 0U) {
      reset();
      return FrameResult::EmptyFrame;
    }
    if (expectedBytes_ > maximumFrameBytes_ || expectedBytes_ > storageBytes_) {
      reset();
      return FrameResult::Oversize;
    }
  }
  if (headerBytes_ < kFrameHeaderBytes) return FrameResult::NeedMore;

  const size_t remaining = expectedBytes_ - receivedBytes_;
  const size_t available = inputBytes - cursor;
  const size_t copyBytes = available < remaining ? available : remaining;
  if (copyBytes != 0U) {
    memcpy(storage_ + receivedBytes_, input + cursor, copyBytes);
    receivedBytes_ += copyBytes;
    cursor += copyBytes;
  }
  if (cursor != inputBytes) {
    reset();
    return FrameResult::PipelinedFrame;
  }
  if (receivedBytes_ == expectedBytes_) {
    ready_ = true;
    return FrameResult::Ready;
  }
  return FrameResult::NeedMore;
}

FrameResult LengthFrameParser::poll(uint32_t nowMillis) {
  if (ready_) return FrameResult::Ready;
  if (started_ && deadlineExpired(nowMillis, startedAt_, timeoutMs_)) {
    reset();
    return FrameResult::TimedOut;
  }
  return FrameResult::NeedMore;
}

bool LengthFrameParser::frame(const uint8_t*& output,
                              size_t& outputBytes) const {
  if (!ready_) {
    output = nullptr;
    outputBytes = 0U;
    return false;
  }
  output = storage_;
  outputBytes = receivedBytes_;
  return true;
}

void LengthFrameParser::consume() { reset(); }

FrameResult encodeLengthFrame(const uint8_t* payload, size_t payloadBytes,
                              uint8_t* output, size_t outputCapacity,
                              size_t& outputBytes) {
  outputBytes = 0U;
  if (!payload || !output) return FrameResult::InvalidArgument;
  if (payloadBytes == 0U) return FrameResult::EmptyFrame;
  if (payloadBytes > kMaximumFrameBytes || payloadBytes > UINT32_MAX) {
    return FrameResult::Oversize;
  }
  if (outputCapacity < kFrameHeaderBytes + payloadBytes) {
    return FrameResult::OutputTooSmall;
  }
  putU32Be(output, static_cast<uint32_t>(payloadBytes));
  memcpy(output + kFrameHeaderBytes, payload, payloadBytes);
  outputBytes = kFrameHeaderBytes + payloadBytes;
  return FrameResult::Ready;
}

const char* protocolResultName(ProtocolResult result) {
  switch (result) {
    case ProtocolResult::Ok: return "ok";
    case ProtocolResult::InvalidArgument: return "invalid_argument";
    case ProtocolResult::OutputTooSmall: return "output_too_small";
    case ProtocolResult::MalformedJson: return "malformed_json";
    case ProtocolResult::DuplicateField: return "duplicate_field";
    case ProtocolResult::UnknownField: return "unknown_field";
    case ProtocolResult::MissingField: return "missing_field";
    case ProtocolResult::UnsupportedVersion: return "unsupported_version";
    case ProtocolResult::InvalidChannel: return "invalid_channel";
    case ProtocolResult::InvalidSequence: return "invalid_sequence";
    case ProtocolResult::UnexpectedSequence: return "unexpected_sequence";
    case ProtocolResult::InvalidBase64: return "invalid_base64";
    case ProtocolResult::InvalidOperation: return "invalid_operation";
    case ProtocolResult::PayloadTooLarge: return "payload_too_large";
    case ProtocolResult::AuthenticationFailed:
      return "authentication_failed";
    case ProtocolResult::InvalidPayloadUtf8: return "invalid_payload_utf8";
    case ProtocolResult::InvalidPayloadJson: return "invalid_payload_json";
    case ProtocolResult::CryptoFailed: return "crypto_failed";
    default: return "unknown";
  }
}

size_t base64UrlEncodedBytes(size_t inputBytes) {
  const size_t full = (inputBytes / 3U) * 4U;
  const size_t remainder = inputBytes % 3U;
  return full + (remainder == 0U ? 0U : remainder + 1U);
}

bool encodeBase64Url(const uint8_t* input, size_t inputBytes, char* output,
                     size_t outputCapacity, size_t& outputBytes) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  outputBytes = 0U;
  if ((!input && inputBytes != 0U) || !output) return false;
  const size_t required = base64UrlEncodedBytes(inputBytes);
  if (required > outputCapacity) return false;
  size_t read = 0U;
  size_t write = 0U;
  while (read + 3U <= inputBytes) {
    const uint32_t value = (static_cast<uint32_t>(input[read]) << 16U) |
                           (static_cast<uint32_t>(input[read + 1U]) << 8U) |
                           static_cast<uint32_t>(input[read + 2U]);
    output[write++] = alphabet[(value >> 18U) & 63U];
    output[write++] = alphabet[(value >> 12U) & 63U];
    output[write++] = alphabet[(value >> 6U) & 63U];
    output[write++] = alphabet[value & 63U];
    read += 3U;
  }
  const size_t remaining = inputBytes - read;
  if (remaining == 1U) {
    const uint32_t value = static_cast<uint32_t>(input[read]) << 16U;
    output[write++] = alphabet[(value >> 18U) & 63U];
    output[write++] = alphabet[(value >> 12U) & 63U];
  } else if (remaining == 2U) {
    const uint32_t value = (static_cast<uint32_t>(input[read]) << 16U) |
                           (static_cast<uint32_t>(input[read + 1U]) << 8U);
    output[write++] = alphabet[(value >> 18U) & 63U];
    output[write++] = alphabet[(value >> 12U) & 63U];
    output[write++] = alphabet[(value >> 6U) & 63U];
  }
  outputBytes = write;
  return write == required;
}

bool decodeBase64Url(const char* input, size_t inputBytes, uint8_t* output,
                     size_t outputCapacity, size_t& outputBytes) {
  outputBytes = 0U;
  if ((!input && inputBytes != 0U) || !output || inputBytes % 4U == 1U) {
    return false;
  }
  const size_t required = (inputBytes / 4U) * 3U +
      (inputBytes % 4U == 0U ? 0U : inputBytes % 4U - 1U);
  if (required > outputCapacity) return false;
  size_t read = 0U;
  size_t write = 0U;
  while (read + 4U <= inputBytes) {
    const int a = base64Value(input[read]);
    const int b = base64Value(input[read + 1U]);
    const int c = base64Value(input[read + 2U]);
    const int d = base64Value(input[read + 3U]);
    if (a < 0 || b < 0 || c < 0 || d < 0) return false;
    const uint32_t value = (static_cast<uint32_t>(a) << 18U) |
                           (static_cast<uint32_t>(b) << 12U) |
                           (static_cast<uint32_t>(c) << 6U) |
                           static_cast<uint32_t>(d);
    output[write++] = static_cast<uint8_t>(value >> 16U);
    output[write++] = static_cast<uint8_t>(value >> 8U);
    output[write++] = static_cast<uint8_t>(value);
    read += 4U;
  }
  const size_t remaining = inputBytes - read;
  if (remaining == 2U) {
    const int a = base64Value(input[read]);
    const int b = base64Value(input[read + 1U]);
    if (a < 0 || b < 0 || (b & 0x0f) != 0) return false;
    output[write++] = static_cast<uint8_t>((a << 2U) | (b >> 4U));
  } else if (remaining == 3U) {
    const int a = base64Value(input[read]);
    const int b = base64Value(input[read + 1U]);
    const int c = base64Value(input[read + 2U]);
    if (a < 0 || b < 0 || c < 0 || (c & 0x03) != 0) return false;
    output[write++] = static_cast<uint8_t>((a << 2U) | (b >> 4U));
    output[write++] = static_cast<uint8_t>((b << 4U) | (c >> 2U));
  }
  outputBytes = write;
  return write == required;
}

bool validUtf8(const uint8_t* input, size_t inputBytes) {
  if (!input && inputBytes != 0U) return false;
  size_t i = 0U;
  while (i < inputBytes) {
    const uint8_t first = input[i++];
    if (first <= 0x7fU) {
      if (first == 0U) return false;
      continue;
    }
    uint8_t continuation = 0U;
    uint32_t codepoint = 0U;
    uint32_t minimum = 0U;
    if (first >= 0xc2U && first <= 0xdfU) {
      continuation = 1U;
      codepoint = first & 0x1fU;
      minimum = 0x80U;
    } else if (first >= 0xe0U && first <= 0xefU) {
      continuation = 2U;
      codepoint = first & 0x0fU;
      minimum = 0x800U;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      continuation = 3U;
      codepoint = first & 0x07U;
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (i + continuation > inputBytes) return false;
    for (uint8_t j = 0U; j < continuation; ++j) {
      const uint8_t next = input[i++];
      if ((next & 0xc0U) != 0x80U) return false;
      codepoint = (codepoint << 6U) | (next & 0x3fU);
    }
    if (codepoint < minimum || codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
      return false;
    }
  }
  return true;
}

ProtocolResult encodeEnvelope(
    EnvelopeChannel channel, uint64_t sequence,
    const uint8_t nonce[kEnvelopeNonceBytes],
    const uint8_t requestId[kRequestIdBytes], const char* operation,
    const uint8_t* payload, size_t payloadBytes,
    const uint8_t key[kEnvelopeKeyBytes], CompanionCrypto& crypto,
    uint8_t* outputJson, size_t outputCapacity, size_t& outputBytes) {
  outputBytes = 0U;
  if (!nonce || !requestId || !operation || !payload || !key ||
      !outputJson) {
    return ProtocolResult::InvalidArgument;
  }
  if (static_cast<uint8_t>(channel) >
      static_cast<uint8_t>(EnvelopeChannel::Event)) {
    return ProtocolResult::InvalidChannel;
  }
  if (sequence == 0U) return ProtocolResult::InvalidSequence;
  const size_t operationBytes = stringLengthBounded(
      operation, kMaximumOperationBytes);
  if (!operationValid(operation, operationBytes)) {
    return ProtocolResult::InvalidOperation;
  }
  if (payloadBytes > UINT32_MAX ||
      payloadBytes > kMaximumEnvelopePayloadBytes) {
    return ProtocolResult::PayloadTooLarge;
  }
  if (!validUtf8(payload, payloadBytes) ||
      !JsonValidator(payload, payloadBytes).valid()) {
    return ProtocolResult::InvalidPayloadJson;
  }

  uint8_t mac[kEnvelopeMacBytes]{};
  const ProtocolResult macResult = calculateEnvelopeMac(
      channel, sequence, nonce, requestId, operation, operationBytes, payload,
      payloadBytes, key, crypto, mac);
  if (macResult != ProtocolResult::Ok) return macResult;

  char sequenceText[21]{};
  const size_t sequenceBytes = formatDecimal(sequence, sequenceText);
  const char channelText = static_cast<char>('0' +
      static_cast<uint8_t>(channel));
  OutputWriter writer(outputJson, outputCapacity);
  const bool written =
      writer.append("{\"v\":1,\"channel\":", 17U) &&
      writer.append(&channelText, 1U) &&
      writer.append(",\"seq\":\"", 8U) &&
      writer.append(sequenceText, sequenceBytes) &&
      writer.append("\",\"nonce_b64\":\"", 15U) &&
      writer.appendBase64(nonce, kEnvelopeNonceBytes) &&
      writer.append("\",\"request_id_b64\":\"", 20U) &&
      writer.appendBase64(requestId, kRequestIdBytes) &&
      writer.append("\",\"op\":\"", 8U) &&
      writer.append(operation, operationBytes) &&
      writer.append("\",\"payload_b64\":\"", 17U) &&
      writer.appendBase64(payload, payloadBytes) &&
      writer.append("\",\"mac_b64\":\"", 13U) &&
      writer.appendBase64(mac, kEnvelopeMacBytes) &&
      writer.append("\"}", 2U);
  secureZero(mac, sizeof(mac));
  if (!written) return ProtocolResult::OutputTooSmall;
  outputBytes = writer.bytes();
  return ProtocolResult::Ok;
}

ProtocolResult decodeAndVerifyEnvelope(
    const uint8_t* json, size_t jsonBytes,
    const uint8_t key[kEnvelopeKeyBytes], EnvelopeChannel expectedChannel,
    uint64_t expectedSequence, CompanionCrypto& crypto,
    DecodedEnvelope& output, uint8_t* payloadOutput,
    size_t payloadCapacity) {
  output = DecodedEnvelope{};
  if (!json || jsonBytes == 0U || !key || !payloadOutput) {
    return ProtocolResult::InvalidArgument;
  }
  if (jsonBytes > kMaximumFrameBytes) return ProtocolResult::PayloadTooLarge;
  Span fields[FieldCount]{};
  ProtocolResult result = parseOuterObject(json, jsonBytes, fields);
  if (result != ProtocolResult::Ok) return result;
  if (fields[FieldVersion].bytes != 1U ||
      fields[FieldVersion].data[0] != '1') {
    return ProtocolResult::UnsupportedVersion;
  }
  if (fields[FieldChannel].bytes != 1U ||
      fields[FieldChannel].data[0] < '0' ||
      fields[FieldChannel].data[0] > '2') {
    return ProtocolResult::InvalidChannel;
  }
  DecodedEnvelope candidate{};
  candidate.version = kProtocolVersion;
  candidate.channel = static_cast<EnvelopeChannel>(
      fields[FieldChannel].data[0] - '0');
  if (candidate.channel != expectedChannel) {
    return ProtocolResult::InvalidChannel;
  }
  if (!parseDecimal(fields[FieldSequence].data,
                    fields[FieldSequence].bytes, candidate.sequence)) {
    return ProtocolResult::InvalidSequence;
  }
  if (expectedSequence != 0U && candidate.sequence != expectedSequence) {
    return ProtocolResult::UnexpectedSequence;
  }

  size_t decoded = 0U;
  if (fields[FieldNonce].bytes != 22U ||
      !decodeBase64Url(reinterpret_cast<const char*>(
                           fields[FieldNonce].data),
                       fields[FieldNonce].bytes, candidate.nonce,
                       sizeof(candidate.nonce), decoded) ||
      decoded != sizeof(candidate.nonce)) {
    return ProtocolResult::InvalidBase64;
  }
  if (fields[FieldRequestId].bytes != 22U ||
      !decodeBase64Url(reinterpret_cast<const char*>(
                           fields[FieldRequestId].data),
                       fields[FieldRequestId].bytes, candidate.requestId,
                       sizeof(candidate.requestId), decoded) ||
      decoded != sizeof(candidate.requestId)) {
    return ProtocolResult::InvalidBase64;
  }
  if (!operationValid(reinterpret_cast<const char*>(
                          fields[FieldOperation].data),
                      fields[FieldOperation].bytes)) {
    return ProtocolResult::InvalidOperation;
  }
  memcpy(candidate.operation, fields[FieldOperation].data,
         fields[FieldOperation].bytes);
  candidate.operation[fields[FieldOperation].bytes] = '\0';

  const size_t payloadRemainder = fields[FieldPayload].bytes % 4U;
  const size_t maximumDecoded =
      (fields[FieldPayload].bytes / 4U) * 3U +
      (payloadRemainder == 0U ? 0U : payloadRemainder - 1U);
  if (maximumDecoded > kMaximumEnvelopePayloadBytes) {
    return ProtocolResult::PayloadTooLarge;
  }
  if (maximumDecoded > payloadCapacity) return ProtocolResult::PayloadTooLarge;
  if (!decodeBase64Url(reinterpret_cast<const char*>(
                           fields[FieldPayload].data),
                       fields[FieldPayload].bytes, payloadOutput,
                       payloadCapacity, candidate.payloadBytes)) {
    return ProtocolResult::InvalidBase64;
  }
  uint8_t suppliedMac[kEnvelopeMacBytes]{};
  if (fields[FieldMac].bytes != 43U ||
      !decodeBase64Url(reinterpret_cast<const char*>(fields[FieldMac].data),
                       fields[FieldMac].bytes, suppliedMac,
                       sizeof(suppliedMac), decoded) ||
      decoded != sizeof(suppliedMac)) {
    secureZero(payloadOutput, candidate.payloadBytes);
    return ProtocolResult::InvalidBase64;
  }
  uint8_t expectedMac[kEnvelopeMacBytes]{};
  result = calculateEnvelopeMac(
      candidate.channel, candidate.sequence, candidate.nonce,
      candidate.requestId, candidate.operation,
      fields[FieldOperation].bytes, payloadOutput, candidate.payloadBytes,
      key, crypto, expectedMac);
  const bool authenticated = result == ProtocolResult::Ok &&
      constantTimeEqual(suppliedMac, expectedMac, sizeof(suppliedMac));
  secureZero(suppliedMac, sizeof(suppliedMac));
  secureZero(expectedMac, sizeof(expectedMac));
  if (result != ProtocolResult::Ok) {
    secureZero(payloadOutput, candidate.payloadBytes);
    return result;
  }
  if (!authenticated) {
    secureZero(payloadOutput, candidate.payloadBytes);
    return ProtocolResult::AuthenticationFailed;
  }
  if (!validUtf8(payloadOutput, candidate.payloadBytes)) {
    secureZero(payloadOutput, candidate.payloadBytes);
    return ProtocolResult::InvalidPayloadUtf8;
  }
  if (!JsonValidator(payloadOutput, candidate.payloadBytes).valid()) {
    secureZero(payloadOutput, candidate.payloadBytes);
    return ProtocolResult::InvalidPayloadJson;
  }
  output = candidate;
  return ProtocolResult::Ok;
}

ProtocolResult makeHandshakeProof(
    const uint8_t root[kEnvelopeKeyBytes], const char* role,
    const uint8_t controllerId[16], const uint8_t clientNonce[16],
    const uint8_t deviceNonce[16], CompanionCrypto& crypto,
    uint8_t output[kEnvelopeMacBytes]) {
  if (!root || !roleAllowed(role) || !controllerId || !clientNonce ||
      !deviceNonce || !output) {
    return ProtocolResult::InvalidArgument;
  }
  static const uint8_t domain[] = {'K', 'I', 'T', 'S', 'U', '-', 'H', 'S',
                                   '-', '1', 0};
  const uint8_t separator = 0U;
  const CryptoPart parts[] = {
      {domain, sizeof(domain)},
      {reinterpret_cast<const uint8_t*>(role), strlen(role)},
      {&separator, 1U},
      {controllerId, 16U},
      {clientNonce, 16U},
      {deviceNonce, 16U},
  };
  return crypto.hmacSha256(root, parts, sizeof(parts) / sizeof(parts[0]),
                           output)
      ? ProtocolResult::Ok
      : ProtocolResult::CryptoFailed;
}

ProtocolResult deriveBleSessionKeys(
    const uint8_t root[kEnvelopeKeyBytes], const uint8_t clientNonce[16],
    const uint8_t deviceNonce[16], CompanionCrypto& crypto,
    uint8_t clientToDevice[kEnvelopeKeyBytes],
    uint8_t deviceToClient[kEnvelopeKeyBytes]) {
  if (!root || !clientNonce || !deviceNonce || !clientToDevice ||
      !deviceToClient) {
    return ProtocolResult::InvalidArgument;
  }
  static const uint8_t c2dInfo[] = "kitsu868/ble/c2d/v1";
  static const uint8_t d2cInfo[] = "kitsu868/ble/d2c/v1";
  uint8_t salt[32]{};
  const CryptoPart nonceParts[] = {{clientNonce, 16U}, {deviceNonce, 16U}};
  if (!crypto.sha256(nonceParts, 2U, salt) ||
      !crypto.hkdfSha256(root, salt, sizeof(salt), c2dInfo,
                         sizeof(c2dInfo) - 1U, clientToDevice) ||
      !crypto.hkdfSha256(root, salt, sizeof(salt), d2cInfo,
                         sizeof(d2cInfo) - 1U, deviceToClient)) {
    secureZero(salt, sizeof(salt));
    secureZero(clientToDevice, kEnvelopeKeyBytes);
    secureZero(deviceToClient, kEnvelopeKeyBytes);
    return ProtocolResult::CryptoFailed;
  }
  secureZero(salt, sizeof(salt));
  return ProtocolResult::Ok;
}

ProtocolResult makePairingProof(
    const uint8_t root[kEnvelopeKeyBytes], const char* role,
    const uint8_t controllerId[16], const char deviceUid[7],
    const uint8_t clientNonce[16], const uint8_t deviceNonce[16],
    CompanionCrypto& crypto, uint8_t output[kEnvelopeMacBytes]) {
  if (!root || !roleAllowed(role) || !controllerId || !deviceUid ||
      !clientNonce || !deviceNonce || !output) {
    return ProtocolResult::InvalidArgument;
  }
  if (deviceUid[0] != 'K' || deviceUid[1] != 'T' ||
      deviceUid[6] != '\0') {
    return ProtocolResult::InvalidArgument;
  }
  for (size_t i = 2U; i < 6U; ++i) {
    const char c = deviceUid[i];
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
      return ProtocolResult::InvalidArgument;
    }
  }
  static const uint8_t domain[] = {'K', 'I', 'T', 'S', 'U', '-', 'P', 'A',
                                   'I', 'R', '-', '1', 0};
  const uint8_t separator = 0U;
  const CryptoPart parts[] = {
      {domain, sizeof(domain)},
      {reinterpret_cast<const uint8_t*>(role), strlen(role)},
      {&separator, 1U},
      {controllerId, 16U},
      {root, kEnvelopeKeyBytes},
      {reinterpret_cast<const uint8_t*>(deviceUid), 6U},
      {clientNonce, 16U},
      {deviceNonce, 16U},
  };
  return crypto.hmacSha256(root, parts, sizeof(parts) / sizeof(parts[0]),
                           output)
      ? ProtocolResult::Ok
      : ProtocolResult::CryptoFailed;
}

}  // namespace companion
}  // namespace kitsu868
