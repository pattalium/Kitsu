#include "kitsu_ble_ota.h"

#include <stdio.h>
#include <string.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <ed_25519.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#endif

namespace kitsu868 {
namespace connectivity {
namespace {

constexpr uint32_t kJournalOffset = kBleOtaMaximumImageBytes;
constexpr uint32_t kJournalHeaderBytes = 128U;
constexpr uint32_t kJournalCheckpointBytes = 16U;
constexpr uint16_t kJournalCheckpointCapacity = static_cast<uint16_t>(
    (kBleOtaJournalBytes - kJournalHeaderBytes) /
    kJournalCheckpointBytes);
constexpr uint8_t kJournalReceiving = 1U;
constexpr uint8_t kJournalReady = 2U;
constexpr uint8_t kJournalAborted = 3U;
constexpr uint8_t kJournalVersion = 1U;

// Raw key from the installed Kitsu Ed25519 SPKI whose SHA-256 is
// df530766fbc4fc93e82cdbd354ebe4a17a453c83e9bb7fe2af30ca2d202494ab.
constexpr uint8_t kUpdateAuthority[kBleOtaDigestBytes] = {
    0x24U, 0x00U, 0x11U, 0xf1U, 0x49U, 0xe9U, 0xcfU, 0xb9U,
    0xfbU, 0x87U, 0xfaU, 0xb4U, 0xd9U, 0xcaU, 0x45U, 0x73U,
    0xc1U, 0xc7U, 0xffU, 0xb3U, 0x87U, 0x17U, 0x76U, 0x58U,
    0x00U, 0x05U, 0xecU, 0x41U, 0xaeU, 0xe5U, 0x13U, 0x82U,
};

struct Span {
  constexpr Span(const uint8_t* input = nullptr, size_t inputBytes = 0U)
      : data(input), bytes(inputBytes) {}
  const uint8_t* data;
  size_t bytes;
};

void secureZero(void* memory, size_t bytes) {
  volatile uint8_t* output = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *output++ = 0U;
}

bool constantTimeEqual(const uint8_t* left, const uint8_t* right,
                       size_t bytes) {
  if (!left || !right) return false;
  uint8_t difference = 0U;
  for (size_t index = 0U; index < bytes; ++index) {
    difference |= static_cast<uint8_t>(left[index] ^ right[index]);
  }
  return difference == 0U;
}

bool allBytes(const uint8_t* input, size_t bytes, uint8_t expected) {
  if (!input) return false;
  uint8_t difference = 0U;
  for (size_t index = 0U; index < bytes; ++index) {
    difference |= static_cast<uint8_t>(input[index] ^ expected);
  }
  return difference == 0U;
}

bool deadlineReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

uint32_t getU32Le(const uint8_t* input) {
  return static_cast<uint32_t>(input[0]) |
      (static_cast<uint32_t>(input[1]) << 8U) |
      (static_cast<uint32_t>(input[2]) << 16U) |
      (static_cast<uint32_t>(input[3]) << 24U);
}

uint16_t getU16Le(const uint8_t* input) {
  return static_cast<uint16_t>(
      static_cast<uint16_t>(input[0]) |
      static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8U));
}

void putU32Le(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
  output[2] = static_cast<uint8_t>(value >> 16U);
  output[3] = static_cast<uint8_t>(value >> 24U);
}

void putU16Le(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
}

uint32_t crc32(const uint8_t* input, size_t inputBytes) {
  uint32_t value = 0xffffffffUL;
  for (size_t index = 0U; index < inputBytes; ++index) {
    value ^= input[index];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      value = (value >> 1U) ^
          (0xedb88320UL & static_cast<uint32_t>(
              0U - static_cast<uint32_t>(value & 1U)));
    }
  }
  return value ^ 0xffffffffUL;
}

bool asciiAlphaNumeric(uint8_t value) {
  return (value >= '0' && value <= '9') ||
      (value >= 'A' && value <= 'Z') ||
      (value >= 'a' && value <= 'z');
}

bool validReleaseId(const uint8_t* input, size_t bytes) {
  if (!input || bytes == 0U || bytes > 64U ||
      !asciiAlphaNumeric(input[0])) {
    return false;
  }
  for (size_t index = 1U; index < bytes; ++index) {
    const uint8_t value = input[index];
    if (!asciiAlphaNumeric(value) && value != '.' && value != '_' &&
        value != '-') {
      return false;
    }
  }
  return true;
}

bool validSemver(const uint8_t* input, size_t bytes) {
  if (!input || bytes == 0U || bytes > kBleOtaVersionMaximumBytes) {
    return false;
  }
  size_t cursor = 0U;
  for (uint8_t component = 0U; component < 3U; ++component) {
    const size_t start = cursor;
    while (cursor < bytes && input[cursor] >= '0' && input[cursor] <= '9') {
      ++cursor;
    }
    if (cursor == start ||
        (cursor - start > 1U && input[start] == '0')) {
      return false;
    }
    if (component != 2U) {
      if (cursor >= bytes || input[cursor++] != '.') return false;
    }
  }
  if (cursor == bytes) return true;
  if (input[cursor] != '-' && input[cursor] != '+') return false;
  ++cursor;
  const size_t suffix = cursor;
  while (cursor < bytes) {
    const uint8_t value = input[cursor++];
    if (!asciiAlphaNumeric(value) && value != '.' && value != '-') {
      return false;
    }
  }
  return cursor > suffix;
}

bool validSemverString(const char* input) {
  if (!input) return false;
  size_t bytes = 0U;
  while (bytes <= kBleOtaVersionMaximumBytes && input[bytes] != '\0') {
    ++bytes;
  }
  return bytes <= kBleOtaVersionMaximumBytes &&
      validSemver(reinterpret_cast<const uint8_t*>(input), bytes);
}

bool decimal(const uint8_t* input, size_t inputBytes, size_t& cursor,
             uint32_t& output) {
  const size_t start = cursor;
  uint64_t value = 0U;
  while (cursor < inputBytes && input[cursor] >= '0' &&
         input[cursor] <= '9') {
    const uint8_t digit = static_cast<uint8_t>(input[cursor++] - '0');
    value = value * 10ULL + digit;
    if (value > UINT32_MAX) return false;
  }
  if (cursor == start ||
      (cursor - start > 1U && input[start] == '0')) {
    return false;
  }
  output = static_cast<uint32_t>(value);
  return true;
}

bool literal(const uint8_t* input, size_t inputBytes, size_t& cursor,
             const char* expected) {
  if (!input || !expected) return false;
  const size_t expectedBytes = strlen(expected);
  if (cursor + expectedBytes > inputBytes ||
      memcmp(input + cursor, expected, expectedBytes) != 0) {
    return false;
  }
  cursor += expectedBytes;
  return true;
}

bool spanUntilQuote(const uint8_t* input, size_t inputBytes, size_t& cursor,
                    Span& output) {
  const size_t start = cursor;
  while (cursor < inputBytes && input[cursor] != '"') {
    const uint8_t value = input[cursor];
    if (value < 0x20U || value > 0x7eU || value == '\\') return false;
    ++cursor;
  }
  if (cursor >= inputBytes) return false;
  output.data = input + start;
  output.bytes = cursor - start;
  return true;
}

int base64Value(uint8_t value) {
  if (value >= 'A' && value <= 'Z') return value - 'A';
  if (value >= 'a' && value <= 'z') return value - 'a' + 26;
  if (value >= '0' && value <= '9') return value - '0' + 52;
  if (value == '-') return 62;
  if (value == '_') return 63;
  return -1;
}

bool decodeBase64Url(const Span& input, uint8_t* output,
                     size_t outputCapacity, size_t& outputBytes) {
  outputBytes = 0U;
  if ((!input.data && input.bytes != 0U) || !output ||
      input.bytes == 0U || input.bytes % 4U == 1U) {
    return false;
  }
  const size_t decoded = (input.bytes / 4U) * 3U +
      (input.bytes % 4U == 2U ? 1U : input.bytes % 4U == 3U ? 2U : 0U);
  if (decoded > outputCapacity) return false;

  size_t cursor = 0U;
  while (cursor + 4U <= input.bytes) {
    const int a = base64Value(input.data[cursor]);
    const int b = base64Value(input.data[cursor + 1U]);
    const int c = base64Value(input.data[cursor + 2U]);
    const int d = base64Value(input.data[cursor + 3U]);
    if (a < 0 || b < 0 || c < 0 || d < 0) return false;
    output[outputBytes++] = static_cast<uint8_t>((a << 2U) | (b >> 4U));
    output[outputBytes++] = static_cast<uint8_t>((b << 4U) | (c >> 2U));
    output[outputBytes++] = static_cast<uint8_t>((c << 6U) | d);
    cursor += 4U;
  }
  const size_t tail = input.bytes - cursor;
  if (tail == 2U) {
    const int a = base64Value(input.data[cursor]);
    const int b = base64Value(input.data[cursor + 1U]);
    if (a < 0 || b < 0 || (b & 0x0f) != 0) return false;
    output[outputBytes++] = static_cast<uint8_t>((a << 2U) | (b >> 4U));
  } else if (tail == 3U) {
    const int a = base64Value(input.data[cursor]);
    const int b = base64Value(input.data[cursor + 1U]);
    const int c = base64Value(input.data[cursor + 2U]);
    if (a < 0 || b < 0 || c < 0 || (c & 0x03) != 0) return false;
    output[outputBytes++] = static_cast<uint8_t>((a << 2U) | (b >> 4U));
    output[outputBytes++] = static_cast<uint8_t>((b << 4U) | (c >> 2U));
  } else if (tail != 0U) {
    return false;
  }
  return outputBytes == decoded;
}

int hexValue(uint8_t value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return -1;
}

bool decodeLowerHex(const Span& input, uint8_t* output, size_t outputBytes) {
  if (!input.data || !output || input.bytes != outputBytes * 2U) {
    return false;
  }
  for (size_t index = 0U; index < outputBytes; ++index) {
    const int high = hexValue(input.data[index * 2U]);
    const int low = hexValue(input.data[index * 2U + 1U]);
    if (high < 0 || low < 0) return false;
    output[index] = static_cast<uint8_t>((high << 4U) | low);
  }
  return true;
}

void encodeLowerHex(const uint8_t* input, size_t inputBytes, char* output) {
  static const char alphabet[] = "0123456789abcdef";
  for (size_t index = 0U; index < inputBytes; ++index) {
    output[index * 2U] = alphabet[input[index] >> 4U];
    output[index * 2U + 1U] = alphabet[input[index] & 0x0fU];
  }
  output[inputBytes * 2U] = '\0';
}

bool parseIdRequest(const uint8_t* payload, size_t payloadBytes,
                    uint8_t output[kBleOtaDigestBytes]) {
  static const char prefix[] = "{\"update_id\":\"";
  static const char suffix[] = "\"}";
  if (!payload || payloadBytes != sizeof(prefix) - 1U + 64U +
                                       sizeof(suffix) - 1U ||
      memcmp(payload, prefix, sizeof(prefix) - 1U) != 0 ||
      memcmp(payload + payloadBytes - (sizeof(suffix) - 1U), suffix,
             sizeof(suffix) - 1U) != 0) {
    return false;
  }
  const Span id{payload + sizeof(prefix) - 1U, 64U};
  return decodeLowerHex(id, output, kBleOtaDigestBytes);
}

bool parseBeginRequest(const uint8_t* payload, size_t payloadBytes,
                       Span& manifest, Span& signature) {
  size_t cursor = 0U;
  if (!literal(payload, payloadBytes, cursor, "{\"manifest_b64\":\"") ||
      !spanUntilQuote(payload, payloadBytes, cursor, manifest) ||
      !literal(payload, payloadBytes, cursor,
               "\",\"signature_b64\":\"") ||
      !spanUntilQuote(payload, payloadBytes, cursor, signature) ||
      !literal(payload, payloadBytes, cursor, "\"}")) {
    return false;
  }
  return cursor == payloadBytes && manifest.bytes != 0U &&
      signature.bytes != 0U;
}

bool parseWriteRequest(const uint8_t* payload, size_t payloadBytes,
                       uint8_t updateId[kBleOtaDigestBytes],
                       uint32_t& offset, Span& encodedData) {
  size_t cursor = 0U;
  if (!literal(payload, payloadBytes, cursor, "{\"update_id\":\"") ||
      cursor + 64U > payloadBytes) {
    return false;
  }
  const Span id{payload + cursor, 64U};
  cursor += id.bytes;
  if (!decodeLowerHex(id, updateId, kBleOtaDigestBytes) ||
      !literal(payload, payloadBytes, cursor, "\",\"offset\":") ||
      !decimal(payload, payloadBytes, cursor, offset) ||
      !literal(payload, payloadBytes, cursor, ",\"data_b64\":\"") ||
      !spanUntilQuote(payload, payloadBytes, cursor, encodedData) ||
      !literal(payload, payloadBytes, cursor, "\"}")) {
    return false;
  }
  return cursor == payloadBytes && encodedData.bytes != 0U;
}

uint32_t rotateRight(uint32_t value, uint8_t bits) {
  return (value >> bits) | (value << (32U - bits));
}

}  // namespace

