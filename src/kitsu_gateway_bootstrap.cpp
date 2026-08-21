#include "kitsu_gateway_bootstrap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>

namespace kitsu868 {
namespace connectivity {
namespace {

constexpr size_t kMaximumObjectFields = 8U;
constexpr char kHpkeSuite[] =
    "DHKEM(P-256,HKDF-SHA256)/HKDF-SHA256/AES-256-GCM";

void secureZero(void* memory, size_t bytes) {
  volatile uint8_t* output = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *output++ = 0U;
}

class ScopedZeroBuffer {
 public:
  explicit ScopedZeroBuffer(size_t bytes = 0U) { allocate(bytes); }

  ~ScopedZeroBuffer() {
    if (data_) {
      secureZero(data_, bytes_);
      free(data_);
    }
  }

  ScopedZeroBuffer(const ScopedZeroBuffer&) = delete;
  ScopedZeroBuffer& operator=(const ScopedZeroBuffer&) = delete;

  bool allocate(size_t bytes) {
    reset();
    bytes_ = bytes;
    if (bytes_ == 0U) return true;
    data_ = static_cast<uint8_t*>(malloc(bytes_));
    if (data_) memset(data_, 0, bytes_);
    return data_ != nullptr;
  }

  void reset() {
    if (data_) {
      secureZero(data_, bytes_);
      free(data_);
    }
    data_ = nullptr;
    bytes_ = 0U;
  }

  bool ready() const { return data_ != nullptr; }
  uint8_t* data() { return data_; }
  size_t bytes() const { return bytes_; }

