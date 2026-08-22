#include "../src/kitsu_device_security.h"

#include <assert.h>
#include <string.h>

using kitsu868::connectivity::DeviceSecurityPlatform;
using kitsu868::connectivity::DeviceSecurityStorage;
using kitsu868::connectivity::KitsuDeviceSecurity;
using kitsu868::connectivity::SecurityMode;
using kitsu868::connectivity::SecurityResult;
using kitsu868::connectivity::kKitsuControllerIdBytes;
using kitsu868::connectivity::kKitsuSecretBytes;
using kitsu868::connectivity::kSecurityBlobCapacity;
using kitsu868::connectivity::kSecurityNonceBytes;
using kitsu868::connectivity::kSecuritySlots;
using kitsu868::connectivity::kSecurityTagBytes;

namespace {

uint32_t mix(uint32_t state, uint8_t value) {
  state ^= value;
  state *= 16777619UL;
  state ^= state >> 11U;
  return state;
}

class TestPlatform final : public DeviceSecurityPlatform {
 public:
  SecurityMode securityMode() const override { return mode; }

  bool deriveWrappingKey(const uint8_t hardwareId[8],
                         uint8_t output[kKitsuSecretBytes]) override {
    if (failRoot) return false;
    for (size_t i = 0U; i < kKitsuSecretBytes; ++i) {
      output[i] = static_cast<uint8_t>(hardwareId[i & 7U] ^ i ^ 0xa5U);
    }
    return true;
  }

  bool randomBytes(uint8_t* output, size_t outputBytes) override {
    if (failRandom || !output) return false;
    for (size_t i = 0U; i < outputBytes; ++i) {
      state = state * 1664525UL + 1013904223UL;
      output[i] = static_cast<uint8_t>(state >> 24U);
    }
    return true;
  }

  bool seal(const uint8_t key[kKitsuSecretBytes], uint32_t generation,
            const uint8_t nonce[kSecurityNonceBytes],
            const uint8_t* plaintext, size_t plaintextBytes,
            uint8_t* ciphertext,
            uint8_t tag[kSecurityTagBytes]) override {
    for (size_t i = 0U; i < plaintextBytes; ++i) {
      ciphertext[i] = static_cast<uint8_t>(
          plaintext[i] ^ key[i % kKitsuSecretBytes] ^
          nonce[i % kSecurityNonceBytes] ^
          static_cast<uint8_t>(generation + i * 13U));
    }
    makeTag(key, generation, nonce, ciphertext, plaintextBytes, tag);
    return true;
  }

  bool open(const uint8_t key[kKitsuSecretBytes], uint32_t generation,
            const uint8_t nonce[kSecurityNonceBytes],
            const uint8_t* ciphertext, size_t ciphertextBytes,
            const uint8_t tag[kSecurityTagBytes],
            uint8_t* plaintext) override {
    uint8_t expected[kSecurityTagBytes]{};
    makeTag(key, generation, nonce, ciphertext, ciphertextBytes, expected);
    if (memcmp(expected, tag, sizeof(expected)) != 0) return false;
    for (size_t i = 0U; i < ciphertextBytes; ++i) {
      plaintext[i] = static_cast<uint8_t>(
          ciphertext[i] ^ key[i % kKitsuSecretBytes] ^
          nonce[i % kSecurityNonceBytes] ^
          static_cast<uint8_t>(generation + i * 13U));
    }
    return true;
  }

  bool hkdfSha256(const uint8_t* inputKey, size_t inputKeyBytes,
                  const uint8_t* salt, size_t saltBytes,
                  const uint8_t* info, size_t infoBytes,
                  uint8_t* output, size_t outputBytes) override {
    if (!inputKey || !salt || !info || !output) return false;
    uint32_t hash = 2166136261UL;
    for (size_t i = 0U; i < inputKeyBytes; ++i) hash = mix(hash, inputKey[i]);
    for (size_t i = 0U; i < saltBytes; ++i) hash = mix(hash, salt[i]);
    for (size_t i = 0U; i < infoBytes; ++i) hash = mix(hash, info[i]);
    for (size_t i = 0U; i < outputBytes; ++i) {
      hash = mix(hash, static_cast<uint8_t>(i));
      output[i] = static_cast<uint8_t>(hash >> ((i & 3U) * 8U));
    }
    return true;
  }

  SecurityMode mode = SecurityMode::Reflashable;
  bool failRoot = false;
  bool failRandom = false;
  uint32_t state = 7U;

