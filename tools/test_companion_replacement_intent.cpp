#include "../src/companion_replacement_intent.h"

#include <cstring>
#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char* description) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL " << description << '\n';
  }
}

kitsu868::CompanionReplacementIntent foxGirlToFox() {
  kitsu868::CompanionReplacementIntent intent{};
  const char magic[8] = {'K', '8', '6', '8', 'R', 'P', '1', '\0'};
  std::memcpy(intent.magic, magic, sizeof(magic));
  intent.schema = kitsu868::KITSU_REPLACEMENT_INTENT_SCHEMA;
  intent.recordBytes = sizeof(intent);
  intent.sourcePackId = 0x492e6628U;
  intent.targetPackId = 0x6c393e21U;
  intent.targetRevision = 2U;
  intent.targetBytes = 24976U;
  intent.targetPayloadCrc32 = 0x2301202eU;
  intent.targetHeaderCrc32 = 0xac7b0040U;
  intent.recordCrc32 = kitsu868::companionReplacementIntentCrc32(intent);
  return intent;
}

}  // namespace

int main() {
  const kitsu868::CompanionReplacementIntent intent = foxGirlToFox();
  check(intent.recordCrc32 == 0x6098fe41U,
        "C++ intent CRC matches the web flasher golden vector");
  check(kitsu868::companionReplacementIntentAuthorizes(
            intent, 0x492e6628U, 0x6c393e21U, 2U, 24976U,
            0x2301202eU, 0xac7b0040U),
        "exact old ID and exact target metadata authorize replacement");
  check(!kitsu868::companionReplacementIntentAuthorizes(
            intent, 0x11111111U, 0x6c393e21U, 2U, 24976U,
            0x2301202eU, 0xac7b0040U),
        "different stored companion ID fails closed");
  kitsu868::CompanionReplacementIntent zeroSource = intent;
  zeroSource.sourcePackId = 0U;
  zeroSource.recordCrc32 =
      kitsu868::companionReplacementIntentCrc32(zeroSource);
  check(!kitsu868::companionReplacementIntentValid(zeroSource),
        "source ID zero is invalid in both firmware and web contracts");
  check(!kitsu868::companionReplacementIntentAuthorizes(
            intent, 0x492e6628U, 0x6c393e21U, 3U, 24976U,
            0x2301202eU, 0xac7b0040U),
        "different target revision fails closed");
  check(!kitsu868::companionReplacementIntentAuthorizes(
            intent, 0x492e6628U, 0x6c393e21U, 2U, 24976U,
            0x2301202fU, 0xac7b0040U),
        "different target payload fails closed");

  kitsu868::CompanionReplacementTransaction preparedOnly{};
  preparedOnly.prepared = intent;
  check(!kitsu868::companionReplacementTransactionAuthorizes(
            preparedOnly, 0x492e6628U, 0x6c393e21U, 2U, 24976U,
            0x2301202eU, 0xac7b0040U),
        "PREPARED without a separate COMMITTED record never authorizes reset");

  kitsu868::CompanionReplacementTransaction committed{};
  committed.prepared = intent;
  committed.committed = intent;
  check(kitsu868::companionReplacementTransactionAuthorizes(
            committed, 0x492e6628U, 0x6c393e21U, 2U, 24976U,
            0x2301202eU, 0xac7b0040U),
        "matching PREPARED and COMMITTED records authorize exact replacement");

  kitsu868::CompanionReplacementTransaction splitTarget = committed;
  splitTarget.committed.targetRevision += 1U;
  splitTarget.committed.recordCrc32 =
      kitsu868::companionReplacementIntentCrc32(splitTarget.committed);
  check(!kitsu868::companionReplacementTransactionValid(splitTarget),
        "different PREPARED and COMMITTED bodies fail closed");

  kitsu868::CompanionReplacementIntent tampered = intent;
  tampered.targetHeaderCrc32 ^= 1U;
  check(!kitsu868::companionReplacementIntentAuthorizes(
            tampered, 0x492e6628U, 0x6c393e21U, 2U, 24976U,
            0x2301202eU, 0xac7b0040U),
        "record mutation without a new CRC fails closed");

  kitsu868::CompanionReplacementIntent sameSpecies = intent;
  sameSpecies.sourcePackId = sameSpecies.targetPackId;
  sameSpecies.recordCrc32 =
      kitsu868::companionReplacementIntentCrc32(sameSpecies);
  check(!kitsu868::companionReplacementIntentAuthorizes(
            sameSpecies, sameSpecies.sourcePackId, sameSpecies.targetPackId,
            sameSpecies.targetRevision, sameSpecies.targetBytes,
            sameSpecies.targetPayloadCrc32, sameSpecies.targetHeaderCrc32),
        "same-species update cannot carry destructive intent");

  kitsu868::CompanionReplacementIntent fullSlot = intent;
  fullSlot.targetBytes = kitsu868::KITSU_COMPANION_PACK_MAX_BYTES;
  fullSlot.recordCrc32 =
      kitsu868::companionReplacementIntentCrc32(fullSlot);
  check(kitsu868::companionReplacementIntentAuthorizes(
            fullSlot, fullSlot.sourcePackId, fullSlot.targetPackId,
            fullSlot.targetRevision, fullSlot.targetBytes,
            fullSlot.targetPayloadCrc32, fullSlot.targetHeaderCrc32),
        "replacement intent preserves the complete original pack capacity");
  fullSlot.targetBytes = kitsu868::KITSU_COMPANION_PACK_MAX_BYTES + 1U;
  fullSlot.recordCrc32 =
      kitsu868::companionReplacementIntentCrc32(fullSlot);
  check(!kitsu868::companionReplacementIntentValid(fullSlot),
        "replacement intent rejects a target beyond the original pack slot");

  if (failures != 0) return 1;
  std::cout << "PASS companion replacement intent\n";
  return 0;
}
