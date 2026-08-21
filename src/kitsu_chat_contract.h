#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kitsu868 {
namespace chat {

// v0.9 grows the serial input line just enough for one complete MeshCore text
// payload plus its target.  It is still a fixed, bounded buffer.
constexpr size_t kInputLineMaxBytes = 224;
// The pinned wire helper permits 160 bytes. Kitsu deliberately exposes a
// smaller symmetric 128-byte outbound limit to fit its bounded transport
// queues and leave headroom for the group sender prefix.
constexpr size_t kDirectTextMaxBytes = 128;
constexpr size_t kKitsuChannelTextMaxBytes = 128;
constexpr size_t kContactNameMaxBytes = 31;
constexpr size_t kChannelNameMaxBytes = 31;
constexpr size_t kPublicKeyBytes = 32;
constexpr size_t kChannelSecretBytes = 16;
constexpr size_t kContactReferenceBytes = 6;
constexpr uint8_t kChannelCount = 4;
constexpr uint8_t kContactCapacity = 12;
constexpr uint8_t kInboxCapacity = 24;

enum class ContactRole : uint8_t {
  Client = 1,
  Repeater = 2,
  Room = 3,
  Sensor = 4,
};

enum class CommandKind : uint8_t {
  None = 0,
  Status,
  Reset,
  ListContacts,
  ListChannels,
  ListInbox,
  SetContact,
  DropContact,
  SetChannel,
  ClearChannel,
  SendDirect,
  SendChannel,
};

enum class ParseStatus : uint8_t {
  Ok = 0,
  NotChat,
  Empty,
  LineTooLong,
  ControlCharacter,
  InvalidUtf8,
  BadSyntax,
  BadHex,
  IntegerOverflow,
  InvalidRole,
  InvalidChannel,
  PublicChannelImmutable,
  ZeroKey,
  EmptyName,
  NameTooLong,
  EmptyText,
  TextTooLong,
};

const char* parseStatusName(ParseStatus status);
const char* contactRoleName(ContactRole role);

// The parser never allocates and never mutates input.  Command words are
// matched case-insensitively, while the final name/text tail is copied exactly
// as UTF-8 so the existing lower-casing dispatcher cannot alter user content.
struct Command {
  CommandKind kind = CommandKind::None;
  uint32_t afterMessageId = 0;
  uint8_t channelIndex = 0;
  ContactRole contactRole = ContactRole::Client;
  uint8_t publicKey[kPublicKeyBytes]{};
  uint8_t channelSecret[kChannelSecretBytes]{};
  uint8_t contactReference[kContactReferenceBytes]{};
  char name[kContactNameMaxBytes + 1]{};
  uint8_t nameLength = 0;
  char text[kDirectTextMaxBytes + 1]{};
  uint16_t textLength = 0;
};

// Input excludes the trailing CR/LF.  A recognized but malformed `chat ...`
// line returns a specific error.  A non-chat line returns NotChat so the
// caller can dispatch it through the existing v0.8 command path.
ParseStatus parseCommand(const char* input, size_t inputLength,
                         Command& output);

// Public utility used by the serial integration before emitting JSON.  It is
// deliberately strict: overlong encodings, surrogate code points, Unicode
// control characters, NUL, CR and LF are rejected.
bool validUserUtf8(const char* input, size_t inputLength);

}  // namespace chat
}  // namespace kitsu868