KitsuBleOta::KitsuBleOta() = default;

KitsuBleOta::~KitsuBleOta() {
  secureZero(updateId_, sizeof(updateId_));
  secureZero(expectedImageSha256_, sizeof(expectedImageSha256_));
  secureZero(manifestScratch_, sizeof(manifestScratch_));
  secureZero(chunkScratch_, sizeof(chunkScratch_));
  secureZero(ioScratch_, sizeof(ioScratch_));
  secureZero(&prefixHash_, sizeof(prefixHash_));
}

const char* KitsuBleOta::stateName(BleOtaState state) {
  switch (state) {
    case BleOtaState::Idle: return "idle";
    case BleOtaState::Receiving: return "receiving";
    case BleOtaState::ReadyToReboot: return "ready_to_reboot";
    case BleOtaState::PendingVerify: return "pending_verify";
    case BleOtaState::Confirmed: return "confirmed";
    case BleOtaState::RolledBack: return "rolled_back";
    case BleOtaState::Failed: return "failed";
  }
  return "failed";
}

void KitsuBleOta::shaStart(Sha256Context& context) {
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

void KitsuBleOta::shaTransform(Sha256Context& context,
                               const uint8_t block[64]) {
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
  secureZero(schedule, sizeof(schedule));
}

void KitsuBleOta::shaUpdate(Sha256Context& context, const uint8_t* input,
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
      shaTransform(context, context.block);
      context.blockBytes = 0U;
    }
  }
}

bool KitsuBleOta::shaFinish(Sha256Context& context,
                            uint8_t output[kBleOtaDigestBytes]) {
  if (!output) return false;
  const uint64_t totalBits = context.totalBytes * 8ULL;
  context.block[context.blockBytes++] = 0x80U;
  if (context.blockBytes > 56U) {
    memset(context.block + context.blockBytes, 0,
           sizeof(context.block) - context.blockBytes);
    shaTransform(context, context.block);
    context.blockBytes = 0U;
  }
  memset(context.block + context.blockBytes, 0, 56U - context.blockBytes);
  for (size_t index = 0U; index < 8U; ++index) {
    context.block[56U + index] = static_cast<uint8_t>(
        totalBits >> ((7U - index) * 8U));
  }
  shaTransform(context, context.block);
  for (size_t index = 0U; index < 8U; ++index) {
    output[index * 4U] = static_cast<uint8_t>(context.state[index] >> 24U);
    output[index * 4U + 1U] =
        static_cast<uint8_t>(context.state[index] >> 16U);
    output[index * 4U + 2U] =
        static_cast<uint8_t>(context.state[index] >> 8U);
    output[index * 4U + 3U] = static_cast<uint8_t>(context.state[index]);
  }
  secureZero(&context, sizeof(context));
  return true;
}