 private:
  static void makeTag(const uint8_t key[kKitsuSecretBytes],
                      uint32_t generation,
                      const uint8_t nonce[kSecurityNonceBytes],
                      const uint8_t* data, size_t bytes,
                      uint8_t tag[kSecurityTagBytes]) {
    uint32_t hash = 2166136261UL ^ generation;
    for (size_t i = 0U; i < kKitsuSecretBytes; ++i) hash = mix(hash, key[i]);
    for (size_t i = 0U; i < kSecurityNonceBytes; ++i) hash = mix(hash, nonce[i]);
    for (size_t i = 0U; i < bytes; ++i) hash = mix(hash, data[i]);
    for (size_t i = 0U; i < kSecurityTagBytes; ++i) {
      hash = mix(hash, static_cast<uint8_t>(i + bytes));
      tag[i] = static_cast<uint8_t>(hash >> ((i & 3U) * 8U));
    }
  }
};

class MemoryStorage final : public DeviceSecurityStorage {
 public:
  bool readSlot(uint8_t slot, uint8_t* output, size_t capacity,
                size_t& outputBytes) override {
    if (failRead || slot >= kSecuritySlots || sizes[slot] > capacity) {
      return false;
    }
    memcpy(output, data[slot], sizes[slot]);
    outputBytes = sizes[slot];
    return true;
  }

  bool writeSlot(uint8_t slot, const uint8_t* input,
                 size_t inputBytes) override {
    ++writeCalls;
    if (slot >= kSecuritySlots || inputBytes > kSecurityBlobCapacity) {
      return false;
    }
    if (failWrite) return false;
    if (tearWrite) {
      const size_t bytes = inputBytes < 60U ? inputBytes : 60U;
      memcpy(data[slot], input, bytes);
      sizes[slot] = bytes;
      tearWrite = false;
      return false;
    }
    memcpy(data[slot], input, inputBytes);
    sizes[slot] = inputBytes;
    if (corruptWrite && inputBytes > 70U) {
      data[slot][70U] ^= 0x80U;
      corruptWrite = false;
    }
    return true;
  }

  bool clearSlot(uint8_t slot) override {
    ++clearCalls;
    if (slot >= kSecuritySlots || failClear) return false;
    memset(data[slot], 0, sizeof(data[slot]));
    sizes[slot] = 0U;
    return true;
  }

  bool contains(const uint8_t* needle, size_t needleBytes) const {
    for (size_t slot = 0U; slot < kSecuritySlots; ++slot) {
      if (needleBytes > sizes[slot]) continue;
      for (size_t offset = 0U; offset + needleBytes <= sizes[slot]; ++offset) {
        if (memcmp(data[slot] + offset, needle, needleBytes) == 0) return true;
      }
    }
    return false;
  }

  uint8_t data[kSecuritySlots][kSecurityBlobCapacity]{};
  size_t sizes[kSecuritySlots]{};
  bool failRead = false;
  bool failWrite = false;
  bool tearWrite = false;
  bool corruptWrite = false;
  bool failClear = false;
  size_t writeCalls = 0U;
  size_t clearCalls = 0U;
};

const uint8_t kHardwareId[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};

constexpr size_t kOuterHeaderBytes = 44U;
constexpr size_t kPlainBytes = 320U;
constexpr size_t kPlainCrcOffset = 316U;
constexpr size_t kRetiredKeyOffset = 60U;
constexpr size_t kRetiredKeyBytes = 32U;
constexpr size_t kControllerTableOffset = 92U;
constexpr size_t kControllerTableBytes = 208U;
constexpr size_t kRetiredCountersOffset = 300U;
constexpr size_t kRetiredCountersBytes = 16U;

uint32_t readU32(const uint8_t* input) {
  return static_cast<uint32_t>(input[0]) |
      (static_cast<uint32_t>(input[1]) << 8U) |
      (static_cast<uint32_t>(input[2]) << 16U) |
      (static_cast<uint32_t>(input[3]) << 24U);
}

void writeU32(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
  output[2] = static_cast<uint8_t>(value >> 16U);
  output[3] = static_cast<uint8_t>(value >> 24U);
}

uint32_t plaintextCrc32(const uint8_t* input, size_t bytes) {
  uint32_t crc = 0xffffffffUL;
  for (size_t index = 0U; index < bytes; ++index) {
    crc ^= input[index];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = static_cast<uint32_t>(
          -static_cast<int32_t>(crc & 1U));
      crc = (crc >> 1U) ^ (0xedb88320UL & mask);
    }
  }
  return ~crc;
}

