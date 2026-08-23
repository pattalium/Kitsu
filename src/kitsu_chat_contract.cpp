#include "kitsu_chat_contract.h"

#include <string.h>

namespace kitsu868 {
namespace chat {
namespace {

struct Token {
  const char* data = nullptr;
  size_t length = 0;
};

struct Cursor {
  const char* data;
  size_t length;
  size_t offset;
};

bool asciiSpace(char value) { return value == ' '; }

char asciiLower(char value) {
  if (value >= 'A' && value <= 'Z') {
    return static_cast<char>(value + ('a' - 'A'));
  }
  return value;
}

bool tokenEquals(const Token& token, const char* expected) {
  const size_t expectedLength = strlen(expected);
  if (token.length != expectedLength) return false;
  for (size_t i = 0; i < token.length; ++i) {
    if (asciiLower(token.data[i]) != expected[i]) return false;
  }
  return true;
}

bool tokenEqualsExact(const Token& token, const char* expected) {
  const size_t expectedLength = strlen(expected);
  return token.length == expectedLength &&
      memcmp(token.data, expected, expectedLength) == 0;
}

bool tokenStartsWithExact(const Token& token, const char* prefix) {
  const size_t prefixLength = strlen(prefix);
  return token.length >= prefixLength &&
      memcmp(token.data, prefix, prefixLength) == 0;
}

bool nextToken(Cursor& cursor, Token& token) {
  while (cursor.offset < cursor.length &&
         asciiSpace(cursor.data[cursor.offset])) {
    ++cursor.offset;
  }
  if (cursor.offset >= cursor.length) return false;
  const size_t start = cursor.offset;
  while (cursor.offset < cursor.length &&
         !asciiSpace(cursor.data[cursor.offset])) {
    ++cursor.offset;
  }
  token.data = cursor.data + start;
  token.length = cursor.offset - start;
  return token.length != 0;
}

bool noMoreTokens(Cursor cursor) {
  while (cursor.offset < cursor.length &&
         asciiSpace(cursor.data[cursor.offset])) {
    ++cursor.offset;
  }
  return cursor.offset == cursor.length;
}

// Consumes exactly one structural separator.  Any additional spaces belong
// to the user's name/message and are preserved.
bool remainingTail(Cursor& cursor, Token& tail) {
  if (cursor.offset >= cursor.length ||
      !asciiSpace(cursor.data[cursor.offset])) {
    return false;
  }
  ++cursor.offset;
  if (cursor.offset >= cursor.length) return false;
  tail.data = cursor.data + cursor.offset;
  tail.length = cursor.length - cursor.offset;
  cursor.offset = cursor.length;
  return true;
}

int hexNibble(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  value = asciiLower(value);
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return -1;
}

bool decodeHex(const Token& token, uint8_t* destination,
               size_t expectedBytes) {
  if (token.length != expectedBytes * 2U) return false;
  for (size_t i = 0; i < expectedBytes; ++i) {
    const int high = hexNibble(token.data[i * 2U]);
    const int low = hexNibble(token.data[i * 2U + 1U]);
    if (high < 0 || low < 0) return false;
    destination[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

bool allZero(const uint8_t* data, size_t length) {
  uint8_t combined = 0;
  for (size_t i = 0; i < length; ++i) combined |= data[i];
  return combined == 0;
}

bool parseUint32(const Token& token, uint32_t& output) {
  if (token.length == 0) return false;
  uint32_t value = 0;
  for (size_t i = 0; i < token.length; ++i) {
    const char c = token.data[i];
    if (c < '0' || c > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(c - '0');
    if (value > (UINT32_MAX - digit) / 10U) return false;
    value = value * 10U + digit;
  }
  output = value;
  return true;
}

bool parseChannel(const Token& token, uint8_t& output) {
  uint32_t value = 0;
  if (!parseUint32(token, value) || value >= kChannelCount) return false;
  output = static_cast<uint8_t>(value);
  return true;
}

bool copyUserText(const Token& source, char* destination, size_t capacity,
                  uint16_t& outputLength, ParseStatus& status,
                  bool isName) {
  if (source.length == 0) {
    status = isName ? ParseStatus::EmptyName : ParseStatus::EmptyText;
    return false;
  }
  if (source.length + 1U > capacity) {
    status = isName ? ParseStatus::NameTooLong : ParseStatus::TextTooLong;
    return false;
  }
  if (!validUserUtf8(source.data, source.length)) {
    status = ParseStatus::InvalidUtf8;
    return false;
  }
  memcpy(destination, source.data, source.length);
  destination[source.length] = '\0';
  outputLength = static_cast<uint16_t>(source.length);
  return true;
}

ParseStatus parseContactRole(const Token& token, ContactRole& output) {
  if (tokenEquals(token, "client")) {
    output = ContactRole::Client;
  } else if (tokenEquals(token, "repeater")) {
    output = ContactRole::Repeater;
  } else if (tokenEquals(token, "room")) {
    output = ContactRole::Room;
  } else if (tokenEquals(token, "sensor")) {
    output = ContactRole::Sensor;
  } else {
    return ParseStatus::InvalidRole;
  }
  return ParseStatus::Ok;
}

ParseStatus parseListCommand(Cursor& cursor, CommandKind kind,
                             Command& output) {
  if (!noMoreTokens(cursor)) return ParseStatus::BadSyntax;
  output.kind = kind;
  return ParseStatus::Ok;
}

}  // namespace

const char* parseStatusName(ParseStatus status) {
  switch (status) {
    case ParseStatus::Ok: return "ok";
    case ParseStatus::NotChat: return "not_chat";
    case ParseStatus::Empty: return "empty";
    case ParseStatus::LineTooLong: return "line_too_long";
    case ParseStatus::ControlCharacter: return "control_character";
    case ParseStatus::InvalidUtf8: return "invalid_utf8";
    case ParseStatus::BadSyntax: return "bad_syntax";
    case ParseStatus::BadHex: return "bad_hex";
    case ParseStatus::IntegerOverflow: return "integer_overflow";
    case ParseStatus::InvalidRole: return "invalid_role";
    case ParseStatus::InvalidRegionScope: return "invalid_region_scope";
    case ParseStatus::InvalidChannel: return "invalid_channel";
    case ParseStatus::PublicChannelImmutable:
      return "public_channel_immutable";
    case ParseStatus::ZeroKey: return "zero_key";
    case ParseStatus::EmptyName: return "empty_name";
    case ParseStatus::NameTooLong: return "name_too_long";
    case ParseStatus::EmptyText: return "empty_text";
    case ParseStatus::TextTooLong: return "text_too_long";
  }
  return "unknown";
}

const char* contactRoleName(ContactRole role) {
  switch (role) {
    case ContactRole::Client: return "client";
    case ContactRole::Repeater: return "repeater";
    case ContactRole::Room: return "room";
    case ContactRole::Sensor: return "sensor";
  }
  return "unknown";
}

bool validUserUtf8(const char* input, size_t inputLength) {
  if (!input && inputLength != 0) return false;
  size_t i = 0;
  while (i < inputLength) {
    const uint8_t first = static_cast<uint8_t>(input[i]);
    uint32_t codePoint = 0;
    size_t continuationCount = 0;
    if (first <= 0x7FU) {
      codePoint = first;
      continuationCount = 0;
    } else if (first >= 0xC2U && first <= 0xDFU) {
      codePoint = first & 0x1FU;
      continuationCount = 1;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      codePoint = first & 0x0FU;
      continuationCount = 2;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      codePoint = first & 0x07U;
      continuationCount = 3;
    } else {
      return false;
    }

    if (i + continuationCount >= inputLength) return false;
    for (size_t j = 1; j <= continuationCount; ++j) {
      const uint8_t next = static_cast<uint8_t>(input[i + j]);
      if ((next & 0xC0U) != 0x80U) return false;
      codePoint = (codePoint << 6U) | (next & 0x3FU);
    }

    if ((continuationCount == 1 && codePoint < 0x80U) ||
        (continuationCount == 2 && codePoint < 0x800U) ||
        (continuationCount == 3 && codePoint < 0x10000U) ||
        codePoint > 0x10FFFFU ||
        (codePoint >= 0xD800U && codePoint <= 0xDFFFU)) {
      return false;
    }
    // Serial framing and JSON safety: reject C0, DEL, and C1 controls.
    if (codePoint <= 0x1FU || codePoint == 0x7FU ||
        (codePoint >= 0x80U && codePoint <= 0x9FU)) {
      return false;
    }
    i += continuationCount + 1U;
  }
  return true;
}

ParseStatus parseCommand(const char* input, size_t inputLength,
                         Command& output) {
  output = Command{};
  if (!input || inputLength == 0) return ParseStatus::Empty;
  if (inputLength > kInputLineMaxBytes) return ParseStatus::LineTooLong;

  for (size_t i = 0; i < inputLength; ++i) {
    const uint8_t value = static_cast<uint8_t>(input[i]);
    if (value == 0 || value == '\r' || value == '\n' || value == 0x7F ||
        value < 0x20U) {
      return ParseStatus::ControlCharacter;
    }
  }
  if (!validUserUtf8(input, inputLength)) return ParseStatus::InvalidUtf8;

  Cursor cursor{input, inputLength, 0};
  Token token{};
  if (!nextToken(cursor, token)) return ParseStatus::Empty;
  if (!tokenEquals(token, "chat")) return ParseStatus::NotChat;
  if (!nextToken(cursor, token)) return ParseStatus::BadSyntax;

  if (tokenEquals(token, "status")) {
    return parseListCommand(cursor, CommandKind::Status, output);
  }
  if (tokenEquals(token, "reset")) {
    return parseListCommand(cursor, CommandKind::Reset, output);
  }
  if (tokenEquals(token, "contacts")) {
    return parseListCommand(cursor, CommandKind::ListContacts, output);
  }
  if (tokenEquals(token, "channels")) {
    return parseListCommand(cursor, CommandKind::ListChannels, output);
  }
  if (tokenEquals(token, "inbox")) {
    if (noMoreTokens(cursor)) {
      output.kind = CommandKind::ListInbox;
      return ParseStatus::Ok;
    }
    Token after{};
    if (!nextToken(cursor, after) || !noMoreTokens(cursor)) {
      return ParseStatus::BadSyntax;
    }
    if (!parseUint32(after, output.afterMessageId)) {
      return ParseStatus::IntegerOverflow;
    }
    output.kind = CommandKind::ListInbox;
    return ParseStatus::Ok;
  }

  if (tokenEquals(token, "contact")) {
    Token action{};
    Token key{};
    if (!nextToken(cursor, action) || !nextToken(cursor, key)) {
      return ParseStatus::BadSyntax;
    }
    if (!decodeHex(key, output.publicKey, kPublicKeyBytes)) {
      return ParseStatus::BadHex;
    }
    if (allZero(output.publicKey, sizeof(output.publicKey))) {
      return ParseStatus::ZeroKey;
    }
    if (tokenEquals(action, "drop")) {
      if (!noMoreTokens(cursor)) return ParseStatus::BadSyntax;
      output.kind = CommandKind::DropContact;
      return ParseStatus::Ok;
    }
    if (!tokenEquals(action, "set")) return ParseStatus::BadSyntax;
    Token role{};
    if (!nextToken(cursor, role)) return ParseStatus::BadSyntax;
    const ParseStatus roleStatus = parseContactRole(role, output.contactRole);
    if (roleStatus != ParseStatus::Ok) return roleStatus;
    Token name{};
    if (!remainingTail(cursor, name)) return ParseStatus::EmptyName;
    uint16_t nameLength = 0;
    ParseStatus status = ParseStatus::Ok;
    if (!copyUserText(name, output.name, sizeof(output.name), nameLength,
                      status, true)) {
      return status;
    }
    output.nameLength = static_cast<uint8_t>(nameLength);
    output.kind = CommandKind::SetContact;
    return ParseStatus::Ok;
  }

  if (tokenEquals(token, "channel")) {
    Token action{};
    Token channel{};
    if (!nextToken(cursor, action) || !nextToken(cursor, channel)) {
      return ParseStatus::BadSyntax;
    }
    if (!parseChannel(channel, output.channelIndex)) {
      return ParseStatus::InvalidChannel;
    }
    if (output.channelIndex == 0) {
      return ParseStatus::PublicChannelImmutable;
    }
    if (tokenEquals(action, "clear")) {
      if (!noMoreTokens(cursor)) return ParseStatus::BadSyntax;
      output.kind = CommandKind::ClearChannel;
      return ParseStatus::Ok;
    }
    if (!tokenEquals(action, "set")) return ParseStatus::BadSyntax;
    Token secret{};
    if (!nextToken(cursor, secret)) return ParseStatus::BadSyntax;
    if (tokenStartsWithExact(secret, "region_scope=")) {
      if (!tokenEqualsExact(secret, "region_scope=EU")) {
        return ParseStatus::InvalidRegionScope;
      }
      output.channelRegionScope = ChannelRegionScope::Eu;
      if (!nextToken(cursor, secret)) return ParseStatus::BadSyntax;
    }
    if (!decodeHex(secret, output.channelSecret, kChannelSecretBytes)) {
      return ParseStatus::BadHex;
    }
    if (allZero(output.channelSecret, sizeof(output.channelSecret))) {
      return ParseStatus::ZeroKey;
    }
    Token name{};
    if (!remainingTail(cursor, name)) return ParseStatus::EmptyName;
    uint16_t nameLength = 0;
    ParseStatus status = ParseStatus::Ok;
    if (!copyUserText(name, output.name, sizeof(output.name), nameLength,
                      status, true)) {
      return status;
    }
    output.nameLength = static_cast<uint8_t>(nameLength);
    output.kind = CommandKind::SetChannel;
    return ParseStatus::Ok;
  }

  if (tokenEquals(token, "send")) {
    Token targetKind{};
    Token target{};
    if (!nextToken(cursor, targetKind) || !nextToken(cursor, target)) {
      return ParseStatus::BadSyntax;
    }
    if (tokenEquals(targetKind, "dm")) {
      if (!decodeHex(target, output.contactReference,
                     kContactReferenceBytes)) {
        return ParseStatus::BadHex;
      }
      Token text{};
      if (!remainingTail(cursor, text)) return ParseStatus::EmptyText;
      ParseStatus status = ParseStatus::Ok;
      if (!copyUserText(text, output.text, sizeof(output.text),
                        output.textLength, status, false)) {
        return status;
      }
      output.kind = CommandKind::SendDirect;
      return ParseStatus::Ok;
    }
    if (tokenEquals(targetKind, "ch")) {
      if (!parseChannel(target, output.channelIndex)) {
        return ParseStatus::InvalidChannel;
      }
      Token text{};
      if (!remainingTail(cursor, text)) return ParseStatus::EmptyText;
      ParseStatus status = ParseStatus::Ok;
      char channelText[kKitsuChannelTextMaxBytes + 1]{};
      uint16_t channelTextLength = 0;
      if (!copyUserText(text, channelText, sizeof(channelText),
                        channelTextLength, status, false)) {
        return status;
      }
      memcpy(output.text, channelText, channelTextLength + 1U);
      output.textLength = channelTextLength;
      output.kind = CommandKind::SendChannel;
      return ParseStatus::Ok;
    }
    return ParseStatus::BadSyntax;
  }

  return ParseStatus::BadSyntax;
}

}  // namespace chat
}  // namespace kitsu868