bool KitsuBleOta::parseManifest(const uint8_t* input, size_t inputBytes,
                                ParsedManifest& output) {
  output = ParsedManifest{};
  if (!input || inputBytes == 0U ||
      inputBytes > kBleOtaManifestMaximumBytes) {
    return false;
  }
  size_t cursor = 0U;
  Span release{};
  Span version{};
  Span digest{};
  uint32_t imageBytes = 0U;
  uint32_t partitionBytes = 0U;
  uint32_t chunkBytes = 0U;
  if (!literal(input, inputBytes, cursor,
               "{\"schema\":\"kitsu.ble-firmware.v1\","
               "\"release_id\":\"") ||
      !spanUntilQuote(input, inputBytes, cursor, release) ||
      !literal(input, inputBytes, cursor, "\",\"firmware_version\":\"") ||
      !spanUntilQuote(input, inputBytes, cursor, version) ||
      !literal(input, inputBytes, cursor,
               "\",\"device_class\":\"heltec-wifi-lora-32-v3-esp32s3-8mb\","
               "\"image_format\":\"esp32s3-app\",\"image_bytes\":") ||
      !decimal(input, inputBytes, cursor, imageBytes) ||
      !literal(input, inputBytes, cursor, ",\"image_sha256\":\"") ||
      !spanUntilQuote(input, inputBytes, cursor, digest) ||
      !literal(input, inputBytes, cursor, "\",\"partition_bytes\":") ||
      !decimal(input, inputBytes, cursor, partitionBytes) ||
      !literal(input, inputBytes, cursor, ",\"chunk_bytes\":") ||
      !decimal(input, inputBytes, cursor, chunkBytes) ||
      !literal(input, inputBytes, cursor, ",\"rollback\":true}")) {
    return false;
  }
  if (cursor != inputBytes || !validReleaseId(release.data, release.bytes) ||
      !validSemver(version.data, version.bytes) ||
      !decodeLowerHex(digest, output.imageSha256,
                      sizeof(output.imageSha256)) ||
      imageBytes == 0U || imageBytes > kBleOtaMaximumImageBytes ||
      partitionBytes != kBleOtaAppPartitionBytes ||
      chunkBytes != kBleOtaChunkBytes) {
    return false;
  }
  memcpy(output.releaseId, release.data, release.bytes);
  output.releaseId[release.bytes] = '\0';
  memcpy(output.firmwareVersion, version.data, version.bytes);
  output.firmwareVersion[version.bytes] = '\0';
  output.imageBytes = imageBytes;
  return true;
}

bool KitsuBleOta::begin(BleOtaPlatform& platform,
                        const char* runningVersion) {
  if (begun_ || !validSemverString(runningVersion)) return false;
  platform_ = &platform;
  memcpy(runningVersion_, runningVersion, strlen(runningVersion) + 1U);
  if (!platform_->resolvePartitions(running_, inactive_) ||
      running_.size != kBleOtaAppPartitionBytes ||
      inactive_.size != kBleOtaAppPartitionBytes ||
      running_.address == inactive_.address ||
      !((strcmp(running_.label, "app0") == 0 &&
         running_.subtype == 0x10U &&
         strcmp(inactive_.label, "app1") == 0 &&
         inactive_.subtype == 0x11U) ||
        (strcmp(running_.label, "app1") == 0 &&
         running_.subtype == 0x11U &&
         strcmp(inactive_.label, "app0") == 0 &&
         inactive_.subtype == 0x10U))) {
    platform_ = nullptr;
    return false;
  }
  begun_ = true;
  if (!loadExistingState()) {
    // Preserve a specific terminal failure (notably an explicit rollback)
    // raised while binding a pending running image.  A plain journal I/O
    // failure has no more specific error yet.
    if (state_ != BleOtaState::Failed || !error_) {
      state_ = BleOtaState::Failed;
      error_ = "flash_read_failed";
    }
    return false;
  }
  return true;
}

bool KitsuBleOta::readJournal(
    const BleOtaPartition& partition, bool& present, uint8_t& journalState,
    uint32_t& journalOffset, uint16_t& sequence, uint16_t& nextSlot,
    uint8_t updateId[kBleOtaDigestBytes],
    uint8_t imageSha256[kBleOtaDigestBytes], uint32_t& imageBytes,
    char targetVersion[kBleOtaVersionMaximumBytes + 1U]) {
  present = false;
  journalState = 0U;
  journalOffset = 0U;
  sequence = 0U;
  nextSlot = 0U;
  memset(updateId, 0, kBleOtaDigestBytes);
  memset(imageSha256, 0, kBleOtaDigestBytes);
  imageBytes = 0U;
  memset(targetVersion, 0, kBleOtaVersionMaximumBytes + 1U);

  uint8_t header[kJournalHeaderBytes]{};
  if (!platform_->readPartition(partition, kJournalOffset, header,
                                sizeof(header))) {
    return false;
  }
  if (allBytes(header, sizeof(header), 0xffU)) return true;
  if (memcmp(header, "KOTA", 4U) != 0 ||
      header[4] != kJournalVersion || header[5] != partition.subtype ||
      header[6] != 0U || header[7] != 0U ||
      getU32Le(header + 12U) != kBleOtaChunkBytes ||
      getU32Le(header + 124U) != crc32(header, 124U)) {
    return true;
  }
  imageBytes = getU32Le(header + 8U);
  if (imageBytes == 0U || imageBytes > kBleOtaMaximumImageBytes ||
      !validSemver(header + 80U,
                   strnlen(reinterpret_cast<const char*>(header + 80U),
                           kBleOtaVersionMaximumBytes + 1U)) ||
      header[112U] != '\0') {
    return true;
  }
  memcpy(updateId, header + 16U, kBleOtaDigestBytes);
  memcpy(imageSha256, header + 48U, kBleOtaDigestBytes);
  memcpy(targetVersion, header + 80U,
         kBleOtaVersionMaximumBytes + 1U);

  bool foundCheckpoint = false;
  uint16_t lastOccupied = 0U;
  for (uint16_t slot = 0U; slot < kJournalCheckpointCapacity; ++slot) {
    uint8_t record[kJournalCheckpointBytes]{};
    const uint32_t offset = kJournalOffset + kJournalHeaderBytes +
        static_cast<uint32_t>(slot) * kJournalCheckpointBytes;
    if (!platform_->readPartition(partition, offset, record,
                                  sizeof(record))) {
      secureZero(header, sizeof(header));
      return false;
    }
    if (allBytes(record, sizeof(record), 0xffU)) continue;
    lastOccupied = static_cast<uint16_t>(slot + 1U);
    const uint8_t candidateState = record[6U];
    const uint32_t candidateOffset = getU32Le(record + 8U);
    const bool stateValid = candidateState == kJournalReceiving ||
        candidateState == kJournalReady ||
        candidateState == kJournalAborted;
    const bool offsetValid = candidateOffset <= imageBytes &&
        (candidateState != kJournalReady || candidateOffset == imageBytes) &&
        (candidateState != kJournalReceiving || candidateOffset == 0U ||
         candidateOffset % kBleOtaCheckpointBytes == 0U);
    if (memcmp(record, "KOTC", 4U) != 0 || record[7U] != 0U ||
        !stateValid || !offsetValid ||
        getU32Le(record + 12U) != crc32(record, 12U)) {
      continue;
    }
    const uint16_t candidateSequence = getU16Le(record + 4U);
    if (candidateSequence == 0U ||
        (foundCheckpoint && candidateSequence <= sequence)) {
      continue;
    }
    foundCheckpoint = true;
    sequence = candidateSequence;
    journalState = candidateState;
    journalOffset = candidateOffset;
  }
  nextSlot = lastOccupied;
  present = foundCheckpoint;
  secureZero(header, sizeof(header));
  return true;
}