bool openSlotPlaintext(const MemoryStorage& storage, uint8_t slot,
                       TestPlatform& platform,
                       uint8_t output[kPlainBytes]) {
  if (slot >= kSecuritySlots ||
      storage.sizes[slot] != kOuterHeaderBytes + kPlainBytes) {
    return false;
  }
  const uint8_t* blob = storage.data[slot];
  uint8_t wrappingKey[kKitsuSecretBytes]{};
  if (!platform.deriveWrappingKey(kHardwareId, wrappingKey)) return false;
  const bool opened = platform.open(
      wrappingKey, readU32(blob + 8U), blob + 16U,
      blob + kOuterHeaderBytes, kPlainBytes, blob + 28U, output);
  memset(wrappingKey, 0, sizeof(wrappingKey));
  return opened;
}

bool openActivePlaintext(const MemoryStorage& storage,
                         const KitsuDeviceSecurity& security,
                         TestPlatform& platform,
                         uint8_t output[kPlainBytes]) {
  const int8_t active = security.status().activeSlot;
  return active >= 0 && openSlotPlaintext(
      storage, static_cast<uint8_t>(active), platform, output);
}

bool retiredRangesAreZero(const uint8_t plain[kPlainBytes]) {
  uint8_t combined = 0U;
  for (size_t index = 0U; index < kRetiredKeyBytes; ++index) {
    combined |= plain[kRetiredKeyOffset + index];
  }
  for (size_t index = 0U; index < kRetiredCountersBytes; ++index) {
    combined |= plain[kRetiredCountersOffset + index];
  }
  return combined == 0U;
}

bool controllerTableIsZero(const uint8_t plain[kPlainBytes]) {
  uint8_t combined = 0U;
  for (size_t index = 0U; index < kControllerTableBytes; ++index) {
    combined |= plain[kControllerTableOffset + index];
  }
  return combined == 0U;
}

void assertAllStoredGenerationsSanitized(const MemoryStorage& storage,
                                         TestPlatform& platform,
                                         bool expectEmptyControllerTable) {
  for (uint8_t slot = 0U; slot < kSecuritySlots; ++slot) {
    uint8_t plain[kPlainBytes]{};
    assert(openSlotPlaintext(storage, slot, platform, plain));
    assert(retiredRangesAreZero(plain));
    if (expectEmptyControllerTable) assert(controllerTableIsZero(plain));
  }
}

bool seedRetiredNetworkMaterial(MemoryStorage& storage,
                                const KitsuDeviceSecurity& security,
                                TestPlatform& platform) {
  const int8_t active = security.status().activeSlot;
  if (active < 0) return false;
  uint8_t* blob = storage.data[static_cast<uint8_t>(active)];
  uint8_t plain[kPlainBytes]{};
  if (!openActivePlaintext(storage, security, platform, plain)) return false;
  writeU32(plain + 8U, readU32(plain + 8U) | 0x01U);
  memset(plain + kRetiredKeyOffset, 0xa5, kRetiredKeyBytes);
  memset(plain + kRetiredCountersOffset, 0x5a, kRetiredCountersBytes);
  writeU32(plain + kPlainCrcOffset,
           plaintextCrc32(plain, kPlainCrcOffset));
  uint8_t wrappingKey[kKitsuSecretBytes]{};
  if (!platform.deriveWrappingKey(kHardwareId, wrappingKey)) return false;
  const bool sealed = platform.seal(
      wrappingKey, readU32(blob + 8U), blob + 16U, plain, kPlainBytes,
      blob + kOuterHeaderBytes, blob + 28U);
  memset(wrappingKey, 0, sizeof(wrappingKey));
  memset(plain, 0, sizeof(plain));
  return sealed;
}

void testProfileGates() {
  MemoryStorage storage;
  TestPlatform blocked;
  blocked.mode = static_cast<SecurityMode>(0xffU);
  KitsuDeviceSecurity security;
  assert(security.begin(storage, blocked, kHardwareId) ==
         SecurityResult::SecurityModeUnavailable);
  assert(!security.ready());

  TestPlatform missingRoot;
  missingRoot.failRoot = true;
  assert(security.begin(storage, missingRoot, kHardwareId) ==
         SecurityResult::WrappingRootUnavailable);
}