 private:
  uint8_t* data_ = nullptr;
  size_t bytes_ = 0U;
};

bool allZero(const uint8_t* input, size_t bytes) {
  uint8_t combined = 0U;
  for (size_t i = 0U; i < bytes; ++i) combined |= input[i];
  return combined == 0U;
}

struct Span {
  constexpr Span(const uint8_t* input = nullptr, size_t inputBytes = 0U)
      : data(input), bytes(inputBytes) {}
  const uint8_t* data = nullptr;
  size_t bytes = 0U;
};

enum class JsonKind : uint8_t { String = 0, Unsigned, Boolean, Object, Array };

struct JsonValue {
  JsonKind kind = JsonKind::String;
  Span span{};
  bool boolean = false;
};

struct JsonField {
  Span key{};
  JsonValue value{};
};

void skipWhitespace(const uint8_t* json, size_t bytes, size_t& cursor) {
  while (cursor < bytes &&
         (json[cursor] == ' ' || json[cursor] == '\t' ||
          json[cursor] == '\r' || json[cursor] == '\n')) {
    ++cursor;
  }
}

bool parseString(const uint8_t* json, size_t bytes, size_t& cursor,
                 Span& output) {
  skipWhitespace(json, bytes, cursor);
  if (cursor >= bytes || json[cursor++] != '"') return false;
  const size_t start = cursor;
  while (cursor < bytes && json[cursor] != '"') {
    const uint8_t value = json[cursor++];
    // Frozen bootstrap wrappers contain canonical ASCII protocol strings,
    // UUIDs and base64url.  Escapes create alternate spellings and are denied.
    if (value < 0x20U || value >= 0x80U || value == '\\') return false;
  }
  if (cursor >= bytes) return false;
  output.data = json + start;
  output.bytes = cursor - start;
  ++cursor;
  return true;
}

bool spanEquals(const Span& span, const char* expected) {
  const size_t expectedBytes = expected ? strlen(expected) : 0U;
  return span.bytes == expectedBytes &&
         memcmp(span.data, expected, expectedBytes) == 0;
}

bool sameSpan(const Span& left, const Span& right) {
  return left.bytes == right.bytes &&
         memcmp(left.data, right.data, left.bytes) == 0;
}

bool parseValue(const uint8_t* json, size_t bytes, size_t& cursor,
                JsonValue& output, uint8_t depth);

bool parseObject(const uint8_t* json, size_t bytes, size_t& cursor,
                 JsonField* fields, size_t capacity, size_t& count,
                 uint8_t depth) {
  count = 0U;
  skipWhitespace(json, bytes, cursor);
  if (depth > 4U || cursor >= bytes || json[cursor++] != '{') return false;
  skipWhitespace(json, bytes, cursor);
  if (cursor < bytes && json[cursor] == '}') {
    ++cursor;
    return true;
  }
  while (cursor < bytes) {
    if (count >= capacity ||
        !parseString(json, bytes, cursor, fields[count].key) ||
        fields[count].key.bytes == 0U) {
      return false;
    }
    for (size_t i = 0U; i < count; ++i) {
      if (sameSpan(fields[i].key, fields[count].key)) return false;
    }
    skipWhitespace(json, bytes, cursor);
    if (cursor >= bytes || json[cursor++] != ':') return false;
    if (!parseValue(json, bytes, cursor, fields[count].value,
                    static_cast<uint8_t>(depth + 1U))) {
      return false;
    }
    ++count;
    skipWhitespace(json, bytes, cursor);
    if (cursor >= bytes) return false;
    if (json[cursor] == '}') {
      ++cursor;
      return true;
    }
    if (json[cursor++] != ',') return false;
  }
  return false;
}

bool parseArray(const uint8_t* json, size_t bytes, size_t& cursor,
                uint8_t depth) {
  skipWhitespace(json, bytes, cursor);
  if (depth > 4U || cursor >= bytes || json[cursor++] != '[') return false;
  skipWhitespace(json, bytes, cursor);
  if (cursor < bytes && json[cursor] == ']') {
    ++cursor;
    return true;
  }
  size_t elements = 0U;
  while (cursor < bytes) {
    if (++elements > kEnrollmentMaximumChainCertificates) return false;
    JsonValue ignored{};
    if (!parseValue(json, bytes, cursor, ignored,
                    static_cast<uint8_t>(depth + 1U))) {
      return false;
    }
    skipWhitespace(json, bytes, cursor);
    if (cursor >= bytes) return false;
    if (json[cursor] == ']') {
      ++cursor;
      return true;
    }
    if (json[cursor++] != ',') return false;
  }
  return false;
}

bool parseValue(const uint8_t* json, size_t bytes, size_t& cursor,
                JsonValue& output, uint8_t depth) {
  skipWhitespace(json, bytes, cursor);
  if (cursor >= bytes || depth > 4U) return false;
  if (json[cursor] == '"') {
    output.kind = JsonKind::String;
    return parseString(json, bytes, cursor, output.span);
  }
  if (json[cursor] == '{') {
    const size_t start = cursor;
    JsonField nested[kMaximumObjectFields]{};
    size_t count = 0U;
    if (!parseObject(json, bytes, cursor, nested,
                     kMaximumObjectFields, count, depth)) {
      return false;
    }
    output.kind = JsonKind::Object;
    output.span = Span{json + start, cursor - start};
    return true;
  }
  if (json[cursor] == '[') {
    const size_t start = cursor;
    if (!parseArray(json, bytes, cursor, depth)) return false;
    output.kind = JsonKind::Array;
    output.span = Span{json + start, cursor - start};
    return true;
  }
  if (cursor + 4U <= bytes && memcmp(json + cursor, "true", 4U) == 0) {
    output.kind = JsonKind::Boolean;
    output.boolean = true;
    output.span = Span{json + cursor, 4U};
    cursor += 4U;
    return true;
  }
  if (cursor + 5U <= bytes && memcmp(json + cursor, "false", 5U) == 0) {
    output.kind = JsonKind::Boolean;
    output.boolean = false;
    output.span = Span{json + cursor, 5U};
    cursor += 5U;
    return true;
  }
  const size_t start = cursor;
  while (cursor < bytes && json[cursor] >= '0' && json[cursor] <= '9') {
    ++cursor;
  }
  if (cursor == start || (cursor - start > 1U && json[start] == '0')) {
    return false;
  }
  output.kind = JsonKind::Unsigned;
  output.span = Span{json + start, cursor - start};
  return true;
}

bool parseRootObject(const uint8_t* json, size_t bytes, JsonField* fields,
                     size_t capacity, size_t& count) {
  if (!json || bytes == 0U || !companion::validUtf8(json, bytes)) return false;
  size_t cursor = 0U;
  if (!parseObject(json, bytes, cursor, fields, capacity, count, 0U)) {
    return false;
  }
  skipWhitespace(json, bytes, cursor);
  return cursor == bytes;
}

const JsonField* field(const JsonField* fields, size_t count,
                       const char* name) {
  for (size_t i = 0U; i < count; ++i) {
    if (spanEquals(fields[i].key, name)) return fields + i;
  }
  return nullptr;
}

bool exactFields(const JsonField* fields, size_t count,
                 const char* const* names, size_t nameCount) {
  if (count != nameCount) return false;
  for (size_t i = 0U; i < nameCount; ++i) {
    if (!field(fields, count, names[i])) return false;
  }
  return true;
}

bool stringEquals(const JsonField* value, const char* expected) {
  return value && value->value.kind == JsonKind::String &&
         spanEquals(value->value.span, expected);
}

bool versionOne(const JsonField* fields, size_t count) {
  const JsonField* version = field(fields, count, "v");
  return version && version->value.kind == JsonKind::Unsigned &&
         version->value.span.bytes == 1U &&
         version->value.span.data[0] == '1';
}

int hexNibble(uint8_t value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return -1;
}

bool parseCanonicalUuid(const Span& value,
                        uint8_t output[kEnrollmentUuidBytes]) {
  if (!value.data || value.bytes != 36U || !output) return false;
  size_t source = 0U;
  size_t target = 0U;
  while (source < value.bytes) {
    if (source == 8U || source == 13U || source == 18U || source == 23U) {
      if (value.data[source++] != '-') return false;
      continue;
    }
    if (source + 1U >= value.bytes || target >= kEnrollmentUuidBytes) {
      return false;
    }
    const int high = hexNibble(value.data[source++]);
    const int low = hexNibble(value.data[source++]);
    if (high < 0 || low < 0) return false;
    output[target++] = static_cast<uint8_t>((high << 4U) | low);
  }
  return target == kEnrollmentUuidBytes &&
         !allZero(output, kEnrollmentUuidBytes);
}

void formatUuid(const uint8_t input[kEnrollmentUuidBytes], char output[37]) {
  static const char hex[] = "0123456789abcdef";
  size_t cursor = 0U;
  for (size_t i = 0U; i < kEnrollmentUuidBytes; ++i) {
    if (i == 4U || i == 6U || i == 8U || i == 10U) output[cursor++] = '-';
    output[cursor++] = hex[input[i] >> 4U];
    output[cursor++] = hex[input[i] & 0x0fU];
  }
  output[cursor] = '\0';
}

bool parseU32(const JsonField* value, uint32_t& output) {
  if (!value || value->value.kind != JsonKind::Unsigned ||
      value->value.span.bytes == 0U || value->value.span.bytes > 10U) {
    return false;
  }
  uint64_t parsed = 0U;
  for (size_t i = 0U; i < value->value.span.bytes; ++i) {
    const uint8_t digit = value->value.span.data[i];
    if (digit < '0' || digit > '9') return false;
    parsed = parsed * 10U + static_cast<uint8_t>(digit - '0');
    if (parsed > UINT32_MAX) return false;
  }
  output = static_cast<uint32_t>(parsed);
  return true;
}

bool canonicalBase64Alphabet(const Span& encoded) {
  if (!encoded.data || encoded.bytes == 0U) return false;
  for (size_t i = 0U; i < encoded.bytes; ++i) {
    const uint8_t value = encoded.data[i];
    if (!((value >= 'A' && value <= 'Z') ||
          (value >= 'a' && value <= 'z') ||
          (value >= '0' && value <= '9') || value == '-' || value == '_')) {
      return false;
    }
  }
  return true;
}

bool decodeBase64(const JsonField* value, uint8_t* output, size_t capacity,
                  size_t& outputBytes) {
  outputBytes = 0U;
  if (!value || value->value.kind != JsonKind::String ||
      !canonicalBase64Alphabet(value->value.span)) {
    return false;
  }
  return companion::decodeBase64Url(
      reinterpret_cast<const char*>(value->value.span.data),
      value->value.span.bytes, output, capacity, outputBytes);
}

bool validFqdn(const char* value) {
  if (!value) return false;
  const size_t bytes = strlen(value);
  if (bytes == 0U || bytes > 253U || value[0] == '.' ||
      value[bytes - 1U] == '.') {
    return false;
  }
  size_t labelBytes = 0U;
  bool hasDot = false;
  bool hasNonNumericLabelByte = false;
  for (size_t i = 0U; i < bytes; ++i) {
    const char c = value[i];
    if (c == '.') {
      if (labelBytes == 0U || labelBytes > 63U || value[i - 1U] == '-') {
        return false;
      }
      labelBytes = 0U;
      hasDot = true;
      continue;
    }
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-')) {
      return false;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-') {
      hasNonNumericLabelByte = true;
    }
    if (labelBytes == 0U && c == '-') return false;
    ++labelBytes;
  }
  return hasDot && hasNonNumericLabelByte && labelBytes != 0U &&
         labelBytes <= 63U &&
         value[bytes - 1U] != '-';
}

bool validIpv4(const char* value) {
  if (!value) return false;
  const size_t bytes = strlen(value);
  if (bytes < 7U || bytes > 15U) return false;
  size_t start = 0U;
  uint8_t groups = 0U;
  for (size_t i = 0U; i <= bytes; ++i) {
    if (i != bytes && value[i] != '.') continue;
    const size_t count = i - start;
    if (count == 0U || count > 3U ||
        (count > 1U && value[start] == '0')) return false;
    uint16_t part = 0U;
    for (size_t j = start; j < i; ++j) {
      if (value[j] < '0' || value[j] > '9') return false;
      part = static_cast<uint16_t>(part * 10U + value[j] - '0');
    }
    if (part > 255U) return false;
    ++groups;
    start = i + 1U;
  }
  return groups == 4U;
}

bool validIpv6(const char* value) {
  if (!value) return false;
  const size_t bytes = strlen(value);
  if (bytes < 2U || bytes > 45U) return false;
  bool compressed = false;
  for (size_t i = 0U; i + 1U < bytes; ++i) {
    if (i + 2U < bytes && value[i] == ':' && value[i + 1U] == ':' &&
        value[i + 2U] == ':') return false;
    if (value[i] == ':' && value[i + 1U] == ':') {
      if (compressed) return false;
      compressed = true;
      ++i;
    }
  }
  if ((value[0] == ':' && value[1] != ':') ||
      (value[bytes - 1U] == ':' && value[bytes - 2U] != ':')) return false;
  uint8_t groups = 0U;
  size_t start = 0U;
  while (start < bytes) {
    if (value[start] == ':') {
      ++start;
      continue;
    }
    size_t end = start;
    while (end < bytes && value[end] != ':') ++end;
    bool dotted = false;
    for (size_t i = start; i < end; ++i) dotted = dotted || value[i] == '.';
    if (dotted) {
      char embedded[16]{};
      if (end != bytes || end - start >= sizeof(embedded)) return false;
      memcpy(embedded, value + start, end - start);
      if (!validIpv4(embedded)) return false;
      groups = static_cast<uint8_t>(groups + 2U);
    } else {
      if (end - start == 0U || end - start > 4U) return false;
      for (size_t i = start; i < end; ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
      }
      ++groups;
    }
    start = end + 1U;
  }
  return compressed ? groups < 8U : groups == 8U;
}

bool validEndpointHost(const char* value) {
  return validIpv4(value) || validIpv6(value) || validFqdn(value);
}

bool validTrust(const GatewayBootstrapTrust& trust) {
  return validEndpointHost(trust.host) && validFqdn(trust.serverName) &&
         trust.port != 0U && trust.caCertificateDer &&
         trust.caCertificateBytes != 0U &&
         trust.caCertificateBytes <= kGatewayBootstrapMaximumCaBytes &&
         !allZero(trust.spkiSha256, sizeof(trust.spkiSha256)) &&
         !allZero(trust.gatewayUuid, sizeof(trust.gatewayUuid));
}

bool parseCertificateChain(const JsonValue& array,
                           GatewayBootstrapWorkspace& workspace,
                           EnrollmentResponse& output) {
  if (array.kind != JsonKind::Array || !array.span.data ||
      array.span.bytes < 2U) {
    return false;
  }
  size_t cursor = 0U;
  skipWhitespace(array.span.data, array.span.bytes, cursor);
  if (cursor >= array.span.bytes || array.span.data[cursor++] != '[') {
    return false;
  }
  size_t count = 0U;
  for (;;) {
    skipWhitespace(array.span.data, array.span.bytes, cursor);
    if (cursor < array.span.bytes && array.span.data[cursor] == ']') {
      ++cursor;
      break;
    }
    if (count >= kEnrollmentMaximumChainCertificates) return false;
    Span encoded{};
    if (!parseString(array.span.data, array.span.bytes, cursor, encoded) ||
        !canonicalBase64Alphabet(encoded)) {
      return false;
    }
    size_t decoded = 0U;
    if (!companion::decodeBase64Url(
            reinterpret_cast<const char*>(encoded.data), encoded.bytes,
            workspace.chainCertificates[count],
            sizeof(workspace.chainCertificates[count]), decoded) ||
        decoded == 0U) {
      return false;
    }
    output.certificateChainDer[count] = workspace.chainCertificates[count];
    output.certificateChainBytes[count] = decoded;
    ++count;
    skipWhitespace(array.span.data, array.span.bytes, cursor);
    if (cursor >= array.span.bytes) return false;
    if (array.span.data[cursor] == ']') {
      ++cursor;
      break;
    }
    if (array.span.data[cursor++] != ',') return false;
  }
  skipWhitespace(array.span.data, array.span.bytes, cursor);
  if (cursor != array.span.bytes || count == 0U) return false;
  output.certificateChainCount = count;
  return true;
}

}  // namespace

const char* gatewayBootstrapResultName(GatewayBootstrapResult result) {
  switch (result) {
    case GatewayBootstrapResult::ReconnectSteady: return "reconnect_steady";
    case GatewayBootstrapResult::InProgress: return "in_progress";
    case GatewayBootstrapResult::NotActive: return "not_active";
    case GatewayBootstrapResult::InvalidArgument: return "invalid_argument";
    case GatewayBootstrapResult::RemoteConnectivityUnavailable:
      return "remote_connectivity_unavailable";
    case GatewayBootstrapResult::TimeUnavailable: return "time_unavailable";
    case GatewayBootstrapResult::TrustRejected: return "trust_rejected";
    case GatewayBootstrapResult::TransportFailed: return "transport_failed";
    case GatewayBootstrapResult::ProxyMalformed: return "proxy_malformed";
    case GatewayBootstrapResult::ProxyRejected: return "proxy_rejected";
    case GatewayBootstrapResult::BackendMalformed: return "backend_malformed";
    case GatewayBootstrapResult::EnrollmentFailed: return "enrollment_failed";
  }
  return "invalid_argument";
}

bool GatewayBootstrapFrameParser::begin(uint8_t* storage,
                                        size_t storageBytes,
                                        uint32_t timeoutMs) {
  storage_ = storage;
  storageBytes_ = storageBytes;
  timeoutMs_ = timeoutMs;
  consume();
  return storage_ &&
         storageBytes_ >= kGatewayBootstrapMaximumProxyResponseBytes &&
         timeoutMs_ != 0U;
}

companion::FrameResult GatewayBootstrapFrameParser::feed(
    const uint8_t* input, size_t inputBytes, uint32_t nowMillis) {
  if ((!input && inputBytes != 0U) || !storage_ || timeoutMs_ == 0U) {
    return companion::FrameResult::InvalidArgument;
  }
  if (ready_) {
    return inputBytes == 0U ? companion::FrameResult::Ready
                            : companion::FrameResult::PipelinedFrame;
  }
  if (started_ &&
      static_cast<uint32_t>(nowMillis - startedAt_) >= timeoutMs_) {
    consume();
    return companion::FrameResult::TimedOut;
  }
  size_t cursor = 0U;
  if (!started_ && inputBytes != 0U) {
    started_ = true;
    startedAt_ = nowMillis;
  }
  while (cursor < inputBytes && headerBytes_ < sizeof(header_)) {
    header_[headerBytes_++] = input[cursor++];
  }
  if (headerBytes_ == sizeof(header_) && expectedBytes_ == 0U) {
    const uint32_t declared =
        (static_cast<uint32_t>(header_[0]) << 24U) |
        (static_cast<uint32_t>(header_[1]) << 16U) |
        (static_cast<uint32_t>(header_[2]) << 8U) |
        static_cast<uint32_t>(header_[3]);
    if (declared == 0U) {
      consume();
      return companion::FrameResult::EmptyFrame;
    }
    if (declared > kGatewayBootstrapMaximumProxyResponseBytes) {
      consume();
      return companion::FrameResult::Oversize;
    }
    if (declared > storageBytes_) {
      consume();
      return companion::FrameResult::OutputTooSmall;
    }
    expectedBytes_ = declared;
  }
  if (expectedBytes_ != 0U && cursor < inputBytes) {
    const size_t remaining = expectedBytes_ - receivedBytes_;
    const size_t available = inputBytes - cursor;
    const size_t copying = remaining < available ? remaining : available;
    memcpy(storage_ + receivedBytes_, input + cursor, copying);
    receivedBytes_ += copying;
    cursor += copying;
  }
  if (expectedBytes_ != 0U && receivedBytes_ == expectedBytes_) {
    ready_ = true;
    if (cursor != inputBytes) return companion::FrameResult::PipelinedFrame;
    return companion::FrameResult::Ready;
  }
  return companion::FrameResult::NeedMore;
}

companion::FrameResult GatewayBootstrapFrameParser::poll(
    uint32_t nowMillis) const {
  if (ready_) return companion::FrameResult::Ready;
  if (started_ &&
      static_cast<uint32_t>(nowMillis - startedAt_) >= timeoutMs_) {
    return companion::FrameResult::TimedOut;
  }
  return companion::FrameResult::NeedMore;
}

bool GatewayBootstrapFrameParser::frame(const uint8_t*& output,
                                        size_t& outputBytes) const {
  output = nullptr;
  outputBytes = 0U;
  if (!ready_) return false;
  output = storage_;
  outputBytes = expectedBytes_;
  return true;
}

void GatewayBootstrapFrameParser::consume() {
  memset(header_, 0, sizeof(header_));
  headerBytes_ = 0U;
  expectedBytes_ = 0U;
  receivedBytes_ = 0U;
  startedAt_ = 0U;
  started_ = false;
  ready_ = false;
}

bool encodeGatewayBootstrapProxyRequest(
    const uint8_t enrollmentUuid[kEnrollmentUuidBytes],
    const uint8_t* backendRequest, size_t backendRequestBytes,
    uint8_t* output, size_t outputCapacity, size_t& outputBytes) {
  outputBytes = 0U;
  if (!enrollmentUuid || allZero(enrollmentUuid, kEnrollmentUuidBytes) ||
      !backendRequest || backendRequestBytes == 0U ||
      backendRequestBytes > kEnrollmentMaximumRequestBytes || !output) {
    return false;
  }
  char uuid[37]{};
  formatUuid(enrollmentUuid, uuid);
  const int prefixBytes = snprintf(
      reinterpret_cast<char*>(output), outputCapacity,
      "{\"v\":1,\"type\":\"device_enrollment\",\"enrollment_id\":\"%s\"," 
      "\"request_b64\":\"", uuid);
  if (prefixBytes <= 0 || static_cast<size_t>(prefixBytes) >= outputCapacity) {
    return false;
  }
  size_t encodedBytes = 0U;
  if (!companion::encodeBase64Url(
          backendRequest, backendRequestBytes,
          reinterpret_cast<char*>(output + prefixBytes),
          outputCapacity - static_cast<size_t>(prefixBytes), encodedBytes)) {
    return false;
  }
  size_t used = static_cast<size_t>(prefixBytes) + encodedBytes;
  if (used + 2U > outputCapacity ||
      used + 2U > kGatewayBootstrapMaximumProxyRequestBytes) {
    return false;
  }
  output[used++] = '"';
  output[used++] = '}';
  outputBytes = used;
  return true;
}

GatewayBootstrapResult decodeGatewayBootstrapProxyResponse(
    const uint8_t* json, size_t jsonBytes,
    const uint8_t expectedEnrollmentUuid[kEnrollmentUuidBytes],
    uint8_t* backendResponse, size_t backendResponseCapacity,
    size_t& backendResponseBytes) {
  backendResponseBytes = 0U;
  if (!json || jsonBytes == 0U ||
      jsonBytes > kGatewayBootstrapMaximumProxyResponseBytes ||
      !expectedEnrollmentUuid || !backendResponse) {
    return GatewayBootstrapResult::InvalidArgument;
  }
  JsonField fields[kMaximumObjectFields]{};
  size_t count = 0U;
  if (!parseRootObject(json, jsonBytes, fields, kMaximumObjectFields, count) ||
      !versionOne(fields, count) ||
      !stringEquals(field(fields, count, "type"),
                    "device_enrollment_result")) {
    return GatewayBootstrapResult::ProxyMalformed;
  }
  const JsonField* ok = field(fields, count, "ok");
  if (!ok || ok->value.kind != JsonKind::Boolean) {
    return GatewayBootstrapResult::ProxyMalformed;
  }
  if (!ok->value.boolean) {
    static const char* const failure[] = {"v", "type", "ok", "error"};
    if (!exactFields(fields, count, failure,
                     sizeof(failure) / sizeof(failure[0])) ||
        !field(fields, count, "error") ||
        field(fields, count, "error")->value.kind != JsonKind::String ||
        field(fields, count, "error")->value.span.bytes == 0U) {
      return GatewayBootstrapResult::ProxyMalformed;
    }
    return GatewayBootstrapResult::ProxyRejected;
  }
  static const char* const success[] = {
      "v", "type", "ok", "enrollment_id", "response_b64"};
  if (!exactFields(fields, count, success,
                   sizeof(success) / sizeof(success[0]))) {
    return GatewayBootstrapResult::ProxyMalformed;
  }
  const JsonField* enrollment = field(fields, count, "enrollment_id");
  uint8_t parsedEnrollment[kEnrollmentUuidBytes]{};
  if (!enrollment || enrollment->value.kind != JsonKind::String ||
      !parseCanonicalUuid(enrollment->value.span, parsedEnrollment) ||
      memcmp(parsedEnrollment, expectedEnrollmentUuid,
             kEnrollmentUuidBytes) != 0) {
    return GatewayBootstrapResult::ProxyMalformed;
  }
  const JsonField* encodedResponse = field(fields, count, "response_b64");
  if (!encodedResponse || encodedResponse->value.kind != JsonKind::String ||
      !canonicalBase64Alphabet(encodedResponse->value.span)) {
    return GatewayBootstrapResult::ProxyMalformed;
  }
  // The bootstrap runner intentionally decodes response_b64 to the beginning
  // of the same 64 KiB response allocation. The decoder consumes four bytes
  // for every three it writes, and the JSON prefix places the source strictly
  // after the destination, so this forward overlap is safe. Any other overlap
  // shape is rejected.
  const uintptr_t outputBegin = reinterpret_cast<uintptr_t>(backendResponse);
  const uintptr_t outputEnd = outputBegin + backendResponseCapacity;
  const uintptr_t inputBegin =
      reinterpret_cast<uintptr_t>(encodedResponse->value.span.data);
  const uintptr_t inputEnd = inputBegin + encodedResponse->value.span.bytes;
  const bool overlaps = outputBegin < inputEnd && inputBegin < outputEnd;
  if (overlaps && outputBegin > inputBegin) {
    return GatewayBootstrapResult::ProxyMalformed;
  }
  if (!overlaps) secureZero(backendResponse, backendResponseCapacity);
  size_t decoded = 0U;
  if (!companion::decodeBase64Url(
          reinterpret_cast<const char*>(encodedResponse->value.span.data),
          encodedResponse->value.span.bytes, backendResponse,
          backendResponseCapacity, decoded) ||
      decoded == 0U ||
      decoded > kGatewayBootstrapMaximumBackendResponseBytes ||
      !companion::validUtf8(backendResponse, decoded)) {
    secureZero(backendResponse, backendResponseCapacity);
    return GatewayBootstrapResult::ProxyMalformed;
  }
  backendResponseBytes = decoded;
  return GatewayBootstrapResult::ReconnectSteady;
}

GatewayBootstrapResult decodeBackendEnrollmentResponse(
    const uint8_t* json, size_t jsonBytes,
    const uint8_t enrollmentUuid[kEnrollmentUuidBytes],
    const uint8_t expectedGatewayUuid[kEnrollmentUuidBytes],
    GatewayBootstrapWorkspace& workspace, EnrollmentResponse& output) {
  output = EnrollmentResponse{};
  if (!json || jsonBytes == 0U ||
      jsonBytes > kGatewayBootstrapMaximumBackendResponseBytes ||
      !enrollmentUuid || !expectedGatewayUuid) {
    return GatewayBootstrapResult::InvalidArgument;
  }
  JsonField fields[kMaximumObjectFields]{};
  size_t count = 0U;
  static const char* const schema[] = {
      "companion_id", "gateway_id", "key_version",
      "device_certificate_der_b64", "device_certificate_chain_der_b64",
      "sealed_secret"};
  if (!parseRootObject(json, jsonBytes, fields, kMaximumObjectFields, count) ||
      !exactFields(fields, count, schema,
                   sizeof(schema) / sizeof(schema[0]))) {
    return GatewayBootstrapResult::BackendMalformed;
  }
  const JsonField* companionId = field(fields, count, "companion_id");
  const JsonField* gatewayId = field(fields, count, "gateway_id");
  if (!companionId || companionId->value.kind != JsonKind::String ||
      !gatewayId || gatewayId->value.kind != JsonKind::String ||
      !parseCanonicalUuid(companionId->value.span, output.companionUuid) ||
      !parseCanonicalUuid(gatewayId->value.span, output.gatewayUuid) ||
      memcmp(output.gatewayUuid, expectedGatewayUuid,
             kEnrollmentUuidBytes) != 0 ||
      !parseU32(field(fields, count, "key_version"), output.keyVersion) ||
      output.keyVersion == 0U) {
    return GatewayBootstrapResult::BackendMalformed;
  }
  memcpy(output.enrollmentUuid, enrollmentUuid, kEnrollmentUuidBytes);

  size_t leafBytes = 0U;
  if (!decodeBase64(field(fields, count, "device_certificate_der_b64"),
                    workspace.leafCertificate,
                    sizeof(workspace.leafCertificate), leafBytes) ||
      leafBytes == 0U) {
    return GatewayBootstrapResult::BackendMalformed;
  }
  output.certificateDer = workspace.leafCertificate;
  output.certificateBytes = leafBytes;

  const JsonField* chain =
      field(fields, count, "device_certificate_chain_der_b64");
  if (!chain || !parseCertificateChain(chain->value, workspace, output)) {
    return GatewayBootstrapResult::BackendMalformed;
  }

  const JsonField* sealed = field(fields, count, "sealed_secret");
  if (!sealed || sealed->value.kind != JsonKind::Object) {
    return GatewayBootstrapResult::BackendMalformed;
  }
  JsonField sealedFields[kMaximumObjectFields]{};
  size_t sealedCount = 0U;
  static const char* const sealedSchema[] = {
      "suite", "enc_b64", "ciphertext_b64"};
  if (!parseRootObject(sealed->value.span.data, sealed->value.span.bytes,
                       sealedFields, kMaximumObjectFields, sealedCount) ||
      !exactFields(sealedFields, sealedCount, sealedSchema,
                   sizeof(sealedSchema) / sizeof(sealedSchema[0])) ||
      !stringEquals(field(sealedFields, sealedCount, "suite"), kHpkeSuite)) {
    return GatewayBootstrapResult::BackendMalformed;
  }
  size_t encBytes = 0U;
  size_t ciphertextBytes = 0U;
  if (!decodeBase64(field(sealedFields, sealedCount, "enc_b64"),
                    output.encapsulatedKey,
                    sizeof(output.encapsulatedKey), encBytes) ||
      encBytes != sizeof(output.encapsulatedKey) ||
      output.encapsulatedKey[0] != 0x04U ||
      !decodeBase64(field(sealedFields, sealedCount, "ciphertext_b64"),
                    output.ciphertext, sizeof(output.ciphertext),
                    ciphertextBytes) ||
      ciphertextBytes != sizeof(output.ciphertext)) {
    return GatewayBootstrapResult::BackendMalformed;
  }
  return GatewayBootstrapResult::ReconnectSteady;
}

struct KitsuGatewayBootstrap::Implementation {
  ScopedZeroBuffer proxyRequest{kGatewayBootstrapMaximumProxyRequestBytes};
  ScopedZeroBuffer proxyResponse{kGatewayBootstrapMaximumProxyResponseBytes};
  uint8_t enrollmentUuid[kEnrollmentUuidBytes]{};
  uint8_t gatewayUuid[kEnrollmentUuidBytes]{};
  char host[254]{};
  char serverName[254]{};
  uint16_t port = 0U;
  uint8_t caCertificateDer[kGatewayBootstrapMaximumCaBytes]{};
  size_t caCertificateBytes = 0U;
  uint8_t spkiSha256[kGatewayBootstrapSpkiBytes]{};
  size_t proxyRequestBytes = 0U;
  size_t proxyResponseBytes = 0U;
  KitsuEnrollmentRecipient* recipient = nullptr;
  GatewayBootstrapTransport* transport = nullptr;
  EnrollmentCredentialSink* sink = nullptr;
  GatewayBootstrapWorkspace* workspace = nullptr;

