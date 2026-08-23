#include "kitsu_message_read_contract.h"

#include <string.h>

namespace kitsu868 {
namespace message_read {
namespace {

void skipWhitespace(const uint8_t* input, size_t inputBytes, size_t& cursor) {
  while (cursor < inputBytes &&
         (input[cursor] == ' ' || input[cursor] == '\t' ||
          input[cursor] == '\r' || input[cursor] == '\n')) {
    ++cursor;
  }
}

bool consume(const uint8_t* input, size_t inputBytes, size_t& cursor,
             uint8_t expected) {
  skipWhitespace(input, inputBytes, cursor);
  if (cursor >= inputBytes || input[cursor] != expected) return false;
  ++cursor;
  return true;
}

bool parseAsciiString(const uint8_t* input, size_t inputBytes, size_t& cursor,
                      const uint8_t*& output, size_t& outputBytes) {
  skipWhitespace(input, inputBytes, cursor);
  if (cursor >= inputBytes || input[cursor++] != '"') return false;
  const size_t start = cursor;
  while (cursor < inputBytes && input[cursor] != '"') {
    // The contract has no user-controlled strings. Rejecting escapes and
    // non-ASCII bytes gives property and decimal tokens one canonical form.
    if (input[cursor] < 0x20U || input[cursor] > 0x7eU ||
        input[cursor] == '\\') {
      return false;
    }
    ++cursor;
  }
  if (cursor >= inputBytes) return false;
  output = input + start;
  outputBytes = cursor - start;
  ++cursor;
  return true;
}

bool sameToken(const uint8_t* input, size_t inputBytes,
               const char* expected) {
  const size_t expectedBytes = strlen(expected);
  return inputBytes == expectedBytes &&
         memcmp(input, expected, expectedBytes) == 0;
}

bool parseCanonicalNonzeroUint32(const uint8_t* input, size_t inputBytes,
                                 uint32_t& output) {
  if (!input || inputBytes == 0U || inputBytes > 10U || input[0] == '0') {
    return false;
  }
  uint64_t value = 0U;
  for (size_t index = 0U; index < inputBytes; ++index) {
    if (input[index] < '0' || input[index] > '9') return false;
    value = value * 10U + static_cast<uint8_t>(input[index] - '0');
    if (value > UINT32_MAX) return false;
  }
  output = static_cast<uint32_t>(value);
  return output != 0U;
}

bool parseQuotedUint32(const uint8_t* input, size_t inputBytes,
                       size_t& cursor, uint32_t& output) {
  const uint8_t* token = nullptr;
  size_t tokenBytes = 0U;
  return parseAsciiString(input, inputBytes, cursor, token, tokenBytes) &&
         parseCanonicalNonzeroUint32(token, tokenBytes, output);
}

ParseStatus parseMessageIds(const uint8_t* input, size_t inputBytes,
                            size_t& cursor, Command& output) {
  if (!consume(input, inputBytes, cursor, '[')) {
    return ParseStatus::InvalidArgument;
  }
  skipWhitespace(input, inputBytes, cursor);
  if (cursor < inputBytes && input[cursor] == ']') {
    return ParseStatus::InvalidArgument;
  }

  while (true) {
    if (output.messageCount >= kMaximumMessageIds) {
      return ParseStatus::InvalidArgument;
    }
    uint32_t messageId = 0U;
    if (!parseQuotedUint32(input, inputBytes, cursor, messageId)) {
      return ParseStatus::InvalidArgument;
    }
    for (uint8_t index = 0U; index < output.messageCount; ++index) {
      if (output.messageIds[index] == messageId) {
        return ParseStatus::DuplicateMessageId;
      }
    }
    output.messageIds[output.messageCount++] = messageId;

    skipWhitespace(input, inputBytes, cursor);
    if (cursor >= inputBytes) return ParseStatus::InvalidArgument;
    if (input[cursor] == ']') {
      ++cursor;
      return ParseStatus::Ok;
    }
    if (input[cursor++] != ',') return ParseStatus::InvalidArgument;
  }
}

}  // namespace

ParseStatus parseCommand(const uint8_t* input, size_t inputBytes,
                         Command& output) {
  output = Command{};
  if (!input || inputBytes == 0U) return ParseStatus::InvalidArgument;

  size_t cursor = 0U;
  if (!consume(input, inputBytes, cursor, '{')) {
    return ParseStatus::InvalidArgument;
  }
  bool sawSession = false;
  bool sawMessageIds = false;
  for (uint8_t field = 0U; field < 2U; ++field) {
    const uint8_t* key = nullptr;
    size_t keyBytes = 0U;
    if (!parseAsciiString(input, inputBytes, cursor, key, keyBytes) ||
        !consume(input, inputBytes, cursor, ':')) {
      return ParseStatus::InvalidArgument;
    }
    if (sameToken(key, keyBytes, "journal_session")) {
      if (sawSession ||
          !parseQuotedUint32(input, inputBytes, cursor,
                             output.journalSession)) {
        return ParseStatus::InvalidArgument;
      }
      sawSession = true;
    } else if (sameToken(key, keyBytes, "message_ids")) {
      if (sawMessageIds) return ParseStatus::InvalidArgument;
      const ParseStatus status =
          parseMessageIds(input, inputBytes, cursor, output);
      if (status != ParseStatus::Ok) return status;
      sawMessageIds = true;
    } else {
      return ParseStatus::InvalidArgument;
    }

    skipWhitespace(input, inputBytes, cursor);
    if (field == 0U) {
      if (cursor >= inputBytes || input[cursor++] != ',') {
        return ParseStatus::InvalidArgument;
      }
    }
  }
  if (!sawSession || !sawMessageIds ||
      !consume(input, inputBytes, cursor, '}')) {
    return ParseStatus::InvalidArgument;
  }
  skipWhitespace(input, inputBytes, cursor);
  return cursor == inputBytes ? ParseStatus::Ok
                              : ParseStatus::InvalidArgument;
}

PlanStatus plan(uint32_t currentJournalSession, const Command& command,
                const Record* records, uint8_t recordCount, Plan& output) {
  output = Plan{};
  if (currentJournalSession == 0U ||
      command.journalSession != currentJournalSession) {
    return PlanStatus::JournalSessionMismatch;
  }
  if (!records || recordCount > kMaximumMessageIds ||
      command.messageCount == 0U ||
      command.messageCount > kMaximumMessageIds) {
    return PlanStatus::SnapshotChanged;
  }

  for (uint8_t requested = 0U; requested < command.messageCount; ++requested) {
    uint8_t match = recordCount;
    for (uint8_t candidate = 0U; candidate < recordCount; ++candidate) {
      if (records[candidate].messageId == command.messageIds[requested]) {
        match = candidate;
        break;
      }
    }
    if (match == recordCount) return PlanStatus::SnapshotChanged;
    if (!records[match].inbound) return PlanStatus::MessageNotInbound;
    output.recordIndexes[output.messageCount++] = match;
    if (records[match].unread) ++output.markedCount;
    else ++output.unchangedCount;
  }
  return PlanStatus::Ok;
}

const char* planStatusError(PlanStatus status) {
  switch (status) {
    case PlanStatus::Ok: return nullptr;
    case PlanStatus::JournalSessionMismatch:
      return "journal_session_mismatch";
    case PlanStatus::SnapshotChanged: return "snapshot_changed";
    case PlanStatus::MessageNotInbound: return "message_not_inbound";
  }
  return "request_rejected";
}

uint32_t advanceJournalSession(uint32_t current) {
  ++current;
  return current == 0U ? 1U : current;
}

bool revisionBatchRequiresGenerationAdvance(uint32_t currentRevision,
                                            uint8_t allocations) {
  return allocations != 0U &&
         currentRevision > UINT32_MAX - static_cast<uint32_t>(allocations);
}

}  // namespace message_read
}  // namespace kitsu868