void testReflashableCreationRecoveryAndDerivation() {
  MemoryStorage storage;
  TestPlatform platform;
  KitsuDeviceSecurity security;
  assert(security.begin(storage, platform, kHardwareId) ==
         SecurityResult::OkReflashable);
  assert(security.ready());
  assert(security.status().securityMode == SecurityMode::Reflashable);
  assert(security.status().applicationEncrypted);
  assert(!security.status().hardwareRootProtected);
  assert(security.status().generation == 1U);
  assert(security.status().activeSlot == 0);

  uint8_t id[16]{};
  uint8_t journalKey[32]{};
  assert(security.copyDeviceId(id));
  assert(security.deriveJournalKey(journalKey) == SecurityResult::Ok);
  assert(!storage.contains(journalKey, sizeof(journalKey)));
  uint8_t freshPlain[kPlainBytes]{};
  assert(openActivePlaintext(storage, security, platform, freshPlain));
  assert(retiredRangesAreZero(freshPlain));
  assert((readU32(freshPlain + 8U) & 0x01U) == 0U);

  KitsuDeviceSecurity restored;
  assert(restored.begin(storage, platform, kHardwareId) ==
         SecurityResult::OkReflashable);
  uint8_t restoredId[16]{};
  assert(restored.copyDeviceId(restoredId));
  assert(memcmp(id, restoredId, sizeof(id)) == 0);
}

void testControllerPhysicalGateAndRevocation() {
  MemoryStorage storage;
  TestPlatform platform;
  KitsuDeviceSecurity security;
  assert(security.begin(storage, platform, kHardwareId) ==
         SecurityResult::OkReflashable);
  uint8_t controllerId[kKitsuControllerIdBytes]{};
  for (size_t i = 0U; i < sizeof(controllerId); ++i) {
    controllerId[i] = static_cast<uint8_t>(0x40U + i);
  }
  uint8_t capability[kKitsuSecretBytes]{};
  assert(security.generatePendingControllerRoot(
             true, true, true, false, capability) ==
         SecurityResult::AuthorizationRequired);
  uint8_t zero[kKitsuSecretBytes]{};
  assert(memcmp(capability, zero, sizeof(zero)) == 0);
  assert(security.generatePendingControllerRoot(
             true, true, true, true, capability) == SecurityResult::Ok);
  assert(security.status().controllerCount == 0U);
  assert(security.commitControllerAfterPairing(
             controllerId, capability, true, true, true, true, false) ==
         SecurityResult::AuthorizationRequired);
  assert(security.commitControllerAfterPairing(
             controllerId, capability, true, true, true, true, true) ==
         SecurityResult::Ok);
  assert(security.status().controllerCount == 1U);

  uint8_t copied[kKitsuSecretBytes]{};
  uint8_t copiedId[kKitsuControllerIdBytes]{};
  assert(security.controllerAt(0U, copiedId));
  assert(security.findControllerRoot(controllerId, copied));
  assert(memcmp(copied, capability, sizeof(copied)) == 0);
  assert(memcmp(copiedId, controllerId, sizeof(copiedId)) == 0);
  assert(!storage.contains(capability, sizeof(capability)));

  KitsuDeviceSecurity restored;
  assert(restored.begin(storage, platform, kHardwareId) ==
         SecurityResult::OkReflashable);
  memset(copied, 0, sizeof(copied));
  assert(restored.controllerAt(0U, copiedId));
  assert(restored.findControllerRoot(controllerId, copied));
  assert(memcmp(copied, capability, sizeof(copied)) == 0);
  assert(restored.revokeControllerAfterPhysicalConfirmation(controllerId,
                                                             false) ==
         SecurityResult::AuthorizationRequired);
  assert(restored.revokeControllerAfterPhysicalConfirmation(controllerId,
                                                             true) ==
         SecurityResult::Ok);
  assert(!restored.findControllerRoot(controllerId, copied));
  assertAllStoredGenerationsSanitized(storage, platform, true);

  uint8_t secondRoot[kKitsuSecretBytes]{};
  assert(restored.generatePendingControllerRoot(
             true, true, true, true, secondRoot) == SecurityResult::Ok);
  assert(restored.commitControllerAfterPairing(
             controllerId, secondRoot, true, true, true, true, true) ==
         SecurityResult::Ok);
  storage.failClear = true;
  assert(restored.revokeAuthenticatedController(controllerId) ==
         SecurityResult::StorageWriteFailed);
  assert(!restored.findControllerRoot(controllerId, copied));
  const int8_t clearSlot = static_cast<int8_t>(restored.status().activeSlot ^ 1);
  assert(storage.sizes[static_cast<uint8_t>(clearSlot)] != 0U);
  storage.failClear = false;
  assert(restored.revokeAuthenticatedController(controllerId) ==
         SecurityResult::Ok);
  assertAllStoredGenerationsSanitized(storage, platform, true);
}

