#include "kitsu_connectivity_config.h"

#include <new>
#include <string.h>

#include "kitsu_companion_protocol.h"

namespace kitsu868 {
namespace connectivity {
namespace {

constexpr uint8_t kOuterMagic[4] = {'K', 'C', 'O', 'N'};
constexpr uint8_t kPlainMagic[4] = {'K', 'C', 'F', 'G'};
constexpr uint16_t kLegacyConnectionVersion = 1U;
constexpr uint16_t kConnectionVersion = 2U;
constexpr uint16_t kLegacyBootstrapPort = 7442U;
constexpr uint16_t kLegacySteadyPort = 7443U;
constexpr size_t kPlainCrcOffset = kConnectionPlainBytes - 4U;
constexpr uint32_t kFlagWifi = 1UL << 0U;
constexpr uint32_t kFlagGateway = 1UL << 1U;
constexpr uint32_t kFlagEnrollment = 1UL << 2U;
constexpr uint32_t kFlagMobileRelay = 1UL << 3U;
constexpr uint32_t kKnownFlags =
    kFlagWifi | kFlagGateway | kFlagEnrollment | kFlagMobileRelay;

// The fixed-width plaintext deliberately reserves all EnrollmentCredentialSink
// maxima.  Offsets are explicit so Wi-Fi or gateway updates can preserve an
// enrollment without keeping its private material resident in RAM.
constexpr size_t kWifiOffset = 16U;
constexpr size_t kWifiSsidLengthOffset = kWifiOffset;
constexpr size_t kWifiPassphraseLengthOffset = kWifiOffset + 1U;
constexpr size_t kWifiSecurityOffset = kWifiOffset + 2U;
constexpr size_t kWifiSsidOffset = kWifiOffset + 4U;
constexpr size_t kWifiPassphraseOffset =
    kWifiSsidOffset + kWifiSsidMaximumBytes;
constexpr size_t kGatewayOffset = kWifiPassphraseOffset + 64U;
constexpr size_t kGatewayIdOffset = kGatewayOffset;
constexpr size_t kGatewayHostLengthOffset = kGatewayIdOffset + 16U;
constexpr size_t kGatewayPortOffset = kGatewayHostLengthOffset + 2U;
constexpr size_t kGatewayServerNameLengthOffset = kGatewayPortOffset + 2U;
constexpr size_t kGatewayCaLengthOffset =
    kGatewayServerNameLengthOffset + 2U;
constexpr size_t kGatewayHostOffset = kGatewayCaLengthOffset + 2U;
constexpr size_t kGatewayServerNameOffset =
    kGatewayHostOffset + kGatewayHostMaximumBytes;
constexpr size_t kGatewayCaOffset =
    kGatewayServerNameOffset + kGatewayServerNameMaximumBytes;
constexpr size_t kGatewaySpkiOffset = kGatewayCaOffset + kGatewayCaMaximumBytes;
constexpr size_t kEnrollmentOffset =
    kGatewaySpkiOffset + kGatewaySpkiSha256Bytes;
constexpr size_t kEnrollmentCompanionIdOffset = kEnrollmentOffset;
constexpr size_t kEnrollmentGatewayIdOffset =
    kEnrollmentCompanionIdOffset + 16U;
constexpr size_t kEnrollmentKeyVersionOffset =
    kEnrollmentGatewayIdOffset + 16U;
constexpr size_t kEnrollmentPrivateKeyOffset =
    kEnrollmentKeyVersionOffset + 4U;
constexpr size_t kEnrollmentCertificateLengthOffset =
    kEnrollmentPrivateKeyOffset + kEnrollmentPrivateKeyBytes;
constexpr size_t kEnrollmentChainCountOffset =
    kEnrollmentCertificateLengthOffset + 2U;
constexpr size_t kEnrollmentChainTotalOffset =
    kEnrollmentChainCountOffset + 2U;
constexpr size_t kEnrollmentChainLengthsOffset =
    kEnrollmentChainTotalOffset + 2U;
constexpr size_t kEnrollmentBackendSecretOffset =
    kEnrollmentChainLengthsOffset +
    kEnrollmentMaximumChainCertificates * 2U;
constexpr size_t kEnrollmentCertificateOffset =
    kEnrollmentBackendSecretOffset + kEnrollmentSecretBytes;
constexpr size_t kEnrollmentChainOffset =
    kEnrollmentCertificateOffset + kEnrollmentMaximumCertificateBytes;
constexpr size_t kUsedPlainBytes =
    kEnrollmentChainOffset + kEnrollmentMaximumChainBytes;
// v2 consumes previously reserved tail space instead of moving any enrolled
// credential offsets. This permits an authenticated, power-loss-safe v1 -> v2
// migration without copying secret fields through a second layout.
constexpr size_t kGatewayBootstrapPortOffset = kUsedPlainBytes;
constexpr size_t kUsedPlainV2Bytes = kGatewayBootstrapPortOffset + 2U;

static_assert(kGatewayOffset == 116U, "Wi-Fi layout changed");
static_assert(kEnrollmentOffset == 8870U, "gateway layout changed");
static_assert(kUsedPlainBytes == 29464U,
              "enrollment storage bound changed");
static_assert(kUsedPlainV2Bytes <= kPlainCrcOffset,
              "enrollment maxima no longer fit the snapshot");

void secureZero(void* memory, size_t bytes) {
  volatile uint8_t* output = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *output++ = 0U;
}

void putU16(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
}

void putU32(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
  output[2] = static_cast<uint8_t>(value >> 16U);
  output[3] = static_cast<uint8_t>(value >> 24U);
}

uint16_t getU16(const uint8_t* input) {
  return static_cast<uint16_t>(input[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8U);
}

uint32_t getU32(const uint8_t* input) {
  return static_cast<uint32_t>(input[0]) |
         (static_cast<uint32_t>(input[1]) << 8U) |
         (static_cast<uint32_t>(input[2]) << 16U) |
         (static_cast<uint32_t>(input[3]) << 24U);
}

uint32_t crc32(const uint8_t* input, size_t bytes) {
  uint32_t crc = 0xffffffffUL;
  for (size_t i = 0U; i < bytes; ++i) {
    crc ^= input[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask =
          static_cast<uint32_t>(-static_cast<int32_t>(crc & 1U));
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

class TransientBuffers {
 public:
  TransientBuffers() {
    outer = new (std::nothrow) uint8_t[kConnectionSnapshotBytes];
    plain = new (std::nothrow) uint8_t[kConnectionPlainBytes];
    if (!outer || !plain) release();
  }

  ~TransientBuffers() { release(); }
  bool ready() const { return outer && plain; }

  uint8_t* outer = nullptr;
  uint8_t* plain = nullptr;

 private:
  void release() {
    if (outer) {
      secureZero(outer, kConnectionSnapshotBytes);
      delete[] outer;
      outer = nullptr;
    }
    if (plain) {
      secureZero(plain, kConnectionPlainBytes);
      delete[] plain;
      plain = nullptr;
    }
  }
};

struct Span {
  const uint8_t* data = nullptr;
  size_t bytes = 0U;
  bool escaped = false;
};

enum class JsonValueKind : uint8_t { String = 0, Unsigned };

struct JsonField {
  Span key{};
  Span value{};
  JsonValueKind kind = JsonValueKind::String;
};

constexpr size_t kMaximumConfigFields = 7U;

void skipWhitespace(const uint8_t* json, size_t bytes, size_t& cursor) {
  while (cursor < bytes &&
         (json[cursor] == ' ' || json[cursor] == '\t' ||
          json[cursor] == '\r' || json[cursor] == '\n')) {
    ++cursor;
  }
}

bool isHex(uint8_t input) {
  return (input >= '0' && input <= '9') ||
         (input >= 'a' && input <= 'f') ||
         (input >= 'A' && input <= 'F');
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

bool parseStringSpan(const uint8_t* json, size_t bytes, size_t& cursor,
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

bool spanEquals(const Span& span, const char* text) {
  const size_t bytes = text ? strlen(text) : 0U;
  return !span.escaped && span.bytes == bytes &&
         memcmp(span.data, text, bytes) == 0;
}

bool sameSpan(const Span& left, const Span& right) {
  return !left.escaped && !right.escaped && left.bytes == right.bytes &&
         memcmp(left.data, right.data, left.bytes) == 0;
}

bool parseObject(const uint8_t* json, size_t bytes,
                 JsonField fields[kMaximumConfigFields], size_t& count) {
  count = 0U;
  if (!json || bytes == 0U ||
      !companion::validUtf8(json, bytes)) {
    return false;
  }
  size_t cursor = 0U;
  skipWhitespace(json, bytes, cursor);
  if (cursor >= bytes || json[cursor++] != '{') return false;
  skipWhitespace(json, bytes, cursor);
  if (cursor < bytes && json[cursor] == '}') {
    ++cursor;
    skipWhitespace(json, bytes, cursor);
    return cursor == bytes;
  }
  while (cursor < bytes) {
    if (count == kMaximumConfigFields ||
        !parseStringSpan(json, bytes, cursor, fields[count].key) ||
        fields[count].key.escaped) {
      return false;
    }
    for (size_t i = 0U; i < count; ++i) {
      if (sameSpan(fields[i].key, fields[count].key)) return false;
    }
    skipWhitespace(json, bytes, cursor);
    if (cursor >= bytes || json[cursor++] != ':') return false;
    skipWhitespace(json, bytes, cursor);
    if (cursor < bytes && json[cursor] == '"') {
      fields[count].kind = JsonValueKind::String;
      if (!parseStringSpan(json, bytes, cursor, fields[count].value)) {
        return false;
      }
    } else {
      fields[count].kind = JsonValueKind::Unsigned;
      const size_t start = cursor;
      while (cursor < bytes && json[cursor] >= '0' &&
             json[cursor] <= '9') {
        ++cursor;
      }
      if (cursor == start) return false;
      fields[count].value.data = json + start;
      fields[count].value.bytes = cursor - start;
    }
    ++count;
    skipWhitespace(json, bytes, cursor);
    if (cursor >= bytes) return false;
    if (json[cursor] == '}') {
      ++cursor;
      skipWhitespace(json, bytes, cursor);
      return cursor == bytes;
    }
    if (json[cursor++] != ',') return false;
    skipWhitespace(json, bytes, cursor);
  }
  return false;
}

const JsonField* findField(const JsonField* fields, size_t count,
                           const char* name, JsonValueKind kind) {
  for (size_t i = 0U; i < count; ++i) {
    if (fields[i].kind == kind && spanEquals(fields[i].key, name)) {
      return fields + i;
    }
  }
  return nullptr;
}

bool decodeAsciiString(const Span& span, char* output, size_t capacity,
                       size_t& outputBytes) {
  outputBytes = 0U;
  if (!output || capacity == 0U) return false;
  for (size_t cursor = 0U; cursor < span.bytes;) {
    uint32_t value = span.data[cursor++];
    if (value == '\\') {
      if (cursor >= span.bytes) return false;
      const uint8_t escape = span.data[cursor++];
      switch (escape) {
        case '"': value = '"'; break;
        case '\\': value = '\\'; break;
        case '/': value = '/'; break;
        case 'b': value = '\b'; break;
        case 'f': value = '\f'; break;
        case 'n': value = '\n'; break;
        case 'r': value = '\r'; break;
        case 't': value = '\t'; break;
        case 'u': {
          if (cursor + 4U > span.bytes) return false;
          value = hex4(span.data + cursor);
          cursor += 4U;
          if (value >= 0xd800U && value <= 0xdbffU) {
            // Configuration text is ASCII, so no surrogate pair can become
            // a valid result.  Consume/reject it without truncation.
            return false;
          }
          break;
        }
        default: return false;
      }
    }
    if (value > 0x7fU || outputBytes + 1U >= capacity) return false;
    output[outputBytes++] = static_cast<char>(value);
  }
  output[outputBytes] = '\0';
  return true;
}

int base64Value(uint8_t c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

bool canonicalBase64Url(const Span& span) {
  if (span.escaped || span.bytes == 0U || span.bytes % 4U == 1U) {
    return false;
  }
  for (size_t i = 0U; i < span.bytes; ++i) {
    if (base64Value(span.data[i]) < 0) return false;
  }
  const int tail = base64Value(span.data[span.bytes - 1U]);
  if (span.bytes % 4U == 2U && (tail & 0x0f) != 0) return false;
  if (span.bytes % 4U == 3U && (tail & 0x03) != 0) return false;
  return true;
}

bool parseUnsigned(const Span& span, uint32_t maximum, uint32_t& output) {
  output = 0U;
  if (span.bytes == 0U || span.bytes > 10U ||
      (span.bytes > 1U && span.data[0] == '0')) {
    return false;
  }
  for (size_t i = 0U; i < span.bytes; ++i) {
    if (span.data[i] < '0' || span.data[i] > '9') return false;
    output = output * 10U + static_cast<uint8_t>(span.data[i] - '0');
    if (output > maximum) return false;
  }
  return true;
}

bool validPrintablePassphrase(const char* value, size_t bytes) {
  if (!value || bytes < 8U || bytes > kWifiPassphraseMaximumBytes) {
    return false;
  }
  for (size_t i = 0U; i < bytes; ++i) {
    if (static_cast<uint8_t>(value[i]) < 0x20U ||
        static_cast<uint8_t>(value[i]) > 0x7eU) {
      return false;
    }
  }
  return true;
}

bool validSsid(const uint8_t* value, size_t bytes) {
  if (!value || bytes == 0U || bytes > kWifiSsidMaximumBytes ||
      !companion::validUtf8(value, bytes)) {
    return false;
  }
  for (size_t i = 0U; i < bytes; ++i) {
    if (value[i] == 0U) return false;
  }
  return true;
}

bool validDnsName(const char* value, size_t bytes) {
  if (!value || bytes == 0U || bytes > kGatewayServerNameMaximumBytes ||
      value[bytes - 1U] == '.') {
    return false;
  }
  size_t labelStart = 0U;
  bool hasNonNumericLabelByte = false;
  for (size_t i = 0U; i <= bytes; ++i) {
    if (i != bytes && value[i] != '.') {
      const char c = value[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-')) {
        return false;
      }
      hasNonNumericLabelByte = hasNonNumericLabelByte ||
                               (c < '0' || c > '9');
      continue;
    }
    const size_t labelBytes = i - labelStart;
    if (labelBytes == 0U || labelBytes > 63U ||
        value[labelStart] == '-' || value[i - 1U] == '-') {
      return false;
    }
    labelStart = i + 1U;
  }
  // A dotted-decimal token is an IP literal, not a DNS identity.  Rejecting
  // all-numeric labels also prevents malformed IPv4 from slipping through as
  // a hostname after the strict IPv4 parser rejects it.
  return hasNonNumericLabelByte;
}

bool validServerName(const char* value, size_t bytes) {
  if (!validDnsName(value, bytes)) return false;
  for (size_t i = 0U; i < bytes; ++i) {
    if (value[i] == '.') return true;
  }
  return false;
}

bool validIpv4(const char* value, size_t bytes) {
  if (!value || bytes < 7U || bytes > 15U) return false;
  size_t start = 0U;
  uint8_t groups = 0U;
  for (size_t i = 0U; i <= bytes; ++i) {
    if (i != bytes && value[i] != '.') continue;
    const size_t count = i - start;
    if (count == 0U || count > 3U ||
        (count > 1U && value[start] == '0')) {
      return false;
    }
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

bool validIpv6(const char* value, size_t bytes) {
  if (!value || bytes < 2U || bytes > 45U) return false;
  bool compressed = false;
  for (size_t i = 0U; i + 1U < bytes; ++i) {
    if (value[i] == ':' && value[i + 1U] == ':') {
      if (i + 2U < bytes && value[i + 2U] == ':') return false;
      if (compressed) return false;
      compressed = true;
      ++i;
    }
  }
  if ((value[0] == ':' && (bytes < 2U || value[1] != ':')) ||
      (value[bytes - 1U] == ':' &&
       (bytes < 2U || value[bytes - 2U] != ':'))) {
    return false;
  }
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
      if (end != bytes || !validIpv4(value + start, end - start)) {
        return false;
      }
      groups = static_cast<uint8_t>(groups + 2U);
    } else {
      if (end - start == 0U || end - start > 4U) return false;
      for (size_t i = start; i < end; ++i) {
        if (!isHex(static_cast<uint8_t>(value[i]))) return false;
      }
      ++groups;
    }
    start = end + 1U;
  }
  return compressed ? groups < 8U : groups == 8U;
}

bool validHost(const char* value, size_t bytes) {
  if (!value || bytes == 0U || bytes > kGatewayHostMaximumBytes) {
    return false;
  }
  for (size_t i = 0U; i < bytes; ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if (c < 0x21U || c > 0x7eU || c == '/' || c == '\\' ||
        c == '@' || c == '?' || c == '#') {
      return false;
    }
  }
  return validIpv4(value, bytes) || validIpv6(value, bytes) ||
         validDnsName(value, bytes);
}

bool parseUuid(const char* value, size_t bytes, uint8_t output[16]) {
  if (!value || bytes != 36U || !output) return false;
  size_t outputIndex = 0U;
  uint8_t high = 0U;
  bool haveHigh = false;
  for (size_t i = 0U; i < bytes; ++i) {
    if (i == 8U || i == 13U || i == 18U || i == 23U) {
      if (value[i] != '-') return false;
      continue;
    }
    const char c = value[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    const uint8_t nibble = c <= '9'
        ? static_cast<uint8_t>(c - '0')
        : static_cast<uint8_t>(c - 'a' + 10U);
    if (!haveHigh) {
      high = static_cast<uint8_t>(nibble << 4U);
      haveHigh = true;
    } else {
      if (outputIndex >= 16U) return false;
      output[outputIndex++] = static_cast<uint8_t>(high | nibble);
      haveHigh = false;
    }
  }
  return outputIndex == 16U && !haveHigh && !allZero(output, 16U);
}

bool validWifiStruct(const WifiConfig& config) {
  return validSsid(config.ssid, config.ssidBytes) &&
         validPrintablePassphrase(config.passphrase,
                                  config.passphraseBytes) &&
         static_cast<uint8_t>(config.security) <=
             static_cast<uint8_t>(WifiSecurity::Wpa3);
}

bool validGatewayStruct(const GatewayConfig& config,
                        GatewayTrustValidator& trust) {
  if (allZero(config.gatewayId, sizeof(config.gatewayId)) ||
      config.caCertificateBytes == 0U ||
      config.caCertificateBytes > kGatewayCaMaximumBytes ||
      !trust.validCertificateAuthority(config.caCertificateDer,
                                       config.caCertificateBytes)) {
    return false;
  }
  if (config.mobileRelayOnly) {
    return config.hostBytes == 0U && config.bootstrapPort == 0U &&
        config.port == 0U && config.serverNameBytes == 0U &&
        allZero(config.spkiSha256, sizeof(config.spkiSha256));
  }
  return validHost(config.host, config.hostBytes) &&
      config.bootstrapPort != 0U && config.port != 0U &&
      config.bootstrapPort != config.port &&
      validServerName(config.serverName, config.serverNameBytes) &&
      !allZero(config.spkiSha256, sizeof(config.spkiSha256));
}

void initializePlain(uint8_t* plain) {
  memset(plain, 0, kConnectionPlainBytes);
  memcpy(plain, kPlainMagic, sizeof(kPlainMagic));
  putU16(plain + 4U, kConnectionVersion);
  putU16(plain + 6U, static_cast<uint16_t>(kConnectionPlainBytes));
  putU32(plain + 8U, 0U);
  putU32(plain + 12U, 0U);
  putU32(plain + kPlainCrcOffset, crc32(plain, kPlainCrcOffset));
}

bool validatePlain(const uint8_t* plain, uint32_t generation,
                   GatewayTrustValidator& trust) {
  const uint16_t version = plain ? getU16(plain + 4U) : 0U;
  if (!plain || memcmp(plain, kPlainMagic, sizeof(kPlainMagic)) != 0 ||
      (version != kLegacyConnectionVersion && version != kConnectionVersion) ||
      getU16(plain + 6U) != kConnectionPlainBytes ||
      getU32(plain + 12U) != generation ||
      crc32(plain, kPlainCrcOffset) != getU32(plain + kPlainCrcOffset)) {
    return false;
  }
  const uint32_t flags = getU32(plain + 8U);
  if ((flags & ~kKnownFlags) != 0U ||
      ((flags & kFlagEnrollment) != 0U &&
       (flags & kFlagGateway) == 0U) ||
      ((flags & kFlagMobileRelay) != 0U &&
       (flags & kFlagGateway) == 0U)) {
    return false;
  }
  if ((flags & kFlagWifi) != 0U) {
    const size_t ssidBytes = plain[kWifiSsidLengthOffset];
    const size_t passphraseBytes = plain[kWifiPassphraseLengthOffset];
    if (!validSsid(plain + kWifiSsidOffset, ssidBytes) ||
        !validPrintablePassphrase(
            reinterpret_cast<const char*>(plain + kWifiPassphraseOffset),
            passphraseBytes) ||
        plain[kWifiSecurityOffset] >
            static_cast<uint8_t>(WifiSecurity::Wpa3)) {
      return false;
    }
  }
  if ((flags & kFlagGateway) != 0U) {
    const bool mobileRelay = (flags & kFlagMobileRelay) != 0U;
    const size_t hostBytes = getU16(plain + kGatewayHostLengthOffset);
    const size_t serverNameBytes =
        getU16(plain + kGatewayServerNameLengthOffset);
    const size_t caBytes = getU16(plain + kGatewayCaLengthOffset);
    const uint16_t steadyPort = getU16(plain + kGatewayPortOffset);
    // There was no bootstrap-port field in v1. Only the deployed, frozen
    // Kitsu steady listener is migrated; any other legacy record is rejected
    // and must be re-provisioned through authenticated BLE.
    const uint16_t bootstrapPort = version == kLegacyConnectionVersion
        ? (steadyPort == kLegacySteadyPort ? kLegacyBootstrapPort : 0U)
        : getU16(plain + kGatewayBootstrapPortOffset);
    const bool commonValid =
        !allZero(plain + kGatewayIdOffset, kEnrollmentUuidBytes) &&
        caBytes != 0U && caBytes <= kGatewayCaMaximumBytes &&
        trust.validCertificateAuthority(plain + kGatewayCaOffset, caBytes);
    const bool transportValid = mobileRelay
        ? hostBytes == 0U && steadyPort == 0U && bootstrapPort == 0U &&
              serverNameBytes == 0U &&
              allZero(plain + kGatewaySpkiOffset,
                      kGatewaySpkiSha256Bytes)
        : validHost(
              reinterpret_cast<const char*>(plain + kGatewayHostOffset),
              hostBytes) && steadyPort != 0U && bootstrapPort != 0U &&
              bootstrapPort != steadyPort &&
              validServerName(
                  reinterpret_cast<const char*>(
                      plain + kGatewayServerNameOffset),
                  serverNameBytes) &&
              !allZero(plain + kGatewaySpkiOffset,
                       kGatewaySpkiSha256Bytes);
    if (!commonValid || !transportValid) {
      return false;
    }
  }
  if ((flags & kFlagEnrollment) != 0U) {
    const size_t certificateBytes =
        getU16(plain + kEnrollmentCertificateLengthOffset);
    const size_t chainCount = plain[kEnrollmentChainCountOffset];
    const size_t chainTotal = getU16(plain + kEnrollmentChainTotalOffset);
    if (allZero(plain + kEnrollmentCompanionIdOffset, 16U) ||
        memcmp(plain + kEnrollmentGatewayIdOffset,
               plain + kGatewayIdOffset, 16U) != 0 ||
        getU32(plain + kEnrollmentKeyVersionOffset) == 0U ||
        allZero(plain + kEnrollmentPrivateKeyOffset,
                kEnrollmentPrivateKeyBytes) ||
        certificateBytes == 0U ||
        certificateBytes > kEnrollmentMaximumCertificateBytes ||
        chainCount == 0U ||
        chainCount > kEnrollmentMaximumChainCertificates ||
        chainTotal > kEnrollmentMaximumChainBytes) {
      return false;
    }
    const uint8_t* chainDer[kEnrollmentMaximumChainCertificates]{};
    size_t chainBytes[kEnrollmentMaximumChainCertificates]{};
    size_t chainCursor = 0U;
    for (size_t i = 0U; i < chainCount; ++i) {
      chainBytes[i] = getU16(
          plain + kEnrollmentChainLengthsOffset + i * 2U);
      if (chainBytes[i] == 0U ||
          chainBytes[i] > kEnrollmentMaximumCertificateBytes ||
          chainCursor + chainBytes[i] > chainTotal) {
        return false;
      }
      chainDer[i] = plain + kEnrollmentChainOffset + chainCursor;
      chainCursor += chainBytes[i];
    }
    if (chainCursor != chainTotal ||
        !trust.validateEnrollmentChain(
            plain + kGatewayCaOffset,
            getU16(plain + kGatewayCaLengthOffset),
            plain + kEnrollmentCertificateOffset, certificateBytes,
            chainDer, chainBytes, chainCount)) {
      return false;
    }
  }
  return true;
}

void encodeWifi(uint8_t* plain, const WifiConfig& config) {
  plain[kWifiSsidLengthOffset] = config.ssidBytes;
  plain[kWifiPassphraseLengthOffset] = config.passphraseBytes;
  plain[kWifiSecurityOffset] = static_cast<uint8_t>(config.security);
  plain[kWifiSecurityOffset + 1U] = 0U;
  memset(plain + kWifiSsidOffset, 0, kWifiSsidMaximumBytes);
  memcpy(plain + kWifiSsidOffset, config.ssid, config.ssidBytes);
  memset(plain + kWifiPassphraseOffset, 0, 64U);
  memcpy(plain + kWifiPassphraseOffset, config.passphrase,
         config.passphraseBytes);
  putU32(plain + 8U, getU32(plain + 8U) | kFlagWifi);
}

void encodeGateway(uint8_t* plain, const GatewayConfig& config) {
  memcpy(plain + kGatewayIdOffset, config.gatewayId,
         kEnrollmentUuidBytes);
  putU16(plain + kGatewayHostLengthOffset, config.hostBytes);
  putU16(plain + kGatewayBootstrapPortOffset, config.bootstrapPort);
  putU16(plain + kGatewayPortOffset, config.port);
  putU16(plain + kGatewayServerNameLengthOffset,
         config.serverNameBytes);
  putU16(plain + kGatewayCaLengthOffset, config.caCertificateBytes);
  memset(plain + kGatewayHostOffset, 0, kGatewayHostMaximumBytes);
  memcpy(plain + kGatewayHostOffset, config.host, config.hostBytes);
  memset(plain + kGatewayServerNameOffset, 0,
         kGatewayServerNameMaximumBytes);
  memcpy(plain + kGatewayServerNameOffset, config.serverName,
         config.serverNameBytes);
  memset(plain + kGatewayCaOffset, 0, kGatewayCaMaximumBytes);
  memcpy(plain + kGatewayCaOffset, config.caCertificateDer,
         config.caCertificateBytes);
  memcpy(plain + kGatewaySpkiOffset, config.spkiSha256,
         kGatewaySpkiSha256Bytes);
  // A changed gateway trust record invalidates credentials enrolled under the
  // old gateway/CA.  A fresh enrollment is required before mTLS may start.
  memset(plain + kEnrollmentOffset, 0,
         kUsedPlainBytes - kEnrollmentOffset);
  uint32_t flags = (getU32(plain + 8U) | kFlagGateway) & ~kFlagEnrollment;
  if (config.mobileRelayOnly) {
    flags |= kFlagMobileRelay;
  } else {
    flags &= ~kFlagMobileRelay;
  }
  putU32(plain + 8U, flags);
}

}  // namespace

const char* wifiSecurityName(WifiSecurity security) {
  switch (security) {
    case WifiSecurity::Wpa2: return "wpa2";
    case WifiSecurity::Wpa2Wpa3: return "wpa2_wpa3";
    case WifiSecurity::Wpa3: return "wpa3";
    default: return "unknown";
  }
}

const char* configResultName(ConfigResult result) {
  switch (result) {
    case ConfigResult::Ok: return "ok";
    case ConfigResult::NotBegun: return "storage_unavailable";
    case ConfigResult::InvalidArgument: return "invalid_argument";
    case ConfigResult::InvalidWifiPayload: return "invalid_wifi_payload";
    case ConfigResult::InvalidSsid: return "invalid_ssid";
    case ConfigResult::InvalidSecurity: return "invalid_security";
    case ConfigResult::InvalidPassphrase: return "invalid_passphrase";
    case ConfigResult::InvalidGatewayPayload:
      return "invalid_gateway_payload";
    case ConfigResult::InvalidGatewayId: return "invalid_gateway_id";
    case ConfigResult::InvalidHost: return "invalid_host";
    case ConfigResult::InvalidBootstrapPort:
      return "invalid_bootstrap_port";
    case ConfigResult::InvalidPort: return "invalid_port";
    case ConfigResult::InvalidServerName: return "invalid_server_name";
    case ConfigResult::InvalidCaCertificate:
      return "invalid_ca_certificate";
    case ConfigResult::InvalidSpkiSha256: return "invalid_spki_sha256";
    case ConfigResult::SecurityUnavailable: return "security_unavailable";
    case ConfigResult::StorageUnavailable: return "storage_unavailable";
    case ConfigResult::StorageReadFailed: return "storage_read_failed";
    case ConfigResult::StorageWriteFailed: return "storage_write_failed";
    case ConfigResult::StorageReadbackFailed:
      return "storage_readback_failed";
    case ConfigResult::StorageAllocationFailed:
      return "storage_allocation_failed";
    case ConfigResult::StorageCorrupt: return "storage_corrupt";
    case ConfigResult::CryptoFailed: return "security_unavailable";
    case ConfigResult::EnrollmentInvalid: return "invalid_enrollment";
    case ConfigResult::EnrollmentTrustFailed:
      return "invalid_enrollment_trust";
    default: return "storage_unavailable";
  }
}

ConfigResult decodeWifiConfig(const uint8_t* json, size_t jsonBytes,
                              WifiConfig& output) {
  secureZero(&output, sizeof(output));
  JsonField fields[kMaximumConfigFields]{};
  size_t count = 0U;
  if (!parseObject(json, jsonBytes, fields, count) || count != 3U) {
    return ConfigResult::InvalidWifiPayload;
  }
  const JsonField* ssid =
      findField(fields, count, "ssid_b64", JsonValueKind::String);
  const JsonField* security =
      findField(fields, count, "security", JsonValueKind::String);
  const JsonField* passphrase =
      findField(fields, count, "passphrase", JsonValueKind::String);
  if (!ssid || !security || !passphrase) {
    return ConfigResult::InvalidWifiPayload;
  }
  if (!canonicalBase64Url(ssid->value)) return ConfigResult::InvalidSsid;
  size_t decodedBytes = 0U;
  if (!companion::decodeBase64Url(
          reinterpret_cast<const char*>(ssid->value.data),
          ssid->value.bytes, output.ssid, kWifiSsidMaximumBytes,
          decodedBytes) || !validSsid(output.ssid, decodedBytes)) {
    secureZero(&output, sizeof(output));
    return ConfigResult::InvalidSsid;
  }
  output.ssidBytes = static_cast<uint8_t>(decodedBytes);
  output.ssid[decodedBytes] = 0U;

  char securityText[16]{};
  size_t securityBytes = 0U;
  if (!decodeAsciiString(security->value, securityText,
                         sizeof(securityText), securityBytes)) {
    secureZero(&output, sizeof(output));
    return ConfigResult::InvalidSecurity;
  }
  if (strcmp(securityText, "wpa2") == 0) {
    output.security = WifiSecurity::Wpa2;
  } else if (strcmp(securityText, "wpa2_wpa3") == 0) {
    output.security = WifiSecurity::Wpa2Wpa3;
  } else if (strcmp(securityText, "wpa3") == 0) {
    output.security = WifiSecurity::Wpa3;
  } else {
    secureZero(&output, sizeof(output));
    return ConfigResult::InvalidSecurity;
  }
  size_t passphraseBytes = 0U;
  if (!decodeAsciiString(passphrase->value, output.passphrase,
                         sizeof(output.passphrase), passphraseBytes) ||
      !validPrintablePassphrase(output.passphrase, passphraseBytes)) {
    secureZero(&output, sizeof(output));
    return ConfigResult::InvalidPassphrase;
  }
  output.passphraseBytes = static_cast<uint8_t>(passphraseBytes);
  return ConfigResult::Ok;
}

ConfigResult decodeGatewayConfig(const uint8_t* json, size_t jsonBytes,
                                 GatewayTrustValidator& trust,
                                 GatewayConfig& output) {
  secureZero(&output, sizeof(output));
  JsonField fields[kMaximumConfigFields]{};
  size_t count = 0U;
  if (!parseObject(json, jsonBytes, fields, count) || count != 7U) {
    return ConfigResult::InvalidGatewayPayload;
  }
  const JsonField* gatewayId =
      findField(fields, count, "gateway_id", JsonValueKind::String);
  const JsonField* host =
      findField(fields, count, "host", JsonValueKind::String);
  const JsonField* bootstrapPort =
      findField(fields, count, "bootstrap_port", JsonValueKind::Unsigned);
  const JsonField* port =
      findField(fields, count, "port", JsonValueKind::Unsigned);
  const JsonField* serverName =
      findField(fields, count, "server_name", JsonValueKind::String);
  const JsonField* ca =
      findField(fields, count, "ca_cert_der_b64", JsonValueKind::String);
  const JsonField* spki =
      findField(fields, count, "spki_sha256_b64", JsonValueKind::String);
  if (!gatewayId || !host || !bootstrapPort || !port || !serverName ||
      !ca || !spki) {
    return ConfigResult::InvalidGatewayPayload;
  }
  char uuidText[37]{};
  size_t uuidBytes = 0U;
  if (!decodeAsciiString(gatewayId->value, uuidText, sizeof(uuidText),
                         uuidBytes) ||
      !parseUuid(uuidText, uuidBytes, output.gatewayId)) {
    secureZero(&output, sizeof(output));
    return ConfigResult::InvalidGatewayId;
  }
  size_t hostBytes = 0U;
  if (!decodeAsciiString(host->value, output.host, sizeof(output.host),
                         hostBytes) || !validHost(output.host, hostBytes)) {
    secureZero(&output, sizeof(output));
    return ConfigResult::InvalidHost;
  }
  output.hostBytes = static_cast<uint16_t>(hostBytes);
  uint32_t parsedBootstrapPort = 0U;
  if (!parseUnsigned(bootstrapPort->value, 65535U, parsedBootstrapPort) ||
      parsedBootstrapPort == 0U) {
    secureZero(&output, sizeof(output));
    return ConfigResult::InvalidBootstrapPort;
  }
  output.bootstrapPort = static_cast<uint16_t>(parsedBootstrapPort);
  uint32_t parsedPort = 0U;
  if (!parseUnsigned(port->value, 65535U, parsedPort) || parsedPort == 0U ||
      parsedPort == parsedBootstrapPort) {
    secureZero(&output, sizeof(output));
    return ConfigResult::InvalidPort;
  }
  output.port = static_cast<uint16_t>(parsedPort);
  size_t serverNameBytes = 0U;
  if (!decodeAsciiString(serverName->value, output.serverName,
                         sizeof(output.serverName), serverNameBytes) ||
      !validServerName(output.serverName, serverNameBytes)) {
    secureZero(&output, sizeof(output));
    return ConfigResult::InvalidServerName;
  }
  output.serverNameBytes = static_cast<uint16_t>(serverNameBytes);
  if (!canonicalBase64Url(ca->value)) {
    secureZero(&output, sizeof(output));
    return ConfigResult::InvalidCaCertificate;
  }
  size_t caBytes = 0U;
  if (!companion::decodeBase64Url(
          reinterpret_cast<const char*>(ca->value.data), ca->value.bytes,
          output.caCertificateDer, sizeof(output.caCertificateDer),
          caBytes) || caBytes == 0U || caBytes > kGatewayCaMaximumBytes ||
      !trust.validCertificateAuthority(output.caCertificateDer, caBytes)) {
    secureZero(&output, sizeof(output));
    return ConfigResult::InvalidCaCertificate;
  }
  output.caCertificateBytes = static_cast<uint16_t>(caBytes);
  if (!canonicalBase64Url(spki->value)) {
    secureZero(&output, sizeof(output));
    return ConfigResult::InvalidSpkiSha256;
  }
  size_t spkiBytes = 0U;
  if (!companion::decodeBase64Url(
          reinterpret_cast<const char*>(spki->value.data), spki->value.bytes,
          output.spkiSha256, sizeof(output.spkiSha256), spkiBytes) ||
      spkiBytes != kGatewaySpkiSha256Bytes ||
      allZero(output.spkiSha256, sizeof(output.spkiSha256))) {
    secureZero(&output, sizeof(output));
    return ConfigResult::InvalidSpkiSha256;
  }
  return ConfigResult::Ok;
}

ConnectionConfigStore::ConnectionConfigStore() { clear(); }

ConnectionConfigStore::~ConnectionConfigStore() { clear(); }

void ConnectionConfigStore::clear() {
  GatewayLanCredentialView lease{};
  release(lease);
  secureZero(&gateway_, sizeof(gateway_));
  storage_ = nullptr;
  crypto_ = nullptr;
  trust_ = nullptr;
  remoteConnectivityAllowed_ = false;
  status_ = ConnectionConfigStatus{};
}

ConfigResult ConnectionConfigStore::setResult(ConfigResult result) {
  status_.lastResult = result;
  return result;
}

bool ConnectionConfigStore::generationAfter(uint32_t candidate,
                                             uint32_t reference) {
  return static_cast<int32_t>(candidate - reference) > 0;
}

ConfigResult ConnectionConfigStore::loadSlot(
    uint8_t slot, uint8_t* outer, uint8_t* plain, uint32_t& generation,
    uint16_t& version, bool& nonempty) const {
  generation = 0U;
  version = 0U;
  nonempty = false;
  if (!storage_ || !crypto_ || !trust_ || !outer || !plain ||
      slot >= kConnectionSlotCount) {
    return ConfigResult::InvalidArgument;
  }
  size_t loaded = 0U;
  if (!storage_->readSlot(slot, outer, kConnectionSnapshotBytes, loaded)) {
    return ConfigResult::StorageReadFailed;
  }
  nonempty = loaded != 0U;
  if (!nonempty) return ConfigResult::Ok;
  version = loaded >= 6U ? getU16(outer + 4U) : 0U;
  if (loaded != kConnectionSnapshotBytes ||
      memcmp(outer, kOuterMagic, sizeof(kOuterMagic)) != 0 ||
      (version != kLegacyConnectionVersion && version != kConnectionVersion) ||
      getU16(outer + 6U) != kConnectionOuterHeaderBytes ||
      getU16(outer + 12U) != kConnectionPlainBytes ||
      getU16(outer + 14U) != kConnectionPlainBytes ||
      crc32(outer, 44U) != getU32(outer + 44U)) {
    return ConfigResult::StorageCorrupt;
  }
  generation = getU32(outer + 8U);
  if (generation == 0U ||
      !crypto_->open(outer + 16U, outer, 28U,
                     outer + kConnectionOuterHeaderBytes,
                     kConnectionPlainBytes, outer + 28U, plain) ||
      getU16(plain + 4U) != version ||
      !validatePlain(plain, generation, *trust_)) {
    secureZero(plain, kConnectionPlainBytes);
    return ConfigResult::StorageCorrupt;
  }
  return ConfigResult::Ok;
}

ConfigResult ConnectionConfigStore::loadActivePlain(uint8_t* outer,
                                                     uint8_t* plain) const {
  if (status_.activeSlot < 0) {
    initializePlain(plain);
    return ConfigResult::Ok;
  }
  uint32_t generation = 0U;
  uint16_t version = 0U;
  bool nonempty = false;
  const ConfigResult result = loadSlot(
      static_cast<uint8_t>(status_.activeSlot), outer, plain, generation,
      version, nonempty);
  if (result != ConfigResult::Ok || !nonempty ||
      generation != status_.generation || version != kConnectionVersion) {
    return result == ConfigResult::Ok ? ConfigResult::StorageCorrupt : result;
  }
  return ConfigResult::Ok;
}

bool ConnectionConfigStore::decodeResident(const uint8_t* plain,
                                            uint32_t generation) {
  if (!plain || !trust_ || !validatePlain(plain, generation, *trust_)) {
    return false;
  }
  const uint32_t flags = getU32(plain + 8U);
  // GatewayConfig is ~8.8 KiB, so decode directly into the resident member
  // only after the complete plaintext has authenticated and validated.
  secureZero(&gateway_, sizeof(gateway_));
  if ((flags & kFlagGateway) != 0U) {
    gateway_.mobileRelayOnly = (flags & kFlagMobileRelay) != 0U;
    memcpy(gateway_.gatewayId, plain + kGatewayIdOffset, 16U);
    gateway_.hostBytes = getU16(plain + kGatewayHostLengthOffset);
    gateway_.bootstrapPort =
        getU16(plain + kGatewayBootstrapPortOffset);
    gateway_.port = getU16(plain + kGatewayPortOffset);
    gateway_.serverNameBytes =
        getU16(plain + kGatewayServerNameLengthOffset);
    gateway_.caCertificateBytes = getU16(plain + kGatewayCaLengthOffset);
    memcpy(gateway_.host, plain + kGatewayHostOffset, gateway_.hostBytes);
    gateway_.host[gateway_.hostBytes] = '\0';
    memcpy(gateway_.serverName, plain + kGatewayServerNameOffset,
           gateway_.serverNameBytes);
    gateway_.serverName[gateway_.serverNameBytes] = '\0';
    memcpy(gateway_.caCertificateDer, plain + kGatewayCaOffset,
           gateway_.caCertificateBytes);
    memcpy(gateway_.spkiSha256, plain + kGatewaySpkiOffset,
           sizeof(gateway_.spkiSha256));
  }
  status_.wifiConfigured = (flags & kFlagWifi) != 0U;
  status_.gatewayConfigured = (flags & kFlagGateway) != 0U;
  status_.mobileRelayConfigured =
      (flags & kFlagMobileRelay) != 0U;
  status_.gatewayLanConfigured = status_.gatewayConfigured &&
      !status_.mobileRelayConfigured;
  status_.gatewayEnrolled = (flags & kFlagEnrollment) != 0U;
  return true;
}

ConfigResult ConnectionConfigStore::begin(ConnectionSlotStorage& storage,
                                           ConnectionStoreCrypto& crypto,
                                           GatewayTrustValidator& trust) {
  clear();
  storage_ = &storage;
  crypto_ = &crypto;
  trust_ = &trust;
  if (!storage.available()) return setResult(ConfigResult::StorageUnavailable);
  if (!crypto.ready()) return setResult(ConfigResult::SecurityUnavailable);
  TransientBuffers buffers;
  if (!buffers.ready()) {
    return setResult(ConfigResult::StorageAllocationFailed);
  }
  uint32_t generations[kConnectionSlotCount]{};
  uint16_t versions[kConnectionSlotCount]{};
  bool valid[kConnectionSlotCount]{};
  bool anyNonempty = false;
  for (uint8_t slot = 0U; slot < kConnectionSlotCount; ++slot) {
    bool nonempty = false;
    const ConfigResult result = loadSlot(slot, buffers.outer, buffers.plain,
                                         generations[slot], versions[slot],
                                         nonempty);
    anyNonempty = anyNonempty || nonempty;
    if (result == ConfigResult::StorageReadFailed) return setResult(result);
    valid[slot] = result == ConfigResult::Ok && nonempty;
  }
  int chosen = -1;
  for (uint8_t slot = 0U; slot < kConnectionSlotCount; ++slot) {
    if (valid[slot] &&
        (chosen < 0 || generationAfter(generations[slot],
                                       generations[chosen]))) {
      chosen = slot;
    }
  }
  if (chosen < 0) {
    if (anyNonempty) return setResult(ConfigResult::StorageCorrupt);
    status_.begun = true;
    return setResult(ConfigResult::Ok);
  }
  uint32_t generation = 0U;
  uint16_t version = 0U;
  bool nonempty = false;
  if (loadSlot(static_cast<uint8_t>(chosen), buffers.outer, buffers.plain,
                generation, version, nonempty) != ConfigResult::Ok ||
      !nonempty) {
    return setResult(ConfigResult::StorageCorrupt);
  }
  status_.activeSlot = static_cast<int8_t>(chosen);
  status_.generation = generation;
  status_.begun = true;
  if (version == kLegacyConnectionVersion) {
    if (generation == UINT32_MAX) return setResult(ConfigResult::CryptoFailed);
    // validatePlain already rejected any ambiguous legacy gateway port.
    putU16(buffers.plain + 4U, kConnectionVersion);
    if ((getU32(buffers.plain + 8U) & kFlagGateway) != 0U) {
      putU16(buffers.plain + kGatewayBootstrapPortOffset,
             kLegacyBootstrapPort);
    }
    const ConfigResult migrated = persistPlain(
        buffers.outer, buffers.plain, generation + 1U);
    if (migrated != ConfigResult::Ok) return setResult(migrated);
    return setResult(ConfigResult::Ok);
  }
  if (!decodeResident(buffers.plain, generation)) {
    return setResult(ConfigResult::StorageCorrupt);
  }
  return setResult(ConfigResult::Ok);
}

ConfigResult ConnectionConfigStore::persistPlain(uint8_t* outer,
                                                 uint8_t* plain,
                                                 uint32_t generation) {
  if (!outer || !plain || generation == 0U) {
    return setResult(ConfigResult::InvalidArgument);
  }
  putU16(plain + 4U, kConnectionVersion);
  putU32(plain + 12U, generation);
  putU32(plain + kPlainCrcOffset, crc32(plain, kPlainCrcOffset));
  memset(outer, 0, kConnectionSnapshotBytes);
  memcpy(outer, kOuterMagic, sizeof(kOuterMagic));
  putU16(outer + 4U, kConnectionVersion);
  putU16(outer + 6U, static_cast<uint16_t>(kConnectionOuterHeaderBytes));
  putU32(outer + 8U, generation);
  putU16(outer + 12U, static_cast<uint16_t>(kConnectionPlainBytes));
  putU16(outer + 14U, static_cast<uint16_t>(kConnectionPlainBytes));
  if (!crypto_->randomBytes(outer + 16U, 12U) ||
      !crypto_->seal(outer + 16U, outer, 28U, plain,
                     kConnectionPlainBytes,
                     outer + kConnectionOuterHeaderBytes, outer + 28U)) {
    return setResult(ConfigResult::CryptoFailed);
  }
  putU32(outer + 44U, crc32(outer, 44U));
  const uint8_t target = status_.activeSlot < 0
      ? 0U
      : static_cast<uint8_t>((status_.activeSlot + 1) %
                             kConnectionSlotCount);
  if (!storage_->writeSlot(target, outer, kConnectionSnapshotBytes)) {
    return setResult(ConfigResult::StorageWriteFailed);
  }
  secureZero(plain, kConnectionPlainBytes);
  uint32_t readbackGeneration = 0U;
  uint16_t readbackVersion = 0U;
  bool nonempty = false;
  const ConfigResult readback = loadSlot(target, outer, plain,
                                         readbackGeneration,
                                         readbackVersion, nonempty);
  if (readback != ConfigResult::Ok || !nonempty ||
      readbackGeneration != generation ||
      readbackVersion != kConnectionVersion ||
      !decodeResident(plain, generation)) {
    return setResult(ConfigResult::StorageReadbackFailed);
  }
  status_.activeSlot = static_cast<int8_t>(target);
  status_.generation = generation;
  return setResult(ConfigResult::Ok);
}

ConfigResult ConnectionConfigStore::commitWifi(const WifiConfig& config) {
  if (!status_.begun) return setResult(ConfigResult::NotBegun);
  if (!validWifiStruct(config)) return setResult(ConfigResult::InvalidArgument);
  if (status_.generation == UINT32_MAX) {
    return setResult(ConfigResult::CryptoFailed);
  }
  TransientBuffers buffers;
  if (!buffers.ready()) {
    return setResult(ConfigResult::StorageAllocationFailed);
  }
  const ConfigResult loaded = loadActivePlain(buffers.outer, buffers.plain);
  if (loaded != ConfigResult::Ok) return setResult(loaded);
  encodeWifi(buffers.plain, config);
  return persistPlain(buffers.outer, buffers.plain, status_.generation + 1U);
}

ConfigResult ConnectionConfigStore::commitGateway(
    const GatewayConfig& config) {
  if (!status_.begun) return setResult(ConfigResult::NotBegun);
  if (!trust_ || !validGatewayStruct(config, *trust_)) {
    return setResult(ConfigResult::InvalidArgument);
  }
  if (status_.generation == UINT32_MAX) {
    return setResult(ConfigResult::CryptoFailed);
  }
  TransientBuffers buffers;
  if (!buffers.ready()) {
    return setResult(ConfigResult::StorageAllocationFailed);
  }
  const ConfigResult loaded = loadActivePlain(buffers.outer, buffers.plain);
  if (loaded != ConfigResult::Ok) return setResult(loaded);
  encodeGateway(buffers.plain, config);
  return persistPlain(buffers.outer, buffers.plain, status_.generation + 1U);
}

MobileRelayGatewayConfigResult
ConnectionConfigStore::commitMobileRelayGateway(
    const uint8_t gatewayUuid[kEnrollmentUuidBytes],
    const uint8_t* caCertificateDer, size_t caCertificateBytes) {
  if (!gatewayUuid || !caCertificateDer || caCertificateBytes == 0U ||
      caCertificateBytes > kGatewayCaMaximumBytes) {
    setResult(ConfigResult::InvalidArgument);
    return MobileRelayGatewayConfigResult::Failed;
  }
  // Android repeats relay_configure after reconnect. An exact identity/trust
  // replay is a read-only success so it cannot erase a completed enrollment
  // or rotate the power-loss-safe generation.
  if (status_.begun && status_.mobileRelayConfigured &&
      memcmp(gateway_.gatewayId, gatewayUuid, kEnrollmentUuidBytes) == 0 &&
      gateway_.caCertificateBytes == caCertificateBytes &&
      memcmp(gateway_.caCertificateDer, caCertificateDer,
             caCertificateBytes) == 0) {
    setResult(ConfigResult::Ok);
    return MobileRelayGatewayConfigResult::Unchanged;
  }
  GatewayConfig relay{};
  relay.mobileRelayOnly = true;
  memcpy(relay.gatewayId, gatewayUuid, sizeof(relay.gatewayId));
  memcpy(relay.caCertificateDer, caCertificateDer, caCertificateBytes);
  relay.caCertificateBytes = static_cast<uint16_t>(caCertificateBytes);
  const ConfigResult result = commitGateway(relay);
  secureZero(&relay, sizeof(relay));
  return result == ConfigResult::Ok
      ? MobileRelayGatewayConfigResult::Changed
      : MobileRelayGatewayConfigResult::Failed;
}

void ConnectionConfigStore::setRemoteConnectivityAllowed(bool allowed) {
  remoteConnectivityAllowed_ = allowed;
  if (!allowed) {
    GatewayLanCredentialView lease{};
    release(lease);
  }
}

bool ConnectionConfigStore::copyWifi(WifiConfig& output) const {
  secureZero(&output, sizeof(output));
  if (!status_.begun || !status_.wifiConfigured ||
      status_.activeSlot < 0) {
    return false;
  }
  // The passphrase is not a permanently resident store member.  Decrypt it
  // only for the association handoff; the caller wipes its short-lived copy
  // immediately after WiFi.begin(), and these larger buffers wipe on return.
  TransientBuffers buffers;
  if (!buffers.ready()) return false;
  uint32_t generation = 0U;
  uint16_t version = 0U;
  bool nonempty = false;
  if (loadSlot(static_cast<uint8_t>(status_.activeSlot), buffers.outer,
               buffers.plain, generation, version, nonempty) !=
          ConfigResult::Ok ||
      !nonempty || version != kConnectionVersion ||
      generation != status_.generation ||
      (getU32(buffers.plain + 8U) & kFlagWifi) == 0U) {
    return false;
  }
  output.ssidBytes = buffers.plain[kWifiSsidLengthOffset];
  output.passphraseBytes = buffers.plain[kWifiPassphraseLengthOffset];
  output.security = static_cast<WifiSecurity>(
      buffers.plain[kWifiSecurityOffset]);
  memcpy(output.ssid, buffers.plain + kWifiSsidOffset, output.ssidBytes);
  output.ssid[output.ssidBytes] = 0U;
  memcpy(output.passphrase, buffers.plain + kWifiPassphraseOffset,
         output.passphraseBytes);
  output.passphrase[output.passphraseBytes] = '\0';
  return true;
}

bool ConnectionConfigStore::copyGateway(GatewayConfig& output) const {
  if (!status_.begun || !status_.gatewayConfigured) return false;
  output = gateway_;
  return true;
}

bool ConnectionConfigStore::copyGatewayId(
    uint8_t output[kEnrollmentUuidBytes]) const {
  if (!status_.begun || !status_.gatewayConfigured || !output) return false;
  memcpy(output, gateway_.gatewayId, kEnrollmentUuidBytes);
  return true;
}

ConnectionConfigStatus ConnectionConfigStore::status() const {
  return status_;
}

bool ConnectionConfigStore::ready() const { return status_.begun; }

bool ConnectionConfigStore::remoteConnectivityAllowed() const {
  return remoteConnectivityAllowed_ && status_.begun;
}

bool ConnectionConfigStore::acquire(GatewayLanCredentialView& output) {
  output = GatewayLanCredentialView{};
  if (!remoteConnectivityAllowed() || !status_.gatewayConfigured ||
      !status_.gatewayEnrolled || status_.activeSlot < 0 ||
      credentialLeasePlain_) {
    return false;
  }
  uint8_t* outer =
      new (std::nothrow) uint8_t[kConnectionSnapshotBytes];
  uint8_t* plain = new (std::nothrow) uint8_t[kConnectionPlainBytes];
  if (!outer || !plain) {
    if (outer) {
      secureZero(outer, kConnectionSnapshotBytes);
      delete[] outer;
    }
    if (plain) {
      secureZero(plain, kConnectionPlainBytes);
      delete[] plain;
    }
    return false;
  }
  uint32_t generation = 0U;
  uint16_t version = 0U;
  bool nonempty = false;
  if (loadSlot(static_cast<uint8_t>(status_.activeSlot), outer, plain,
                generation, version, nonempty) != ConfigResult::Ok ||
      !nonempty || version != kConnectionVersion ||
      generation != status_.generation ||
      (getU32(plain + 8U) & kFlagEnrollment) == 0U) {
    secureZero(outer, kConnectionSnapshotBytes);
    secureZero(plain, kConnectionPlainBytes);
    delete[] outer;
    delete[] plain;
    return false;
  }
  // Ciphertext is no longer needed once the generation authenticates.  Free
  // it before TLS allocates handshake state; only the 30 KiB plaintext lease
  // remains until the credential consumer calls release().
  secureZero(outer, kConnectionSnapshotBytes);
  delete[] outer;
  outer = nullptr;
  output.host = gateway_.host;
  output.serverName = gateway_.serverName;
  output.port = gateway_.port;
  output.caCertificateDer = gateway_.caCertificateDer;
  output.caCertificateBytes = gateway_.caCertificateBytes;
  memcpy(output.spkiSha256, gateway_.spkiSha256,
         sizeof(output.spkiSha256));
  memcpy(output.companionUuid, plain + kEnrollmentCompanionIdOffset,
         sizeof(output.companionUuid));
  memcpy(output.gatewayUuid, plain + kEnrollmentGatewayIdOffset,
         sizeof(output.gatewayUuid));
  output.keyVersion = getU32(plain + kEnrollmentKeyVersionOffset);
  output.privateKey = plain + kEnrollmentPrivateKeyOffset;
  output.privateKeyBytes = kEnrollmentPrivateKeyBytes;
  output.leafCertificateDer = plain + kEnrollmentCertificateOffset;
  output.leafCertificateBytes =
      getU16(plain + kEnrollmentCertificateLengthOffset);
  output.certificateChainCount = plain[kEnrollmentChainCountOffset];
  size_t chainCursor = 0U;
  for (size_t i = 0U; i < output.certificateChainCount; ++i) {
    output.certificateChainBytes[i] = getU16(
        plain + kEnrollmentChainLengthsOffset + i * 2U);
    output.certificateChainDer[i] = plain + kEnrollmentChainOffset +
                                    chainCursor;
    chainCursor += output.certificateChainBytes[i];
  }
  output.backendHmacSecret = plain + kEnrollmentBackendSecretOffset;
  output.backendHmacSecretBytes = kEnrollmentSecretBytes;
  credentialLeasePlain_ = plain;
  return true;
}

void ConnectionConfigStore::release(GatewayLanCredentialView& view) {
  view = GatewayLanCredentialView{};
  if (credentialLeasePlain_) {
    secureZero(credentialLeasePlain_, kConnectionPlainBytes);
    delete[] credentialLeasePlain_;
    credentialLeasePlain_ = nullptr;
  }
}

bool ConnectionConfigStore::commitEnrollmentCredential(
    const uint8_t companionUuid[kEnrollmentUuidBytes],
    const uint8_t gatewayUuid[kEnrollmentUuidBytes], uint32_t keyVersion,
    const uint8_t mtlsPrivateKey[kEnrollmentPrivateKeyBytes],
    const uint8_t* certificateDer, size_t certificateBytes,
    const uint8_t* const* certificateChainDer,
    const size_t* certificateChainBytes, size_t certificateChainCount,
    const uint8_t backendHmacSecret[kEnrollmentSecretBytes]) {
  if (!status_.begun || !status_.gatewayConfigured || !trust_ ||
      !companionUuid || !gatewayUuid || keyVersion == 0U ||
      !mtlsPrivateKey || !certificateDer || certificateBytes == 0U ||
      certificateBytes > kEnrollmentMaximumCertificateBytes ||
      certificateChainCount == 0U ||
      certificateChainCount > kEnrollmentMaximumChainCertificates ||
      (certificateChainCount != 0U &&
       (!certificateChainDer || !certificateChainBytes)) ||
      !backendHmacSecret || allZero(backendHmacSecret, kEnrollmentSecretBytes) ||
      allZero(companionUuid, kEnrollmentUuidBytes) ||
      allZero(mtlsPrivateKey, kEnrollmentPrivateKeyBytes) ||
      memcmp(gatewayUuid, gateway_.gatewayId, kEnrollmentUuidBytes) != 0 ||
      status_.generation == UINT32_MAX) {
    setResult(ConfigResult::EnrollmentInvalid);
    return false;
  }
  size_t chainTotal = 0U;
  for (size_t i = 0U; i < certificateChainCount; ++i) {
    if (!certificateChainDer[i] || certificateChainBytes[i] == 0U ||
        certificateChainBytes[i] > kEnrollmentMaximumCertificateBytes ||
        chainTotal + certificateChainBytes[i] >
            kEnrollmentMaximumChainBytes) {
      setResult(ConfigResult::EnrollmentInvalid);
      return false;
    }
    chainTotal += certificateChainBytes[i];
  }
  if (!trust_->validateEnrollmentChain(
          gateway_.caCertificateDer, gateway_.caCertificateBytes,
          certificateDer, certificateBytes, certificateChainDer,
          certificateChainBytes, certificateChainCount)) {
    setResult(ConfigResult::EnrollmentTrustFailed);
    return false;
  }
  TransientBuffers buffers;
  if (!buffers.ready()) {
    setResult(ConfigResult::StorageAllocationFailed);
    return false;
  }
  const ConfigResult loaded = loadActivePlain(buffers.outer, buffers.plain);
  if (loaded != ConfigResult::Ok) {
    setResult(loaded);
    return false;
  }
  uint8_t* plain = buffers.plain;
  memset(plain + kEnrollmentOffset, 0,
         kUsedPlainBytes - kEnrollmentOffset);
  memcpy(plain + kEnrollmentCompanionIdOffset, companionUuid, 16U);
  memcpy(plain + kEnrollmentGatewayIdOffset, gatewayUuid, 16U);
  putU32(plain + kEnrollmentKeyVersionOffset, keyVersion);
  memcpy(plain + kEnrollmentPrivateKeyOffset, mtlsPrivateKey,
         kEnrollmentPrivateKeyBytes);
  putU16(plain + kEnrollmentCertificateLengthOffset,
         static_cast<uint16_t>(certificateBytes));
  plain[kEnrollmentChainCountOffset] =
      static_cast<uint8_t>(certificateChainCount);
  putU16(plain + kEnrollmentChainTotalOffset,
         static_cast<uint16_t>(chainTotal));
  memcpy(plain + kEnrollmentBackendSecretOffset, backendHmacSecret,
         kEnrollmentSecretBytes);
  memcpy(plain + kEnrollmentCertificateOffset, certificateDer,
         certificateBytes);
  size_t cursor = 0U;
  for (size_t i = 0U; i < certificateChainCount; ++i) {
    putU16(plain + kEnrollmentChainLengthsOffset + i * 2U,
           static_cast<uint16_t>(certificateChainBytes[i]));
    memcpy(plain + kEnrollmentChainOffset + cursor, certificateChainDer[i],
           certificateChainBytes[i]);
    cursor += certificateChainBytes[i];
  }
  putU32(plain + 8U, getU32(plain + 8U) | kFlagEnrollment);
  return persistPlain(buffers.outer, buffers.plain,
                      status_.generation + 1U) == ConfigResult::Ok;
}

}  // namespace connectivity
}  // namespace kitsu868