bool KitsuBleOta::loadExistingState() {
  bool runningPresent = false;
  bool inactivePresent = false;
  uint8_t runningJournalState = 0U;
  uint8_t inactiveJournalState = 0U;
  uint32_t runningOffset = 0U;
  uint32_t inactiveOffset = 0U;
  uint16_t runningSequence = 0U;
  uint16_t inactiveSequence = 0U;
  uint16_t runningNextSlot = 0U;
  uint16_t inactiveNextSlot = 0U;
  uint8_t runningId[kBleOtaDigestBytes]{};
  uint8_t inactiveId[kBleOtaDigestBytes]{};
  uint8_t runningSha[kBleOtaDigestBytes]{};
  uint8_t inactiveSha[kBleOtaDigestBytes]{};
  uint32_t runningBytes = 0U;
  uint32_t inactiveBytes = 0U;
  char runningTarget[kBleOtaVersionMaximumBytes + 1U]{};
  char inactiveTarget[kBleOtaVersionMaximumBytes + 1U]{};
  const bool loaded = readJournal(
      running_, runningPresent, runningJournalState, runningOffset,
      runningSequence, runningNextSlot, runningId, runningSha, runningBytes,
      runningTarget) &&
      readJournal(inactive_, inactivePresent, inactiveJournalState,
                  inactiveOffset, inactiveSequence, inactiveNextSlot,
                  inactiveId, inactiveSha, inactiveBytes, inactiveTarget);
  if (!loaded) return false;

  const BleOtaBootState runningBoot = platform_->bootState(running_);
  // A rollback-pending application is trusted only when its own slot carries
  // the exact durable Ready journal that binds image length, SHA-256, version,
  // and update ID.  Missing/corrupt metadata must roll back before ordinary
  // application runtime; never silently expose an unbound pending image as
  // an idle updater.
  if (runningBoot == BleOtaBootState::PendingVerify &&
      (!runningPresent || runningJournalState != kJournalReady)) {
    return rollback("image_invalid");
  }
  if (runningPresent && runningJournalState == kJournalReady &&
      (runningBoot == BleOtaBootState::PendingVerify ||
       runningBoot == BleOtaBootState::Valid)) {
    memcpy(updateId_, runningId, sizeof(updateId_));
    memcpy(expectedImageSha256_, runningSha,
           sizeof(expectedImageSha256_));
    memcpy(targetVersion_, runningTarget, sizeof(targetVersion_));
    updateIdValid_ = true;
    imageBytes_ = runningBytes;
    nextOffset_ = runningOffset;
    persistedOffset_ = runningOffset;
    journalSequence_ = runningSequence;
    journalNextSlot_ = runningNextSlot;
    journalHeaderReady_ = true;
    if (runningBoot == BleOtaBootState::PendingVerify) {
      state_ = BleOtaState::PendingVerify;
      if (!bindPendingRunningImage()) return false;
    } else {
      state_ = BleOtaState::Confirmed;
    }
    return true;
  }

  if (inactivePresent && inactiveJournalState == kJournalReceiving) {
    memcpy(updateId_, inactiveId, sizeof(updateId_));
    memcpy(expectedImageSha256_, inactiveSha,
           sizeof(expectedImageSha256_));
    memcpy(targetVersion_, inactiveTarget, sizeof(targetVersion_));
    updateIdValid_ = true;
    imageBytes_ = inactiveBytes;
    nextOffset_ = inactiveOffset;
    persistedOffset_ = inactiveOffset;
    journalSequence_ = inactiveSequence;
    journalNextSlot_ = inactiveNextSlot;
    journalHeaderReady_ = true;
    state_ = BleOtaState::Receiving;
    resumed_ = true;
    error_ = "begin_required";
    // Do not erase an unbounded suffix during boot or begin.  The durable
    // checkpoint is sector-aligned; write() erases at most the one or two
    // sectors touched by its next <=4096-byte sequential chunk.
    erasedThrough_ = nextOffset_;
    return rebuildPrefixHash();
  }

  if (inactivePresent && inactiveJournalState == kJournalReady) {
    memcpy(updateId_, inactiveId, sizeof(updateId_));
    memcpy(expectedImageSha256_, inactiveSha,
           sizeof(expectedImageSha256_));
    memcpy(targetVersion_, inactiveTarget, sizeof(targetVersion_));
    updateIdValid_ = true;
    imageBytes_ = inactiveBytes;
    nextOffset_ = inactiveOffset;
    persistedOffset_ = inactiveOffset;
    journalSequence_ = inactiveSequence;
    journalNextSlot_ = inactiveNextSlot;
    journalHeaderReady_ = true;
    resumed_ = true;
    error_ = "begin_required";
    const BleOtaBootState inactiveBoot = platform_->bootState(inactive_);
    state_ = inactiveBoot == BleOtaBootState::Invalid ||
                     inactiveBoot == BleOtaBootState::Aborted
        ? BleOtaState::RolledBack
        : BleOtaState::ReadyToReboot;
    return true;
  }

  return clearForIdle();
}

bool KitsuBleOta::bindPendingRunningImage() {
  uint8_t digest[kBleOtaDigestBytes]{};
  const bool valid = strcmp(targetVersion_, runningVersion_) == 0 &&
      nextOffset_ == imageBytes_ &&
      hashPartition(running_, imageBytes_, digest) &&
      constantTimeEqual(digest, expectedImageSha256_, sizeof(digest)) &&
      platform_->verifyEspApplication(running_, imageBytes_);
  secureZero(digest, sizeof(digest));
  if (!valid) return rollback("image_invalid");
  return true;
}

bool KitsuBleOta::writeJournalHeader(
    const ParsedManifest& manifest,
    const uint8_t updateId[kBleOtaDigestBytes]) {
  uint8_t header[kJournalHeaderBytes]{};
  memcpy(header, "KOTA", 4U);
  header[4U] = kJournalVersion;
  header[5U] = inactive_.subtype;
  putU32Le(header + 8U, manifest.imageBytes);
  putU32Le(header + 12U, kBleOtaChunkBytes);
  memcpy(header + 16U, updateId, kBleOtaDigestBytes);
  memcpy(header + 48U, manifest.imageSha256, kBleOtaDigestBytes);
  memcpy(header + 80U, manifest.firmwareVersion,
         strlen(manifest.firmwareVersion) + 1U);
  putU32Le(header + 124U, crc32(header, 124U));
  const bool written = platform_->writePartition(
      inactive_, kJournalOffset, header, sizeof(header));
  const bool verified = written && platform_->readPartition(
      inactive_, kJournalOffset, ioScratch_, sizeof(header)) &&
      memcmp(header, ioScratch_, sizeof(header)) == 0;
  secureZero(header, sizeof(header));
  secureZero(ioScratch_, sizeof(header));
  return verified;
}