void testRetiredNetworkMaterialIsTransactionallyRemoved() {
  MemoryStorage storage;
  TestPlatform platform;
  KitsuDeviceSecurity security;
  assert(security.begin(storage, platform, kHardwareId) ==
         SecurityResult::OkReflashable);
  uint8_t controllerId[kKitsuControllerIdBytes]{};
  uint8_t controllerRoot[kKitsuSecretBytes]{};
  memset(controllerId, 0x42, sizeof(controllerId));
  memset(controllerRoot, 0xc3, sizeof(controllerRoot));
  assert(security.commitControllerAfterPairing(
             controllerId, controllerRoot, true, true, true, true, true) ==
         SecurityResult::Ok);
  const uint32_t legacyGeneration = security.status().generation;
  assert(seedRetiredNetworkMaterial(storage, security, platform));

  KitsuDeviceSecurity restored;
  assert(restored.begin(storage, platform, kHardwareId) ==
         SecurityResult::OkReflashable);
  assert(restored.status().generation == legacyGeneration + 2U);
  uint8_t recovered[kKitsuSecretBytes]{};
  assert(restored.findControllerRoot(controllerId, recovered));
  assert(memcmp(controllerRoot, recovered, sizeof(recovered)) == 0);
  uint8_t migratedPlain[kPlainBytes]{};
  assert(openActivePlaintext(storage, restored, platform, migratedPlain));
  assert(retiredRangesAreZero(migratedPlain));
  assert((readU32(migratedPlain + 8U) & 0x03U) == 0U);
  assertAllStoredGenerationsSanitized(storage, platform, false);

  const size_t writesBeforeCleanBoot = storage.writeCalls;
  const size_t clearsBeforeCleanBoot = storage.clearCalls;
  KitsuDeviceSecurity rebooted;
  assert(rebooted.begin(storage, platform, kHardwareId) ==
         SecurityResult::OkReflashable);
  assert(storage.writeCalls == writesBeforeCleanBoot);
  assert(storage.clearCalls == clearsBeforeCleanBoot);
  memset(recovered, 0, sizeof(recovered));
  assert(rebooted.findControllerRoot(controllerId, recovered));
  assert(memcmp(controllerRoot, recovered, sizeof(recovered)) == 0);
}

void testFourControllerLimitHasNoEviction() {
  MemoryStorage storage;
  TestPlatform platform;
  KitsuDeviceSecurity security;
  assert(security.begin(storage, platform, kHardwareId) ==
         SecurityResult::OkReflashable);
  uint8_t firstId[kKitsuControllerIdBytes]{};
  uint8_t firstRoot[kKitsuSecretBytes]{};
  for (uint8_t controller = 1U; controller <= 4U; ++controller) {
    uint8_t id[kKitsuControllerIdBytes]{};
    uint8_t root[kKitsuSecretBytes]{};
    memset(id, controller, sizeof(id));
    memset(root, static_cast<int>(0x80U + controller), sizeof(root));
    if (controller == 1U) {
      memcpy(firstId, id, sizeof(firstId));
      memcpy(firstRoot, root, sizeof(firstRoot));
    }
    assert(security.commitControllerAfterPairing(
               id, root, true, true, true, true, true) == SecurityResult::Ok);
  }
  assert(security.status().controllerCount == 4U);
  uint8_t fifthId[kKitsuControllerIdBytes]{};
  uint8_t fifthRoot[kKitsuSecretBytes]{};
  memset(fifthId, 5, sizeof(fifthId));
  memset(fifthRoot, 0x85, sizeof(fifthRoot));
  assert(security.commitControllerAfterPairing(
             fifthId, fifthRoot, true, true, true, true, true) ==
         SecurityResult::ControllerTableFull);
  uint8_t recovered[kKitsuSecretBytes]{};
  assert(security.findControllerRoot(firstId, recovered));
  assert(memcmp(recovered, firstRoot, sizeof(recovered)) == 0);
}

