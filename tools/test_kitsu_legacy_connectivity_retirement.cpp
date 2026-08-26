#include "../src/kitsu_legacy_connectivity_retirement.h"
#include "../src/companion_replacement_intent.h"

#include <assert.h>
#include <string.h>

#include <algorithm>
#include <vector>

using kitsu868::connectivity::KitsuLegacyConnectivityRetirement;
using kitsu868::connectivity::LegacyConnectivityPartition;
using kitsu868::connectivity::LegacyConnectivityPreservation;
using kitsu868::connectivity::LegacyConnectivityRetirementPlatform;
using kitsu868::connectivity::LegacyConnectivityRetirementResult;
using kitsu868::connectivity::kLegacyConnectivityPartitionAddress;
using kitsu868::connectivity::kLegacyConnectivityPartitionBytes;
using kitsu868::connectivity::kLegacyConnectivityPartitionLabel;
using kitsu868::connectivity::kLegacyConnectivityPartitionSubtype;
using kitsu868::connectivity::kLegacyConnectivityPartitionType;
using kitsu868::KITSU_REPLACEMENT_TRANSACTION_BYTES;

namespace {

class MemoryPlatform final : public LegacyConnectivityRetirementPlatform {
 public:
  MemoryPlatform()
      : bytes(kLegacyConnectivityPartitionBytes, 0xffU) {
    memcpy(partition.label, kLegacyConnectivityPartitionLabel,
           sizeof(kLegacyConnectivityPartitionLabel));
    partition.type = kLegacyConnectivityPartitionType;
    partition.subtype = kLegacyConnectivityPartitionSubtype;
    partition.address = kLegacyConnectivityPartitionAddress;
    partition.size = kLegacyConnectivityPartitionBytes;
  }

  bool inspectPartition(LegacyConnectivityPartition& output) override {
    ++inspectCalls;
    if (failInspect) return false;
    output = partition;
    return true;
  }

  bool readPartition(size_t offset, uint8_t* output,
                     size_t outputBytes) override {
    ++readCalls;
    if (failRead || !output || offset > bytes.size() ||
        outputBytes > bytes.size() - offset) {
      return false;
    }
    memcpy(output, bytes.data() + offset, outputBytes);
    if (corruptReadback && eraseCalls != 0U && offset == lastEraseOffset &&
        outputBytes != 0U) {
      output[0] = 0U;
    }
    return true;
  }

  bool eraseEntirePartition() override {
    ++eraseCalls;
    lastEraseOffset = 0U;
    if (failErase) return false;
    std::fill(bytes.begin(), bytes.end(), static_cast<uint8_t>(0xffU));
    return true;
  }

  bool eraseAfterReplacementPrepared() override {
    ++eraseCalls;
    ++preparedEraseCalls;
    lastEraseOffset = kitsu868::KITSU_REPLACEMENT_INTENT_SECTOR_BYTES;
    if (failErase) return false;
    std::fill(
        bytes.begin() + kitsu868::KITSU_REPLACEMENT_INTENT_SECTOR_BYTES,
        bytes.end(), static_cast<uint8_t>(0xffU));
    return true;
  }

  bool eraseAfterReplacementTransaction() override {
    ++eraseCalls;
    ++tailEraseCalls;
    lastEraseOffset = KITSU_REPLACEMENT_TRANSACTION_BYTES;
    if (failErase) return false;
    std::fill(bytes.begin() + KITSU_REPLACEMENT_TRANSACTION_BYTES,
              bytes.end(), static_cast<uint8_t>(0xffU));
    return true;
  }

  bool clearLegacyReplayNamespace(bool& changed) override {
    ++namespaceChecks;
    changed = false;
    if (failNamespace) return false;
    if (namespacePresent) {
      namespacePresent = false;
      ++namespaceEraseWrites;
      changed = true;
    }
    return true;
  }

