#include "kitsu_mesh_messages.h"

#include <string.h>

namespace kitsu868 {
namespace mesh {
namespace {

uint32_t readLe32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) |
      static_cast<uint32_t>(bytes[1]) << 8U |
      static_cast<uint32_t>(bytes[2]) << 16U |
      static_cast<uint32_t>(bytes[3]) << 24U;
}

void writeLe32(uint8_t* bytes, uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8U);
  bytes[2] = static_cast<uint8_t>(value >> 16U);
  bytes[3] = static_cast<uint8_t>(value >> 24U);
}

TextCodecStatus decodePlain(const uint8_t* input, size_t inputBytes,
                            DecodedTextPayload& output) {
  if (!input) return TextCodecStatus::NullArgument;
  if (inputBytes <= 5U) return TextCodecStatus::PayloadTooShort;
  if ((input[4] >> 2U) != 0U) {
    return TextCodecStatus::UnsupportedTextType;
  }

  size_t textBytes = 0;
  while (5U + textBytes < inputBytes && input[5U + textBytes] != 0U) {
    ++textBytes;
  }
  // Empty MeshCore text packets carry no useful companion/app content. Reject
  // them at the RF boundary so every emitted phone record remains actionable.
  if (textBytes == 0U) return TextCodecStatus::EmptyText;
  if (textBytes > kMeshTextBytes) return TextCodecStatus::TextTooLong;
  if (!validMeshTextUtf8(reinterpret_cast<const char*>(input + 5U),
                         textBytes)) {
    return TextCodecStatus::InvalidUtf8;
  }

  DecodedTextPayload decoded{};
  decoded.timestamp = readLe32(input);
  decoded.attempt = input[4] & 0x03U;
  const size_t terminator = 5U + textBytes;
  if (terminator + 1U < inputBytes && input[terminator] == 0U &&
      input[terminator + 1U] > 3U) {
    decoded.attempt = input[terminator + 1U];
  }
  memcpy(decoded.text, input + 5U, textBytes);
  decoded.text[textBytes] = '\0';
  decoded.textBytes = textBytes;
  output = decoded;
  return TextCodecStatus::Ok;
}

}  // namespace

const char* textCodecStatusName(TextCodecStatus status) {
  switch (status) {
    case TextCodecStatus::Ok: return "ok";
    case TextCodecStatus::NullArgument: return "null_argument";
    case TextCodecStatus::EmptyText: return "empty_text";
    case TextCodecStatus::TextTooLong: return "text_too_long";
    case TextCodecStatus::OutputTooSmall: return "output_too_small";
    case TextCodecStatus::PayloadTooShort: return "payload_too_short";
    case TextCodecStatus::UnsupportedTextType:
      return "unsupported_text_type";
    case TextCodecStatus::InvalidUtf8: return "invalid_utf8";
  }
  return "unknown";
}

bool validMeshTextUtf8(const char* text, size_t textBytes) {
  if (!text && textBytes != 0U) return false;
  size_t offset = 0;
  while (offset < textBytes) {
    const uint8_t first = static_cast<uint8_t>(text[offset]);
    size_t sequenceBytes = 1;
    uint32_t codePoint = first;
    if (first < 0x80U) {
      if (first < 0x20U || first == 0x7fU) return false;
    } else if (first >= 0xc2U && first <= 0xdfU) {
      sequenceBytes = 2;
    } else if (first >= 0xe0U && first <= 0xefU) {
      sequenceBytes = 3;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      sequenceBytes = 4;
    } else {
      return false;
    }
    if (offset + sequenceBytes > textBytes) return false;
    for (size_t index = 1; index < sequenceBytes; ++index) {
      if ((static_cast<uint8_t>(text[offset + index]) & 0xc0U) != 0x80U) {
        return false;
      }
    }
    if (sequenceBytes == 2U) {
      codePoint = ((first & 0x1fU) << 6U) |
          (static_cast<uint8_t>(text[offset + 1U]) & 0x3fU);
    } else if (sequenceBytes == 3U) {
      codePoint = ((first & 0x0fU) << 12U) |
          ((static_cast<uint8_t>(text[offset + 1U]) & 0x3fU) << 6U) |
          (static_cast<uint8_t>(text[offset + 2U]) & 0x3fU);
      if (codePoint < 0x800U ||
          (codePoint >= 0xd800U && codePoint <= 0xdfffU)) {
        return false;
      }
    } else if (sequenceBytes == 4U) {
      codePoint = ((first & 0x07U) << 18U) |
          ((static_cast<uint8_t>(text[offset + 1U]) & 0x3fU) << 12U) |
          ((static_cast<uint8_t>(text[offset + 2U]) & 0x3fU) << 6U) |
          (static_cast<uint8_t>(text[offset + 3U]) & 0x3fU);
      if (codePoint < 0x10000U || codePoint > 0x10ffffU) return false;
    }
    if (codePoint >= 0x80U && codePoint <= 0x9fU) return false;
    offset += sequenceBytes;
  }
  return true;
}