bool KitsuBleOta::appendCheckpoint(uint8_t journalState,
                                   uint32_t nextOffset) {
  if (!journalHeaderReady_ || journalNextSlot_ >= kJournalCheckpointCapacity ||
      journalSequence_ == UINT16_MAX || nextOffset > imageBytes_) {
    return false;
  }
  uint8_t record[kJournalCheckpointBytes]{};
  memcpy(record, "KOTC", 4U);
  const uint16_t sequence = static_cast<uint16_t>(journalSequence_ + 1U);
  putU16Le(record + 4U, sequence);
  record[6U] = journalState;
  putU32Le(record + 8U, nextOffset);
  putU32Le(record + 12U, crc32(record, 12U));
  const uint32_t offset = kJournalOffset + kJournalHeaderBytes +
      static_cast<uint32_t>(journalNextSlot_) * kJournalCheckpointBytes;
  const bool written = platform_->writePartition(
      inactive_, offset, record, sizeof(record));
  const bool verified = written && platform_->readPartition(
      inactive_, offset, ioScratch_, sizeof(record)) &&
      memcmp(record, ioScratch_, sizeof(record)) == 0;
  secureZero(record, sizeof(record));
  secureZero(ioScratch_, sizeof(record));
  if (!verified) return false;
  journalSequence_ = sequence;
  ++journalNextSlot_;
  persistedOffset_ = nextOffset;
  return true;
}

bool KitsuBleOta::prepareNewUpdate(
    const ParsedManifest& manifest,
    const uint8_t updateId[kBleOtaDigestBytes]) {
  rebootScheduled_ = false;
  rebootAt_ = 0U;
  rebootFallbackAt_ = 0U;
  if (!platform_->setBootPartition(running_)) {
    return fail("boot_select_failed", true);
  }
  // Begin is response-time bounded.  Only the private 4-KiB journal sector is
  // erased synchronously; payload sectors are erased immediately before the
  // sequential write that needs them.
  if (!platform_->erasePartition(inactive_, kJournalOffset,
                                 kBleOtaJournalBytes)) {
    return fail("flash_erase_failed", true);
  }
  journalSequence_ = 0U;
  journalNextSlot_ = 0U;
  journalHeaderReady_ = false;
  if (!writeJournalHeader(manifest, updateId)) {
    return fail("journal_failed", true);
  }
  journalHeaderReady_ = true;
  memcpy(updateId_, updateId, sizeof(updateId_));
  memcpy(expectedImageSha256_, manifest.imageSha256,
         sizeof(expectedImageSha256_));
  memcpy(targetVersion_, manifest.firmwareVersion, sizeof(targetVersion_));
  updateIdValid_ = true;
  imageBytes_ = manifest.imageBytes;
  nextOffset_ = 0U;
  persistedOffset_ = 0U;
  erasedThrough_ = 0U;
  if (!appendCheckpoint(kJournalReceiving, 0U)) {
    return fail("journal_failed", true);
  }
  shaStart(prefixHash_);
  prefixHashReady_ = true;
  manifestLoaded_ = true;
  resumed_ = false;
  state_ = BleOtaState::Receiving;
  error_ = nullptr;
  return true;
}

bool KitsuBleOta::bindResumedManifest(
    const ParsedManifest& manifest,
    const uint8_t updateId[kBleOtaDigestBytes]) {
  if (!updateIdValid_ ||
      !constantTimeEqual(updateId_, updateId, sizeof(updateId_)) ||
      manifest.imageBytes != imageBytes_ ||
      !constantTimeEqual(manifest.imageSha256, expectedImageSha256_,
                         sizeof(expectedImageSha256_)) ||
      strcmp(manifest.firmwareVersion, targetVersion_) != 0) {
    return false;
  }
  manifestLoaded_ = true;
  resumed_ = true;
  error_ = nullptr;
  if (state_ == BleOtaState::Receiving && !prefixHashReady_) {
    return rebuildPrefixHash();
  }
  return true;
}

bool KitsuBleOta::rebuildPrefixHash() {
  shaStart(prefixHash_);
  uint32_t offset = 0U;
  while (offset < nextOffset_) {
    const size_t bytes = nextOffset_ - offset < sizeof(chunkScratch_)
        ? static_cast<size_t>(nextOffset_ - offset)
        : sizeof(chunkScratch_);
    if (!platform_->readPartition(inactive_, offset, chunkScratch_, bytes)) {
      secureZero(&prefixHash_, sizeof(prefixHash_));
      prefixHashReady_ = false;
      return false;
    }
    shaUpdate(prefixHash_, chunkScratch_, bytes);
    offset += static_cast<uint32_t>(bytes);
  }
  prefixHashReady_ = true;
  return true;
}

bool KitsuBleOta::hashPartition(
    const BleOtaPartition& partition, uint32_t bytes,
    uint8_t output[kBleOtaDigestBytes]) {
  Sha256Context context{};
  shaStart(context);
  uint32_t offset = 0U;
  while (offset < bytes) {
    const size_t readBytes = bytes - offset < sizeof(chunkScratch_)
        ? static_cast<size_t>(bytes - offset)
        : sizeof(chunkScratch_);
    if (!platform_->readPartition(partition, offset, chunkScratch_,
                                  readBytes)) {
      secureZero(&context, sizeof(context));
      memset(output, 0, kBleOtaDigestBytes);
      return false;
    }
    shaUpdate(context, chunkScratch_, readBytes);
    offset += static_cast<uint32_t>(readBytes);
  }
  return shaFinish(context, output);
}

bool KitsuBleOta::comparePartition(uint32_t offset,
                                   const uint8_t* expected,
                                   size_t expectedBytes) {
  size_t compared = 0U;
  while (compared < expectedBytes) {
    const size_t bytes = expectedBytes - compared < sizeof(ioScratch_)
        ? expectedBytes - compared
        : sizeof(ioScratch_);
    if (!platform_->readPartition(inactive_,
                                  offset + static_cast<uint32_t>(compared),
                                  ioScratch_, bytes) ||
        memcmp(ioScratch_, expected + compared, bytes) != 0) {
      return false;
    }
    compared += bytes;
  }
  return true;
}

bool KitsuBleOta::clearForIdle() {
  state_ = BleOtaState::Idle;
  manifestLoaded_ = false;
  resumed_ = false;
  updateIdValid_ = false;
  rebootScheduled_ = false;
  confirmationArmed_ = false;
  prefixHashReady_ = false;
  journalHeaderReady_ = false;
  imageBytes_ = 0U;
  nextOffset_ = 0U;
  persistedOffset_ = 0U;
  erasedThrough_ = 0U;
  journalSequence_ = 0U;
  journalNextSlot_ = 0U;
  healthySince_ = 0U;
  rebootAt_ = 0U;
  rebootFallbackAt_ = 0U;
  error_ = nullptr;
  secureZero(updateId_, sizeof(updateId_));
  secureZero(expectedImageSha256_, sizeof(expectedImageSha256_));
  memset(targetVersion_, 0, sizeof(targetVersion_));
  secureZero(&prefixHash_, sizeof(prefixHash_));
  return true;
}

bool KitsuBleOta::fail(const char* error, bool terminal) {
  error_ = error ? error : "internal_error";
  if (terminal) state_ = BleOtaState::Failed;
  return false;
}

bool KitsuBleOta::rollback(const char* error) {
  state_ = BleOtaState::Failed;
  error_ = error ? error : "rollback_failed";
  confirmationArmed_ = false;
  if (!platform_ || !platform_->rollbackRunningAndRestart()) {
    error_ = "rollback_failed";
  }
  return false;
}

BleOtaStatus KitsuBleOta::status() const {
  BleOtaStatus output{};
  output.state = state_;
  output.begun = begun_;
  output.updateIdValid = updateIdValid_;
  output.resumed = resumed_;
  output.rebootScheduled = rebootScheduled_;
  memcpy(output.updateId, updateId_, sizeof(output.updateId));
  output.imageBytes = imageBytes_;
  output.nextOffset = nextOffset_;
  output.runningVersion = runningVersion_;
  output.error = error_;
  return output;
}

