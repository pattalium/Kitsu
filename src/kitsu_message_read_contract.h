#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kitsu868 {
namespace message_read {

// The in-memory message journal is deliberately bounded to twenty-four rows.
// A read acknowledgement can therefore cover every retained message without
// accepting an unbounded request from a companion controller.
constexpr uint8_t kMaximumMessageIds = 24U;

enum class ParseStatus : uint8_t {
  Ok = 0,
  InvalidArgument,
  DuplicateMessageId,
};

struct Command {
  uint32_t journalSession = 0U;
  uint8_t messageCount = 0U;
  uint32_t messageIds[kMaximumMessageIds]{};
};

// Parses the exact authenticated-operation payload:
//   {"journal_session":"<uint32>","message_ids":["<uint32>", ...]}
//
// Both properties may appear in either order. Property names, decimal values,
// and array entries are escape-free canonical ASCII. The session and every ID
// must be non-zero, and the array contains 1..kMaximumMessageIds unique IDs.
ParseStatus parseCommand(const uint8_t* input, size_t inputBytes,
                         Command& output);

struct Record {
  uint32_t messageId = 0U;
  bool inbound = false;
  bool unread = false;
};

enum class PlanStatus : uint8_t {
  Ok = 0,
  JournalSessionMismatch,
  SnapshotChanged,
  MessageNotInbound,
};

struct Plan {
  uint8_t recordIndexes[kMaximumMessageIds]{};
  uint8_t messageCount = 0U;
  uint8_t markedCount = 0U;
  uint8_t unchangedCount = 0U;
};

// Builds a mutation plan without changing any record. Callers apply the plan
// only after Ok, which makes a mixed valid/invalid batch atomic. Already-read
// inbound records are valid and counted as unchanged, making retries
// idempotent.
PlanStatus plan(uint32_t currentJournalSession, const Command& command,
                const Record* records, uint8_t recordCount, Plan& output);

const char* planStatusError(PlanStatus status);

// Message IDs are uint32. Advancing the journal session whenever the allocator
// wraps prevents a stale pre-wrap ID from aliasing a newly reused row.
uint32_t advanceJournalSession(uint32_t current);

// A multi-row read must rotate before its first mutation if allocating all
// required revisions would cross UINT32_MAX. This preserves batch atomicity at
// the revision boundary.
bool revisionBatchRequiresGenerationAdvance(uint32_t currentRevision,
                                            uint8_t allocations);

}  // namespace message_read
}  // namespace kitsu868