void testRetiredMaterialMigrationResumesAfterEraseFailure() {
  MemoryStorage storage;
  TestPlatform platform;
  KitsuDeviceSecurity security;
  assert(security.begin(storage, platform, kHardwareId) ==
         SecurityResult::OkReflashable);
  uint8_t controllerId[kKitsuControllerIdBytes]{};
  uint8_t controllerRoot[kKitsuSecretBytes]{};
  memset(controllerId, 0x33, sizeof(controllerId));
  memset(controllerRoot, 0x77, sizeof(controllerRoot));
  assert(security.commitControllerAfterPairing(
             controllerId, controllerRoot, true, true, true, true, true) ==
         SecurityResult::Ok);
  assert(seedRetiredNetworkMaterial(storage, security, platform));
  storage.failClear = true;

  KitsuDeviceSecurity interrupted;
  assert(interrupted.begin(storage, platform, kHardwareId) ==
         SecurityResult::StorageWriteFailed);
  assert(!interrupted.ready());
  storage.failClear = false;

  KitsuDeviceSecurity recovered;
  assert(recovered.begin(storage, platform, kHardwareId) ==
         SecurityResult::OkReflashable);
  uint8_t copied[kKitsuSecretBytes]{};
  assert(recovered.findControllerRoot(controllerId, copied));
  assert(memcmp(controllerRoot, copied, sizeof(copied)) == 0);
  uint8_t plain[kPlainBytes]{};
  assert(openActivePlaintext(storage, recovered, platform, plain));
  assert(retiredRangesAreZero(plain));
  assert((readU32(plain + 8U) & 0x03U) == 0U);
  assertAllStoredGenerationsSanitized(storage, platform, false);
}

void testRetiredMaterialMigrationResumesAfterWriteFailure() {
  MemoryStorage storage;
  TestPlatform platform;
  KitsuDeviceSecurity security;
  assert(security.begin(storage, platform, kHardwareId) ==
         SecurityResult::OkReflashable);
  uint8_t controllerId[kKitsuControllerIdBytes]{};
  uint8_t controllerRoot[kKitsuSecretBytes]{};
  memset(controllerId, 0x22, sizeof(controllerId));
  memset(controllerRoot, 0x66, sizeof(controllerRoot));
  assert(security.commitControllerAfterPairing(
             controllerId, controllerRoot, true, true, true, true, true) ==
         SecurityResult::Ok);
  assert(seedRetiredNetworkMaterial(storage, security, platform));
  storage.failWrite = true;

  KitsuDeviceSecurity interrupted;
  assert(interrupted.begin(storage, platform, kHardwareId) ==
         SecurityResult::StorageWriteFailed);
  assert(!interrupted.ready());
  storage.failWrite = false;

  KitsuDeviceSecurity recovered;
  assert(recovered.begin(storage, platform, kHardwareId) ==
         SecurityResult::OkReflashable);
  uint8_t copied[kKitsuSecretBytes]{};
  assert(recovered.findControllerRoot(controllerId, copied));
  assert(memcmp(controllerRoot, copied, sizeof(copied)) == 0);
  uint8_t plain[kPlainBytes]{};
  assert(openActivePlaintext(storage, recovered, platform, plain));
  assert(retiredRangesAreZero(plain));
  assert((readU32(plain + 8U) & 0x03U) == 0U);
  assertAllStoredGenerationsSanitized(storage, platform, false);
}

void testCorruptionRefusal() {
  TestPlatform platform;
  MemoryStorage onlyCorrupt;
  onlyCorrupt.sizes[0] = 20U;
  memset(onlyCorrupt.data[0], 0xa5, onlyCorrupt.sizes[0]);
  KitsuDeviceSecurity refuses;
  assert(refuses.begin(onlyCorrupt, platform, kHardwareId) ==
         SecurityResult::CorruptStorage);
}

}  // namespace

int main() {
  testProfileGates();
  testReflashableCreationRecoveryAndDerivation();
  testControllerPhysicalGateAndRevocation();
  testRetiredNetworkMaterialIsTransactionallyRemoved();
  testFourControllerLimitHasNoEviction();
  testRetiredMaterialMigrationResumesAfterEraseFailure();
  testRetiredMaterialMigrationResumesAfterWriteFailure();
  testCorruptionRefusal();
  return 0;
}