bool KitsuBleOta::encodeReceipt(bool ok, bool replayed, bool scheduled,
                                uint8_t* response,
                                size_t responseCapacity,
                                size_t& responseBytes) const {
  responseBytes = 0U;
  if (!response || responseCapacity == 0U) return false;
  char id[65]{};
  char idToken[68]{};
  if (updateIdValid_) {
    encodeLowerHex(updateId_, sizeof(updateId_), id);
    idToken[0] = '"';
    memcpy(idToken + 1U, id, 64U);
    idToken[65U] = '"';
    idToken[66U] = '\0';
  } else {
    memcpy(idToken, "null", 5U);
  }
  const char* receiptError = ok
      ? (state_ == BleOtaState::Failed ? error_ : nullptr)
      : (error_ ? error_ : "internal_error");
  char errorToken[64]{};
  if (receiptError) {
    const size_t errorBytes = strlen(receiptError);
    if (errorBytes > sizeof(errorToken) - 3U) return false;
    errorToken[0] = '"';
    memcpy(errorToken + 1U, receiptError, errorBytes);
    errorToken[errorBytes + 1U] = '"';
    errorToken[errorBytes + 2U] = '\0';
  } else {
    memcpy(errorToken, "null", 5U);
  }
  const int written = snprintf(
      reinterpret_cast<char*>(response), responseCapacity,
      "{\"ok\":%s,\"protocol\":1,\"state\":\"%s\","
      "\"update_id\":%s,\"firmware_version\":\"%s\","
      "\"image_bytes\":%lu,\"next_offset\":%lu,\"chunk_bytes\":4096,"
      "\"resumed\":%s,\"replayed\":%s,\"scheduled\":%s,\"error\":%s}",
      ok ? "true" : "false", stateName(state_), idToken, runningVersion_,
      static_cast<unsigned long>(imageBytes_),
      static_cast<unsigned long>(nextOffset_), resumed_ ? "true" : "false",
      replayed ? "true" : "false", scheduled ? "true" : "false",
      errorToken);
  secureZero(id, sizeof(id));
  if (written <= 0 || static_cast<size_t>(written) >= responseCapacity) {
    if (responseCapacity != 0U) response[0] = 0U;
    return false;
  }
  responseBytes = static_cast<size_t>(written);
  return true;
}