  GatewayBootstrapTrust trust() const {
    GatewayBootstrapTrust output{};
    output.host = host;
    output.serverName = serverName;
    output.port = port;
    output.caCertificateDer = caCertificateDer;
    output.caCertificateBytes = caCertificateBytes;
    memcpy(output.spkiSha256, spkiSha256, sizeof(output.spkiSha256));
    memcpy(output.gatewayUuid, gatewayUuid, sizeof(output.gatewayUuid));
    return output;
  }

  ~Implementation() {
    secureZero(enrollmentUuid, sizeof(enrollmentUuid));
    secureZero(gatewayUuid, sizeof(gatewayUuid));
    secureZero(host, sizeof(host));
    secureZero(serverName, sizeof(serverName));
    secureZero(caCertificateDer, sizeof(caCertificateDer));
    secureZero(spkiSha256, sizeof(spkiSha256));
    if (workspace) secureZero(workspace, sizeof(*workspace));
  }
};

KitsuGatewayBootstrap::KitsuGatewayBootstrap() = default;

KitsuGatewayBootstrap::~KitsuGatewayBootstrap() { cancel(); }

bool KitsuGatewayBootstrap::active() const {
  return implementation_ != nullptr;
}

void KitsuGatewayBootstrap::cancel() {
  if (!implementation_) return;
  if (implementation_->transport) implementation_->transport->close();
  if (implementation_->recipient) implementation_->recipient->abort();
  delete implementation_;
  implementation_ = nullptr;
}

GatewayBootstrapResult KitsuGatewayBootstrap::beginExchangeAndInstall(
    const uint8_t enrollmentUuid[kEnrollmentUuidBytes],
    bool remoteConnectivityAllowed, const GatewayBootstrapTrust& trust,
    KitsuEnrollmentRecipient& recipient,
    GatewayBootstrapTransport& transport, EnrollmentCredentialSink& sink,
    GatewayBootstrapWorkspace& workspace) {
  cancel();
  transport.close();
  secureZero(&workspace, sizeof(workspace));
  if (!recipient.active()) return GatewayBootstrapResult::NotActive;
  if (!remoteConnectivityAllowed) {
    recipient.abort();
    return GatewayBootstrapResult::RemoteConnectivityUnavailable;
  }
  if (!enrollmentUuid || !validTrust(trust)) {
    recipient.abort();
    return GatewayBootstrapResult::InvalidArgument;
  }
  Implementation* state = new (std::nothrow) Implementation();
  ScopedZeroBuffer backendRequest(kEnrollmentMaximumRequestBytes);
  if (!state || !state->proxyRequest.ready() ||
      !state->proxyResponse.ready() || !backendRequest.ready()) {
    delete state;
    recipient.abort();
    return GatewayBootstrapResult::TransportFailed;
  }
  size_t backendRequestBytes = 0U;
  if (recipient.buildRequestJson(
          backendRequest.data(), backendRequest.bytes(),
          backendRequestBytes) != EnrollmentResult::Ok ||
      !encodeGatewayBootstrapProxyRequest(
          enrollmentUuid, backendRequest.data(), backendRequestBytes,
          state->proxyRequest.data(), state->proxyRequest.bytes(),
          state->proxyRequestBytes)) {
    delete state;
    recipient.abort();
    return GatewayBootstrapResult::EnrollmentFailed;
  }
  memcpy(state->enrollmentUuid, enrollmentUuid,
         sizeof(state->enrollmentUuid));
  memcpy(state->gatewayUuid, trust.gatewayUuid, sizeof(state->gatewayUuid));
  const size_t hostBytes = strlen(trust.host);
  const size_t serverNameBytes = strlen(trust.serverName);
  memcpy(state->host, trust.host, hostBytes + 1U);
  memcpy(state->serverName, trust.serverName, serverNameBytes + 1U);
  state->port = trust.port;
  memcpy(state->caCertificateDer, trust.caCertificateDer,
         trust.caCertificateBytes);
  state->caCertificateBytes = trust.caCertificateBytes;
  memcpy(state->spkiSha256, trust.spkiSha256, sizeof(state->spkiSha256));
  state->recipient = &recipient;
  state->transport = &transport;
  state->sink = &sink;
  state->workspace = &workspace;
  implementation_ = state;
  return GatewayBootstrapResult::InProgress;
}

GatewayBootstrapResult KitsuGatewayBootstrap::pollExchangeAndInstall() {
  if (!implementation_) return GatewayBootstrapResult::NotActive;
  Implementation& state = *implementation_;
  GatewayBootstrapTlsEvidence evidence{};
  const GatewayBootstrapTrust trust = state.trust();
  const GatewayBootstrapIoResult exchanged =
      state.transport->exchangeOneFramedRequest(
          trust, kGatewayBootstrapAlpn, state.proxyRequest.data(),
          state.proxyRequestBytes, state.proxyResponse.data(),
          state.proxyResponse.bytes(), state.proxyResponseBytes, evidence);
  if (exchanged == GatewayBootstrapIoResult::WouldBlock) {
    return GatewayBootstrapResult::InProgress;
  }
  state.transport->close();
  GatewayBootstrapResult result = GatewayBootstrapResult::TransportFailed;
  if (exchanged != GatewayBootstrapIoResult::Ok) {
    result = evidence.systemTimeChecked && !evidence.systemTimeValid
        ? GatewayBootstrapResult::TimeUnavailable
        : GatewayBootstrapResult::TransportFailed;
  } else if (!evidence.serverChainVerified ||
             !evidence.serverNameVerified || !evidence.spkiMatched ||
             !evidence.alpnMatched || !evidence.tlsVersionAtLeast12 ||
             !evidence.systemTimeChecked || !evidence.systemTimeValid ||
             evidence.plaintextFallbackUsed || evidence.redirectFollowed ||
             evidence.clientCredentialPresented) {
    result = GatewayBootstrapResult::TrustRejected;
  } else {
    size_t backendResponseBytes = 0U;
    result = decodeGatewayBootstrapProxyResponse(
        state.proxyResponse.data(), state.proxyResponseBytes,
        state.enrollmentUuid, state.proxyResponse.data(),
        state.proxyResponse.bytes(), backendResponseBytes);
    if (result == GatewayBootstrapResult::ReconnectSteady) {
      EnrollmentResponse response{};
      result = decodeBackendEnrollmentResponse(
          state.proxyResponse.data(), backendResponseBytes,
          state.enrollmentUuid, state.gatewayUuid, *state.workspace,
          response);
      if (result == GatewayBootstrapResult::ReconnectSteady &&
          state.recipient->finish(response, *state.sink) !=
              EnrollmentResult::Ok) {
        result = GatewayBootstrapResult::EnrollmentFailed;
      }
    }
  }
  if (result != GatewayBootstrapResult::ReconnectSteady) {
    state.recipient->abort();
  }
  delete implementation_;
  implementation_ = nullptr;
  return result;
}

}  // namespace connectivity
}  // namespace kitsu868
