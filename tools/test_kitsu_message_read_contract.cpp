#include "../src/kitsu_message_read_contract.h"

#include <cstring>
#include <iostream>
#include <string>

namespace read_contract = kitsu868::message_read;

namespace {

int failures = 0;

void check(bool condition, const char* description) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL " << description << '\n';
  }
}

read_contract::ParseStatus parse(const std::string& json,
                                 read_contract::Command& command) {
  return read_contract::parseCommand(
      reinterpret_cast<const uint8_t*>(json.data()), json.size(), command);
}

void testExactRequestSchema() {
  read_contract::Command command{};
  check(parse("{\"journal_session\":\"7\",\"message_ids\":[\"1\",\"4294967295\"]}",
              command) == read_contract::ParseStatus::Ok &&
            command.journalSession == 7U && command.messageCount == 2U &&
            command.messageIds[0] == 1U &&
            command.messageIds[1] == UINT32_MAX,
        "canonical request parses");
  check(parse(" { \"message_ids\" : [ \"9\" ], \n"
              "\"journal_session\" : \"8\" } ",
              command) == read_contract::ParseStatus::Ok &&
            command.journalSession == 8U && command.messageIds[0] == 9U,
        "exact fields may be reversed with JSON whitespace");

  const char* const invalid[] = {
      "{}",
      "{\"journal_session\":\"7\",\"message_ids\":[]}",
      "{\"journal_session\":\"0\",\"message_ids\":[\"1\"]}",
      "{\"journal_session\":\"07\",\"message_ids\":[\"1\"]}",
      "{\"journal_session\":7,\"message_ids\":[\"1\"]}",
      "{\"journal_session\":\"7\",\"message_ids\":[\"0\"]}",
      "{\"journal_session\":\"7\",\"message_ids\":[\"01\"]}",
      "{\"journal_session\":\"7\",\"message_ids\":[\"4294967296\"]}",
      "{\"journal_session\":\"7\",\"message_ids\":[1]}",
      "{\"journal_session\":\"7\",\"messages\":[\"1\"]}",
      "{\"journal_session\":\"7\",\"message_ids\":[\"1\"],\"extra\":1}",
      "{\"journal_session\":\"7\",\"journal_session\":\"7\"}",
      "{\"journal_session\":\"7\",\"message_ids\":[\"1\"],}",
  };
  for (const char* value : invalid) {
    check(parse(value, command) == read_contract::ParseStatus::InvalidArgument,
          "malformed, noncanonical, or non-exact request is rejected");
  }
  check(parse("{\"journal_session\":\"7\",\"message_ids\":[\"1\",\"1\"]}",
              command) == read_contract::ParseStatus::DuplicateMessageId,
        "duplicate IDs are rejected before planning");
}

std::string maximumBatchJson(uint8_t count) {
  std::string output =
      "{\"journal_session\":\"1\",\"message_ids\":[";
  for (uint8_t index = 0U; index < count; ++index) {
    if (index != 0U) output += ',';
    output += '"';
    output += std::to_string(static_cast<unsigned>(index) + 1U);
    output += '"';
  }
  output += "]}";
  return output;
}

void testBoundedBatch() {
  read_contract::Command command{};
  check(parse(maximumBatchJson(read_contract::kMaximumMessageIds), command) ==
            read_contract::ParseStatus::Ok &&
            command.messageCount == read_contract::kMaximumMessageIds,
        "all twenty-four retained rows fit one bounded read request");
  check(parse(maximumBatchJson(
                  static_cast<uint8_t>(read_contract::kMaximumMessageIds + 1U)),
              command) == read_contract::ParseStatus::InvalidArgument,
        "twenty-fifth ID is rejected");
}

