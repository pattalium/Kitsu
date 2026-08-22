#include "../src/kitsu_legacy_connectivity_retirement.h"

#include <assert.h>
#include <string.h>

#include <algorithm>
#include <vector>

using kitsu868::connectivity::KitsuLegacyConnectivityRetirement;
using kitsu868::connectivity::LegacyConnectivityPartition;
using kitsu868::connectivity::LegacyConnectivityRetirementPlatform;
using kitsu868::connectivity::LegacyConnectivityRetirementResult;
using kitsu868::connectivity::kLegacyConnectivityPartitionAddress;
using kitsu868::connectivity::kLegacyConnectivityPartitionBytes;
using kitsu868::connectivity::kLegacyConnectivityPartitionLabel;
using kitsu868::connectivity::kLegacyConnectivityPartitionSubtype;
using kitsu868::connectivity::kLegacyConnectivityPartitionType;

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
    if (corruptReadback && eraseCalls != 0U && offset == 0U &&
        outputBytes != 0U) {
      output[0] = 0U;
    }
    return true;
  }

  bool eraseEntirePartition() override {
    ++eraseCalls;
    if (failErase) return false;
    std::fill(bytes.begin(), bytes.end(), static_cast<uint8_t>(0xffU));
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
  testFailuresAreClosedAndRetryable();
  testResultNamesAndSuccessBoundary();
  return 0;
}
