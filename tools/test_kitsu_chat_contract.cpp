#include "../src/kitsu_chat_contract.h"

#include <cstring>
#include <iostream>
#include <string>

namespace chat = kitsu868::chat;

namespace {

int failures = 0;

void check(bool condition, const char* description) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL " << description << '\n';
  }
}

chat::ParseStatus parse(const std::string& line, chat::Command& command) {
  return chat::parseCommand(line.data(), line.size(), command);
}

std::string repeated(char value, size_t count) {
  return std::string(count, value);
}

void testConstantsMatchTransportBounds() {
  check(chat::kInputLineMaxBytes == 224, "serial line is bounded at 224 bytes");
  check(chat::kDirectTextMaxBytes == 128,
        "Kitsu outbound direct text is bounded at 128 bytes");
  check(chat::kKitsuChannelTextMaxBytes == 128,
        "Kitsu outbound channel text is bounded at 128 bytes");
  check(chat::kContactCapacity == 12, "transport exposes twelve contacts");
  check(chat::kChannelCount == 4,
        "transport exposes Public plus three private channel slots");
  check(chat::kInboxCapacity == 24,
        "phone-facing RAM journal retains twenty-four messages");
}

void testQueries() {
  chat::Command command{};
  check(parse("chat status", command) == chat::ParseStatus::Ok &&
            command.kind == chat::CommandKind::Status,
        "status query parses");
  check(parse("CHAT RESET", command) == chat::ParseStatus::Ok &&
            command.kind == chat::CommandKind::Reset,
        "owner messaging reset parses case-insensitively");
  check(parse("chat reset now", command) == chat::ParseStatus::BadSyntax,
        "owner messaging reset takes no arguments");
  check(parse("CHAT CONTACTS", command) == chat::ParseStatus::Ok &&
            command.kind == chat::CommandKind::ListContacts,
        "command words are case-insensitive");
  check(parse("chat channels", command) == chat::ParseStatus::Ok &&
            command.kind == chat::CommandKind::ListChannels,
        "channel query parses");
  check(parse("chat inbox 4294967295", command) == chat::ParseStatus::Ok &&
            command.kind == chat::CommandKind::ListInbox &&
            command.afterMessageId == UINT32_MAX,
        "inbox accepts an unsigned boot-scoped cursor");
  check(parse("chat inbox 4294967296", command) ==
            chat::ParseStatus::IntegerOverflow,
        "inbox rejects cursor overflow");
  check(parse("mesh status", command) == chat::ParseStatus::NotChat,
        "non-chat commands fall through to the legacy dispatcher");
}

void testContactProvisioning() {
  const std::string key = repeated('A', 64);
  chat::Command command{};
  const std::string line = "CHAT CONTACT SET " + key +
                           " CLIENT Alice McFox";
  check(parse(line, command) == chat::ParseStatus::Ok &&
            command.kind == chat::CommandKind::SetContact &&
            command.contactRole == chat::ContactRole::Client &&
            std::strcmp(command.name, "Alice McFox") == 0 &&
            command.publicKey[0] == 0xAA && command.publicKey[31] == 0xAA,
        "full contact key/type/name provision in one bounded line");
  check(parse("chat contact drop " + key, command) == chat::ParseStatus::Ok &&
            command.kind == chat::CommandKind::DropContact,
        "full-key contact removal parses");
  check(parse("chat contact set " + repeated('0', 64) +
                  " client Alice",
              command) == chat::ParseStatus::ZeroKey,
        "all-zero contact identity is rejected");
  check(parse("chat contact set " + key + " kitsu Alice", command) ==
            chat::ParseStatus::InvalidRole,
        "private Kitsu wire role is rejected");
}

void testChannelProvisioning() {
  chat::Command command{};
  const std::string secret = "9CD8FCF22A47333B591D96A2B848B73F";
  check(parse("chat channel set 1 " + secret + " Team Alpha", command) ==
                chat::ParseStatus::Ok &&
            command.kind == chat::CommandKind::SetChannel &&
            command.channelIndex == 1 &&
            command.channelSecret[0] == 0x9C &&
            std::strcmp(command.name, "Team Alpha") == 0,
        "private channel name and 16-byte secret parse");
  check(parse("chat channel clear 3", command) == chat::ParseStatus::Ok &&
            command.kind == chat::CommandKind::ClearChannel,
        "last available private slot can be cleared");
  check(parse("chat channel clear 0", command) ==
            chat::ParseStatus::PublicChannelImmutable,
        "built-in Public channel is immutable");
  check(parse("chat channel clear 4", command) ==
            chat::ParseStatus::InvalidChannel,
        "unsupported channel slot is rejected");
  check(parse("chat channel set 1 " + repeated('0', 32) + " Team", command) ==
            chat::ParseStatus::ZeroKey,
        "all-zero private channel secret is rejected");
}

void testRawMessageTailAndBounds() {
  chat::Command command{};
  const std::string reference = "A1B2C3D4E5F6";
  const std::string mixed =
      "Meet Kitsu \xF0\x9F\xA6\x8A at 20:30";
  check(parse("CHAT SEND DM " + reference + " " + mixed, command) ==
                chat::ParseStatus::Ok &&
            command.kind == chat::CommandKind::SendDirect &&
            command.textLength == mixed.size() &&
            std::memcmp(command.text, mixed.data(), mixed.size()) == 0,
        "raw UTF-8 tail preserves case and emoji bytes");

  const std::string maxDirect = repeated('D', chat::kDirectTextMaxBytes);
  check(parse("chat send dm " + reference + " " + maxDirect, command) ==
            chat::ParseStatus::Ok,
        "128-byte direct message is accepted");
  check(parse("chat send dm " + reference + " " + maxDirect + "X", command) ==
            chat::ParseStatus::TextTooLong,
        "129-byte direct message is rejected");

  const std::string maxChannel = repeated('C', chat::kKitsuChannelTextMaxBytes);
  check(parse("chat send ch 0 " + maxChannel, command) ==
                chat::ParseStatus::Ok &&
            command.kind == chat::CommandKind::SendChannel,
        "128-byte channel message is accepted including Public");
  check(parse("chat send ch 0 " + maxChannel + "X", command) ==
            chat::ParseStatus::TextTooLong,
        "129-byte channel message is rejected");
}

void testFramingAndUtf8Rejection() {
  chat::Command command{};
  check(parse("chat send ch 0 hello\nworld", command) ==
            chat::ParseStatus::ControlCharacter,
        "embedded newline is rejected");
  check(parse("chat send ch 0 hello\tworld", command) ==
            chat::ParseStatus::ControlCharacter,
        "embedded tab is rejected");
  const std::string invalid =
      std::string("chat send ch 0 ") + static_cast<char>(0xC0) +
      static_cast<char>(0x80);
  check(parse(invalid, command) == chat::ParseStatus::InvalidUtf8,
        "overlong UTF-8 is rejected");
  check(parse(repeated('x', chat::kInputLineMaxBytes + 1), command) ==
            chat::ParseStatus::LineTooLong,
        "input longer than the fixed line buffer is rejected");
}

}  // namespace

int main() {
  testConstantsMatchTransportBounds();
  testQueries();
  testContactProvisioning();
  testChannelProvisioning();
  testRawMessageTailAndBounds();
  testFramingAndUtf8Rejection();

  if (failures != 0) {
    std::cerr << "TEST_FAIL kitsu_chat_contract failures=" << failures << '\n';
    return 1;
  }
  std::cout << "TEST_PASS kitsu_chat_contract" << '\n';
  return 0;
}