  LegacyConnectivityPartition partition{};
  std::vector<uint8_t> bytes;
  bool namespacePresent = false;
  bool failInspect = false;
  bool failRead = false;
  bool failErase = false;
  bool failNamespace = false;
  bool corruptReadback = false;
  size_t inspectCalls = 0U;
  size_t readCalls = 0U;
  size_t eraseCalls = 0U;
  size_t preparedEraseCalls = 0U;
  size_t tailEraseCalls = 0U;
  size_t lastEraseOffset = 0U;
  size_t namespaceChecks = 0U;
  size_t namespaceEraseWrites = 0U;
};

void assertErased(const MemoryPlatform& platform) {
  assert(std::all_of(platform.bytes.begin(), platform.bytes.end(),
                     [](uint8_t value) { return value == 0xffU; }));
}

void testRetiresWholePartitionAndNamespaceOnce() {
  MemoryPlatform platform;
  platform.bytes[0U] = 0x4bU;
  platform.bytes[30047U] = 0xa5U;
  platform.bytes[kLegacyConnectivityPartitionBytes - 1U] = 0x00U;
  platform.namespacePresent = true;
  assert(KitsuLegacyConnectivityRetirement::run(platform) ==
         LegacyConnectivityRetirementResult::OkRetired);
  assert(platform.eraseCalls == 1U);
  assert(platform.namespaceEraseWrites == 1U);
  assert(!platform.namespacePresent);
  assertErased(platform);

  const size_t eraseCalls = platform.eraseCalls;
  const size_t namespaceEraseWrites = platform.namespaceEraseWrites;
  assert(KitsuLegacyConnectivityRetirement::run(platform) ==
         LegacyConnectivityRetirementResult::OkAlreadyClean);
  assert(platform.eraseCalls == eraseCalls);
  assert(platform.namespaceEraseWrites == namespaceEraseWrites);
  assertErased(platform);
}

void testStrictPartitionIdentityFailsBeforeAuthority() {
  auto rejected = [](MemoryPlatform& platform) {
    assert(KitsuLegacyConnectivityRetirement::run(platform) ==
           LegacyConnectivityRetirementResult::InvalidPartition);
    assert(platform.readCalls == 0U);
    assert(platform.eraseCalls == 0U);
    assert(platform.namespaceChecks == 0U);
  };

  MemoryPlatform missing;
  missing.failInspect = true;
  rejected(missing);

  MemoryPlatform label;
  label.partition.label[0] = 'x';
  rejected(label);

  MemoryPlatform type;
  type.partition.type = 0x02U;
  rejected(type);

  MemoryPlatform subtype;
  subtype.partition.subtype = 0x41U;
  rejected(subtype);

  MemoryPlatform address;
  address.partition.address += 0x1000U;
  rejected(address);

  MemoryPlatform size;
  size.partition.size -= 0x1000U;
  rejected(size);
}

void testValidReplacementPrefixSurvivesRetirementAndRetry() {
  MemoryPlatform platform;
  for (size_t index = 0U; index < KITSU_REPLACEMENT_TRANSACTION_BYTES;
       ++index) {
    platform.bytes[index] = static_cast<uint8_t>((index * 29U) & 0xffU);
  }
  const std::vector<uint8_t> expectedPrefix(
      platform.bytes.begin(),
      platform.bytes.begin() + KITSU_REPLACEMENT_TRANSACTION_BYTES);
  platform.bytes[KITSU_REPLACEMENT_TRANSACTION_BYTES] = 0xa5U;
  platform.bytes.back() = 0x00U;

  assert(KitsuLegacyConnectivityRetirement::run(
             platform, LegacyConnectivityPreservation::Transaction) ==
         LegacyConnectivityRetirementResult::OkRetired);
  assert(platform.eraseCalls == 1U);
  assert(platform.tailEraseCalls == 1U);
  assert(std::equal(expectedPrefix.begin(), expectedPrefix.end(),
                    platform.bytes.begin()));
  assert(std::all_of(
      platform.bytes.begin() + KITSU_REPLACEMENT_TRANSACTION_BYTES,
      platform.bytes.end(),
      [](uint8_t value) { return value == 0xffU; }));

  const size_t eraseCalls = platform.eraseCalls;
  assert(KitsuLegacyConnectivityRetirement::run(
             platform, LegacyConnectivityPreservation::Transaction) ==
         LegacyConnectivityRetirementResult::OkAlreadyClean);
  assert(platform.eraseCalls == eraseCalls);
  assert(std::equal(expectedPrefix.begin(), expectedPrefix.end(),
                    platform.bytes.begin()));

  MemoryPlatform failure;
  failure.bytes[0U] = 0x4bU;
  failure.bytes[KITSU_REPLACEMENT_TRANSACTION_BYTES] = 0x11U;
  failure.failErase = true;
  assert(KitsuLegacyConnectivityRetirement::run(
             failure, LegacyConnectivityPreservation::Transaction) ==
         LegacyConnectivityRetirementResult::PartitionEraseFailed);
  assert(failure.bytes[0U] == 0x4bU);
  assert(failure.tailEraseCalls == 1U);

  MemoryPlatform tornCommit;
  for (size_t index = 0U;
       index < kitsu868::KITSU_REPLACEMENT_INTENT_SECTOR_BYTES; ++index) {
    tornCommit.bytes[index] = static_cast<uint8_t>((index * 17U) & 0xffU);
  }
  const std::vector<uint8_t> expectedPrepared(
      tornCommit.bytes.begin(),
      tornCommit.bytes.begin() +
          kitsu868::KITSU_REPLACEMENT_INTENT_SECTOR_BYTES);
  tornCommit.bytes[kitsu868::KITSU_REPLACEMENT_INTENT_SECTOR_BYTES] = 0x4bU;
  tornCommit.bytes[KITSU_REPLACEMENT_TRANSACTION_BYTES] = 0x22U;
  assert(KitsuLegacyConnectivityRetirement::run(
             tornCommit, LegacyConnectivityPreservation::Prepared) ==
         LegacyConnectivityRetirementResult::OkRetired);
  assert(tornCommit.preparedEraseCalls == 1U);
  assert(std::equal(expectedPrepared.begin(), expectedPrepared.end(),
                    tornCommit.bytes.begin()));
  assert(std::all_of(
      tornCommit.bytes.begin() +
          kitsu868::KITSU_REPLACEMENT_INTENT_SECTOR_BYTES,
      tornCommit.bytes.end(),
      [](uint8_t value) { return value == 0xffU; }));
}

void testFailuresAreClosedAndRetryable() {
  MemoryPlatform readFailure;
  readFailure.bytes[0U] = 0U;
  readFailure.failRead = true;
  assert(KitsuLegacyConnectivityRetirement::run(readFailure) ==
         LegacyConnectivityRetirementResult::PartitionReadFailed);
  assert(readFailure.eraseCalls == 0U);
  assert(readFailure.namespaceChecks == 0U);

  MemoryPlatform eraseFailure;
  eraseFailure.bytes[0U] = 0U;
  eraseFailure.failErase = true;
  assert(KitsuLegacyConnectivityRetirement::run(eraseFailure) ==
         LegacyConnectivityRetirementResult::PartitionEraseFailed);
  assert(eraseFailure.eraseCalls == 1U);
  assert(eraseFailure.namespaceChecks == 0U);

  MemoryPlatform badReadback;
  badReadback.bytes[0U] = 0U;
  badReadback.corruptReadback = true;
  assert(KitsuLegacyConnectivityRetirement::run(badReadback) ==
         LegacyConnectivityRetirementResult::PartitionReadbackFailed);
  assert(badReadback.eraseCalls == 1U);
  assert(badReadback.namespaceChecks == 0U);

  MemoryPlatform namespaceFailure;
  namespaceFailure.bytes[0U] = 0U;
  namespaceFailure.namespacePresent = true;
  namespaceFailure.failNamespace = true;
  assert(KitsuLegacyConnectivityRetirement::run(namespaceFailure) ==
         LegacyConnectivityRetirementResult::ReplayNamespaceFailed);
  assert(namespaceFailure.eraseCalls == 1U);
  assertErased(namespaceFailure);
  namespaceFailure.failNamespace = false;
  assert(KitsuLegacyConnectivityRetirement::run(namespaceFailure) ==
         LegacyConnectivityRetirementResult::OkRetired);
  assert(namespaceFailure.eraseCalls == 1U);
  assert(namespaceFailure.namespaceEraseWrites == 1U);
}

void testResultNamesAndSuccessBoundary() {
  using kitsu868::connectivity::legacyConnectivityRetirementResultName;
  using kitsu868::connectivity::legacyConnectivityRetirementSucceeded;
  assert(strcmp(legacyConnectivityRetirementResultName(
                    LegacyConnectivityRetirementResult::OkAlreadyClean),
                "already_clean") == 0);
  assert(strcmp(legacyConnectivityRetirementResultName(
                    LegacyConnectivityRetirementResult::OkRetired),
                "retired") == 0);
  assert(legacyConnectivityRetirementSucceeded(
      LegacyConnectivityRetirementResult::OkAlreadyClean));
  assert(legacyConnectivityRetirementSucceeded(
      LegacyConnectivityRetirementResult::OkRetired));
  assert(!legacyConnectivityRetirementSucceeded(
      LegacyConnectivityRetirementResult::InvalidPartition));
}

}  // namespace

int main() {
  testRetiresWholePartitionAndNamespaceOnce();
  testStrictPartitionIdentityFailsBeforeAuthority();
  testValidReplacementPrefixSurvivesRetirementAndRetry();
  testFailuresAreClosedAndRetryable();
  testResultNamesAndSuccessBoundary();
  return 0;
}
