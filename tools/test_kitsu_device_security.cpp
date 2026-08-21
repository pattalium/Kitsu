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
};

const uint8_t kHardwareId[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};

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
  assert(security.remoteConnectivityAllowed());
  assert(security.status().securityMode == SecurityMode::Reflashable);
  assert(security.status().applicationEncrypted);
  assert(!security.status().hardwareRootProtected);
  assert(security.status().generation == 1U);
  assert(security.status().activeSlot == 0);

  uint8_t id[16]{};
  uint8_t lanKey[32]{};
  uint8_t journalKey[32]{};
  assert(security.copyDeviceId(id));
  assert(security.copyLanAuthKey(lanKey));
  assert(security.deriveJournalKey(journalKey) == SecurityResult::Ok);
  assert(memcmp(lanKey, journalKey, sizeof(lanKey)) != 0);
  assert(!storage.contains(lanKey, sizeof(lanKey)));
  assert(!storage.contains(journalKey, sizeof(journalKey)));

  KitsuDeviceSecurity restored;
  assert(restored.begin(storage, platform, kHardwareId) ==
         SecurityResult::OkReflashable);
  uint8_t restoredId[16]{};
  uint8_t restoredLan[32]{};
  assert(restored.copyDeviceId(restoredId));
  assert(restored.copyLanAuthKey(restoredLan));
  assert(memcmp(id, restoredId, sizeof(id)) == 0);
  assert(memcmp(lanKey, restoredLan, sizeof(lanKey)) == 0);
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
}

void testSequencePersistenceAndStrictIncrement() {
  MemoryStorage storage;
  TestPlatform platform;
  KitsuDeviceSecurity security;
  assert(security.begin(storage, platform, kHardwareId) ==
         SecurityResult::OkReflashable);
  assert(security.acceptLanRxSequence(2U) ==
         SecurityResult::SequenceRejected);
  assert(security.acceptLanRxSequence(1U) ==
         SecurityResult::Ok);
  assert(security.acceptLanRxSequence(1U) ==
         SecurityResult::SequenceRejected);
  uint64_t first = 0U;
  uint64_t last = 0U;
  assert(security.reserveLanTxSequenceBlock(32U, first, last) ==
         SecurityResult::Ok);
  assert(first == 1U && last == 32U);

  KitsuDeviceSecurity restored;
  assert(restored.begin(storage, platform, kHardwareId) ==
         SecurityResult::OkReflashable);
  assert(restored.status().lanRxHighWater == 1U);
  assert(restored.status().lanTxReservedHigh == 32U);
  assert(restored.acceptLanRxSequence(2U) ==
         SecurityResult::Ok);
  assert(restored.reserveLanTxSequenceBlock(1U, first, last) ==
         SecurityResult::Ok);
  assert(first == 33U && last == 33U);
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

void testPowerLossRollbackAndCorruptionRefusal() {
  MemoryStorage storage;
  TestPlatform platform;
  KitsuDeviceSecurity security;
  assert(security.begin(storage, platform, kHardwareId) ==
         SecurityResult::OkReflashable);
  storage.tearWrite = true;
  assert(security.acceptLanRxSequence(1U) ==
         SecurityResult::StorageWriteFailed);
  assert(security.status().lanRxHighWater == 0U);

  KitsuDeviceSecurity restored;
  assert(restored.begin(storage, platform, kHardwareId) ==
         SecurityResult::OkReflashable);
  assert(restored.status().lanRxHighWater == 0U);

  uint8_t lanBefore[32]{};
  assert(restored.copyLanAuthKey(lanBefore));
  uint8_t output[32]{};
  storage.corruptWrite = true;
  assert(restored.rotateLanKeyAfterPhysicalConfirmation(
             true, true, output) == SecurityResult::ReadbackFailed);
  uint8_t lanAfter[32]{};
  assert(restored.copyLanAuthKey(lanAfter));
  assert(memcmp(lanBefore, lanAfter, sizeof(lanBefore)) == 0);

  MemoryStorage onlyCorrupt;
  onlyCorrupt.sizes[0] = 20U;
  memset(onlyCorrupt.data[0], 0xa5, onlyCorrupt.sizes[0]);
  KitsuDeviceSecurity refuses;
  assert(refuses.begin(onlyCorrupt, platform, kHardwareId) ==
         SecurityResult::CorruptStorage);
}

void testReflashableMaterialRestoresWithRemoteConnectivity() {
  MemoryStorage storage;
  TestPlatform platform;
  KitsuDeviceSecurity security;
  assert(security.begin(storage, platform, kHardwareId) ==
         SecurityResult::OkReflashable);
  assert(security.remoteConnectivityAllowed());

  KitsuDeviceSecurity restored;
  assert(restored.begin(storage, platform, kHardwareId) ==
         SecurityResult::OkReflashable);
  assert(restored.remoteConnectivityAllowed());
}

}  // namespace

int main() {
  testProfileGates();
  testReflashableCreationRecoveryAndDerivation();
  testControllerPhysicalGateAndRevocation();
  testSequencePersistenceAndStrictIncrement();
  testFourControllerLimitHasNoEviction();
  testPowerLossRollbackAndCorruptionRefusal();
  testReflashableMaterialRestoresWithRemoteConnectivity();
  return 0;
}