bool KitsuBleOta::handleRequest(
    const char* operation, const uint8_t* payload, size_t payloadBytes,
    uint8_t* response, size_t responseCapacity, size_t& responseBytes) {
  responseBytes = 0U;
  if (!begun_ || !operation) {
    fail("not_begun");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (strcmp(operation, "firmware.update.status") == 0) {
    return handleStatus(payload, payloadBytes, response, responseCapacity,
                        responseBytes);
  }
  if (strcmp(operation, "firmware.update.begin") == 0) {
    return handleBegin(payload, payloadBytes, response, responseCapacity,
                       responseBytes);
  }
  if (strcmp(operation, "firmware.update.write") == 0) {
    return handleWrite(payload, payloadBytes, response, responseCapacity,
                       responseBytes);
  }
  if (strcmp(operation, "firmware.update.finish") == 0) {
    return handleFinish(payload, payloadBytes, response, responseCapacity,
                        responseBytes);
  }
  if (strcmp(operation, "firmware.update.reboot") == 0) {
    return handleReboot(payload, payloadBytes, response, responseCapacity,
                        responseBytes);
  }
  if (strcmp(operation, "firmware.update.abort") == 0) {
    return handleAbort(payload, payloadBytes, response, responseCapacity,
                       responseBytes);
  }
  fail("malformed_request");
  return encodeReceipt(false, false, false, response, responseCapacity,
                       responseBytes);
}

bool KitsuBleOta::handleStatus(
    const uint8_t* payload, size_t payloadBytes, uint8_t* response,
    size_t responseCapacity, size_t& responseBytes) {
  if (!payload || payloadBytes != 2U || memcmp(payload, "{}", 2U) != 0) {
    fail("malformed_request");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (state_ != BleOtaState::Failed) error_ = nullptr;
  return encodeReceipt(true, false, rebootScheduled_, response,
                       responseCapacity, responseBytes);
}

bool KitsuBleOta::handleBegin(
    const uint8_t* payload, size_t payloadBytes, uint8_t* response,
    size_t responseCapacity, size_t& responseBytes) {
  Span manifestEncoded{};
  Span signatureEncoded{};
  uint8_t signature[kBleOtaSignatureBytes]{};
  size_t manifestBytes = 0U;
  size_t signatureBytes = 0U;
  ParsedManifest manifest{};
  uint8_t updateId[kBleOtaDigestBytes]{};
  bool ok = parseBeginRequest(payload, payloadBytes, manifestEncoded,
                              signatureEncoded) &&
      decodeBase64Url(manifestEncoded, manifestScratch_,
                      sizeof(manifestScratch_), manifestBytes) &&
      decodeBase64Url(signatureEncoded, signature, sizeof(signature),
                      signatureBytes) &&
      signatureBytes == sizeof(signature) &&
      parseManifest(manifestScratch_, manifestBytes, manifest);
  if (!ok) {
    secureZero(signature, sizeof(signature));
    secureZero(&manifest, sizeof(manifest));
    fail("malformed_request");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (!platform_->verifyEd25519(kUpdateAuthority, manifestScratch_,
                                manifestBytes, signature)) {
    secureZero(signature, sizeof(signature));
    secureZero(&manifest, sizeof(manifest));
    fail("invalid_signature");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  Sha256Context digestContext{};
  shaStart(digestContext);
  shaUpdate(digestContext, manifestScratch_, manifestBytes);
  ok = shaFinish(digestContext, updateId);
  secureZero(signature, sizeof(signature));
  if (!ok) {
    secureZero(&manifest, sizeof(manifest));
    fail("internal_error");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }

  bool accepted = false;
  if (state_ == BleOtaState::Idle || state_ == BleOtaState::Confirmed ||
      state_ == BleOtaState::RolledBack) {
    accepted = prepareNewUpdate(manifest, updateId);
  } else if (state_ == BleOtaState::Receiving ||
             state_ == BleOtaState::ReadyToReboot) {
    if (!updateIdValid_ ||
        !constantTimeEqual(updateId_, updateId, sizeof(updateId_))) {
      fail("update_conflict");
    } else if (!bindResumedManifest(manifest, updateId)) {
      fail("update_conflict");
    } else {
      accepted = true;
    }
  } else if (state_ == BleOtaState::Failed && updateIdValid_ &&
             !constantTimeEqual(updateId_, updateId, sizeof(updateId_))) {
    fail("update_conflict");
  } else {
    fail("invalid_state");
  }
  secureZero(updateId, sizeof(updateId));
  secureZero(&manifest, sizeof(manifest));
  return encodeReceipt(accepted, false, false, response, responseCapacity,
                       responseBytes);
}

bool KitsuBleOta::handleWrite(
    const uint8_t* payload, size_t payloadBytes, uint8_t* response,
    size_t responseCapacity, size_t& responseBytes) {
  uint8_t requestId[kBleOtaDigestBytes]{};
  uint32_t offset = 0U;
  Span encodedData{};
  size_t chunkBytes = 0U;
  if (!parseWriteRequest(payload, payloadBytes, requestId, offset,
                         encodedData) ||
      !decodeBase64Url(encodedData, chunkScratch_, sizeof(chunkScratch_),
                       chunkBytes)) {
    secureZero(requestId, sizeof(requestId));
    fail("malformed_request");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (state_ != BleOtaState::Receiving) {
    secureZero(requestId, sizeof(requestId));
    fail("invalid_state");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (!manifestLoaded_) {
    secureZero(requestId, sizeof(requestId));
    fail("begin_required");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (!constantTimeEqual(requestId, updateId_, sizeof(requestId))) {
    secureZero(requestId, sizeof(requestId));
    fail("invalid_update_id");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  secureZero(requestId, sizeof(requestId));
  const bool rangeValid = chunkBytes != 0U &&
      chunkBytes <= kBleOtaChunkBytes && offset <= imageBytes_ &&
      chunkBytes <= imageBytes_ - offset;
  if (!rangeValid) {
    fail(chunkBytes == 0U || chunkBytes > kBleOtaChunkBytes
             ? "chunk_size_invalid"
             : "offset_invalid");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (offset < nextOffset_) {
    if (offset + chunkBytes > nextOffset_ ||
        !comparePartition(offset, chunkScratch_, chunkBytes)) {
      fail("chunk_mismatch");
      return encodeReceipt(false, false, false, response, responseCapacity,
                           responseBytes);
    }
    error_ = nullptr;
    return encodeReceipt(true, true, false, response, responseCapacity,
                         responseBytes);
  }
  if (offset != nextOffset_) {
    fail("offset_invalid");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (!prefixHashReady_) {
    fail("internal_error", true);
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  const uint32_t chunkEnd = offset + static_cast<uint32_t>(chunkBytes);
  if (chunkEnd > erasedThrough_) {
    const uint32_t eraseEnd =
        (chunkEnd + kBleOtaJournalBytes - 1U) &
        ~(kBleOtaJournalBytes - 1U);
    if (eraseEnd > kBleOtaMaximumImageBytes ||
        erasedThrough_ % kBleOtaJournalBytes != 0U ||
        !platform_->erasePartition(inactive_, erasedThrough_,
                                   eraseEnd - erasedThrough_)) {
      fail("flash_erase_failed", true);
      return encodeReceipt(false, false, false, response, responseCapacity,
                           responseBytes);
    }
    erasedThrough_ = eraseEnd;
  }
  if (!platform_->writePartition(inactive_, offset, chunkScratch_,
                                 chunkBytes)) {
    fail("flash_write_failed", true);
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (!comparePartition(offset, chunkScratch_, chunkBytes)) {
    fail("flash_read_failed", true);
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  shaUpdate(prefixHash_, chunkScratch_, chunkBytes);
  nextOffset_ += static_cast<uint32_t>(chunkBytes);
  const uint32_t durableOffset = nextOffset_ -
      nextOffset_ % kBleOtaCheckpointBytes;
  if (durableOffset > persistedOffset_ &&
      !appendCheckpoint(kJournalReceiving, durableOffset)) {
    fail("journal_failed", true);
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  error_ = nullptr;
  return encodeReceipt(true, false, false, response, responseCapacity,
                       responseBytes);
}

bool KitsuBleOta::handleFinish(
    const uint8_t* payload, size_t payloadBytes, uint8_t* response,
    size_t responseCapacity, size_t& responseBytes) {
  uint8_t requestId[kBleOtaDigestBytes]{};
  if (!parseIdRequest(payload, payloadBytes, requestId)) {
    fail("malformed_request");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (!updateIdValid_ ||
      !constantTimeEqual(requestId, updateId_, sizeof(requestId))) {
    secureZero(requestId, sizeof(requestId));
    fail("invalid_update_id");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  secureZero(requestId, sizeof(requestId));
  if (!manifestLoaded_) {
    fail("begin_required");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (state_ == BleOtaState::ReadyToReboot) {
    const bool selected = platform_->setBootPartition(inactive_);
    error_ = selected ? nullptr : "boot_select_failed";
    return encodeReceipt(selected, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (state_ != BleOtaState::Receiving) {
    fail("invalid_state");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (nextOffset_ != imageBytes_) {
    fail("incomplete_image");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  uint8_t streamed[kBleOtaDigestBytes]{};
  uint8_t readback[kBleOtaDigestBytes]{};
  const bool streamedOk = prefixHashReady_ &&
      shaFinish(prefixHash_, streamed) &&
      constantTimeEqual(streamed, expectedImageSha256_, sizeof(streamed));
  prefixHashReady_ = false;
  const bool readbackOk = streamedOk &&
      hashPartition(inactive_, imageBytes_, readback) &&
      constantTimeEqual(readback, expectedImageSha256_, sizeof(readback));
  secureZero(streamed, sizeof(streamed));
  secureZero(readback, sizeof(readback));
  if (!streamedOk || !readbackOk) {
    fail("image_hash_mismatch", true);
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (!platform_->verifyEspApplication(inactive_, imageBytes_)) {
    fail("image_invalid", true);
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (!appendCheckpoint(kJournalReady, imageBytes_)) {
    fail("journal_failed", true);
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  state_ = BleOtaState::ReadyToReboot;
  const bool selected = platform_->setBootPartition(inactive_);
  error_ = selected ? nullptr : "boot_select_failed";
  return encodeReceipt(selected, false, false, response, responseCapacity,
                       responseBytes);
}

bool KitsuBleOta::handleReboot(
    const uint8_t* payload, size_t payloadBytes, uint8_t* response,
    size_t responseCapacity, size_t& responseBytes) {
  uint8_t requestId[kBleOtaDigestBytes]{};
  if (!parseIdRequest(payload, payloadBytes, requestId)) {
    fail("malformed_request");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  const bool idMatches = updateIdValid_ &&
      constantTimeEqual(requestId, updateId_, sizeof(requestId));
  secureZero(requestId, sizeof(requestId));
  if (!idMatches) {
    fail("invalid_update_id");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (state_ != BleOtaState::ReadyToReboot) {
    fail("reboot_not_ready");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (!platform_->setBootPartition(inactive_)) {
    fail("boot_select_failed");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  rebootScheduled_ = true;
  // The absolute deadline is armed by loop() on the first iteration after
  // the authenticated response has been handed to the GATT TX queue.
  rebootAt_ = 0U;
  error_ = nullptr;
  return encodeReceipt(true, false, true, response, responseCapacity,
                       responseBytes);
}

bool KitsuBleOta::handleAbort(
    const uint8_t* payload, size_t payloadBytes, uint8_t* response,
    size_t responseCapacity, size_t& responseBytes) {
  uint8_t requestId[kBleOtaDigestBytes]{};
  if (!parseIdRequest(payload, payloadBytes, requestId)) {
    fail("malformed_request");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (state_ == BleOtaState::PendingVerify ||
      state_ == BleOtaState::Confirmed) {
    secureZero(requestId, sizeof(requestId));
    fail("invalid_state");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (updateIdValid_ &&
      !constantTimeEqual(requestId, updateId_, sizeof(requestId))) {
    secureZero(requestId, sizeof(requestId));
    fail("invalid_update_id");
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  secureZero(requestId, sizeof(requestId));
  rebootScheduled_ = false;
  rebootAt_ = 0U;
  rebootFallbackAt_ = 0U;
  if (!platform_->setBootPartition(running_)) {
    fail("boot_select_failed", true);
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  if (journalHeaderReady_ &&
      !appendCheckpoint(kJournalAborted, nextOffset_) &&
      !platform_->erasePartition(inactive_, kJournalOffset,
                                 kBleOtaJournalBytes)) {
    fail("journal_failed", true);
    return encodeReceipt(false, false, false, response, responseCapacity,
                         responseBytes);
  }
  clearForIdle();
  return encodeReceipt(true, false, false, response, responseCapacity,
                       responseBytes);
}

bool KitsuBleOta::finishCriticalInitialization(bool healthy,
                                                uint32_t nowMillis) {
  if (!begun_) return false;
  if (state_ != BleOtaState::PendingVerify) return healthy;
  if (!healthy) return rollback("rollback_failed");
  confirmationArmed_ = true;
  healthySince_ = nowMillis;
  return true;
}

void KitsuBleOta::loop(uint32_t nowMillis, bool criticalHealth,
                       bool transmitIdle) {
  if (!begun_) return;
  if (rebootScheduled_) {
    if (rebootAt_ == 0U) {
      rebootAt_ = nowMillis + kBleOtaRebootDelayMs;
      rebootFallbackAt_ = nowMillis + kBleOtaRebootFallbackMs;
    }
    if (deadlineReached(nowMillis, rebootAt_) &&
        (transmitIdle || deadlineReached(nowMillis, rebootFallbackAt_))) {
      rebootScheduled_ = false;
      platform_->restart();
      return;
    }
  }
  if (state_ != BleOtaState::PendingVerify || !confirmationArmed_) return;
  if (!criticalHealth) {
    rollback("rollback_failed");
    return;
  }
  if (static_cast<uint32_t>(nowMillis - healthySince_) <
      kBleOtaHealthyConfirmationMs) {
    return;
  }
  if (!platform_->markRunningValid()) {
    rollback("rollback_failed");
    return;
  }
  state_ = BleOtaState::Confirmed;
  confirmationArmed_ = false;
  error_ = nullptr;
}

#if defined(ARDUINO_ARCH_ESP32)

const void* Esp32KitsuBleOtaPlatform::native(
    const BleOtaPartition& partition) const {
  const esp_partition_t* running =
      static_cast<const esp_partition_t*>(runningNative_);
  const esp_partition_t* inactive =
      static_cast<const esp_partition_t*>(inactiveNative_);
  if (running && partition.address == running->address &&
      partition.size == running->size &&
      partition.subtype == running->subtype) {
    return running;
  }
  if (inactive && partition.address == inactive->address &&
      partition.size == inactive->size &&
      partition.subtype == inactive->subtype) {
    return inactive;
  }
  return nullptr;
}

bool Esp32KitsuBleOtaPlatform::resolvePartitions(
    BleOtaPartition& running, BleOtaPartition& inactive) {
  const esp_partition_t* runningPart = esp_ota_get_running_partition();
  const esp_partition_t* inactivePart =
      esp_ota_get_next_update_partition(runningPart);
  if (!runningPart || !inactivePart || runningPart == inactivePart ||
      runningPart->type != ESP_PARTITION_TYPE_APP ||
      inactivePart->type != ESP_PARTITION_TYPE_APP ||
      runningPart->size != kBleOtaAppPartitionBytes ||
      inactivePart->size != kBleOtaAppPartitionBytes ||
      !((strcmp(runningPart->label, "app0") == 0 &&
         runningPart->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
         strcmp(inactivePart->label, "app1") == 0 &&
         inactivePart->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) ||
        (strcmp(runningPart->label, "app1") == 0 &&
         runningPart->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 &&
         strcmp(inactivePart->label, "app0") == 0 &&
         inactivePart->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0))) {
    return false;
  }
  running = BleOtaPartition{};
  inactive = BleOtaPartition{};
  memcpy(running.label, runningPart->label, sizeof(running.label));
  memcpy(inactive.label, inactivePart->label, sizeof(inactive.label));
  running.label[sizeof(running.label) - 1U] = '\0';
  inactive.label[sizeof(inactive.label) - 1U] = '\0';
  running.address = runningPart->address;
  running.size = runningPart->size;
  running.subtype = runningPart->subtype;
  inactive.address = inactivePart->address;
  inactive.size = inactivePart->size;
  inactive.subtype = inactivePart->subtype;
  runningNative_ = runningPart;
  inactiveNative_ = inactivePart;
  return true;
}

bool Esp32KitsuBleOtaPlatform::readPartition(
    const BleOtaPartition& partition, uint32_t offset, uint8_t* output,
    size_t outputBytes) {
  const esp_partition_t* part =
      static_cast<const esp_partition_t*>(native(partition));
  return part && output && offset <= part->size &&
      outputBytes <= part->size - offset &&
      esp_partition_read(part, offset, output, outputBytes) == ESP_OK;
}

bool Esp32KitsuBleOtaPlatform::erasePartition(
    const BleOtaPartition& partition, uint32_t offset,
    uint32_t eraseBytes) {
  const esp_partition_t* part =
      static_cast<const esp_partition_t*>(native(partition));
  return part && offset % kBleOtaJournalBytes == 0U &&
      eraseBytes % kBleOtaJournalBytes == 0U &&
      offset <= part->size && eraseBytes <= part->size - offset &&
      esp_partition_erase_range(part, offset, eraseBytes) == ESP_OK;
}

bool Esp32KitsuBleOtaPlatform::writePartition(
    const BleOtaPartition& partition, uint32_t offset,
    const uint8_t* input, size_t inputBytes) {
  const esp_partition_t* part =
      static_cast<const esp_partition_t*>(native(partition));
  return part && input && inputBytes != 0U && offset <= part->size &&
      inputBytes <= part->size - offset &&
      esp_partition_write(part, offset, input, inputBytes) == ESP_OK;
}

bool Esp32KitsuBleOtaPlatform::verifyEspApplication(
    const BleOtaPartition& partition, uint32_t imageBytes) {
  const esp_partition_t* part =
      static_cast<const esp_partition_t*>(native(partition));
  if (!part || imageBytes == 0U || imageBytes > kBleOtaMaximumImageBytes) {
    return false;
  }
  esp_partition_pos_t position{};
  position.offset = part->address;
  position.size = imageBytes;
  esp_image_metadata_t metadata{};
  return esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata) == ESP_OK &&
      metadata.image_len == imageBytes;
}

bool Esp32KitsuBleOtaPlatform::verifyEd25519(
    const uint8_t publicKey[kBleOtaDigestBytes], const uint8_t* message,
    size_t messageBytes,
    const uint8_t signature[kBleOtaSignatureBytes]) {
  return publicKey && message && messageBytes != 0U && signature &&
      ed25519_verify(signature, message, messageBytes, publicKey) == 1;
}

BleOtaBootState Esp32KitsuBleOtaPlatform::bootState(
    const BleOtaPartition& partition) {
  const esp_partition_t* part =
      static_cast<const esp_partition_t*>(native(partition));
  if (!part) return BleOtaBootState::Unknown;
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (esp_ota_get_state_partition(part, &state) != ESP_OK) {
    return BleOtaBootState::Unknown;
  }
  switch (state) {
    case ESP_OTA_IMG_NEW: return BleOtaBootState::New;
    case ESP_OTA_IMG_PENDING_VERIFY: return BleOtaBootState::PendingVerify;
    case ESP_OTA_IMG_VALID: return BleOtaBootState::Valid;
    case ESP_OTA_IMG_INVALID: return BleOtaBootState::Invalid;
    case ESP_OTA_IMG_ABORTED: return BleOtaBootState::Aborted;
    case ESP_OTA_IMG_UNDEFINED:
    default: return BleOtaBootState::Unknown;
  }
}

bool Esp32KitsuBleOtaPlatform::setBootPartition(
    const BleOtaPartition& partition) {
  const esp_partition_t* part =
      static_cast<const esp_partition_t*>(native(partition));
  return part && esp_ota_set_boot_partition(part) == ESP_OK;
}

bool Esp32KitsuBleOtaPlatform::markRunningValid() {
  return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}

bool Esp32KitsuBleOtaPlatform::rollbackRunningAndRestart() {
  return esp_ota_mark_app_invalid_rollback_and_reboot() == ESP_OK;
}

void Esp32KitsuBleOtaPlatform::restart() { esp_restart(); }

#endif

}  // namespace connectivity
}  // namespace kitsu868

#if defined(ARDUINO_ARCH_ESP32)
// Arduino-ESP32 otherwise weakly marks a pending image valid inside
// initArduino(), before Kitsu storage, BLE, or its 30-second health window has
// run.  This strong hook retains ESP-IDF bootloader rollback until Kitsu calls
// esp_ota_mark_app_valid_cancel_rollback() itself.
extern "C" bool verifyRollbackLater() { return true; }
#endif