void testAtomicPlanningAndIdempotence() {
  read_contract::Record records[] = {
      {10U, true, true},
      {11U, false, false},
      {12U, true, false},
  };
  read_contract::Command command{};
  command.journalSession = 7U;
  command.messageCount = 2U;
  command.messageIds[0] = 10U;
  command.messageIds[1] = 12U;
  read_contract::Plan plan{};
  check(read_contract::plan(7U, command, records, 3U, plan) ==
            read_contract::PlanStatus::Ok &&
            plan.messageCount == 2U && plan.recordIndexes[0] == 0U &&
            plan.recordIndexes[1] == 2U && plan.markedCount == 1U &&
            plan.unchangedCount == 1U,
        "valid mixed unread/already-read batch produces one atomic plan");

  // Simulate applying the first plan. The identical retry remains accepted
  // but produces no further mutation.
  records[0].unread = false;
  check(read_contract::plan(7U, command, records, 3U, plan) ==
            read_contract::PlanStatus::Ok &&
            plan.markedCount == 0U && plan.unchangedCount == 2U,
        "identical retry is idempotently unchanged");

  command.messageIds[1] = 99U;
  const read_contract::Record beforeMissing[] = {records[0], records[1],
                                                 records[2]};
  check(read_contract::plan(7U, command, records, 3U, plan) ==
            read_contract::PlanStatus::SnapshotChanged &&
            std::memcmp(beforeMissing, records, sizeof(records)) == 0,
        "mixed missing-ID batch fails before any record mutation");

  command.messageCount = 1U;
  command.messageIds[0] = 11U;
  check(read_contract::plan(7U, command, records, 3U, plan) ==
            read_contract::PlanStatus::MessageNotInbound,
        "outbound target is rejected");
  command.messageIds[0] = 10U;
  check(read_contract::plan(8U, command, records, 3U, plan) ==
            read_contract::PlanStatus::JournalSessionMismatch,
        "stale journal session is rejected before ID lookup");
}

void testMaximumPlanAndSessionRotation() {
  read_contract::Record records[read_contract::kMaximumMessageIds]{};
  read_contract::Command command{};
  command.journalSession = UINT32_MAX;
  command.messageCount = read_contract::kMaximumMessageIds;
  for (uint8_t index = 0U; index < read_contract::kMaximumMessageIds; ++index) {
    records[index] = {static_cast<uint32_t>(index) + 1U, true, true};
    command.messageIds[index] = static_cast<uint32_t>(index) + 1U;
  }
  read_contract::Plan plan{};
  check(read_contract::plan(UINT32_MAX, command, records,
                            read_contract::kMaximumMessageIds, plan) ==
            read_contract::PlanStatus::Ok &&
            plan.markedCount == read_contract::kMaximumMessageIds,
        "maximum read batch plans every retained unread row");

  check(read_contract::advanceJournalSession(7U) == 8U,
        "ordinary journal generation advances");
  check(read_contract::advanceJournalSession(UINT32_MAX) == 1U,
        "journal generation skips zero on wrap");
  check(!read_contract::revisionBatchRequiresGenerationAdvance(
            UINT32_MAX - 2U, 2U),
        "batch ending exactly at UINT32_MAX stays in one generation");
  check(read_contract::revisionBatchRequiresGenerationAdvance(
            UINT32_MAX - 1U, 2U),
        "multi-ID batch crossing revision wrap rotates before mutation");
  check(!read_contract::revisionBatchRequiresGenerationAdvance(
            UINT32_MAX, 0U),
        "empty/idempotent batch never rotates a generation");

  // A reused message ID after uint32 wrap cannot be selected by a command
  // carrying the pre-wrap generation.
  command.journalSession = UINT32_MAX;
  command.messageCount = 1U;
  command.messageIds[0] = 1U;
  check(read_contract::plan(1U, command, records,
                            read_contract::kMaximumMessageIds, plan) ==
            read_contract::PlanStatus::JournalSessionMismatch,
        "session rotation prevents stale ID reuse aliasing");
}

}  // namespace

int main() {
  testExactRequestSchema();
  testBoundedBatch();
  testAtomicPlanningAndIdempotence();
  testMaximumPlanAndSessionRotation();
  if (failures != 0) {
    std::cerr << failures << " message-read contract test(s) failed\n";
    return 1;
  }
  std::cout << "TEST_PASS kitsu_message_read_contract strict atomic idempotent"
               " bounded\n";
  return 0;
}