TextCodecStatus encodeDirectTextPayload(uint32_t timestamp, uint8_t attempt,
                                        const char* text, uint8_t* output,
                                        size_t outputCapacity,
                                        size_t& outputBytes) {
  outputBytes = 0;
  if (!text || !output) return TextCodecStatus::NullArgument;
  const size_t textBytes = strnlen(text, kMeshTextCapacity);
  if (textBytes == 0U) return TextCodecStatus::EmptyText;
  if (textBytes > kMeshTextBytes ||
      (attempt > 3U && textBytes > kMeshTextBytes - 2U)) {
    return TextCodecStatus::TextTooLong;
  }
  if (!validMeshTextUtf8(text, textBytes)) {
    return TextCodecStatus::InvalidUtf8;
  }
  const size_t needed = 5U + textBytes + (attempt > 3U ? 2U : 0U);
  if (outputCapacity < needed) return TextCodecStatus::OutputTooSmall;
  writeLe32(output, timestamp);
  output[4] = attempt & 0x03U;
  memcpy(output + 5U, text, textBytes);
  if (attempt > 3U) {
    output[5U + textBytes] = 0;
    output[6U + textBytes] = attempt;
  }
  outputBytes = needed;
  return TextCodecStatus::Ok;
}

TextCodecStatus decodeDirectTextPayload(const uint8_t* input,
                                        size_t inputBytes,
                                        DecodedTextPayload& output) {
  return decodePlain(input, inputBytes, output);
}

TextCodecStatus encodeChannelTextPayload(uint32_t timestamp,
                                         const char* senderName,
                                         const char* text,
                                         size_t textBytes,
                                         uint8_t* output,
                                         size_t outputCapacity,
                                         size_t& outputBytes) {
  outputBytes = 0;
  if (!senderName || !text || !output) {
    return TextCodecStatus::NullArgument;
  }
  if (textBytes == 0U) return TextCodecStatus::EmptyText;
  const size_t senderBytes = strnlen(senderName, 33U);
  if (senderBytes == 0U || senderBytes >= 33U ||
      !validMeshTextUtf8(senderName, senderBytes) ||
      !validMeshTextUtf8(text, textBytes)) {
    return TextCodecStatus::InvalidUtf8;
  }
  const size_t combinedBytes = senderBytes + 2U + textBytes;
  if (combinedBytes > kMeshTextBytes) return TextCodecStatus::TextTooLong;
  const size_t needed = 5U + combinedBytes;
  if (outputCapacity < needed) return TextCodecStatus::OutputTooSmall;
  writeLe32(output, timestamp);
  output[4] = 0;
  memcpy(output + 5U, senderName, senderBytes);
  output[5U + senderBytes] = ':';
  output[6U + senderBytes] = ' ';
  memcpy(output + 7U + senderBytes, text, textBytes);
  outputBytes = needed;
  return TextCodecStatus::Ok;
}

TextCodecStatus decodeChannelTextPayload(const uint8_t* input,
                                         size_t inputBytes,
                                         DecodedTextPayload& output) {
  DecodedTextPayload decoded{};
  const TextCodecStatus status = decodePlain(input, inputBytes, decoded);
  if (status != TextCodecStatus::Ok) return status;
  const char* separator = strstr(decoded.text, ": ");
  if (separator && separator[2] == '\0') {
    return TextCodecStatus::EmptyText;
  }
  output = decoded;
  return TextCodecStatus::Ok;
}

}  // namespace mesh
}  // namespace kitsu868
